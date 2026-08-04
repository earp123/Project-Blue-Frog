/*
 * soak_log - binary soak-run logger. See soak_log.h.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>

#if defined(CONFIG_APP_TDMA_CONSOLE)

#include "soak_log.h"
#include "sd_log.h"

#include <zephyr/fs/fs.h>
#include <zephyr/sys/util.h>
#include <errno.h>
#include <string.h>

/*
 * Writer thread: preemptible and below the main/UI thread, so a card stall
 * never delays a redraw, and far below the cooperative radio thread, which
 * outranks everything here by construction. Stack covers FATFS + the SPI
 * driver's call depth.
 */
#define WRITER_PRIO	K_PRIO_PREEMPT(10)
#define WRITER_STACK	3072

/* Ring between the producers (main loop) and the writer thread. */
K_MSGQ_DEFINE(rec_msgq, SOAK_REC_SIZE, SOAK_LOG_RING_DEPTH, 4);

static K_SEM_DEFINE(writer_idle, 0, 1);

/*
 * Commands from the UI thread to the writer. Mount / open / close are all
 * card I/O and can block for as long as the card feels like — an unreadable
 * or unexpected volume is exactly the case that used to hang the UI — so the
 * UI thread only ever raises a flag here and the writer does the work.
 */
#define CMD_OPEN	BIT(0)
#define CMD_CLOSE	BIT(1)
static atomic_t cmd_flags;

/* Open parameters; written by the UI thread before it raises CMD_OPEN. */
static struct {
	enum tdma_role role;
	uint8_t slot_id;
	int8_t tx_power_dbm;
	char dir[16];	/* "" = root */
	char base[16];	/* "<PWR>_<DUR>", e.g. "M9_5M" */
} open_req;

/* One-deep browser op slot (see soak_log.h). */
enum { FSOP_IDLE, FSOP_PENDING, FSOP_DONE };

static struct {
	enum sd_fsop_op op;
	char a[SD_PATH_MAX];
	char b[SD_PATH_MAX];
	struct sd_dirent *ents;
	int cap;
	int count;
	int result;
} fsop;
static atomic_t fsop_state = ATOMIC_INIT(FSOP_IDLE);

static atomic_t log_active;	/* producers gate on this */
static bool file_open;		/* writer-thread side */
static char log_path[SD_PATH_MAX];
static const char *err_stage = "";	/* which step failed, for the UI */

static uint32_t seq_next;
static atomic_t stat_queued;
static atomic_t stat_written;
static atomic_t stat_dropped;
static atomic_t stat_err;

/* Hand a record to the writer. Never blocks: a full ring means the card is
 * not keeping up, and dropping is strictly better than stalling the UI.
 */
static void submit(struct soak_rec *r)
{
	if (!atomic_get(&log_active)) {
		return;
	}

	r->seq = seq_next++;
	r->uptime_ms = (uint32_t)k_uptime_get();

	if (k_msgq_put(&rec_msgq, r, K_NO_WAIT) != 0) {
		atomic_inc(&stat_dropped);
		return;
	}
	atomic_inc(&stat_queued);
}

/* "M9" / "P22": sign letter + magnitude, no characters FAT dislikes. */
static void fmt_power(char *out, size_t n, int8_t dbm)
{
	snprintf(out, n, "%c%d", (dbm < 0) ? 'M' : 'P',
		 (dbm < 0) ? -(int)dbm : (int)dbm);
}

/* "5M" / "30M" / "2H" / "CT" (continuous). Whole hours shorten to H. */
static void fmt_duration(char *out, size_t n, uint32_t ms)
{
	uint32_t mins = ms / 60000U;

	if (ms == 0U) {
		snprintf(out, n, "CT");
	} else if (mins >= 60U && (mins % 60U) == 0U) {
		snprintf(out, n, "%uH", mins / 60U);
	} else {
		snprintf(out, n, "%uM", mins);
	}
}

