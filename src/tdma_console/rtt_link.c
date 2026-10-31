/*
 * rtt_link - J-Link RTT bench port. See rtt_link.h.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/init.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/byteorder.h>
#include <SEGGER_RTT.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rtt_link.h"
#include "soak_log.h"
#ifdef CONFIG_FAT_FILESYSTEM_ELM
#include <zephyr/fs/fs.h>
#include "sd_log.h"
#endif
#include "payload_src.h"
#ifdef CONFIG_SOAK_C2_PROFILE
#include "c2_prof.h"
#endif
#ifdef CONFIG_SOAK_MIC_DSP
#include "mic_dsp.h"
#endif
#ifdef CONFIG_SOAK_OPUS_DEC
#include "opus_dec.h"
#endif

#define CH_SOAK		1	/* up: record stream */
#define CH_CTL		1	/* down: commands */
#define CH_REPLY	2	/* up: replies */

/*
 * The size of the soak_log ring (8 KB, 16 KB at 128 B records): ~2.5 s at
 * the 20 ms slot, enough to swallow a host hiccup or a capture started a
 * moment late.
 */
static uint8_t soak_buf[SOAK_REC_SIZE * SOAK_LOG_RING_DEPTH];
static uint8_t reply_buf[2048];	/* bench + profile: ~10 lines at once */
static uint8_t ctl_buf[1024];
#ifdef CONFIG_SOAK_AUDIO_HAT
#define CH_RAW		3	/* up: raw mic frames */
/* 12 frames, 240 ms: the host drains every 5 ms. */
/* With the mic DSP, two taps (raw and conditioned) at 16.4 KB/s each. */
static uint8_t raw_buf[IS_ENABLED(CONFIG_SOAK_MIC_DSP) ? 8192 : 4096];
static atomic_t raw_on;
static uint32_t raw_skipped;
#endif

/* Longest command line; anything longer is discarded as malformed. */
#define LINE_MAX	64

/* Longest timed soak: a week, well inside the log's uint32 duration_ms. */
#define SOAK_MAX_MIN	(7U * 24U * 60U)

/* Writer-thread line assembly. */
static char line[LINE_MAX];
static size_t line_len;
static bool line_overflow;

/*
 * Clip upload in progress (writer thread). The bytes after a "clip" line are
 * consumed whatever happens, so a refused upload is never misread as
 * commands; bin_err says why it will be refused. An upload that stalls for
 * CLIP_STALL_MS is abandoned.
 */
#define CLIP_STALL_MS	2000
static uint32_t bin_left;
static uint32_t bin_len;
static uint32_t bin_crc;
static const char *bin_err;	/* NULL = loading into clip_src */
static int64_t bin_last_ms;

/* A HAT unit with an encoder boots as an intercom: a soak started from the
 * unit's own controls, no PC attached, talks and listens (mic mode). A unit
 * with a clip in flash boots sending it.
 */
#if defined(CONFIG_SOAK_AUDIO_HAT) && defined(PAYLOAD_HAS_ENC)
static atomic_t next_mode = ATOMIC_INIT(SOAK_PAYLOAD_MIC);
#elif defined(CONFIG_SOAK_CLIP_BUILTIN)
static atomic_t next_mode = ATOMIC_INIT(SOAK_PAYLOAD_CLIP);
#else
static atomic_t next_mode = ATOMIC_INIT(PAYLOAD_DEFAULT_MODE);
#endif
static atomic_t next_lead_us;

/* Longest staging lead: a frame; anything longer is just "at once". */
#define LEAD_MAX_US	TDMA_FRAME_DURATION_US

uint32_t rtt_link_lead_us(void)
{
	return (uint32_t)atomic_get(&next_lead_us);
}

/* A generous default: a whole 20 ms slot for the encoder. Codec 2 HAT builds
 * use 12 ms, clean on the bench with live audio (G2): less delay. The Opus
 * encoder needs the 20 ms (Kconfig SOAK_OPUS_ENC).
 */
#if defined(CONFIG_SOAK_AUDIO_HAT) && !defined(CONFIG_SOAK_OPUS_ENC)
static atomic_t next_phase_us = ATOMIC_INIT(12000);
#else
static atomic_t next_phase_us = ATOMIC_INIT(20000);
#endif

uint32_t rtt_link_phase_us(void)
{
	return (uint32_t)atomic_get(&next_phase_us);
}

static atomic_t next_decx;

