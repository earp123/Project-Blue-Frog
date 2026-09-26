/*
 * clip_src - Codec 2 clip for the soak payload (payload mode CLIP).
 *
 * A pre-encoded speech clip, uploaded over the RTT bench port (rtt_link.h)
 * into a static RAM buffer and kept until reset. Each 40-byte payload is an
 * 8-byte test header plus 32 clip bytes, four Codec 2 3200 frames = 80 ms:
 *
 *   off len field
 *    0   1  magic      CLIP_MAGIC (0xC2)
 *    1   1  codec      CLIP_CODEC_3200 (0), the only mode this task sends
 *    2   2  chunk_idx  uint16 LE, n mod 65536: per sender, monotonic per run
 *    4   2  clip_id    uint16 LE, low 16 bits of the clip's CRC32
 *    6   2  rsvd       0
 *    8  32  clip bytes [(n mod chunks) * 32, +32)
 *
 * This is a test header, not the product L3 header. The chunk index is what
 * lets the host place, de-duplicate and bit-compare chunks without decoding,
 * and pair a sender's TX records with a receiver's RX records.
 *
 * Same contract as tone_src: stateless and frame-keyed. Chunk n is the
 * payload for the unit's n-th TX slot of the run, so a refused submit retries
 * the same bytes and a missed frame's audio is dropped, never delayed. The
 * clip loops. See docs/rtt_link_c2_transport.md.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef CLIP_SRC_H_
#define CLIP_SRC_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "tdma.h"

#define CLIP_MAGIC		0xC2
#define CLIP_CODEC_3200		0
#define CLIP_HDR_LEN		8
#define CLIP_CHUNK		(TDMA_PAYLOAD_LEN - CLIP_HDR_LEN)	/* 32 */

/* Write chunk n of the loaded clip into out. Only valid while
 * clip_src_valid(); the runner refuses to start a clip soak otherwise.
 */
void clip_src_fill(uint8_t out[TDMA_PAYLOAD_LEN], uint32_t n);

bool clip_src_valid(void);
uint32_t clip_src_chunks(void);	/* 0 unless valid */
uint32_t clip_src_crc(void);	/* 0 unless valid */

/*
 * Loader, for the RTT command channel (soak_log writer thread). begin marks
 * the clip invalid and checks the size (-EINVAL: 0, not a multiple of
 * CLIP_CHUNK, or larger than the buffer); bytes appends; end checks the
 * CRC32 (crc32_ieee) and makes the clip valid, or returns -EBADMSG. Never
 * call these while a soak is running: the TX path reads the buffer unlocked.
 */
int clip_src_load_begin(uint32_t nbytes, uint32_t crc);
void clip_src_load_bytes(const uint8_t *data, size_t len);
int clip_src_load_end(void);

#endif /* CLIP_SRC_H_ */
