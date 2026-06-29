/*
 * pingpong - symmetric LoRa ping/pong link test. See pingpong.h.
 *
 * One dedicated RX thread owns the radio while the mode runs. It polls
 * lora_recv() in short slices (so a user SEND PING fires within ~one slice) and
 * tracks the 1-second RTT timeout itself by timestamp. Because only this thread
 * ever touches the radio, no cross-thread radio coordination is needed; the
 * existing SEND-ONCE TX thread is left untouched and is never active in this
 * mode (different screen).
 *
 * The applied RF config (set via the CONFIG screen) is used as-is; the native
 * SX126x driver configures both TX and RX from one lora_config(), so alternating
 * lora_recv()/lora_send() needs no reconfiguration here.
 */

#include <zephyr/kernel.h>

#if defined(CONFIG_APP_CONSOLE)

#include "pingpong.h"

#include <zephyr/device.h>
#include <zephyr/drivers/lora.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/util.h>
#include <errno.h>
#include <string.h>

/* Packet: 44 bytes to match other test traffic on this project. */
#define PP_LEN          44
#define PP_MAGIC        0xA5
#define PP_TYPE_PING    0x01
#define PP_TYPE_PONG    0x02
#define PP_OFF_SEQ      2   /* uint32 LE */
#define PP_OFF_TS       6   /* uint32 LE */

#define PP_TIMEOUT_US   1000000U /* outstanding ping lifetime */
#define PP_RECV_SLICE_MS 100     /* lora_recv() poll slice (SEND PING latency) */

static const struct device *const lora_dev = DEVICE_DT_GET(DT_ALIAS(lora0));

static K_MUTEX_DEFINE(pp_mutex);
static pingpong_stats_t g_stats;

static atomic_t pp_running = ATOMIC_INIT(0);
static atomic_t pp_pending_ping = ATOMIC_INIT(0);

static uint32_t pp_next_seq; /* RX-thread only */

uint32_t pingpong_now_us(void)
{
	return k_cyc_to_us_floor32(k_cycle_get_32());
}

static void build_pkt(uint8_t *b, uint8_t type, uint32_t seq, uint32_t ts)
{
	b[0] = PP_MAGIC;
	b[1] = type;
	sys_put_le32(seq, &b[PP_OFF_SEQ]);
	sys_put_le32(ts, &b[PP_OFF_TS]);
	memset(&b[10], 0, PP_LEN - 10);
}

static void reset_stats_locked_clear(void)
{
	k_mutex_lock(&pp_mutex, K_FOREVER);
	memset(&g_stats, 0, sizeof(g_stats));
	g_stats.min_rtt_us = UINT32_MAX; /* so the first sample sets the min */
	k_mutex_unlock(&pp_mutex);
}

/* ---- Public control --------------------------------------------------- */

int pingpong_start(void)
{
	reset_stats_locked_clear();
	pp_next_seq = 0;
	atomic_set(&pp_pending_ping, 0);
	atomic_set(&pp_running, 1);
	printk("PINGPONG,START\n");
	return 0;
}

int pingpong_stop(void)
{
	atomic_set(&pp_running, 0);
	printk("PINGPONG,STOP\n");
	return 0;
}

void pingpong_send_ping(void)
{
	atomic_set(&pp_pending_ping, 1);
}

void pingpong_reset_stats(void)
{
	reset_stats_locked_clear();
	printk("PINGPONG,STATS_RESET\n");
}

void pingpong_get_stats(pingpong_stats_t *out)
{
	k_mutex_lock(&pp_mutex, K_FOREVER);
	*out = g_stats;
	k_mutex_unlock(&pp_mutex);
}

bool pingpong_is_running(void)
{
	return atomic_get(&pp_running) != 0;
}

/* ---- RX-thread radio operations --------------------------------------- */

/* Send a user-initiated ping, abandoning any still-outstanding one. */
static void do_send_ping(void)
{
	uint8_t buf[PP_LEN];
	bool abandoned = false;
	uint32_t abandoned_seq = 0;

	k_mutex_lock(&pp_mutex, K_FOREVER);
	if (g_stats.outstanding_valid) {
		abandoned = true;
		abandoned_seq = g_stats.outstanding_seq;
		g_stats.outstanding_valid = false;
	}
	k_mutex_unlock(&pp_mutex);

	if (abandoned) {
		printk("PINGPONG,ABANDONED,seq=%u\n", abandoned_seq);
	}

	uint32_t seq = ++pp_next_seq;
	uint32_t tx_us = pingpong_now_us();

	build_pkt(buf, PP_TYPE_PING, seq, tx_us);

	int rc = lora_send(lora_dev, buf, PP_LEN);

	if (rc < 0) {
		printk("PINGPONG,TX_PING_ERR,seq=%u,rc=%d\n", seq, rc);
		return; /* not counted / not outstanding: nothing went on air */
	}

	k_mutex_lock(&pp_mutex, K_FOREVER);
	g_stats.outstanding_valid = true;
	g_stats.outstanding_seq = seq;
	g_stats.outstanding_tx_us = tx_us;
	g_stats.pings_sent++;
	k_mutex_unlock(&pp_mutex);

	printk("PINGPONG,TX_PING,seq=%u,tx_us=%u\n", seq, tx_us);
}

