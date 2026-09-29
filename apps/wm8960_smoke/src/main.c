/*
 * WM8960 HAT smoke test: wiring check and a first listen, one DK.
 *
 * The codec is the I2S master (8 kHz, 16-bit stereo); the nRF's i2s0 is the
 * slave and runs both directions in 10 ms blocks. One audio thread reads a
 * captured block, writes one output block, and keeps the numbers.
 *
 * Modes (shell "wm <mode>" on the VCOM at 115200, or the DK buttons):
 *   stop   silence                                   button 4
 *   tone   1 kHz sine, -12 dBFS, both ears           button 1
 *   play   speech clip, looping, both ears          button 2 (cycles F, M
 *          filtered, then F, M raw)
 *   loop   mic -> headphones (left mic, right mic,   button 3 (cycles
 *          or each mic to its own ear)                          L, R, LR)
 * Plus "wm rec" (8 s of both mics into RAM, for tools/wm_smoke.py to pull
 * over J-Link), "wm stat", "wm vol", "wm gain", "wm reg".
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/i2s.h>
#include <zephyr/kernel.h>
#include <zephyr/shell/shell.h>
#include <zephyr/sys/printk.h>

#include "wm8960.h"

#define FS		8000
#define BLOCK_FRAMES	80				/* 10 ms */
/* 24-bit I2S words: the codec's BCLK is SYSCLK / 32 = 48 clocks per frame,
 * and the nRF slave must see exactly its sample width per channel (16-bit
 * words misframe: a tone came back as a clipped square at the wrong pitch).
 * In memory each sample is a 32-bit word, the 24 bits right-aligned; the app
 * works in 16-bit and converts at the buffers. */
#define BLOCK_BYTES	(BLOCK_FRAMES * 2 * sizeof(int32_t))
#define NUM_BLOCKS	16
#define REC_FRAMES	(8 * FS)			/* 8 s */

enum mode { M_STOP, M_TONE, M_PLAY, M_LOOP };
enum loop_src { LOOP_L, LOOP_R, LOOP_LR };

/* The .inc files are the raw clip bytes (16-bit little-endian PCM). */
static const uint8_t clip_f_bytes[] __aligned(4) = {
#include "clip_f.inc"
};
static const uint8_t clip_m_bytes[] __aligned(4) = {
#include "clip_m.inc"
};
/* The same two band-limited to 300-3400 Hz and peak-normalised to -1 dBFS
 * (tools/speech_filter.py), for small speakers. */
static const uint8_t clip_f_bp_bytes[] __aligned(4) = {
#include "clip_f_bp.inc"
};
static const uint8_t clip_m_bp_bytes[] __aligned(4) = {
#include "clip_m_bp.inc"
};

#define CLIP(b, name) { (const int16_t *)(b), sizeof(b) / 2, name }
static const struct clip {
	const int16_t *pcm;
	size_t len;
	const char *name;
} clips[] = {
	CLIP(clip_f_bp_bytes, "F filtered (OSR_0010_F_bp)"),
	CLIP(clip_m_bp_bytes, "M filtered (OSR_0030_M_bp)"),
	CLIP(clip_f_bytes, "F raw (OSR_0010_F)"),
	CLIP(clip_m_bytes, "M raw (OSR_0030_M)"),
};

/* Recording: interleaved L, R. Read by the host over J-Link. */
int16_t wm_rec_buf[REC_FRAMES * 2];
volatile uint32_t wm_rec_frames;	/* frames recorded so far */
static volatile bool rec_on;

static volatile enum mode mode = M_STOP;
static volatile uint8_t clip_sel;	/* index into clips[] */
static volatile enum loop_src loop_src = LOOP_L;
static volatile uint32_t tone_hz = 1000;
static volatile float tone_amp = 8192.0f;	/* -12 dBFS */
/* Bring-up knobs: where a 16-bit sample sits in the 32-bit I2S word, out
 * and in ("wm shift <tx> <rx>"). */
static volatile uint8_t tx_shift = 8, rx_shift = 8;
/* Bring-up: DC offset added to the tone ("wm tone <hz> <dBFS> <dc>"). */
static volatile int16_t tone_dc;

