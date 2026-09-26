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

/* A source is linked if the bench port can select it, or if it is the one. */
#define PAYLOAD_TONE_LINKED \
	(IS_ENABLED(CONFIG_SOAK_RTT) || IS_ENABLED(CONFIG_SOAK_PAYLOAD_TONE))
#define PAYLOAD_CLIP_LINKED \
	(IS_ENABLED(CONFIG_SOAK_RTT) || IS_ENABLED(CONFIG_SOAK_PAYLOAD_CLIP))

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

/* True if a soak can start in mode (a clip must be loaded). */
static inline bool payload_ready(enum soak_payload_mode mode)
{
	return !(PAYLOAD_CLIP_LINKED && mode == SOAK_PAYLOAD_CLIP) ||
	       clip_src_valid();
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