static void handle_ping(uint32_t seq, uint32_t ts, uint32_t rx_us,
			int16_t rssi, int8_t snr)
{
	uint8_t pong[PP_LEN];

	printk("PINGPONG,RX_PING,seq=%u,rx_us=%u,rssi=%d,snr=%d\n",
	       seq, rx_us, rssi, snr);

	k_mutex_lock(&pp_mutex, K_FOREVER);
	g_stats.pings_received++;
	k_mutex_unlock(&pp_mutex);

	/* Echo a pong with the same seq + timestamp (the originator matches on
	 * seq; the echoed timestamp is diagnostic only).
	 */
	build_pkt(pong, PP_TYPE_PONG, seq, ts);

	int rc = lora_send(lora_dev, pong, PP_LEN);

	if (rc < 0) {
		printk("PINGPONG,TX_PONG_ERR,seq=%u,rc=%d\n", seq, rc);
		return; /* do not retry; the originator will time out cleanly */
	}

	k_mutex_lock(&pp_mutex, K_FOREVER);
	g_stats.pongs_sent++;
	k_mutex_unlock(&pp_mutex);

	printk("PINGPONG,TX_PONG,seq=%u,tx_us=%u\n", seq, ts);
}

static void handle_pong(uint32_t seq, uint32_t rx_us, int16_t rssi, int8_t snr)
{
	bool matched = false;
	uint32_t rtt = 0;

	k_mutex_lock(&pp_mutex, K_FOREVER);
	if (g_stats.outstanding_valid && g_stats.outstanding_seq == seq) {
		rtt = rx_us - g_stats.outstanding_tx_us; /* wrap-safe */
		g_stats.outstanding_valid = false;
		g_stats.pongs_received++;
		g_stats.last_rtt_us = rtt;
		if (rtt < g_stats.min_rtt_us) {
			g_stats.min_rtt_us = rtt;
		}
		if (rtt > g_stats.max_rtt_us) {
			g_stats.max_rtt_us = rtt;
		}
		g_stats.rtt_sum_us += rtt;
		g_stats.rtt_count++;
		matched = true;
	}
	k_mutex_unlock(&pp_mutex);

	if (matched) {
		printk("PINGPONG,RX_PONG,seq=%u,rx_us=%u,rtt_us=%u,rssi=%d,snr=%d\n",
		       seq, rx_us, rtt, rssi, snr);
	} else {
		/* No matching outstanding ping (e.g. a late pong for an abandoned
		 * or already-timed-out ping).
		 */
		printk("PINGPONG,UNEXPECTED,type=pong,seq=%u\n", seq);
	}
}

static void handle_rx(const uint8_t *buf, int n, int16_t rssi, int8_t snr)
{
	uint32_t rx_us = pingpong_now_us();

	if (n != PP_LEN || buf[0] != PP_MAGIC) {
		printk("PINGPONG,UNEXPECTED,type=malformed\n");
		return;
	}

	uint8_t type = buf[1];
	uint32_t seq = sys_get_le32(&buf[PP_OFF_SEQ]);
	uint32_t ts = sys_get_le32(&buf[PP_OFF_TS]);

	switch (type) {
	case PP_TYPE_PING:
		handle_ping(seq, ts, rx_us, rssi, snr);
		break;
	case PP_TYPE_PONG:
		handle_pong(seq, rx_us, rssi, snr);
		break;
	default:
		printk("PINGPONG,UNEXPECTED,type=0x%02x\n", type);
		break;
	}
}

/* Expire an outstanding ping that has waited longer than the timeout. */
static void check_timeout(void)
{
	bool timed_out = false;
	uint32_t seq = 0;

	k_mutex_lock(&pp_mutex, K_FOREVER);
	if (g_stats.outstanding_valid &&
	    (pingpong_now_us() - g_stats.outstanding_tx_us) >= PP_TIMEOUT_US) {
		timed_out = true;
		seq = g_stats.outstanding_seq;
		g_stats.outstanding_valid = false;
		g_stats.timeouts++;
	}
	k_mutex_unlock(&pp_mutex);

	if (timed_out) {
		printk("PINGPONG,TIMEOUT,seq=%u\n", seq);
	}
}

static uint8_t rx_buf[64]; /* > PP_LEN so wrong-length packets are detectable */

static void pingpong_thread_fn(void *a, void *b, void *c)
{
	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);

	bool was_running = false;

	while (1) {
		if (!atomic_get(&pp_running)) {
			/* The driver already sleeps the chip after each op, so
			 * there is nothing to tear down; just go idle.
			 */
			was_running = false;
			k_sleep(K_MSEC(50));
			continue;
		}
		was_running = true;

		if (atomic_cas(&pp_pending_ping, 1, 0)) {
			do_send_ping();
		}

		int16_t rssi = 0;
		int8_t snr = 0;
		int n = lora_recv(lora_dev, rx_buf, sizeof(rx_buf),
				  K_MSEC(PP_RECV_SLICE_MS), &rssi, &snr);

		if (n >= 0) {
			handle_rx(rx_buf, n, rssi, snr);
		} else if (n != -EAGAIN) {
			/* -EAGAIN is the normal slice timeout; anything else is a
			 * real error - log and back off briefly, don't exit.
			 */
			printk("PINGPONG,RECV_ERR,rc=%d\n", n);
			k_sleep(K_MSEC(20));
		}

		check_timeout();
	}
}

K_THREAD_DEFINE(pp_tid, 4096, pingpong_thread_fn, NULL, NULL, NULL, 7, 0, 0);

#endif /* CONFIG_APP_CONSOLE */
