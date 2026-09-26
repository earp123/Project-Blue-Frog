/*
 * Intercom TDMA field unit (CONFIG_APP_TDMA_FIELD): variant entry point for
 * the rev 2 shield. SSD1306 128x64 OLED on i2c1, microSD on spi4, Wio-SX1262
 * on spi2 (boards/nrf5340dk_nrf5340_cpuapp_shield.overlay).
 *
 * The TDMA field console's soak runner on a buttons-only, essentials-only UI.
 * The runner (soak_start / soak_finish / soak_data_step / soak_data_fn /
 * soak_data_tid) and button_cb are copied from src/tdma_console/main.c.
 * Moving the runner into a module both consoles share is a follow-on.
 * Record logging uses the console's sd_log / soak_log, which do not depend
 * on the display (linked from src/tdma_console, see CMakeLists.txt).
 *
 * One image serves every unit. Power-on goes straight to UNIT, which is
 * mandatory: MASTER (slot 0) or SEC 1..3, one TX slot per unit. The choice
 * locks at the first tdma_init(); reboot to change it.
 *
 * The UI is a cycle menu: one item per screen, drawn as large as it fits.
 * UP/DOWN cycles, OK acts, B4 goes back. HOME cycles three items:
 *   SOAK     cycle 5 min / 30 min / 2 h / continuous, OK starts. The live
 *            screen cycles pages: time, RX, TX, phase, link, log, latency.
 *   TX PWR   OK to edit: UP/DOWN steps -9..+22 dBm, default +0 dBm
 *   SD CARD  pages: .BIN count + newest file, free space, card-detect
 * Buttons: B1 UP, B2 DOWN, B3 OK, B4 BACK. B4 also stops a running soak.
 * Entering HOME rescans the card on the soak_log writer thread; the UI
 * never calls FatFs itself.
 *
 * Drawing goes through CFB, which rebuilds the whole 1 KB frame and pushes
 * it every time. At 400 kHz that is ~25 ms of I2C on this thread and nowhere
 * else. The soak data thread outranks it and never touches a bus, so a
 * repaint cannot delay a payload. The live soak screen repaints at 2 Hz.
 *
 * UART logging stays on for bring-up. Boot, card scans, card-detect changes,
 * role, TX power and soak start/stop are logged, but nothing per packet.
 *
 * With CONFIG_SOAK_RTT the J-Link RTT bench port (rtt_link.h) can also start
 * and stop soaks and stream their records to the host; the unit pick stays
 * on the buttons.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/display/cfb.h>
#include <zephyr/drivers/display.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/input/input.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/util.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "tdma.h"
#include "soak_log.h"
#include "payload_src.h"
#ifdef CONFIG_SOAK_RTT
#include "rtt_link.h"
#endif

LOG_MODULE_REGISTER(tdma_field, LOG_LEVEL_INF);

/* Boot TX power for this variant (the engine's own default is +22 dBm). */
#define FIELD_TX_POWER_DBM	0

/* Live soak repaint cadence: <= 2 Hz keeps the I2C cost off the budget. */
#define SOAK_DRAW_PERIOD_MS	500

#define SPLASH_MS		1200

/* ---- Navigation (posted by the button callback, consumed in main) ---- */
enum nav_action {
	NAV_NONE = 0,
	NAV_UP,
	NAV_DOWN,
	NAV_OK,
	NAV_BACK,
};
static atomic_t nav_event = ATOMIC_INIT(NAV_NONE);

/* ---- Screen state machine ---- */
enum screen_id {
	SCR_ROLE_PICK = 0,	/* power-on, mandatory */
	SCR_HOME,
	SCR_SOAK_PICK,
	SCR_SOAK,
	SCR_SD,
};
static enum screen_id screen = SCR_ROLE_PICK;

enum home_row {
	HR_SOAK = 0,
	HR_POWER,
	HR_SD,
	HR_COUNT,
};
static int home_sel;
static bool tx_edit;		/* HR_POWER is being stepped in place */
static char home_msg[24] = "";

static int role_sel;
static int pick_sel;
static char pick_msg[20] = "";

/* The live soak and SD screens are cycles of one-value pages. */
enum soak_page {
	SP_TIME = 0,
	SP_RX,
	SP_TX,
	SP_SYNC,
	SP_LINK,
	SP_LOG,
	SP_LAT,
	SP_COUNT,
};
static int soak_page;

enum sd_page {
	SDP_FILES = 0,
	SDP_FREE,
	SDP_DET,
	SDP_COUNT,
};
static int sd_page;

/* ---- Engine / unit state ----
 * The unit is picked at power-on as one choice: its TX slot, with the role
 * following from it. Slot 0 is the master, which beacons the frame timing;
 * slots 1-3 are secondaries. Picking them together means a master can never
 * sit outside slot 0. Two units must still not pick the same slot.
 */
static enum tdma_role role = TDMA_ROLE_SECONDARY; /* fail-safe default: two
						   * unset units just listen
						   * instead of colliding. */
static uint8_t my_slot = 1;	/* SEC 1 until picked */
static bool engine_inited;	/* unit locks once true */
static int tx_power_dbm = FIELD_TX_POWER_DBM;

/* ---- Soak test state ----
 * Split ownership: the soak data thread (below) owns every field while a
 * run is active; the UI loop only starts/stops a run and reads the fields
 * to draw them. Reads for drawing are deliberately unlocked -- a counter
 * caught mid-update misdraws one frame of a diagnostic, and holding a lock
 * across a repaint would reintroduce exactly the stall this split exists to
 * remove.
 */
static const struct {
	const char *label;
	int64_t ms;		/* 0 = run until STOP */
} soak_durations[] = {
	{ "5 MIN", 5 * 60 * 1000LL },
	{ "30 MIN", 30 * 60 * 1000LL },
	{ "2 HOURS", 120 * 60 * 1000LL },
	{ "CONTINUOUS", 0 },
};
#define PICK_COUNT ((int)ARRAY_SIZE(soak_durations))

static struct {
	bool active;
	bool done;		/* finished (elapsed or STOP); stats frozen */
	int64_t start_ms;
	int64_t duration_ms;
	struct tdma_telemetry snap;	/* engine totals at soak start */

	/* Frame-counter continuity per transmitting slot: the peer's counter
	 * advances by one per frame, so a gap is a missed frame and a repeat
	 * is a stale retransmit — content-level loss the CRC counters miss.
	 */
	uint16_t last_ctr[TDMA_SLOT_COUNT];
	bool have_ctr[TDMA_SLOT_COUNT];
	uint32_t rx_ok;
	uint32_t missed;
	uint32_t dup;

