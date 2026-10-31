/*
 * payload_src - which payload a soak sends, and the one fill call the runner
 * makes per staged frame.
 *
 * Four sources, enum soak_payload_mode: the ramp, tone_src, clip_src and the
 * TX power ladder (tx_ladder.h). With CONFIG_SOAK_RTT all four are linked
 * and the bench port's "mode" command picks the one the next soak starts in
 * (rtt_link_mode()); without it only the source chosen by the SOAK_PAYLOAD
 * Kconfig choice is linked.
 * The runner latches the mode at soak start and passes it back to every
 * payload_fill() of that run, so a run never switches source.
 *
 * Header-only on purpose: in a ramp build without RTT every other branch is
 * a compile-time constant false, and payload_fill() folds to the ramp loop
 * the runner always had.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef PAYLOAD_SRC_H_
#define PAYLOAD_SRC_H_

#include <zephyr/sys/util.h>
#include <stdint.h>
#include <string.h>

#include "tdma.h"
#include "soak_log.h"
#include "tone_src.h"
#include "clip_src.h"
#include "tx_ladder.h"
#ifdef CONFIG_SOAK_RTT
#include "rtt_link.h"
#endif
/* An on-device encoder: Codec 2, or Opus (CONFIG_SOAK_OPUS_ENC). */
#if defined(CONFIG_SOAK_C2_ENCODE) || defined(CONFIG_SOAK_OPUS_ENC)
#define PAYLOAD_HAS_ENC 1
#include "c2_enc.h"
#endif
#ifdef CONFIG_SOAK_C2_ENCODE
#include "c2_dec.h"
#endif
#ifdef CONFIG_SOAK_AUDIO_HAT
#include "hat_audio.h"
#endif
#ifdef CONFIG_SOAK_OPUS_DEC
#include "opus_dec.h"
#endif

/* A source is linked if the bench port can select it, or if it is the one. */
#define PAYLOAD_TONE_LINKED \
	(IS_ENABLED(CONFIG_SOAK_RTT) || IS_ENABLED(CONFIG_SOAK_PAYLOAD_TONE))
#define PAYLOAD_CLIP_LINKED \
	(IS_ENABLED(CONFIG_SOAK_RTT) || IS_ENABLED(CONFIG_SOAK_PAYLOAD_CLIP))
#define PAYLOAD_LADDER_LINKED \
	(IS_ENABLED(CONFIG_SOAK_RTT) || IS_ENABLED(CONFIG_SOAK_PAYLOAD_LADDER))
/* The on-device encoder: only with CONFIG_SOAK_C2_ENCODE or
 * CONFIG_SOAK_OPUS_ENC, and never a Kconfig default (it needs a PCM clip,
 * uploaded over RTT, and "mode pcm").
 */
#define PAYLOAD_PCM_LINKED	IS_ENABLED(PAYLOAD_HAS_ENC)
/* The same encoder fed by the WM8960 HAT's mics ("mode mic"). An Opus
 * decode-only build has the HAT for its earpieces but no encoder: no mic.
 */
#define PAYLOAD_MIC_LINKED \
	(IS_ENABLED(CONFIG_SOAK_AUDIO_HAT) && IS_ENABLED(PAYLOAD_HAS_ENC))

/* The modes whose chunks the on-device encoder makes and stages itself,
 * and whose received chunks go to the decoder.
 */
static inline bool payload_is_c2(enum soak_payload_mode mode)
{
	return (PAYLOAD_PCM_LINKED && mode == SOAK_PAYLOAD_PCM) ||
	       (PAYLOAD_MIC_LINKED && mode == SOAK_PAYLOAD_MIC);
}

/* The ladder run: the runner also steps the TX power and ends the run
 * itself (tx_ladder.h).
 */
static inline bool payload_is_ladder(enum soak_payload_mode mode)
{
	return PAYLOAD_LADDER_LINKED && mode == SOAK_PAYLOAD_LADDER;
}

