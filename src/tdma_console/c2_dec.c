/*
 * c2_dec - Codec 2 decoding on the device. See c2_dec.h.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/util.h>
#include <zephyr/timing/timing.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include <codec2.h>

#include "c2_dec.h"
#include "c2_enc.h"
#ifdef CONFIG_SOAK_C2_PROFILE
#include "c2_prof.h"
#endif
#include "clip_src.h"
#ifdef CONFIG_SOAK_AUDIO_HAT
#include "hat_audio.h"
#endif

/* codec2_decode synthesises through a 512-point inverse FFT on the stack,
 * like the encoder's analysis; same size, checked by stack_unused.
 */
#define DEC_STACK	20480
#define DEC_PRIO	K_PRIO_PREEMPT(1)	/* below the encoder, above UI */

#define FRAMES_PER_CHUNK	4
#define FRAME_SAMPLES		(C2_ENC_CHUNK_SAMPLES / FRAMES_PER_CHUNK)
#define FRAME_BYTES		8
#define BENCH_MAX		256

struct dec_item {
	uint8_t slot_id;
	uint8_t payload[TDMA_PAYLOAD_LEN];
};

/* A few frames of slack: three peers' chunks arrive within one 80 ms frame. */
K_MSGQ_DEFINE(dec_q, sizeof(struct dec_item), 8, 4);

enum { REQ_NONE, REQ_RUN, REQ_BENCH };

static K_SEM_DEFINE(dec_go, 0, 1);
static K_SEM_DEFINE(dec_done, 0, 1);	/* states created / bench finished */
static int req;
static int req_result;
static uint8_t own_slot;
static uint32_t n_extra;
static uint32_t bench_chunks;
static struct c2_bench *bench_out;

static atomic_t running;
static atomic_t run_id;
static atomic_t alive;

static struct CODEC2 *peer[TDMA_SLOT_COUNT];
static struct CODEC2 *extra_st[C2_DEC_EXTRA_MAX];

static int16_t dec_pcm[C2_ENC_CHUNK_SAMPLES];
static int16_t mix_pcm[C2_ENC_CHUNK_SAMPLES];

static struct k_spinlock stats_lock;
static struct c2_dec_stats stats;
static uint64_t dec_sum_us;

/* CPU accounting baseline, taken at run start. */
extern const k_tid_t c2_enc_tid;
extern const k_tid_t c2_dec_tid;
extern const k_tid_t tdma_radio_tid;
extern const k_tid_t soak_data_tid;

static k_thread_runtime_stats_t cpu0, enc0, dec0, radio0, data0;

static void cpu_snapshot(void)
{
	(void)k_thread_runtime_stats_all_get(&cpu0);
	(void)k_thread_runtime_stats_get(c2_enc_tid, &enc0);
	(void)k_thread_runtime_stats_get(c2_dec_tid, &dec0);
	(void)k_thread_runtime_stats_get(tdma_radio_tid, &radio0);
	(void)k_thread_runtime_stats_get(soak_data_tid, &data0);
}

static uint32_t permille(uint64_t part, uint64_t whole)
{
	return whole ? (uint32_t)((part * 1000U) / whole) : 0;
}

void c2_dec_get_cpu(struct c2_cpu *out)
{
	k_thread_runtime_stats_t cpu, t;
	uint64_t span;

	(void)k_thread_runtime_stats_all_get(&cpu);
	span = cpu.execution_cycles - cpu0.execution_cycles;
	out->busy = permille(cpu.total_cycles - cpu0.total_cycles, span);
	(void)k_thread_runtime_stats_get(c2_enc_tid, &t);
	out->enc = permille(t.total_cycles - enc0.total_cycles, span);
	(void)k_thread_runtime_stats_get(c2_dec_tid, &t);
	out->dec = permille(t.total_cycles - dec0.total_cycles, span);
	(void)k_thread_runtime_stats_get(tdma_radio_tid, &t);
	out->radio = permille(t.total_cycles - radio0.total_cycles, span);
	(void)k_thread_runtime_stats_get(soak_data_tid, &t);
	out->data = permille(t.total_cycles - data0.total_cycles, span);
}