	int16_t last_rssi;
	int8_t last_snr;
	uint8_t seq;		/* TX pattern sequence */

	/*
	 * Staged-payload tracking, so the log records transmissions rather
	 * than submissions. tdma_tx_submit() succeeds whenever the engine's
	 * stage buffer is free — several times per frame at the data thread's
	 * tick — but only one payload per frame is actually sent; the rest
	 * are overwritten in place. We therefore hold a payload "armed" and
	 * only log it once the engine's tx_done confirms it went out.
	 */
	bool tx_armed;
	uint8_t tx_payload[TDMA_PAYLOAD_LEN];
	uint32_t tx_done_at_arm;

	/*
	 * Staging headroom: the worst gap between consecutive data-thread
	 * passes while a run is active, against SOAK_STAGE_BUDGET_US. Under
	 * budget, staging can never be late and stale_retx is impossible. See
	 * the console's copy for why this is pass-to-pass latency rather than
	 * the interval between submits.
	 */
	int64_t last_tick_ms;
	uint32_t tick_gap_max_ms;

	/*
	 * Set by the data thread when the elapsed limit is reached; the UI
	 * loop performs the teardown. soak_finish() closes the log, which
	 * waits on the writer thread for up to 5 s — that must never run on
	 * the cooperative data thread.
	 */
	bool expired;

	/* Payload source, latched at soak start (payload_src.h). Last, so no
	 * other field moves.
	 */
	enum soak_payload_mode payload_mode;

	/* With RTT: slot-clock time the armed payload was staged at, logged
	 * in its TX record for the latency pairing.
	 */
	uint32_t tx_stage_us;
	uint32_t tx_lead_us;	/* and how long before its TX boundary */
	uint32_t lead_us;	/* staging lead, latched at start (0 = at once) */
} soak;

static int64_t soak_last_draw_ms;

/* Counter-snapshot cadence: one STATS record per second is negligible against
 * the packet stream but bounds how stale the counters are if a run is cut off.
 */
#define SOAK_STATS_PERIOD_MS 1000
static int64_t soak_last_stats_ms;

/*
 * Payload-staging budget: how long the application has, after its own TxDone,
 * to hand the engine a fresh payload before the engine re-sends the previous
 * one and counts a stale_retx (51.7 ms at the 20 ms slot). See the console's
 * copy for the derivation.
 */
#define SOAK_STAGE_BUDGET_US \
	((TDMA_SLOT_COUNT - 1) * TDMA_SLOT_DURATION_US - \
	 (TDMA_TX_START_LATENCY_US + TDMA_TOA_US))

BUILD_ASSERT(SOAK_STAGE_BUDGET_US > 0,
	     "TX latency + time-on-air must leave a staging window");

/*
 * Data-thread tick. The engine needs one submit per frame; polling at 2 ms
 * keeps worst-case staging latency an order of magnitude inside the budget,
 * and the work per tick is a mutex plus a handful of compares. Slower when
 * idle, since nothing can arrive between runs.
 */
#define SOAK_DATA_TICK_MS	2
#define SOAK_DATA_IDLE_MS	50
#define SOAK_DATA_STACK		1536

/*
 * Cooperative, one notch below the radio thread (K_PRIO_COOP(4)) and above
 * every preemptible thread, so no amount of drawing, button handling or card
 * I/O can delay a payload. It never blocks on anything but its own sleep and
 * never touches a bus, so sitting above the UI costs the UI nothing.
 */
#define SOAK_DATA_PRIO		K_PRIO_COOP(6)

/* Guards the soak struct's lifecycle transitions against the data thread. */
static K_MUTEX_DEFINE(soak_lock);

/* Set by any thread to ask main to redraw. */
static atomic_t screen_dirty = ATOMIC_INIT(0);

static void mark_dirty(void)
{
	atomic_set(&screen_dirty, 1);
}

/* ---------------------------------------------------------------------------
 * Input callback (input thread context; post events only, never draw)
 * ------------------------------------------------------------------------- */

/* DK buttons -> nav. B1=UP(KEY_0) B2=DOWN(KEY_1) B3=OK(KEY_2) B4=BACK(KEY_3). */
static void button_cb(struct input_event *evt, void *user_data)
{
	ARG_UNUSED(user_data);

	if (evt->type != INPUT_EV_KEY || evt->value == 0) {
		return; /* press only */
	}

	enum nav_action action;

	switch (evt->code) {
	case INPUT_KEY_0:
		action = NAV_UP;
		break;
	case INPUT_KEY_1:
		action = NAV_DOWN;
		break;
	case INPUT_KEY_2:
		action = NAV_OK;
		break;
	case INPUT_KEY_3:
		action = NAV_BACK;
		break;
	default:
		return;
	}

	atomic_set(&nav_event, action);
}
INPUT_CALLBACK_DEFINE(DEVICE_DT_GET(DT_PATH(buttons)), button_cb, NULL);

/* ---------------------------------------------------------------------------
 * OLED (CFB): the cycle UI
 *
 * Every screen is one card: a caption line on top, one item in the middle,
 * and a detail line at the bottom. UP/DOWN cycles, OK acts, B4 goes back.
 *
 * The edge lines use the 16 px font (12 characters) so they stay readable;
 * the 8x8 font is only a fallback for a runtime value too long for that.
 * The middle item is capped at 24 px and centred in the 32 px left between
 * the edges, so it never crowds them.
 * ------------------------------------------------------------------------- */
static const struct device *const disp = DEVICE_DT_GET(DT_CHOSEN(zephyr_display));

static int disp_w, disp_h;

/* CFB fonts with their cell sizes, tallest first. CFB orders fonts by
 * section name, not by size, so they are sorted once at init.
 */
#define FONTS_MAX	8
static struct {
	uint8_t idx;
	uint8_t w, h;
} fonts[FONTS_MAX];
static int n_fonts;

#define EDGE_H		16	/* caption and detail line height */
#define MID_GAP		4	/* minimum clearance between middle and edges */
#define CAP_GAP		4	/* minimum gap between the caption's halves */

