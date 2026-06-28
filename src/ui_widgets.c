/*
 * ui_widgets - console display toolkit + reusable keypad modal. See header.
 */

#include <zephyr/kernel.h>

#if defined(CONFIG_APP_CONSOLE)

#include "ui_widgets.h"

#include <zephyr/device.h>
#include <zephyr/drivers/display.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/display/cfb.h>
#include <zephyr/sys/util.h>
#include <zephyr/logging/log.h>
#include <string.h>
#include <stdio.h>

LOG_MODULE_REGISTER(ui_widgets, LOG_LEVEL_INF);

static const struct device *const display_dev = DEVICE_DT_GET(DT_CHOSEN(zephyr_display));
static const struct gpio_dt_spec backlight = GPIO_DT_SPEC_GET(DT_NODELABEL(backlight), gpios);

static uint16_t disp_w;
static uint16_t disp_h;

static const struct cfb_font *body_font;  /* 10x16 */
static const struct cfb_font *title_font; /* 20x32 */

/* One row at the largest bundled font (20x32) fits here; fill_rect chunks. */
static uint16_t linebuf[320 * 32];

/* ---------------------------------------------------------------------------
 * Font / text rendering (ported from the original console.c)
 * ------------------------------------------------------------------------- */

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

/* True if pixel (px,py) of glyph c is set. Bundled fonts are MONO_VPACKED,
 * LSB-first: byte = glyph[px*(h/8) + py/8], bit = py%8.
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

static void text_font(int x, int y, const char *s, const struct cfb_font *f,
		      uint16_t fg, uint16_t bg)
{
	if (f == NULL || !device_is_ready(display_dev)) {
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

void ui_text(int x, int y, const char *s, uint16_t fg, uint16_t bg)
{
	text_font(x, y, s, body_font, fg, bg);
}

void ui_text_big(int x, int y, const char *s, uint16_t fg, uint16_t bg)
{
	text_font(x, y, s, title_font, fg, bg);
}

void ui_fill_rect(int x, int y, int w, int h, uint16_t color)
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

void ui_clear(uint16_t color)
{
	ui_fill_rect(0, 0, disp_w, disp_h, color);
}

/* ---------------------------------------------------------------------------
 * Init / accessors
 * ------------------------------------------------------------------------- */

bool ui_init(void)
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

	body_font = find_font(10);
	title_font = find_font(20);
	if (title_font == NULL) {
		title_font = body_font;
	}

	LOG_INF("Display %ux%u ready, body=%s title=%s", disp_w, disp_h,
		body_font ? "ok" : "MISSING", title_font ? "ok" : "MISSING");
	return body_font != NULL;
}

uint16_t ui_disp_w(void)
{
	return disp_w;
}

uint16_t ui_disp_h(void)
{
	return disp_h;
}

uint8_t ui_body_w(void)
{
	return body_font ? body_font->width : 0;
}

uint8_t ui_body_h(void)
{
	return body_font ? body_font->height : 0;
}

uint8_t ui_title_h(void)
{
	return title_font ? title_font->height : 0;
}

bool ui_hit(int px, int py, int x, int y, int w, int h)
{
	return px >= x && px < x + w && py >= y && py < y + h;
}

/* ---------------------------------------------------------------------------
 * Widgets
 * ------------------------------------------------------------------------- */

static void outline(int x, int y, int w, int h, uint16_t color)
{
	ui_fill_rect(x, y, w, 1, color);
	ui_fill_rect(x, y + h - 1, w, 1, color);
	ui_fill_rect(x, y, 1, h, color);
	ui_fill_rect(x + w - 1, y, 1, h, color);
}

void ui_button(int x, int y, int w, int h, const char *label, bool selected)
{
	uint16_t bg = selected ? COLOR_WHITE : COLOR_BLACK;
	uint16_t fg = selected ? COLOR_BLACK : COLOR_WHITE;

	ui_fill_rect(x, y, w, h, bg);
	outline(x, y, w, h, selected ? COLOR_BLACK : COLOR_GREY);

	int bw = ui_body_w();
	int bh = ui_body_h();
	int tw = (int)strlen(label) * bw;
	int tx = x + (w - tw) / 2;
	int ty = y + (h - bh) / 2;

	if (tx < x + 2) {
		tx = x + 2;
	}
	ui_text(tx, ty, label, fg, bg);
}

void ui_value_row(int x, int y, int w, int h, const char *label,
		  const char *value, bool steppable, bool selected, bool invalid)
{
	uint16_t bg = selected ? COLOR_WHITE : COLOR_BLACK;
	uint16_t fg = selected ? COLOR_BLACK : COLOR_WHITE;

	if (invalid) {
		bg = COLOR_GREY;
		fg = COLOR_BLACK;
	}

	ui_fill_rect(x, y, w, h, bg);

	int bw = ui_body_w();
	int cy = y + (h - ui_body_h()) / 2;

	ui_text(x + 3, cy, label, fg, bg);

	int vlen = (int)strlen(value);

	if (steppable) {
		int gx = x + w - 3 - bw;          /* ">" cell */
		int vx = gx - 2 - vlen * bw;      /* value */
		int lx = vx - 2 - bw;             /* "<" cell */

		ui_text(gx, cy, ">", fg, bg);
		ui_text(vx, cy, value, fg, bg);
		ui_text(lx, cy, "<", fg, bg);
	} else {
		ui_text(x + w - 3 - vlen * bw, cy, value, fg, bg);
	}
}