/* The Kconfig choice, as the boot default. */
#define PAYLOAD_DEFAULT_MODE \
	(IS_ENABLED(CONFIG_SOAK_PAYLOAD_TONE) ? SOAK_PAYLOAD_TONE : \
	 IS_ENABLED(CONFIG_SOAK_PAYLOAD_CLIP) ? SOAK_PAYLOAD_CLIP : \
	 IS_ENABLED(CONFIG_SOAK_PAYLOAD_LADDER) ? SOAK_PAYLOAD_LADDER : \
						SOAK_PAYLOAD_RAMP)

/* The mode the next soak starts in. */
static inline enum soak_payload_mode payload_next_mode(void)
{
#ifdef CONFIG_SOAK_RTT
	return rtt_link_mode();
#else
	return PAYLOAD_DEFAULT_MODE;
#endif
}

/* True if a soak can start in mode (a clip must be loaded; PCM mode needs
 * it PCM-sized).
 */
static inline bool payload_ready(enum soak_payload_mode mode)
{
#ifdef PAYLOAD_HAS_ENC
	if (mode == SOAK_PAYLOAD_PCM) {
		return c2_enc_pcm_ok();
	}
#endif
	if (mode == SOAK_PAYLOAD_MIC) {
#if defined(CONFIG_SOAK_AUDIO_HAT) && defined(PAYLOAD_HAS_ENC)
		return hat_audio_ok();
#else
		return false;
#endif
	}
	return !(PAYLOAD_CLIP_LINKED && mode == SOAK_PAYLOAD_CLIP) ||
	       clip_src_valid();
}

/* PCM and MIC modes: a received peer payload, for the decoder: c2_dec, or
 * opus_dec in an Opus build.
 */
static inline void payload_rx_pcm(uint8_t slot_id,
				  const uint8_t payload[TDMA_PAYLOAD_LEN])
{
#if defined(CONFIG_SOAK_C2_ENCODE)
	c2_dec_submit(slot_id, payload);
#elif defined(CONFIG_SOAK_OPUS_ENC)
	opus_dec_submit(slot_id, payload);
#else
	ARG_UNUSED(slot_id);
	ARG_UNUSED(payload);
#endif
}

/* PCM and MIC modes: run start of the decoder that payload_rx_pcm() feeds
 * (c2_dec with its "decx" phantom streams, or opus_dec), and its stop.
 */
static inline int payload_pcm_dec_start(uint8_t own_slot)
{
#if defined(CONFIG_SOAK_C2_ENCODE)
	return c2_dec_start(own_slot, rtt_link_decx());
#elif defined(CONFIG_SOAK_OPUS_ENC)
	return opus_dec_start(own_slot);
#else
	ARG_UNUSED(own_slot);
	return 0;
#endif
}

static inline void payload_pcm_dec_stop(void)
{
#if defined(CONFIG_SOAK_C2_ENCODE)
	c2_dec_stop();
#elif defined(CONFIG_SOAK_OPUS_ENC)
	opus_dec_stop();
#endif
}

/* CLIP mode with CONFIG_SOAK_OPUS_DEC: the peers' Opus chunks are decoded
 * on the unit (opus_dec.h) and, with the HAT, played to the earpieces.
 */
static inline bool payload_decodes_clip(enum soak_payload_mode mode)
{
	return IS_ENABLED(CONFIG_SOAK_OPUS_DEC) && mode == SOAK_PAYLOAD_CLIP;
}

/* Run start and stop for that decoder; no-ops in other builds. */
static inline int payload_clip_dec_start(uint8_t own_slot)
{
#ifdef CONFIG_SOAK_OPUS_DEC
	return opus_dec_start(own_slot);
#else
	ARG_UNUSED(own_slot);
	return 0;
#endif
}

static inline void payload_clip_dec_stop(void)
{
#ifdef CONFIG_SOAK_OPUS_DEC
	opus_dec_stop();
#endif
}