int soak_log_start(enum tdma_role role, uint8_t slot_id, int8_t tx_power_dbm,
		   uint32_t duration_ms, const char *dir)
{
	struct soak_rec r = { 0 };
	struct soak_meta m = { 0 };
	char pwr[8], dur[8];

	if (atomic_get(&log_active) || file_open) {
		return -EBUSY;	/* previous run still closing */
	}

	k_msgq_purge(&rec_msgq);
	/* Clear any drain-complete signal left by the previous run, or this
	 * run's stop would return without waiting for the writer.
	 */
	k_sem_reset(&writer_idle);
	seq_next = 0;
	atomic_set(&stat_queued, 0);
	atomic_set(&stat_written, 0);
	atomic_set(&stat_dropped, 0);
	atomic_set(&stat_err, 0);
	atomic_set(&cmd_flags, 0);
	log_path[0] = '\0';

	open_req.role = role;
	open_req.slot_id = slot_id;
	open_req.tx_power_dbm = tx_power_dbm;
	strncpy(open_req.dir, (dir != NULL) ? dir : "", sizeof(open_req.dir) - 1);
	open_req.dir[sizeof(open_req.dir) - 1] = '\0';
	fmt_power(pwr, sizeof(pwr), tx_power_dbm);
	fmt_duration(dur, sizeof(dur), duration_ms);
	snprintf(open_req.base, sizeof(open_req.base), "%s_%s", pwr, dur);

	/* Record 0 describes the run, so a decoded file is self-contained. */
	m.magic = SOAK_LOG_MAGIC;
	m.version = SOAK_LOG_VERSION;
	m.role = (uint8_t)role;
	m.slot_id = slot_id;
	m.freq_hz = TDMA_RF_FREQ_HZ;
	m.slot_us = TDMA_SLOT_DURATION_US;
	m.frame_us = TDMA_FRAME_DURATION_US;
	m.toa_us = TDMA_TOA_US;
	m.sf = 5;
	m.cr = 1;		/* CR 4/5 */
	m.bw_khz = 500;
	m.tx_power_dbm = tx_power_dbm;
	m.slot_count = TDMA_SLOT_COUNT;
	m.payload_len = TDMA_PAYLOAD_LEN;
	m.preamble_syms = TDMA_PREAMBLE_SYMS;
	m.uptime_ms = (uint32_t)k_uptime_get();

	r.type = SOAK_REC_META;
	r.slot_id = slot_id;
	memcpy(r.payload, &m, sizeof(m));

	/*
	 * Open on the writer, not here. Producers are enabled immediately and
	 * META goes in as record 0, so it lands first once the file exists; if
	 * the open fails the writer disables logging and purges the ring. This
	 * call does no card I/O at all and always returns at once — the UI can
	 * never be held up by the card, whatever state it is in.
	 */
	atomic_set(&log_active, 1);
	submit(&r);
	atomic_or(&cmd_flags, CMD_OPEN);

	return 0;
}

void soak_log_rx(const struct tdma_rx_msg *msg, uint8_t sync_state)
{
	struct soak_rec r = { 0 };

	r.type = SOAK_REC_RX;
	r.slot_id = msg->slot_id;
	r.sync_state = sync_state;
	r.t_us = msg->timestamp_us;
	r.frame_ctr = msg->frame_ctr;
	r.rssi = msg->rssi;
	r.snr = msg->snr;
	memcpy(r.payload, msg->payload, TDMA_PAYLOAD_LEN);

	submit(&r);
}

void soak_log_tx(const uint8_t payload[TDMA_PAYLOAD_LEN], uint8_t slot_id,
		 uint8_t sync_state)
{
	struct soak_rec r = { 0 };

	r.type = SOAK_REC_TX;
	r.slot_id = slot_id;
	r.sync_state = sync_state;
	/*
	 * Submit time, not air time: the engine transmits this payload on the
	 * radio thread at the next TX slot boundary. The peer's RX record
	 * carries the authoritative on-air timing, and the payload content
	 * pairs the two. The engine does not expose its frame counter to the
	 * application, hence SOAK_F_NO_CTR.
	 */
	r.t_us = 0;
	r.flags = SOAK_F_NO_CTR;
	memcpy(r.payload, payload, TDMA_PAYLOAD_LEN);

	submit(&r);
}

