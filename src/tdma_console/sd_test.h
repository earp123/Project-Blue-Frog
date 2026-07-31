/*
 * sd_test - mock soak-log write validation.
 *
 * Writes a synthetic log in the same CSV shape the real soak logger will use,
 * to prove the card path end to end (mount -> open -> sustained buffered
 * append -> sync -> close) and to measure what it costs: throughput and, more
 * importantly, the worst single write/sync stall, since the card shares spi4
 * with the display.
 *
 * Runs in chunks driven from the main loop rather than one blocking pass, so
 * the UI keeps redrawing and the measured stalls are the real interleaved ones.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef SD_TEST_H_
#define SD_TEST_H_

#include <stdint.h>

#include "sd_log.h"

/* ~70 B/row: 5000 rows is ~350 KB, comparable to a multi-hour soak at one row
 * per 200 ms frame, so sustained behaviour shows up rather than a burst.
 */
#define SD_TEST_ROWS	5000
/* Rows per main-loop pass: bounded work so the 20 ms UI cadence survives. */
#define SD_TEST_CHUNK	100

enum sd_test_state {
	SD_TEST_IDLE = 0,
	SD_TEST_RUNNING,
	SD_TEST_DONE,
	SD_TEST_ERROR,
};

/* Mount (if needed), open a fresh MOCKNNN.CSV and begin. */
void sd_test_start(void);

/* Write the next chunk. Safe to call in any state; only acts while RUNNING. */
void sd_test_step(void);

/* Stop early and close the file cleanly. */
void sd_test_abort(void);

enum sd_test_state sd_test_get_state(void);
uint32_t sd_test_progress_pct(void);
uint32_t sd_test_rows(void);	  /* data rows written (excludes the header) */
const char *sd_test_path(void);	  /* file being written */
const char *sd_test_error(void);  /* reason when state == SD_TEST_ERROR */
const struct sd_log_stats *sd_test_stats(void);

/* Derived: mean throughput in bytes/sec over the run (0 before any elapsed). */
uint32_t sd_test_bytes_per_sec(void);

#endif /* SD_TEST_H_ */
