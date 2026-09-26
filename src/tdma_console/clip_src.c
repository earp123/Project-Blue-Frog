/*
 * clip_src - Codec 2 clip for the soak payload. See clip_src.h.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/sys/crc.h>
#include <zephyr/sys/byteorder.h>
#include <errno.h>
#include <string.h>

#include "clip_src.h"

BUILD_ASSERT(CLIP_CHUNK == 32, "4 x 8 B Codec 2 3200 frames per payload");

static uint8_t clip_buf[CONFIG_SOAK_CLIP_BUF_KB * 1024];

/* Loaded clip; published by clip_valid, which the loader clears first. */
static uint32_t clip_chunks;
static uint32_t clip_crc;
static atomic_t clip_valid;

/* Upload in progress (writer thread only). */
static uint32_t load_len;
static uint32_t load_got;
static uint32_t load_crc;

void clip_src_fill(uint8_t out[TDMA_PAYLOAD_LEN], uint32_t n)
{
	uint32_t off = (n % clip_chunks) * CLIP_CHUNK;

	out[0] = CLIP_MAGIC;
	out[1] = CLIP_CODEC_3200;
	sys_put_le16((uint16_t)n, &out[2]);
	sys_put_le16((uint16_t)clip_crc, &out[4]);
	out[6] = 0;
	out[7] = 0;
	memcpy(&out[CLIP_HDR_LEN], &clip_buf[off], CLIP_CHUNK);
}

bool clip_src_valid(void)
{
	return atomic_get(&clip_valid) != 0;
}

uint32_t clip_src_chunks(void)
{
	return clip_src_valid() ? clip_chunks : 0;
}

uint32_t clip_src_crc(void)
{
	return clip_src_valid() ? clip_crc : 0;
}

const uint8_t *clip_src_data(uint32_t *len)
{
	if (!clip_src_valid()) {
		*len = 0;
		return NULL;
	}
	*len = clip_chunks * CLIP_CHUNK;
	return clip_buf;
}

int clip_src_load_begin(uint32_t nbytes, uint32_t crc)
{
	atomic_set(&clip_valid, 0);
	load_got = 0;
	load_len = 0;

	if (nbytes == 0 || nbytes % CLIP_CHUNK != 0 ||
	    nbytes > sizeof(clip_buf)) {
		return -EINVAL;
	}
	load_len = nbytes;
	load_crc = crc;
	return 0;
}

void clip_src_load_bytes(const uint8_t *data, size_t len)
{
	len = MIN(len, (size_t)(load_len - load_got));
	memcpy(&clip_buf[load_got], data, len);
	load_got += len;
}

int clip_src_load_end(void)
{
	if (load_len == 0 || load_got != load_len ||
	    crc32_ieee(clip_buf, load_len) != load_crc) {
		return -EBADMSG;
	}

	clip_chunks = load_len / CLIP_CHUNK;
	clip_crc = load_crc;
	atomic_set(&clip_valid, 1);
	return 0;
}
