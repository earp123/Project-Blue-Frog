/*
 * opus_dec - Opus decoding on the device: received clip chunks to the HAT
 * earpieces (CONFIG_SOAK_OPUS_DEC, payload mode CLIP, 80-byte payloads).
 * docs/opus_payload.md.
 *
 * Every peer's received chunk (clip test header, codec 1, then four 18 B
 * Opus SILK frames) is decoded with that peer's own persistent decoder into
 * 640 samples and handed to hat_audio_play_chunk(), which mixes the peers
 * into the earpiece output as it does Codec 2's (live audio G3). A chunk
 * that is overdue (a frame plus 20 ms after its predecessor) is lost, and
 * the decoder's PLC fills it then, in its own place in the playout
 * (decision 3): up to OPUS_DEC_PLC_MAX chunks in a row are played; past
 * that the decoder still runs its PLC, so its state follows real time, and
 * the playout bridges the rest with silence. Concealing on time, not when
 * the next chunk shows the gap, is what keeps a loss from becoming a late
 * write: a playout resync and 80 ms of extra delay (2026-10-30). The clips
 * are Opus-encoded on the host (c2clip.py opus); there is no device
 * encoder yet.
 *
 * Without CONFIG_SOAK_AUDIO_HAT the audio is decoded and dropped: the CPU
 * and memory figures of gate C1's decode half, on any unit.
 *
 * Threads: like c2_dec, one preemptible decoder thread, fed by the soak data
 * thread through a queue it never waits on. Decoder states are static (no
 * heap), and libopus's scratch is on the thread's stack (VAR_ARRAYS).
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef OPUS_DEC_H_
#define OPUS_DEC_H_

#include <stdint.h>

#include "tdma.h"

#define OPUS_DEC_PLC_MAX	2	/* chunks of PLC played before silence */

/* Start a run: a fresh decoder per peer slot (every slot but own_slot).
 * Returns 0, or -ENOMEM if a decoder state does not fit its buffer.
 */
int opus_dec_start(uint8_t own_slot);

/* Stop decoding. Safe to call when not started. */
void opus_dec_stop(void);

/* Soak data thread: one received payload. Never blocks; drops if full. */
void opus_dec_submit(uint8_t slot_id, const uint8_t payload[TDMA_PAYLOAD_LEN]);

struct opus_dec_stats {
	uint32_t decoded;	/* received chunks decoded */
	uint32_t plc_played;	/* lost chunks concealed and played */
	uint32_t plc_silent;	/* lost chunks past OPUS_DEC_PLC_MAX in a row */
	uint32_t resets;	/* outages past the PLC horizon: fresh decoder */
	uint32_t late;		/* arrived after being concealed (or repeats) */
	uint32_t bad;		/* not an Opus clip chunk (header) */
	uint32_t errors;	/* opus_decode() failures */
	uint32_t dropped;	/* queue full: the decoder fell behind */
	uint32_t dec_avg_us;	/* one chunk, four frames */
	uint32_t dec_max_us;
	uint32_t plc_max_us;	/* one lost chunk's four PLC frames */
	uint32_t late_max_us;	/* worst arrival past a frame after the last */
	uint32_t stack_unused;	/* decoder stack never touched, bytes */
	uint32_t state_bytes;	/* opus_decoder_get_size(1) */
	uint32_t cpu;		/* decoder thread, tenths of a percent */
};

void opus_dec_get_stats(struct opus_dec_stats *out);

#endif /* OPUS_DEC_H_ */
