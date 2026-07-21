/*
 * tdma_core - frame engine: schedule table, STOPPED->SYNCING->RUNNING state
 * machine, roles, telemetry, and the naive secondary phase alignment.
 * Implements the public API in tdma.h. All handlers run on the radio thread
 * (tdma_port); the public entry points run on application threads and touch
 * no SPI themselves.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>
#include <errno.h>
#include <string.h>
#include <stdlib.h>

#include "tdma.h"
#include "tdma_port.h"
#include "tdma_radio.h"
#include "tdma_buf.h"
#include "sx126x_cmd.h"

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(tdma_core, CONFIG_LOG_DEFAULT_LEVEL);

static struct {
	struct tdma_config cfg;
	bool initialized;

	enum tdma_sync_state state;
	uint8_t cur_slot;
	uint16_t frame_ctr;	/* master: own count; secondary: last heard */

	/* TX freshness: set when a staged payload reaches the chip, cleared
	 * on each TX slot entry; entering a TX slot without it is a stale
	 * retransmit of whatever the TX region still holds.
	 */
	bool payload_fresh;

	/* The chip's TX region holds a valid header (written during the
	 * pre-TX slot's slack). Until then TX slots fall back to listening
	 * so we never emit garbage.
	 */
	bool hdr_written;

	/* Secondary sync tracking. */
	bool beacon_seen;
	uint8_t lock_streak;

	/* Diagnostic: previous beacon, for the local-vs-master rate estimate. */
	uint32_t sync_prev_ts;
	uint16_t sync_prev_ctr;
	bool sync_have_prev;

	struct tdma_telemetry telem;

	/* Manual M0 ops: one frame counter for hand-fired packets. */
	uint16_t manual_ctr;
} eng;

const struct tdma_telemetry *tdma_get_telemetry(void)
{
	/* Read-mostly counters, updated in place on the radio thread; a
	 * torn 32-bit read is impossible on this core and staleness is fine
	 * for periodic printing.
	 */
	eng.telem.sync_state = (uint8_t)eng.state;
	return &eng.telem;
}

/* Fatal: stop the engine on schedule-breaking errors (BUSY stuck high). */
static void engine_fatal_stop(void)
{
	tdma_port_schedule_stop();
	eng.state = TDMA_SYNC_STOPPED;
	LOG_ERR("engine stopped (fatal radio error)");
}

static int l1_check(int ret)
{
	if (ret == -ETIMEDOUT) {
		eng.telem.busy_timeouts++;
		engine_fatal_stop();
	}
	return ret;
}

/*
 * Secondary phase alignment (M2 — deliberately naive).
 *
 * Each master beacon's RxDone timestamp, minus the time-on-air and the
 * TX-start latency, is where the master's slot-0 boundary sits on our local
 * clock. Compare with our own slot-0 boundary (mod one frame), then:
 *  - first beacon after (re)start: snap the full error into the schedule
 *    (acquisition — a clamped loop would take tens of seconds to pull in a
 *    worst-case 100 ms offset);
 *  - afterwards: proportional step, clamped to +/-500 us per frame.
 * TODO(M3+): replace the proportional step with a real filter (PI / drift
 * estimator) before tightening slots to 20 ms.
 */