/* Largest block malloc can hand out now: a binary search of allocations. */
static uint32_t heap_largest(void)
{
	uint32_t lo = 0;
	uint32_t hi = CONFIG_COMMON_LIBC_MALLOC_ARENA_SIZE;

	while (hi - lo > 32U) {
		uint32_t mid = lo + (hi - lo) / 2U;
		void *p = malloc(mid);

		if (p != NULL) {
			free(p);
			lo = mid;
		} else {
			hi = mid;
		}
	}
	return lo;
}

static uint32_t us_since(timing_t t0)
{
	timing_t t1 = timing_counter_get();

	return (uint32_t)(timing_cycles_to_ns(timing_cycles_get(&t0, &t1)) /
			  1000U);
}

/* Decode one chunk's four frames with st into dec_pcm. */
static void decode_chunk(struct CODEC2 *st, const uint8_t *frames)
{
	for (int f = 0; f < FRAMES_PER_CHUNK; f++) {
		codec2_decode(st, &dec_pcm[f * FRAME_SAMPLES],
			      &frames[f * FRAME_BYTES]);
	}
}

/* Sum dec_pcm into the mix with saturation, as playout will. */
static void mix_chunk(void)
{
	for (int i = 0; i < C2_ENC_CHUNK_SAMPLES; i++) {
		int32_t v = (int32_t)mix_pcm[i] + dec_pcm[i];

		mix_pcm[i] = (int16_t)CLAMP(v, INT16_MIN, INT16_MAX);
	}
}

static void free_states(void)
{
	for (int i = 0; i < TDMA_SLOT_COUNT; i++) {
		if (peer[i] != NULL) {
			codec2_destroy(peer[i]);
			peer[i] = NULL;
		}
	}
	for (int i = 0; i < C2_DEC_EXTRA_MAX; i++) {
		if (extra_st[i] != NULL) {
			codec2_destroy(extra_st[i]);
			extra_st[i] = NULL;
		}
	}
}

static int create_states(void)
{
	for (int i = 0; i < TDMA_SLOT_COUNT; i++) {
		if (i == own_slot) {
			continue;
		}
		peer[i] = codec2_create(CODEC2_MODE_3200);
		if (peer[i] == NULL) {
			return -ENOMEM;
		}
	}
	for (uint32_t i = 0; i < n_extra; i++) {
		extra_st[i] = codec2_create(CODEC2_MODE_3200);
		if (extra_st[i] == NULL) {
			return -ENOMEM;
		}
	}
	return 0;
}

static uint8_t lowest_peer(void)
{
	return (own_slot == 0) ? 1 : 0;
}

static void handle(const struct dec_item *it)
{
	const uint8_t *p = it->payload;
	uint8_t s = it->slot_id % TDMA_SLOT_COUNT;

	/* The clip test header; anything else (card #23's garbled first
	 * packet) is not audio.
	 */
	if (p[0] != CLIP_MAGIC || p[1] != CLIP_CODEC_3200 || peer[s] == NULL) {
		k_spinlock_key_t key = k_spin_lock(&stats_lock);

		stats.bad++;
		k_spin_unlock(&stats_lock, key);
		return;
	}

	int n = 1 + ((s == lowest_peer()) ? (int)n_extra : 0);

	for (int i = 0; i < n; i++) {
		struct CODEC2 *st = (i == 0) ? peer[s] : extra_st[i - 1];
		timing_t t0 = timing_counter_get();

		decode_chunk(st, &p[CLIP_HDR_LEN]);

		uint32_t dec = us_since(t0);
		timing_t t1 = timing_counter_get();

#ifdef CONFIG_SOAK_AUDIO_HAT
		/* A real peer goes to the earpieces; a phantom stream is only
		 * the cost of a fourth unit.
		 */
		if (i == 0) {
			hat_audio_play_chunk(s, sys_get_le16(&p[2]), dec_pcm,
					     C2_ENC_CHUNK_SAMPLES);
		} else {
			mix_chunk();
		}
#else
		mix_chunk();
#endif

		uint32_t mix = us_since(t1);
		k_spinlock_key_t key = k_spin_lock(&stats_lock);

		if (i == 0) {
			stats.decoded++;
		} else {
			stats.extra++;
		}
		dec_sum_us += dec;
		stats.dec_avg_us = (uint32_t)(dec_sum_us /
					      (stats.decoded + stats.extra));
		stats.dec_max_us = MAX(stats.dec_max_us, dec);
		stats.mix_max_us = MAX(stats.mix_max_us, mix);
		k_spin_unlock(&stats_lock, key);
	}

	/* A fresh mix buffer per frame's worth is playout's business; for
	 * the cost measurement, just keep it from saturating forever.
	 */
	if ((stats.decoded % 8U) == 0U) {
		memset(mix_pcm, 0, sizeof(mix_pcm));
	}
}

