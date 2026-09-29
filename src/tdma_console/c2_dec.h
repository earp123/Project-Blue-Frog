/*
 * c2_dec - Codec 2 decoding on the device: the receive side of the voice
 * path (CONFIG_SOAK_C2_ENCODE, payload mode PCM). docs/live_audio_wm8960.md.
 *
 * Every peer's received chunk (clip test header + four Codec 2 3200 frames)
 * is decoded with that peer's own persistent Codec 2 state into 640 samples,
 * then summed with saturation into a mix buffer, which is what a playout
 * path will consume. Gate G0 of the live-audio task runs this before any
 * audio hardware exists: the output is discarded, and what matters is what
 * decoding and mixing cost on top of encoding.
 *
 * To size the four-unit case on a three-unit bench, "extra" decode streams
 * decode the lowest peer's chunks again with states of their own: the same
 * work a third peer would bring.
 *
 * Threads. The decoder is preemptible, one below the encoder (the encoder
 * has the hard staging deadline; decoded audio will go to a playout buffer)
 * and above the UI. The soak data thread hands it chunks through a queue
 * and never waits on it.
 *
 * It also keeps the whole-CPU accounting for the run (thread runtime stats,
 * DWT-timed): busy share overall and per voice-path thread.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef C2_DEC_H_
#define C2_DEC_H_

#include <stdbool.h>
#include <stdint.h>

#include "tdma.h"

#define C2_DEC_EXTRA_MAX	2

/*
 * Start decoding for a run: one Codec 2 state per peer slot (every slot but
 * own_slot) plus `extra` phantom streams, created on the decoder thread.
 * Also starts this run's CPU accounting. Returns 0 or -ENOMEM.
 */
int c2_dec_start(uint8_t own_slot, uint32_t extra);

/* Stop and free the states. Safe to call when not started. */
void c2_dec_stop(void);

/* Soak data thread: one received payload. Never blocks; drops if full. */
void c2_dec_submit(uint8_t slot_id, const uint8_t payload[TDMA_PAYLOAD_LEN]);

struct c2_dec_stats {
	uint32_t decoded;	/* peer chunks decoded (not counting extras) */
	uint32_t extra;		/* phantom-stream chunks decoded */
	uint32_t bad;		/* not a clip payload (card #23's first packet) */
	uint32_t dropped;	/* queue full: the decoder fell behind */
	uint32_t dec_max_us;	/* worst decode of one chunk (4 frames) */
	uint32_t dec_avg_us;
	uint32_t mix_max_us;	/* worst saturating mix of one chunk */
	uint32_t stack_unused;	/* decoder stack never touched, bytes */
	uint32_t heap_left;	/* largest block malloc can still give after
				 * this run's states were created, bytes */
};

void c2_dec_get_stats(struct c2_dec_stats *out);

/* This run's CPU use, in tenths of a percent of the app core. */
struct c2_cpu {
	uint32_t busy;		/* everything but idle */
	uint32_t enc;		/* encoder thread */
	uint32_t dec;		/* decoder thread (decode + mix) */
	uint32_t radio;		/* TDMA radio thread */
	uint32_t data;		/* soak data thread */
};

void c2_dec_get_cpu(struct c2_cpu *out);

/*
 * Isolated benchmark, no soak running: encode `chunks` chunks of the PCM in
 * the clip buffer back to back, decode each, mix it, and time each step with
 * the cycle counter. Blocks the caller until done (~35 ms per chunk). us per
 * chunk; p99 over the run. Returns 0, -EBUSY (a soak is running), -EINVAL
 * (no PCM loaded) or -ENOMEM.
 */
struct c2_bench {
	uint32_t n;
	uint32_t enc_min, enc_avg, enc_p99, enc_max;
	uint32_t dec_min, dec_avg, dec_p99, dec_max;
	uint32_t mix_max;
	uint32_t heap_per_state;	/* heap one Codec 2 decoder state takes,
					 * by largest-free-block probe */
};

int c2_dec_bench(uint32_t chunks, struct c2_bench *out);

#endif /* C2_DEC_H_ */