K_MEM_SLAB_DEFINE_STATIC(i2s_slab, BLOCK_BYTES, NUM_BLOCKS, 4);
static const struct device *const i2s = DEVICE_DT_GET(DT_NODELABEL(i2s0));

static struct {
	uint64_t frames;		/* since the first block */
	int64_t t0_ticks;		/* uptime at the first block */
	int64_t t_ticks;		/* uptime at the latest block */
	uint32_t restarts;
	uint32_t timeouts;
	int16_t peak[2];		/* this stat window, per mic */
	uint64_t sq[2];
	uint32_t win_frames;
	bool codec_ok;
} st;

static const char *const mode_name[] = { "stop", "tone", "play", "loop" };
static const char *const loop_name[] = { "left mic", "right mic",
					 "each mic to its ear" };

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
		printk("i2s_configure: %d\n", err);
		return err;
	}
	/* Two blocks of silence queued before start: ~20 ms of output lead. */
	for (int i = 0; i < 2; i++) {
		void *b;

		k_mem_slab_alloc(&i2s_slab, &b, K_FOREVER);
		memset(b, 0, BLOCK_BYTES);
		i2s_write(i2s, b, BLOCK_BYTES);
	}
	err = i2s_trigger(i2s, I2S_DIR_BOTH, I2S_TRIGGER_START);
	if (err) {
		printk("i2s start: %d\n", err);
	}
	return err;
}

static void i2s_restart(void)
{
	st.restarts++;
	i2s_trigger(i2s, I2S_DIR_BOTH, I2S_TRIGGER_DROP);
	k_msleep(10);
	i2s_start();
}

static void fill_out(int16_t *out, const int16_t *in)
{
	static uint32_t phase;		/* tone, 32-bit phase accumulator */
	static uint32_t clip_pos;
	static uint8_t last_sel;
	enum mode m = mode;

	for (int i = 0; i < BLOCK_FRAMES; i++) {
		int16_t l = 0, r = 0;

		switch (m) {
		case M_TONE:
			l = r = tone_dc + (int16_t)(tone_amp *
				sinf(6.2831853f * (float)phase / 4294967296.0f));
			phase += (uint32_t)(((uint64_t)tone_hz << 32) / FS);
			break;
		case M_PLAY: {
			const struct clip *c = &clips[clip_sel];

			if (clip_sel != last_sel) {
				last_sel = clip_sel;
				clip_pos = 0;
			}
			l = r = c->pcm[clip_pos];
			clip_pos = (clip_pos + 1) % c->len;
			break;
		}
		case M_LOOP:
			if (loop_src == LOOP_L) {
				l = r = in[2 * i];
			} else if (loop_src == LOOP_R) {
				l = r = in[2 * i + 1];
			} else {
				l = in[2 * i];
				r = in[2 * i + 1];
			}
			break;
		default:
			break;
		}
		out[2 * i] = l;
		out[2 * i + 1] = r;
	}
	if (m != M_PLAY) {
		clip_pos = 0;
	}
}

/*
 * The codec takes each DAC word one bit clock early: the MSB it reads is the
 * last bit the nRF sent in the previous slot, and its remaining 23 bits are
 * the first 23 of this slot. Unpacked, every zero crossing becomes a
 * full-scale step (a negative sample reads as near +FS): speech turns into
 * a loud square of its sign. Measured on the bench, see docs.
 *
 * So each 24-bit word carries its sample shifted up one bit, and its bit 0
 * carries the MSB (sign) of the NEXT sample. To know the next sample at the
 * end of a block, the stream is delayed by one frame (two words, keeping L
 * and R in their slots): 125 us. "wm fix 0" turns this off.
 */
static volatile bool tx_fix = true;

static void pack_tx(void *tx, const int16_t *out)
{
	static int16_t hist[2];		/* last frame of the previous block */
	uint32_t *w = tx;
	const int n = BLOCK_FRAMES * 2;

	if (!tx_fix) {
		for (int i = 0; i < n; i++) {
			w[i] = (uint32_t)(int32_t)out[i] << tx_shift;
		}
		return;
	}
	for (int j = 0; j < n; j++) {
		int16_t e = j < 2 ? hist[j] : out[j - 2];
		int16_t next = j + 1 < 2 ? hist[j + 1] : out[j - 1];
		uint32_t v = (uint32_t)(int32_t)e << 8;		/* 24-bit */

		w[j] = ((v << 1) & 0xFFFFFE) | (next < 0 ? 1 : 0);
	}
	hist[0] = out[n - 2];
	hist[1] = out[n - 1];
}

