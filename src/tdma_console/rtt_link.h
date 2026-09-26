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
 *   down 1 "ctl"   1 KB    text commands from tools/rtt_link.py, plus clip
 *                          bytes after a "clip" line
 *
 * Both up channels are NO_BLOCK_SKIP: a record goes in whole or not at all,
 * so a slow or absent host can never corrupt the stream. It drops, and the
 * drop is counted (soak_log_status.rtt_dropped). Channel 0 is left alone.
 *
 * Commands, one "\n"-terminated line each, one reply line each:
 *
 *   status                  ok status role= slot= sync= soak= mode=
 *                           clip=<chunks>/<crc32> lead= rtt_drop=
 *                           pretx=<pickups>/<staged>/<min>/<last margin us>
 *   mode ramp|tone|clip|pcm payload source for the next soak start (pcm
 *                           only with CONFIG_SOAK_C2_ENCODE)
 *   clip <nbytes> <crc32>   followed by exactly nbytes raw bytes: load the
 *                           clip buffer (clip_src.h); refused while a soak
 *                           runs, or if the size or CRC is wrong
 *   phase <us>              PCM mode: each 80 ms chunk of the simulated
 *                           mic becomes available this long before its TX
 *                           boundary, from the next soak (c2_enc.h)
 *   lead <us>               stage each payload this long before the unit's
 *                           TX boundary, from the next soak (0 = at once,
 *                           right after TxDone; the default)
 *   soak <minutes> [sd]     start a soak (0 = continuous); sd adds the card
 *   stop                    stop the running soak
 *
 * Threads. The soak_log writer thread owns the record stream and polls the
 * command channel on its 10 ms idle tick; mode and clip are handled there.
 * A command that touches the runner (status, soak, stop) is parked in a
 * one-deep slot and carried out by the unit's UI loop through
 * rtt_link_service(), because starting and stopping a soak is the UI loop's
 * job: soak_finish() waits on the writer thread, so it can never run there.
 * Replies go out from whichever thread executes the command. Nothing here is
 * reachable from the radio.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef RTT_LINK_H_
#define RTT_LINK_H_

#include <stdbool.h>
#include <stdint.h>

#include "tdma.h"
#include "soak_log.h"

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

/* Payload source for the next soak: the "mode" command, else the Kconfig
 * default. Runners use payload_next_mode() (payload_src.h).
 */
enum soak_payload_mode rtt_link_mode(void);

/* Staging lead for the next soak, us ("lead" command; 0 = at once). */
uint32_t rtt_link_lead_us(void);

/* PCM availability before the TX boundary for the next soak ("phase"). */
uint32_t rtt_link_phase_us(void);

#endif /* RTT_LINK_H_ */
