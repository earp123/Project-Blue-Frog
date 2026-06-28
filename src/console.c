/*
 * Telemetry console application.
 *
 * Front end: the Hiletgo ILI9341 TFT breakout (display + XPT2046 touch + microSD
 * slot), jumper-wired to the DK Port 0 headers on a shared SPI bus (spi4). It
 * drives a button menu, reports touch coordinates, optionally logs to SD, and
 * fires asynchronous SX1262 LoRa transmits from the menu while showing radio
 * telemetry on screen.
 *
 * Text is rendered by blitting Zephyr's bundled CFB font glyph data into RGB565
 * with display_write(). CFB's own framebuffer is monochrome-only and cannot
 * drive a colour ILI9341, so we reuse only its (correct) font data.
 */

#include <zephyr/kernel.h>

#if defined(CONFIG_APP_CONSOLE)

#include <zephyr/device.h>
#include <zephyr/drivers/display.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/lora.h>
#include <zephyr/display/cfb.h>
#include <zephyr/input/input.h>
#include <zephyr/fs/fs.h>
#include <ff.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/util.h>
#include <zephyr/logging/log.h>
#include <string.h>
#include <stdio.h>

LOG_MODULE_REGISTER(console, LOG_LEVEL_INF);

/* Set to 1 once an SD card is inserted; 0 skips SD bring-up entirely. */
#define ENABLE_SD 0

/* ---- Devices ---- */
static const struct device *const display_dev = DEVICE_DT_GET(DT_CHOSEN(zephyr_display));
static const struct device *const touch_dev = DEVICE_DT_GET(DT_CHOSEN(zephyr_touch));
static const struct device *const buttons_dev = DEVICE_DT_GET(DT_PATH(buttons));
static const struct device *const lora_dev = DEVICE_DT_GET(DT_ALIAS(lora0));
static const struct gpio_dt_spec backlight = GPIO_DT_SPEC_GET(DT_NODELABEL(backlight), gpios);
static const struct gpio_dt_spec lora_rf_sw = GPIO_DT_SPEC_GET(DT_PATH(zephyr_user), rf_sw_gpios);

/* Telemetry payload: same 16 zero bytes as the standalone LoRa app. */
static uint8_t tx_payload[16];
#define PAYLOAD_LEN sizeof(tx_payload)

/* Radio config: identical to the standalone LoRa firmware. */
#define LORA_SPREADING_FACTOR SF_5

/* ---- Colours (RGB565). White/black are byte-order agnostic. ---- */
#define COLOR_WHITE 0xFFFFU
#define COLOR_BLACK 0x0000U

/* Persistent white frame thickness (px). */
#define BORDER_PX 3

static uint16_t disp_w;
static uint16_t disp_h;

/* One text row at the largest bundled font (20x32) fits in here. */
static uint16_t linebuf[320 * 32];

static const struct cfb_font *body_font;  /* 10x16 */
static const struct cfb_font *title_font; /* 20x32 */

/* ---- Touch state shared with the input callback ---- */
static volatile int touch_x;
static volatile int touch_y;
static volatile bool touch_down;

/* ---- Menu navigation, posted by the button callback, consumed in main ---- */
enum nav_action {
	NAV_NONE = 0,
	NAV_UP,
	NAV_DOWN,
	NAV_OK,
	NAV_BACK,
};
static atomic_t nav_event = ATOMIC_INIT(NAV_NONE);

/* ---- Telemetry / async transmit state ---- */
enum tx_state {
	TX_IDLE = 0,
	TX_SENDING,
	TX_SENT,
	TX_FAILED,
};

static volatile bool lora_ready;
static volatile enum tx_state tx_state = TX_IDLE;
static volatile int tx_count;       /* successful transmits */
static volatile int tx_last_rc;     /* last lora_send() return code */
static volatile int64_t tx_last_ms; /* uptime of last transmit */

/* The TX thread waits on this; main posts it (async) on an OK "Send". */
static K_SEM_DEFINE(tx_sem, 0, 1);

/* Set by either thread to ask main to redraw the screen. */
static atomic_t screen_dirty = ATOMIC_INIT(0);

