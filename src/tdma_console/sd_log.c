/*
 * sd_log - buffered append-only SD logger. See sd_log.h.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>

#if defined(CONFIG_APP_TDMA_CONSOLE)

#include "sd_log.h"

#include <zephyr/fs/fs.h>
#include <zephyr/storage/disk_access.h>
#include <zephyr/sys/util.h>
#include <ff.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define SD_DISK_NAME	"SD"	/* matches disk-name in the display overlay */
#define SD_MOUNT_POINT	"/SD:"
#define SD_MAX_SESSIONS	1000	/* NNN in the 8.3 session filename */

static FATFS fat_fs;
static struct fs_mount_t sd_mp = {
	.type = FS_FATFS,
	.fs_data = &fat_fs,
	.mnt_point = SD_MOUNT_POINT,
};

static bool mounted;
static bool file_open;
static struct fs_file_t file;

/* Staging buffer: rows land here and leave in whole SD_LOG_BLOCK chunks. */
static uint8_t buf[SD_LOG_BUF_SIZE];
static size_t buf_len;

static struct sd_log_stats stats;
static int64_t open_ms;
static uint32_t bytes_since_sync;

static uint32_t us_since(uint32_t start_cyc)
{
	return k_cyc_to_us_floor32(k_cycle_get_32() - start_cyc);
}

int sd_log_mount(void)
{
	int rc;

	if (mounted) {
		return 0;
	}

	/*
	 * Initialise the disk explicitly first. fs_mount() would do it too,
	 * but going through disk_access lets "no card inserted" surface as
	 * -ENODEV instead of being folded into a generic mount failure — the
	 * single most common bench condition.
	 */
	rc = disk_access_init(SD_DISK_NAME);
	if (rc < 0) {
		return -ENODEV;
	}

	rc = fs_mount(&sd_mp);
	if (rc < 0) {
		return rc;
	}

	mounted = true;
	return 0;
}

int sd_log_unmount(void)
{
	int rc;

	if (!mounted) {
		return 0;
	}
	if (file_open) {
		sd_log_close();
	}

	rc = fs_unmount(&sd_mp);
	if (rc < 0) {
		return rc;
	}

	mounted = false;
	return 0;
}

bool sd_log_mounted(void)
{
	return mounted;
}

/*
 * Drain staged data to the card. Normally (final == false) only whole sectors
 * go out and the tail stays buffered; the final flush writes the remainder.
 * Either way it is a single fs_write() call, which is what keeps the cost per
 * row down.
 */
static int flush_blocks(bool final)
{
	size_t n;
	uint32_t t0;
	ssize_t written;

	if (!file_open) {
		return -ENOENT;
	}

	n = final ? buf_len : (buf_len / SD_LOG_BLOCK) * SD_LOG_BLOCK;
	if (n == 0) {
		return 0;
	}

	t0 = k_cycle_get_32();
	written = fs_write(&file, buf, n);
	if (written < 0) {
		return (int)written;
	}

	uint32_t dt = us_since(t0);

	if (dt > stats.max_write_us) {
		stats.max_write_us = dt;
	}
	stats.writes++;

	/* Keep whatever the card did not take (short write) plus the tail. */
	buf_len -= (size_t)written;
	if (buf_len > 0) {
		memmove(buf, buf + written, buf_len);
	}

	bytes_since_sync += (uint32_t)written;
	if (final || bytes_since_sync >= SD_LOG_SYNC_BYTES) {
		t0 = k_cycle_get_32();
		int rc = fs_sync(&file);

		if (rc < 0) {
			return rc;
		}
		dt = us_since(t0);
		if (dt > stats.max_sync_us) {
			stats.max_sync_us = dt;
		}
		stats.syncs++;
		bytes_since_sync = 0;
	}

	return 0;
}

int sd_log_open(const char *prefix, char *path_out, size_t path_len)
{
	char path[32];
	struct fs_dirent ent;
	int rc = -ENOSPC;

	if (!mounted) {
		return -ENODEV;
	}
	if (file_open) {
		return -EBUSY;
	}

	/* First unused <prefix>NNN.CSV, so a run never clobbers an earlier one. */
	for (int i = 0; i < SD_MAX_SESSIONS; i++) {
		snprintf(path, sizeof(path), SD_MOUNT_POINT "/%s%03d.CSV",
			 prefix, i);
		if (fs_stat(path, &ent) == -ENOENT) {
			rc = 0;
			break;
		}
	}
	if (rc < 0) {
		return rc;
	}

	fs_file_t_init(&file);
	rc = fs_open(&file, path, FS_O_CREATE | FS_O_WRITE);
	if (rc < 0) {
		return rc;
	}

	file_open = true;
	buf_len = 0;
	bytes_since_sync = 0;
	memset(&stats, 0, sizeof(stats));
	open_ms = k_uptime_get();

	if (path_out && path_len) {
		strncpy(path_out, path, path_len - 1);
		path_out[path_len - 1] = '\0';
	}
	return 0;
}

int sd_log_printf(const char *fmt, ...)
{
	va_list ap;
	int n;

	if (!file_open) {
		return -ENOENT;
	}

	/* Guarantee room for the longest row before formatting into the tail,
	 * so vsnprintf never has to truncate.
	 */
	if (SD_LOG_BUF_SIZE - buf_len < SD_LOG_ROW_MAX) {
		int rc = flush_blocks(false);

		if (rc < 0) {
			stats.dropped++;
			return rc;
		}
	}

	va_start(ap, fmt);
	n = vsnprintf((char *)buf + buf_len, SD_LOG_BUF_SIZE - buf_len, fmt, ap);
	va_end(ap);

	if (n < 0 || (size_t)n >= SD_LOG_BUF_SIZE - buf_len) {
		stats.dropped++; /* row longer than SD_LOG_ROW_MAX */
		return -EINVAL;
	}

	buf_len += (size_t)n;
	stats.bytes += (uint32_t)n;
	stats.rows++;

	if (buf_len >= SD_LOG_BLOCK) {
		int rc = flush_blocks(false);

		if (rc < 0) {
			return rc;
		}
	}

	return 0;
}

int sd_log_write(const void *data, size_t len)
{
	const uint8_t *p = data;

	if (!file_open) {
		return -ENOENT;
	}

	while (len > 0) {
		size_t room = SD_LOG_BUF_SIZE - buf_len;
		size_t n;

		if (room == 0) {
			int rc = flush_blocks(false);

			if (rc < 0) {
				stats.dropped++;
				return rc;
			}
			room = SD_LOG_BUF_SIZE - buf_len;
		}

		n = MIN(room, len);
		memcpy(buf + buf_len, p, n);
		buf_len += n;
		p += n;
		len -= n;
		stats.bytes += (uint32_t)n;

		if (buf_len >= SD_LOG_BLOCK) {
			int rc = flush_blocks(false);

			if (rc < 0) {
				return rc;
			}
		}
	}

	stats.rows++;
	return 0;
}

int sd_log_flush(void)
{
	return flush_blocks(true);
}

int sd_log_close(void)
{
	int rc;

	if (!file_open) {
		return 0;
	}

	rc = flush_blocks(true);
	fs_close(&file);
	file_open = false;
	stats.elapsed_ms = (uint32_t)(k_uptime_get() - open_ms);
	return rc;
}

const struct sd_log_stats *sd_log_get_stats(void)
{
	if (file_open) {
		stats.elapsed_ms = (uint32_t)(k_uptime_get() - open_ms);
	}
	return &stats;
}

#endif /* CONFIG_APP_TDMA_CONSOLE */
