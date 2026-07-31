/*
 * sd_test - mock soak-log write validation. See sd_test.h.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>

#if defined(CONFIG_APP_TDMA_CONSOLE)

#include "sd_test.h"

#include <errno.h>
#include <stdio.h>

/* Same columns the real soak logger will emit, so the row width (and therefore
 * the sectors-per-row arithmetic) measured here is the one we will ship.
 */
#define MOCK_HEADER \
	"t_ms,sync,tx,rx,crc,hdr,miss,dup,rssi,snr,phase_us,ppm\n"

static enum sd_test_state state = SD_TEST_IDLE;
static uint32_t row;
static char path[32];
static char err[32];
static uint32_t rng;

/* Small LCG: gives the columns realistic, varying widths (a fixed row would
 * make every row the same length and hide any per-row cost variation).
 */
static uint32_t next_rand(void)
{
	rng = rng * 1664525u + 1013904223u;
	return rng >> 16;
}

static void fail(const char *what, int rc)
{
	snprintf(err, sizeof(err), "%s: %d", what, rc);
	state = SD_TEST_ERROR;
	sd_log_close();
}

void sd_test_start(void)
{
	int rc;

	state = SD_TEST_IDLE;
	row = 0;
	rng = 0x1234abcdu;
	err[0] = '\0';
	path[0] = '\0';

	rc = sd_log_mount();
	if (rc < 0) {
		fail(rc == -ENODEV ? "no card" : "mount", rc);
		return;
	}

	rc = sd_log_open("MOCK", path, sizeof(path));
	if (rc < 0) {
		fail("open", rc);
		return;
	}

	rc = sd_log_printf(MOCK_HEADER);
	if (rc < 0) {
		fail("write hdr", rc);
		return;
	}

	state = SD_TEST_RUNNING;
}

void sd_test_step(void)
{
	if (state != SD_TEST_RUNNING) {
		return;
	}

	for (int i = 0; i < SD_TEST_CHUNK && row < SD_TEST_ROWS; i++, row++) {
		uint32_t r = next_rand();
		int rc;

		/* Plausible telemetry: counters advance with the row, the link
		 * quality columns jitter, errors stay rare.
		 */
		rc = sd_log_printf("%u,%u,%u,%u,%u,%u,%u,%u,%d,%d,%d,%d\n",
				   row * 200u,		/* t_ms, one frame apart */
				   2u,			/* sync = RUNNING */
				   row + 1u,		/* tx */
				   row - (row / 50u),	/* rx */
				   row / 500u,		/* crc errors */
				   row / 800u,		/* bad headers */
				   row / 50u,		/* missed */
				   row / 900u,		/* dup */
				   -40 - (int)(r % 55u),	/* rssi dBm */
				   12 - (int)(r % 9u),		/* snr dB */
				   (int)(r % 400u) - 200,	/* phase err us */
				   (int)(r % 21u) - 10);	/* ppm */
		if (rc < 0) {
			fail("write", rc);
			return;
		}
	}

	if (row >= SD_TEST_ROWS) {
		int rc = sd_log_close();

		if (rc < 0) {
			fail("close", rc);
			return;
		}
		state = SD_TEST_DONE;
	}
}

void sd_test_abort(void)
{
	if (state == SD_TEST_RUNNING) {
		sd_log_close();
		state = SD_TEST_DONE;
	}
}

enum sd_test_state sd_test_get_state(void)
{
	return state;
}

uint32_t sd_test_progress_pct(void)
{
	return (row * 100u) / SD_TEST_ROWS;
}

uint32_t sd_test_rows(void)
{
	return row;
}

const char *sd_test_path(void)
{
	return path;
}

const char *sd_test_error(void)
{
	return err;
}

const struct sd_log_stats *sd_test_stats(void)
{
	return sd_log_get_stats();
}

uint32_t sd_test_bytes_per_sec(void)
{
	const struct sd_log_stats *s = sd_log_get_stats();

	if (s->elapsed_ms == 0) {
		return 0;
	}
	return (uint32_t)(((uint64_t)s->bytes * 1000u) / s->elapsed_ms);
}

#endif /* CONFIG_APP_TDMA_CONSOLE */
