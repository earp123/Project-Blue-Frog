/*
 * c2_enc - Codec 2 encoding on the device. See c2_enc.h.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/init.h>
#include <zephyr/sys/byteorder.h>
#include <nrfx_clock.h>
#include <stdlib.h>
#include <zephyr/sys/util.h>
#include <errno.h>
#include <string.h>

#include <codec2.h>

#include "c2_enc.h"
#include "clip_src.h"

/* codec2_encode keeps FFT and pitch buffers on the stack: 16 KB left only
 * 1.7 KB never touched (status enc=.../<unused>), so 20 KB.
 */
#define ENC_STACK	20480
#define ENC_PRIO	K_PRIO_PREEMPT(0)

#define FRAMES_PER_CHUNK	4
#define FRAME_SAMPLES		(C2_ENC_CHUNK_SAMPLES / FRAMES_PER_CHUNK)
#define FRAME_BYTES		8

BUILD_ASSERT(FRAMES_PER_CHUNK * FRAME_BYTES == CLIP_CHUNK,
	     "four Codec 2 3200 frames fill the clip payload");

static K_SEM_DEFINE(enc_go, 0, 1);
static K_SEM_DEFINE(enc_ready, 0, 1);	/* codec created (or not) */
static int create_result;

/* Run parameters and codec, owned by the encoder thread while running. */
static struct CODEC2 *codec;
static uint32_t phase_us;
static atomic_t running;
static atomic_t run_id;		/* bumped by start/stop: a stale wait aborts */
static atomic_t alive;		/* the thread holds the codec for a run */

/* One-deep output slot, published under the lock. */
static struct k_spinlock slot_lock;
static struct {
	bool valid;
	uint32_t boundary;
	uint32_t enc_us;
	uint8_t payload[TDMA_PAYLOAD_LEN];
} slot;

static struct c2_enc_stats stats;

/*
 * Codec 2 built with __EMBEDDED__ allocates through these (debug_alloc.h).
 * The libc heap (CONFIG_COMMON_LIBC_MALLOC_ARENA_SIZE) serves them and the
 * few plain malloc() calls in the codec; only c2_enc_start() and the
 * encoder thread's teardown allocate or free.
 */
void *codec2_malloc(size_t size)
{
	return malloc(size);
}

void *codec2_calloc(size_t nmemb, size_t size)
{
	return calloc(nmemb, size);
}

void codec2_free(void *ptr)
{
	free(ptr);
}

/* Codec 2's frames are 20 ms of int16 PCM; the clip is little-endian bytes. */
static int16_t frame_pcm[FRAME_SAMPLES];

bool c2_enc_pcm_ok(void)
{
	uint32_t len;

	return clip_src_data(&len) != NULL && len >= C2_ENC_CHUNK_PCM_BYTES &&
	       len % C2_ENC_CHUNK_PCM_BYTES == 0;
}

int c2_enc_start(uint32_t phase)
{
	if (!c2_enc_pcm_ok()) {
		return -EINVAL;
	}

	c2_enc_stop();

	k_spinlock_key_t key = k_spin_lock(&slot_lock);

	slot.valid = false;
	memset(&stats, 0, sizeof(stats));
	k_spin_unlock(&slot_lock, key);

	phase_us = phase;
	atomic_inc(&run_id);
	atomic_set(&alive, 1);
	atomic_set(&running, 1);
	k_sem_reset(&enc_ready);
	k_sem_give(&enc_go);

	/*
	 * The encoder thread creates the codec: codec2_create() keeps a 512
	 * point FFT buffer on the stack, more than the UI thread (the caller)
	 * has. Wait for it here so a failure still comes back to the caller.
	 */
	k_sem_take(&enc_ready, K_FOREVER);
	if (create_result < 0) {
		atomic_set(&running, 0);
		atomic_set(&alive, 0);
	}
	return create_result;
}

void c2_enc_stop(void)
{
	if (!atomic_cas(&running, 1, 0)) {
		return;
	}
	atomic_inc(&run_id);
	/* The thread notices at its next wake (a frame at most) and frees
	 * the codec; wait for it so a following start gets a clean slate.
	 */
	while (atomic_get(&alive)) {
		k_sleep(K_MSEC(5));
	}
}

bool c2_enc_take(uint32_t boundary_us, uint8_t out[TDMA_PAYLOAD_LEN],
		 uint32_t *enc_us)
{
	bool got = false;
	k_spinlock_key_t key = k_spin_lock(&slot_lock);

	/* Same boundary within half a slot: a secondary's sync correction
	 * may move it a few us between encode and stage.
	 */
	int32_t d = (int32_t)(slot.boundary - boundary_us);

	if (slot.valid) {
		if (d > -(int32_t)TDMA_SLOT_DURATION_US / 2 &&
		    d < (int32_t)TDMA_SLOT_DURATION_US / 2) {
			memcpy(out, slot.payload, TDMA_PAYLOAD_LEN);
			*enc_us = slot.enc_us;
			slot.valid = false;
			got = true;
		} else if (d < 0) {
			/* Its boundary has gone by: too late to send. */
			slot.valid = false;
			stats.late++;
		}
	}
	k_spin_unlock(&slot_lock, key);
	return got;
}

void c2_enc_get_stats(struct c2_enc_stats *out)
{
	k_spinlock_key_t key = k_spin_lock(&slot_lock);

	*out = stats;
	k_spin_unlock(&slot_lock, key);
}