static bool display_init(void)
{
	if (!device_is_ready(disp)) {
		LOG_ERR("OLED not ready: no ACK at 0x%02x on i2c1. Try the "
			"overlay's fallbacks (0x3d, then P1.02/P1.03)",
			(unsigned int)DT_REG_ADDR(DT_CHOSEN(zephyr_display)));
		return false;
	}

	if (display_set_pixel_format(disp, PIXEL_FORMAT_MONO10) != 0 &&
	    display_set_pixel_format(disp, PIXEL_FORMAT_MONO01) != 0) {
		LOG_ERR("OLED: no monochrome pixel format");
		return false;
	}

	int rc = display_blanking_off(disp);

	if (rc != 0 && rc != -ENOSYS) {
		LOG_ERR("OLED: blanking off failed (%d)", rc);
		return false;
	}

	rc = cfb_framebuffer_init(disp);
	if (rc != 0) {
		LOG_ERR("OLED: framebuffer init failed (%d)", rc);
		return false;
	}

	disp_w = cfb_get_display_parameter(disp, CFB_DISPLAY_WIDTH);
	disp_h = cfb_get_display_parameter(disp, CFB_DISPLAY_HEIGHT);

	int n = cfb_get_numof_fonts(disp);

	for (int i = 0; i < n && n_fonts < FONTS_MAX; i++) {
		uint8_t w, h;

		if (cfb_get_font_size(disp, i, &w, &h) != 0 || w == 0 || h == 0) {
			continue;
		}
		/* Insertion sort, tallest first. */
		int j = n_fonts++;

		while (j > 0 && fonts[j - 1].h < h) {
			fonts[j] = fonts[j - 1];
			j--;
		}
		fonts[j].idx = (uint8_t)i;
		fonts[j].w = w;
		fonts[j].h = h;
	}

	if (n_fonts == 0 || fonts[n_fonts - 1].h > EDGE_H) {
		LOG_ERR("OLED: no font of %d px or less", EDGE_H);
		return false;
	}

	cfb_framebuffer_clear(disp, true);
	LOG_INF("OLED %dx%d, %d fonts, smallest %dx%d", disp_w, disp_h,
		n_fonts, fonts[n_fonts - 1].w, fonts[n_fonts - 1].h);
	return true;
}

static int text_w(int f, const char *s)
{
	return (int)strlen(s) * fonts[f].w;
}

/* Tallest font no taller than max_h that fits s in the display width; the
 * smallest font if none does (it then clips at the right edge).
 */
static int font_fit(const char *s, int max_h)
{
	for (int i = 0; i < n_fonts; i++) {
		if (fonts[i].h <= max_h && text_w(i, s) <= disp_w) {
			return i;
		}
	}
	return n_fonts - 1;
}

static void d_text(int f, int x, int y, const char *s)
{
	cfb_framebuffer_set_font(disp, fonts[f].idx);
	cfb_draw_text(disp, s, MAX(0, x), y);
}

/* One whole screen. Any part may be NULL or empty. */
static void draw_card(const char *top_l, const char *top_r, const char *mid,
		      const char *bottom)
{
	int band = disp_h - 2 * EDGE_H;
	int f;

	top_l = (top_l != NULL) ? top_l : "";
	top_r = (top_r != NULL) ? top_r : "";

	cfb_framebuffer_clear(disp, false);

	/* Caption: both halves in one font, the tallest that fits them side by
	 * side with at least CAP_GAP between, so they never overlap.
	 */
	for (f = 0; f < n_fonts - 1; f++) {
		if (fonts[f].h <= EDGE_H &&
		    text_w(f, top_l) + CAP_GAP + text_w(f, top_r) <= disp_w) {
			break;
		}
	}
	d_text(f, 0, 0, top_l);
	d_text(f, disp_w - text_w(f, top_r), 0, top_r);

	if (mid != NULL && mid[0]) {
		f = font_fit(mid, band - 2 * MID_GAP);
		d_text(f, (disp_w - text_w(f, mid)) / 2,
		       EDGE_H + (band - fonts[f].h) / 2, mid);
	}

	if (bottom != NULL && bottom[0]) {
		f = font_fit(bottom, EDGE_H);
		d_text(f, (disp_w - text_w(f, bottom)) / 2,
		       disp_h - fonts[f].h, bottom);
	}

	cfb_framebuffer_finalize(disp);
}

/* A unit by slot: "MASTER" for slot 0, "SEC 1".."SEC 3" otherwise. */
static const char *unit_name(uint8_t slot)
{
	static const char *const name[TDMA_SLOT_COUNT] = {
		"MASTER", "SEC 1", "SEC 2", "SEC 3",
	};

	return name[slot % TDMA_SLOT_COUNT];
}

/* Short form for the soak caption: "M", "S1".."S3". */
static const char *unit_tag(void)
{
	static const char *const tag[TDMA_SLOT_COUNT] = {
		"M", "S1", "S2", "S3",
	};

	return tag[my_slot % TDMA_SLOT_COUNT];
}

static const char *role_str(void)
{
	return unit_name(my_slot);
}

/* "4:59" under an hour, "1:04:59" from there (hours wrap at 99). */
static void fmt_clock(char *out, size_t n, int64_t ms)
{
	unsigned s = (unsigned)(ms / 1000);

	if (s < 3600U) {
		snprintf(out, n, "%u:%02u", s / 60, s % 60);
	} else {
		snprintf(out, n, "%u:%02u:%02u", (s / 3600) % 100,
			 (s / 60) % 60, s % 60);
	}
}

/* "118.4G" / "950M". */
static void fmt_mib(char *out, size_t n, uint32_t mib)
{
	if (mib >= 1024U) {
		snprintf(out, n, "%u.%uG", mib / 1024U, (mib % 1024U) * 10U / 1024U);
	} else {
		snprintf(out, n, "%uM", mib);
	}
}

/* ---------------------------------------------------------------------------
 * SD card summary + card-detect
 *
 * Every card operation runs on the soak_log writer thread. The UI submits a
 * summary op and polls for completion from its loop, the same contract as the
 * console's file browser.
 * ------------------------------------------------------------------------- */
#define SD_RES_NONE	1	/* never scanned */

static struct sd_summary sd_scratch;	/* written by the writer mid-op */
static struct sd_summary sd_sum;	/* last completed scan, for drawing */
static int sd_res = SD_RES_NONE;	/* 0 ok, < 0 errno */
static const char *sd_stage = "";	/* failing step when sd_res < 0 */
static bool sd_busy;

static void sd_request_scan(void)
{
	if (sd_busy) {
		return;
	}
	if (sd_fsop_submit_summary(SD_MOUNT_POINT, &sd_scratch) == 0) {
		sd_busy = true;
		mark_dirty();
	}
}