/* ---------------------------------------------------------------------------
 * Keypad modal
 * ------------------------------------------------------------------------- */

/* Manual entry cap. HEX => 48 bytes; plenty for field test vectors (longer
 * payloads are built from the canned presets / ramp instead).
 */
#define KP_MAX_CHARS 96

enum kk { KK_NONE = 0, KK_CH, KK_SIGN, KK_DEL, KK_CLR, KK_OK, KK_CANCEL };

struct kp_key {
	uint8_t row, col;
	const char *label;
	enum kk kind;
	char ch; /* for KK_CH */
};

static const struct kp_key hex_tab[] = {
	{0, 0, "7", KK_CH, '7'}, {0, 1, "8", KK_CH, '8'}, {0, 2, "9", KK_CH, '9'}, {0, 3, "DEL", KK_DEL, 0},
	{1, 0, "4", KK_CH, '4'}, {1, 1, "5", KK_CH, '5'}, {1, 2, "6", KK_CH, '6'}, {1, 3, "CLR", KK_CLR, 0},
	{2, 0, "1", KK_CH, '1'}, {2, 1, "2", KK_CH, '2'}, {2, 2, "3", KK_CH, '3'}, {2, 3, "OK", KK_OK, 0},
	{3, 0, "0", KK_CH, '0'}, {3, 1, "A", KK_CH, 'A'}, {3, 2, "B", KK_CH, 'B'}, {3, 3, "CXL", KK_CANCEL, 0},
	{4, 0, "C", KK_CH, 'C'}, {4, 1, "D", KK_CH, 'D'}, {4, 2, "E", KK_CH, 'E'}, {4, 3, "F", KK_CH, 'F'},
};

static const struct kp_key dec_tab[] = {
	{0, 0, "7", KK_CH, '7'}, {0, 1, "8", KK_CH, '8'}, {0, 2, "9", KK_CH, '9'}, {0, 3, "DEL", KK_DEL, 0},
	{1, 0, "4", KK_CH, '4'}, {1, 1, "5", KK_CH, '5'}, {1, 2, "6", KK_CH, '6'}, {1, 3, "CLR", KK_CLR, 0},
	{2, 0, "1", KK_CH, '1'}, {2, 1, "2", KK_CH, '2'}, {2, 2, "3", KK_CH, '3'}, {2, 3, "OK", KK_OK, 0},
	{3, 0, "0", KK_CH, '0'}, {3, 1, ".", KK_CH, '.'}, {3, 2, "+/-", KK_SIGN, 0}, {3, 3, "CXL", KK_CANCEL, 0},
};

static enum keypad_mode kp_mode;
static const struct kp_key *kp_tab;
static int kp_count;
static int kp_rows;
static char kp_title[24];
static char kp_buf[KP_MAX_CHARS + 1];
static int kp_len;
static int kp_sel;

/* Layout: title + echo across the top, then a 4-column key grid. */
#define KP_MARGIN 4

static int kp_grid_top(void)
{
	return KP_MARGIN + 2 * (ui_body_h() + 3) + 2;
}

static void kp_cell_rect(int idx, int *x, int *y, int *w, int *h)
{
	int gtop = kp_grid_top();
	int gw = disp_w - 2 * KP_MARGIN;
	int gh = disp_h - gtop - KP_MARGIN;
	int cw = gw / 4;
	int chh = gh / kp_rows;

	*x = KP_MARGIN + kp_tab[idx].col * cw;
	*y = gtop + kp_tab[idx].row * chh;
	*w = cw;
	*h = chh;
}

static bool kp_has_dot(void)
{
	return strchr(kp_buf, '.') != NULL;
}