void soak_log_stats(const struct tdma_telemetry *t)
{
	struct soak_rec r = { 0 };
	struct soak_stats s = {
		.tx_done = t->tx_done,
		.rx_done = t->rx_done,
		.rx_crc_err = t->rx_crc_err,
		.rx_bad_header = t->rx_bad_header,
		.slot_timeouts = t->slot_timeouts,
		.stale_retx = t->stale_retx,
		.busy_timeouts = t->busy_timeouts,
		/* Full width: these are the sync-quality numbers that decide
		 * how far the slot can be tightened, and v1 clamped ppm into
		 * an int8 where it simply railed.
		 */
		.phase_err_us = t->last_phase_err_us,
		.ppm = t->last_ppm,
		.evt_dt_us = t->last_evt_dt_us,
	};

	r.type = SOAK_REC_STATS;
	r.sync_state = t->sync_state;
	memcpy(r.payload, &s, sizeof(s));

	submit(&r);
}

int soak_log_stop(void)
{
	if (!atomic_get(&log_active) && !file_open) {
		return 0;	/* never opened, or the open failed */
	}

	/* Stop accepting new records, then let the writer drain and close. */
	atomic_set(&log_active, 0);
	atomic_or(&cmd_flags, CMD_CLOSE);

	/*
	 * Wait for the close so a following run cannot race this file. The
	 * drain is at most the ring (8 KB, ~16 sector writes) and normally
	 * finishes in milliseconds; the timeout only elapses on a card that is
	 * already failing, and bounds that case instead of hanging.
	 */
	(void)k_sem_take(&writer_idle, K_SECONDS(5));

	return (int)atomic_get(&stat_err);
}

void soak_log_get_status(struct soak_log_status *out)
{
	const struct sd_log_stats *s = sd_log_get_stats();

	out->active = atomic_get(&log_active) != 0;
	out->queued = (uint32_t)atomic_get(&stat_queued);
	out->written = (uint32_t)atomic_get(&stat_written);
	out->dropped = (uint32_t)atomic_get(&stat_dropped);
	out->bytes = s->bytes;
	out->err = (int)atomic_get(&stat_err);
	out->err_stage = err_stage;
	out->path = log_path;
}

/* Card I/O, writer thread only. Blocking here costs nothing but log latency. */
static void do_open(void)
{
	int rc = sd_log_mount();

	if (rc == 0) {
		rc = sd_log_open(open_req.dir, open_req.base,
				 log_path, sizeof(log_path));
	}

	if (rc < 0) {
		/* Unusable card: report it, stop the producers, and drop what
		 * was queued rather than churn a full ring for the whole run.
		 */
		err_stage = sd_log_mount_stage();
		if (err_stage[0] == '\0') {
			err_stage = "open";	/* mount was fine, open was not */
		}
		atomic_set(&stat_err, rc);
		atomic_set(&log_active, 0);
		k_msgq_purge(&rec_msgq);
		file_open = false;
		return;
	}

	file_open = true;
}

static void do_close(void)
{
	if (file_open) {
		int rc = sd_log_close();

		if (rc < 0) {
			atomic_set(&stat_err, rc);
		}
		file_open = false;
	}
}

/* ---- Browser fs ops (writer thread only) ------------------------------- */

int sd_fsop_submit(enum sd_fsop_op op, const char *a, const char *b,
		   struct sd_dirent *ents, int cap)
{
	if (atomic_get(&fsop_state) != FSOP_IDLE) {
		return -EBUSY;
	}

	fsop.op = op;
	strncpy(fsop.a, (a != NULL) ? a : "", sizeof(fsop.a) - 1);
	fsop.a[sizeof(fsop.a) - 1] = '\0';
	strncpy(fsop.b, (b != NULL) ? b : "", sizeof(fsop.b) - 1);
	fsop.b[sizeof(fsop.b) - 1] = '\0';
	fsop.ents = ents;
	fsop.cap = cap;
	fsop.count = 0;
	fsop.result = -EIO;

	atomic_set(&fsop_state, FSOP_PENDING);
	return 0;
}

bool sd_fsop_poll(int *result, int *count)
{
	if (atomic_get(&fsop_state) != FSOP_DONE) {
		return false;
	}
	*result = fsop.result;
	*count = fsop.count;
	atomic_set(&fsop_state, FSOP_IDLE);
	return true;
}

