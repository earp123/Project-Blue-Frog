/*
 * c2_enc - Codec 2 encoding on the device. See c2_enc.h.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/init.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/util.h>
#include <nrfx_clock.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include <codec2.h>

#include "c2_enc.h"
#include "clip_src.h"
#ifdef CONFIG_SOAK_AUDIO_HAT
#include "hat_audio.h"
#include "rtt_link.h"
#endif

/* codec2_encode keeps FFT and pitch buffers on the stack: 16 KB left only
 * 1.7 KB never touched (status enc=.../<unused>), so 20 KB.
 */
#define ENC_STACK	20480
#define ENC_PRIO	K_PRIO_PREEMPT(0)

#define FRAMES_PER_CHUNK	4
#define FRAME_SAMPLES		(C2_ENC_CHUNK_SAMPLES / FRAMES_PER_CHUNK)
#define FRAME_BYTES		8
#define FRAME_US		20000	/* one Codec 2 3200 frame of audio */

BUILD_ASSERT(FRAMES_PER_CHUNK * FRAME_BYTES == CLIP_CHUNK,
	     "four Codec 2 3200 frames fill the clip payload");
BUILD_ASSERT(FRAMES_PER_CHUNK * FRAME_US == TDMA_FRAME_DURATION_US,
	     "a chunk is one TDMA frame of audio");

/* The engine's pickup deadline, less its measured wake (stage_lead_sweep). */
#define PICKUP_DEADLINE_US	(TDMA_PRE_TX_PICKUP_US - 50)

static K_SEM_DEFINE(enc_go, 0, 1);
static K_SEM_DEFINE(enc_ready, 0, 1);	/* codec created (or not) */
static int create_result;

/* Run parameters and codec, owned by the encoder thread while running. */
static struct CODEC2 *codec;
static uint32_t phase_us;
static bool from_mic;		/* live HAT frames, not the PCM clip */
static atomic_t running;
static atomic_t run_id;		/* bumped by start/stop: a stale wait aborts */
static atomic_t alive;		/* the thread holds the codec for a run */

/* Last staged chunk, for the data thread's TX record. */
static struct k_spinlock rec_lock;
static struct c2_enc_tx rec;
static bool rec_new;

static struct c2_enc_stats stats;

/*
 * Codec 2 built with __EMBEDDED__ allocates through these (debug_alloc.h).
 * The libc heap (CONFIG_COMMON_LIBC_MALLOC_ARENA_SIZE) serves them and the
 * few plain malloc() calls in the codec; only codec creation at a run's start
 * and teardown at its end allocate or free.
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

int c2_enc_start(uint32_t phase, bool mic)
{
	if (mic) {
#ifdef CONFIG_SOAK_AUDIO_HAT
		if (!hat_audio_ok()) {
			return -ENODEV;
		}
#else
		return -ENODEV;
#endif
	} else if (!c2_enc_pcm_ok()) {
		return -EINVAL;
	}

	c2_enc_stop();

	k_spinlock_key_t key = k_spin_lock(&rec_lock);

	rec_new = false;
	memset(&stats, 0, sizeof(stats));
	k_spin_unlock(&rec_lock, key);

	phase_us = phase;
	from_mic = mic;
#ifdef CONFIG_SOAK_AUDIO_HAT
	if (mic) {
		hat_audio_capture_reset();
	}
#endif
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

bool c2_enc_staged(struct c2_enc_tx *out)
{
	bool got = false;
	k_spinlock_key_t key = k_spin_lock(&rec_lock);

	if (rec_new) {
		*out = rec;
		rec_new = false;
		got = true;
	}
	k_spin_unlock(&rec_lock, key);
	return got;
}

void c2_enc_get_stats(struct c2_enc_stats *out)
{
	k_spinlock_key_t key = k_spin_lock(&rec_lock);

	*out = stats;
	k_spin_unlock(&rec_lock, key);
}

/*
 * Encode frame k of chunk n into out: from the PCM clip (it loops), or the
 * next 20 ms of live audio, taken now that the frame is due.
 */
static void encode_frame(uint32_t n, int k, uint8_t *out)
{
#ifdef CONFIG_SOAK_AUDIO_HAT
	if (from_mic) {
		hat_audio_frame(frame_pcm, FRAME_SAMPLES);
		if (rtt_link_raw_on()) {
			/* What the encoder is fed, for comparison. */
			rtt_link_raw_frame(n, k, frame_pcm, FRAME_SAMPLES);
		}
		codec2_encode(codec, out, frame_pcm);
		return;
	}
#endif
	uint32_t len;
	const uint8_t *pcm = clip_src_data(&len);
	uint32_t chunks = len / C2_ENC_CHUNK_PCM_BYTES;
	const uint8_t *src = pcm + (n % chunks) * C2_ENC_CHUNK_PCM_BYTES +
			     k * FRAME_SAMPLES * 2;

	for (int i = 0; i < FRAME_SAMPLES; i++) {
		frame_pcm[i] = (int16_t)sys_get_le16(&src[i * 2]);
	}
	codec2_encode(codec, out, frame_pcm);
}

