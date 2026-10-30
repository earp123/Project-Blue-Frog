/*
 * opus_dec - Opus decoding on the device. See opus_dec.h.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/util.h>
#include <zephyr/timing/timing.h>
#include <nrfx_clock.h>
#include <errno.h>
#include <string.h>

#include <opus.h>

#include "clip_src.h"
#include "opus_dec.h"
#ifdef CONFIG_SOAK_AUDIO_HAT
#include "hat_audio.h"
#endif

BUILD_ASSERT(CLIP_CODEC == CLIP_CODEC_OPUS, "Opus clips need 80 B payloads");

/* libopus's scratch (VAR_ARRAYS) lands here; stack_unused is the check. */
#define DEC_STACK	16384
#define DEC_PRIO	K_PRIO_PREEMPT(1)	/* as c2_dec: above the UI */

#define FRAMES		4
#define FRAME_SAMPLES	160
#define CHUNK_SAMPLES	(FRAMES * FRAME_SAMPLES)
#define FRAME_BYTES	(CLIP_CHUNK / FRAMES)		/* 18 */
#define GAP_RESET	25	/* chunks (2 s): an outage, not a loss burst */
#define CHUNK_US	80000	/* a peer's chunks arrive one frame apart */
/*
 * A chunk not here this long after the previous one's arrival plus a frame
 * is lost: concealed now, in its own place in the playout. Received chunks
 * reach this thread within ~2 ms (the data thread's tick), and the playout
 * writes a chunk 40 ms (PLAY_TARGET) ahead of playing it, so the PLC must
 * land well inside those 40 ms. A chunk that arrives after all is "late".
 */
#define OVERDUE_US	20000
/* One decoder state; opus_dec_start() checks opus_decoder_get_size(). */
#define STATE_BYTES	18432	/* 17800 measured, 1.5.2 fixed point */

struct dec_item {
	uint8_t slot_id;
	uint8_t payload[TDMA_PAYLOAD_LEN];
};

/* A few frames of slack: three peers' chunks arrive within one 80 ms frame. */
K_MSGQ_DEFINE(opus_dec_q, sizeof(struct dec_item), 8, 4);

struct peer {
	OpusDecoder *st;
	bool live;		/* last and due are valid */
	uint16_t last;		/* chunk index of the last chunk handled */
	uint32_t run;		/* lost chunks in a row */
	uint32_t arrived;	/* slot clock at the last received chunk */
	uint32_t due;		/* slot clock past which the next one is lost */
};

static uint8_t state_mem[TDMA_SLOT_COUNT][STATE_BYTES] __aligned(8);
static struct peer peer[TDMA_SLOT_COUNT];
static int16_t pcm[CHUNK_SAMPLES];
static uint8_t own_slot;
static atomic_t running;

/* Held by the decoder thread while it decodes, and by start/stop while they
 * change the states, so a run never starts under a half-decoded chunk.
 */
static K_MUTEX_DEFINE(dec_lock);

static struct k_spinlock stats_lock;
static struct opus_dec_stats stats;
static uint64_t dec_sum_us;

extern const k_tid_t opus_dec_tid;
static k_thread_runtime_stats_t cpu0, dec0;

static uint32_t us_since(timing_t t0)
{
	timing_t t1 = timing_counter_get();

	return (uint32_t)(timing_cycles_to_ns(timing_cycles_get(&t0, &t1)) /
			  1000U);
}

/* One chunk's four frames, or (frames == NULL) four frames of PLC, into pcm.
 * Returns the time it took, us, or a negative opus error.
 */
static int decode(OpusDecoder *st, const uint8_t *frames)
{
	timing_t t0 = timing_counter_get();

	for (int f = 0; f < FRAMES; f++) {
		int n = opus_decode(st, frames ? &frames[f * FRAME_BYTES] : NULL,
				    frames ? FRAME_BYTES : 0,
				    &pcm[f * FRAME_SAMPLES], FRAME_SAMPLES, 0);

		if (n != FRAME_SAMPLES) {
			return n < 0 ? n : -EIO;
		}
	}
	return (int)us_since(t0);
}

static void play(uint8_t slot, uint16_t idx)
{
#ifdef CONFIG_SOAK_AUDIO_HAT
	hat_audio_play_chunk(slot, idx, pcm, CHUNK_SAMPLES);
#else
	ARG_UNUSED(slot);
	ARG_UNUSED(idx);
#endif
}

