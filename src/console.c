/*
 * Telemetry console application - radio evaluation tooling.
 *
 * Front end: ILI9341 TFT + XPT2046 touch + microSD on the DK Port 0 SPI bus
 * (spi4), DK buttons for navigation, and an SX1262 LoRa radio on spi2/Port 1.
 *
 * This file is the screen router and main loop. It owns ALL drawing (single
 * owner): the 20 ms loop polls nav (button) and tap (touch) events plus a
 * screen_dirty flag, and is the only place that calls into ui_widgets to draw.
 * Input callbacks and the TX thread never draw - they set atomics and signal.
 *
 * Modules: radio_cfg (staged modem config), payload (TX buffer + presets),
 * ui_widgets (display primitives + reusable keypad modal).
 *
 * Screens: HOME -> {SEND ONCE, CONFIG, PAYLOAD}; CONFIG edits the staged config
 * and APPLYs it (commit point); PAYLOAD edits/loads the TX bytes; KEYPAD is a
 * modal overlay for hex/numeric entry; TX_STATUS shows send progress/result.
 */

#include <zephyr/kernel.h>

#if defined(CONFIG_APP_CONSOLE)

#include <zephyr/device.h>
#include <zephyr/drivers/lora.h>
#include <zephyr/input/input.h>
#include <zephyr/fs/fs.h>
#include <ff.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/util.h>
#include <zephyr/logging/log.h>
#include <errno.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include "ui_widgets.h"
#include "radio_cfg.h"
#include "payload.h"
#include "touch_cal.h"

LOG_MODULE_REGISTER(console, LOG_LEVEL_INF);

/* Set to 1 once an SD card is inserted; 0 skips SD bring-up entirely. */
#define ENABLE_SD 0

/* ---- Devices (display/touch owned by ui_widgets; radio by radio_cfg) ---- */
static const struct device *const touch_dev = DEVICE_DT_GET(DT_CHOSEN(zephyr_touch));
static const struct device *const buttons_dev = DEVICE_DT_GET(DT_PATH(buttons));
static const struct device *const lora_dev = DEVICE_DT_GET(DT_ALIAS(lora0));

/* ---- Layout ---- */
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

/* ---- Touch tap (debounced: one tap per press, release required) ---- */
/* The ISR latches the RAW controller coordinate; the main loop applies the
 * calibration transform (touch_cal). Calibration capture needs the raw value,
 * so the transform deliberately lives outside the ISR.
 */
static volatile int touch_x, touch_y;
static volatile bool touch_down;
static volatile bool touch_latched;
static volatile int tap_raw_x, tap_raw_y;
static atomic_t tap_ready = ATOMIC_INIT(0);

/* ---- Screen state machine ---- */
enum screen_id {
	SCR_HOME = 0,
	SCR_CONFIG,
	SCR_PAYLOAD,
	SCR_KEYPAD,
	SCR_TX_STATUS,
	SCR_CALIBRATE,
};
static enum screen_id screen = SCR_HOME;
static enum screen_id keypad_return = SCR_HOME;

/* What the keypad is currently editing (interpreted on KEYPAD_OK). */
enum edit_target {
	ED_NONE = 0,
	ED_PAYLOAD,
	ED_FREQ,
	ED_POWER,
	ED_PREAMBLE,
};
static enum edit_target edit_target = ED_NONE;

/* Per-screen selection (for button navigation). */
static int home_sel;
static int cfg_sel;
static int payload_sel;

static char home_msg[28] = "";
static char cfg_msg[40] = "APPLY";

/* ---- Telemetry / async transmit state ---- */
enum tx_state {
	TX_IDLE = 0,
	TX_SENDING,
	TX_SENT,
	TX_FAILED,
};
static volatile bool lora_ready;
static volatile enum tx_state tx_state = TX_IDLE;
static volatile int tx_count;
static volatile int tx_last_rc;
static volatile int64_t tx_last_ms;

/* Payload snapshot taken at send time so edits can't race the TX thread. */
static uint8_t tx_buf[PAYLOAD_MAX];
static volatile size_t tx_buf_len;
static char tx_cfg_summary[40] = "";

