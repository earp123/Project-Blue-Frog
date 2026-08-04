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

/* Longest VFS path handled anywhere in the SD code:
 * "/SD:/DRAWER0/P22_30M_000_ABCDEFGHIJ.BIN" comfortably fits.
 */
#define SD_PATH_MAX	64

/* FAT volume mount point; browser paths are built against this. */
#define SD_MOUNT_POINT	"/SD:"

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

/*
 * Mount the volume (FAT32 or exFAT). Returns 0, or a negative errno — pair it
 * with sd_log_mount_stage() to tell a card that never came up from a volume
 * that was rejected. Never formats: an unrecognised card is reported, not
 * overwritten.
 */
int sd_log_mount(void);
int sd_log_unmount(void);
bool sd_log_mounted(void);

/* Which step the last sd_log_mount() failed at: "disk" (card never came up)
 * or "mount" (card responds, volume rejected). Empty string on success.
 */
const char *sd_log_mount_stage(void);

/*
 * Open a new session file "<base>_NNN.BIN" at the next free NNN, inside dir
 * (a bare top-level directory name, e.g. "DRAWER0"; "" or NULL = the root).
 * A missing dir is created first. The chosen path is copied to path_out.
 * Resets the statistics.
 *
 * Names may exceed 8.3: long-name support is always compiled in
 * (CONFIG_FS_FATFS_EXFAT selects FS_FATFS_LFN), and LFN works on FAT32
 * volumes too, so the naming is portable across both card formats.
 *
 * May block for as long as the card takes to answer: call it from the
 * soak_log writer thread, never from the UI loop.
 */
int sd_log_open(const char *dir, const char *base,
		char *path_out, size_t path_len);

/* Append one formatted row. Returns 0, or a negative errno on write failure
 * (the row is counted in stats.dropped and logging continues).
 */
int sd_log_printf(const char *fmt, ...);

/*
 * Append raw bytes, with the same sector-aligned buffering as sd_log_printf.
 * This is the path the soak logger uses: fixed-size binary records cost a
 * memcpy instead of a format, and a 64 B record divides 512 exactly, so eight
 * records fill a sector with nothing wasted and no partial-sector rewrite.
 */
int sd_log_write(const void *data, size_t len);

/* Push the partial block and fs_sync(). Call at natural checkpoints. */
int sd_log_flush(void);

/* Flush and close. The volume stays mounted. */
int sd_log_close(void);

/* Live statistics (elapsed_ms is refreshed on each call while open). */
const struct sd_log_stats *sd_log_get_stats(void);

#endif /* SD_LOG_H_ */
