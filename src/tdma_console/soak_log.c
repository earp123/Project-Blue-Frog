/*
 * soak_log - binary soak-run logger. See soak_log.h.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>

#if defined(CONFIG_APP_TDMA_CONSOLE)

#include "soak_log.h"
#include "sd_log.h"

#include <zephyr/sys/util.h>
#include <errno.h>
#include <string.h>

/*
 * Writer thread: preemptible and below the main/UI thread, so a card stall
 * never delays a redraw, and far below the cooperative radio thread, which
 * outranks everything here by construction. Stack covers FATFS + the SPI
 * driver's call depth.
 */
#define WRITER_PRIO	K_PRIO_PREEMPT(10)
#define WRITER_STACK	3072

/* Ring between the producers (main loop) and the writer thread. */
K_MSGQ_DEFINE(rec_msgq, SOAK_REC_SIZE, SOAK_LOG_RING_DEPTH, 4);

static K_SEM_DEFINE(writer_idle, 0, 1);

static atomic_t log_active;	/* producers gate on this */
static bool file_open;		/* writer-thread side */
static char log_path[32];

static uint32_t seq_next;
static atomic_t stat_queued;
static atomic_t stat_written;
static atomic_t stat_dropped;
static atomic_t stat_err;

/* Hand a record to the writer. Never blocks: a full ring means the card is
 * not keeping up, and dropping is strictly better than stalling the UI.
 */
static void submit(struct soak_rec *r)
{
	if (!atomic_get(&log_active)) {
		return;
	}

	r->seq = seq_next++;
	r->uptime_ms = (uint32_t)k_uptime_get();

	if (k_msgq_put(&rec_msgq, r, K_NO_WAIT) != 0) {
		atomic_inc(&stat_dropped);
		return;
	}
	atomic_inc(&stat_queued);
}

int soak_log_start(enum tdma_role role, uint8_t slot_id, int8_t tx_power_dbm)
{
	struct soak_rec r = { 0 };
	struct soak_meta m = { 0 };
	int rc;

	if (atomic_get(&log_active)) {
		return -EBUSY;
	}

	k_msgq_purge(&rec_msgq);
	/* Clear any drain-complete signal left by the previous run, or this
	 * run's stop would return without waiting for the writer.
	 */
	k_sem_reset(&writer_idle);
	seq_next = 0;
	atomic_set(&stat_queued, 0);
	atomic_set(&stat_written, 0);
	atomic_set(&stat_dropped, 0);
	atomic_set(&stat_err, 0);
	log_path[0] = '\0';

	rc = sd_log_mount();
	if (rc < 0) {
		atomic_set(&stat_err, rc);
		return rc;
	}

	rc = sd_log_open("SOAK", log_path, sizeof(log_path));
	if (rc < 0) {
		atomic_set(&stat_err, rc);
		return rc;
	}

	file_open = true;

	/* Record 0 describes the run, so a decoded file is self-contained. */
	m.magic = SOAK_LOG_MAGIC;
	m.version = SOAK_LOG_VERSION;
	m.role = (uint8_t)role;
	m.slot_id = slot_id;
	m.freq_hz = TDMA_RF_FREQ_HZ;
	m.slot_us = TDMA_SLOT_DURATION_US;
	m.frame_us = TDMA_FRAME_DURATION_US;
	m.toa_us = TDMA_TOA_US;
	m.sf = 5;
	m.cr = 1;		/* CR 4/5 */
	m.bw_khz = 500;
	m.tx_power_dbm = tx_power_dbm;
	m.slot_count = TDMA_SLOT_COUNT;
	m.payload_len = TDMA_PAYLOAD_LEN;
	m.preamble_syms = TDMA_PREAMBLE_SYMS;
	m.uptime_ms = (uint32_t)k_uptime_get();

	r.type = SOAK_REC_META;
	r.slot_id = slot_id;
	memcpy(r.payload, &m, sizeof(m));

	atomic_set(&log_active, 1);
	submit(&r);

	return 0;
}

