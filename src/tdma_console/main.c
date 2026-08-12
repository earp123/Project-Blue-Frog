/*
 * Intercom TDMA field console (CONFIG_APP_TDMA_CONSOLE) — variant entry point.
 *
 * Field-test container UI for the TDMA radio layer (src/tdma): run timed soak
 * tests of the continuous TX/RX frame exchange and watch link telemetry live
 * on the TFT. Reuses the telemetry console's display toolkit (ui_widgets) and
 * touch calibration (touch_cal); the radio path is exclusively the TDMA
 * L1/L2 shim — the native driver is init-only, exactly as in the TDMA test
 * variant. This file is only compiled for CONFIG_APP_TDMA_CONSOLE.
 *
 * Display-only build (no UART / log / shell). One firmware image serves both
 * units: the role is picked on the HOME screen and locks at the first soak
 * start (tdma_init() is once-only) — reboot to change it. Every PHY
 * parameter is a compile-time constant in tdma.h except TX power, which is
 * adjustable from HOME (applied by the engine before the next transmit).
 *
 * Power-on sequence, both steps mandatory and session-only: TOUCH CAL (the
 * transform is RAM-only in touch_cal, so every boot re-runs it) then SELECT
 * ROLE. Calibration comes first so the role can be chosen by touch. HOME is
 * unreachable until both are done, so no soak can run uncalibrated or on an
 * unset role. Neither is reachable from HOME afterwards — reboot to redo
 * either; the selected role is shown in the HOME header.
 *
 * Screens: HOME -> {SOAK PICK -> SOAK, TX PWR keypad, FILES}. The soak
 * screen shows per-soak deltas of the engine telemetry plus frame-counter
 * continuity stats (received / missed / duplicate peer frames), which catch
 * losses the CRC counters cannot.
 *
 * FILES is a minimal SD browser: list, delete (two-press confirm), move
 * between the fixed top-level drawers (DRAWER0-3, created on demand), and an
 * optional label appended to a filename via the alpha keypad. The soak-pick
 * screen selects which drawer (or the root) new logs land in. All card I/O
 * is submitted to the soak_log writer thread and polled, never blocking the
 * UI; log files are named <PWR>_<DUR>_NNN.BIN (e.g. M9_5M_000.BIN).
 *
 * Every soak also writes a binary record log to the microSD card (soak_log ->
 * sd_log): one 64-byte record per received packet — payload included — plus
 * TX and periodic counter records. Logging is best-effort and never aborts a
 * run; the soak screen carries the file name, record count and drop count.
 * Decode with tools/decode_soak_log.py.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/input/input.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/util.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include "ui_widgets.h"
#include "touch_cal.h"
#include "tdma.h"
#include "soak_log.h"

/* ---- Layout ----
 * Portrait, 240x320 (see the rotation note in the display overlay). Rows use
 * fixed heights rather than dividing the body: portrait leaves 272 px of body,
 * and splitting that between two menu rows would produce 130 px-tall buttons.
 * Text is 10 px/char, so a full-width line is 23 characters at MARG.
 */
#define BORDER_PX 3
#define MARG 4

#define ROW_H	 56	/* HOME menu row */
#define ROW_GAP	 10
#define FLIP_W	120	/* the small 180-deg view-flip button */
#define FLIP_H	 34
#define PICK_H	 38	/* soak-pick rows (log-dir + durations + BACK) */
#define PICK_GAP  8
#define ROLE_H	 90	/* role-select buttons */
#define ROLE_GAP 14
#define BR_ROW_H 22	/* file-browser list row */
#define ACT_H	 44	/* stacked action buttons (file act / move) */
#define ACT_GAP	  8

/* ---- Navigation (posted by the button callback, consumed in main) ---- */
enum nav_action {
	NAV_NONE = 0,
	NAV_UP,
	NAV_DOWN,
	NAV_OK,
	NAV_BACK,
};
static atomic_t nav_event = ATOMIC_INIT(NAV_NONE);

/* ---- Touch tap (debounced; raw latched in the callback, mapped in main) ---- */
static volatile int touch_x, touch_y;
static volatile bool touch_down;
static volatile bool touch_latched;
static volatile int tap_raw_x, tap_raw_y;
static atomic_t tap_ready = ATOMIC_INIT(0);

/* ---- Screen state machine ---- */
enum screen_id {
	SCR_HOME = 0,
	SCR_SOAK_PICK,
	SCR_SOAK,
	SCR_KEYPAD,	/* TX power / label entry */
	SCR_CALIBRATE,
	SCR_ROLE_PICK,	/* power-on role selection */
	SCR_FILES,	/* SD file browser: list + new-drawer */
	SCR_FILE_ACT,	/* per-file actions: delete / move / label */
	SCR_FILE_MOVE,	/* move destination pick */
};
static enum screen_id screen = SCR_HOME;

/*
 * Touch calibration is a mandatory power-on step: the transform lives in RAM
 * only (touch_cal), so every boot starts from the identity mapping and taps
 * would land nowhere useful until it is run. main() forces the CAL screen
 * before anything else and this flag blocks cancelling out of it; it clears
 * once a fit is accepted. Session-only by design — no flash persistence.
 */
static bool cal_required = true;

/*
 * Role selection is the second half of the power-on sequence, run once
 * calibration is accepted: with a working transform the choice can be made by
 * touch. Like the calibration pass there is no way out but choosing — HOME is
 * unreachable until this clears, so no soak can start on an unset role. The
 * HOME row still re-toggles the role afterwards, up until the engine locks it.
 */
static bool role_required = true;
static int role_sel;

/* ---- Log destination + file browser state ----
 * dir index 0 is the card root; 1..SD_DRAWERS are the fixed drawer names. The
 * soak logger points at log_dir_idx (selectable on the soak-pick screen); a
 * missing drawer is created when a log actually opens there.
 */
#define SD_DRAWERS 4
static const char *const drawer_name[SD_DRAWERS + 1] = {
	"", "DRAWER0", "DRAWER1", "DRAWER2", "DRAWER3",
};
static int log_dir_idx;			/* 0 = "/" */

#define BR_LIST_MAX 64
static struct sd_dirent br_ents[BR_LIST_MAX];
static int br_count;
static int br_dir;			/* browsed dir, same indexing */
static int br_sel;
static int br_scroll;
static bool br_busy;			/* fs op in flight on the writer */
static bool br_truncated;		/* directory had > BR_LIST_MAX entries */
static char br_msg[24];			/* footer status / error */
static enum sd_fsop_op br_op;		/* op awaiting completion */
static char fa_name[SD_FSOP_NAME_MAX];	/* file the action menu targets */
static int fa_sel;
static bool fa_confirm_del;
static int mv_sel;

/* What the keypad edits when SCR_KEYPAD closes. */
static enum { KP_FOR_POWER, KP_FOR_LABEL } kp_purpose;

static int home_sel;
static int pick_sel;
static char home_msg[28] = "";

/* ---- Engine / role state ---- */
static enum tdma_role role = TDMA_ROLE_SECONDARY; /* fail-safe default: two
						   * unset units just listen
						   * instead of colliding. */
static bool engine_inited;	/* role locks once true */
static int tx_power_dbm = TDMA_TX_POWER_DBM;