uint32_t rtt_link_decx(void)
{
	return (uint32_t)atomic_get(&next_decx);
}

static const char *const mode_name[] = {
	[SOAK_PAYLOAD_RAMP] = "ramp",
	[SOAK_PAYLOAD_TONE] = "tone",
	[SOAK_PAYLOAD_CLIP] = "clip",
	[SOAK_PAYLOAD_PCM] = "pcm",
	[SOAK_PAYLOAD_MIC] = "mic",
	[SOAK_PAYLOAD_LADDER] = "ladder",
};

enum soak_payload_mode rtt_link_mode(void)
{
	return (enum soak_payload_mode)atomic_get(&next_mode);
}

/*
 * One-deep slot for commands the runner must carry out. The writer thread
 * fills it and publishes PENDING; the UI loop reads nothing before that, and
 * frees it once the reply is out.
 */
enum { SLOT_IDLE, SLOT_PENDING };

enum run_cmd { RUN_STATUS, RUN_SOAK, RUN_STOP };

static struct {
	enum run_cmd cmd;
	uint32_t minutes;
	bool sd;
} slot;
static atomic_t slot_state = ATOMIC_INIT(SLOT_IDLE);

static int rtt_link_init(void)
{
	SEGGER_RTT_ConfigUpBuffer(CH_SOAK, "soak", soak_buf, sizeof(soak_buf),
				  SEGGER_RTT_MODE_NO_BLOCK_SKIP);
	SEGGER_RTT_ConfigUpBuffer(CH_REPLY, "ctl", reply_buf,
				  sizeof(reply_buf),
				  SEGGER_RTT_MODE_NO_BLOCK_SKIP);
	SEGGER_RTT_ConfigDownBuffer(CH_CTL, "ctl", ctl_buf, sizeof(ctl_buf),
				    SEGGER_RTT_MODE_NO_BLOCK_SKIP);
#ifdef CONFIG_SOAK_AUDIO_HAT
	SEGGER_RTT_ConfigUpBuffer(CH_RAW, "raw", raw_buf, sizeof(raw_buf),
				  SEGGER_RTT_MODE_NO_BLOCK_SKIP);
#endif
	return 0;
}

#ifdef CONFIG_SOAK_AUDIO_HAT
bool rtt_link_raw_on(void)
{
	return atomic_get(&raw_on) != 0;
}

void rtt_link_raw_frame(char tap, uint32_t n, int k, const int16_t *pcm,
			uint32_t samples)
{
	/* Encoder thread only, so one static frame buffer will do. */
	static uint8_t f[8 + 2 * 160];

	samples = MIN(samples, 160U);
	f[0] = 'R';
	f[1] = (uint8_t)tap;
	f[2] = (uint8_t)k;
	f[3] = 0;
	sys_put_le32(n, &f[4]);
	memcpy(&f[8], pcm, samples * 2);	/* the M33 is little-endian */
	if (SEGGER_RTT_Write(CH_RAW, f, 8 + samples * 2) == 0) {
		raw_skipped++;
	}
}
#endif

/* Before the writer thread starts: static threads start after APPLICATION. */
SYS_INIT(rtt_link_init, APPLICATION, 0);

bool rtt_link_write_rec(const void *rec, uint32_t len)
{
	return SEGGER_RTT_Write(CH_SOAK, rec, len) == len;
}

/* One reply line. Whole or nothing, like the records: a host that is not
 * reading loses the reply, never gets half of one.
 */
static void reply(const char *fmt, ...);

#ifdef CONFIG_FAT_FILESYSTEM_ELM
/*
 * Card access for the host (sdls / sdget / sdrm): the soak logs off a unit
 * without pulling its card. These run in handle_line(), which rtt_link_poll() calls
 * on the soak_log writer thread, the one thread that may touch FatFs.
 */
#define SDGET_MAX	512	/* bytes per sdget: 1 KB of hex in one reply */

static void sd_path(char *out, size_t len, const char *p)
{
	snprintf(out, len, "%s%s%s", SD_MOUNT_POINT, (p[0] == '/') ? "" : "/",
		 (strcmp(p, "/") == 0) ? "" : p);
}

