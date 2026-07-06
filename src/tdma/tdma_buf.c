/*
 * tdma_buf - frame header pack/unpack and TX staging. See tdma_buf.h.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/spinlock.h>
#include <string.h>
#include <errno.h>

#include "tdma_buf.h"

void tdma_buf_pack_hdr(uint8_t out[TDMA_HDR_LEN], uint8_t slot_id,
		       uint16_t frame_ctr)
{
	out[0] = TDMA_MAGIC_VER;
	out[1] = slot_id;
	sys_put_le16(frame_ctr, &out[2]);
}

int tdma_buf_unpack_hdr(const uint8_t raw[TDMA_ON_AIR_LEN], struct tdma_hdr *hdr)
{
	if (raw[0] != TDMA_MAGIC_VER) {
		return -EINVAL;
	}

	hdr->magic_ver = raw[0];
	hdr->slot_id = raw[1];
	hdr->frame_ctr = sys_get_le16(&raw[2]);

	if (hdr->slot_id >= TDMA_SLOT_COUNT) {
		return -EINVAL;
	}

	return 0;
}

static struct k_spinlock stage_lock;
static uint8_t stage_buf[TDMA_PAYLOAD_LEN];
static bool stage_pending;

int tdma_buf_stage(const uint8_t payload[TDMA_PAYLOAD_LEN])
{
	k_spinlock_key_t key = k_spin_lock(&stage_lock);

	if (stage_pending) {
		k_spin_unlock(&stage_lock, key);
		return -EAGAIN;
	}

	memcpy(stage_buf, payload, TDMA_PAYLOAD_LEN);
	stage_pending = true;
	k_spin_unlock(&stage_lock, key);
	return 0;
}

bool tdma_buf_take_staged(uint8_t out[TDMA_PAYLOAD_LEN])
{
	k_spinlock_key_t key = k_spin_lock(&stage_lock);

	if (!stage_pending) {
		k_spin_unlock(&stage_lock, key);
		return false;
	}

	memcpy(out, stage_buf, TDMA_PAYLOAD_LEN);
	stage_pending = false;
	k_spin_unlock(&stage_lock, key);
	return true;
}