/* The TX thread waits on this; main posts it on a SEND. */
static K_SEM_DEFINE(tx_sem, 0, 1);

/* Set by any thread to ask main to redraw. */
static atomic_t screen_dirty = ATOMIC_INIT(0);

/* TX_STATUS spinner. */
static int spin;
static int64_t last_spin;

static void mark_dirty(void)
{
	atomic_set(&screen_dirty, 1);
}

/* ---------------------------------------------------------------------------
 * Input callbacks (run in the input thread context, not an ISR). They only
 * post events; they never draw.
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
		printk("TAP raw x=%d y=%d\n", touch_x, touch_y);
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
 * LoRa transmit thread: blocks on tx_sem, sends one snapshotted frame,
 * publishes the result, and asks main to redraw. Keeps the UI responsive.
 * ------------------------------------------------------------------------- */
static void tx_thread_fn(void *a, void *b, void *c)
{
	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);

	while (1) {
		k_sem_take(&tx_sem, K_FOREVER);

		size_t n = tx_buf_len;

		printk("TX: sending %u bytes...\n", (unsigned)n);
		int rc = lora_send(lora_dev, tx_buf, n);

		tx_last_rc = rc;
		tx_last_ms = k_uptime_get();
		if (rc < 0) {
			tx_state = TX_FAILED;
			printk("TX: FAILED rc=%d\n", rc);
		} else {
			tx_count++;
			tx_state = TX_SENT;
			printk("TX: #%d OK (+%lld ms)\n", tx_count, tx_last_ms);
		}
		mark_dirty();
	}
}
K_THREAD_DEFINE(tx_tid, 2048, tx_thread_fn, NULL, NULL, NULL, 7, 0, 0);

/* ---------------------------------------------------------------------------
 * Geometry helpers
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
	char t[40];

	snprintf(t, sizeof(t), "%s%s", title, radio_cfg_dirty() ? " *" : "");
	ui_fill_rect(BORDER_PX, MARG, w - 2 * BORDER_PX, bh, COLOR_BLACK);
	ui_text(MARG, MARG, t, COLOR_WHITE, COLOR_BLACK);
}

static void draw_footer_text(const char *s)
{
	int w = ui_disp_w();
	int bh = ui_body_h();
	int fy = ui_disp_h() - MARG - bh;

	ui_fill_rect(BORDER_PX, fy, w - 2 * BORDER_PX, bh, COLOR_BLACK);
	ui_text(MARG, fy, s, COLOR_WHITE, COLOR_BLACK);
}

static void draw_footer_summary(void)
{
	char s[40];

	radio_cfg_summary(s, sizeof(s));
	draw_footer_text(s);
}

/* ---------------------------------------------------------------------------
 * HOME
 * ------------------------------------------------------------------------- */
static const char *const home_items[] = { "SEND ONCE", "CONFIG", "PAYLOAD", "TOUCH CAL" };
#define HOME_COUNT ARRAY_SIZE(home_items)

static void home_btn_rect(int i, int *x, int *y, int *w, int *h)
{
	int top = body_top();
	int bot = body_bot();
	int gap = 6;
	int hh = (bot - top - ((int)HOME_COUNT - 1) * gap) / (int)HOME_COUNT;

	*x = MARG;
	*w = ui_disp_w() - 2 * MARG;
	*h = hh;
	*y = top + i * (hh + gap);
}

static void draw_home(void)
{
	draw_header("TELEMETRY");

	for (int i = 0; i < (int)HOME_COUNT; i++) {
		int x, y, w, h;

		home_btn_rect(i, &x, &y, &w, &h);
		ui_button(x, y, w, h, home_items[i], home_sel == i);
	}

	if (home_msg[0]) {
		draw_footer_text(home_msg);
	} else {
		draw_footer_summary();
	}
}

/* ---------------------------------------------------------------------------
 * CONFIG
 * ------------------------------------------------------------------------- */
