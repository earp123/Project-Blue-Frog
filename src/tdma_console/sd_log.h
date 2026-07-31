/*
 * sd_log - buffered append-only logger to the microSD card.
 *
 * Written for the field console's soak logging, where rows are small (~70 B)
 * and frequent, and the main loop must stay responsive at 20 ms. Three things
 * make that work:
 *
 *  - The file is opened once and stays open for the whole session. Per-row
 *    open/close would rewrite the directory entry every time.
 *  - Rows are staged in RAM and reach the card only in whole 512-byte FAT
 *    sectors, so N small rows cost ceil(bytes/512) fs_write() calls, not N.
 *    A sector-aligned write also spares FATFS the read-modify-write it would
 *    otherwise do on a partial sector.
 *  - fs_sync() (the expensive part: it rewrites FAT + directory metadata) runs
 *    on a byte-count cadence, not per row. That bounds how much is lost to a
 *    power cut without paying metadata cost on every write.
 *
 * The card shares spi4 with the display and touch controller, so a flush
 * contends with drawing; the max_write_us / max_sync_us stats exist to show
 * exactly how long the worst such stall was.
 *
 * Not thread-safe: call from the main/draw loop only, like ui_widgets.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef SD_LOG_H_
#define SD_LOG_H_

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

/* FAT sector size: the write granularity everything here is aligned to. */
#define SD_LOG_BLOCK	 512
/* Staging buffer: two sectors, so a full block can always be drained while
 * leaving room for the next row without a second flush.
 */
#define SD_LOG_BUF_SIZE	 (2 * SD_LOG_BLOCK)
/* Longest single formatted row; the buffer is flushed to keep this much free. */
#define SD_LOG_ROW_MAX	 128
/* fs_sync() cadence: bounds data loss on a power cut to this many bytes. */
#define SD_LOG_SYNC_BYTES 65536

struct sd_log_stats {
	uint32_t rows;		/* successful sd_log_printf() calls */
	uint32_t bytes;		/* payload bytes staged */
	uint32_t writes;	/* fs_write() calls issued */
	uint32_t syncs;		/* fs_sync() calls issued */
	uint32_t max_write_us;	/* worst single fs_write(), microseconds */
	uint32_t max_sync_us;	/* worst single fs_sync(), microseconds */
	uint32_t elapsed_ms;	/* since sd_log_open() */
	uint32_t dropped;	/* rows lost (buffer full and flush failed) */
};

/* Mount the FAT volume. Returns 0 on success, or a negative errno; -ENODEV
 * distinguishes "no card / disk init failed" from a mount/format failure.
 */
int sd_log_mount(void);
int sd_log_unmount(void);
bool sd_log_mounted(void);

/*
 * Open a new session file "<prefix>NNN.CSV" at the next free NNN (8.3 names:
 * prefix must be <= 4 chars, since FATFS long names are not enabled). The
 * chosen path is copied to path_out. Resets the statistics.
 */
int sd_log_open(const char *prefix, char *path_out, size_t path_len);

/* Append one formatted row. Returns 0, or a negative errno on write failure
 * (the row is counted in stats.dropped and logging continues).
 */
int sd_log_printf(const char *fmt, ...);

/* Push the partial block and fs_sync(). Call at natural checkpoints. */
int sd_log_flush(void);

/* Flush and close. The volume stays mounted. */
int sd_log_close(void);

/* Live statistics (elapsed_ms is refreshed on each call while open). */
const struct sd_log_stats *sd_log_get_stats(void);

#endif /* SD_LOG_H_ */