static void cmd_sdls(const char *dir)
{
	static struct fs_dir_t d;
	static struct fs_dirent e;
	char path[64];
	int n = 0;
	int rc = sd_log_mount();

	if (rc < 0) {
		reply("err sd mount %d", rc);
		return;
	}
	sd_path(path, sizeof(path), dir);
	fs_dir_t_init(&d);
	rc = fs_opendir(&d, path);
	if (rc < 0) {
		reply("err sd open %d", rc);
		return;
	}
	while (fs_readdir(&d, &e) == 0 && e.name[0] != '\0') {
		reply("ok sdls %c %u %s",
		      (e.type == FS_DIR_ENTRY_DIR) ? 'd' : 'f',
		      (unsigned int)e.size, e.name);
		n++;
		if ((n % 16) == 0) {
			k_msleep(20);	/* let the host drain the reply buffer */
		}
	}
	fs_closedir(&d);
	reply("ok sdls end %d", n);
}

static void cmd_sdget(const char *file, uint32_t off, uint32_t len)
{
	static struct fs_file_t f;
	static uint8_t data[SDGET_MAX];
	static char line[16 + 2 * SDGET_MAX + 2];
	static const char hex[] = "0123456789abcdef";
	char path[64];
	int rc = sd_log_mount();

	if (rc < 0) {
		reply("err sd mount %d", rc);
		return;
	}
	sd_path(path, sizeof(path), file);
	fs_file_t_init(&f);
	rc = fs_open(&f, path, FS_O_READ);
	if (rc < 0) {
		reply("err sd open %d", rc);
		return;
	}
	rc = fs_seek(&f, off, FS_SEEK_SET);
	ssize_t got = (rc < 0) ? rc : fs_read(&f, data, MIN(len, SDGET_MAX));

	fs_close(&f);
	if (got < 0) {
		reply("err sd read %d", (int)got);
		return;
	}

	int n = snprintf(line, sizeof(line), "ok sdget %d ", (int)got);

	for (ssize_t i = 0; i < got; i++) {
		line[n++] = hex[data[i] >> 4];
		line[n++] = hex[data[i] & 0xF];
	}
	line[n++] = '\n';
	(void)SEGGER_RTT_Write(CH_REPLY, line, (unsigned int)n);
}

static void cmd_sdrm(const char *file)
{
	char path[64];
	int rc = sd_log_mount();

	if (rc < 0) {
		reply("err sd mount %d", rc);
		return;
	}
	sd_path(path, sizeof(path), file);
	rc = fs_unlink(path);
	if (rc < 0) {
		reply("err sd unlink %d", rc);
		return;
	}
	reply("ok sdrm %s", file);
}
#endif

static void reply(const char *fmt, ...)
{
	char buf[256];	/* the encode build's status line is ~150 chars */
	va_list ap;
	int n;

	va_start(ap, fmt);
	n = vsnprintf(buf, sizeof(buf) - 1, fmt, ap);
	va_end(ap);
	if (n < 0) {
		return;
	}
	n = MIN(n, (int)sizeof(buf) - 2);
	buf[n++] = '\n';
	(void)SEGGER_RTT_Write(CH_REPLY, buf, (unsigned int)n);
}

/* Parse a non-negative decimal integer that must fill the whole token. */
static bool parse_u32(const char *s, uint32_t *out)
{
	char *end;
	unsigned long v;

	if (s == NULL || *s < '0' || *s > '9') {
		return false;
	}
	v = strtoul(s, &end, 10);
	if (*end != '\0' || v > UINT32_MAX) {
		return false;
	}
	*out = (uint32_t)v;
	return true;
}

static void park(enum run_cmd cmd, uint32_t minutes, bool sd)
{
	if (atomic_get(&slot_state) != SLOT_IDLE) {
		/* The host sends one command and waits for its reply, so this
		 * only happens if it stopped waiting.
		 */
		reply("err busy");
		return;
	}
	slot.cmd = cmd;
	slot.minutes = minutes;
	slot.sd = sd;
	atomic_set(&slot_state, SLOT_PENDING);
}

/* Split on spaces in place; returns the token count (at most max). */
static int tokenize(char *s, char *tok[], int max)
{
	int n = 0;
	char *save = NULL;

	for (char *t = strtok_r(s, " \t", &save); t != NULL && n < max;
	     t = strtok_r(NULL, " \t", &save)) {
		tok[n++] = t;
	}
	return n;
}

static void clip_finish(void)
{
	if (bin_err == NULL && clip_src_load_end() != 0) {
		bin_err = "crc";
	}
	if (bin_err != NULL) {
		reply("err clip %s", bin_err);
	} else {
		reply("ok clip %u %08x chunks=%u", bin_len, bin_crc,
		      clip_src_chunks());
	}
}