/* ---- Soak test state (all owned by the main loop) ---- */
static const struct {
	const char *label;
	int64_t ms;		/* 0 = run until STOP */
} soak_durations[] = {
	{ "5 MIN", 5 * 60 * 1000LL },
	{ "30 MIN", 30 * 60 * 1000LL },
	{ "2 HOURS", 120 * 60 * 1000LL },
	{ "CONTINUOUS", 0 },
};
/* Rows on the pick screen: log-dir selector, durations, BACK. */
#define PICK_COUNT (ARRAY_SIZE(soak_durations) + 2)
#define PICK_ROW_DIR  0
#define PICK_ROW_BACK ((int)PICK_COUNT - 1)

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
	 * stage buffer is free — several times per frame at the 20 ms UI
	 * cadence — but only one payload per frame is actually sent; the rest
	 * are overwritten in place. We therefore hold a payload "armed" and
	 * only log it once the engine's tx_done confirms it went out.
	 */
	bool tx_armed;
	uint8_t tx_payload[TDMA_PAYLOAD_LEN];
	uint32_t tx_done_at_arm;
} soak;

static int64_t soak_last_draw_ms;

/* Counter-snapshot cadence: one STATS record per second is negligible against
 * the packet stream but bounds how stale the counters are if a run is cut off.
 */
#define SOAK_STATS_PERIOD_MS 1000
static int64_t soak_last_stats_ms;

/* Set by any thread to ask main to redraw. */
static atomic_t screen_dirty = ATOMIC_INIT(0);

static void mark_dirty(void)
{
	atomic_set(&screen_dirty, 1);
}

/* ---------------------------------------------------------------------------
 * Input callbacks (input thread context; post events only, never draw)
 * ------------------------------------------------------------------------- */
static void touch_cb(struct input_event *evt, void *user_data)
{
	ARG_UNUSED(user_data);

	switch (evt->code) {
	case INPUT_ABS_X:
		touch_x = evt->value;
		break;
	case INPUT_ABS_Y:
		touch_y = evt->value;
		break;
	case INPUT_BTN_TOUCH:
		touch_down = evt->value != 0;
		break;
	default:
		break;
	}

	if (!evt->sync) {
		return;
	}

	if (touch_down && !touch_latched) {
		touch_latched = true;
		tap_raw_x = touch_x;
		tap_raw_y = touch_y;
		atomic_set(&tap_ready, 1);
	} else if (!touch_down) {
		touch_latched = false;
	}
}
INPUT_CALLBACK_DEFINE(DEVICE_DT_GET(DT_CHOSEN(zephyr_touch)), touch_cb, NULL);

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
 * Geometry / chrome helpers
 * ------------------------------------------------------------------------- */
static int body_top(void)
{
	return MARG + ui_body_h() + 4;
}

static int body_bot(void)
{
	return ui_disp_h() - MARG - ui_body_h() - 4;
}

static void draw_border(void)
{
	int w = ui_disp_w();
	int h = ui_disp_h();

	ui_fill_rect(0, 0, w, BORDER_PX, COLOR_WHITE);
	ui_fill_rect(0, h - BORDER_PX, w, BORDER_PX, COLOR_WHITE);
	ui_fill_rect(0, 0, BORDER_PX, h, COLOR_WHITE);
	ui_fill_rect(w - BORDER_PX, 0, BORDER_PX, h, COLOR_WHITE);
}

static void draw_header(const char *title)
{
	int w = ui_disp_w();
	int bh = ui_body_h();

	ui_fill_rect(BORDER_PX, MARG, w - 2 * BORDER_PX, bh, COLOR_BLACK);
	ui_text(MARG, MARG, title, COLOR_WHITE, COLOR_BLACK);
}

static void draw_footer_text(const char *s)
{
	int w = ui_disp_w();
	int bh = ui_body_h();
	int fy = ui_disp_h() - MARG - bh;

	ui_fill_rect(BORDER_PX, fy, w - 2 * BORDER_PX, bh, COLOR_BLACK);
	ui_text(MARG, fy, s, COLOR_WHITE, COLOR_BLACK);
}

/* Fixed PHY summary + the one adjustable knob. */
static void draw_footer_summary(void)
{
	char s[28];

	snprintf(s, sizeof(s), "915MHz SF5/500k %+ddBm", tx_power_dbm);
	draw_footer_text(s);
}

static const char *role_str(void)
{
	return (role == TDMA_ROLE_MASTER) ? "MASTER" : "SECONDARY";
}

/* h:mm:ss, always sortable at a glance in the field (hours wrap at 99). */
static void fmt_dur(char *out, size_t n, int64_t ms)
{
	unsigned s = (unsigned)(ms / 1000);

	snprintf(out, n, "%u:%02u:%02u", (s / 3600) % 100, (s / 60) % 60,
		 s % 60);
}

/* ---------------------------------------------------------------------------
 * HOME
 * ------------------------------------------------------------------------- */
/*
 * HOME is deliberately minimal. Touch calibration and role selection are
 * power-on steps only (see the sequence at the top of this file) — reboot to
 * redo either. TX power stays because it is the one PHY parameter meant to be
 * swept from the bench, and is where further run parameters will land.
 */
enum home_row {
	HR_SOAK = 0,
	HR_POWER,
	HR_FILES,
	HR_FLIP,	/* small button, sits apart from the menu rows */
	HR_COUNT,
};

static void home_row_rect(int i, int *x, int *y, int *w, int *h)
{
	if (i == HR_FLIP) {
		/* Deliberately small and set apart at the foot of the body:
		 * it changes how the screen is held, not what the test does.
		 */
		*w = FLIP_W;
		*h = FLIP_H;
		*x = (ui_disp_w() - FLIP_W) / 2;
		*y = body_bot() - FLIP_H;
		return;
	}

	*x = MARG;
	*w = ui_disp_w() - 2 * MARG;
	*h = ROW_H;
	*y = body_top() + i * (ROW_H + ROW_GAP);
}

static void draw_home(void)
{
	char val[16];
	char hdr[28];

	/* Role moved out of the menu, so carry it in the header: with two
	 * identical units on the bench it is the one thing you cannot infer by
	 * looking at them.
	 */
	snprintf(hdr, sizeof(hdr), "FIELD TEST: %s", role_str());
	draw_header(hdr);

	for (int i = 0; i < HR_COUNT; i++) {
		int x, y, w, h;

		home_row_rect(i, &x, &y, &w, &h);

		switch (i) {
		case HR_SOAK:
			ui_button(x, y, w, h, "SOAK TEST", home_sel == i);
			break;
		case HR_POWER:
			snprintf(val, sizeof(val), "%+d dBm", tx_power_dbm);
			ui_value_row(x, y, w, h, "TX pwr", val, false,
				     home_sel == i, false);
			break;
		case HR_FILES:
			ui_button(x, y, w, h, "FILES", home_sel == i);
			break;
		case HR_FLIP:
			ui_button(x, y, w, h, ui_flipped() ? "FLIP ^" : "FLIP v",
				  home_sel == i);
			break;
		default:
			break;
		}
	}

	if (home_msg[0]) {
		draw_footer_text(home_msg);
	} else {
		draw_footer_summary();
	}
}

/* ---------------------------------------------------------------------------
 * SOAK PICK (duration buttons)
 * ------------------------------------------------------------------------- */
static void pick_btn_rect(int i, int *x, int *y, int *w, int *h)
{
	int stack = (int)PICK_COUNT * PICK_H + ((int)PICK_COUNT - 1) * PICK_GAP;
	int top = body_top() + (body_bot() - body_top() - stack) / 2;

	*x = MARG;
	*w = ui_disp_w() - 2 * MARG;
	*h = PICK_H;
	*y = top + i * (PICK_H + PICK_GAP);
}

/* "/" or the drawer name, for the selector row and browser headers. */
static const char *dir_label(int idx)
{
	return (idx == 0) ? "/" : drawer_name[idx];
}

