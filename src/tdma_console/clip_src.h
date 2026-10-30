/*
 * clip_src - pre-encoded speech clip for the soak payload (payload mode CLIP).
 *
 * A pre-encoded speech clip, uploaded over the RTT bench port (rtt_link.h)
 * into a static RAM buffer and kept until reset. Each payload is an 8-byte
 * test header plus CLIP_CHUNK clip bytes, four 20 ms frames = 80 ms: Codec 2
 * 3200 (4 x 8 B) in a 40-byte payload, Opus SILK at 7.2 kbit/s (4 x 18 B,
 * docs/opus_payload.md) in an 80-byte one. The chunk size picks the codec;
 * the clip itself is opaque bytes, encoded on the host.
 *
 *   off len field
 *    0   1  magic      CLIP_MAGIC (0xC2)
 *    1   1  codec      CLIP_CODEC: 0 = Codec 2 3200, 1 = Opus
 *    2   2  chunk_idx  uint16 LE, n mod 65536: per sender, monotonic per run
 *    4   2  clip_id    uint16 LE, low 16 bits of the clip's CRC32
 *    6   2  rsvd       0
 *    8   C  clip bytes [(n mod chunks) * C, +C), C = CLIP_CHUNK (32 or 72)
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
#define CLIP_CODEC_OPUS		1
#define CLIP_HDR_LEN		8
#define CLIP_CHUNK		(TDMA_PAYLOAD_LEN - CLIP_HDR_LEN)	/* 32 or 72 */
#define CLIP_CODEC		(CLIP_CHUNK == 32 ? CLIP_CODEC_3200 : CLIP_CODEC_OPUS)

/* Write chunk n of the loaded clip into out. Only valid while
 * clip_src_valid(); the runner refuses to start a clip soak otherwise.
 */
void clip_src_fill(uint8_t out[TDMA_PAYLOAD_LEN], uint32_t n);

bool clip_src_valid(void);
uint32_t clip_src_chunks(void);	/* 0 unless valid */
uint32_t clip_src_crc(void);	/* 0 unless valid */

/* The loaded clip's raw bytes and length; NULL / 0 unless valid. The PCM
 * mode reads it as 16-bit samples (c2_enc.h).
 */
const uint8_t *clip_src_data(uint32_t *len);

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
