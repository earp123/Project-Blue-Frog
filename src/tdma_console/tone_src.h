/*
 * tone_src - per-slot PCM tone for the soak payload (CONFIG_SOAK_PAYLOAD_TONE).
 *
 * Each unit fills its 40-byte payload with 40 samples of a sine at
 * TONE_F0_HZ * (slot_id + 1): 8 kHz, 8-bit unsigned (silence = 128). A
 * receiver's log then holds its peers' audio, which
 * tools/reconstruct_tone.py turns into a WAV (docs/tone_payload_test.md).
 *
 * Stateless and frame-keyed: chunk n is the audio for the n-th TX slot of the
 * run, so a failed submit retried for the same slot yields the same bytes,
 * and a missed frame's audio is simply gone rather than delaying every later
 * chunk. The host regenerates chunks with the same integer math, so any
 * change here must be mirrored in reconstruct_tone.py.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef TONE_SRC_H_
#define TONE_SRC_H_

#include <stdint.h>

#include "tdma.h"

#define TONE_FS_HZ	8000
#define TONE_F0_HZ	330	/* slot s plays TONE_F0_HZ * (s + 1) */
#define TONE_AMPL	100	/* peak deviation about TONE_SILENCE */
#define TONE_SILENCE	128
#define TONE_CHUNK	TDMA_PAYLOAD_LEN	/* samples per transmitted frame */

/* Write chunk n of slot_id's tone into out. Pure function of its arguments. */
void tone_src_fill(uint8_t out[TONE_CHUNK], uint8_t slot_id, uint32_t n);

#endif /* TONE_SRC_H_ */
