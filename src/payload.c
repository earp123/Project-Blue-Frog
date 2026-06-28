/*
 * payload - TX payload buffer. See payload.h.
 */

#include <zephyr/kernel.h>

#if defined(CONFIG_APP_CONSOLE)

#include "payload.h"

#include <zephyr/sys/util.h>
#include <string.h>

static uint8_t buf[PAYLOAD_MAX];
static size_t len;

void payload_init(void)
{
	memset(buf, 0, sizeof(buf));
	len = 16;
}

const uint8_t *payload_bytes(void)
{
	return buf;
}

size_t payload_len(void)
{
	return len;
}

bool payload_set(const uint8_t *src, size_t n)
{
	bool ok = (n <= PAYLOAD_MAX);

	len = MIN(n, (size_t)PAYLOAD_MAX);
	if (len) {
		memcpy(buf, src, len);
	}
	return ok;
}

void payload_clear(void)
{
	len = 0;
}

void payload_preset_zeros(void)
{
	memset(buf, 0x00, 16);
	len = 16;
}

void payload_preset_ff(void)
{
	memset(buf, 0xFF, 16);
	len = 16;
}

void payload_preset_ramp(void)
{
	for (int i = 0; i < 16; i++) {
		buf[i] = (uint8_t)i;
	}
	len = 16;
}

void payload_preset_ascii(void)
{
	static const char s[] = "BLUEFROG";

	len = sizeof(s) - 1; /* drop the NUL */
	memcpy(buf, s, len);
}

size_t payload_hex_string(char *out, size_t outsz)
{
	static const char hexd[] = "0123456789ABCDEF";
	size_t w = 0;

	if (outsz == 0) {
		return 0;
	}

	for (size_t i = 0; i < len; i++) {
		/* Each byte needs "XX" plus a leading space (except the first):
		 * 3 chars, then the NUL. Stop if it would not fit.
		 */
		size_t need = (i == 0) ? 2 : 3;

		if (w + need + 1 > outsz) {
			break;
		}
		if (i != 0) {
			out[w++] = ' ';
		}
		out[w++] = hexd[(buf[i] >> 4) & 0xF];
		out[w++] = hexd[buf[i] & 0xF];
	}

	out[w] = '\0';
	return w;
}

static int hex_nibble(char c)
{
	if (c >= '0' && c <= '9') {
		return c - '0';
	}
	if (c >= 'a' && c <= 'f') {
		return c - 'a' + 10;
	}
	if (c >= 'A' && c <= 'F') {
		return c - 'A' + 10;
	}
	return -1;
}

int payload_set_from_hex(const char *hex)
{
	size_t n = 0;
	int hi = -1; /* pending high nibble, -1 = none */

	for (const char *p = hex; *p && n < PAYLOAD_MAX; p++) {
		int v = hex_nibble(*p);

		if (v < 0) {
			continue; /* skip spaces / separators */
		}
		if (hi < 0) {
			hi = v;
		} else {
			buf[n++] = (uint8_t)((hi << 4) | v);
			hi = -1;
		}
	}

	/* Trailing single nibble -> low-nibble byte. */
	if (hi >= 0 && n < PAYLOAD_MAX) {
		buf[n++] = (uint8_t)hi;
	}

	len = n;
	return (int)n;
}

#endif /* CONFIG_APP_CONSOLE */