/* Encode chunk n (the PCM loops) into out: header + four frames. */
static void encode_chunk(uint32_t n, uint8_t out[TDMA_PAYLOAD_LEN])
{
	uint32_t len;
	const uint8_t *pcm = clip_src_data(&len);
	uint32_t chunks = len / C2_ENC_CHUNK_PCM_BYTES;
	const uint8_t *src = pcm + (n % chunks) * C2_ENC_CHUNK_PCM_BYTES;

	/* The clip test header (clip_src.h). clip_id is the PCM's CRC: the
	 * host ties it to its own encoding of the same PCM.
	 */
	out[0] = CLIP_MAGIC;
	out[1] = CLIP_CODEC_3200;
	sys_put_le16((uint16_t)n, &out[2]);
	sys_put_le16((uint16_t)clip_src_crc(), &out[4]);
	out[6] = 0;
	out[7] = 0;

	for (int f = 0; f < FRAMES_PER_CHUNK; f++) {
		for (int i = 0; i < FRAME_SAMPLES; i++) {
			frame_pcm[i] = (int16_t)sys_get_le16(
				&src[(f * FRAME_SAMPLES + i) * 2]);
		}
		codec2_encode(codec, &out[CLIP_HDR_LEN + f * FRAME_BYTES],
			      frame_pcm);
	}
}

static void enc_thread_fn(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	for (;;) {
		k_sem_take(&enc_go, K_FOREVER);

		codec = codec2_create(CODEC2_MODE_3200);
		create_result = (codec != NULL) ? 0 : -ENOMEM;
		k_sem_give(&enc_ready);
		if (codec == NULL) {
			continue;
		}

		atomic_val_t id = atomic_get(&run_id);
		bool anchored = false;
		uint32_t b0 = 0;
		uint32_t last_b = 0;

		while (atomic_get(&running) && atomic_get(&run_id) == id) {
			/* Frame timing means nothing until the engine is
			 * RUNNING (a secondary snaps its phase on lock).
			 */
			if (tdma_get_telemetry()->sync_state !=
			    TDMA_SYNC_RUNNING) {
				k_sleep(K_MSEC(10));
				continue;
			}

			uint32_t b = tdma_next_tx_us();
			uint32_t now = tdma_now_us();

			if (anchored && (int32_t)(b - last_b) <
					(int32_t)TDMA_SLOT_DURATION_US) {
				/* This boundary's chunk is done: sleep past
				 * the boundary, then look at the next one.
				 */
				k_sleep(K_USEC((int32_t)(b - now) + 100));
				continue;
			}

			int32_t wait = (int32_t)(b - phase_us - now);

			if (wait > 0) {
				/* Not captured yet: sleep until it is, then
				 * re-read the boundary (sync may move it).
				 */
				k_sleep(K_USEC(wait));
				continue;
			}
			if (anchored) {
				/* How long after availability we got here. */
				stats.start_late_max_us =
					MAX(stats.start_late_max_us,
					    (uint32_t)-wait);
			}

			if (!anchored) {
				anchored = true;
				b0 = b;
			}
			/* Time-keyed: whole frames since the first boundary,
			 * so a skipped frame skips its chunk.
			 */
			uint32_t n = (uint32_t)(((b - b0) +
						 TDMA_FRAME_DURATION_US / 2) /
						TDMA_FRAME_DURATION_US);
			if (last_b != 0 &&
			    (b - last_b) > TDMA_FRAME_DURATION_US * 3 / 2) {
				stats.skipped += (b - last_b) /
						 TDMA_FRAME_DURATION_US - 1;
			}
			last_b = b;

			uint8_t out[TDMA_PAYLOAD_LEN];
			uint32_t t0 = tdma_now_us();

			encode_chunk(n, out);

			uint32_t enc = tdma_now_us() - t0;
			k_spinlock_key_t key = k_spin_lock(&slot_lock);

			slot.boundary = b;
			slot.enc_us = enc;
			memcpy(slot.payload, out, sizeof(out));
			if (slot.valid) {
				stats.late++;	/* previous one never taken */
			}
			slot.valid = true;
			stats.encoded++;
			stats.enc_last_us = enc;
			stats.enc_max_us = MAX(stats.enc_max_us, enc);
			k_spin_unlock(&slot_lock, key);

			/* Stack headroom (painted stacks, INIT_STACKS): a
			 * scan of the 16 KB, so not every chunk.
			 */
			if ((n % 16U) == 0U) {
				size_t unused;

				if (k_thread_stack_space_get(k_current_get(),
							     &unused) == 0) {
					stats.stack_unused = (uint32_t)unused;
				}
			}
		}

		codec2_destroy(codec);
		codec = NULL;
		atomic_set(&alive, 0);
	}
}

/*
 * Run the application core at 128 MHz. It resets to 64 MHz (HFCLKCTRL
 * Div2), where one 80 ms chunk took ~100 ms to encode: slower than real
 * time. The divider sets the CPU clock only; TIMER2 (the slot clock) and the
 * SPI peripherals run from the 16 MHz peripheral clock, so TDMA timing is
 * unchanged. Encode builds only, so every other build stays as measured.
 */
static int cpu_128mhz(void)
{
	return nrfx_clock_divider_set(NRF_CLOCK_DOMAIN_HFCLK,
				      NRF_CLOCK_HFCLK_DIV_1) == NRFX_SUCCESS
		       ? 0 : -EIO;
}

SYS_INIT(cpu_128mhz, PRE_KERNEL_1, 0);

K_THREAD_DEFINE(c2_enc_tid, ENC_STACK, enc_thread_fn, NULL, NULL, NULL,
		ENC_PRIO, 0, 0);