static void audio_thread(void *a, void *b, void *c)
{
	bool said_no_clock = false;

	if (i2s_start()) {
		return;
	}
	for (;;) {
		void *rx, *tx;
		size_t sz;
		int err = i2s_read(i2s, &rx, &sz);

		if (err == -EAGAIN) {
			st.timeouts++;
			if (!said_no_clock) {
				printk("no I2S data in 500 ms: check BCLK P0.04 and "
				       "LRCLK P0.05 (the codec drives them)\n");
				said_no_clock = true;
			}
			continue;
		} else if (err) {
			printk("i2s_read: %d, restarting\n", err);
			i2s_restart();
			continue;
		}
		if (said_no_clock) {
			printk("I2S running\n");
			said_no_clock = false;
		}

		/* 24-bit words to 16-bit samples: bits 23..8. */
		static int16_t in[BLOCK_FRAMES * 2];
		static int16_t out[BLOCK_FRAMES * 2];

		for (int i = 0; i < BLOCK_FRAMES * 2; i++) {
			in[i] = (int16_t)(((const uint32_t *)rx)[i] >> rx_shift);
		}
		int64_t now = k_uptime_ticks();

		if (st.frames == 0) {
			st.t0_ticks = now;
		}
		st.frames += BLOCK_FRAMES;
		st.t_ticks = now;
		for (int i = 0; i < BLOCK_FRAMES; i++) {
			for (int ch = 0; ch < 2; ch++) {
				int16_t s = in[2 * i + ch];
				int16_t a = (s == INT16_MIN) ? INT16_MAX : abs(s);

				if (a > st.peak[ch]) {
					st.peak[ch] = a;
				}
				st.sq[ch] += (int32_t)s * s;
			}
		}
		st.win_frames += BLOCK_FRAMES;

		if (rec_on) {
			uint32_t n = MIN(BLOCK_FRAMES, REC_FRAMES - wm_rec_frames);

			memcpy(&wm_rec_buf[2 * wm_rec_frames], in, n * 4);
			wm_rec_frames += n;
			if (wm_rec_frames >= REC_FRAMES) {
				rec_on = false;
				printk("rec done: %u frames\n", wm_rec_frames);
			}
		}

		if (k_mem_slab_alloc(&i2s_slab, &tx, K_NO_WAIT) == 0) {
			fill_out(out, in);
			pack_tx(tx, out);
			k_mem_slab_free(&i2s_slab, rx);
			err = i2s_write(i2s, tx, BLOCK_BYTES);
			if (err) {
				k_mem_slab_free(&i2s_slab, tx);
				printk("i2s_write: %d, restarting\n", err);
				i2s_restart();
			}
		} else {
			k_mem_slab_free(&i2s_slab, rx);
		}
	}
}

K_THREAD_DEFINE(audio_tid, 2048, audio_thread, NULL, NULL, NULL,
		K_PRIO_COOP(5), 0, 500);

/* ---- stats ---- */

static float dbfs(float x)
{
	return x > 0.5f ? 20.0f * log10f(x / 32768.0f) : -99.0f;
}

