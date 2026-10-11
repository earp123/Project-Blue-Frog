/*
 * mic_dsp - conditioning ahead of Codec 2. See mic_dsp.h.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/* -O2 like the vendored DSP code: the global -Os made the HPF loop slow. */
#pragma GCC optimize("O2")

#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/util.h>
#include <errno.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include <speex/speex_preprocess.h>

#include "mic_dsp.h"
#include "tdma.h"

#define FS		8000
#define SUB		80	/* Speex frame: 10 ms, so 10 ms of added delay */
#define HPF_HZ		150.0f
#define PI_F		3.14159265f

/* Arena for the preprocessor state (os_support_custom.h). The 80-sample
 * state takes 12,840 B ("dsp ... state=" in the status reply).
 */
#define ARENA_BYTES	13312

static uint8_t arena[ARENA_BYTES] __aligned(8);
static size_t arena_used;
static bool arena_failed;

static SpeexPreprocessState *pp;

/* Requested by any thread; applied by the encoder at its next frame. */
static atomic_t req_on = ATOMIC_INIT(1);
static atomic_t req_stages = ATOMIC_INIT(MIC_DSP_ALL);
static atomic_t agc_level = ATOMIC_INIT(MIC_DSP_AGC_LEVEL);
static atomic_t ns_db = ATOMIC_INIT(MIC_DSP_NS_DB);

/* Speex stages as last set in the preprocessor (encoder thread). */
static uint32_t applied = MIC_DSP_ALL;

/* HPF: transposed direct form II biquad (encoder thread). */
static float hb0, hb1, hb2, ha1, ha2;
static float hz1, hz2;

static struct k_spinlock lock;
static struct mic_dsp_stats stats;
static uint64_t us_sum;

void *mic_dsp_speex_alloc(int size)
{
	size_t at = ROUND_UP(arena_used, 8);

	if (size < 0 || at + (size_t)size > sizeof(arena)) {
		arena_failed = true;
		/* Speex does not check: hand it something harmless and fail
		 * the run in mic_dsp_start() before it is ever used.
		 */
		return arena;
	}
	arena_used = at + (size_t)size;
	return &arena[at];
}

void mic_dsp_speex_fatal(const char *str)
{
	ARG_UNUSED(str);
	k_oops();
}

void mic_dsp_set_on(bool on)
{
	atomic_set(&req_on, on ? 1 : 0);
}

bool mic_dsp_get_on(void)
{
	return atomic_get(&req_on) != 0;
}

void mic_dsp_set_stages(uint32_t mask)
{
	atomic_set(&req_stages, (atomic_val_t)(mask & MIC_DSP_ALL));
}

uint32_t mic_dsp_get_stages(void)
{
	return (uint32_t)atomic_get(&req_stages);
}

void mic_dsp_set_agc_level(uint32_t level)
{
	atomic_set(&agc_level, (atomic_val_t)level);
}

void mic_dsp_set_ns_db(uint32_t db)
{
	atomic_set(&ns_db, (atomic_val_t)db);
}

/* 2nd-order Butterworth high-pass (RBJ cookbook, Q = 1/sqrt(2)). */
static void hpf_init(void)
{
	float w0 = 2.0f * PI_F * HPF_HZ / FS;
	float c = cosf(w0);
	float alpha = sinf(w0) / (2.0f * 0.70710678f);
	float a0 = 1.0f + alpha;

	hb0 = (1.0f + c) / 2.0f / a0;
	hb1 = -(1.0f + c) / a0;
	hb2 = hb0;
	ha1 = -2.0f * c / a0;
	ha2 = (1.0f - alpha) / a0;
	hz1 = 0.0f;
	hz2 = 0.0f;
}

static int16_t sat16(float v, uint32_t *clipped)
{
	if (v >= 32767.0f) {
		(*clipped)++;
		return 32767;
	}
	if (v <= -32768.0f) {
		(*clipped)++;
		return -32768;
	}
	/* Round half away from zero; lrintf() is a library call here. */
	return (int16_t)(v >= 0.0f ? v + 0.5f : v - 0.5f);
}

static void hpf_run(int16_t *pcm, size_t n, uint32_t *clipped)
{
	for (size_t i = 0; i < n; i++) {
		float x = pcm[i];
		float y = hb0 * x + hz1;

		hz1 = hb1 * x - ha1 * y + hz2;
		hz2 = hb2 * x - ha2 * y;
		pcm[i] = sat16(y, clipped);
	}
}

static void speex_stages(uint32_t stages)
{
	spx_int32_t v;

	v = (stages & MIC_DSP_NS) ? 1 : 0;
	speex_preprocess_ctl(pp, SPEEX_PREPROCESS_SET_DENOISE, &v);
	v = (stages & MIC_DSP_AGC) ? 1 : 0;
	speex_preprocess_ctl(pp, SPEEX_PREPROCESS_SET_AGC, &v);
	applied = stages;
}