void keypad_open(enum keypad_mode mode, const char *title, const char *initial)
{
	kp_mode = mode;
	if (mode == KEYPAD_HEX) {
		kp_tab = hex_tab;
		kp_count = ARRAY_SIZE(hex_tab);
		kp_rows = 5;
	} else {
		kp_tab = dec_tab;
		kp_count = ARRAY_SIZE(dec_tab);
		kp_rows = 4;
	}

	strncpy(kp_title, title ? title : "", sizeof(kp_title) - 1);
	kp_title[sizeof(kp_title) - 1] = '\0';

	kp_len = 0;
	kp_buf[0] = '\0';
	kp_sel = 0;

	/* Seed from initial, keeping only chars valid in this mode (so callers can
	 * pass a formatted value like "915.0 MHz" or "DE AD" directly).
	 */
	if (initial) {
		for (const char *p = initial; *p && kp_len < KP_MAX_CHARS; p++) {
			char c = *p;
			bool keep;

			if (mode == KEYPAD_HEX) {
				keep = (c >= '0' && c <= '9') ||
				       (c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f');
				if (keep && c >= 'a') {
					c -= 32; /* upper-case */
				}
			} else {
				keep = (c >= '0' && c <= '9') ||
				       (c == '.' && !kp_has_dot());
			}
			if (keep) {
				kp_buf[kp_len++] = c;
				kp_buf[kp_len] = '\0';
			}
		}
	}
}

static void kp_append(char c)
{
	if (kp_len >= KP_MAX_CHARS) {
		return;
	}
	if (c == '.' && kp_has_dot()) {
		return;
	}
	kp_buf[kp_len++] = c;
	kp_buf[kp_len] = '\0';
}

static enum keypad_result kp_act(const struct kp_key *k)
{
	switch (k->kind) {
	case KK_CH:
		kp_append(k->ch);
		break;
	case KK_SIGN:
		/* Toggle a leading '-' (for negative TX power). */
		if (kp_len > 0 && kp_buf[0] == '-') {
			memmove(kp_buf, kp_buf + 1, kp_len); /* includes NUL */
			kp_len--;
		} else if (kp_len < KP_MAX_CHARS) {
			memmove(kp_buf + 1, kp_buf, kp_len + 1); /* includes NUL */
			kp_buf[0] = '-';
			kp_len++;
		}
		break;
	case KK_DEL:
		if (kp_len > 0) {
			kp_buf[--kp_len] = '\0';
		}
		break;
	case KK_CLR:
		kp_len = 0;
		kp_buf[0] = '\0';
		break;
	case KK_OK:
		return KEYPAD_OK;
	case KK_CANCEL:
		return KEYPAD_CANCEL;
	default:
		break;
	}
	return KEYPAD_PENDING;
}

/* Build the echo string (HEX groups bytes in pairs). */
static void kp_echo(char *out, size_t outsz)
{
	if (outsz == 0) {
		return;
	}
	if (kp_mode != KEYPAD_HEX) {
		strncpy(out, kp_buf, outsz - 1);
		out[outsz - 1] = '\0';
		return;
	}

	size_t w = 0;

	for (int i = 0; i < kp_len && w + 2 < outsz; i++) {
		if (i > 0 && (i % 2) == 0 && w + 1 < outsz) {
			out[w++] = ' ';
		}
		out[w++] = kp_buf[i];
	}
	out[w] = '\0';
}

void keypad_draw(void)
{
	int bw = ui_body_w();
	int bh = ui_body_h();

	/* Title + echo band (cleared each draw since the echo grows/shrinks). */
	ui_fill_rect(KP_MARGIN, KP_MARGIN, disp_w - 2 * KP_MARGIN,
		     2 * (bh + 3), COLOR_BLACK);
	ui_text(KP_MARGIN, KP_MARGIN, kp_title, COLOR_WHITE, COLOR_BLACK);

	char echo[160];
	char line[48];

	kp_echo(echo, sizeof(echo));

	/* Show a leading '>' then the tail of the entry that fits the line. */
	int avail = (int)sizeof(line) - 2; /* '>' + NUL */
	int max_chars = (disp_w - 2 * KP_MARGIN) / bw - 1;

	if (max_chars < 1) {
		max_chars = 1;
	}
	if (max_chars > avail) {
		max_chars = avail;
	}
	int elen = (int)strlen(echo);
	const char *tail = (elen > max_chars) ? echo + (elen - max_chars) : echo;
	int tlen = (int)strlen(tail);

	line[0] = '>';
	memcpy(line + 1, tail, tlen);
	line[1 + tlen] = '\0';
	ui_text(KP_MARGIN, KP_MARGIN + bh + 3, line, COLOR_WHITE, COLOR_BLACK);

	for (int i = 0; i < kp_count; i++) {
		int x, y, w, h;

		kp_cell_rect(i, &x, &y, &w, &h);
		ui_button(x + 1, y + 1, w - 2, h - 2, kp_tab[i].label, i == kp_sel);
	}
}

enum keypad_result keypad_handle_touch(int px, int py)
{
	for (int i = 0; i < kp_count; i++) {
		int x, y, w, h;

		kp_cell_rect(i, &x, &y, &w, &h);
		if (ui_hit(px, py, x, y, w, h)) {
			kp_sel = i;
			return kp_act(&kp_tab[i]);
		}
	}
	return KEYPAD_PENDING;
}

void keypad_move(int dir)
{
	kp_sel = (kp_sel + (dir < 0 ? -1 : 1) + kp_count) % kp_count;
}

enum keypad_result keypad_activate(void)
{
	return kp_act(&kp_tab[kp_sel]);
}

const char *keypad_text(void)
{
	return kp_buf;
}

#endif /* CONFIG_APP_CONSOLE */