static void sd_poll(void)
{
	int res, cnt;

	if (!sd_busy || !sd_fsop_poll(&res, &cnt)) {
		return;
	}
	sd_busy = false;
	sd_res = res;

	if (res == 0) {
		sd_sum = sd_scratch;
		sd_stage = "";
		LOG_INF("SD: %u .BIN in root, newest \"%s\"; free %u MiB of "
			"%u MiB (statvfs %d)", sd_sum.bin_count, sd_sum.newest,
			sd_sum.free_mib, sd_sum.total_mib, sd_sum.vfs_err);
	} else {
		/* "disk" = card never came up (the SCK/MISO fallback), "mount"
		 * = card answers but the volume was rejected, "" = the mount
		 * was fine and the directory walk failed.
		 */
		sd_stage = sd_log_mount_stage();
		LOG_WRN("SD scan failed at %s: %d",
			sd_stage[0] ? sd_stage : "list", res);
	}
	mark_dirty();
}

/* Newest log: this boot's own last file if there is one (exact), otherwise
 * the scan's last *.BIN in directory order.
 */
static const char *sd_newest(void)
{
	struct soak_log_status ls;

	soak_log_get_status(&ls);
	if (!ls.err && ls.path[0]) {
		const char *base = strrchr(ls.path, '/');

		return (base != NULL) ? base + 1 : ls.path;
	}
	return sd_sum.newest;
}

/* The failing step, short: "disk" / "mnt" / "io" (the directory walk). */
static const char *sd_stage_short(void)
{
	if (strcmp(sd_stage, "mount") == 0) {
		return "mnt";
	}
	return sd_stage[0] ? sd_stage : "io";
}

/* Card state in 12 characters or fewer, for HOME's detail line. */
static void sd_state(char *out, size_t n)
{
	if (sd_res == SD_RES_NONE) {
		snprintf(out, n, "%s", sd_busy ? "SD scanning" : "SD -");
	} else if (sd_res < 0) {
		snprintf(out, n, "SD %s %d", sd_stage_short(), sd_res);
	} else {
		snprintf(out, n, "SD %u logs", sd_sum.bin_count);
	}
}

/*
 * Card-detect: an application-read GPIO, not wired into the SD driver, so a
 * card swapped while mounted is not re-initialised (reboot after a swap).
 * Active-low, confirmed on the bench 2026-09-24: raw 0 with a card seated,
 * raw 1 without. Every change is logged with the raw level.
 */
static const struct gpio_dt_spec sd_det =
	GPIO_DT_SPEC_GET(DT_PATH(zephyr_user), sd_det_gpios);
static bool det_ok;
static int det_raw = -1;
static int det_logical = -1;

static void det_init(void)
{
	if (!gpio_is_ready_dt(&sd_det) ||
	    gpio_pin_configure_dt(&sd_det, GPIO_INPUT) != 0) {
		LOG_ERR("SD DET: P1.12 not configurable");
		return;
	}
	det_ok = true;
}

static void det_poll(void)
{
	if (!det_ok) {
		return;
	}

	int raw = gpio_pin_get_raw(sd_det.port, sd_det.pin);
	int logical = gpio_pin_get_dt(&sd_det);

	if (raw < 0 || logical < 0 || raw == det_raw) {
		return;
	}
	det_raw = raw;
	det_logical = logical;
	LOG_INF("SD DET: %s (raw %d)", logical ? "card in" : "no card", raw);
	mark_dirty();
}

/* ---------------------------------------------------------------------------
 * Soak control + data plane (copied from src/tdma_console/main.c)
 * ------------------------------------------------------------------------- */
static void soak_start(int64_t duration_ms, bool sd)
{
	enum soak_payload_mode mode = payload_next_mode();
	int rc;

	/* A clip soak needs a clip; refuse before touching the engine. */
	if (!payload_ready(mode)) {
		snprintf(home_msg, sizeof(home_msg), "no clip");
		screen = SCR_HOME;
		return;
	}

	if (!engine_inited) {
		struct tdma_config cfg = {
			.role = role,
			/* Picked at power-on: the master beacons in slot 0,
			 * each secondary in its own slot 1-3.
			 */
			.slot_id = my_slot,
			.slot_duration_us = TDMA_SLOT_DURATION_US,
		};

		rc = tdma_init(&cfg);
		if (rc < 0) {
			snprintf(home_msg, sizeof(home_msg), "init err %d", rc);
			screen = SCR_HOME;
			return;
		}
		engine_inited = true; /* unit is now locked */
	}

	/* The data thread reads this struct on its own tick; clear and
	 * republish it under the lock rather than racing a memset against it.
	 * The payload source is latched here for the whole run.
	 */
	k_mutex_lock(&soak_lock, K_FOREVER);
	memset(&soak, 0, sizeof(soak));
	soak.snap = *tdma_get_telemetry();
	soak.payload_mode = mode;
#ifdef CONFIG_SOAK_RTT
	soak.lead_us = rtt_link_lead_us();
#endif
	k_mutex_unlock(&soak_lock);

	/* Logging is best-effort: a missing or failed card must never stop a
	 * radio test. The failure is surfaced on the soak screen instead. No
	 * drawers on this variant: logs always land in the card root.
	 */
	(void)soak_log_start(role, my_slot,
			     (int8_t)tx_power_dbm, (uint32_t)duration_ms, NULL,
			     sd, mode);

	rc = tdma_start();
	if (rc < 0) {
		snprintf(home_msg, sizeof(home_msg), "start err %d", rc);
		soak_log_stop();
		screen = SCR_HOME;
		return;
	}

	/*
	 * Baseline counter snapshot at t=0. The engine's counters are not
	 * reset by tdma_start(), so they carry over between soaks in one boot;
	 * with this record the decoder gets an exact per-run delta by
	 * differencing the first and last STATS instead of missing the first
	 * sample period.
	 *
	 * Written before the data thread is armed, and it seeds
	 * soak_last_stats_ms too: arming first would let the thread's own
	 * cadence check fire against a stale timestamp and emit a snapshot
	 * ahead of the baseline.
	 */
	soak_last_stats_ms = k_uptime_get();
	soak_log_stats(tdma_get_telemetry());

	/* Arming last: the data thread does nothing until active is set, so
	 * the engine is running and the baseline is on disk by the time it
	 * stages its first payload.
	 */
	k_mutex_lock(&soak_lock, K_FOREVER);
	soak.start_ms = k_uptime_get();
	soak.duration_ms = duration_ms;
	soak.active = true;
	k_mutex_unlock(&soak_lock);

	soak_last_draw_ms = 0;
	home_msg[0] = '\0';

	screen = SCR_SOAK;
}