/* The clip test header (clip_src.h). clip_id is the PCM's CRC: the host ties
 * it to its own encoding of the same PCM.
 */
static void put_header(uint32_t n, uint8_t out[TDMA_PAYLOAD_LEN])
{
	out[0] = CLIP_MAGIC;
	out[1] = CLIP_CODEC_3200;
	sys_put_le16((uint16_t)n, &out[2]);
	/* Live audio has no clip to name: clip_id 0. */
	sys_put_le16(from_mic ? 0 : (uint16_t)clip_src_crc(), &out[4]);
	out[6] = 0;
	out[7] = 0;
}

/* Sleep until slot-clock time t; returns how far past t we woke. */
static uint32_t sleep_until(uint32_t t)
{
	int32_t wait = (int32_t)(t - tdma_now_us());

	if (wait > 0) {
		k_sleep(K_USEC(wait));
	}

	int32_t late = (int32_t)(tdma_now_us() - t);

	return late > 0 ? (uint32_t)late : 0;
}

/*
 * Follow the engine's boundary: the chunk after b is due one frame later,
 * give or take a secondary's sync corrections, which tdma_next_tx_us()
 * already includes.
 */
static uint32_t next_boundary(uint32_t b)
{
	uint32_t want = b + TDMA_FRAME_DURATION_US;
	uint32_t ref = tdma_next_tx_us();

	for (int i = 0; i < 2; i++, ref += TDMA_FRAME_DURATION_US) {
		int32_t d = (int32_t)(ref - want);

		if (d > -(int32_t)TDMA_SLOT_DURATION_US &&
		    d < (int32_t)TDMA_SLOT_DURATION_US) {
			return ref;
		}
	}
	return want;
}

static void run(atomic_val_t id)
{
	bool anchored = false;
	uint32_t b = 0;
	uint32_t b0 = 0;
	uint8_t out[TDMA_PAYLOAD_LEN];

	while (atomic_get(&running) && atomic_get(&run_id) == id) {
		/* Frame timing means nothing until the engine is RUNNING (a
		 * secondary snaps its phase on lock).
		 */
		if (tdma_get_telemetry()->sync_state != TDMA_SYNC_RUNNING) {
			k_sleep(K_MSEC(10));
			anchored = false;
			continue;
		}

		if (!anchored) {
			/* The first boundary whose first frame is still to
			 * be captured.
			 */
			uint32_t now = tdma_now_us();

			b = tdma_next_tx_us();
			while ((int32_t)(b - phase_us -
					 (FRAMES_PER_CHUNK - 1) * FRAME_US -
					 now) < 0) {
				b += TDMA_FRAME_DURATION_US;
			}
			b0 = b;
			anchored = true;
		}

		/* Time-keyed: whole frames since the first boundary. */
		uint32_t n = (uint32_t)(((b - b0) +
					 TDMA_FRAME_DURATION_US / 2) /
					TDMA_FRAME_DURATION_US);
		uint32_t captured = 0;

		put_header(n, out);
		for (int k = 0; k < FRAMES_PER_CHUNK; k++) {
			/* Frame k is complete (FRAMES_PER_CHUNK - 1 - k)
			 * frames before the chunk is.
			 */
			captured = b - phase_us -
				   (FRAMES_PER_CHUNK - 1 - k) * FRAME_US;

			uint32_t late = sleep_until(captured);

			if (atomic_get(&run_id) != id) {
				return;
			}
			stats.start_late_max_us =
				MAX(stats.start_late_max_us, late);

			uint32_t t0 = tdma_now_us();

			encode_frame(n, k, &out[CLIP_HDR_LEN + k * FRAME_BYTES]);
			stats.frame_enc_max_us = MAX(stats.frame_enc_max_us,
						     tdma_now_us() - t0);
		}

		/* The chunk is complete and encoded: straight to the engine. */
		uint32_t done = tdma_get_telemetry()->tx_done;
		uint32_t stage = tdma_now_us();
		int rc = tdma_tx_submit(out);
		int32_t lead = (int32_t)(b - stage);
		k_spinlock_key_t key = k_spin_lock(&rec_lock);

		if (rc == 0) {
			if (rec_new) {
				stats.unlogged++;
			}
			memcpy(rec.payload, out, sizeof(out));
			rec.stage_us = stage;
			rec.lead_us = lead > 0 ? (uint32_t)lead : 0;
			rec.ready_us = stage - captured;
			rec.tx_done = done;
			rec_new = true;
			stats.staged++;
			if (lead < PICKUP_DEADLINE_US) {
				stats.late++;
			}
			stats.ready_max_us = MAX(stats.ready_max_us,
						 stage - captured);
		} else {
			stats.busy++;
		}
		k_spin_unlock(&rec_lock, key);

		/* Stack headroom (painted stacks, INIT_STACKS): a scan of the
		 * whole stack, so not every chunk.
		 */
		if ((n % 16U) == 0U) {
			size_t unused;

			if (k_thread_stack_space_get(k_current_get(),
						     &unused) == 0) {
				stats.stack_unused = (uint32_t)unused;
			}
		}

		b = next_boundary(b);
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

		run(atomic_get(&run_id));

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
