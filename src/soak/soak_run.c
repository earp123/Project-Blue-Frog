/*
 * soak_run - shared fixed-length TDMA soak runner. See soak_run.h.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>

#if defined(CONFIG_APP_SOAK)

#include "soak_run.h"
#include "soak_role.h"
#include "tdma.h"

/* Received-packet accounting, mirrored from the TDMA test main. */
static uint32_t rx_per_slot[TDMA_SLOT_COUNT];
static int16_t last_rssi;
static int8_t last_snr;
static uint16_t last_frame_ctr;

static const char *const sync_str[] = {
	[TDMA_SYNC_STOPPED] = "STOPPED",
	[TDMA_SYNC_SYNCING] = "SYNCING",
	[TDMA_SYNC_RUNNING] = "RUNNING",
};

static void fill_pattern(uint8_t payload[TDMA_PAYLOAD_LEN], uint8_t seq)
{
	for (int i = 0; i < TDMA_PAYLOAD_LEN; i++) {
		payload[i] = (uint8_t)(seq + i);
	}
}

static void drain_rx(void)
{
	struct tdma_rx_msg msg;

	while (k_msgq_get(&tdma_rx_msgq, &msg, K_NO_WAIT) == 0) {
		rx_per_slot[msg.slot_id % TDMA_SLOT_COUNT]++;
		last_rssi = msg.rssi;
		last_snr = msg.snr;
		last_frame_ctr = msg.frame_ctr;
	}
}

static void print_row(const char *id, const char *role, uint32_t t_s)
{
	const struct tdma_telemetry *t = tdma_get_telemetry();

	printk("SOAK,%s,row,%u,%s,%s,%u,%u,%u,%u,%u,%u,%u,"
	       "%u,%u,%u,%u,%d,%d,%d,%d\n",
	       id, t_s, role, sync_str[t->sync_state],
	       t->tx_done, t->rx_done, t->rx_crc_err, t->rx_bad_header,
	       t->slot_timeouts, t->stale_retx, t->busy_timeouts,
	       rx_per_slot[0], rx_per_slot[1], rx_per_slot[2], rx_per_slot[3],
	       last_rssi, last_snr, t->last_phase_err_us, t->last_ppm);
}

int soak_run(const struct soak_params *p)
{
	enum tdma_role role = soak_role_get();
	uint8_t slot = soak_role_slot(role);
	const char *role_s = (role == TDMA_ROLE_MASTER) ? "master" : "secondary";
	struct tdma_config cfg = {
		.role = role,
		.slot_id = slot,
		.slot_duration_us = TDMA_SLOT_DURATION_US,
	};
	int64_t start, last_sample;
	uint8_t seq = 0;
	int ret;

	printk("\nSOAK,%s,meta,role=%s,slot=%u,power=%ddBm,phy=SF5/BW500/CR45,"
	       "freq=%luHz,frame_ms=%u,slot_ms=%u,dur_s=%u,period_s=%u\n",
	       p->test_id, role_s, slot, (int)p->tx_power_dbm,
	       (unsigned long)TDMA_RF_FREQ_HZ,
	       TDMA_FRAME_DURATION_US / 1000U, TDMA_SLOT_DURATION_US / 1000U,
	       p->duration_s, p->sample_period_s);
	printk("SOAK,%s,hdr,t_s,role,sync,tx_done,rx_done,crc_err,bad_hdr,"
	       "timeouts,stale,busy_to,rx0,rx1,rx2,rx3,rssi,snr,phase_us,ppm\n",
	       p->test_id);

	ret = tdma_init(&cfg);
	if (ret < 0) {
		printk("SOAK,%s,error,tdma_init=%d\n", p->test_id, ret);
		return ret;
	}

	/* The one PHY knob under test: SetTxParams is issued on the radio thread
	 * just before the next transmit, so it takes effect on the first packet.
	 */
	ret = tdma_set_tx_power(p->tx_power_dbm);
	if (ret < 0) {
		printk("SOAK,%s,error,tx_power=%d\n", p->test_id, ret);
		return ret;
	}

	ret = tdma_start();
	if (ret < 0) {
		printk("SOAK,%s,error,tdma_start=%d\n", p->test_id, ret);
		return ret;
	}

	start = k_uptime_get();
	last_sample = start;
	print_row(p->test_id, role_s, 0); /* t=0 baseline */

	while (1) {
		uint8_t payload[TDMA_PAYLOAD_LEN];
		int64_t now;

		/* One fresh pattern payload per frame; -EAGAIN just means the
		 * previous one has not gone out yet.
		 */
		fill_pattern(payload, seq);
		if (tdma_tx_submit(payload) == 0) {
			seq++;
		}

		drain_rx();

		now = k_uptime_get();
		if (now - last_sample >= (int64_t)p->sample_period_s * 1000) {
			last_sample = now;
			print_row(p->test_id, role_s,
				  (uint32_t)((now - start) / 1000));
		}
		if (now - start >= (int64_t)p->duration_s * 1000) {
			break;
		}

		k_sleep(K_MSEC(TDMA_FRAME_DURATION_US / 1000U));
	}

	tdma_stop();
	drain_rx();

	/* Summary. The peer sends one packet per frame in its own slot, so the
	 * expected count over the run is the number of frames elapsed, and the
	 * packet error rate is derived from what we heard in the peer's slot.
	 */
	const struct tdma_telemetry *t = tdma_get_telemetry();
	enum tdma_role peer = (role == TDMA_ROLE_MASTER) ?
			      TDMA_ROLE_SECONDARY : TDMA_ROLE_MASTER;
	uint32_t rx_peer = rx_per_slot[soak_role_slot(peer)];
	uint32_t frames = (uint32_t)(((int64_t)p->duration_s * 1000000) /
				     TDMA_FRAME_DURATION_US);
	uint32_t per_milli = 0; /* PER in thousandths of a percent */

	if (frames > 0 && rx_peer <= frames) {
		per_milli = (uint32_t)(((uint64_t)(frames - rx_peer) * 100000) /
				       frames);
	}

	printk("SOAK,%s,done,role=%s,tx_done=%u,rx_done=%u,rx_peer=%u,"
	       "frames_exp=%u,per_pct=%u.%03u,crc_err=%u,bad_hdr=%u,"
	       "timeouts=%u,busy_to=%u\n",
	       p->test_id, role_s, t->tx_done, t->rx_done, rx_peer, frames,
	       per_milli / 1000U, per_milli % 1000U, t->rx_crc_err,
	       t->rx_bad_header, t->slot_timeouts, t->busy_timeouts);
	printk("SOAK,%s,END\n", p->test_id);

	return 0;
}

#endif /* CONFIG_APP_SOAK */