enum cfg_row {
	CR_FREQ = 0,
	CR_SF,
	CR_BW,
	CR_CR,
	CR_PWR,
	CR_PRE,
	CR_CRC,
	CR_IQ,
	CR_PUB,
	CR_APPLY,
	CR_COUNT,
};

static void cfg_row_rect(int i, int *x, int *y, int *w, int *h)
{
	int top = body_top();
	int bot = body_bot();
	int rh = (bot - top) / CR_COUNT;

	*x = MARG;
	*w = ui_disp_w() - 2 * MARG;
	*h = rh;
	*y = top + i * rh;
}

/* Fill label/value for a parameter row. Returns whether the row is "steppable"
 * (enum/bool, shown with < > hint) vs. numeric-via-keypad.
 */
static bool cfg_row_text(int i, const char **label, char *val, size_t valn)
{
	switch (i) {
	case CR_FREQ:  *label = "Freq";     radio_cfg_freq_str(val, valn);     return false;
	case CR_SF:    *label = "SF";       radio_cfg_sf_str(val, valn);       return true;
	case CR_BW:    *label = "BW";       radio_cfg_bw_str(val, valn);       return true;
	case CR_CR:    *label = "CR";       radio_cfg_cr_str(val, valn);       return true;
	case CR_PWR:   *label = "TX pwr";   radio_cfg_power_str(val, valn);    return false;
	case CR_PRE:   *label = "Preamble"; radio_cfg_preamble_str(val, valn); return false;
	case CR_CRC:   *label = "CRC";      radio_cfg_crc_str(val, valn);      return true;
	case CR_IQ:    *label = "IQ";       radio_cfg_iq_str(val, valn);       return true;
	case CR_PUB:   *label = "Net";      radio_cfg_pubnet_str(val, valn);   return true;
	default:       *label = "";         val[0] = '\0';                     return false;
	}
}

static void draw_config(void)
{
	draw_header("CONFIG");

	for (int i = 0; i < CR_COUNT; i++) {
		int x, y, w, h;

		cfg_row_rect(i, &x, &y, &w, &h);

		if (i == CR_APPLY) {
			ui_button(x, y, w, h - 1, cfg_msg, cfg_sel == i);
			continue;
		}

		const char *label;
		char val[20];
		bool step = cfg_row_text(i, &label, val, sizeof(val));

		ui_value_row(x, y, w, h - 1, label, val, step, cfg_sel == i, false);
	}

	draw_footer_summary();
}

static void open_keypad(enum edit_target tgt, enum keypad_mode mode,
			const char *title, const char *initial)
{
	edit_target = tgt;
	keypad_return = screen;
	keypad_open(mode, title, initial);
	screen = SCR_KEYPAD;
}

static void do_apply(void)
{
	int rc = radio_cfg_apply();

	lora_ready = (rc == 0);
	if (rc == 0) {
		snprintf(cfg_msg, sizeof(cfg_msg), "Applied OK");
	} else if (rc == -EINVAL) {
		char e[40];

		radio_cfg_validate(e, sizeof(e));
		snprintf(cfg_msg, sizeof(cfg_msg), "%s", e[0] ? e : "invalid");
	} else if (rc == -ENODEV) {
		snprintf(cfg_msg, sizeof(cfg_msg), "radio not ready");
	} else {
		snprintf(cfg_msg, sizeof(cfg_msg), "cfg err %d", rc);
	}
	mark_dirty();
}

/* Perform a config row's action. dir (-1/+1) steps enum rows (touch uses the
 * tapped half; the OK button uses +1); numeric rows open the keypad and bool
 * rows toggle regardless of dir.
 */