static void draw_soak_pick(void)
{
	char hdr[28];

	snprintf(hdr, sizeof(hdr), "SOAK: %s", role_str());
	draw_header(hdr);

	for (int i = 0; i < (int)PICK_COUNT; i++) {
		int x, y, w, h;

		pick_btn_rect(i, &x, &y, &w, &h);
		if (i == PICK_ROW_DIR) {
			ui_value_row(x, y, w, h, "LOG",
				     dir_label(log_dir_idx), true,
				     pick_sel == i, false);
		} else if (i == PICK_ROW_BACK) {
			ui_button(x, y, w, h, "BACK", pick_sel == i);
		} else {
			ui_button(x, y, w, h, soak_durations[i - 1].label,
				  pick_sel == i);
		}
	}

	draw_footer_summary();
}

/* ---------------------------------------------------------------------------
 * SOAK (live stats)
 * ------------------------------------------------------------------------- */
static void soak_btn_rect(int *x, int *y, int *w, int *h)
{
	*h = ui_body_h() + 10;
	*x = MARG;
	*w = ui_disp_w() - 2 * MARG;
	*y = body_bot() - *h;
}

/* Stats lines + action button: everything below the header is dynamic. */
static void draw_soak_dynamic(void)
{
	const struct tdma_telemetry *t = tdma_get_telemetry();
	int bh = ui_body_h();
	int sp = bh + 2;
	int y = body_top();
	int w = ui_disp_w();
	char l[40], e[12], d[12];
	int bx, by, bw, bbh;

	soak_btn_rect(&bx, &by, &bw, &bbh);
	ui_fill_rect(BORDER_PX, y, w - 2 * BORDER_PX, by - y, COLOR_BLACK);

	static const char *const state_str[] = {
		[TDMA_SYNC_STOPPED] = "STOPPED",
		[TDMA_SYNC_SYNCING] = "SYNCING",
		[TDMA_SYNC_RUNNING] = "RUNNING",
	};
	const char *st = soak.done ? "DONE"
				   : state_str[t->sync_state %
					       ARRAY_SIZE(state_str)];

	snprintf(l, sizeof(l), "%s  %+ddBm", st, tx_power_dbm);
	ui_text(MARG, y, l, COLOR_WHITE, COLOR_BLACK);
	y += sp;

	int64_t elapsed = soak.active || soak.done
				  ? (soak.done ? soak_last_draw_ms
					       : k_uptime_get()) - soak.start_ms
				  : 0;

	fmt_dur(e, sizeof(e), elapsed);
	if (soak.duration_ms) {
		fmt_dur(d, sizeof(d), soak.duration_ms);
		snprintf(l, sizeof(l), "t %s / %s", e, d);
	} else {
		snprintf(l, sizeof(l), "t %s / inf", e);
	}
	ui_text(MARG, y, l, COLOR_WHITE, COLOR_BLACK);
	y += sp;

	/* Per-soak deltas against the start-of-soak snapshot. */
	snprintf(l, sizeof(l), "tx %u  stale %u",
		 t->tx_done - soak.snap.tx_done,
		 t->stale_retx - soak.snap.stale_retx);
	ui_text(MARG, y, l, COLOR_WHITE, COLOR_BLACK);
	y += sp;

	/* Split across lines rather than packed: a 240 px row holds 23
	 * characters, and large counters overran the combined form.
	 */
	snprintf(l, sizeof(l), "rx %u  crc %u", soak.rx_ok,
		 t->rx_crc_err - soak.snap.rx_crc_err);
	ui_text(MARG, y, l, COLOR_WHITE, COLOR_BLACK);
	y += sp;

	snprintf(l, sizeof(l), "bad hdr %u",
		 t->rx_bad_header - soak.snap.rx_bad_header);
	ui_text(MARG, y, l, COLOR_WHITE, COLOR_BLACK);
	y += sp;

	snprintf(l, sizeof(l), "miss %u  dup %u", soak.missed, soak.dup);
	ui_text(MARG, y, l, COLOR_WHITE, COLOR_BLACK);
	y += sp;

	snprintf(l, sizeof(l), "ph %dus  ppm %d",
		 t->last_phase_err_us, t->last_ppm);
	ui_text(MARG, y, l, COLOR_WHITE, COLOR_BLACK);
	y += sp;

	/* Own boundary->TxDone dt, and the peer slot's boundary->RxDone dt
	 * (2-unit kit: master watches slot 1, secondary watches slot 0).
	 * Both are L + TOA; see the tdma_telemetry comment in tdma.h.
	 */
	snprintf(l, sizeof(l), "dtx %u  drx %u", t->tx_evt_dt_us,
		 t->dt_by_slot[(role == TDMA_ROLE_MASTER) ? 1 : 0]);
	ui_text(MARG, y, l, COLOR_WHITE, COLOR_BLACK);
	y += sp;

	if (soak.rx_ok) {
		snprintf(l, sizeof(l), "rssi %d  snr %d",
			 soak.last_rssi, soak.last_snr);
	} else {
		snprintf(l, sizeof(l), "rssi -  snr -");
	}
	ui_text(MARG, y, l, COLOR_WHITE, COLOR_BLACK);
	y += sp;

	/* Fatal-fault line: BUSY timeouts stop the engine. */
	uint32_t busy = t->busy_timeouts - soak.snap.busy_timeouts;

	if (busy) {
		snprintf(l, sizeof(l), "** BUSY FAULT x%u **", busy);
	} else {
		snprintf(l, sizeof(l), "busy 0");
	}
	ui_text(MARG, y, l, COLOR_WHITE, COLOR_BLACK);
	y += sp;

	/* Log line: file, records on disk, and any loss. "drop" non-zero means
	 * the card could not keep up and the run's record is incomplete.
	 */
	struct soak_log_status ls;

	soak_log_get_status(&ls);
	if (ls.err) {
		/* Stage says where: disk = card never came up, mount = volume
		 * rejected (format), open = volume fine but file creation failed.
		 */
		snprintf(l, sizeof(l), "LOG FAIL %s %d", ls.err_stage, ls.err);
	} else if (ls.path[0]) {
		/* Basename only: with a drawer in it the full path no longer
		 * fits a 23-character portrait line.
		 */
		const char *base = strrchr(ls.path, '/');

		snprintf(l, sizeof(l), "%s %uk d%u",
			 (base != NULL) ? base + 1 : ls.path,
			 ls.written / 1000u, ls.dropped);
	} else if (ls.active) {
		/* Mount/open runs on the writer thread, so the file name only
		 * appears once the card has answered.
		 */
		snprintf(l, sizeof(l), "log opening...");
	} else {
		snprintf(l, sizeof(l), "log off");
	}
	ui_text(MARG, y, l, COLOR_WHITE, COLOR_BLACK);

	ui_button(bx, by, bw, bbh, soak.done ? "BACK" : "STOP", true);
}

static void draw_soak(void)
{
	char hdr[28];

	snprintf(hdr, sizeof(hdr), "SOAK: %s", role_str());
	draw_header(hdr);
	draw_soak_dynamic();
}

/* ---------------------------------------------------------------------------
 * Soak control + stats feed (main loop only)
 * ------------------------------------------------------------------------- */
static void fill_pattern(uint8_t payload[TDMA_PAYLOAD_LEN], uint8_t seq)
{
	for (int i = 0; i < TDMA_PAYLOAD_LEN; i++) {
		payload[i] = (uint8_t)(seq + i);
	}
}