void tdma_core_sync_feed(uint32_t rx_timestamp_us, uint16_t frame_ctr)
{
	const int32_t frame_us = TDMA_FRAME_DURATION_US;
	uint32_t local_slot0;
	uint32_t master_slot0;
	int32_t err;

	eng.frame_ctr = frame_ctr;

	/*
	 * Diagnostic: relative clock rate, free of the +/-frame/2 wrap that
	 * makes last_phase_err_us ambiguous. The master's frame counter rides
	 * in the header, so elapsed master time is exact; compare it with
	 * elapsed local slot-clock time between two beacons.
	 */
	if (eng.sync_have_prev) {
		uint32_t d_local = rx_timestamp_us - eng.sync_prev_ts;
		uint16_t d_frames = frame_ctr - eng.sync_prev_ctr;
		uint64_t d_master = (uint64_t)d_frames * TDMA_FRAME_DURATION_US;

		if (d_frames != 0U) {
			eng.telem.last_ppm = (int32_t)
				((((int64_t)d_local - (int64_t)d_master) *
				  1000000) / (int64_t)d_master);
		}
	}
	eng.sync_prev_ts = rx_timestamp_us;
	eng.sync_prev_ctr = frame_ctr;
	eng.sync_have_prev = true;

	local_slot0 = tdma_port_last_boundary() -
		      (uint32_t)eng.cur_slot * eng.cfg.slot_duration_us;
	master_slot0 = rx_timestamp_us - TDMA_TOA_US - TDMA_TX_START_LATENCY_US;

	/* Wrap-safe difference, reduced to [-frame/2, frame/2). */
	err = (int32_t)(master_slot0 - local_slot0);
	err = ((err % frame_us) + frame_us + frame_us / 2) % frame_us -
	      frame_us / 2;

	eng.telem.last_phase_err_us = err;

	if (!eng.beacon_seen) {
		/* Snap forward-only (phase-equivalent mod one frame) so the
		 * adjusted alarm target can never land in the past.
		 */
		int32_t fwd = ((err % frame_us) + frame_us) % frame_us;

		eng.beacon_seen = true;
		tdma_port_add_phase_adj(fwd);
		LOG_INF("beacon acquired, snapping phase forward %d us", fwd);
		return;
	}

	tdma_port_add_phase_adj(CLAMP(err / 2, -(int32_t)TDMA_SYNC_STEP_CLAMP_US,
				      (int32_t)TDMA_SYNC_STEP_CLAMP_US));

	if (abs(err) < TDMA_SYNC_LOCK_ERR_US) {
		if (eng.lock_streak < TDMA_SYNC_LOCK_STREAK) {
			eng.lock_streak++;
		}
		if (eng.state == TDMA_SYNC_SYNCING &&
		    eng.lock_streak >= TDMA_SYNC_LOCK_STREAK) {
			eng.state = TDMA_SYNC_RUNNING;
			LOG_INF("sync locked, entering RUNNING");
		}
	} else {
		eng.lock_streak = 0;
	}
}

/*
 * RX-slot slack work, done right after SetRx (or after an RxDone drain):
 * push a freshly staged payload into the chip's TX region, and write the
 * header for the upcoming TX slot once its frame counter is known — never
 * in the TX slot entry path itself.
 */
static void rx_slot_slack_work(void)
{
	uint8_t payload[TDMA_PAYLOAD_LEN];

	if (tdma_buf_take_staged(payload)) {
		if (l1_check(tdma_radio_stage_payload(payload)) == 0) {
			eng.payload_fresh = true;
		}
	}

	if (eng.state != TDMA_SYNC_RUNNING) {
		return;
	}

	/* Slot immediately before ours: the next boundary starts our TX. */
	if ((uint8_t)((eng.cur_slot + 1) % TDMA_SLOT_COUNT) == eng.cfg.slot_id) {
		uint16_t ctr = eng.frame_ctr;

		/* Master's counter bumps at the slot-0 wrap; if our TX slot
		 * is 0, the packet goes out in the next frame.
		 */
		if (eng.cfg.role == TDMA_ROLE_MASTER && eng.cfg.slot_id == 0) {
			ctr++;
		}

		if (l1_check(tdma_radio_write_hdr(eng.cfg.slot_id, ctr)) == 0) {
			eng.hdr_written = true;
		}
	}
}

void tdma_core_on_slot_tick(void)
{
	if (eng.state == TDMA_SYNC_STOPPED) {
		return;
	}

	eng.cur_slot = (eng.cur_slot + 1) % TDMA_SLOT_COUNT;
	if (eng.cur_slot == 0 && eng.cfg.role == TDMA_ROLE_MASTER) {
		eng.frame_ctr++;
	}

	/* Secondaries transmit only once RUNNING; while SYNCING every slot
	 * listens so an unaligned schedule cannot collide with anyone. A TX
	 * slot before the first header write also listens instead.
	 */
	bool tx_slot = (eng.cur_slot == eng.cfg.slot_id) &&
		       (eng.state == TDMA_SYNC_RUNNING) &&
		       eng.hdr_written;

	if (tx_slot) {
		if (!eng.payload_fresh) {
			eng.telem.stale_retx++;
		}
		eng.payload_fresh = false;
		(void)l1_check(tdma_radio_slot_tx_enter());
	} else {
		if (l1_check(tdma_radio_slot_rx_enter()) == 0) {
			uint8_t mode = 0;

			/* Diagnostic: did the chip actually enter RX? BUSY is
			 * already low by the time this transaction starts, so
			 * SetRx has been processed, not merely accepted.
			 */
			eng.telem.rx_arm++;
			eng.telem.arm_by_slot[eng.cur_slot]++;
			if (tdma_radio_probe_mode(&mode) == 0) {
				eng.telem.last_chip_mode = mode;
				if (mode != SX126X_MODE_RX) {
					eng.telem.rx_mode_bad++;
				}
			}

			rx_slot_slack_work();
		}
	}
}