static const char *tx_state_str(enum tx_state s)
{
	switch (s) {
	case TX_IDLE:    return "IDLE";
	case TX_SENDING: return "SENDING";
	case TX_SENT:    return "SENT";
	case TX_FAILED:  return "FAILED";
	default:         return "?";
	}
}

/* ---------------------------------------------------------------------------
 * Font / text rendering
 * ------------------------------------------------------------------------- */

/* Pick the bundled CFB font whose glyph width matches, else the first one. */
static const struct cfb_font *find_font(uint8_t want_width)
{
	const struct cfb_font *first = NULL;

	STRUCT_SECTION_FOREACH(cfb_font, f) {
		if (first == NULL) {
			first = f;
		}
		if (f->width == want_width) {
			return f;
		}
	}
	return first;
}

/* Return true if pixel (px,py) of glyph c is set. The bundled fonts are
 * MONO_VPACKED, LSB-first: byte = glyph[px*(h/8) + py/8], bit = py%8.
 */
static bool glyph_pixel(const struct cfb_font *f, uint8_t c, int px, int py)
{
	if (c < f->first_char || c > f->last_char) {
		c = ' ';
	}

	const uint8_t *glyph = (const uint8_t *)f->data +
		(size_t)(c - f->first_char) * ((f->width * f->height) / 8U);
	uint8_t byte = glyph[px * (f->height / 8U) + (py / 8)];

	if (f->caps & CFB_FONT_MSB_FIRST) {
		return (byte >> (7 - (py % 8))) & 1U;
	}
	return (byte >> (py % 8)) & 1U;
}

static void draw_text(int x, int y, const char *s, const struct cfb_font *f,
		      uint16_t fg, uint16_t bg)
{
	if (f == NULL || display_dev == NULL) {
		return;
	}

	int len = strlen(s);
	int cw = f->width;
	int ch = f->height;
	int total_w = len * cw;

	if (total_w <= 0 || x >= disp_w) {
		return;
	}
	if (x + total_w > disp_w) {
		total_w = disp_w - x;
	}
	if ((size_t)total_w * ch > ARRAY_SIZE(linebuf)) {
		total_w = ARRAY_SIZE(linebuf) / ch;
	}

	for (int row = 0; row < ch; row++) {
		for (int i = 0; i < len; i++) {
			for (int col = 0; col < cw; col++) {
				int gx = i * cw + col;

				if (gx >= total_w) {
					break;
				}
				linebuf[row * total_w + gx] =
					glyph_pixel(f, (uint8_t)s[i], col, row) ? fg : bg;
			}
		}
	}

	struct display_buffer_descriptor desc = {
		.buf_size = (uint32_t)total_w * ch * sizeof(uint16_t),
		.width = total_w,
		.height = ch,
		.pitch = total_w,
	};
	display_write(display_dev, x, y, &desc, linebuf);
}

/* Fill a rectangle, chunking through linebuf so any size works. */
static void fill_rect(int x, int y, int w, int h, uint16_t color)
{
	if (x < 0) {
		w += x;
		x = 0;
	}
	if (y < 0) {
		h += y;
		y = 0;
	}
	if (x + w > disp_w) {
		w = disp_w - x;
	}
	if (y + h > disp_h) {
		h = disp_h - y;
	}
	if (w <= 0 || h <= 0) {
		return;
	}

	int rows_per_chunk = ARRAY_SIZE(linebuf) / w;

	if (rows_per_chunk < 1) {
		rows_per_chunk = 1;
	}

	int yy = y;
	int remaining = h;

	while (remaining > 0) {
		int rows = MIN(remaining, rows_per_chunk);

		for (int i = 0; i < w * rows; i++) {
			linebuf[i] = color;
		}

		struct display_buffer_descriptor desc = {
			.buf_size = (uint32_t)w * rows * sizeof(uint16_t),
			.width = w,
			.height = rows,
			.pitch = w,
		};
		display_write(display_dev, x, yy, &desc, linebuf);
		yy += rows;
		remaining -= rows;
	}
}

/* ---------------------------------------------------------------------------
 * Screen layout
 * ------------------------------------------------------------------------- */