static void print_stat(const struct shell *sh)
{
	unsigned int key = irq_lock();
	uint64_t frames = st.frames;
	int64_t dt = st.t_ticks - st.t0_ticks;
	int16_t pk[2] = { st.peak[0], st.peak[1] };
	uint64_t sq[2] = { st.sq[0], st.sq[1] };
	uint32_t wf = st.win_frames;

	st.peak[0] = st.peak[1] = 0;
	st.sq[0] = st.sq[1] = 0;
	st.win_frames = 0;
	irq_unlock(key);

	/* Rate from the first block to the latest, both timed on the 32 kHz
	 * RTC (the DK's LFXO): the count between them is frames - one block. */
	double hz = dt > 0 ? (double)(frames - BLOCK_FRAMES) *
		    CONFIG_SYS_CLOCK_TICKS_PER_SEC / (double)dt : 0.0;
	/* And over this stat window alone (since the previous stat), so a
	 * codec re-init, which stops the clocks, only spoils one window. */
	static uint64_t w_frames;
	static int64_t w_ticks;
	double whz = (w_ticks && st.t_ticks > w_ticks) ?
		     (double)(frames - w_frames) * CONFIG_SYS_CLOCK_TICKS_PER_SEC /
		     (double)(st.t_ticks - w_ticks) : 0.0;
	double wsec = (double)(st.t_ticks - w_ticks) /
		      CONFIG_SYS_CLOCK_TICKS_PER_SEC;

	w_frames = frames;
	w_ticks = st.t_ticks;

	shell_print(sh, "mode %s%s%s%s  codec %s  frames %llu  fs %.3f Hz "
		    "(over %.1f s)  restarts %u  timeouts %u  i2c retries %u",
		    mode_name[mode],
		    mode == M_PLAY ? " " : "",
		    mode == M_PLAY ? clips[clip_sel].name : "",
		    mode == M_LOOP ? (loop_src == LOOP_L ? " L" :
				      loop_src == LOOP_R ? " R" : " LR") : "",
		    st.codec_ok ? "ack" : "NO ACK", frames, hz,
		    (double)dt / CONFIG_SYS_CLOCK_TICKS_PER_SEC,
		    st.restarts, st.timeouts, wm8960_nacks);
	shell_print(sh, "  fs this window %.3f Hz over %.1f s", whz, wsec);
	for (int ch = 0; ch < 2; ch++) {
		float rms = wf ? sqrtf((float)sq[ch] / wf) : 0.0f;

		shell_print(sh, "  %s mic: peak %6d (%5.1f dBFS)  rms %7.1f "
			    "(%5.1f dBFS)  since last stat",
			    ch ? "right" : "left ", pk[ch], (double)dbfs(pk[ch]),
			    (double)rms, (double)dbfs(rms));
	}
}

/* ---- modes, shared by the shell and the buttons ---- */

static void set_mode(enum mode m)
{
	/* Mic loopback plays on the headphones only: the mics sit right next to
	 * the speaker, and at +41 dB of mic gain the loop
	 * would howl. Every other mode uses both. */
	wm8960_spk_mute(m == M_LOOP);
	mode = m;
	printk("mode %s", mode_name[m]);
	if (m == M_PLAY) {
		printk(" %s", clips[clip_sel].name);
	} else if (m == M_LOOP) {
		printk(" %s (headphones only)", loop_name[loop_src]);
	} else if (m == M_TONE) {
		printk(" %u Hz", tone_hz);
	}
	printk("\n");
}

static int cmd_stop(const struct shell *sh, size_t argc, char **argv)
{
	set_mode(M_STOP);
	return 0;
}

static int cmd_tone(const struct shell *sh, size_t argc, char **argv)
{
	if (argc > 1) {
		tone_hz = CLAMP(strtoul(argv[1], NULL, 0), 50, 3900);
	}
	/* Level in dBFS, e.g. -12 (the default); at most 0. */
	tone_amp = 8192.0f;
	tone_dc = argc > 3 ? (int16_t)strtol(argv[3], NULL, 0) : 0;
	if (argc > 2) {
		tone_amp = 32767.0f * powf(10.0f, MIN(strtof(argv[2], NULL),
						     0.0f) / 20.0f);
	}
	set_mode(M_TONE);
	return 0;
}

static int cmd_play(const struct shell *sh, size_t argc, char **argv)
{
	/* f, m: filtered; fr, mr: raw. */
	bool m = argc > 1 && (argv[1][0] == 'm' || argv[1][0] == 'M');
	bool raw = argc > 1 && (argv[1][1] == 'r' || argv[1][1] == 'R');

	clip_sel = (raw ? 2 : 0) + (m ? 1 : 0);
	set_mode(M_PLAY);
	return 0;
}

static int cmd_loop(const struct shell *sh, size_t argc, char **argv)
{
	if (argc > 1) {
		loop_src = !strcmp(argv[1], "r") ? LOOP_R :
			   !strcmp(argv[1], "lr") ? LOOP_LR : LOOP_L;
	}
	set_mode(M_LOOP);
	return 0;
}