void tdma_core_on_dio1(void)
{
	uint32_t ts = tdma_port_dio1_timestamp();
	struct tdma_radio_event ev;
	int ret;

	if (eng.state == TDMA_SYNC_STOPPED) {
		/* Stray edge after stop (or manual op remnant): ignore. The
		 * manual paths consume their own DIO1 events synchronously.
		 */
		return;
	}

	ret = l1_check(tdma_radio_drain_dio1(&ev));
	if (ret < 0) {
		return;
	}

	/*
	 * Diagnostic. An empty drain means the edge that woke us referred to
	 * IRQ bits somebody else already cleared — the slot-entry
	 * clear_irq_status(ALL) is the only other writer, so a nonzero count
	 * here is direct evidence of events being destroyed before they are
	 * read. Preamble/HeaderValid are latched but not routed to DIO1, so
	 * they show whether the receiver heard energy during the window.
	 */
	eng.telem.dio1_edges = tdma_port_dio1_edges();
	eng.telem.last_evt_dt_us = ts - tdma_port_last_boundary();
	eng.telem.evt_by_slot[eng.cur_slot]++;
	if (ev.irq == 0U) {
		eng.telem.drain_empty++;
	}
	if (ev.irq & SX126X_IRQ_PREAMBLE_DETECTED) {
		eng.telem.preamble_det++;
	}
	if (ev.irq & SX126X_IRQ_HEADER_VALID) {
		eng.telem.header_valid++;
	}

	if (ev.irq & SX126X_IRQ_TX_DONE) {
		eng.telem.tx_done++;
	}

	if (ev.irq & (SX126X_IRQ_CRC_ERR | SX126X_IRQ_HEADER_ERR)) {
		eng.telem.rx_crc_err++;
	} else if (ev.irq & SX126X_IRQ_RX_DONE) {
		struct tdma_hdr hdr;

		if (!ev.have_rx || tdma_buf_unpack_hdr(ev.raw, &hdr) != 0) {
			eng.telem.rx_bad_header++;
		} else {
			struct tdma_rx_msg msg = {
				.slot_id = hdr.slot_id,
				.frame_ctr = hdr.frame_ctr,
				.rssi = ev.rssi,
				.snr = ev.snr,
				.timestamp_us = ts,
			};

			memcpy(msg.payload, &ev.raw[TDMA_HDR_LEN],
			       TDMA_PAYLOAD_LEN);

			/* Drop-oldest on overflow: the stream must not stall
			 * the engine.
			 */
			if (k_msgq_put(&tdma_rx_msgq, &msg, K_NO_WAIT) != 0) {
				struct tdma_rx_msg scratch;

				(void)k_msgq_get(&tdma_rx_msgq, &scratch,
						 K_NO_WAIT);
				(void)k_msgq_put(&tdma_rx_msgq, &msg,
						 K_NO_WAIT);
			}
			eng.telem.rx_done++;

			/* Master beacons (slot 0) anchor the frame timing. */
			if (eng.cfg.role == TDMA_ROLE_SECONDARY &&
			    hdr.slot_id == 0) {
				tdma_core_sync_feed(ts, hdr.frame_ctr);
			}

			/* Post-RxDone slack is also a safe staging window. */
			rx_slot_slack_work();
		}
	}

	if (ev.irq & SX126X_IRQ_RX_TX_TIMEOUT) {
		/* Empty RX slot (or, pathologically, a TX that never
		 * finished): count and roll on — the frame never stalls.
		 */
		eng.telem.slot_timeouts++;
	}
}