static void cfg_act(int row, int dir)
{
	char init[20];

	switch (row) {
	case CR_FREQ:
		radio_cfg_freq_str(init, sizeof(init));
		open_keypad(ED_FREQ, KEYPAD_DEC, "Freq MHz", init);
		return;
	case CR_PWR:
		radio_cfg_power_str(init, sizeof(init));
		open_keypad(ED_POWER, KEYPAD_DEC, "TX power dBm", init);
		return;
	case CR_PRE:
		radio_cfg_preamble_str(init, sizeof(init));
		open_keypad(ED_PREAMBLE, KEYPAD_DEC, "Preamble syms", init);
		return;
	case CR_SF:
		radio_cfg_step_sf(dir);
		break;
	case CR_BW:
		radio_cfg_step_bw(dir);
		break;
	case CR_CR:
		radio_cfg_step_cr(dir);
		break;
	case CR_CRC:
		radio_cfg_toggle_crc();
		break;
	case CR_IQ:
		radio_cfg_toggle_iq();
		break;
	case CR_PUB:
		radio_cfg_toggle_pubnet();
		break;
	case CR_APPLY:
		do_apply();
		return;
	default:
		return;
	}
	mark_dirty();
}

/* ---------------------------------------------------------------------------
 * PAYLOAD
 * ------------------------------------------------------------------------- */
static const char *const payload_items[] = {
	"EDIT", "CLEAR", "0x00..", "0xFF..", "RAMP", "ASCII", "BACK",
};
#define PAYLOAD_BTN_COUNT ARRAY_SIZE(payload_items)

static int payload_info_h(void)
{
	return 4 * ui_body_h() + 4; /* LEN line + up to 3 hex lines */
}

static void payload_btn_rect(int i, int *x, int *y, int *w, int *h)
{
	int top = body_top() + payload_info_h();
	int bot = body_bot();
	int gap = 4;
	int rows = 4;
	int bh = (bot - top - (rows - 1) * gap) / rows;
	int colw = (ui_disp_w() - 2 * MARG - gap) / 2;

	if (i < 6) {
		int r = i / 2;
		int c = i % 2;

		*x = MARG + c * (colw + gap);
		*y = top + r * (bh + gap);
		*w = colw;
		*h = bh;
	} else {
		*x = MARG;
		*y = top + 3 * (bh + gap);
		*w = ui_disp_w() - 2 * MARG;
		*h = bh;
	}
}

static void draw_payload(void)
{
	draw_header("PAYLOAD");

	int w = ui_disp_w();
	int bw = ui_body_w();
	int bh = ui_body_h();
	int top = body_top();
	char line[40];

	/* Info area: length + wrapped hex. */
	ui_fill_rect(BORDER_PX, top, w - 2 * BORDER_PX, payload_info_h(), COLOR_BLACK);
	snprintf(line, sizeof(line), "LEN: %u / %u",
		 (unsigned)payload_len(), (unsigned)PAYLOAD_MAX);
	ui_text(MARG, top, line, COLOR_WHITE, COLOR_BLACK);

	/* Static (UI is single-threaded) to keep the main stack small. */
	static char hex[3 * PAYLOAD_MAX + 1];

	payload_hex_string(hex, sizeof(hex));

	int per_line = (w - 2 * MARG) / bw;

	if (per_line > (int)sizeof(line) - 1) {
		per_line = (int)sizeof(line) - 1;
	}
	int hlen = (int)strlen(hex);

	for (int row = 0; row < 3; row++) {
		int off = row * per_line;

		if (off >= hlen) {
			break;
		}
		int n = MIN(per_line, hlen - off);

		memcpy(line, hex + off, n);
		line[n] = '\0';
		/* Mark continuation if the payload spills past the 3rd line. */
		if (row == 2 && off + n < hlen && n >= 2) {
			line[n - 1] = '.';
			line[n - 2] = '.';
		}
		ui_text(MARG, top + bh + row * bh, line, COLOR_WHITE, COLOR_BLACK);
	}

	for (int i = 0; i < (int)PAYLOAD_BTN_COUNT; i++) {
		int x, y, h;
		int bwid;

		payload_btn_rect(i, &x, &y, &bwid, &h);
		ui_button(x, y, bwid, h, payload_items[i], payload_sel == i);
	}

	draw_footer_summary();
}

static void payload_activate(int i)
{
	static char init[3 * PAYLOAD_MAX + 1];

	switch (i) {
	case 0: /* EDIT */
		payload_hex_string(init, sizeof(init));
		open_keypad(ED_PAYLOAD, KEYPAD_HEX, "Payload hex", init);
		return;
	case 1:
		payload_clear();
		break;
	case 2:
		payload_preset_zeros();
		break;
	case 3:
		payload_preset_ff();
		break;
	case 4:
		payload_preset_ramp();
		break;
	case 5:
		payload_preset_ascii();
		break;
	case 6:
		screen = SCR_HOME;
		return;
	default:
		return;
	}
	mark_dirty();
}