static void count(uint32_t *field)
{
	k_spinlock_key_t key = k_spin_lock(&stats_lock);

	(*field)++;
	k_spin_unlock(&stats_lock, key);
}

/* One lost chunk, concealed (decision 3): PLC played for the first
 * OPUS_DEC_PLC_MAX in a row, then silence; the decoder runs either way so
 * its state follows real time. Past GAP_RESET it is an outage: stop.
 */
static void conceal(struct peer *pr, uint8_t s, bool on_time)
{
	int us = decode(pr->st, NULL);
	bool audible = on_time && pr->run < OPUS_DEC_PLC_MAX;

	pr->run++;
	pr->last++;
	if (us < 0) {
		count(&stats.errors);
	} else {
		k_spinlock_key_t key = k_spin_lock(&stats_lock);

		stats.plc_max_us = MAX(stats.plc_max_us, (uint32_t)us);
		if (audible) {
			stats.plc_played++;
		} else {
			stats.plc_silent++;
		}
		k_spin_unlock(&stats_lock, key);
		if (audible) {
			play(s, pr->last);
		}
	}
	if (pr->run >= GAP_RESET) {
		pr->live = false;
		count(&stats.resets);
	}
}

/* Conceal every chunk now overdue, each in its own place. */
static void conceal_overdue(void)
{
	uint32_t now = tdma_now_us();

	for (uint8_t s = 0; s < TDMA_SLOT_COUNT; s++) {
		struct peer *pr = &peer[s];

		while (pr->live && (int32_t)(now - pr->due) >= 0) {
			conceal(pr, s, true);
			pr->due += CHUNK_US;
		}
	}
}

static void handle(const struct dec_item *it)
{
	const uint8_t *p = it->payload;
	uint8_t s = it->slot_id % TDMA_SLOT_COUNT;
	struct peer *pr = &peer[s];
	uint32_t now = tdma_now_us();

	/* The clip test header; anything else is not Opus audio. */
	if (p[0] != CLIP_MAGIC || p[1] != CLIP_CODEC_OPUS || pr->st == NULL) {
		count(&stats.bad);
		return;
	}

	uint16_t idx = sys_get_le16(&p[2]);

	if (pr->live) {
		uint16_t d = (uint16_t)(idx - pr->last);

		if (d == 0 || d >= 0x8000) {
			/* Already concealed (it came after OVERDUE_US), or a
			 * repeat: the playout has moved past it.
			 */
			count(&stats.late);
			return;
		}
		/* How late a chunk can come right after its predecessor:
		 * the margin OVERDUE_US has to cover.
		 */
		uint32_t late = now - pr->arrived - CHUNK_US;

		if (d == 1 && pr->run == 0 && (int32_t)late > 0) {
			k_spinlock_key_t key = k_spin_lock(&stats_lock);

			stats.late_max_us = MAX(stats.late_max_us, late);
			k_spin_unlock(&stats_lock, key);
		}
		/* Lost chunks the timer did not reach (none, normally): the
		 * decoder state only; the playout bridges them with silence.
		 */
		for (uint16_t k = 1; k < d && pr->live; k++) {
			conceal(pr, s, false);
		}
	}
	if (!pr->live) {
		/* First chunk of the run, or after an outage: a fresh state. */
		(void)opus_decoder_init(pr->st, 8000, 1);
	}

	int us = decode(pr->st, &p[CLIP_HDR_LEN]);

	pr->run = 0;
	pr->last = idx;
	pr->arrived = now;
	pr->due = now + CHUNK_US + OVERDUE_US;
	pr->live = true;
	if (us < 0) {
		count(&stats.errors);
		return;
	}
	play(s, idx);

	k_spinlock_key_t key = k_spin_lock(&stats_lock);

	stats.decoded++;
	dec_sum_us += (uint32_t)us;
	stats.dec_avg_us = (uint32_t)(dec_sum_us / stats.decoded);
	stats.dec_max_us = MAX(stats.dec_max_us, (uint32_t)us);
	k_spin_unlock(&stats_lock, key);
}