void tdma_core_on_manual(struct tdma_manual_req *req)
{
	struct tdma_radio_event ev;

	switch (req->op) {
	case TDMA_MANUAL_TX:
		if (eng.state != TDMA_SYNC_STOPPED) {
			req->result = -EBUSY;
			break;
		}
		req->result = tdma_radio_manual_tx(eng.cfg.slot_id,
						   eng.manual_ctr++,
						   req->payload);
		break;

	case TDMA_MANUAL_RX:
		if (eng.state != TDMA_SYNC_STOPPED) {
			req->result = -EBUSY;
			break;
		}
		req->result = tdma_radio_manual_rx(req->timeout_ms, &ev);
		if (req->result == 0 && req->rx != NULL) {
			struct tdma_hdr hdr;

			if (tdma_buf_unpack_hdr(ev.raw, &hdr) != 0) {
				req->result = -EBADMSG;
				break;
			}
			memcpy(req->rx->payload, &ev.raw[TDMA_HDR_LEN],
			       TDMA_PAYLOAD_LEN);
			req->rx->slot_id = hdr.slot_id;
			req->rx->frame_ctr = hdr.frame_ctr;
			req->rx->rssi = ev.rssi;
			req->rx->snr = ev.snr;
			req->rx->timestamp_us = tdma_port_dio1_timestamp();
		}
		break;

	case TDMA_MANUAL_STANDBY:
		eng.state = TDMA_SYNC_STOPPED;
		req->result = tdma_radio_standby();
		break;

	default:
		req->result = -EINVAL;
		break;
	}

	k_sem_give(&req->done);
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

int tdma_init(const struct tdma_config *cfg)
{
	int ret;

	if (eng.initialized) {
		return -EALREADY;
	}
	if (cfg == NULL || cfg->slot_id >= TDMA_SLOT_COUNT ||
	    cfg->slot_duration_us != TDMA_SLOT_DURATION_US) {
		/* The port schedules with the compile-time constant; reject a
		 * config that disagrees rather than silently drifting.
		 */
		return -EINVAL;
	}

	eng.cfg = *cfg;
	eng.state = TDMA_SYNC_STOPPED;

	ret = sx126x_cmd_init();
	if (ret < 0) {
		return ret;
	}

	ret = tdma_port_init();
	if (ret < 0) {
		return ret;
	}

	ret = tdma_radio_init(cfg);
	if (ret < 0) {
		return ret;
	}

	eng.initialized = true;

	/* Handoff: from here the radio thread is the sole SPI owner. */
	k_sem_give(&tdma_spi_bus_sem);
	return 0;
}

int tdma_start(void)
{
	if (!eng.initialized) {
		return -EPERM;
	}
	if (eng.state != TDMA_SYNC_STOPPED) {
		return -EALREADY;
	}

	/* First tick advances into slot 0. */
	eng.cur_slot = TDMA_SLOT_COUNT - 1;
	eng.frame_ctr = 0;
	eng.payload_fresh = false;
	eng.hdr_written = false;
	eng.beacon_seen = false;
	eng.lock_streak = 0;
	eng.sync_have_prev = false;

	eng.state = (eng.cfg.role == TDMA_ROLE_MASTER) ? TDMA_SYNC_RUNNING
						       : TDMA_SYNC_SYNCING;

	int ret = tdma_port_schedule_start();

	if (ret < 0) {
		eng.state = TDMA_SYNC_STOPPED;
	}
	return ret;
}

void tdma_stop(void)
{
	struct tdma_manual_req req = { .op = TDMA_MANUAL_STANDBY };

	if (!eng.initialized || eng.state == TDMA_SYNC_STOPPED) {
		return;
	}

	tdma_port_schedule_stop();
	/* Park the radio from the radio thread (sole SPI owner); it also
	 * flips the engine state so pending events become no-ops.
	 */
	(void)tdma_port_manual_submit(&req);
}

int tdma_tx_submit(const uint8_t payload[TDMA_PAYLOAD_LEN])
{
	if (!eng.initialized) {
		return -EPERM;
	}

	return tdma_buf_stage(payload);
}

int tdma_set_tx_power(int8_t dbm)
{
	if (dbm < TDMA_TX_POWER_MIN_DBM || dbm > TDMA_TX_POWER_MAX_DBM) {
		return -EINVAL;
	}

	/* Deliberately no initialized check: pre-init requests are latched
	 * and picked up by the first transmit.
	 */
	tdma_radio_request_tx_power(dbm);
	return 0;
}

int tdma_manual_tx(const uint8_t payload[TDMA_PAYLOAD_LEN])
{
	struct tdma_manual_req req = {
		.op = TDMA_MANUAL_TX,
		.payload = payload,
	};

	if (!eng.initialized) {
		return -EPERM;
	}

	return tdma_port_manual_submit(&req);
}

int tdma_manual_rx(uint32_t timeout_ms, struct tdma_rx_msg *msg)
{
	struct tdma_manual_req req = {
		.op = TDMA_MANUAL_RX,
		.timeout_ms = timeout_ms,
		.rx = msg,
	};

	if (!eng.initialized) {
		return -EPERM;
	}

	return tdma_port_manual_submit(&req);
}
