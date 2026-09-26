/*
 * c2_enc - Codec 2 encoding on the device, for the encode test
 * (CONFIG_SOAK_C2_ENCODE, payload mode PCM). docs/c2_encode_test.md.
 *
 * A simulated microphone locked to the TDMA frame: the clip buffer holds raw
 * 8 kHz 16-bit PCM (uploaded over RTT, a whole number of 80 ms chunks). The
 * chunk for the unit's TX boundary B is complete at B - phase_us, and its
 * four 20 ms frames are captured one by one before that: frame k (0..3) at
 * B - phase_us - (3 - k) * 20 ms.
 *
 * Per-frame pipeline: the encoder thread encodes each frame into 8 bytes of
 * Codec 2 3200 as soon as it is captured, so once the chunk is complete only
 * the last frame's encode remains. It then builds the 40-byte payload (clip
 * test header + 32 B) and stages it with tdma_tx_submit() itself, with no
 * polling thread in between. What it staged is published for the soak data
 * thread to log as the TX record (c2_enc_staged()).
 *
 * Time-keyed like real audio: chunk n is the audio for the n-th TX boundary
 * since the unit went RUNNING (the PCM loops); a late chunk is staged late,
 * never re-timed. One Codec 2 state per run carries across frames, as it
 * would in a live encoder.
 *
 * Threads. The encoder is preemptible (K_PRIO_PREEMPT(0)), so the cooperative
 * radio and soak data threads always run ahead of it: encoding takes CPU
 * time from the UI, never from the radio. Wall-clock times, preemptions
 * included, are what the deadline sees and what is logged.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef C2_ENC_H_
#define C2_ENC_H_

#include <stdbool.h>
#include <stdint.h>

#include "tdma.h"

#define C2_ENC_CHUNK_SAMPLES	640	/* 80 ms at 8 kHz: 4 x 20 ms frames */
#define C2_ENC_CHUNK_PCM_BYTES	(C2_ENC_CHUNK_SAMPLES * 2)

/*
 * Start encoding for a run: a fresh Codec 2 3200 state (created on the
 * encoder thread, which has the stack for it), the PCM in the clip buffer,
 * each chunk complete phase_us before its TX boundary. Returns 0, -ENOMEM if
 * the codec could not be created, or -EINVAL if the clip is not PCM-sized.
 */
int c2_enc_start(uint32_t phase_us);

/* Stop and free the codec state. Safe to call when not started. */
void c2_enc_stop(void);

/* True if the clip buffer holds PCM this encoder can use. */
bool c2_enc_pcm_ok(void);

/* One chunk the encoder staged, for the TX record. */
struct c2_enc_tx {
	uint8_t payload[TDMA_PAYLOAD_LEN];
	uint32_t stage_us;	/* slot-clock time of tdma_tx_submit() */
	uint32_t lead_us;	/* TX boundary minus stage time (0 if after) */
	uint32_t ready_us;	/* last frame captured -> staged */
	uint32_t tx_done;	/* engine tx_done when staged */
};

/*
 * Soak data thread: the chunk staged since the last call, if any. The
 * encoder has already handed it to the engine; this is only for logging.
 */
bool c2_enc_staged(struct c2_enc_tx *out);

struct c2_enc_stats {
	uint32_t staged;	/* chunks handed to the engine this run */
	uint32_t late;		/* staged after the pre-TX pickup deadline */
	uint32_t busy;		/* tdma_tx_submit() found the stage occupied */
	uint32_t frame_enc_max_us;	/* worst single-frame encode */
	uint32_t ready_max_us;	/* worst last-frame-captured -> staged */
	uint32_t start_late_max_us;	/* worst wake-up after a capture */
	uint32_t stack_unused;	/* encoder stack never touched, bytes */
	uint32_t unlogged;	/* staged records overwritten before logging */
};

void c2_enc_get_stats(struct c2_enc_stats *out);

#endif /* C2_ENC_H_ */