static int cmd_rec(const struct shell *sh, size_t argc, char **argv)
{
	wm_rec_frames = 0;
	rec_on = true;
	shell_print(sh, "recording 8 s of both mics (buffer wm_rec_buf)");
	return 0;
}

static int cmd_stat(const struct shell *sh, size_t argc, char **argv)
{
	print_stat(sh);
	return 0;
}

static int cmd_vol(const struct shell *sh, size_t argc, char **argv)
{
	if (argc < 2) {
		shell_print(sh, "wm vol <0x30..0x7F>: 0x79 = 0 dB, 1 dB steps");
		return 0;
	}
	int err = wm8960_hp_vol(strtoul(argv[1], NULL, 0));

	shell_print(sh, "hp vol %s: %d", argv[1], err);
	return err;
}

static int cmd_spk(const struct shell *sh, size_t argc, char **argv)
{
	if (argc < 2) {
		shell_print(sh, "wm spk <0x30..0x7F>: 0x79 = 0 dB, 0x7F = +6 dB");
		return 0;
	}
	int err = wm8960_spk_vol(strtoul(argv[1], NULL, 0));

	shell_print(sh, "spk vol %s: %d", argv[1], err);
	return err;
}

static int cmd_shift(const struct shell *sh, size_t argc, char **argv)
{
	if (argc > 2) {
		tx_shift = MIN(strtoul(argv[1], NULL, 0), 16);
		rx_shift = MIN(strtoul(argv[2], NULL, 0), 16);
	}
	shell_print(sh, "tx shift %u, rx shift %u", tx_shift, rx_shift);
	return 0;
}

static int cmd_fix(const struct shell *sh, size_t argc, char **argv)
{
	if (argc > 1) {
		tx_fix = strtoul(argv[1], NULL, 0) != 0;
	}
	shell_print(sh, "tx one-bit-early fix %s", tx_fix ? "on" : "off");
	return 0;
}

static int cmd_gain(const struct shell *sh, size_t argc, char **argv)
{
	if (argc < 3) {
		shell_print(sh, "wm gain <pga 0..63> <boost 0..3>: pga 23 = 0 dB, "
			    "0.75 dB steps; boost 0/+13/+20/+29 dB");
		return 0;
	}
	int err = wm8960_mic_gain(strtoul(argv[1], NULL, 0),
				  strtoul(argv[2], NULL, 0));

	shell_print(sh, "mic gain %s %s: %d", argv[1], argv[2], err);
	return err;
}

static int cmd_reg(const struct shell *sh, size_t argc, char **argv)
{
	if (argc < 3) {
		shell_print(sh, "wm reg <reg> <val9>");
		return 0;
	}
	int err = wm8960_write(strtoul(argv[1], NULL, 0),
			       strtoul(argv[2], NULL, 0));

	shell_print(sh, "R%s = %s: %d", argv[1], argv[2], err);
	return err;
}

/* After a good codec init: a 3 s tone, timed from when I2S is actually
 * running (the audio thread starts 500 ms after boot), then silence. Any
 * button or mode change before then cancels it. */
static volatile bool init_tone_armed;
static int64_t init_tone_end;		/* ms; 0 until I2S runs */

static void init_tone(void)
{
	tone_hz = 1000;
	tone_amp = 8192.0f;
	set_mode(M_TONE);
	init_tone_end = 0;
	init_tone_armed = true;
}

static void init_tone_poll(void)
{
	if (!init_tone_armed) {
		return;
	}
	if (mode != M_TONE) {
		init_tone_armed = false;
	} else if (init_tone_end == 0) {
		if (st.frames > 0) {
			init_tone_end = k_uptime_get() + 3000;
		}
	} else if (k_uptime_get() >= init_tone_end) {
		init_tone_armed = false;
		set_mode(M_STOP);
	}
}

static int cmd_init(const struct shell *sh, size_t argc, char **argv)
{
	int err = wm8960_init();

	st.codec_ok = !err;
	if (!err) {
		init_tone();
	}
	shell_print(sh, "wm8960_init: %d", err);
	return err;
}

