/*
 * tx_ladder - the TX power ladder test (payload mode LADDER).
 *
 * A fixed-distance link test that needs no settings. From GO each unit logs
 * its receiver noise floor, then steps its own TX power up a fixed ladder,
 * LADDER_STEP_FRAMES transmissions per rung, holds the top rung for a short
 * tail and ends the run by itself.
 *
 * The ladder is keyed to the unit's own transmissions (tx_done since the run
 * started), not to the clock. A secondary therefore starts its ladder when it
 * locks, and every rung holds the same number of packets. Started with the
 * secondary first, the two ladders run within a frame or two of each other;
 * the tail covers a later start of up to LADDER_TAIL_FRAMES frames.
 *
 * Every packet carries its rung, so the peer's RX records alone say what
 * power each received packet was sent at, with no clock alignment between
 * the two logs. Both units run it on the DK buttons alone, B1 GO and B4
 * STOP. tools/ladder_report.py turns one or both logs into RSSI,
 * SNR and loss per rung and direction.
 *
 * Payload (40 B):
 *   [0]      LADDER_MAGIC ('L')
 *   [1]      rung: 0..LADDER_STEPS-1, LADDER_STEPS for the tail
 *   [2]      TX power, int8 dBm
 *   [3]      sender's slot
 *   [4..7]   n, the sender's TX index in the run, uint32 LE
 *   [8..39]  (n + i) & 0xFF, a check pattern: corruption that passes the
 *            radio's CRC still shows
 *
 * Header-only, like payload_src.h, which includes it.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef TX_LADDER_H_
#define TX_LADDER_H_

#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/util.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "tdma.h"

#define LADDER_MAGIC		0x4C	/* 'L' */
#define LADDER_STEPS		8
#define LADDER_STEP_MS		30000
#define LADDER_TAIL_MS		20000
#define LADDER_STEP_FRAMES	(LADDER_STEP_MS * 1000 / TDMA_FRAME_DURATION_US)
#define LADDER_TAIL_FRAMES	(LADDER_TAIL_MS * 1000 / TDMA_FRAME_DURATION_US)
#define LADDER_FRAMES \
	(LADDER_STEPS * LADDER_STEP_FRAMES + LADDER_TAIL_FRAMES)
/* Nominal run time, for the screens: 4:20. */
#define LADDER_MS \
	((int64_t)LADDER_FRAMES * TDMA_FRAME_DURATION_US / 1000)

/* 1 ms receiver RSSI samples taken at GO, before the engine starts. */
#define LADDER_NOISE_SAMPLES	1000

/* TX power of a rung: -6 to +22 dBm in 4 dB steps; the tail holds the top. */
static inline int8_t ladder_rung_dbm(uint32_t rung)
{
	static const int8_t dbm[] = { -6, -2, 2, 6, 10, 14, 18, 22 };

	BUILD_ASSERT(ARRAY_SIZE(dbm) == LADDER_STEPS, "one power per rung");
	return dbm[MIN(rung, LADDER_STEPS - 1U)];
}

/* The rung TX index n goes out on; LADDER_STEPS is the tail. */
static inline uint32_t ladder_rung(uint32_t n)
{
	return MIN(n / LADDER_STEP_FRAMES, (uint32_t)LADDER_STEPS);
}

/* True once the run's last transmission (the tail's end) is done. */
static inline bool ladder_over(uint32_t n)
{
	return n >= LADDER_FRAMES;
}

static inline void ladder_fill(uint8_t out[TDMA_PAYLOAD_LEN], uint8_t slot_id,
			       uint32_t n)
{
	uint32_t rung = ladder_rung(n);

	out[0] = LADDER_MAGIC;
	out[1] = (uint8_t)rung;
	out[2] = (uint8_t)ladder_rung_dbm(rung);
	out[3] = slot_id;
	sys_put_le32(n, &out[4]);
	for (int i = 8; i < TDMA_PAYLOAD_LEN; i++) {
		out[i] = (uint8_t)(n + i);
	}
}

/*
 * Runner hook at GO, the engine initialized and stopped: measure the noise
 * floor into *nf (logged once the log is open, soak_log_noise()) and latch
 * the first rung's power so the run's first packet goes out on it. Returns
 * tdma_noise_floor()'s result; a failed measurement does not stop the run.
 */
static inline int ladder_prepare(struct tdma_noise *nf)
{
	int rc = tdma_noise_floor(LADDER_NOISE_SAMPLES, nf);

	(void)tdma_set_tx_power(ladder_rung_dbm(0));
	return rc;
}

/*
 * Runner hook before filling TX index n: request the rung's power when it
 * changes. *dbm is the power last requested. Only on a change, since every
 * request adds a SetTxParams ahead of that packet's SetTx.
 */
static inline void ladder_stage(uint32_t n, int8_t *dbm)
{
	int8_t want = ladder_rung_dbm(ladder_rung(n));

	if (want != *dbm && tdma_set_tx_power(want) == 0) {
		*dbm = want;
	}
}

/* Progress for the screens: "3/8", "tail", or "sync" before the first TX. */
static inline void ladder_fmt(char *out, size_t len, uint32_t n)
{
	uint32_t rung = ladder_rung(n);

	if (n == 0) {
		snprintf(out, len, "sync");
	} else if (rung < LADDER_STEPS) {
		snprintf(out, len, "%u/%u", rung + 1U, (unsigned int)LADDER_STEPS);
	} else {
		snprintf(out, len, "tail");
	}
}

#endif /* TX_LADDER_H_ */
