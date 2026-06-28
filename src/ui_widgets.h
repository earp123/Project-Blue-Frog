/*
 * ui_widgets - the console's display toolkit.
 *
 * Owns the low-level RGB565 blit primitives (moved out of console.c) plus the
 * reusable widgets: push-button, config value-row, and the modal keypad.
 *
 * Single-owner drawing rule: every function here writes the panel via
 * display_write() and must only ever be called from the main/draw loop. Input
 * callbacks and the TX thread never call into ui_widgets.
 *
 * Text is rendered by blitting Zephyr's bundled CFB font glyph data into RGB565
 * by hand (CFB's own framebuffer is monochrome-only and cannot drive a colour
 * ILI9341, so only its glyph data is reused).
 */

#ifndef UI_WIDGETS_H
#define UI_WIDGETS_H

#include <stdbool.h>
#include <stdint.h>

/* RGB565 colours. White/black are byte-order agnostic on this panel (proven).
 * The colour entries assume native RGB565 byte order; selection and invalid
 * signalling use inverse white/black instead, so they stay correct regardless
 * of panel byte order.
 */
#define COLOR_WHITE  0xFFFFU
#define COLOR_BLACK  0x0000U
#define COLOR_GREY   0x8410U
#define COLOR_DGREY  0x4208U

/* Bring up the display: ready check, capabilities, backlight on, font
 * discovery. Returns false if the display isn't ready or no usable font exists.
 */
bool ui_init(void);

uint16_t ui_disp_w(void);
uint16_t ui_disp_h(void);
uint8_t ui_body_w(void); /* body font glyph width  (px) */
uint8_t ui_body_h(void); /* body font glyph height (px) */
uint8_t ui_title_h(void);

/* Primitives. */
void ui_fill_rect(int x, int y, int w, int h, uint16_t color);
void ui_clear(uint16_t color);
void ui_text(int x, int y, const char *s, uint16_t fg, uint16_t bg);     /* body */
void ui_text_big(int x, int y, const char *s, uint16_t fg, uint16_t bg); /* title */

/* Point-in-rect hit test. */
bool ui_hit(int px, int py, int x, int y, int w, int h);

/* A labelled push-button, centred text. selected => inverse (black-on-white). */
void ui_button(int x, int y, int w, int h, const char *label, bool selected);

/* A config value row: "LABEL            value" with optional < > step arrows.
 * selected => inverse highlight bar; invalid => grey background.
 */
void ui_value_row(int x, int y, int w, int h, const char *label,
		  const char *value, bool steppable, bool selected, bool invalid);

/* ---- Keypad modal (reusable) ------------------------------------------- *
 * HEX mode: 0-9 A-F, two nibbles per byte; DEC mode: 0-9 plus '.' (for MHz).
 * The editor accumulates a text string; the caller interprets it on OK via
 * keypad_text(). Drawn and driven entirely from the main loop.
 */
enum keypad_mode { KEYPAD_HEX, KEYPAD_DEC };
enum keypad_result { KEYPAD_PENDING = 0, KEYPAD_OK, KEYPAD_CANCEL };

/* Open the modal. initial may be NULL (start empty) or seed the entry. */
void keypad_open(enum keypad_mode mode, const char *title, const char *initial);
void keypad_draw(void);

/* Touch at (px,py): act on whichever key was hit. Returns OK/CANCEL when the
 * corresponding control key is pressed, else PENDING.
 */
enum keypad_result keypad_handle_touch(int px, int py);

/* Button navigation: move the key highlight (-1 prev / +1 next) and activate
 * the highlighted key. keypad_activate() returns OK/CANCEL/PENDING like touch.
 */
void keypad_move(int dir);
enum keypad_result keypad_activate(void);

const char *keypad_text(void); /* accumulated entry (digits only, no spaces) */

#endif /* UI_WIDGETS_H */