static void soak_finish(void)
{
	/*
	 * Clear active first: it is what stops the data thread staging and
	 * logging, so nothing can queue a record after soak_log_stop() has
	 * drained and closed the file.
	 */
	k_mutex_lock(&soak_lock, K_FOREVER);
	soak.active = false;
	/* Clearing the latch matters: the UI loop calls us straight off
	 * soak.expired, so leaving it set would tear the run down again on
	 * every pass — stopping an already-stopped engine and reclosing a
	 * closed log.
	 */
	soak.expired = false;
	k_mutex_unlock(&soak_lock);

	tdma_stop();

	/* One last counter snapshot, then drain and close the log. */
	soak_log_stats(tdma_get_telemetry());
	soak_log_stop();

	soak.done = true;
	soak_last_draw_ms = k_uptime_get();
	mark_dirty();
}

/*
 * One data-plane pass: drain received frames into the continuity stats, log
 * the transmission the engine has just completed, and stage the next payload.
 * Runs on the soak data thread with soak_lock held — never on the UI loop.
 */
static void soak_data_step(void)
{
	struct tdma_rx_msg msg;

	while (k_msgq_get(&tdma_rx_msgq, &msg, K_NO_WAIT) == 0) {
		uint8_t s = msg.slot_id % TDMA_SLOT_COUNT;

		if (!soak.active) {
			continue; /* discard outside a soak */
		}

		/* Log every received packet verbatim — payload included — before
		 * any derived accounting, so the file holds the raw evidence.
		 */
		soak_log_rx(&msg, tdma_get_telemetry()->sync_state);

		if (soak.have_ctr[s]) {
			uint16_t delta = msg.frame_ctr - soak.last_ctr[s];

			if (delta == 0) {
				soak.dup++;
			} else if (delta > 1 && delta < 0x8000) {
				/* Backward jumps (>= 0x8000) are a peer
				 * restart, not loss; count forward gaps only.
				 */
				soak.missed += delta - 1;
			}
		}
		soak.last_ctr[s] = msg.frame_ctr;
		soak.have_ctr[s] = true;
		soak.rx_ok++;
		soak.last_rssi = msg.rssi;
		soak.last_snr = msg.snr;
	}

	if (!soak.active) {
		return;
	}

	/*
	 * Worst pass-to-pass latency of this thread — the headroom figure.
	 * Seeded rather than recorded on the first active pass: the pass
	 * before it was an idle-cadence sleep (SOAK_DATA_IDLE_MS), which
	 * would otherwise be logged as the run's worst latency.
	 */
	int64_t tick_now = k_uptime_get();

	if (soak.last_tick_ms) {
		uint32_t lat = (uint32_t)(tick_now - soak.last_tick_ms);

		if (lat > soak.tick_gap_max_ms) {
			soak.tick_gap_max_ms = lat;
		}
	}
	soak.last_tick_ms = tick_now;

	const struct tdma_telemetry *t = tdma_get_telemetry();

	/*
	 * An armed payload that the engine has now transmitted: log it as the
	 * transmission it actually was, then arm the next one. Submitting on
	 * every pass instead would overwrite most payloads before their slot.
	 */
	if (soak.tx_armed && t->tx_done != soak.tx_done_at_arm) {
		if (IS_ENABLED(CONFIG_SOAK_RTT)) {
			soak_log_tx_at(soak.tx_payload, my_slot,
				       t->sync_state, soak.tx_stage_us,
				       soak.tx_lead_us);
		} else {
			soak_log_tx(soak.tx_payload, my_slot,
				    t->sync_state);
		}
		soak.tx_armed = false;
	}

	uint32_t lead_us = 0;

	if (!soak.tx_armed && payload_stage_due(soak.lead_us, &lead_us)) {
		/* Keyed to tx_done; see the console's copy. */
		payload_fill(soak.tx_payload, soak.payload_mode, my_slot,
			     t->tx_done - soak.snap.tx_done, soak.seq);
		uint32_t stage_us = IS_ENABLED(CONFIG_SOAK_RTT) ?
					    tdma_now_us() : 0;

		if (tdma_tx_submit(soak.tx_payload) == 0) {
			if (IS_ENABLED(CONFIG_SOAK_RTT)) {
				soak.tx_stage_us = stage_us;
				soak.tx_lead_us = lead_us;
			}
			soak.tx_armed = true;
			soak.tx_done_at_arm = t->tx_done;
			soak.seq++;
		}
	}

	/* Periodic engine-counter snapshot: cheap, and it lets the decoder
	 * reconstruct rates without re-deriving them from the packet stream.
	 */
	if (k_uptime_get() - soak_last_stats_ms >= SOAK_STATS_PERIOD_MS) {
		soak_last_stats_ms = k_uptime_get();
		soak_log_stats(t);
	}

	/*
	 * Elapsed limit reached. Only flag it: soak_finish() closes the log,
	 * which waits on the writer thread, and this thread is cooperative.
	 */
	if (soak.duration_ms &&
	    k_uptime_get() - soak.start_ms >= soak.duration_ms) {
		soak.expired = true;
	}
}

static void soak_data_fn(void *a, void *b, void *c)
{
	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);

	while (1) {
		bool active;

		k_mutex_lock(&soak_lock, K_FOREVER);
		soak_data_step();
		active = soak.active;
		k_mutex_unlock(&soak_lock);

		k_sleep(active ? K_MSEC(SOAK_DATA_TICK_MS)
			       : K_MSEC(SOAK_DATA_IDLE_MS));
	}
}

K_THREAD_DEFINE(soak_data_tid, SOAK_DATA_STACK, soak_data_fn,
		NULL, NULL, NULL, SOAK_DATA_PRIO, 0, 0);

/* Start/stop wrappers: the runner above, plus a UART record of each run. */
static void soak_begin(int pick)
{
	LOG_INF("soak start: %s, %s, %+d dBm", role_str(),
		soak_durations[pick].label, tx_power_dbm);
	soak_page = SP_TIME;
	soak_start(soak_durations[pick].ms, true);
	if (screen != SCR_SOAK) {
		LOG_ERR("soak did not start: %s", home_msg);
	}
}

static void soak_end(const char *why)
{
	const struct tdma_telemetry *t;
	struct soak_log_status ls;

	soak_finish();

	t = tdma_get_telemetry();
	soak_log_get_status(&ls);
	LOG_INF("soak end (%s) after %u s: tx %u stale %u, rx %u miss %u "
		"dup %u, crc %u, busy %u, lat %u/%u ms", why,
		(uint32_t)((soak_last_draw_ms - soak.start_ms) / 1000),
		t->tx_done - soak.snap.tx_done,
		t->stale_retx - soak.snap.stale_retx, soak.rx_ok, soak.missed,
		soak.dup, t->rx_crc_err - soak.snap.rx_crc_err,
		t->busy_timeouts - soak.snap.busy_timeouts,
		soak.tick_gap_max_ms, SOAK_STAGE_BUDGET_US / 1000);
	if (ls.err) {
		LOG_WRN("soak log failed at %s: %d", ls.err_stage, ls.err);
	} else {
		LOG_INF("soak log %s: %u records written, %u dropped",
			ls.path[0] ? ls.path : "(none)", ls.written,
			ls.dropped);
	}
	if (IS_ENABLED(CONFIG_SOAK_RTT)) {
		LOG_INF("soak rtt: %u records sent, %u dropped",
			ls.rtt_written, ls.rtt_dropped);
	}
}

