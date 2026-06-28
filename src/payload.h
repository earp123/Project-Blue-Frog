/*
 * payload - the TX payload buffer for the telemetry console.
 *
 * A simple byte buffer plus length, edited via the hex keypad or filled from
 * canned presets for fast field testing. Owned/edited by the main (UI) thread;
 * the TX thread snapshots it at send time, so no locking is needed here.
 */

#ifndef PAYLOAD_H
#define PAYLOAD_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define PAYLOAD_MAX 255

/* Default payload: 16 zero bytes (matches the standalone LoRa firmware). */
void payload_init(void);

const uint8_t *payload_bytes(void);
size_t payload_len(void);

/* Replace the payload with buf[0..len). Clamps len to PAYLOAD_MAX; returns
 * false if it had to clamp.
 */
bool payload_set(const uint8_t *buf, size_t len);
void payload_clear(void);

/* Canned presets. */
void payload_preset_zeros(void); /* 16 x 0x00 */
void payload_preset_ff(void);    /* 16 x 0xFF */
void payload_preset_ramp(void);  /* 00 01 02 .. 0F (16 bytes) */
void payload_preset_ascii(void); /* "BLUEFROG" */

/* Format the payload as "DE AD BE EF ..." into out (NUL-terminated). Returns
 * the number of characters written (excluding NUL); truncates to fit outsz.
 */
size_t payload_hex_string(char *out, size_t outsz);

/* Parse a hex string into the payload (spaces ignored, case-insensitive). A
 * trailing single nibble becomes a low-nibble byte. Returns the byte count.
 */
int payload_set_from_hex(const char *hex);

#endif /* PAYLOAD_H */