int mic_dsp_start(void)
{
	/* The previous run's state lives in the arena: drop it whole. */
	memset(arena, 0, arena_used);
	arena_used = 0;
	arena_failed = false;
	pp = speex_preprocess_state_init(SUB, FS);
	if (arena_failed) {
		pp = NULL;
		return -ENOMEM;
	}

	spx_int32_t v = -(spx_int32_t)atomic_get(&ns_db);

	speex_preprocess_ctl(pp, SPEEX_PREPROCESS_SET_NOISE_SUPPRESS, &v);

	float level = (float)atomic_get(&agc_level);

	speex_preprocess_ctl(pp, SPEEX_PREPROCESS_SET_AGC_LEVEL, &level);
	v = MIC_DSP_AGC_MAX_GAIN_DB;
	speex_preprocess_ctl(pp, SPEEX_PREPROCESS_SET_AGC_MAX_GAIN, &v);
	speex_stages((uint32_t)atomic_get(&req_stages));
	hpf_init();

	k_spinlock_key_t key = k_spin_lock(&lock);

	memset(&stats, 0, sizeof(stats));
	stats.state_bytes = arena_used;
	stats.arena_bytes = sizeof(arena);
	us_sum = 0;
	k_spin_unlock(&lock, key);
	return 0;
}

/* Telemetry the preprocessor holds; encoder thread, now and then. */
static void probe(int32_t *gain_db, int32_t *noise_db, uint32_t *speech)
{
	static spx_int32_t psd[SUB];
	spx_int32_t v = 0;
	spx_int32_t bins = 0;
	float sum = 0.0f;

	speex_preprocess_ctl(pp, SPEEX_PREPROCESS_GET_AGC_GAIN, &v);
	*gain_db = v;
	speex_preprocess_ctl(pp, SPEEX_PREPROCESS_GET_PROB, &v);
	*speech = (uint32_t)MAX(v, 0);
	speex_preprocess_ctl(pp, SPEEX_PREPROCESS_GET_NOISE_PSD_SIZE, &bins);
	bins = MIN(bins, (spx_int32_t)ARRAY_SIZE(psd));
	speex_preprocess_ctl(pp, SPEEX_PREPROCESS_GET_NOISE_PSD, psd);
	for (int i = 0; i < bins; i++) {
		sum += (float)psd[i];
	}
	*noise_db = bins ? (int32_t)lrintf(10.0f * log10f(1.0f + sum / bins))
			 : 0;
}

bool mic_dsp_frame(int16_t *pcm, size_t n)
{
	if (pp == NULL || !atomic_get(&req_on)) {
		return false;
	}

	uint32_t stages = (uint32_t)atomic_get(&req_stages);

	if (stages == 0) {
		return false;
	}

	uint32_t t0 = tdma_now_us();
	uint32_t clipped = 0;

	if ((stages & (MIC_DSP_NS | MIC_DSP_AGC)) !=
	    (applied & (MIC_DSP_NS | MIC_DSP_AGC))) {
		speex_stages(stages);
	}
	applied = stages;

	if (stages & MIC_DSP_HPF) {
		hpf_run(pcm, n, &clipped);
	}
	if (stages & (MIC_DSP_NS | MIC_DSP_AGC)) {
		for (size_t i = 0; i + SUB <= n; i += SUB) {
			speex_preprocess_run(pp, &pcm[i]);
		}
	}

	int16_t peak = 0;

	for (size_t i = 0; i < n; i++) {
		int16_t a = pcm[i] == INT16_MIN ? INT16_MAX : (int16_t)abs(pcm[i]);

		if (a == INT16_MAX) {
			clipped++;
		}
		peak = MAX(peak, a);
	}

	int32_t gain_db = 0, noise_db = 0;
	uint32_t speech = 0;
	bool probed = (stats.frames % 16U) == 0U &&
		      (stages & (MIC_DSP_NS | MIC_DSP_AGC));

	if (probed) {
		probe(&gain_db, &noise_db, &speech);
	}

	/* Telemetry included: the whole cost to the encoder thread. */
	uint32_t us = tdma_now_us() - t0;

	k_spinlock_key_t key = k_spin_lock(&lock);

	stats.frames++;
	us_sum += us;
	stats.us_avg = (uint32_t)(us_sum / stats.frames);
	stats.us_max = MAX(stats.us_max, us);
	stats.clipped += clipped;
	stats.peak = MAX(stats.peak, peak);
	stats.stages = stages;
	if (probed) {
		stats.agc_gain_db = gain_db;
		stats.noise_db = noise_db;
		stats.speech_pct = speech;
	}
	k_spin_unlock(&lock, key);
	return true;
}

void mic_dsp_get_stats(struct mic_dsp_stats *out)
{
	k_spinlock_key_t key = k_spin_lock(&lock);

	*out = stats;
	out->on = atomic_get(&req_on) != 0;
	stats.peak = 0;
	k_spin_unlock(&lock, key);
}