/* A received peer payload, for that decoder. */
static inline void payload_rx_clip(uint8_t slot_id,
				   const uint8_t payload[TDMA_PAYLOAD_LEN])
{
#ifdef CONFIG_SOAK_OPUS_DEC
	opus_dec_submit(slot_id, payload);
#else
	ARG_UNUSED(slot_id);
	ARG_UNUSED(payload);
#endif
}

/* MIC mode: the encoder's mono mix (META p16, enum hat_mix). */
static inline uint32_t payload_mic_mix(void)
{
#ifdef CONFIG_SOAK_AUDIO_HAT
	return (uint32_t)hat_audio_get_mix();
#else
	return 0;
#endif
}

/* PCM mode: 80 ms chunks in the clip buffer (META). */
static inline uint32_t payload_pcm_chunks(void)
{
#ifdef PAYLOAD_HAS_ENC
	uint32_t len;

	(void)clip_src_data(&len);
	return len / C2_ENC_CHUNK_PCM_BYTES;
#else
	return 0;
#endif
}

/*
 * PCM mode: the chunk the encoder has staged since last asked, if any, for
 * the TX record: payload, stage time, lead, capture-to-stage time and the
 * engine's tx_done at staging (c2_enc_staged()). The encoder hands chunks
 * to the engine itself, so the runner only logs them. Always false in other
 * builds.
 */
static inline bool payload_take_pcm(uint8_t out[TDMA_PAYLOAD_LEN],
				    uint32_t *stage_us, uint32_t *lead_us,
				    uint32_t *ready_us, uint32_t *tx_done)
{
#ifdef PAYLOAD_HAS_ENC
	struct c2_enc_tx rec;

	if (!c2_enc_staged(&rec)) {
		return false;
	}
	memcpy(out, rec.payload, TDMA_PAYLOAD_LEN);
	*stage_us = rec.stage_us;
	*lead_us = rec.lead_us;
	*ready_us = rec.ready_us;
	*tx_done = rec.tx_done;
	return true;
#else
	ARG_UNUSED(out);
	ARG_UNUSED(stage_us);
	ARG_UNUSED(lead_us);
	ARG_UNUSED(ready_us);
	ARG_UNUSED(tx_done);
	return false;
#endif
}

/*
 * Whether to stage the next payload now. lead_us 0 stages at once, which is
 * what the runner has always done: right after its own TxDone, so a payload
 * waits most of a frame. Otherwise it waits until lead_us before this
 * unit's next TX boundary (tdma_next_tx_us()). *lead_out gets the lead
 * actually achieved, logged in the TX record. Without RTT this is always
 * true and costs nothing.
 */
static inline bool payload_stage_due(uint32_t lead_us, uint32_t *lead_out)
{
#ifdef CONFIG_SOAK_RTT
	uint32_t now = tdma_now_us();
	uint32_t lead = tdma_next_tx_us() - now;

	if (lead_us != 0 && lead > lead_us) {
		return false;
	}
	*lead_out = lead;
#else
	ARG_UNUSED(lead_us);
	ARG_UNUSED(lead_out);
#endif
	return true;
}

/*
 * Fill one staged payload. n is the unit's TX-slot index in the run (keyed
 * to tx_done, see tone_src.h); seq is the ramp base, which advances once per
 * staged frame as it always has.
 */
static inline void payload_fill(uint8_t out[TDMA_PAYLOAD_LEN],
				enum soak_payload_mode mode, uint8_t slot_id,
				uint32_t n, uint8_t seq)
{
	if (PAYLOAD_TONE_LINKED && mode == SOAK_PAYLOAD_TONE) {
		tone_src_fill(out, slot_id, n);
	} else if (PAYLOAD_CLIP_LINKED && mode == SOAK_PAYLOAD_CLIP) {
		clip_src_fill(out, n);
	} else if (payload_is_ladder(mode)) {
		ladder_fill(out, slot_id, n);
	} else {
		for (int i = 0; i < TDMA_PAYLOAD_LEN; i++) {
			out[i] = (uint8_t)(seq + i);
		}
	}
}

#endif /* PAYLOAD_SRC_H_ */