/* Menu options. Index 0 triggers the async telemetry send. */
#define MENU_SEND 0
static const char *const menu_items[] = {
	"Send Telemetry",
	"Settings",
	"About",
};
#define MENU_COUNT ARRAY_SIZE(menu_items)

static int menu_sel;             /* highlighted item */
static char menu_msg[28] = "";   /* status line (selection / send) */
static int msg_y;                /* y of the status line, set in draw_screen() */

/* Draw a single text line, clearing its row first so it can be redrawn. */
static void draw_line(int y, const char *s, uint16_t fg, uint16_t bg)
{
	fill_rect(BORDER_PX, y, disp_w - 2 * BORDER_PX, body_font->height, bg);
	draw_text(BORDER_PX + 4, y, s, body_font, fg, bg);
}

/* Redraw the whole screen (menu + telemetry). Per-row clears avoid a
 * full-screen flash and preserve the persistent border from draw_border().
 */
static void draw_screen(void)
{
	char l[28];
	int y = BORDER_PX + 4;

	draw_text(BORDER_PX + 4, y, "TELEMETRY", title_font, COLOR_WHITE, COLOR_BLACK);
	y += title_font->height + 6;

	for (int i = 0; i < (int)MENU_COUNT; i++) {
		bool sel = (i == menu_sel);

		/* Selected row: black text on a full-width white bar. */
		draw_line(y, menu_items[i],
			  sel ? COLOR_BLACK : COLOR_WHITE,
			  sel ? COLOR_WHITE : COLOR_BLACK);
		y += body_font->height + 2;
	}

	y += 4;

	/* Telemetry block. */
	draw_line(y, lora_ready ? "LORA: READY" : "LORA: NOT READY",
		  COLOR_WHITE, COLOR_BLACK);
	y += body_font->height + 2;

	snprintf(l, sizeof(l), "STATE: %s", tx_state_str(tx_state));
	draw_line(y, l, COLOR_WHITE, COLOR_BLACK);
	y += body_font->height + 2;

	snprintf(l, sizeof(l), "TX#:%d  RC:%d", tx_count, tx_last_rc);
	draw_line(y, l, COLOR_WHITE, COLOR_BLACK);
	y += body_font->height + 2;

	msg_y = y + 2;
	draw_line(msg_y, menu_msg, COLOR_WHITE, COLOR_BLACK);

	/* Button legend pinned to the bottom. */
	draw_text(BORDER_PX + 4, disp_h - body_font->height - BORDER_PX - 2,
		  "B1:UP B2:DN B3:OK B4:BK", body_font, COLOR_WHITE, COLOR_BLACK);
}

static void set_menu_msg(const char *s)
{
	strncpy(menu_msg, s, sizeof(menu_msg) - 1);
	menu_msg[sizeof(menu_msg) - 1] = '\0';
}

/* ---------------------------------------------------------------------------
 * Input callbacks (run in the input thread context, not an ISR)
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

	if (evt->sync) {
		printk("TOUCH %-4s x=%d y=%d\n", touch_down ? "DOWN" : "UP",
		       touch_x, touch_y);
	}
}
INPUT_CALLBACK_DEFINE(DEVICE_DT_GET(DT_CHOSEN(zephyr_touch)), touch_cb, NULL);

/* DK buttons -> menu navigation. Mapping:
 *   Button 1 (KEY_0, P0.23) = UP
 *   Button 2 (KEY_1, P0.24) = DOWN
 *   Button 3 (KEY_2, P0.08) = OK
 *   Button 4 (KEY_3, P0.09) = BACK
 */