static void dec_thread_fn(void *p1, void *p2, void *p3)
{
	struct dec_item it;
	uint32_t n = 0;

	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	timing_init();
	timing_start();

	for (;;) {
		/* Woken by a chunk, or every 5 ms to conceal overdue ones. */
		bool got = k_msgq_get(&opus_dec_q, &it, K_MSEC(5)) == 0;

		k_mutex_lock(&dec_lock, K_FOREVER);
		if (atomic_get(&running)) {
			if (got) {
				handle(&it);
			}
			conceal_overdue();
		}
		k_mutex_unlock(&dec_lock);

		if ((++n % 64U) == 0U) {
			size_t unused;

			if (k_thread_stack_space_get(k_current_get(),
						     &unused) == 0) {
				stats.stack_unused = (uint32_t)unused;
			}
		}
	}
}

K_THREAD_DEFINE(opus_dec_tid, DEC_STACK, dec_thread_fn, NULL, NULL, NULL,
		DEC_PRIO, 0, 0);

int opus_dec_start(uint8_t own)
{
	int size = opus_decoder_get_size(1);

	opus_dec_stop();
	k_mutex_lock(&dec_lock, K_FOREVER);
	k_msgq_purge(&opus_dec_q);

	k_spinlock_key_t key = k_spin_lock(&stats_lock);

	memset(&stats, 0, sizeof(stats));
	stats.state_bytes = (uint32_t)size;
	dec_sum_us = 0;
	k_spin_unlock(&stats_lock, key);

	if (size <= 0 || size > STATE_BYTES) {
		k_mutex_unlock(&dec_lock);
		return -ENOMEM;
	}
	own_slot = own;
	for (int i = 0; i < TDMA_SLOT_COUNT; i++) {
		peer[i] = (struct peer){ 0 };
		if (i == own_slot) {
			continue;
		}
		peer[i].st = (OpusDecoder *)state_mem[i];
		if (opus_decoder_init(peer[i].st, 8000, 1) != OPUS_OK) {
			k_mutex_unlock(&dec_lock);
			return -EINVAL;
		}
	}
#ifdef CONFIG_SOAK_AUDIO_HAT
	hat_audio_play_reset();
#endif
	(void)k_thread_runtime_stats_all_get(&cpu0);
	(void)k_thread_runtime_stats_get(opus_dec_tid, &dec0);
	atomic_set(&running, 1);
	k_mutex_unlock(&dec_lock);
	return 0;
}

void opus_dec_stop(void)
{
	k_mutex_lock(&dec_lock, K_FOREVER);
	atomic_set(&running, 0);
	k_mutex_unlock(&dec_lock);
}

void opus_dec_submit(uint8_t slot_id, const uint8_t payload[TDMA_PAYLOAD_LEN])
{
	struct dec_item it = { .slot_id = slot_id };

	if (!atomic_get(&running)) {
		return;
	}
	memcpy(it.payload, payload, TDMA_PAYLOAD_LEN);
	if (k_msgq_put(&opus_dec_q, &it, K_NO_WAIT) != 0) {
		count(&stats.dropped);
	}
}

void opus_dec_get_stats(struct opus_dec_stats *out)
{
	k_thread_runtime_stats_t cpu, t;

	(void)k_thread_runtime_stats_all_get(&cpu);
	(void)k_thread_runtime_stats_get(opus_dec_tid, &t);

	uint64_t span = cpu.execution_cycles - cpu0.execution_cycles;
	k_spinlock_key_t key = k_spin_lock(&stats_lock);

	*out = stats;
	k_spin_unlock(&stats_lock, key);
	out->cpu = span ? (uint32_t)(((t.execution_cycles -
				       dec0.execution_cycles) * 1000U) / span)
			: 0;
}

#ifndef CONFIG_SOAK_C2_ENCODE
/*
 * The application core at 128 MHz, as c2_enc.c sets it for the Codec 2
 * builds: it resets to 64 MHz. The divider sets the CPU clock only; the
 * slot clock and SPI run from the 16 MHz peripheral clock, so TDMA timing
 * is unchanged.
 */
static int cpu_128mhz(void)
{
	return nrfx_clock_divider_set(NRF_CLOCK_DOMAIN_HFCLK,
				      NRF_CLOCK_HFCLK_DIV_1) == NRFX_SUCCESS
		       ? 0 : -EIO;
}

SYS_INIT(cpu_128mhz, PRE_KERNEL_1, 0);
#endif
