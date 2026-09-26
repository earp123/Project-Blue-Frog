/*
 * rtt_link - J-Link RTT bench port (CONFIG_SOAK_RTT).
 *
 * Replaces the SD card on the bench. Both unit types already hang off the
 * host by their J-Link USB, and RTT needs no pins and no USB stack: it is a
 * RAM ring the on-board J-Link reads and writes over SWD. Task and design:
 * docs/rtt_link_c2_transport.md.
 *
 *   up 1   "soak"  8 KB    the raw 64 B soak_rec stream, identical to the card
 *   up 2   "ctl"   512 B   one text reply per command line
 *   down 1 "ctl"   1 KB    text commands from tools/rtt_link.py
 *
 * Both up channels are NO_BLOCK_SKIP: a record goes in whole or not at all,
 * so a slow or absent host can never corrupt the stream. It drops, and the
 * drop is counted (soak_log_status.rtt_dropped). Channel 0 is left alone.
 *
 * Threads. The soak_log writer thread owns the record stream and polls the
 * command channel on its 10 ms idle tick. A command that touches the runner
 * (status, soak, stop) is parked in a one-deep slot and carried out by the
 * unit's UI loop through rtt_link_service(), because starting and stopping a
 * soak is the UI loop's job: soak_finish() waits on the writer thread, so it
 * can never run there. Replies go out from whichever thread executes the
 * command. Nothing here is reachable from the radio.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef RTT_LINK_H_
#define RTT_LINK_H_

#include <stdbool.h>
#include <stdint.h>

#include "tdma.h"

/* What the runner reports for "status". */
struct rtt_link_unit {
	bool picked;		/* power-on unit pick done */
	enum tdma_role role;
	uint8_t slot_id;
	bool soak_running;
};

/*
 * The runner's side, called on its UI loop only.
 *
 * soak_start: minutes 0 = continuous; sd also opens the card file. Returns 0,
 *   -EBUSY (a soak is running, or the card is busy and sd was asked for),
 *   -ENODEV (unit not picked yet) or another negative errno if the engine
 *   failed to start.
 * soak_stop: 0, or -EALREADY if no soak is running.
 */
struct rtt_link_ops {
	void (*get_unit)(struct rtt_link_unit *out);
	int (*soak_start)(uint32_t minutes, bool sd);
	int (*soak_stop)(void);
};

/*
 * Push one 64 B record to up 1. Returns false if it did not fit (dropped).
 * soak_log writer thread only.
 */
bool rtt_link_write_rec(const void *rec, uint32_t len);

/* Read and parse pending command bytes. soak_log writer thread only. */
void rtt_link_poll(void);

/* Carry out a parked runner command, if any. The unit's UI loop only. */
void rtt_link_service(const struct rtt_link_ops *ops);

#endif /* RTT_LINK_H_ */
