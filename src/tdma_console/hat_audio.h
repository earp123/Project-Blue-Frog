/*
 * hat_audio - the WM8960 Audio HAT as the unit's microphone
 * (CONFIG_SOAK_AUDIO_HAT, payload mode MIC). docs/live_audio_wm8960.md, G2.
 *
 * The codec is the I2S master at 8 kHz and free-runs against the TDMA
 * frame (design decision 2). An audio thread takes 5 ms blocks of both
 * mics from i2s0 into a stereo capture ring. The encoder pulls 20 ms mono
 * frames from it (hat_audio_frame()): left, right, or their average
 * (decision 3, revised after G1).
 *
 * The encoder reads the ring in sequence, so the audio has no gaps or
 * overlaps. Its cursor trails the newest sample by a small margin, and
 * because the two clocks drift apart, the margin wanders. When it leaves
 * its window the cursor jumps back to the target and the jump is counted
 * as a slip: "short" (the encoder caught up with the audio; samples
 * repeat) or "drop" (it fell behind; samples are skipped). At the
 * measured -5 ppm the window lasts over half an hour.
 *
 * The DAC side runs too, sending silence for now; playout is G3. The
 * WM8960 reads each DAC word one bit clock early (G1), so G3 must pack its
 * output as apps/wm8960_smoke's pack_tx() does.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef HAT_AUDIO_H_
#define HAT_AUDIO_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum hat_mix {
	HAT_MIX_AVG = 0,	/* (left + right) / 2: the default */
	HAT_MIX_LEFT = 1,
	HAT_MIX_RIGHT = 2,
};

/* True once the codec has ACKed its setup and I2S blocks are arriving. */
bool hat_audio_ok(void);

/* Which mono mix the encoder gets. Takes effect at the next frame. */
void hat_audio_set_mix(enum hat_mix mix);
enum hat_mix hat_audio_get_mix(void);

/* Start of a run: the next hat_audio_frame() re-anchors the cursor. */
void hat_audio_capture_reset(void);

/* The next n (<= 320) mono samples in sequence, into pcm. Never blocks:
 * zeros if the HAT is not running.
 */
void hat_audio_frame(int16_t *pcm, size_t n);

struct hat_audio_stats {
	bool codec_ok;		/* setup ACKed */
	uint32_t i2c_retries;	/* NACKed writes retried, since boot */
	uint32_t blocks;	/* 5 ms blocks received */
	uint32_t rate_mhz;	/* LRCLK measured since the first block, mHz */
	uint32_t restarts;	/* I2S errors recovered */
	uint32_t timeouts;	/* 500 ms with no block */
	uint32_t slip_short;	/* cursor caught up with the audio */
	uint32_t slip_drop;	/* cursor fell behind */
	uint32_t frames;	/* 20 ms frames served since the reset */
	int16_t peak[2];	/* per mic, since the previous stats call */
};

/* Snapshot; clears the peaks. */
void hat_audio_get_stats(struct hat_audio_stats *out);

#endif /* HAT_AUDIO_H_ */
