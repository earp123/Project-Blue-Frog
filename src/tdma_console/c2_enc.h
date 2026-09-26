/*
 * c2_enc - Codec 2 encoding on the device, for the encode test
 * (CONFIG_SOAK_C2_ENCODE, payload mode PCM). docs/c2_encode_test.md.
 *
 * A simulated microphone locked to the TDMA frame: the clip buffer holds raw
 * 8 kHz 16-bit PCM (uploaded over RTT, a whole number of 80 ms chunks), and
 * the 640 samples for the unit's TX boundary B become available at
 * B - phase_us. At that moment the encoder thread encodes them into four
 * Codec 2 3200 frames (32 B) and publishes the chunk, header included, for
 * the soak data thread to stage.
 *
 * Time-keyed like real audio: chunk n is the audio for the n-th TX boundary
 * since the unit went RUNNING (the PCM loops), so a chunk the encoder
 * finishes too late is dropped, never delayed. One Codec 2 state per run,
 * created at c2_enc_start(), carries across chunks as it would in a live
 * encoder.
 *
 * Threads. The encoder is preemptible (K_PRIO_PREEMPT(0)), so the cooperative
 * radio and soak data threads always run ahead of it: encoding can take
 * CPU time from the UI, never from the radio. Its wall-clock encode time,
 * preemptions included, is what the deadline sees, and is what gets logged.
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
 * Start encoding for a run: a fresh Codec 2 3200 state, the PCM in the clip
 * buffer (clip_src_valid(), a multiple of C2_ENC_CHUNK_PCM_BYTES), each chunk
 * made available phase_us before its TX boundary. Returns 0, -ENOMEM if the
 * codec could not be created, or -EINVAL if the clip is not PCM-sized.
 */
int c2_enc_start(uint32_t phase_us);

/* Stop and free the codec state. Safe to call when not started. */
void c2_enc_stop(void);

/* True if the clip buffer holds PCM this encoder can use. */
bool c2_enc_pcm_ok(void);

/*
 * Soak data thread: if the chunk for the TX boundary at boundary_us is done,
 * copy its 40 B payload (clip test header + 32 B of Codec 2) into out, its
 * encode time into *enc_us, and consume it. A chunk for an earlier boundary
 * is discarded and counted as late.
 */
bool c2_enc_take(uint32_t boundary_us, uint8_t out[TDMA_PAYLOAD_LEN],
		 uint32_t *enc_us);

struct c2_enc_stats {
	uint32_t encoded;	/* chunks encoded this run */
	uint32_t late;		/* finished after their boundary had passed */
	uint32_t skipped;	/* boundaries the encoder woke too late for */
	uint32_t enc_max_us;	/* worst wall-clock encode time this run */
	uint32_t enc_last_us;
	uint32_t start_late_max_us;	/* worst wake-up after availability */
	uint32_t stack_unused;	/* encoder stack never touched, bytes */
};

void c2_enc_get_stats(struct c2_enc_stats *out);

#endif /* C2_ENC_H_ */