#ifdef CONFIG_SOAK_RTT
/* ---- RTT bench port: the runner's side (UI loop only) ---- */
static void rtt_get_unit(struct rtt_link_unit *u)
{
	u->picked = (screen != SCR_ROLE_PICK);
	u->role = role;
	u->slot_id = my_slot;
	u->soak_running = soak.active;
}

static int rtt_soak_start(uint32_t minutes, bool sd)
{
	if (screen == SCR_ROLE_PICK) {
		return -ENODEV;
	}
	/* A card scan still in flight would delay the log's open, as on the
	 * soak pick screen; only a run that asked for the card waits on it.
	 */
	if (soak.active || (sd && sd_busy)) {
		return -EBUSY;
	}

	LOG_INF("soak start (rtt): %s, %u min, %+d dBm%s", role_str(),
		minutes, tx_power_dbm, sd ? ", +SD" : "");
	tx_edit = false;
	soak_page = SP_TIME;
	soak_start((int64_t)minutes * 60 * 1000, sd);
	if (!soak.active) {
		LOG_ERR("soak did not start: %s", home_msg);
		return -EIO;
	}
	mark_dirty();
	return 0;
}

static int rtt_soak_stop(void)
{
	if (!soak.active) {
		return -EALREADY;
	}
	soak_end("rtt");
	return 0;
}

static const struct rtt_link_ops rtt_ops = {
	.get_unit = rtt_get_unit,
	.soak_start = rtt_soak_start,
	.soak_stop = rtt_soak_stop,
};
#endif /* CONFIG_SOAK_RTT */

/* ---------------------------------------------------------------------------
 * Screens. Edge strings are kept to 12 characters so they fit the 16 px font.
 * ------------------------------------------------------------------------- */

/* Log file name without its directory or extension: "P0_5M_000". */
static void file_stem(char *out, size_t n, const char *path)
{
	const char *base = strrchr(path, '/');
	const char *dot;

	base = (base != NULL) ? base + 1 : path;
	dot = strrchr(base, '.');
	snprintf(out, n, "%.*s",
		 (int)((dot != NULL) ? (size_t)(dot - base) : strlen(base)),
		 base);
}

static void draw_splash(void)
{
	char sub[16];

	snprintf(sub, sizeof(sub), "%uMHz %ums",
		 (unsigned)(TDMA_RF_FREQ_HZ / 1000000UL),
		 TDMA_SLOT_DURATION_US / 1000);
	draw_card("rev 2 shield", NULL, "TDMA FIELD", sub);
}

/* Power-on unit pick: one item per slot, MASTER being slot 0. */
static void draw_role_pick(void)
{
	char pos[24], bottom[24];

	snprintf(pos, sizeof(pos), "%d/%d", role_sel + 1, TDMA_SLOT_COUNT);
	snprintf(bottom, sizeof(bottom), "slot %d  OK", role_sel);
	draw_card("UNIT", pos, unit_name((uint8_t)role_sel), bottom);
}

static const char *const home_label[HR_COUNT] = {
	[HR_SOAK] = "SOAK",
	[HR_POWER] = "TX PWR",
	[HR_SD] = "SD CARD",
};

static void draw_home(void)
{
	char mid[12], bottom[24], fr[12];

	if (tx_edit) {
		snprintf(mid, sizeof(mid), "%+ddBm", tx_power_dbm);
		draw_card("TX POWER", NULL, mid, "OK = done");
		return;
	}

	/* Unit on top: with identical units on the bench it is the one
	 * thing you cannot tell by looking at them. The card state sits under
	 * SOAK, the default item, since every soak logs to it.
	 */
	bottom[0] = '\0';
	if (home_msg[0]) {
		snprintf(bottom, sizeof(bottom), "%s", home_msg);
	} else if (home_sel == HR_SOAK) {
		sd_state(bottom, sizeof(bottom));
	} else if (home_sel == HR_POWER) {
		snprintf(bottom, sizeof(bottom), "%+d dBm", tx_power_dbm);
	} else if (sd_res == 0 && sd_sum.vfs_err == 0) {
		fmt_mib(fr, sizeof(fr), sd_sum.free_mib);
		snprintf(bottom, sizeof(bottom), "%s free", fr);
	} else {
		sd_state(bottom, sizeof(bottom));
	}
	draw_card(role_str(), NULL, home_label[home_sel], bottom);
}

static void draw_soak_pick(void)
{
	char pos[24];

	snprintf(pos, sizeof(pos), "%d/%d", pick_sel + 1, PICK_COUNT);
	draw_card("SOAK", pos, soak_durations[pick_sel].label,
		  pick_msg[0] ? pick_msg : "OK = start");
}

/* Live stats: one value per page, UP/DOWN cycles. The caption's right side
 * is the run state and unit on every page.
 */
