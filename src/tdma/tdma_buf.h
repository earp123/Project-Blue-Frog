/*
 * tdma_buf - frame header pack/unpack and the TX staging buffer.
 *
 * On-air layout (first TDMA_HDR_LEN of the 44 bytes):
 *   0: uint8  magic_ver  - 0xA1
 *   1: uint8  slot_id    - transmitter's slot 0-3
 *   2: uint16 frame_ctr  - little-endian; master increments,
 *                          secondaries echo last-heard
 *   4..43: opaque payload (40 B) - L3's business
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef APP_TDMA_BUF_H_
#define APP_TDMA_BUF_H_

#include <stdint.h>
#include <stdbool.h>

#include "tdma.h"

#define TDMA_MAGIC_VER 0xA1

struct tdma_hdr {
	uint8_t magic_ver;
	uint8_t slot_id;
	uint16_t frame_ctr;
};

void tdma_buf_pack_hdr(uint8_t out[TDMA_HDR_LEN], uint8_t slot_id,
		       uint16_t frame_ctr);

/* Returns 0 and fills *hdr, or -EINVAL on magic/version mismatch. */
int tdma_buf_unpack_hdr(const uint8_t raw[TDMA_ON_AIR_LEN], struct tdma_hdr *hdr);

/*
 * TX staging: the app (any thread) stages one payload; the radio thread
 * takes it during RX-slot slack and copies it into the chip's TX region.
 */
int tdma_buf_stage(const uint8_t payload[TDMA_PAYLOAD_LEN]); /* -EAGAIN if pending */
bool tdma_buf_take_staged(uint8_t out[TDMA_PAYLOAD_LEN]);

#endif /* APP_TDMA_BUF_H_ */