/* "clip <nbytes> <crc32hex>": the bytes follow on the same channel. */
static void clip_begin(const char *len_s, const char *crc_s)
{
	struct soak_log_status ls;
	char *end;
	unsigned long crc;

	crc = strtoul(crc_s, &end, 16);
	if (!parse_u32(len_s, &bin_len) || *crc_s == '\0' || *end != '\0' ||
	    crc > UINT32_MAX) {
		reply("err cmd");
		return;
	}
	bin_crc = (uint32_t)crc;
	bin_err = NULL;

	/* A running soak's TX path reads the buffer unlocked. */
	soak_log_get_status(&ls);
	if (ls.active) {
		bin_err = "busy";
	} else if (clip_src_load_begin(bin_len, bin_crc) != 0) {
		bin_err = "size";
	}

	bin_left = bin_len;
	bin_last_ms = k_uptime_get();
	if (bin_left == 0) {
		clip_finish();
	}
}

#ifdef CONFIG_SOAK_MIC_DSP
/*
 * dsp on|off                  the whole front end, at the next frame
 * dsp hpf|ns|agc on|off       one stage, at the next frame
 * dsp level <n>               AGC target (Speex units), from the next soak
 * dsp nsdb <dB>               noise attenuation, from the next soak
 */
static void cmd_dsp(char **tok, int n)
{
	static const char *const stage_name[] = { "hpf", "ns", "agc" };
	uint32_t v;

	if (n == 2 && (strcmp(tok[1], "on") == 0 ||
		       strcmp(tok[1], "off") == 0)) {
		mic_dsp_set_on(tok[1][1] == 'n');
		reply("ok dsp %s", tok[1]);
		return;
	}
	if (n == 3 && strcmp(tok[1], "level") == 0 && parse_u32(tok[2], &v) &&
	    v >= 1 && v <= 32768) {
		mic_dsp_set_agc_level(v);
		reply("ok dsp level %u", v);
		return;
	}
	if (n == 3 && strcmp(tok[1], "nsdb") == 0 && parse_u32(tok[2], &v) &&
	    v <= 40) {
		mic_dsp_set_ns_db(v);
		reply("ok dsp nsdb %u", v);
		return;
	}
	for (int i = 0; i < (int)ARRAY_SIZE(stage_name); i++) {
		if (n == 3 && strcmp(tok[1], stage_name[i]) == 0 &&
		    (strcmp(tok[2], "on") == 0 || strcmp(tok[2], "off") == 0)) {
			uint32_t m = mic_dsp_get_stages();

			m = (tok[2][1] == 'n') ? (m | BIT(i)) : (m & ~BIT(i));
			mic_dsp_set_stages(m);
			reply("ok dsp %s %s", tok[1], tok[2]);
			return;
		}
	}
	reply("err cmd");
}
#endif

/* One second of 1 ms GetRssiInst samples. */
#define NOISE_SAMPLES_DEFAULT	1000

/* dBm x 10 as "-103.5". */
static void fmt_dbm10(char *out, size_t len, int v)
{
	snprintf(out, len, "%s%d.%d", v < 0 ? "-" : "", abs(v) / 10,
		 abs(v) % 10);
}

/*
 * Blocks this (writer) thread for the run, like bench. The engine refuses
 * while a soak runs (busy) and before the first soak start has initialized
 * it (init); with the engine stopped no records are waiting.
 */
static void cmd_noise(uint32_t samples)
{
	struct tdma_noise nf;
	char mn[12], p50[12], p90[12], mx[12], mean[12];
	int rc = tdma_noise_floor(samples, &nf);

	if (rc == -EBUSY) {
		reply("err noise busy");
		return;
	} else if (rc == -EPERM) {
		reply("err noise init");
		return;
	} else if (rc < 0) {
		reply("err noise %d", rc);
		return;
	}

	fmt_dbm10(mn, sizeof(mn), nf.min_dbm10);
	fmt_dbm10(p50, sizeof(p50), nf.p50_dbm10);
	fmt_dbm10(p90, sizeof(p90), nf.p90_dbm10);
	fmt_dbm10(mx, sizeof(mx), nf.max_dbm10);
	fmt_dbm10(mean, sizeof(mean), nf.mean_dbm10);
	reply("ok noise n=%u gain=0x%02x min=%s p50=%s p90=%s max=%s mean=%s",
	      nf.samples, nf.rx_gain, mn, p50, p90, mx, mean);
}