static void run(atomic_val_t id)
{
	struct dec_item it;
	uint32_t count = 0;

	while (atomic_get(&running) && atomic_get(&run_id) == id) {
		if (k_msgq_get(&dec_q, &it, K_MSEC(50)) != 0) {
			continue;
		}
		handle(&it);

		if ((++count % 64U) == 0U) {
			size_t unused;

			if (k_thread_stack_space_get(k_current_get(),
						     &unused) == 0) {
				stats.stack_unused = (uint32_t)unused;
			}
		}
	}
}

/* ---- Isolated benchmark ------------------------------------------------ */

static uint32_t enc_us[BENCH_MAX];
static uint32_t dec_us[BENCH_MAX];

static void sort_u32(uint32_t *a, uint32_t n)
{
	for (uint32_t i = 1; i < n; i++) {
		uint32_t k = a[i];
		uint32_t j = i;

		while (j > 0 && a[j - 1] > k) {
			a[j] = a[j - 1];
			j--;
		}
		a[j] = k;
	}
}

static void summarise(uint32_t *a, uint32_t n, uint32_t *mn, uint32_t *avg,
		      uint32_t *p99, uint32_t *mx)
{
	uint64_t sum = 0;

	sort_u32(a, n);
	for (uint32_t i = 0; i < n; i++) {
		sum += a[i];
	}
	*mn = a[0];
	*mx = a[n - 1];
	*avg = (uint32_t)(sum / n);
	*p99 = a[(n * 99U) / 100U];
}

static uint8_t bench_frames[BENCH_MAX][FRAMES_PER_CHUNK * FRAME_BYTES];

/*
 * Two passes, encode every chunk then decode every chunk, so that with
 * CONFIG_SOAK_C2_PROFILE each function's time lands in the right phase.
 */
static int bench(uint32_t chunks, struct c2_bench *out)
{
	uint32_t len;
	const uint8_t *pcm = clip_src_data(&len);
	struct CODEC2 *enc, *dec;
	int16_t in[FRAME_SAMPLES];
	uint32_t mix_max = 0;

	if (!c2_enc_pcm_ok()) {
		return -EINVAL;
	}
	chunks = CLAMP(chunks, 1U, (uint32_t)BENCH_MAX);

	enc = codec2_create(CODEC2_MODE_3200);

	uint32_t before = heap_largest();

	dec = codec2_create(CODEC2_MODE_3200);

	uint32_t after = heap_largest();

	if (enc == NULL || dec == NULL) {
		if (enc != NULL) {
			codec2_destroy(enc);
		}
		if (dec != NULL) {
			codec2_destroy(dec);
		}
		return -ENOMEM;
	}

	uint32_t nchunks = len / C2_ENC_CHUNK_PCM_BYTES;

#ifdef CONFIG_SOAK_C2_PROFILE
	c2_prof_reset();
	c2_prof_phase(C2_PROF_ENC);
#endif
	for (uint32_t c = 0; c < chunks; c++) {
		const uint8_t *src = pcm + (c % nchunks) *
					   C2_ENC_CHUNK_PCM_BYTES;
		timing_t t0 = timing_counter_get();

		for (int f = 0; f < FRAMES_PER_CHUNK; f++) {
			for (int i = 0; i < FRAME_SAMPLES; i++) {
				in[i] = (int16_t)sys_get_le16(
					&src[(f * FRAME_SAMPLES + i) * 2]);
			}
			codec2_encode(enc, &bench_frames[c][f * FRAME_BYTES],
				      in);
		}
		enc_us[c] = us_since(t0);
	}

#ifdef CONFIG_SOAK_C2_PROFILE
	c2_prof_phase(C2_PROF_DEC);
#endif
	for (uint32_t c = 0; c < chunks; c++) {
		timing_t t1 = timing_counter_get();

		decode_chunk(dec, bench_frames[c]);
		dec_us[c] = us_since(t1);

		timing_t t2 = timing_counter_get();

		mix_chunk();
		mix_max = MAX(mix_max, us_since(t2));
	}

	codec2_destroy(dec);
	codec2_destroy(enc);

	out->n = chunks;
	summarise(enc_us, chunks, &out->enc_min, &out->enc_avg, &out->enc_p99,
		  &out->enc_max);
	summarise(dec_us, chunks, &out->dec_min, &out->dec_avg, &out->dec_p99,
		  &out->dec_max);
	out->mix_max = mix_max;
	out->heap_per_state = before - after;
	return 0;
}

