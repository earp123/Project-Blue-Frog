/*
 * hat_audio - the WM8960 Audio HAT as the unit's microphone. See hat_audio.h.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/device.h>
#include <zephyr/drivers/i2s.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/util.h>
#include <stdlib.h>
#include <string.h>

#include "hat_audio.h"
#include "wm8960.h"

#define FS		8000
#define BLOCK_FRAMES	40				/* 5 ms */
/* 24-bit I2S words, each in a 32-bit memory word (right-aligned, sign
 * extended): the codec's 48 BCLKs per frame hold exactly two (G1).
 */
#define BLOCK_BYTES	(BLOCK_FRAMES * 2 * sizeof(int32_t))
#define NUM_BLOCKS	16				/* 80 ms of slack */

#define RING_FRAMES	2048				/* 256 ms, power of 2 */
#define FRAME_MAX	320

/*
 * Cursor window, in samples beyond the n a frame takes. Anchored at TARGET,
 * the lag moves by up to a block either way from one 20 ms frame to the next
 * (blocks land in 5 ms steps), so keep a block of margin below it and room
 * above it for drift: at -5 ppm, 1 sample per 25 s, (LAG_MAX - TARGET -
 * BLOCK) samples last over half an hour.
 */
#define LAG_TARGET	(BLOCK_FRAMES + 20)
#define LAG_MAX		(LAG_TARGET + BLOCK_FRAMES + 80)

/* Cooperative, below the radio (4) and soak data (6) threads, above every
 * preemptible thread, the encoder included: a block is a few microseconds
 * of work and must not wait behind a 3 ms encode.
 */
#define AUDIO_PRIO	K_PRIO_COOP(7)
#define AUDIO_STACK	1536

K_MEM_SLAB_DEFINE_STATIC(i2s_slab, BLOCK_BYTES, NUM_BLOCKS, 4);
static const struct device *const i2s = DEVICE_DT_GET(DT_NODELABEL(i2s0));

/* Stereo capture ring: one writer (the audio thread), one reader (the
 * encoder). wpos counts frames written, published after the data.
 */
static int16_t ring[RING_FRAMES][2];
static atomic_t wpos;

/* Reader state, the encoder thread's alone (reset flag aside). */
static uint32_t rpos;
static atomic_t anchored;
static atomic_t mix = HAT_MIX_AVG;

static struct {
	struct k_spinlock lock;
	bool codec_ok;
	uint32_t blocks;
	int64_t t0_ticks, t_ticks;
	uint32_t restarts, timeouts;
	uint32_t slip_short, slip_drop, frames;
	int16_t peak[2];
} st;

bool hat_audio_ok(void)
{
	return st.codec_ok && st.blocks > 0;
}

void hat_audio_set_mix(enum hat_mix m)
{
	atomic_set(&mix, m);
}

enum hat_mix hat_audio_get_mix(void)
{
	return (enum hat_mix)atomic_get(&mix);
}

void hat_audio_capture_reset(void)
{
	atomic_set(&anchored, 0);
	st.slip_short = 0;
	st.slip_drop = 0;
	st.frames = 0;
}

void hat_audio_frame(int16_t *pcm, size_t n)
{
	n = MIN(n, (size_t)FRAME_MAX);
	if (!hat_audio_ok()) {
		memset(pcm, 0, n * sizeof(*pcm));
		return;
	}

	uint32_t w = (uint32_t)atomic_get(&wpos);

	if (!atomic_get(&anchored)) {
		rpos = w - n - LAG_TARGET;
		atomic_set(&anchored, 1);
	}

	uint32_t lag = w - rpos;	/* samples available */

	if ((int32_t)lag < (int32_t)n) {
		st.slip_short++;
		rpos = w - n - LAG_TARGET;
	} else if (lag > n + LAG_MAX) {
		st.slip_drop++;
		rpos = w - n - LAG_TARGET;
	}

	enum hat_mix m = (enum hat_mix)atomic_get(&mix);

	for (size_t i = 0; i < n; i++) {
		const int16_t *f = ring[(rpos + i) & (RING_FRAMES - 1)];

		pcm[i] = m == HAT_MIX_LEFT ? f[0] :
			 m == HAT_MIX_RIGHT ? f[1] :
			 (int16_t)(((int32_t)f[0] + f[1]) / 2);
	}
	rpos += n;
	st.frames++;
}