/* ---------------------------------------------------------------------------
 * TX_STATUS
 * ------------------------------------------------------------------------- */
static void draw_tx_status(void)
{
	int w = ui_disp_w();
	int top = body_top();
	int bh = ui_body_h();
	char line[40];

	draw_header("TX STATUS");
	ui_fill_rect(BORDER_PX, top, w - 2 * BORDER_PX, body_bot() - top, COLOR_BLACK);

	if (tx_state == TX_SENDING) {
		static const char sp[] = { '|', '/', '-', '\\' };

		snprintf(line, sizeof(line), "TX %c", sp[spin & 3]);
		ui_text_big(MARG, top + 8, line, COLOR_WHITE, COLOR_BLACK);
		draw_footer_summary();
		return;
	}

	const char *res = (tx_state == TX_SENT) ? "SENT" : "FAILED";

	ui_text_big(MARG, top + 4, res, COLOR_WHITE, COLOR_BLACK);

	int y = top + ui_title_h() + 8;

	snprintf(line, sizeof(line), "rc = %d", tx_last_rc);
	ui_text(MARG, y, line, COLOR_WHITE, COLOR_BLACK);
	y += bh + 2;
	snprintf(line, sizeof(line), "len = %u bytes", (unsigned)tx_buf_len);
	ui_text(MARG, y, line, COLOR_WHITE, COLOR_BLACK);
	y += bh + 2;
	ui_text(MARG, y, tx_cfg_summary, COLOR_WHITE, COLOR_BLACK);
	y += bh + 6;
	ui_text(MARG, y, "(tap / OK to return)", COLOR_WHITE, COLOR_BLACK);

	draw_footer_summary();
}

/* ---------------------------------------------------------------------------
 * CALIBRATE (touch): collect crosshair taps, fit the affine, then verify.
 * ------------------------------------------------------------------------- */
#define CAL_POINTS 5

enum cal_phase { CAL_COLLECT = 0, CAL_VERIFY, CAL_FAIL };
static int cal_phase;
static int cal_idx;
static int cal_rx[CAL_POINTS], cal_ry[CAL_POINTS]; /* raw samples */
static int cal_test_x = -1, cal_test_y = -1;       /* last verify hit (mapped) */

/* Target crosshairs, inset so the controller never clamps at the panel edges. */
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
	int s = 12;

	ui_fill_rect(x - s, y - 1, 2 * s + 1, 3, color);
	ui_fill_rect(x - 1, y - s, 3, 2 * s + 1, color);
}

static void cal_start(void)
{
	cal_phase = CAL_COLLECT;
	cal_idx = 0;
	cal_test_x = -1;
	cal_test_y = -1;
	screen = SCR_CALIBRATE;
}

/* Clears its own interior each draw so the previous crosshair/marker is erased
 * (the persistent border is left intact).
 */
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
		ui_text(MARG, MARG, "TOUCH CAL", COLOR_WHITE, COLOR_BLACK);
		snprintf(l, sizeof(l), "Tap the + (%d/%d)", cal_idx + 1, CAL_POINTS);
		ui_text(MARG, MARG + bh + 2, l, COLOR_WHITE, COLOR_BLACK);
		ui_text(MARG, h - MARG - bh, "B4 = cancel", COLOR_WHITE, COLOR_BLACK);
	} else if (cal_phase == CAL_VERIFY) {
		int tx, ty;

		cal_target(4, &tx, &ty);
		draw_cross(tx, ty, COLOR_WHITE);
		if (cal_test_x >= 0) {
			ui_fill_rect(cal_test_x - 3, cal_test_y - 3, 7, 7, COLOR_GREY);
		}
		ui_text(MARG, MARG, "TOUCH CAL: verify", COLOR_WHITE, COLOR_BLACK);
		ui_text(MARG, MARG + bh + 2, "Tap +; box = where it lands",
			COLOR_WHITE, COLOR_BLACK);
		ui_text(MARG, h - MARG - bh, "B3=accept B4=redo",
			COLOR_WHITE, COLOR_BLACK);
	} else { /* CAL_FAIL */
		ui_text(MARG, MARG, "TOUCH CAL", COLOR_WHITE, COLOR_BLACK);
		ui_text(MARG, MARG + bh + 2, "Failed - tap evenly",
			COLOR_WHITE, COLOR_BLACK);
		ui_text(MARG, h - MARG - bh, "B3=retry B4=cancel",
			COLOR_WHITE, COLOR_BLACK);
	}
}

