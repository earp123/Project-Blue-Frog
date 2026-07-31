/*
 * soak_log - binary soak-run logger for the field console.
 *
 * Records what the radio actually did, per packet, fast enough to keep up at
 * the 20 ms production slot. Three decisions drive the design:
 *
 * 1. Fixed 64-byte binary records, not CSV. The payload alone is 40 bytes,
 *    which as hex text would be 80 characters before any telemetry; formatting
 *    it per packet costs far more CPU than a memcpy. 64 B also divides 512
 *    exactly, so eight records fill a FAT sector with no waste and no
 *    partial-sector rewrite. Text formatting happens offline in the decoder
 *    (tools/decode_soak_log.py), where it is free.
 *
 * 2. The producer never touches the card. Records go into a k_msgq ring; a
 *    dedicated low-priority writer thread drains it into sd_log. A card stall
 *    therefore stalls only the writer, never the UI loop, and never the radio
 *    thread (which is cooperative and outranks both regardless).
 *
 * 3. Logging failure is non-fatal. A missing or full card must not abort a
 *    radio test: the ring simply drops, the drop count is surfaced on screen,
 *    and the soak keeps running.
 *
 * Rate budget at the 20 ms slot (80 ms frame, 4 units): 3 RX + 1 TX per frame
 * = 50 records/s = 3.2 KB/s = ~6 sector writes/s. Comfortably inside what the
 * card demonstrated, with the ring absorbing the sync stalls.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef SOAK_LOG_H_
#define SOAK_LOG_H_

#include <zephyr/kernel.h>
#include <zephyr/toolchain.h>
#include <stdbool.h>
#include <stdint.h>

#include "tdma.h"
#include "sd_log.h"

/* On-disk format version; bump on any struct soak_rec change. */
#define SOAK_LOG_VERSION	1
#define SOAK_LOG_MAGIC		0x4B414F53UL	/* "SOAK", little-endian */

/* Eight records per 512-byte sector. Keep this exact. */
#define SOAK_REC_SIZE		64

/*
 * Ring depth in records. 128 * 64 B = 8 KB, about 2.5 s of headroom at the
 * 20 ms-slot worst case — sized to swallow a card sync stall whole, since
 * anything the ring drops is data the run can never recover.
 */
#define SOAK_LOG_RING_DEPTH	128

enum soak_rec_type {
	SOAK_REC_META = 1,	/* run parameters; always record 0 */
	SOAK_REC_RX = 2,	/* one received packet */
	SOAK_REC_TX = 3,	/* one submitted transmit */
	SOAK_REC_STATS = 4,	/* periodic engine counter snapshot */
};

/* flags bits */
#define SOAK_F_NO_CTR	BIT(0)	/* frame_ctr not meaningful (TX records) */

/*
 * The one on-disk record. Every field is naturally aligned at its offset, so
 * __packed changes nothing about layout here — it only pins the size against
 * accidental padding on a compiler change.
 */
struct soak_rec {
	uint8_t  type;		/* enum soak_rec_type */
	uint8_t  slot_id;
	uint8_t  sync_state;	/* enum tdma_sync_state */
	uint8_t  flags;
	uint32_t t_us;		/* slot-clock time: RX = DIO1 edge, TX = submit */
	uint16_t frame_ctr;
	int16_t  rssi;		/* dBm */
	int8_t   snr;		/* dB */
	uint8_t  rsvd[3];
	uint32_t seq;		/* monotonic; a gap means the ring dropped */
	uint32_t uptime_ms;	/* wall-clock correlation between the two units */
	uint8_t  payload[TDMA_PAYLOAD_LEN];	/* 40 B: RX/TX data, or packed
						 * counters for META/STATS */
} __packed;

BUILD_ASSERT(sizeof(struct soak_rec) == SOAK_REC_SIZE,
	     "soak_rec must stay 64 B so 8 pack into a 512 B sector");
BUILD_ASSERT(SD_LOG_BLOCK % SOAK_REC_SIZE == 0,
	     "record size must divide the FAT sector exactly");

/* META payload overlay (little-endian, written into soak_rec.payload). */
struct soak_meta {
	uint32_t magic;
	uint16_t version;
	uint8_t  role;		/* enum tdma_role */
	uint8_t  slot_id;
	uint32_t freq_hz;
	uint32_t slot_us;
	uint32_t frame_us;
	uint32_t toa_us;
	uint8_t  sf;
	uint8_t  cr;		/* 4/(4+cr) */
	uint16_t bw_khz;
	int8_t   tx_power_dbm;
	uint8_t  slot_count;
	uint8_t  payload_len;
	uint8_t  preamble_syms;
	uint32_t uptime_ms;
} __packed;

struct soak_log_status {
	bool active;
	uint32_t queued;	/* records handed to the ring */
	uint32_t written;	/* records the writer pushed to sd_log */
	uint32_t dropped;	/* ring-full drops (producer side) */
	uint32_t bytes;
	int err;		/* last writer error, 0 if healthy */
	const char *path;
};

/*
 * Open a session file and queue the META record. Returns 0, or a negative
 * errno if the card is unusable — the caller should carry on with the soak
 * regardless and simply show the failure.
 */
int soak_log_start(enum tdma_role role, uint8_t slot_id, int8_t tx_power_dbm);

/* Producers. All are non-blocking and safe to call whether or not a log is
 * open; they drop (and count) rather than wait.
 */
void soak_log_rx(const struct tdma_rx_msg *msg, uint8_t sync_state);
void soak_log_tx(const uint8_t payload[TDMA_PAYLOAD_LEN], uint8_t slot_id,
		 uint8_t sync_state);
void soak_log_stats(const struct tdma_telemetry *t, uint32_t rx_ok,
		    uint32_t missed, uint32_t dup);

/* Drain the ring, flush and close. Blocks briefly for the writer to finish. */
int soak_log_stop(void);

void soak_log_get_status(struct soak_log_status *out);

#endif /* SOAK_LOG_H_ */