/* ---- Control ------------------------------------------------------------ */

int c2_dec_start(uint8_t own, uint32_t extra)
{
#ifdef CONFIG_SOAK_AUDIO_HAT
	hat_audio_play_reset();
#endif
	c2_dec_stop();

	k_spinlock_key_t key = k_spin_lock(&stats_lock);

	memset(&stats, 0, sizeof(stats));
	dec_sum_us = 0;
	k_spin_unlock(&stats_lock, key);
	k_msgq_purge(&dec_q);
	memset(mix_pcm, 0, sizeof(mix_pcm));

	own_slot = own;
	n_extra = MIN(extra, (uint32_t)C2_DEC_EXTRA_MAX);
	req = REQ_RUN;
	atomic_inc(&run_id);
	atomic_set(&alive, 1);
	atomic_set(&running, 1);
	k_sem_reset(&dec_done);
	k_sem_give(&dec_go);

	/* States are created on the decoder thread (its stack); wait. */
	k_sem_take(&dec_done, K_FOREVER);
	if (req_result < 0) {
		atomic_set(&running, 0);
		atomic_set(&alive, 0);
	}
	return req_result;
}

void c2_dec_stop(void)
{
	if (!atomic_cas(&running, 1, 0)) {
		return;
	}
	atomic_inc(&run_id);
	while (atomic_get(&alive)) {
		k_sleep(K_MSEC(5));
	}
}

void c2_dec_submit(uint8_t slot_id, const uint8_t payload[TDMA_PAYLOAD_LEN])
{
	struct dec_item it = { .slot_id = slot_id };

	if (!atomic_get(&running)) {
		return;
	}
	memcpy(it.payload, payload, TDMA_PAYLOAD_LEN);
	if (k_msgq_put(&dec_q, &it, K_NO_WAIT) != 0) {
		k_spinlock_key_t key = k_spin_lock(&stats_lock);

		stats.dropped++;
		k_spin_unlock(&stats_lock, key);
	}
}

void c2_dec_get_stats(struct c2_dec_stats *out)
{
	k_spinlock_key_t key = k_spin_lock(&stats_lock);

	*out = stats;
	k_spin_unlock(&stats_lock, key);
}

int c2_dec_bench(uint32_t chunks, struct c2_bench *out)
{
	if (atomic_get(&running)) {
		return -EBUSY;
	}
	bench_chunks = chunks;
	bench_out = out;
	req = REQ_BENCH;
	k_sem_reset(&dec_done);
	k_sem_give(&dec_go);
	k_sem_take(&dec_done, K_FOREVER);
	return req_result;
}

static void dec_thread_fn(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	timing_init();
	timing_start();

	for (;;) {
		k_sem_take(&dec_go, K_FOREVER);

		if (req == REQ_BENCH) {
			req_result = bench(bench_chunks, bench_out);
			k_sem_give(&dec_done);
			continue;
		}

		req_result = create_states();
		if (req_result == 0) {
			stats.heap_left = heap_largest();
			cpu_snapshot();
		} else {
			free_states();
		}
		k_sem_give(&dec_done);
		if (req_result < 0) {
			continue;
		}

		run(atomic_get(&run_id));

		free_states();
		atomic_set(&alive, 0);
	}
}

K_THREAD_DEFINE(c2_dec_tid, DEC_STACK, dec_thread_fn, NULL, NULL, NULL,
		DEC_PRIO, 0, 0);