/* ---------------------------------------------------------------------------
 * Send
 * ------------------------------------------------------------------------- */
static void start_send(void)
{
	if (!lora_ready) {
		snprintf(home_msg, sizeof(home_msg), "LoRa not applied");
		return;
	}
	if (payload_len() == 0) {
		snprintf(home_msg, sizeof(home_msg), "Payload empty");
		return;
	}

	home_msg[0] = '\0';
	tx_buf_len = payload_len();
	memcpy(tx_buf, payload_bytes(), tx_buf_len);
	radio_cfg_summary(tx_cfg_summary, sizeof(tx_cfg_summary));

	tx_state = TX_SENDING;
	spin = 0;
	last_spin = k_uptime_get();
	screen = SCR_TX_STATUS;
	k_sem_give(&tx_sem);
}

static void home_activate(int i)
{
	switch (i) {
	case 0:
		start_send();
		break;
	case 1:
		cfg_msg[0] = '\0';
		snprintf(cfg_msg, sizeof(cfg_msg), "APPLY");
		screen = SCR_CONFIG;
		break;
	case 2:
		screen = SCR_PAYLOAD;
		break;
	case 3:
		cal_start();
		break;
	default:
		break;
	}
}

/* ---------------------------------------------------------------------------
 * Keypad result -> commit into the right target
 * ------------------------------------------------------------------------- */
static uint32_t parse_mhz(const char *s)
{
	uint32_t mhz = 0, tenths = 0;
	const char *p = s;

	while (*p >= '0' && *p <= '9') {
		mhz = mhz * 10U + (uint32_t)(*p - '0');
		p++;
	}
	if (*p == '.') {
		p++;
		if (*p >= '0' && *p <= '9') {
			tenths = (uint32_t)(*p - '0');
		}
	}
	return mhz * 1000000U + tenths * 100000U;
}

static void keypad_finish(enum keypad_result r)
{
	if (r == KEYPAD_OK) {
		const char *t = keypad_text();

		switch (edit_target) {
		case ED_PAYLOAD:
			payload_set_from_hex(t);
			break;
		case ED_FREQ:
			radio_cfg_set_freq(parse_mhz(t));
			break;
		case ED_POWER:
			radio_cfg_set_power((int)strtol(t, NULL, 10));
			break;
		case ED_PREAMBLE:
			radio_cfg_set_preamble((uint32_t)strtol(t, NULL, 10));
			break;
		default:
			break;
		}
		mark_dirty();
	}

	if (r == KEYPAD_OK || r == KEYPAD_CANCEL) {
		screen = keypad_return;
		edit_target = ED_NONE;
	}
}

/* ---------------------------------------------------------------------------
 * Event routing
 * ------------------------------------------------------------------------- */
