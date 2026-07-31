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
 * Screens: HOME -> {SOAK PICK -> SOAK, TX PWR keypad}. The soak
 * screen shows per-soak deltas of the engine telemetry plus frame-counter
 * continuity stats (received / missed / duplicate peer frames), which catch
 * losses the CRC counters cannot.
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

/* ---- Layout (mirrors console.c) ---- */
#define BORDER_PX 3
#define MARG 4

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
	SCR_KEYPAD,	/* TX power entry */
	SCR_CALIBRATE,
	SCR_ROLE_PICK,	/* power-on role selection */
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
#define PICK_COUNT (ARRAY_SIZE(soak_durations) + 1) /* + BACK */

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
	HR_COUNT,
};

static void home_row_rect(int i, int *x, int *y, int *w, int *h)
{
	int top = body_top();
	int bot = body_bot();
	int gap = 6;
	int hh = (bot - top - (HR_COUNT - 1) * gap) / HR_COUNT;

	*x = MARG;
	*w = ui_disp_w() - 2 * MARG;
	*h = hh;
	*y = top + i * (hh + gap);
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
	int top = body_top();
	int bot = body_bot();
	int gap = 6;
	int hh = (bot - top - ((int)PICK_COUNT - 1) * gap) / (int)PICK_COUNT;

	*x = MARG;
	*w = ui_disp_w() - 2 * MARG;
	*h = hh;
	*y = top + i * (hh + gap);
}

static void draw_soak_pick(void)
{
	char hdr[28];

	snprintf(hdr, sizeof(hdr), "SOAK: %s", role_str());
	draw_header(hdr);

	for (int i = 0; i < (int)PICK_COUNT; i++) {
		int x, y, w, h;
		const char *label = (i < (int)ARRAY_SIZE(soak_durations))
					    ? soak_durations[i].label
					    : "BACK";

		pick_btn_rect(i, &x, &y, &w, &h);
		ui_button(x, y, w, h, label, pick_sel == i);
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

	snprintf(l, sizeof(l), "rx %u  crc %u  hdr %u", soak.rx_ok,
		 t->rx_crc_err - soak.snap.rx_crc_err,
		 t->rx_bad_header - soak.snap.rx_bad_header);
	ui_text(MARG, y, l, COLOR_WHITE, COLOR_BLACK);
	y += sp;

	snprintf(l, sizeof(l), "miss %u  dup %u", soak.missed, soak.dup);
	ui_text(MARG, y, l, COLOR_WHITE, COLOR_BLACK);
	y += sp;

	snprintf(l, sizeof(l), "phase %dus ppm %d",
		 t->last_phase_err_us, t->last_ppm);
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
		snprintf(l, sizeof(l), "%s %uk drop %u",
			 ls.path + 4, /* skip the "/SD:" mount prefix */
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
			     (int8_t)tx_power_dbm);

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
	screen = SCR_SOAK;
}

static void soak_finish(void)
{
	tdma_stop();

	/* One last counter snapshot, then drain and close the log. */
	soak_log_stats(tdma_get_telemetry(), soak.rx_ok, soak.missed, soak.dup);
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

	/* One fresh pattern payload per frame; -EAGAIN just means the
	 * previous one has not been taken yet.
	 */
	uint8_t payload[TDMA_PAYLOAD_LEN];

	fill_pattern(payload, soak.seq);
	if (tdma_tx_submit(payload) == 0) {
		soak_log_tx(payload, (role == TDMA_ROLE_MASTER) ? 0 : 1,
			    tdma_get_telemetry()->sync_state);
		soak.seq++;
	}

	/* Periodic engine-counter snapshot: cheap, and it lets the decoder
	 * reconstruct rates without re-deriving them from the packet stream.
	 */
	if (k_uptime_get() - soak_last_stats_ms >= SOAK_STATS_PERIOD_MS) {
		soak_last_stats_ms = k_uptime_get();
		soak_log_stats(tdma_get_telemetry(), soak.rx_ok, soak.missed,
			       soak.dup);
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
	keypad_open(KEYPAD_DEC, "TX power dBm", init);
	screen = SCR_KEYPAD;
}

static void keypad_finish(enum keypad_result r)
{
	if (r == KEYPAD_OK) {
		int v = (int)strtol(keypad_text(), NULL, 10);

		v = CLAMP(v, TDMA_TX_POWER_MIN_DBM, TDMA_TX_POWER_MAX_DBM);
		if (tdma_set_tx_power((int8_t)v) == 0) {
			tx_power_dbm = v;
		}
		mark_dirty();
	}

	if (r == KEYPAD_OK || r == KEYPAD_CANCEL) {
		screen = SCR_HOME;
	}
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
		ui_text(MARG, cal_instr_y(), "Tap +; box = where it lands",
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
	int top = body_top() + ROLE_BLURB_LINES * (ui_body_h() + 2) + 6;
	int bot = body_bot();
	int gap = 10;
	int hh = (bot - top - gap) / 2;

	*x = MARG;
	*w = ui_disp_w() - 2 * MARG;
	*h = hh;
	*y = top + i * (hh + gap);
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
	default:
		break;
	}
}

static void pick_activate(int i)
{
	if (i < (int)ARRAY_SIZE(soak_durations)) {
		soak_start(soak_durations[i].ms);
	} else {
		screen = SCR_HOME;
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
		}

		if (a != NAV_NONE) {
			handle_nav(a);
		}
		if (tap) {
			handle_tap(sx, sy, rawx, rawy);
		}

		/* Feed the engine / collect stats every pass, soak or not. */
		soak_poll();

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