static void handle_line(char *s)
{
	char *tok[4];
	int n = tokenize(s, tok, ARRAY_SIZE(tok));
	uint32_t minutes;
	uint32_t lead;

	if (n == 0) {
		return;		/* blank line: ignore */
	}

	if (strcmp(tok[0], "mode") == 0 && n == 2) {
		for (int m = 0; m < (int)ARRAY_SIZE(mode_name); m++) {
			if (strcmp(tok[1], mode_name[m]) == 0 &&
			    (m != SOAK_PAYLOAD_PCM || PAYLOAD_PCM_LINKED) &&
			    (m != SOAK_PAYLOAD_MIC || PAYLOAD_MIC_LINKED)) {
				atomic_set(&next_mode, m);
				reply("ok mode %s", mode_name[m]);
				return;
			}
		}
		reply("err cmd");
#ifdef CONFIG_SOAK_C2_ENCODE
	} else if (strcmp(tok[0], "decx") == 0 && n == 2 &&
		   parse_u32(tok[1], &lead) && lead <= C2_DEC_EXTRA_MAX) {
		atomic_set(&next_decx, (atomic_val_t)lead);
		reply("ok decx %u", lead);
	} else if (strcmp(tok[0], "bench") == 0 && n == 2 &&
		   parse_u32(tok[1], &lead)) {
		/* Blocks this (writer) thread for the run: ~35 ms a chunk.
		 * Refused while a soak runs, so no records are waiting.
		 */
		struct c2_bench b;
		int rc = c2_dec_bench(lead, &b);

		if (rc != 0) {
			reply("err bench %d", rc);
		} else {
			reply("ok bench n=%u enc=%u/%u/%u/%u dec=%u/%u/%u/%u "
			      "mix=%u state=%u", b.n, b.enc_min, b.enc_avg,
			      b.enc_p99, b.enc_max, b.dec_min, b.dec_avg,
			      b.dec_p99, b.dec_max, b.mix_max,
			      b.heap_per_state);
#ifdef CONFIG_SOAK_C2_PROFILE
			char pl[200];

			for (int p = 0; p < C2_PROF_PHASES; p++) {
				for (int l = 0;
				     c2_prof_line(p, l, b.n, pl, sizeof(pl));
				     l++) {
					reply("%s", pl);
				}
			}
#endif
		}
#endif
#ifdef CONFIG_SOAK_AUDIO_HAT
	} else if (strcmp(tok[0], "mix") == 0 && n == 2 &&
		   (strcmp(tok[1], "avg") == 0 || strcmp(tok[1], "left") == 0 ||
		    strcmp(tok[1], "right") == 0)) {
		/* The mic mode's mono mix; takes effect at the next frame,
		 * so it can change mid-run.
		 */
		hat_audio_set_mix(tok[1][0] == 'a' ? HAT_MIX_AVG :
				  tok[1][0] == 'l' ? HAT_MIX_LEFT :
						     HAT_MIX_RIGHT);
		reply("ok mix %s", tok[1]);
	} else if (strcmp(tok[0], "rawmic") == 0 && n == 2 &&
		   (strcmp(tok[1], "on") == 0 || strcmp(tok[1], "off") == 0)) {
		atomic_set(&raw_on, tok[1][1] == 'n');
		reply("ok rawmic %s", tok[1]);
	} else if (strcmp(tok[0], "listen") == 0 && n == 2 &&
		   (strcmp(tok[1], "all") == 0 ||
		    (parse_u32(tok[1], &lead) && lead < TDMA_SLOT_COUNT))) {
		/* Test only: every peer is still decoded; only the chosen one
		 * reaches the earpieces. Takes effect at once.
		 */
		hat_audio_set_listen(tok[1][0] == 'a' ? -1 : (int)lead);
		reply("ok listen %s", tok[1]);
	} else if (strcmp(tok[0], "vol") == 0 && n == 2 &&
		   parse_u32(tok[1], &lead) && lead >= 0x30 && lead <= 0x7F) {
		/* Earpiece volume: 0x79 = 0 dB, 1 dB steps. */
		hat_audio_hp_vol((uint8_t)lead);
		reply("ok vol %u", lead);
#endif
#ifdef CONFIG_SOAK_MIC_DSP
	} else if (strcmp(tok[0], "dsp") == 0 && n >= 2) {
		cmd_dsp(tok, n);
#endif
#ifdef CONFIG_FAT_FILESYSTEM_ELM
	} else if (strcmp(tok[0], "sdls") == 0 && n <= 2) {
		cmd_sdls(n == 2 ? tok[1] : "/");
	} else if (strcmp(tok[0], "sdget") == 0 && n == 4 &&
		   parse_u32(tok[2], &minutes) && parse_u32(tok[3], &lead)) {
		cmd_sdget(tok[1], minutes, lead);
	} else if (strcmp(tok[0], "sdrm") == 0 && n == 2) {
		cmd_sdrm(tok[1]);
#endif
	} else if (strcmp(tok[0], "noise") == 0 && n <= 2 &&
		   (n == 1 || parse_u32(tok[1], &lead))) {
		cmd_noise(n == 2 ? lead : NOISE_SAMPLES_DEFAULT);
	} else if (strcmp(tok[0], "phase") == 0 && n == 2 &&
		   parse_u32(tok[1], &lead) && lead <= LEAD_MAX_US) {
		atomic_set(&next_phase_us, (atomic_val_t)lead);
		reply("ok phase %u", lead);
	} else if (strcmp(tok[0], "lead") == 0 && n == 2 &&
		   parse_u32(tok[1], &lead) && lead <= LEAD_MAX_US) {
		atomic_set(&next_lead_us, (atomic_val_t)lead);
		reply("ok lead %u", lead);
	} else if (strcmp(tok[0], "clip") == 0 && n == 3) {
		clip_begin(tok[1], tok[2]);
	} else if (strcmp(tok[0], "status") == 0 && n == 1) {
		park(RUN_STATUS, 0, false);
	} else if (strcmp(tok[0], "soak") == 0 && (n == 2 || n == 3) &&
		   parse_u32(tok[1], &minutes) && minutes <= SOAK_MAX_MIN &&
		   (n == 2 || strcmp(tok[2], "sd") == 0)) {
		park(RUN_SOAK, minutes, n == 3);
	} else if (strcmp(tok[0], "stop") == 0 && n == 1) {
		park(RUN_STOP, 0, false);
	} else {
		reply("err cmd");
	}
}