static void handle_nav(enum nav_action a)
{
	switch (screen) {
	case SCR_HOME:
		if (a == NAV_UP) {
			home_sel = (home_sel + HOME_COUNT - 1) % HOME_COUNT;
		} else if (a == NAV_DOWN) {
			home_sel = (home_sel + 1) % HOME_COUNT;
		} else if (a == NAV_OK) {
			home_activate(home_sel);
		}
		break;

	case SCR_CONFIG:
		if (a == NAV_UP) {
			cfg_sel = (cfg_sel + CR_COUNT - 1) % CR_COUNT;
		} else if (a == NAV_DOWN) {
			cfg_sel = (cfg_sel + 1) % CR_COUNT;
		} else if (a == NAV_OK) {
			cfg_act(cfg_sel, 1);
		} else if (a == NAV_BACK) {
			screen = SCR_HOME;
		}
		break;

	case SCR_PAYLOAD:
		if (a == NAV_UP) {
			payload_sel = (payload_sel + PAYLOAD_BTN_COUNT - 1) % PAYLOAD_BTN_COUNT;
		} else if (a == NAV_DOWN) {
			payload_sel = (payload_sel + 1) % PAYLOAD_BTN_COUNT;
		} else if (a == NAV_OK) {
			payload_activate(payload_sel);
		} else if (a == NAV_BACK) {
			screen = SCR_HOME;
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

	case SCR_TX_STATUS:
		screen = SCR_HOME;
		break;

	case SCR_CALIBRATE:
		if (cal_phase == CAL_VERIFY) {
			if (a == NAV_OK) {
				screen = SCR_HOME; /* accept the fit */
			} else if (a == NAV_BACK) {
				cal_phase = CAL_COLLECT; /* redo from point 1 */
				cal_idx = 0;
				cal_test_x = -1;
			}
		} else if (cal_phase == CAL_FAIL) {
			if (a == NAV_OK) {
				cal_phase = CAL_COLLECT;
				cal_idx = 0;
			} else if (a == NAV_BACK) {
				screen = SCR_HOME;
			}
		} else if (a == NAV_BACK) {
			screen = SCR_HOME; /* cancel; keep the prior transform */
		}
		break;

	default:
		break;
	}
}

static int row_at(int y)
{
	int top = body_top();
	int bot = body_bot();
	int rh = (bot - top) / CR_COUNT;

	if (y < top || y >= top + CR_COUNT * rh) {
		return -1;
	}
	return (y - top) / rh;
}

static void handle_tap(int x, int y, int rx, int ry)
{
	switch (screen) {
	case SCR_HOME:
		for (int i = 0; i < (int)HOME_COUNT; i++) {
			int bx, by, bw, bh;

			home_btn_rect(i, &bx, &by, &bw, &bh);
			if (ui_hit(x, y, bx, by, bw, bh)) {
				home_sel = i;
				home_activate(i);
				return;
			}
		}
		break;

	case SCR_CONFIG: {
		int row = row_at(y);

		if (row < 0) {
			break;
		}
		int prev = cfg_sel;

		cfg_sel = row;
		/* APPLY acts immediately; params act on re-tap of the selected row.
		 * The tapped half picks the step direction for enum rows.
		 */
		if (row == CR_APPLY || row == prev) {
			int dir = (x < ui_disp_w() / 2) ? -1 : 1;

			cfg_act(row, dir);
		}
		break;
	}

	case SCR_PAYLOAD:
		for (int i = 0; i < (int)PAYLOAD_BTN_COUNT; i++) {
			int bx, by, bw, bh;

			payload_btn_rect(i, &bx, &by, &bw, &bh);
			if (ui_hit(x, y, bx, by, bw, bh)) {
				payload_sel = i;
				payload_activate(i);
				return;
			}
		}
		break;

	case SCR_KEYPAD:
		keypad_finish(keypad_handle_touch(x, y));
		break;

	case SCR_TX_STATUS:
		screen = SCR_HOME;
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
				cal_phase = touch_cal_solve(cal_rx, cal_ry, sxt, syt,
							    CAL_POINTS)
						    ? CAL_VERIFY
						    : CAL_FAIL;
			}
		} else if (cal_phase == CAL_VERIFY) {
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
	case SCR_CONFIG:
		draw_config();
		break;
	case SCR_PAYLOAD:
		draw_payload();
		break;
	case SCR_KEYPAD:
		keypad_draw();
		break;
	case SCR_TX_STATUS:
		draw_tx_status();
		break;
	case SCR_CALIBRATE:
		draw_calibrate();
		break;
	default:
		break;
	}
}

/* ---------------------------------------------------------------------------
 * SD card (optional; off by default)
 * ------------------------------------------------------------------------- */