static void soak_start(int64_t duration_ms)
{
	int rc;

	if (!engine_inited) {
		struct tdma_config cfg = {
			.role = role,
			/* Two-unit field kit: master beacons in slot 0, the
			 * secondary answers in slot 1.
			 */
			.slot_id = (role == TDMA_ROLE_MASTER) ? 0 : 1,
			.slot_duration_us = TDMA_SLOT_DURATION_US,
		};

		rc = tdma_init(&cfg);
		if (rc < 0) {
			snprintf(home_msg, sizeof(home_msg), "init err %d", rc);
			screen = SCR_HOME;
			return;
		}
		engine_inited = true; /* role is now locked */
	}

	memset(&soak, 0, sizeof(soak));
	soak.snap = *tdma_get_telemetry();

	/* Logging is best-effort: a missing or failed card must never stop a
	 * radio test. The failure is surfaced on the soak screen instead.
	 */
	(void)soak_log_start(role, (role == TDMA_ROLE_MASTER) ? 0 : 1,
			     (int8_t)tx_power_dbm, (uint32_t)duration_ms,
			     drawer_name[log_dir_idx]);

	rc = tdma_start();
	if (rc < 0) {
		snprintf(home_msg, sizeof(home_msg), "start err %d", rc);
		soak_log_stop();
		screen = SCR_HOME;
		return;
	}

	soak.active = true;
	soak.start_ms = k_uptime_get();
	soak.duration_ms = duration_ms;
	soak_last_draw_ms = 0;
	home_msg[0] = '\0';

	/*
	 * Baseline counter snapshot at t=0. The engine's counters are not
	 * reset by tdma_start(), so they carry over between soaks in one boot;
	 * with this record the decoder gets an exact per-run delta by
	 * differencing the first and last STATS instead of missing the first
	 * sample period.
	 */
	soak_last_stats_ms = k_uptime_get();
	soak_log_stats(tdma_get_telemetry());

	screen = SCR_SOAK;
}

static void soak_finish(void)
{
	tdma_stop();

	/* One last counter snapshot, then drain and close the log. */
	soak_log_stats(tdma_get_telemetry());
	soak_log_stop();

	soak.active = false;
	soak.done = true;
	soak_last_draw_ms = k_uptime_get();
	mark_dirty();
}

