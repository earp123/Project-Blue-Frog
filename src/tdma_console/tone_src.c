/*
 * tone_src - per-slot PCM tone for the soak payload. See tone_src.h.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/toolchain.h>

#include "tone_src.h"

/*
 * TONE_SILENCE + round(TONE_AMPL * sin(2 * pi * i / 256)). No entry lies
 * within 2e-4 of a rounding tie, so every rounding convention (and the host's
 * floor(x + 0.5)) produces this exact table.
 */
static const uint8_t sine_lut[256] = {
	128, 130, 133, 135, 138, 140, 143, 145, 148, 150, 152, 155, 157, 159, 162, 164,
	166, 169, 171, 173, 175, 177, 179, 181, 184, 186, 188, 190, 191, 193, 195, 197,
	199, 200, 202, 204, 205, 207, 208, 210, 211, 212, 214, 215, 216, 217, 218, 219,
	220, 221, 222, 223, 224, 224, 225, 226, 226, 227, 227, 227, 228, 228, 228, 228,
	228, 228, 228, 228, 228, 227, 227, 227, 226, 226, 225, 224, 224, 223, 222, 221,
	220, 219, 218, 217, 216, 215, 214, 212, 211, 210, 208, 207, 205, 204, 202, 200,
	199, 197, 195, 193, 191, 190, 188, 186, 184, 181, 179, 177, 175, 173, 171, 169,
	166, 164, 162, 159, 157, 155, 152, 150, 148, 145, 143, 140, 138, 135, 133, 130,
	128, 126, 123, 121, 118, 116, 113, 111, 108, 106, 104, 101,  99,  97,  94,  92,
	 90,  87,  85,  83,  81,  79,  77,  75,  72,  70,  68,  66,  65,  63,  61,  59,
	 57,  56,  54,  52,  51,  49,  48,  46,  45,  44,  42,  41,  40,  39,  38,  37,
	 36,  35,  34,  33,  32,  32,  31,  30,  30,  29,  29,  29,  28,  28,  28,  28,
	 28,  28,  28,  28,  28,  29,  29,  29,  30,  30,  31,  32,  32,  33,  34,  35,
	 36,  37,  38,  39,  40,  41,  42,  44,  45,  46,  48,  49,  51,  52,  54,  56,
	 57,  59,  61,  63,  65,  66,  68,  70,  72,  75,  77,  79,  81,  83,  85,  87,
	 90,  92,  94,  97,  99, 101, 104, 106, 108, 111, 113, 116, 118, 121, 123, 126,
};

/* Phase step per sample, 2^32 = one cycle: round(f * 2^32 / fs). */
#define TONE_DELTA(s) \
	((uint32_t)(((((uint64_t)TONE_F0_HZ * ((s) + 1)) << 32) + TONE_FS_HZ / 2) / \
		    TONE_FS_HZ))

static const uint32_t phase_delta[TDMA_SLOT_COUNT] = {
	TONE_DELTA(0), TONE_DELTA(1), TONE_DELTA(2), TONE_DELTA(3),
};

BUILD_ASSERT(TDMA_SLOT_COUNT == 4, "one phase_delta entry per slot");
BUILD_ASSERT(TONE_F0_HZ * TDMA_SLOT_COUNT < TONE_FS_HZ / 2,
	     "the highest slot's tone must stay under Nyquist");

void tone_src_fill(uint8_t out[TONE_CHUNK], uint8_t slot_id, uint32_t n)
{
	uint32_t delta = phase_delta[slot_id % TDMA_SLOT_COUNT];
	/* Phase at the chunk's first sample. uint32_t wrap is exactly phase
	 * mod 2*pi, so this equals an accumulator run through n chunks.
	 */
	uint32_t phase = n * TONE_CHUNK * delta;

	for (int i = 0; i < TONE_CHUNK; i++) {
		out[i] = sine_lut[phase >> 24];
		phase += delta;
	}
}
