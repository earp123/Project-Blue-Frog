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
 * Playout (G3): the decoder mixes each peer's chunks into a ring that the
 * same thread plays to the earpieces (hat_audio_play_chunk()). The speaker
 * output is muted. The WM8960 reads each DAC word one bit clock early
 * (G1), so the output words are pre-shifted (pack_tx()).
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

/* ---- Playout (G3) ---- */

/* Start of a run: every peer re-anchors at its next chunk. */
void hat_audio_play_reset(void);

/*
 * The decoder: chunk idx (the test header's chunk index) of peer slot,
 * n samples of decoded audio, mixed into the earpiece output about 40 ms
 * ahead of what is playing. A lost chunk is silence for that peer.
 */
void hat_audio_play_chunk(uint8_t slot, uint16_t idx, const int16_t *pcm,
			  size_t n);

struct hat_play_stats {
	uint32_t chunks;	/* mixed in */
	uint32_t starts;	/* peers anchored (first chunk, or after an
				 * outage longer than PLAY_GAP_MAX) */
	uint32_t missing;	/* chunk-index gaps bridged with silence */
	uint32_t dup;		/* repeated or old chunks ignored */
	uint32_t resync;	/* late (would play behind the cursor) or far
				 * ahead: re-anchored, an audible glitch */
	uint32_t corr_drop;	/* one-sample corrections, lead too long */
	uint32_t corr_rep;	/* one-sample corrections, lead too short */
	int32_t lead_min;	/* samples between write and play, this run */
	int32_t lead_max;
};

void hat_audio_get_play_stats(struct hat_play_stats *out);

/* Test only: play just peer slot (0..3) to the earpieces, or -1 for all.
 * Every peer is still decoded either way.
 */
void hat_audio_set_listen(int slot);
int hat_audio_get_listen(void);

/* Earpiece (headphone) volume, LOUT1/ROUT1 code: 0x79 = 0 dB, 1 dB steps. */
void hat_audio_hp_vol(uint8_t code);

#endif /* HAT_AUDIO_H_ */