/* Drain received frames into the continuity stats. */
static void soak_poll(void)
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

	const struct tdma_telemetry *t = tdma_get_telemetry();

	/*
	 * An armed payload that the engine has now transmitted: log it as the
	 * transmission it actually was, then arm the next one. Submitting on
	 * every pass instead would overwrite most payloads before their slot.
	 */
	if (soak.tx_armed && t->tx_done != soak.tx_done_at_arm) {
		soak_log_tx(soak.tx_payload, (role == TDMA_ROLE_MASTER) ? 0 : 1,
			    t->sync_state);
		soak.tx_armed = false;
	}

	if (!soak.tx_armed) {
		fill_pattern(soak.tx_payload, soak.seq);
		if (tdma_tx_submit(soak.tx_payload) == 0) {
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

	if (soak.duration_ms &&
	    k_uptime_get() - soak.start_ms >= soak.duration_ms) {
		soak_finish();
	}
}

/* ---------------------------------------------------------------------------
 * TX power keypad
 * ------------------------------------------------------------------------- */
static void power_keypad_open(void)
{
	char init[8];

	snprintf(init, sizeof(init), "%d", tx_power_dbm);
	kp_purpose = KP_FOR_POWER;
	keypad_open(KEYPAD_DEC, "TX power dBm", init);
	screen = SCR_KEYPAD;
}

/* Implemented with the browser (needs its path helpers). */
static void label_apply(const char *label);

static void keypad_finish(enum keypad_result r)
{
	if (r == KEYPAD_PENDING) {
		return;
	}

	if (kp_purpose == KP_FOR_LABEL) {
		if (r == KEYPAD_OK && keypad_text()[0] != '\0') {
			label_apply(keypad_text());
		}
		screen = SCR_FILES;	/* cancel/empty: back to the list */
		mark_dirty();
		return;
	}

	if (r == KEYPAD_OK) {
		int v = (int)strtol(keypad_text(), NULL, 10);

		v = CLAMP(v, TDMA_TX_POWER_MIN_DBM, TDMA_TX_POWER_MAX_DBM);
		if (tdma_set_tx_power((int8_t)v) == 0) {
			tx_power_dbm = v;
		}
		mark_dirty();
	}

	screen = SCR_HOME;
}

/* ---------------------------------------------------------------------------
 * CALIBRATE (same flow as the telemetry console; touch_cal does the math)
 * ------------------------------------------------------------------------- */
#define CAL_POINTS 5

/* Crosshair arm length. Shared with the text layout below so the instruction
 * line can be placed clear of the targets instead of guessing.
 */
#define CAL_CROSS_R 12

enum cal_phase { CAL_COLLECT = 0, CAL_VERIFY, CAL_FAIL };
static int cal_phase;
static int cal_idx;
static int cal_rx[CAL_POINTS], cal_ry[CAL_POINTS]; /* raw samples */
static int cal_test_x = -1, cal_test_y = -1;	   /* last verify hit (mapped) */

/* Target crosshairs, inset so the controller never clamps at the edges. */
static void cal_target(int i, int *tx, int *ty)
{
	int w = ui_disp_w();
	int h = ui_disp_h();

	switch (i) {
	case 0: *tx = w * 15 / 100; *ty = h * 15 / 100; break; /* top-left */
	case 1: *tx = w * 85 / 100; *ty = h * 15 / 100; break; /* top-right */
	case 2: *tx = w * 85 / 100; *ty = h * 85 / 100; break; /* bottom-right */
	case 3: *tx = w * 15 / 100; *ty = h * 85 / 100; break; /* bottom-left */
	default: *tx = w / 2;       *ty = h / 2;        break; /* center */
	}
}

static void draw_cross(int x, int y, uint16_t color)
{
	int s = CAL_CROSS_R;

	ui_fill_rect(x - s, y - 1, 2 * s + 1, 3, color);
	ui_fill_rect(x - 1, y - s, 3, 2 * s + 1, color);
}

/*
 * Instruction-line baseline: just under the two top crosshairs. At the old
 * MARG+body+2 the line ran straight through the first (top-left) target,
 * which is the one target you are looking at when you read it. Derived from
 * the same 15 % inset cal_target() uses, so it stays correct if the panel
 * geometry changes.
 */
static int cal_instr_y(void)
{
	return ui_disp_h() * 15 / 100 + CAL_CROSS_R + 4;
}

/*
 * Verify-phase touch buttons. These exist only in CAL_VERIFY, and that is
 * deliberate: it is the one phase where a freshly solved transform is already
 * active, so a tap lands where it looks. In CAL_COLLECT the transform is still
 * the identity (taps are raw ADC counts) and in CAL_FAIL the solve was
 * rejected, so an on-screen button would be unpressable in both — the DK
 * buttons stay the only control there, and remain a working fallback here.
 */
enum cal_btn { CAL_BTN_REDO = 0, CAL_BTN_ACCEPT, CAL_BTN_COUNT };

static void cal_btn_rect(int i, int *x, int *y, int *w, int *h)
{
	int gap = 8;
	int total = ui_disp_w() - 2 * MARG;

	*h = ui_body_h() + 10;
	*w = (total - gap) / CAL_BTN_COUNT;
	*x = MARG + i * (*w + gap);
	*y = ui_disp_h() - MARG - *h;
}

static void cal_start(void)
{
	cal_phase = CAL_COLLECT;
	cal_idx = 0;
	cal_test_x = -1;
	cal_test_y = -1;
	screen = SCR_CALIBRATE;
}

static void draw_calibrate(void)
{
	int w = ui_disp_w();
	int h = ui_disp_h();
	int bh = ui_body_h();
	char l[40];

	ui_fill_rect(BORDER_PX, BORDER_PX, w - 2 * BORDER_PX, h - 2 * BORDER_PX,
		     COLOR_BLACK);

	if (cal_phase == CAL_COLLECT) {
		int tx, ty;

		cal_target(cal_idx, &tx, &ty);
		draw_cross(tx, ty, COLOR_WHITE);
		ui_text(MARG, MARG, cal_required ? "TOUCH CAL (required)" : "TOUCH CAL",
			COLOR_WHITE, COLOR_BLACK);
		snprintf(l, sizeof(l), "Tap the + (%d/%d)", cal_idx + 1, CAL_POINTS);
		ui_text(MARG, cal_instr_y(), l, COLOR_WHITE, COLOR_BLACK);
		ui_text(MARG, h - MARG - bh,
			cal_required ? "required at power-on" : "B4 = cancel",
			COLOR_WHITE, COLOR_BLACK);
	} else if (cal_phase == CAL_VERIFY) {
		int tx, ty, bx, by, bw, bbh;

		cal_target(4, &tx, &ty);
		draw_cross(tx, ty, COLOR_WHITE);
		if (cal_test_x >= 0) {
			ui_fill_rect(cal_test_x - 3, cal_test_y - 3, 7, 7, COLOR_GREY);
		}
		ui_text(MARG, MARG, "TOUCH CAL: verify", COLOR_WHITE, COLOR_BLACK);
		ui_text(MARG, cal_instr_y(), "Tap +; box = hit",
			COLOR_WHITE, COLOR_BLACK);

		cal_btn_rect(CAL_BTN_REDO, &bx, &by, &bw, &bbh);
		/* Hint sits above the buttons, clear of both them and the
		 * centre target.
		 */
		ui_text(MARG, by - bh - 4, "or B3=accept B4=redo",
			COLOR_WHITE, COLOR_BLACK);
		ui_button(bx, by, bw, bbh, "REDO", false);

		cal_btn_rect(CAL_BTN_ACCEPT, &bx, &by, &bw, &bbh);
		/* ACCEPT highlighted as the primary action; tapping it
		 * accurately is itself the proof the fit is good.
		 */
		ui_button(bx, by, bw, bbh, "ACCEPT", true);
	} else { /* CAL_FAIL */
		ui_text(MARG, MARG, "TOUCH CAL", COLOR_WHITE, COLOR_BLACK);
		ui_text(MARG, MARG + bh + 2, "Failed - tap evenly",
			COLOR_WHITE, COLOR_BLACK);
		ui_text(MARG, h - MARG - bh,
			cal_required ? "B3 = retry" : "B3=retry B4=cancel",
			COLOR_WHITE, COLOR_BLACK);
	}
}

/* ---------------------------------------------------------------------------
 * ROLE PICK (power-on, immediately after calibration)
 * ------------------------------------------------------------------------- */
#define ROLE_BLURB_LINES 2

static void role_btn_rect(int i, int *x, int *y, int *w, int *h)
{
	int blurb = body_top() + ROLE_BLURB_LINES * (ui_body_h() + 2) + 6;
	int stack = 2 * ROLE_H + ROLE_GAP;
	int top = blurb + (body_bot() - blurb - stack) / 2;

	*x = MARG;
	*w = ui_disp_w() - 2 * MARG;
	*h = ROLE_H;
	*y = top + i * (ROLE_H + ROLE_GAP);
}

static void draw_role_pick(void)
{
	int y = body_top();
	int sp = ui_body_h() + 2;

	draw_header("SELECT ROLE");

	ui_text(MARG, y, "One unit MASTER, the", COLOR_WHITE, COLOR_BLACK);
	y += sp;
	ui_text(MARG, y, "other SECONDARY.", COLOR_WHITE, COLOR_BLACK);

	for (int i = 0; i < 2; i++) {
		int bx, by, bw, bh;

		role_btn_rect(i, &bx, &by, &bw, &bh);
		ui_button(bx, by, bw, bh,
			  i == 0 ? "MASTER" : "SECONDARY", role_sel == i);
	}

	draw_footer_summary();
}

static void role_pick_start(void)
{
	/* Start on the current role, which defaults to SECONDARY: pressing OK
	 * without moving therefore takes the fail-safe option rather than
	 * making a second master.
	 */
	role_sel = (role == TDMA_ROLE_MASTER) ? 0 : 1;
	screen = SCR_ROLE_PICK;
}

static void role_activate(int i)
{
	role = (i == 0) ? TDMA_ROLE_MASTER : TDMA_ROLE_SECONDARY;
	role_required = false;
	screen = SCR_HOME;
}

/* Calibration accepted: hand off to role selection on the power-on pass,
 * otherwise straight back to HOME (a re-calibration started from HOME).
 */
static void cal_accept(void)
{
	cal_required = false;
	if (role_required) {
		role_pick_start();
	} else {
		screen = SCR_HOME;
	}
}

/* ---------------------------------------------------------------------------
 * FILES (SD browser). Every card op runs on the soak_log writer thread; this
 * screen only submits and polls, so a sick card can never stall the UI.
 * ------------------------------------------------------------------------- */

/* Full VFS path of name inside drawer dir_idx (0 = root). */
static void dir_path(char *out, size_t n, int dir_idx, const char *name)
{
	if (dir_idx == 0) {
		snprintf(out, n, SD_MOUNT_POINT "/%s", name);
	} else {
		snprintf(out, n, SD_MOUNT_POINT "/%s/%s",
			 drawer_name[dir_idx], name);
	}
}

static void br_submit(enum sd_fsop_op op, const char *a, const char *b)
{
	if (sd_fsop_submit(op, a, b, br_ents, BR_LIST_MAX) == 0) {
		br_op = op;
		br_busy = true;
		br_msg[0] = '\0';
	} else {
		snprintf(br_msg, sizeof(br_msg), "busy");
	}
	mark_dirty();
}

static void br_request_list(void)
{
	char a[SD_PATH_MAX];

	dir_path(a, sizeof(a), br_dir, "");
	/* Trim the trailing '/' the root form leaves ("/SD:/" -> "/SD:"). */
	size_t len = strlen(a);

	if (len > 0 && a[len - 1] == '/') {
		a[len - 1] = '\0';
	}
	br_submit(SD_FSOP_LIST, a, NULL);
}

static void browser_open(void)
{
	br_dir = 0;
	br_sel = 0;
	br_scroll = 0;
	br_count = 0;
	br_msg[0] = '\0';
	br_request_list();
	screen = SCR_FILES;
}

/* List rows fill the body above the two bottom buttons. */
static int br_list_bot(void)
{
	return body_bot() - FLIP_H - 6;
}

static int br_visible(void)
{
	return (br_list_bot() - body_top()) / BR_ROW_H;
}

/* Selection space: the list entries, then the two bottom buttons. */
#define BR_SEL_LEFT  (br_count)
#define BR_SEL_RIGHT (br_count + 1)

static void files_btn_rect(int i, int *x, int *y, int *w, int *h)
{
	*w = (ui_disp_w() - 2 * MARG - 8) / 2;
	*h = FLIP_H;
	*x = MARG + i * (*w + 8);
	*y = body_bot() - FLIP_H;
}

static void draw_files(void)
{
	char hdr[28], l[28];
	int y = body_top();
	int bx, by, bw, bh;

	snprintf(hdr, sizeof(hdr), "FILES: %s", dir_label(br_dir));
	draw_header(hdr);

	ui_fill_rect(BORDER_PX, y, ui_disp_w() - 2 * BORDER_PX,
		     br_list_bot() - y, COLOR_BLACK);

	if (br_busy) {
		ui_text(MARG, y, "working...", COLOR_WHITE, COLOR_BLACK);
	} else if (br_count == 0) {
		ui_text(MARG, y, "(empty)", COLOR_GREY, COLOR_BLACK);
	}

	if (!br_busy) {
		int vis = br_visible();

		for (int i = 0; i < vis && br_scroll + i < br_count; i++) {
			const struct sd_dirent *e = &br_ents[br_scroll + i];
			bool sel = (br_sel == br_scroll + i);
			uint16_t fg = sel ? COLOR_BLACK : COLOR_WHITE;
			uint16_t bg = sel ? COLOR_WHITE : COLOR_BLACK;
			int ry = y + i * BR_ROW_H;

			ui_fill_rect(MARG, ry, ui_disp_w() - 2 * MARG,
				     BR_ROW_H, bg);
			if (e->is_dir) {
				snprintf(l, sizeof(l), "/%.21s", e->name);
			} else {
				snprintf(l, sizeof(l), "%.22s", e->name);
			}
			ui_text(MARG + 2, ry + (BR_ROW_H - ui_body_h()) / 2,
				l, fg, bg);
		}
	}

	files_btn_rect(0, &bx, &by, &bw, &bh);
	ui_button(bx, by, bw, bh, (br_dir == 0) ? "+ DRAWER" : "UP",
		  br_sel == BR_SEL_LEFT);
	files_btn_rect(1, &bx, &by, &bw, &bh);
	ui_button(bx, by, bw, bh, "BACK", br_sel == BR_SEL_RIGHT);

	if (br_msg[0]) {
		draw_footer_text(br_msg);
	} else {
		snprintf(l, sizeof(l), "%d items%s  log %s", br_count,
			 br_truncated ? "+" : "", dir_label(log_dir_idx));
		draw_footer_text(l);
	}
}

/* First drawer name not present in the (root) listing; 0 if all exist. */
static int br_free_drawer(void)
{
	for (int d = 1; d <= SD_DRAWERS; d++) {
		bool found = false;

		for (int i = 0; i < br_count; i++) {
			if (br_ents[i].is_dir &&
			    strcmp(br_ents[i].name, drawer_name[d]) == 0) {
				found = true;
				break;
			}
		}
		if (!found) {
			return d;
		}
	}
	return 0;
}

static void files_activate_left(void)
{
	if (br_dir != 0) {	/* UP */
		br_dir = 0;
		br_sel = 0;
		br_scroll = 0;
		br_request_list();
		return;
	}

	int d = br_free_drawer();	/* + DRAWER */

	if (d == 0) {
		snprintf(br_msg, sizeof(br_msg), "%d drawers max", SD_DRAWERS);
		mark_dirty();
		return;
	}

	char a[SD_PATH_MAX];

	snprintf(a, sizeof(a), SD_MOUNT_POINT "/%s", drawer_name[d]);
	br_submit(SD_FSOP_MKDIR, a, NULL);
}

static void files_activate_sel(void)
{
	if (br_busy) {
		return;
	}

	if (br_sel == BR_SEL_LEFT) {
		files_activate_left();
		return;
	}
	if (br_sel == BR_SEL_RIGHT) {
		if (br_dir != 0) {
			br_dir = 0;
			br_sel = 0;
			br_scroll = 0;
			br_request_list();
		} else {
			screen = SCR_HOME;
		}
		return;
	}
	if (br_sel >= br_count) {
		return;
	}

	const struct sd_dirent *e = &br_ents[br_sel];

	if (e->is_dir) {
		/* Only the fixed drawers are enterable; foreign directories
		 * (e.g. what Windows drops on a card) are listed but opaque.
		 */
		for (int d = 1; d <= SD_DRAWERS; d++) {
			if (strcmp(e->name, drawer_name[d]) == 0) {
				br_dir = d;
				br_sel = 0;
				br_scroll = 0;
				br_request_list();
				return;
			}
		}
		snprintf(br_msg, sizeof(br_msg), "not a drawer");
		mark_dirty();
		return;
	}

	strncpy(fa_name, e->name, sizeof(fa_name) - 1);
	fa_name[sizeof(fa_name) - 1] = '\0';
	fa_sel = 0;
	fa_confirm_del = false;
	screen = SCR_FILE_ACT;
}

static void files_move_sel(int dir)
{
	int last = BR_SEL_RIGHT;

	br_sel = (br_sel + dir + last + 1) % (last + 1);

	/* Keep a list selection scrolled into view. */
	if (br_sel < br_count) {
		int vis = br_visible();

		if (br_sel < br_scroll) {
			br_scroll = br_sel;
		} else if (br_sel >= br_scroll + vis) {
			br_scroll = br_sel - vis + 1;
		}
	}
}

/* ---- Per-file action menu ---- */

enum fa_row { FA_DELETE = 0, FA_MOVE, FA_LABEL, FA_BACK, FA_COUNT };

static void fa_btn_rect(int i, int *x, int *y, int *w, int *h)
{
	int top = body_top() + ui_body_h() + 8;

	*x = MARG;
	*w = ui_disp_w() - 2 * MARG;
	*h = ACT_H;
	*y = top + i * (ACT_H + ACT_GAP);
}

static void draw_file_act(void)
{
	char l[28];

	draw_header("FILE");
	snprintf(l, sizeof(l), "%.22s", fa_name);
	ui_text(MARG, body_top(), l, COLOR_WHITE, COLOR_BLACK);

	static const char *const labels[FA_COUNT] = {
		"DELETE", "MOVE", "LABEL", "BACK",
	};

	for (int i = 0; i < FA_COUNT; i++) {
		int x, y, w, h;
		const char *lab = labels[i];

		if (i == FA_DELETE && fa_confirm_del) {
			lab = "CONFIRM DELETE?";
		}
		fa_btn_rect(i, &x, &y, &w, &h);
		ui_button(x, y, w, h, lab, fa_sel == i);
	}

	draw_footer_text(fa_confirm_del ? "DELETE again = erase"
					: "label adds _TEXT");
}

static void fa_activate(int i)
{
	char a[SD_PATH_MAX];

	if (i != FA_DELETE) {
		fa_confirm_del = false;
	}

	switch (i) {
	case FA_DELETE:
		if (!fa_confirm_del) {
			fa_confirm_del = true;	/* arm; next press erases */
			mark_dirty();
			break;
		}
		dir_path(a, sizeof(a), br_dir, fa_name);
		br_submit(SD_FSOP_UNLINK, a, NULL);
		screen = SCR_FILES;
		break;
	case FA_MOVE:
		mv_sel = 0;
		screen = SCR_FILE_MOVE;
		break;
	case FA_LABEL:
		kp_purpose = KP_FOR_LABEL;
		keypad_open(KEYPAD_ALPHA, "ADD LABEL (A-Z 0-9)", NULL);
		screen = SCR_KEYPAD;
		break;
	case FA_BACK:
	default:
		screen = SCR_FILES;
		break;
	}
}

/* Rename in place: insert "_LABEL" ahead of the extension (or append). */
static void label_apply(const char *label)
{
	char newname[SD_FSOP_NAME_MAX];
	char a[SD_PATH_MAX], b[SD_PATH_MAX];
	const char *dot = strrchr(fa_name, '.');
	int stem = (dot != NULL) ? (int)(dot - fa_name) : (int)strlen(fa_name);

	snprintf(newname, sizeof(newname), "%.*s_%.10s%s",
		 stem, fa_name, label, (dot != NULL) ? dot : "");

	dir_path(a, sizeof(a), br_dir, fa_name);
	dir_path(b, sizeof(b), br_dir, newname);
	br_submit(SD_FSOP_RENAME, a, b);
}

/* ---- Move destination pick ---- */

/* Destinations: every location except the current one, then BACK. */
static int mv_dest(int row)
{
	int n = 0;

	for (int d = 0; d <= SD_DRAWERS; d++) {
		if (d == br_dir) {
			continue;
		}
		if (n == row) {
			return d;
		}
		n++;
	}
	return -1;	/* BACK row */
}

#define MV_ROWS (SD_DRAWERS + 1)	/* 4 destinations + BACK */

static void mv_btn_rect(int i, int *x, int *y, int *w, int *h)
{
	int top = body_top() + ui_body_h() + 8;

	*x = MARG;
	*w = ui_disp_w() - 2 * MARG;
	*h = ACT_H;
	*y = top + i * (ACT_H + ACT_GAP);
}

static void draw_file_move(void)
{
	char l[28];

	draw_header("MOVE TO");
	snprintf(l, sizeof(l), "%.22s", fa_name);
	ui_text(MARG, body_top(), l, COLOR_WHITE, COLOR_BLACK);

	for (int i = 0; i < MV_ROWS; i++) {
		int x, y, w, h;
		int d = mv_dest(i);

		mv_btn_rect(i, &x, &y, &w, &h);
		ui_button(x, y, w, h, (d < 0) ? "BACK" : dir_label(d),
			  mv_sel == i);
	}

	draw_footer_text("auto-creates drawer");
}

static void mv_activate(int i)
{
	int d = mv_dest(i);

	if (d < 0) {
		screen = SCR_FILE_ACT;
		return;
	}

	char a[SD_PATH_MAX], b[SD_PATH_MAX];

	dir_path(a, sizeof(a), br_dir, fa_name);
	dir_path(b, sizeof(b), d, fa_name);
	br_submit(SD_FSOP_RENAME, a, b);
	screen = SCR_FILES;
}

/* Completion poll, run every main-loop pass. Mutating ops chain into a fresh
 * listing so the screen always shows the card as it now is.
 */
static void br_poll(void)
{
	int res, cnt;

	if (!br_busy || !sd_fsop_poll(&res, &cnt)) {
		return;
	}
	br_busy = false;

	if (res < 0) {
		snprintf(br_msg, sizeof(br_msg), "err %d (card?)", res);
		if (br_op == SD_FSOP_LIST) {
			br_count = 0;
		}
	} else if (br_op == SD_FSOP_LIST) {
		br_count = cnt;
		br_truncated = (cnt >= BR_LIST_MAX);
		if (br_sel > BR_SEL_RIGHT) {
			br_sel = 0;
		}
	} else {
		br_request_list();	/* delete/mkdir/rename done */
	}
	mark_dirty();
}

/* ---------------------------------------------------------------------------
 * Event routing
 * ------------------------------------------------------------------------- */
static void home_activate(int i)
{
	switch (i) {
	case HR_SOAK:
		pick_sel = 0;
		screen = SCR_SOAK_PICK;
		break;
	case HR_POWER:
		power_keypad_open();
		break;
	case HR_FILES:
		browser_open();
		break;
	case HR_FLIP:
		/* Controller-side 180 deg rotation: geometry is unchanged, so
		 * only a full repaint is needed.
		 */
		ui_set_flipped(!ui_flipped());
		ui_clear(COLOR_BLACK);
		draw_border();
		mark_dirty();
		break;
	default:
		break;
	}
}

static void pick_activate(int i)
{
	if (i == PICK_ROW_DIR) {
		/* Cycle "/", DRAWER0..3. Just an intent — nothing touches the
		 * card until a soak actually opens its log there.
		 */
		log_dir_idx = (log_dir_idx + 1) % (SD_DRAWERS + 1);
		mark_dirty();
	} else if (i == PICK_ROW_BACK) {
		screen = SCR_HOME;
	} else {
		soak_start(soak_durations[i - 1].ms);
	}
}

static void handle_nav(enum nav_action a)
{
	switch (screen) {
	case SCR_HOME:
		if (a == NAV_UP) {
			home_sel = (home_sel + HR_COUNT - 1) % HR_COUNT;
		} else if (a == NAV_DOWN) {
			home_sel = (home_sel + 1) % HR_COUNT;
		} else if (a == NAV_OK) {
			home_msg[0] = '\0';
			home_activate(home_sel);
		}
		break;

	case SCR_ROLE_PICK:
		/* No BACK: both options are valid, so there is nothing to
		 * cancel to — one of them must be chosen.
		 */
		if (a == NAV_UP || a == NAV_DOWN) {
			role_sel = (role_sel + 1) % 2;
		} else if (a == NAV_OK) {
			role_activate(role_sel);
		}
		break;

	case SCR_FILES:
		if (a == NAV_UP) {
			files_move_sel(-1);
		} else if (a == NAV_DOWN) {
			files_move_sel(1);
		} else if (a == NAV_OK) {
			files_activate_sel();
		} else if (a == NAV_BACK) {
			if (br_dir != 0) {
				br_dir = 0;
				br_sel = 0;
				br_scroll = 0;
				br_request_list();
			} else {
				screen = SCR_HOME;
			}
		}
		break;

	case SCR_FILE_ACT:
		if (a == NAV_UP) {
			fa_sel = (fa_sel + FA_COUNT - 1) % FA_COUNT;
			fa_confirm_del = false;
		} else if (a == NAV_DOWN) {
			fa_sel = (fa_sel + 1) % FA_COUNT;
			fa_confirm_del = false;
		} else if (a == NAV_OK) {
			fa_activate(fa_sel);
		} else if (a == NAV_BACK) {
			screen = SCR_FILES;
		}
		break;

	case SCR_FILE_MOVE:
		if (a == NAV_UP) {
			mv_sel = (mv_sel + MV_ROWS - 1) % MV_ROWS;
		} else if (a == NAV_DOWN) {
			mv_sel = (mv_sel + 1) % MV_ROWS;
		} else if (a == NAV_OK) {
			mv_activate(mv_sel);
		} else if (a == NAV_BACK) {
			screen = SCR_FILE_ACT;
		}
		break;

	case SCR_SOAK_PICK:
		if (a == NAV_UP) {
			pick_sel = (pick_sel + (int)PICK_COUNT - 1) % (int)PICK_COUNT;
		} else if (a == NAV_DOWN) {
			pick_sel = (pick_sel + 1) % (int)PICK_COUNT;
		} else if (a == NAV_OK) {
			pick_activate(pick_sel);
		} else if (a == NAV_BACK) {
			screen = SCR_HOME;
		}
		break;

	case SCR_SOAK:
		if (a == NAV_OK) {
			if (soak.active) {
				soak_finish();
			} else {
				screen = SCR_HOME;
			}
		} else if (a == NAV_BACK) {
			/* No accidental exit from a running soak: BACK only
			 * leaves once STOP (OK) has ended it.
			 */
			if (soak.active) {
				draw_footer_text("OK = STOP first");
			} else {
				screen = SCR_HOME;
			}
		}
		break;

	case SCR_KEYPAD:
		if (a == NAV_UP) {
			keypad_move(-1);
		} else if (a == NAV_DOWN) {
			keypad_move(1);
		} else if (a == NAV_OK) {
			keypad_finish(keypad_activate());
		} else if (a == NAV_BACK) {
			keypad_finish(KEYPAD_CANCEL);
		}
		break;

	case SCR_CALIBRATE:
		/* While cal_required (the power-on pass), every exit path that
		 * would leave the transform at identity is refused: the only
		 * way out is accepting a fit.
		 */
		if (cal_phase == CAL_VERIFY) {
			if (a == NAV_OK) {
				cal_accept();
			} else if (a == NAV_BACK) {
				cal_phase = CAL_COLLECT; /* redo from point 1 */
				cal_idx = 0;
				cal_test_x = -1;
			}
		} else if (cal_phase == CAL_FAIL) {
			if (a == NAV_OK) {
				cal_phase = CAL_COLLECT;
				cal_idx = 0;
			} else if (a == NAV_BACK && !cal_required) {
				screen = SCR_HOME;
			}
		} else if (a == NAV_BACK && !cal_required) {
			screen = SCR_HOME; /* cancel; keep the prior transform */
		}
		break;

	default:
		break;
	}
}

static void handle_tap(int x, int y, int rx, int ry)
{
	switch (screen) {
	case SCR_HOME:
		for (int i = 0; i < HR_COUNT; i++) {
			int bx, by, bw, bh;

			home_row_rect(i, &bx, &by, &bw, &bh);
			if (ui_hit(x, y, bx, by, bw, bh)) {
				home_sel = i;
				home_msg[0] = '\0';
				home_activate(i);
				return;
			}
		}
		break;

	case SCR_ROLE_PICK:
		for (int i = 0; i < 2; i++) {
			int bx, by, bw, bh;

			role_btn_rect(i, &bx, &by, &bw, &bh);
			if (ui_hit(x, y, bx, by, bw, bh)) {
				role_sel = i;
				role_activate(i);
				return;
			}
		}
		break;

	case SCR_FILES: {
		int bx, by, bw, bh;

		for (int i = 0; i < 2; i++) {
			files_btn_rect(i, &bx, &by, &bw, &bh);
			if (ui_hit(x, y, bx, by, bw, bh)) {
				br_sel = br_count + i;
				files_activate_sel();
				return;
			}
		}
		if (y >= body_top() && y < br_list_bot() && !br_busy) {
			int idx = br_scroll + (y - body_top()) / BR_ROW_H;

			if (idx < br_count) {
				br_sel = idx;
				files_activate_sel();
			}
		}
		break;
	}

	case SCR_FILE_ACT:
		for (int i = 0; i < FA_COUNT; i++) {
			int bx, by, bw, bh;

			fa_btn_rect(i, &bx, &by, &bw, &bh);
			if (ui_hit(x, y, bx, by, bw, bh)) {
				fa_sel = i;
				fa_activate(i);
				return;
			}
		}
		break;

	case SCR_FILE_MOVE:
		for (int i = 0; i < MV_ROWS; i++) {
			int bx, by, bw, bh;

			mv_btn_rect(i, &bx, &by, &bw, &bh);
			if (ui_hit(x, y, bx, by, bw, bh)) {
				mv_sel = i;
				mv_activate(i);
				return;
			}
		}
		break;

	case SCR_SOAK_PICK:
		for (int i = 0; i < (int)PICK_COUNT; i++) {
			int bx, by, bw, bh;

			pick_btn_rect(i, &bx, &by, &bw, &bh);
			if (ui_hit(x, y, bx, by, bw, bh)) {
				pick_sel = i;
				pick_activate(i);
				return;
			}
		}
		break;

	case SCR_SOAK: {
		int bx, by, bw, bh;

		soak_btn_rect(&bx, &by, &bw, &bh);
		if (ui_hit(x, y, bx, by, bw, bh)) {
			if (soak.active) {
				soak_finish();
			} else {
				screen = SCR_HOME;
			}
		}
		break;
	}

	case SCR_KEYPAD:
		keypad_finish(keypad_handle_touch(x, y));
		break;

	case SCR_CALIBRATE:
		if (cal_phase == CAL_COLLECT) {
			cal_rx[cal_idx] = rx; /* capture the RAW sample */
			cal_ry[cal_idx] = ry;
			if (++cal_idx >= CAL_POINTS) {
				int sxt[CAL_POINTS], syt[CAL_POINTS];

				for (int i = 0; i < CAL_POINTS; i++) {
					cal_target(i, &sxt[i], &syt[i]);
				}
				cal_phase = touch_cal_solve(cal_rx, cal_ry,
							    sxt, syt,
							    CAL_POINTS)
						    ? CAL_VERIFY
						    : CAL_FAIL;
			}
		} else if (cal_phase == CAL_VERIFY) {
			int bx, by, bw, bbh;

			/* Buttons first: otherwise a tap on one would only be
			 * recorded as a verify hit.
			 */
			cal_btn_rect(CAL_BTN_ACCEPT, &bx, &by, &bw, &bbh);
			if (ui_hit(x, y, bx, by, bw, bbh)) {
				cal_accept();
				return;
			}

			cal_btn_rect(CAL_BTN_REDO, &bx, &by, &bw, &bbh);
			if (ui_hit(x, y, bx, by, bw, bbh)) {
				cal_phase = CAL_COLLECT;
				cal_idx = 0;
				cal_test_x = -1;
				return;
			}

			cal_test_x = x; /* x,y already mapped by the new fit */
			cal_test_y = y;
		}
		break;

	default:
		break;
	}
}

static void draw_current(void)
{
	switch (screen) {
	case SCR_HOME:
		draw_home();
		break;
	case SCR_SOAK_PICK:
		draw_soak_pick();
		break;
	case SCR_SOAK:
		draw_soak();
		break;
	case SCR_KEYPAD:
		keypad_draw();
		break;
	case SCR_CALIBRATE:
		draw_calibrate();
		break;
	case SCR_ROLE_PICK:
		draw_role_pick();
		break;
	case SCR_FILES:
		draw_files();
		break;
	case SCR_FILE_ACT:
		draw_file_act();
		break;
	case SCR_FILE_MOVE:
		draw_file_move();
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
	bool ui_ok = ui_init();

	touch_cal_init();

	/* tdma_init() is deferred to the first soak start so the role stays
	 * selectable on HOME until then.
	 */

	/* No UART on this build; if the display failed there is nothing to
	 * report to, so just halt.
	 */
	if (!ui_ok) {
		k_sleep(K_FOREVER);
	}

	/* Mandatory power-on calibration: the transform is RAM-only, so the
	 * console opens on the CAL screen and cal_required keeps it there until
	 * a fit is accepted. Collection uses the RAW samples, so it works
	 * correctly from the identity mapping.
	 */
	cal_start();

	ui_clear(COLOR_BLACK);
	draw_border();
	draw_current();

	enum screen_id last = screen;

	while (1) {
		enum nav_action a = atomic_set(&nav_event, NAV_NONE);
		int rawx = 0, rawy = 0, sx = 0, sy = 0;
		bool tap = atomic_cas(&tap_ready, 1, 0);

		if (tap) {
			rawx = tap_raw_x;
			rawy = tap_raw_y;
			touch_cal_apply(rawx, rawy, &sx, &sy);
			/* The touch controller's axes are fixed to the glass,
			 * so a 180-deg flipped view needs the mapped point
			 * mirrored before any hit test sees it.
			 */
			ui_flip_point(&sx, &sy);
		}

		if (a != NAV_NONE) {
			handle_nav(a);
		}
		if (tap) {
			handle_tap(sx, sy, rawx, rawy);
		}

		/* Feed the engine / collect stats every pass, soak or not. */
		soak_poll();

		/* Browser card-op completions (writer thread finishes them). */
		br_poll();

		bool dirty = atomic_cas(&screen_dirty, 1, 0);
		bool changed = (screen != last);

		if (a != NAV_NONE || tap || dirty || changed) {
			if (changed) {
				ui_clear(COLOR_BLACK);
				draw_border();
			}
			draw_current();
			last = screen;
		}

		/* Live-refresh the soak stats while the screen is up. */
		if (screen == SCR_SOAK && soak.active) {
			int64_t now = k_uptime_get();

			if (now - soak_last_draw_ms >= 250) {
				soak_last_draw_ms = now;
				draw_soak_dynamic();
			}
		}

		k_sleep(K_MSEC(20));
	}

	return 0;
}