static void draw_soak(void)
{
	static const char *const page_name[SP_COUNT] = {
		[SP_TIME] = "TIME",
		[SP_RX] = "RX",
		[SP_TX] = "TX",
		[SP_SYNC] = "PHASE",
		[SP_LINK] = "LINK",
		[SP_LOG] = "LOG",
		[SP_LAT] = "LAT",
	};
	static const char *const state_str[] = {
		[TDMA_SYNC_STOPPED] = "IDLE",
		[TDMA_SYNC_SYNCING] = "SYNC",
		[TDMA_SYNC_RUNNING] = "RUN",
	};
	const struct tdma_telemetry *t = tdma_get_telemetry();
	uint32_t busy = t->busy_timeouts - soak.snap.busy_timeouts;
	unsigned int budget_ms = SOAK_STAGE_BUDGET_US / 1000;
	struct soak_log_status ls;
	char top_r[12], mid[24], bottom[24], lim[12];

	/* BUSY timeouts are fatal to the engine, so they take the state slot. */
	const char *st = soak.done ? "DONE"
			 : busy    ? "BUSY"
				   : state_str[t->sync_state %
					       ARRAY_SIZE(state_str)];

	snprintf(top_r, sizeof(top_r), "%s %s", st, unit_tag());
	bottom[0] = '\0';

	switch (soak_page) {
	case SP_TIME: {
		int64_t elapsed = soak.active || soak.done
					  ? (soak.done ? soak_last_draw_ms
						       : k_uptime_get()) -
						    soak.start_ms
					  : 0;

		fmt_clock(mid, sizeof(mid), elapsed);
		if (soak.duration_ms) {
			fmt_clock(lim, sizeof(lim), soak.duration_ms);
			snprintf(bottom, sizeof(bottom), "of %s", lim);
		} else {
			snprintf(bottom, sizeof(bottom), "no limit");
		}
		break;
	}
	case SP_RX:
		/* Frame-counter continuity: gaps are missed frames, repeats are
		 * stale retransmits. Short form once the counts get long.
		 */
		snprintf(mid, sizeof(mid), "%u", soak.rx_ok);
		snprintf(bottom, sizeof(bottom), "miss %u dup %u", soak.missed,
			 soak.dup);
		if (strlen(bottom) > 12) {
			snprintf(bottom, sizeof(bottom), "m%u d%u", soak.missed,
				 soak.dup);
		}
		break;
	case SP_TX:
		snprintf(mid, sizeof(mid), "%u", t->tx_done - soak.snap.tx_done);
		snprintf(bottom, sizeof(bottom), "stale %u",
			 t->stale_retx - soak.snap.stale_retx);
		break;
	case SP_SYNC:
		snprintf(mid, sizeof(mid), "%+dus", (int)t->last_phase_err_us);
		snprintf(bottom, sizeof(bottom), "ppm %d", (int)t->last_ppm);
		break;
	case SP_LINK:
		if (soak.rx_ok) {
			snprintf(mid, sizeof(mid), "%ddBm", soak.last_rssi);
			snprintf(bottom, sizeof(bottom), "snr %d dB",
				 soak.last_snr);
		} else {
			snprintf(mid, sizeof(mid), "-");
			snprintf(bottom, sizeof(bottom), "no packets");
		}
		break;
	case SP_LOG:
		soak_log_get_status(&ls);
		if (ls.sd && ls.err) {
			/* disk = card never came up, mount = volume rejected,
			 * open = file creation failed, write/close = the card
			 * stopped answering mid-run.
			 */
			snprintf(mid, sizeof(mid), "LOG FAIL");
			snprintf(bottom, sizeof(bottom), "%s %d", ls.err_stage,
				 ls.err);
		} else if (IS_ENABLED(CONFIG_SOAK_RTT) &&
			   (ls.path[0] ||
			    (!ls.sd && (ls.active || ls.rtt_written)))) {
			/* Card file, or "RTT" for a run logged to the host
			 * only. "d" = ring drops, "r" = RTT drops; r climbs
			 * harmlessly whenever no capture is attached.
			 */
			if (ls.path[0]) {
				file_stem(mid, sizeof(mid), ls.path);
			} else {
				snprintf(mid, sizeof(mid), "RTT");
			}
			snprintf(bottom, sizeof(bottom), "%u d%u r%u",
				 ls.path[0] ? ls.written : ls.rtt_written,
				 ls.dropped, ls.rtt_dropped);
		} else if (ls.path[0]) {
			file_stem(mid, sizeof(mid), ls.path);
			/* "d" non-zero: the card fell behind and the run's
			 * record is incomplete.
			 */
			snprintf(bottom, sizeof(bottom), "%u rec d%u",
				 ls.written, ls.dropped);
		} else if (ls.active) {
			/* Mount/open runs on the writer thread. */
			snprintf(mid, sizeof(mid), "...");
			snprintf(bottom, sizeof(bottom), "opening");
		} else {
			snprintf(mid, sizeof(mid), "OFF");
		}
		break;
	case SP_LAT:
	default:
		/* Worst data-thread pass-to-pass latency against the staging
		 * budget. Under it, staging cannot be late.
		 */
		snprintf(mid, sizeof(mid), "%u/%u", soak.tick_gap_max_ms,
			 budget_ms);
		snprintf(bottom, sizeof(bottom), "%s",
			 (soak.tick_gap_max_ms >= budget_ms) ? "OVER BUDGET"
							     : "ms of budget");
		break;
	}

	draw_card(page_name[soak_page], top_r, mid, bottom);
}

static void draw_sd(void)
{
	static const char *const page_name[SDP_COUNT] = {
		[SDP_FILES] = "FILES",
		[SDP_FREE] = "FREE",
		[SDP_DET] = "DETECT",
	};
	char top_r[24], mid[24], bottom[SD_FSOP_NAME_MAX], fr[12];

	if (sd_busy) {
		snprintf(top_r, sizeof(top_r), "scan");
	} else {
		snprintf(top_r, sizeof(top_r), "%d/%d", sd_page + 1, SDP_COUNT);
	}
	bottom[0] = '\0';

	switch (sd_page) {
	case SDP_FILES:
		if (sd_res == SD_RES_NONE) {
			snprintf(mid, sizeof(mid), "-");
			snprintf(bottom, sizeof(bottom), "OK = scan");
		} else if (sd_res < 0) {
			snprintf(mid, sizeof(mid), "ERR");
			snprintf(bottom, sizeof(bottom), "%s %d",
				 sd_stage_short(), sd_res);
		} else {
			const char *nw = sd_newest();

			/* .BIN count, and the newest log under it. */
			snprintf(mid, sizeof(mid), "%u", sd_sum.bin_count);
			if (nw[0]) {
				file_stem(bottom, sizeof(bottom), nw);
			} else {
				snprintf(bottom, sizeof(bottom), "no logs");
			}
		}
		break;
	case SDP_FREE:
		if (sd_res == 0 && sd_sum.vfs_err == 0) {
			fmt_mib(mid, sizeof(mid), sd_sum.free_mib);
			fmt_mib(fr, sizeof(fr), sd_sum.total_mib);
			snprintf(bottom, sizeof(bottom), "of %s", fr);
		} else {
			snprintf(mid, sizeof(mid), "?");
			if (sd_res == 0) {
				snprintf(bottom, sizeof(bottom), "statvfs %d",
					 sd_sum.vfs_err);
			}
		}
		break;
	case SDP_DET:
	default:
		/* Active-low, confirmed on the bench: raw 0 = card seated. */
		if (det_ok && det_raw >= 0) {
			snprintf(mid, sizeof(mid), "%s",
				 det_logical ? "CARD IN" : "NO CARD");
			snprintf(bottom, sizeof(bottom), "raw %d", det_raw);
		} else {
			snprintf(mid, sizeof(mid), "N/A");
		}
		break;
	}

	draw_card(page_name[sd_page], top_r, mid, bottom);
}