void rtt_link_poll(void)
{
	char in[64];
	unsigned int got;

	while ((got = SEGGER_RTT_Read(CH_CTL, in, sizeof(in))) > 0) {
		for (unsigned int i = 0; i < got; i++) {
			if (bin_left > 0) {
				/* Clip bytes, possibly right behind their
				 * line in the same read.
				 */
				uint32_t take = MIN(got - i, bin_left);

				if (bin_err == NULL) {
					clip_src_load_bytes(
						(const uint8_t *)&in[i], take);
				}
				bin_left -= take;
				bin_last_ms = k_uptime_get();
				i += take - 1;
				if (bin_left == 0) {
					clip_finish();
				}
				continue;
			}

			char c = in[i];

			if (c == '\r') {
				continue;
			}
			if (c != '\n') {
				if (line_len < sizeof(line) - 1) {
					line[line_len++] = c;
				} else {
					line_overflow = true;
				}
				continue;
			}

			line[line_len] = '\0';
			if (line_overflow) {
				reply("err cmd");
			} else {
				handle_line(line);
			}
			line_len = 0;
			line_overflow = false;
		}
	}

	if (bin_left > 0 && k_uptime_get() - bin_last_ms > CLIP_STALL_MS) {
		/* The host gave up mid-upload; the clip stays invalid. */
		bin_left = 0;
		reply("err clip timeout");
	}
}

static const char *sync_name(uint8_t s)
{
	switch (s) {
	case TDMA_SYNC_SYNCING:
		return "SYNC";
	case TDMA_SYNC_RUNNING:
		return "RUN";
	default:
		return "IDLE";
	}
}

