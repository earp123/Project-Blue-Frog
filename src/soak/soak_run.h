/*
 * soak_run - shared fixed-length TDMA soak runner with CSV-over-UART logging.
 *
 * Each per-test main() in src/soak fills a struct soak_params and calls
 * soak_run(). The runner reads the role jumper (soak_role), brings up the
 * shared TDMA engine at the requested TX power on the locked PHY (SF5 / BW500 /
 * CR4-5 / 915 MHz, from tdma.h), runs a continuous exchange for a fixed time,
 * and streams machine-parseable lines over UART.
 *
 * Line format (all prefixed "SOAK,<id>," so an ingest script can filter them
 * out of any interleaved log output):
 *   SOAK,<id>,meta,role=..,slot=..,power=..dBm,phy=..,freq=..Hz,frame_ms=..,
 *        slot_ms=..,dur_s=..,period_s=..
 *   SOAK,<id>,hdr,t_s,role,sync,tx_done,rx_done,crc_err,bad_hdr,timeouts,
 *        stale,busy_to,rx0,rx1,rx2,rx3,rssi,snr,phase_us,ppm
 *   SOAK,<id>,row,<those columns>          (one per sample_period_s)
 *   SOAK,<id>,done,role=..,tx_done=..,rx_done=..,rx_peer=..,frames_exp=..,
 *        per_pct=..,crc_err=..,bad_hdr=..,timeouts=..,busy_to=..
 *   SOAK,<id>,END                          (capture sentinel)
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef SOAK_RUN_H_
#define SOAK_RUN_H_

#include <stdint.h>

struct soak_params {
	const char *test_id;      /* short id echoed in every line, e.g. "s01" */
	int8_t tx_power_dbm;      /* -9..+22; applied before the first TX */
	uint32_t duration_s;      /* total run length */
	uint32_t sample_period_s; /* CSV row cadence */
};

/*
 * Run one soak test to completion. Returns 0 on a clean run, or a negative
 * errno if the engine failed to come up (the reason is also emitted as a
 * "SOAK,<id>,error,..." line). The TDMA engine is stopped before returning.
 */
int soak_run(const struct soak_params *p);

#endif /* SOAK_RUN_H_ */