static void button_cb(struct input_event *evt, void *user_data)
{
	ARG_UNUSED(user_data);

	/* Act on press only (value 1), ignore release (value 0). */
	if (evt->type != INPUT_EV_KEY || evt->value == 0) {
		return;
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
 * LoRa telemetry (async transmit on a dedicated thread)
 * ------------------------------------------------------------------------- */

/* Configure the radio once at startup. Identical settings to the standalone
 * LoRa firmware: 915 MHz, BW 500 kHz, SF5, CR 4/5, preamble 12, +4 dBm, TX.
 * RF_SW is held at GND (inactive) for both TX and RX on this module.
 */
static bool lora_telemetry_init(void)
{
	if (!device_is_ready(lora_dev)) {
		LOG_ERR("LoRa device not ready");
		return false;
	}

	if (gpio_is_ready_dt(&lora_rf_sw)) {
		gpio_pin_configure_dt(&lora_rf_sw, GPIO_OUTPUT_INACTIVE);
	} else {
		LOG_WRN("RF_SW GPIO not ready");
	}

	struct lora_modem_config cfg = {
		.frequency = 915000000,
		.bandwidth = BW_500_KHZ,
		.datarate = LORA_SPREADING_FACTOR,
		.preamble_len = 12,
		.coding_rate = CR_4_5,
		.iq_inverted = false,
		.public_network = false,
		.tx_power = 4,
		.tx = true,
	};

	int rc = lora_config(lora_dev, &cfg);

	if (rc < 0) {
		LOG_ERR("lora_config failed: %d", rc);
		return false;
	}

	LOG_INF("LoRa configured: 915MHz BW500 SF5 CR4/5 +4dBm, %u-byte payload",
		(unsigned int)PAYLOAD_LEN);
	return true;
}

/* Dedicated transmit thread: blocks on tx_sem, sends one frame per signal,
 * publishes result, and asks main to redraw. Keeps the UI responsive.
 */
static void tx_thread_fn(void *a, void *b, void *c)
{
	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);

	while (1) {
		k_sem_take(&tx_sem, K_FOREVER);

		printk("TELEMETRY: sending %u bytes...\n", (unsigned int)PAYLOAD_LEN);
		int rc = lora_send(lora_dev, tx_payload, PAYLOAD_LEN);

		tx_last_rc = rc;
		tx_last_ms = k_uptime_get();
		if (rc < 0) {
			tx_state = TX_FAILED;
			printk("TELEMETRY: send FAILED rc=%d\n", rc);
		} else {
			tx_count++;
			tx_state = TX_SENT;
			printk("TELEMETRY: send #%d OK (+%lld ms)\n",
			       tx_count, tx_last_ms);
		}
		atomic_set(&screen_dirty, 1);
	}
}
K_THREAD_DEFINE(tx_tid, 2048, tx_thread_fn, NULL, NULL, NULL, 7, 0, 0);

/* ---------------------------------------------------------------------------
 * Subsystem init
 * ------------------------------------------------------------------------- */

static bool display_init(void)
{
	if (!device_is_ready(display_dev)) {
		LOG_ERR("Display device not ready");
		return false;
	}

	struct display_capabilities cap;

	display_get_capabilities(display_dev, &cap);
	disp_w = cap.x_resolution;
	disp_h = cap.y_resolution;

	if (gpio_is_ready_dt(&backlight)) {
		gpio_pin_configure_dt(&backlight, GPIO_OUTPUT_ACTIVE);
	} else {
		LOG_WRN("Backlight GPIO not ready");
	}

	display_blanking_off(display_dev);
	LOG_INF("Display %ux%u ready, backlight %s", disp_w, disp_h,
		gpio_is_ready_dt(&backlight) ? "ON" : "not-ready");
	return true;
}

/* Clear to black and draw a persistent white border. Also a font-independent
 * confirmation the panel is being driven (SCK/MOSI/CS/D-C/RESET/backlight all
 * OK): if you see the white frame, any blank text afterwards is software, and no
 * frame at all points to display wiring / reset / backlight.
 */
static void draw_border(void)
{
	fill_rect(0, 0, disp_w, disp_h, COLOR_BLACK);
	fill_rect(0, 0, disp_w, BORDER_PX, COLOR_WHITE);            /* top */
	fill_rect(0, disp_h - BORDER_PX, disp_w, BORDER_PX, COLOR_WHITE); /* bottom */
	fill_rect(0, 0, BORDER_PX, disp_h, COLOR_WHITE);            /* left */
	fill_rect(disp_w - BORDER_PX, 0, BORDER_PX, disp_h, COLOR_WHITE); /* right */
}

#if ENABLE_SD
static FATFS fat_fs;
static struct fs_mount_t sd_mp = {
	.type = FS_FATFS,
	.fs_data = &fat_fs,
	.mnt_point = "/SD:",
};

#define BOOT_LOG_PATH "/SD:/BOOT.LOG"

/* Mount the card, append a timestamped boot line, read the file back. */
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
	/* No RTC on this build, so timestamp with uptime since reset. */
	int n = snprintf(msg, sizeof(msg), "Boot at +%lld ms uptime\n",
			 k_uptime_get());

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

int main(void)
{
	bool disp_ok, touch_ok;

	printk("\n=== Telemetry console ===\n");

	body_font = find_font(10);
	title_font = find_font(20);
	if (title_font == NULL) {
		title_font = body_font;
	}
	printk("Fonts: body=%s title=%s\n",
	       body_font ? "found" : "MISSING",
	       title_font ? "found" : "MISSING");

	disp_ok = display_init();
	touch_ok = device_is_ready(touch_dev);
	if (!touch_ok) {
		LOG_ERR("Touch device not ready");
	}

	bool buttons_ok = device_is_ready(buttons_dev);

	if (!buttons_ok) {
		LOG_ERR("Buttons device not ready");
	}
	printk("BUTTONS: %s\n", buttons_ok ? "OK" : "FAIL");

	lora_ready = lora_telemetry_init();
	printk("LORA:    %s\n", lora_ready ? "OK" : "FAIL");

#if ENABLE_SD
	static char readback[512];

	bool sd_ok = sd_init_and_log(readback, sizeof(readback));
#endif

	printk("DISPLAY: %s\n", disp_ok ? "OK" : "FAIL");
	printk("TOUCH:   %s\n", touch_ok ? "OK" : "FAIL");
#if ENABLE_SD
	printk("SD CARD: %s\n", sd_ok ? "OK" : "FAIL");
	if (sd_ok) {
		printk("---- %s ----\n%s----------------------\n",
		       BOOT_LOG_PATH, readback);
	}
#else
	printk("SD CARD: SKIPPED (no card inserted)\n");
#endif

	bool have_ui = disp_ok && body_font != NULL;

	if (have_ui) {
		draw_border();
		draw_screen();
	}

	printk("Buttons: B1=UP B2=DOWN B3=OK B4=BACK. OK on '%s' transmits.\n",
	       menu_items[MENU_SEND]);

	while (1) {
		enum nav_action action = atomic_set(&nav_event, NAV_NONE);
		bool trigger_send = false;

		switch (action) {
		case NAV_UP:
			menu_sel = (menu_sel + MENU_COUNT - 1) % MENU_COUNT;
			menu_msg[0] = '\0';
			break;
		case NAV_DOWN:
			menu_sel = (menu_sel + 1) % MENU_COUNT;
			menu_msg[0] = '\0';
			break;
		case NAV_OK:
			if (menu_sel == MENU_SEND) {
				if (lora_ready) {
					/* Show SENDING now, then kick the async TX. */
					tx_state = TX_SENDING;
					set_menu_msg("Sending...");
					trigger_send = true;
				} else {
					set_menu_msg("LoRa not ready");
				}
			} else {
				char m[28];

				snprintf(m, sizeof(m), "OK: %s", menu_items[menu_sel]);
				set_menu_msg(m);
			}
			break;
		case NAV_BACK:
			menu_sel = 0;
			set_menu_msg("(back)");
			break;
		case NAV_NONE:
		default:
			break;
		}

		if (action != NAV_NONE) {
			printk("MENU sel=%d (%s) msg='%s'\n", menu_sel,
			       menu_items[menu_sel], menu_msg);
			if (have_ui) {
				draw_screen();
			}
		}

		/* Draw SENDING before releasing the TX thread so it's observable. */
		if (trigger_send) {
			k_sem_give(&tx_sem);
		}

		/* Async TX result (or any other thread) asked for a redraw. */
		if (atomic_cas(&screen_dirty, 1, 0) && have_ui) {
			draw_screen();
		}

		k_sleep(K_MSEC(20));
	}

	return 0;
}

#endif /* CONFIG_APP_CONSOLE */
