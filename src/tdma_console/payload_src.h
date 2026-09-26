/*
 * payload_src - which payload a soak sends, and the one fill call the runner
 * makes per staged frame.
 *
 * Three sources, enum soak_payload_mode: the ramp, tone_src and clip_src.
 * With CONFIG_SOAK_RTT all three are linked and the bench port's "mode"
 * command picks the one the next soak starts in (rtt_link_mode()); without
 * it only the source chosen by the SOAK_PAYLOAD Kconfig choice is linked.
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

#include "tdma.h"
#include "soak_log.h"
#include "tone_src.h"
#include "clip_src.h"
#ifdef CONFIG_SOAK_RTT
#include "rtt_link.h"
#endif
#ifdef CONFIG_SOAK_C2_ENCODE
#include "c2_enc.h"
#endif

/* A source is linked if the bench port can select it, or if it is the one. */
#define PAYLOAD_TONE_LINKED \
	(IS_ENABLED(CONFIG_SOAK_RTT) || IS_ENABLED(CONFIG_SOAK_PAYLOAD_TONE))
#define PAYLOAD_CLIP_LINKED \
	(IS_ENABLED(CONFIG_SOAK_RTT) || IS_ENABLED(CONFIG_SOAK_PAYLOAD_CLIP))
/* The on-device encoder: only with CONFIG_SOAK_C2_ENCODE, and never a Kconfig
 * default (it needs a PCM clip, uploaded over RTT, and "mode pcm").
 */
#define PAYLOAD_PCM_LINKED	IS_ENABLED(CONFIG_SOAK_C2_ENCODE)

/* The Kconfig choice, as the boot default. */
#define PAYLOAD_DEFAULT_MODE \
	(IS_ENABLED(CONFIG_SOAK_PAYLOAD_TONE) ? SOAK_PAYLOAD_TONE : \
	 IS_ENABLED(CONFIG_SOAK_PAYLOAD_CLIP) ? SOAK_PAYLOAD_CLIP : \
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
#ifdef CONFIG_SOAK_C2_ENCODE
	if (mode == SOAK_PAYLOAD_PCM) {
		return c2_enc_pcm_ok();
	}
#endif
	return !(PAYLOAD_CLIP_LINKED && mode == SOAK_PAYLOAD_CLIP) ||
	       clip_src_valid();
}

/* PCM mode: 80 ms chunks in the clip buffer (META). */
static inline uint32_t payload_pcm_chunks(void)
{
#ifdef CONFIG_SOAK_C2_ENCODE
	uint32_t len;

	(void)clip_src_data(&len);
	return len / C2_ENC_CHUNK_PCM_BYTES;
#else
	return 0;
#endif
}

/*
 * PCM mode: the encoder's chunk for this unit's next TX boundary, if it is
 * done (c2_enc_take()). Always false in other builds.
 */
static inline bool payload_take_pcm(uint8_t out[TDMA_PAYLOAD_LEN],
				    uint32_t *enc_us)
{
#ifdef CONFIG_SOAK_C2_ENCODE
	return c2_enc_take(tdma_next_tx_us(), out, enc_us);
#else
	ARG_UNUSED(out);
	ARG_UNUSED(enc_us);
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
	} else {
		for (int i = 0; i < TDMA_PAYLOAD_LEN; i++) {
			out[i] = (uint8_t)(seq + i);
		}
	}
}

#endif /* PAYLOAD_SRC_H_ */