/* Directories first, then names; insertion sort is plenty at <= cap items. */
static void fsop_sort(struct sd_dirent *e, int n)
{
	for (int i = 1; i < n; i++) {
		struct sd_dirent key = e[i];
		int j = i - 1;

		while (j >= 0 &&
		       (( key.is_dir && !e[j].is_dir) ||
			(key.is_dir == e[j].is_dir &&
			 strcmp(key.name, e[j].name) < 0))) {
			e[j + 1] = e[j];
			j--;
		}
		e[j + 1] = key;
	}
}

static int fsop_list(void)
{
	/* Static: an LFN fs_dirent is large and the writer stack is sized for
	 * FatFs, not for name buffers.
	 */
	static struct fs_dir_t dirp;
	static struct fs_dirent ent;
	int rc;

	fs_dir_t_init(&dirp);
	rc = fs_opendir(&dirp, fsop.a);
	if (rc < 0) {
		return rc;
	}

	while (fsop.count < fsop.cap) {
		rc = fs_readdir(&dirp, &ent);
		if (rc < 0 || ent.name[0] == '\0') {
			break;
		}

		struct sd_dirent *d = &fsop.ents[fsop.count++];

		strncpy(d->name, ent.name, sizeof(d->name) - 1);
		d->name[sizeof(d->name) - 1] = '\0';
		d->size = (uint32_t)ent.size;
		d->is_dir = (ent.type == FS_DIR_ENTRY_DIR);
	}

	fs_closedir(&dirp);
	if (rc == 0) {
		fsop_sort(fsop.ents, fsop.count);
	}
	return rc;
}

/* RENAME doubles as move; create a missing destination drawer on the way. */
static int fsop_rename(void)
{
	char *slash = strrchr(fsop.b, '/');

	if (slash != NULL && slash > fsop.b + strlen(SD_MOUNT_POINT)) {
		*slash = '\0';
		int rc = fs_mkdir(fsop.b);

		*slash = '/';
		if (rc < 0 && rc != -EEXIST) {
			return rc;
		}
	}
	return fs_rename(fsop.a, fsop.b);
}

static void do_fsop(void)
{
	int rc = sd_log_mount();

	if (rc < 0) {
		fsop.result = rc;
		return;
	}

	switch (fsop.op) {
	case SD_FSOP_LIST:
		rc = fsop_list();
		break;
	case SD_FSOP_UNLINK:
		rc = fs_unlink(fsop.a);
		break;
	case SD_FSOP_MKDIR:
		rc = fs_mkdir(fsop.a);
		break;
	case SD_FSOP_RENAME:
		rc = fsop_rename();
		break;
	default:
		rc = -EINVAL;
		break;
	}
	fsop.result = rc;
}

static void writer_thread_fn(void *p1, void *p2, void *p3)
{
	struct soak_rec r;

	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	for (;;) {
		if (atomic_and(&cmd_flags, ~CMD_OPEN) & CMD_OPEN) {
			do_open();
		}

		/* Browser op, if one is queued. Interleaves freely with record
		 * draining — same thread, so FatFs state is never contended.
		 */
		if (atomic_get(&fsop_state) == FSOP_PENDING) {
			do_fsop();
			atomic_set(&fsop_state, FSOP_DONE);
		}

		/* Wake on a record, or periodically so the close handshake
		 * cannot be missed.
		 */
		if (k_msgq_get(&rec_msgq, &r, K_MSEC(50)) == 0) {
			if (file_open) {
				int rc = sd_log_write(&r, sizeof(r));

				if (rc < 0) {
					atomic_set(&stat_err, rc);
				} else {
					atomic_inc(&stat_written);
				}
			}
			continue;
		}

		/* Ring empty: everything queued is on the card, so a pending
		 * close can now complete.
		 */
		if (atomic_and(&cmd_flags, ~CMD_CLOSE) & CMD_CLOSE) {
			do_close();
			k_sem_give(&writer_idle);
		}
	}
}

K_THREAD_DEFINE(soak_log_writer, WRITER_STACK, writer_thread_fn,
		NULL, NULL, NULL, WRITER_PRIO, 0, 0);

#endif /* CONFIG_APP_TDMA_CONSOLE */