#ifdef CONFIG_SOAK_AUDIO_HAT
/* The HAT's two status lines, in either decoder's build. */
static void status_hat(void)
{
	struct hat_audio_stats hs;
	static const char *const mix_name[] = { "avg", "left", "right" };

	hat_audio_get_stats(&hs);
	/* hat = codec ACKed/I2C retries/5 ms blocks/LRCLK mHz/I2S restarts/
	 * timeouts; slip = short/drop/frames served this run; peak = left/
	 * right since the last status.
	 */
	reply("ok hat %u/%u/%u/%u/%u/%u mix=%s slip=%u/%u/%u peak=%d/%d",
	      hs.codec_ok, hs.i2c_retries, hs.blocks, hs.rate_mhz, hs.restarts,
	      hs.timeouts, mix_name[hat_audio_get_mix()], hs.slip_short,
	      hs.slip_drop, hs.frames, hs.peak[0], hs.peak[1]);

	struct hat_play_stats ps;

	hat_audio_get_play_stats(&ps);
	/* play = chunks/starts/missing/dup/resync; corr = drop/repeat;
	 * lead = min/max samples between write and play (target 320).
	 */
	int lsn = hat_audio_get_listen();
	char lbuf[4];

	snprintf(lbuf, sizeof(lbuf), "%d", lsn);
	/* listen = all, or the one slot played (test); rawmic = on/off and
	 * frames skipped (RTT buffer full).
	 */
	reply("ok play %u/%u/%u/%u/%u corr=%u/%u lead=%d/%d listen=%s "
	      "rawmic=%s/%u", ps.chunks, ps.starts, ps.missing, ps.dup,
	      ps.resync, ps.corr_drop, ps.corr_rep,
	      ps.chunks ? ps.lead_min : 0, ps.chunks ? ps.lead_max : 0,
	      lsn < 0 ? "all" : lbuf, rtt_link_raw_on() ? "on" : "off",
	      raw_skipped);
}
#endif

static void do_status(const struct rtt_link_ops *ops)
{
	struct rtt_link_unit u;
	struct soak_log_status ls;
	char unit[24];

	ops->get_unit(&u);
	soak_log_get_status(&ls);

	if (u.picked) {
		snprintf(unit, sizeof(unit), "role=%c slot=%u",
			 (u.role == TDMA_ROLE_MASTER) ? 'M' : 'S', u.slot_id);
	} else {
		snprintf(unit, sizeof(unit), "role=- slot=-");
	}

	const struct tdma_telemetry *t = tdma_get_telemetry();

	/* pretx = pickups/with a payload/min and last margin (us) before the
	 * TX boundary; the minimum is this run's (tdma.h).
	 */
#ifdef CONFIG_SOAK_C2_ENCODE
	struct c2_enc_stats es;

	c2_enc_get_stats(&es);
	/* enc = staged/late (past the pickup deadline)/busy (stage occupied)/
	 * worst frame encode us/worst last-frame-captured -> staged us/worst
	 * wake after a capture us/encoder stack never used (bytes)/staged
	 * but overwritten before the runner logged it
	 */
	reply("ok status %s sync=%s soak=%s mode=%s clip=%u/%08x lead=%u "
	      "rtt_drop=%u pretx=%u/%u/%d/%d phase=%u enc=%u/%u/%u/%u/%u/%u/%u/%u",
	      unit, sync_name(t->sync_state),
	      u.soak_running ? "run" : "idle", mode_name[rtt_link_mode()],
	      clip_src_chunks(), clip_src_crc(), rtt_link_lead_us(),
	      ls.rtt_dropped, t->pre_tx_pickups, t->pre_tx_staged,
	      (int)t->pre_tx_margin_min_us, (int)t->pre_tx_margin_last_us,
	      rtt_link_phase_us(), es.staged, es.late, es.busy,
	      es.frame_enc_max_us, es.ready_max_us, es.start_late_max_us,
	      es.stack_unused, es.unlogged);

	struct c2_dec_stats ds;
	struct c2_cpu cpu;

	c2_dec_get_stats(&ds);
	c2_dec_get_cpu(&cpu);
	/* dec = decoded/extra/bad/dropped/avg us/max us/mix max us/stack/
	 * heap left; cpu = busy/enc/dec/radio/data, tenths of a percent.
	 */
	reply("ok dec %u/%u/%u/%u/%u/%u/%u/%u/%u cpu=%u/%u/%u/%u/%u",
	      ds.decoded, ds.extra, ds.bad, ds.dropped, ds.dec_avg_us,
	      ds.dec_max_us, ds.mix_max_us, ds.stack_unused, ds.heap_left,
	      cpu.busy, cpu.enc, cpu.dec, cpu.radio, cpu.data);
#ifdef CONFIG_SOAK_AUDIO_HAT
	status_hat();
#endif
#ifdef CONFIG_SOAK_MIC_DSP
	struct mic_dsp_stats dsp;

	mic_dsp_get_stats(&dsp);
	/* dsp = on/off, stages applied (h/n/a), frames this run, us per 20 ms
	 * frame avg/max, arena used/size (B), AGC gain (dB), noise estimate
	 * (dB), speech probability (%), samples at full scale, peak since the
	 * last status.
	 */
	reply("ok dsp %s %c%c%c frames=%u us=%u/%u state=%u/%u gain=%d "
	      "noise=%d speech=%u clip=%u peak=%d",
	      dsp.on ? "on" : "off",
	      (dsp.stages & MIC_DSP_HPF) ? 'h' : '-',
	      (dsp.stages & MIC_DSP_NS) ? 'n' : '-',
	      (dsp.stages & MIC_DSP_AGC) ? 'a' : '-', dsp.frames, dsp.us_avg,
	      dsp.us_max, dsp.state_bytes, dsp.arena_bytes, dsp.agc_gain_db,
	      dsp.noise_db, dsp.speech_pct, dsp.clipped, dsp.peak);
#endif
#else
	reply("ok status %s sync=%s soak=%s mode=%s clip=%u/%08x lead=%u "
	      "rtt_drop=%u pretx=%u/%u/%d/%d", unit, sync_name(t->sync_state),
	      u.soak_running ? "run" : "idle", mode_name[rtt_link_mode()],
	      clip_src_chunks(), clip_src_crc(), rtt_link_lead_us(),
	      ls.rtt_dropped, t->pre_tx_pickups, t->pre_tx_staged,
	      (int)t->pre_tx_margin_min_us, (int)t->pre_tx_margin_last_us);
#ifdef CONFIG_SOAK_OPUS_ENC
	struct c2_enc_stats es;

	c2_enc_get_stats(&es);
	/* enc = staged/late (past the pickup deadline)/busy; us = per 20 ms
	 * frame encode avg/max; ready = worst last-frame-captured -> staged;
	 * wake = worst wake after a capture; stack never used and encoder
	 * state, bytes; err = frames not encoded to size; phase = capture
	 * lead on the TX boundary, us.
	 */
	reply("ok enc %u/%u/%u us=%u/%u ready=%u wake=%u stack=%u state=%u "
	      "err=%u unlogged=%u phase=%u", es.staged, es.late, es.busy,
	      es.frame_enc_avg_us, es.frame_enc_max_us, es.ready_max_us,
	      es.start_late_max_us, es.stack_unused, es.state_bytes,
	      es.enc_errors, es.unlogged, rtt_link_phase_us());
#endif
#ifdef CONFIG_SOAK_OPUS_DEC
	struct opus_dec_stats os;

	opus_dec_get_stats(&os);
	/* opus = decoded/PLC played/PLC silent/resets/late/bad/errors/dropped;
	 * us = per chunk (4 frames) avg/max, PLC chunk max; late = worst
	 * arrival past a frame, us; stack never used and one decoder state,
	 * bytes; cpu = decoder thread, tenths of a percent.
	 */
	reply("ok opus %u/%u/%u/%u/%u/%u/%u/%u us=%u/%u/%u late=%u stack=%u "
	      "state=%u cpu=%u", os.decoded, os.plc_played, os.plc_silent,
	      os.resets, os.late, os.bad, os.errors, os.dropped, os.dec_avg_us,
	      os.dec_max_us, os.plc_max_us, os.late_max_us, os.stack_unused,
	      os.state_bytes, os.cpu);
#endif
#ifdef CONFIG_SOAK_AUDIO_HAT
	status_hat();
#endif
#endif
}