static void draw_current(void)
{
	switch (screen) {
	case SCR_ROLE_PICK:
		draw_role_pick();
		break;
	case SCR_HOME:
		draw_home();
		break;
	case SCR_SOAK_PICK:
		draw_soak_pick();
		break;
	case SCR_SOAK:
		draw_soak();
		break;
	case SCR_SD:
		draw_sd();
		break;
	default:
		break;
	}
}

/* ---------------------------------------------------------------------------
 * Event routing
 * ------------------------------------------------------------------------- */
static void tx_power_step(int delta)
{
	int v = CLAMP(tx_power_dbm + delta, TDMA_TX_POWER_MIN_DBM,
		      TDMA_TX_POWER_MAX_DBM);

	/* Latched by the engine and applied before its next transmit, so
	 * this is "stored for the next soak" whether or not one has run.
	 */
	if (v != tx_power_dbm && tdma_set_tx_power((int8_t)v) == 0) {
		tx_power_dbm = v;
	}
}

/* UP/DOWN on a cycle of n items: wraps both ways. */
static int cycle(int i, int n, enum nav_action a)
{
	return (a == NAV_UP) ? (i + n - 1) % n : (i + 1) % n;
}

static void handle_nav(enum nav_action a)
{
	bool updown = (a == NAV_UP || a == NAV_DOWN);

	switch (screen) {
	case SCR_ROLE_PICK:
		/* No BACK: every option is valid, so there is nothing to
		 * cancel to — one of them must be chosen.
		 */
		if (updown) {
			role_sel = cycle(role_sel, TDMA_SLOT_COUNT, a);
		} else if (a == NAV_OK) {
			my_slot = (uint8_t)role_sel;
			role = (my_slot == 0) ? TDMA_ROLE_MASTER
					      : TDMA_ROLE_SECONDARY;
			LOG_INF("unit: %s (slot %u)", role_str(), my_slot);
			screen = SCR_HOME;
		}
		break;

	case SCR_HOME:
		if (tx_edit) {
			if (a == NAV_UP) {
				tx_power_step(1);
			} else if (a == NAV_DOWN) {
				tx_power_step(-1);
			} else {
				tx_edit = false;
				LOG_INF("TX power %+d dBm", tx_power_dbm);
			}
			break;
		}
		if (updown) {
			home_sel = cycle(home_sel, HR_COUNT, a);
		} else if (a == NAV_OK) {
			home_msg[0] = '\0';
			if (home_sel == HR_SOAK) {
				pick_sel = 0;
				pick_msg[0] = '\0';
				screen = SCR_SOAK_PICK;
			} else if (home_sel == HR_POWER) {
				tx_edit = true;
			} else {
				sd_page = SDP_FILES;
				screen = SCR_SD;
			}
		}
		break;

	case SCR_SOAK_PICK:
		if (updown) {
			pick_sel = cycle(pick_sel, PICK_COUNT, a);
			pick_msg[0] = '\0';
		} else if (a == NAV_OK) {
			/*
			 * A card scan still on the writer thread would delay
			 * the log's open (same thread) while records queue in
			 * the ring. Scans are short, so wait them out rather
			 * than start a run that may drop its first records.
			 */
			if (sd_busy) {
				snprintf(pick_msg, sizeof(pick_msg), "SD busy");
			} else {
				soak_begin(pick_sel);
			}
		} else if (a == NAV_BACK) {
			screen = SCR_HOME;
		}
		break;

	case SCR_SOAK:
		if (updown) {
			soak_page = cycle(soak_page, SP_COUNT, a);
		} else if (a == NAV_BACK) {
			if (soak.active) {
				soak_end("B4");
			} else {
				screen = SCR_HOME;
			}
		} else if (a == NAV_OK && !soak.active) {
			screen = SCR_HOME;
		}
		break;

	case SCR_SD:
		if (updown) {
			sd_page = cycle(sd_page, SDP_COUNT, a);
		} else if (a == NAV_OK) {
			sd_request_scan();
		} else if (a == NAV_BACK) {
			screen = SCR_HOME;
		}
		break;

	default:
		break;
	}
}

/* ---------------------------------------------------------------------------
 * main
 * ------------------------------------------------------------------------- */
int main(void)
{
	LOG_INF("TDMA field unit: rev 2 shield (SSD1306 + microSD + SX1262)");

	/* Engine default is +22 dBm; this variant boots at +0. Latched now,
	 * applied before the first transmit of the first soak.
	 */
	(void)tdma_set_tx_power(FIELD_TX_POWER_DBM);

	det_init();
	det_poll();	/* log the boot level (bench test step 3) */

	/* tdma_init() is deferred to the first soak start so the unit can be
	 * chosen first.
	 */
	if (!display_init()) {
		/* Nothing to drive without the OLED; the reason is on UART. */
		k_sleep(K_FOREVER);
	}

	draw_splash();
	k_msleep(SPLASH_MS);

	/* Power-on unit pick. It starts on the fail-safe default (SEC 1), so
	 * OK without moving never makes a second master.
	 */
	role_sel = my_slot;
	screen = SCR_ROLE_PICK;
	draw_current();

	enum screen_id last = screen;

	while (1) {
		enum nav_action a = atomic_set(&nav_event, NAV_NONE);

		if (a != NAV_NONE) {
			handle_nav(a);
		}

		/*
		 * The data plane runs on soak_data_tid, not here. All the UI
		 * owes it is the teardown, which blocks on the card writer and
		 * so cannot run on that thread.
		 */
		if (soak.expired) {
			soak_end("limit");
		}

#ifdef CONFIG_SOAK_RTT
		rtt_link_service(&rtt_ops);
#endif
		sd_poll();
		det_poll();

		bool changed = (screen != last);

		/* Every entry to HOME rescans the card, so the header shows
		 * what is on it now (including the log a run just closed).
		 */
		if (changed && screen == SCR_HOME) {
			tx_edit = false;
			sd_request_scan();
		}

		bool dirty = atomic_cas(&screen_dirty, 1, 0);

		if (a != NAV_NONE || dirty || changed) {
			draw_current();
			last = screen;
		}

		/* Live-refresh the soak stats while the screen is up. */
		if (screen == SCR_SOAK && soak.active) {
			int64_t now = k_uptime_get();

			if (now - soak_last_draw_ms >= SOAK_DRAW_PERIOD_MS) {
				soak_last_draw_ms = now;
				draw_current();
			}
		}

		k_sleep(K_MSEC(20));
	}

	return 0;
}