#if ENABLE_SD
static FATFS fat_fs;
static struct fs_mount_t sd_mp = {
	.type = FS_FATFS,
	.fs_data = &fat_fs,
	.mnt_point = "/SD:",
};

#define BOOT_LOG_PATH "/SD:/BOOT.LOG"

static bool sd_init_and_log(char *readback, size_t rb_size)
{
	int rc = fs_mount(&sd_mp);

	if (rc < 0) {
		LOG_ERR("SD mount failed (%d) - card present/FAT32?", rc);
		return false;
	}
	LOG_INF("SD mounted at %s", sd_mp.mnt_point);

	struct fs_file_t f;

	fs_file_t_init(&f);
	rc = fs_open(&f, BOOT_LOG_PATH, FS_O_CREATE | FS_O_WRITE | FS_O_APPEND);
	if (rc < 0) {
		LOG_ERR("open(append) %s failed: %d", BOOT_LOG_PATH, rc);
		return false;
	}

	char msg[64];
	int n = snprintf(msg, sizeof(msg), "Boot at +%lld ms uptime\n", k_uptime_get());

	rc = fs_write(&f, msg, n);
	fs_close(&f);
	if (rc < 0) {
		LOG_ERR("write failed: %d", rc);
		return false;
	}

	fs_file_t_init(&f);
	rc = fs_open(&f, BOOT_LOG_PATH, FS_O_READ);
	if (rc < 0) {
		LOG_ERR("open(read) failed: %d", rc);
		return false;
	}

	ssize_t got = fs_read(&f, readback, rb_size - 1);

	fs_close(&f);
	if (got < 0) {
		LOG_ERR("read failed: %d", (int)got);
		return false;
	}
	readback[got] = '\0';
	return true;
}
#endif /* ENABLE_SD */

/* ---------------------------------------------------------------------------
 * main
 * ------------------------------------------------------------------------- */
int main(void)
{
	printk("\n=== Telemetry console (radio eval) ===\n");

	bool ui_ok = ui_init();
	bool touch_ok = device_is_ready(touch_dev);
	bool buttons_ok = device_is_ready(buttons_dev);

	if (!touch_ok) {
		LOG_ERR("Touch device not ready");
	}
	if (!buttons_ok) {
		LOG_ERR("Buttons device not ready");
	}

	radio_cfg_init();
	payload_init();
	touch_cal_init();

	/* Push the default config to the radio so SEND works out of the box. */
	lora_ready = (radio_cfg_apply() == 0);

	printk("DISPLAY: %s\n", ui_ok ? "OK" : "FAIL");
	printk("TOUCH:   %s\n", touch_ok ? "OK" : "FAIL");
	printk("BUTTONS: %s\n", buttons_ok ? "OK" : "FAIL");
	printk("LORA:    %s\n", lora_ready ? "OK" : "FAIL");

#if ENABLE_SD
	static char readback[512];
	bool sd_ok = sd_init_and_log(readback, sizeof(readback));

	printk("SD CARD: %s\n", sd_ok ? "OK" : "FAIL");
	if (sd_ok) {
		printk("---- %s ----\n%s----------------------\n", BOOT_LOG_PATH, readback);
	}
#else
	printk("SD CARD: SKIPPED (no card inserted)\n");
#endif

	if (!ui_ok) {
		LOG_ERR("No display/UI; halting (check wiring/reset/backlight)");
		k_sleep(K_FOREVER);
	}

	ui_clear(COLOR_BLACK);
	draw_border();
	draw_current();

	enum screen_id last = screen;

	printk("Buttons: B1=UP B2=DOWN B3=OK B4=BACK. Touch enabled.\n");

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

		/* Animate the TX spinner while a send is in flight. */
		if (screen == SCR_TX_STATUS && tx_state == TX_SENDING) {
			int64_t now = k_uptime_get();

			if (now - last_spin >= 150) {
				spin++;
				last_spin = now;
				draw_tx_status();
			}
		}

		k_sleep(K_MSEC(20));
	}

	return 0;
}

#endif /* CONFIG_APP_CONSOLE */