void soak_log_rx(const struct tdma_rx_msg *msg, uint8_t sync_state)
{
	struct soak_rec r = { 0 };

	r.type = SOAK_REC_RX;
	r.slot_id = msg->slot_id;
	r.sync_state = sync_state;
	r.t_us = msg->timestamp_us;
	r.frame_ctr = msg->frame_ctr;
	r.rssi = msg->rssi;
	r.snr = msg->snr;
	memcpy(r.payload, msg->payload, TDMA_PAYLOAD_LEN);

	submit(&r);
}

void soak_log_tx(const uint8_t payload[TDMA_PAYLOAD_LEN], uint8_t slot_id,
		 uint8_t sync_state)
{
	struct soak_rec r = { 0 };

	r.type = SOAK_REC_TX;
	r.slot_id = slot_id;
	r.sync_state = sync_state;
	/*
	 * Submit time, not air time: the engine transmits this payload on the
	 * radio thread at the next TX slot boundary. The peer's RX record
	 * carries the authoritative on-air timing, and the payload content
	 * pairs the two. The engine does not expose its frame counter to the
	 * application, hence SOAK_F_NO_CTR.
	 */
	r.t_us = 0;
	r.flags = SOAK_F_NO_CTR;
	memcpy(r.payload, payload, TDMA_PAYLOAD_LEN);

	submit(&r);
}

void soak_log_stats(const struct tdma_telemetry *t, uint32_t rx_ok,
		    uint32_t missed, uint32_t dup)
{
	struct soak_rec r = { 0 };
	uint32_t c[10];

	r.type = SOAK_REC_STATS;
	r.sync_state = t->sync_state;
	r.t_us = t->last_evt_dt_us;
	r.rssi = (int16_t)t->last_phase_err_us;	/* reuse: phase error, us */
	r.snr = (int8_t)CLAMP(t->last_ppm, -128, 127);

	/* Exactly ten counters fit the 40-byte payload. */
	c[0] = t->tx_done;
	c[1] = t->rx_done;
	c[2] = t->rx_crc_err;
	c[3] = t->rx_bad_header;
	c[4] = t->slot_timeouts;
	c[5] = t->stale_retx;
	c[6] = t->busy_timeouts;
	c[7] = rx_ok;
	c[8] = missed;
	c[9] = dup;
	memcpy(r.payload, c, sizeof(c));

	submit(&r);
}

int soak_log_stop(void)
{
	int rc;

	if (!atomic_get(&log_active)) {
		return 0;
	}

	/* Stop accepting new records, then let the writer drain what is left. */
	atomic_set(&log_active, 0);

	/* The writer signals once it finds the ring empty with logging off. */
	(void)k_sem_take(&writer_idle, K_SECONDS(5));

	rc = sd_log_close();
	file_open = false;
	if (rc < 0) {
		atomic_set(&stat_err, rc);
	}
	return rc;
}

void soak_log_get_status(struct soak_log_status *out)
{
	const struct sd_log_stats *s = sd_log_get_stats();

	out->active = atomic_get(&log_active) != 0;
	out->queued = (uint32_t)atomic_get(&stat_queued);
	out->written = (uint32_t)atomic_get(&stat_written);
	out->dropped = (uint32_t)atomic_get(&stat_dropped);
	out->bytes = s->bytes;
	out->err = (int)atomic_get(&stat_err);
	out->path = log_path;
}

static void writer_thread_fn(void *p1, void *p2, void *p3)
{
	struct soak_rec r;

	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	for (;;) {
		/* Wake on a record, or periodically so the drain-complete
		 * handshake in soak_log_stop() cannot be missed.
		 */
		if (k_msgq_get(&rec_msgq, &r, K_MSEC(100)) == 0) {
			if (file_open) {
				int rc = sd_log_write(&r, sizeof(r));

				if (rc < 0) {
					atomic_set(&stat_err, rc);
				} else {
					atomic_inc(&stat_written);
				}
			}
			continue;
		}

		/* Ring empty. If logging has been stopped, the file is fully
		 * drained: hand the close back to soak_log_stop().
		 */
		if (!atomic_get(&log_active) && file_open) {
			k_sem_give(&writer_idle);
		}
	}
}

K_THREAD_DEFINE(soak_log_writer, WRITER_STACK, writer_thread_fn,
		NULL, NULL, NULL, WRITER_PRIO, 0, 0);

#endif /* CONFIG_APP_TDMA_CONSOLE */