static void do_soak(const struct rtt_link_ops *ops)
{
	struct rtt_link_unit u;
	int rc;

	/* The runner refuses this too; checked here for the specific reply. */
	ops->get_unit(&u);
	if (u.picked && !u.soak_running && !payload_ready(rtt_link_mode())) {
		reply("err soak noclip");
		return;
	}

	rc = ops->soak_start(slot.minutes, slot.sd);
	if (rc == 0) {
		reply("ok soak");
	} else if (rc == -EBUSY) {
		reply("err soak busy");
	} else if (rc == -ENODEV) {
		reply("err soak nounit");
	} else {
		reply("err soak %d", rc);
	}
}

static void do_stop(const struct rtt_link_ops *ops)
{
	if (ops->soak_stop() == 0) {
		reply("ok stop");
	} else {
		reply("err stop idle");
	}
}

void rtt_link_service(const struct rtt_link_ops *ops)
{
	if (atomic_get(&slot_state) != SLOT_PENDING) {
		return;
	}

	switch (slot.cmd) {
	case RUN_STATUS:
		do_status(ops);
		break;
	case RUN_SOAK:
		do_soak(ops);
		break;
	case RUN_STOP:
		do_stop(ops);
		break;
	}

	atomic_set(&slot_state, SLOT_IDLE);
}