SHELL_STATIC_SUBCMD_SET_CREATE(wm_cmds,
	SHELL_CMD(stop, NULL, "silence", cmd_stop),
	SHELL_CMD_ARG(tone, NULL, "[hz] [dBFS] sine, default 1000 Hz -12 dBFS", cmd_tone,
		      1, 3),
	SHELL_CMD_ARG(play, NULL, "[f|m|fr|mr] speech clip, looping (r = raw)", cmd_play, 1, 1),
	SHELL_CMD_ARG(loop, NULL, "[l|r|lr] mic to headphones", cmd_loop, 1, 1),
	SHELL_CMD(rec, NULL, "record 8 s of both mics to RAM", cmd_rec),
	SHELL_CMD(stat, NULL, "rate, levels since last stat", cmd_stat),
	SHELL_CMD_ARG(vol, NULL, "<code> headphone volume", cmd_vol, 1, 1),
	SHELL_CMD_ARG(spk, NULL, "<code> speaker volume", cmd_spk, 1, 1),
	SHELL_CMD_ARG(shift, NULL, "[tx rx] sample position in the I2S word",
		      cmd_shift, 1, 2),
	SHELL_CMD_ARG(fix, NULL, "[0|1] DAC word one-bit-early fix",
		      cmd_fix, 1, 1),
	SHELL_CMD_ARG(gain, NULL, "<pga> <boost> mic gain", cmd_gain, 1, 2),
	SHELL_CMD_ARG(reg, NULL, "<reg> <val> raw write", cmd_reg, 1, 2),
	SHELL_CMD(init, NULL, "re-run codec init", cmd_init),
	SHELL_SUBCMD_SET_END);
SHELL_CMD_REGISTER(wm, &wm_cmds, "WM8960 smoke test", NULL);

/* ---- buttons ---- */

static const struct gpio_dt_spec btn[4] = {
	GPIO_DT_SPEC_GET(DT_ALIAS(sw0), gpios),
	GPIO_DT_SPEC_GET(DT_ALIAS(sw1), gpios),
	GPIO_DT_SPEC_GET(DT_ALIAS(sw2), gpios),
	GPIO_DT_SPEC_GET(DT_ALIAS(sw3), gpios),
};
static const struct gpio_dt_spec led = GPIO_DT_SPEC_GET(DT_ALIAS(led0), gpios);

int main(void)
{
	bool was[4] = { 0 };
	int err;

	for (int i = 0; i < 4; i++) {
		gpio_pin_configure_dt(&btn[i], GPIO_INPUT);
	}
	gpio_pin_configure_dt(&led, GPIO_OUTPUT_INACTIVE);

	err = wm8960_init();
	st.codec_ok = !err;
	printk("\nWM8960 smoke test. wm8960_init: %d%s\n", err,
	       err ? " (no ACK at 0x1A: check SDA P0.25, SCL P0.26, 5 V, GND)"
		   : "");
	if (!err) {
		init_tone();
	}
	printk("buttons: 1 tone, 2 speech (F, M filtered; F, M raw), 3 mic loopback L/R/LR, 4 stop. "
	       "Shell: wm help\n");

	for (uint32_t n = 0;; n++) {
		for (int i = 0; i < 4; i++) {
			bool now = gpio_pin_get_dt(&btn[i]) > 0;

			if (now && !was[i]) {
				init_tone_armed = false;
				switch (i) {
				case 0:
					set_mode(M_TONE);
					break;
				case 1:
					/* F filtered, M filtered, F raw,
					 * M raw, then round again. */
					if (mode == M_PLAY) {
						clip_sel = (clip_sel + 1) %
							   ARRAY_SIZE(clips);
					}
					set_mode(M_PLAY);
					break;
				case 2:
					if (mode == M_LOOP) {
						loop_src = (loop_src + 1) % 3;
					}
					set_mode(M_LOOP);
					break;
				default:
					set_mode(M_STOP);
					break;
				}
			}
			was[i] = now;
		}
		init_tone_poll();

		/* LED1 blinks at 1 Hz while I2S blocks arrive, off otherwise. */
		static uint64_t last_frames;

		if (n % 25 == 0) {
			bool running = st.frames != last_frames;

			last_frames = st.frames;
			gpio_pin_set_dt(&led, running && (n / 25) % 2);
		}
		k_msleep(20);
	}
	return 0;
}
