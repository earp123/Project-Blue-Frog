/*
 * rtt_link - J-Link RTT bench port. See rtt_link.h.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/init.h>
#include <zephyr/sys/atomic.h>
#include <SEGGER_RTT.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rtt_link.h"
#include "soak_log.h"
#include "payload_src.h"

#define CH_SOAK		1	/* up: record stream */
#define CH_CTL		1	/* down: commands */
#define CH_REPLY	2	/* up: replies */

/*
 * 8 KB matches the soak_log ring: ~2.5 s at the 20 ms slot, enough to
 * swallow a host hiccup or a capture started a moment late.
 */
static uint8_t soak_buf[8192];
static uint8_t reply_buf[512];
static uint8_t ctl_buf[1024];

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

static atomic_t next_mode = ATOMIC_INIT(PAYLOAD_DEFAULT_MODE);
static atomic_t next_lead_us;

/* Longest staging lead: a frame; anything longer is just "at once". */
#define LEAD_MAX_US	TDMA_FRAME_DURATION_US

uint32_t rtt_link_lead_us(void)
{
	return (uint32_t)atomic_get(&next_lead_us);
}

static const char *const mode_name[] = {
	[SOAK_PAYLOAD_RAMP] = "ramp",
	[SOAK_PAYLOAD_TONE] = "tone",
	[SOAK_PAYLOAD_CLIP] = "clip",
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
	return 0;
}

/* Before the writer thread starts: static threads start after APPLICATION. */
SYS_INIT(rtt_link_init, APPLICATION, 0);

bool rtt_link_write_rec(const void *rec, uint32_t len)
{
	return SEGGER_RTT_Write(CH_SOAK, rec, len) == len;
}

/* One reply line. Whole or nothing, like the records: a host that is not
 * reading loses the reply, never gets half of one.
 */
static void reply(const char *fmt, ...)
{
	char buf[160];
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
			if (strcmp(tok[1], mode_name[m]) == 0) {
				atomic_set(&next_mode, m);
				reply("ok mode %s", mode_name[m]);
				return;
			}
		}
		reply("err cmd");
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

	reply("ok status %s sync=%s soak=%s mode=%s clip=%u/%08x lead=%u "
	      "rtt_drop=%u", unit,
	      sync_name(tdma_get_telemetry()->sync_state),
	      u.soak_running ? "run" : "idle", mode_name[rtt_link_mode()],
	      clip_src_chunks(), clip_src_crc(), rtt_link_lead_us(),
	      ls.rtt_dropped);
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