void hat_audio_get_stats(struct hat_audio_stats *out)
{
	k_spinlock_key_t key = k_spin_lock(&st.lock);
	int64_t dt = st.t_ticks - st.t0_ticks;

	out->codec_ok = st.codec_ok;
	out->i2c_retries = wm8960_nacks;
	out->blocks = st.blocks;
	out->rate_mhz = (dt > 0 && st.blocks > 1)
		? (uint32_t)((uint64_t)(st.blocks - 1) * BLOCK_FRAMES *
			     CONFIG_SYS_CLOCK_TICKS_PER_SEC * 1000U / dt)
		: 0;
	out->restarts = st.restarts;
	out->timeouts = st.timeouts;
	out->slip_short = st.slip_short;
	out->slip_drop = st.slip_drop;
	out->frames = st.frames;
	out->peak[0] = st.peak[0];
	out->peak[1] = st.peak[1];
	st.peak[0] = st.peak[1] = 0;
	k_spin_unlock(&st.lock, key);
}

static int i2s_start(void)
{
	struct i2s_config cfg = {
		.word_size = 24,
		.channels = 2,
		.format = I2S_FMT_DATA_FORMAT_I2S,
		.options = I2S_OPT_BIT_CLK_SLAVE | I2S_OPT_FRAME_CLK_SLAVE,
		.frame_clk_freq = FS,
		.mem_slab = &i2s_slab,
		.block_size = BLOCK_BYTES,
		.timeout = 500,
	};
	int err = i2s_configure(i2s, I2S_DIR_BOTH, &cfg);

	if (err) {
		return err;
	}
	/* Silence queued ahead of the start (playout is G3). */
	for (int i = 0; i < 2; i++) {
		void *b;

		if (k_mem_slab_alloc(&i2s_slab, &b, K_FOREVER) == 0) {
			memset(b, 0, BLOCK_BYTES);
			i2s_write(i2s, b, BLOCK_BYTES);
		}
	}
	return i2s_trigger(i2s, I2S_DIR_BOTH, I2S_TRIGGER_START);
}

static void restart(void)
{
	st.restarts++;
	i2s_trigger(i2s, I2S_DIR_BOTH, I2S_TRIGGER_DROP);
	k_msleep(10);
	i2s_start();
}

static void on_block(const uint32_t *words)
{
	uint32_t w = (uint32_t)atomic_get(&wpos);
	int16_t pk0 = 0, pk1 = 0;

	for (int i = 0; i < BLOCK_FRAMES; i++) {
		/* The 24-bit sample's top 16 bits. */
		int16_t l = (int16_t)(words[2 * i] >> 8);
		int16_t r = (int16_t)(words[2 * i + 1] >> 8);
		int16_t *f = ring[(w + i) & (RING_FRAMES - 1)];

		f[0] = l;
		f[1] = r;
		pk0 = MAX(pk0, l == INT16_MIN ? INT16_MAX : abs(l));
		pk1 = MAX(pk1, r == INT16_MIN ? INT16_MAX : abs(r));
	}
	atomic_set(&wpos, (atomic_val_t)(w + BLOCK_FRAMES));

	k_spinlock_key_t key = k_spin_lock(&st.lock);
	int64_t now = k_uptime_ticks();

	if (st.blocks == 0) {
		st.t0_ticks = now;
	}
	st.t_ticks = now;
	st.blocks++;
	st.peak[0] = MAX(st.peak[0], pk0);
	st.peak[1] = MAX(st.peak[1], pk1);
	k_spin_unlock(&st.lock, key);
}

static void audio_thread(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	if (!device_is_ready(i2s)) {
		return;
	}
	/* From a cold power-up the codec may not answer yet at 300 ms (seen
	 * on the bench; a warm reset always worked): retry for ~5 s.
	 */
	for (int i = 0; wm8960_init() != 0; i++) {
		if (i == 10) {
			return;	/* no HAT: hat_audio_ok() stays false */
		}
		k_msleep(500);
	}
	st.codec_ok = true;
	if (i2s_start() != 0) {
		return;
	}

	for (;;) {
		void *rx, *tx;
		size_t sz;
		int err = i2s_read(i2s, &rx, &sz);

		if (err == -EAGAIN) {
			st.timeouts++;
			continue;
		} else if (err) {
			restart();
			continue;
		}
		on_block(rx);
		k_mem_slab_free(&i2s_slab, rx);

		/* Keep the DAC fed: silence until G3's playout. */
		if (k_mem_slab_alloc(&i2s_slab, &tx, K_NO_WAIT) == 0) {
			memset(tx, 0, BLOCK_BYTES);
			if (i2s_write(i2s, tx, BLOCK_BYTES) != 0) {
				k_mem_slab_free(&i2s_slab, tx);
				restart();
			}
		}
	}
}

/* Started 300 ms after boot: the HAT's codec needs its 5 V settled. */
K_THREAD_DEFINE(hat_audio_tid, AUDIO_STACK, audio_thread, NULL, NULL, NULL,
		AUDIO_PRIO, 0, 300);
