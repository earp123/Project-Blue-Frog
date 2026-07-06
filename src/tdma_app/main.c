/*
 * Intercom TDMA test firmware (CONFIG_APP_TDMA_TEST) — variant entry point.
 *
 * The LoRa-send variant lives in src/main.c and the telemetry console in
 * src/console.c; exactly one main() is linked, selected by the application
 * Kconfig choice (this file is only compiled for CONFIG_APP_TDMA_TEST).
 *
 * Boots the TDMA engine with the Kconfig-selected role/slot, feeds a test
 * pattern payload every frame, and prints telemetry over UART. Shell
 * commands ("tdma ...") provide the M0 manual TX/RX bring-up path and
 * start/stop control.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/shell/shell.h>
#include <stdlib.h>
#include <errno.h>

#include "tdma.h"

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(tdma_app, CONFIG_LOG_DEFAULT_LEVEL);

#define TELEMETRY_PERIOD_MS 5000

static const char *const sync_state_str[] = {
	[TDMA_SYNC_STOPPED] = "STOPPED",
	[TDMA_SYNC_SYNCING] = "SYNCING",
	[TDMA_SYNC_RUNNING] = "RUNNING",
};

/* Received-packet accounting for loss measurement (M2). */
static uint32_t rx_per_slot[TDMA_SLOT_COUNT];
static int16_t last_rssi;
static int8_t last_snr;
static uint16_t last_frame_ctr;

static void fill_pattern(uint8_t payload[TDMA_PAYLOAD_LEN], uint8_t seq)
{
	for (int i = 0; i < TDMA_PAYLOAD_LEN; i++) {
		payload[i] = (uint8_t)(seq + i);
	}
}

static void print_telemetry(void)
{
	const struct tdma_telemetry *t = tdma_get_telemetry();

	printk("tdma: %s tx_done=%u rx_done=%u crc_err=%u bad_hdr=%u "
	       "timeouts=%u stale=%u busy_to=%u phase_err=%d us\n",
	       sync_state_str[t->sync_state], t->tx_done, t->rx_done,
	       t->rx_crc_err, t->rx_bad_header, t->slot_timeouts,
	       t->stale_retx, t->busy_timeouts, t->last_phase_err_us);
	printk("tdma: rx/slot [%u %u %u %u] last: ctr=%u rssi=%d snr=%d\n",
	       rx_per_slot[0], rx_per_slot[1], rx_per_slot[2], rx_per_slot[3],
	       last_frame_ctr, last_rssi, last_snr);
}

int main(void)
{
	struct tdma_config cfg = {
		.role = IS_ENABLED(CONFIG_TDMA_ROLE_MASTER) ?
			TDMA_ROLE_MASTER : TDMA_ROLE_SECONDARY,
		.slot_id = CONFIG_TDMA_SLOT_ID,
		.slot_duration_us = TDMA_SLOT_DURATION_US,
	};
	int64_t last_print = 0;
	uint8_t seq = 0;
	int ret;

	printk("Intercom TDMA test build: role=%s slot=%u (slot %u ms, frame %u ms)\n",
	       cfg.role == TDMA_ROLE_MASTER ? "master" : "secondary",
	       cfg.slot_id, TDMA_SLOT_DURATION_US / 1000U,
	       TDMA_FRAME_DURATION_US / 1000U);

	ret = tdma_init(&cfg);
	if (ret < 0) {
		printk("tdma_init failed: %d\n", ret);
		return ret;
	}
	printk("tdma_init OK%s\n", IS_ENABLED(CONFIG_TDMA_AUTOSTART) ?
	       ", autostarting" : " — start with 'tdma start'");

	if (IS_ENABLED(CONFIG_TDMA_AUTOSTART)) {
		ret = tdma_start();
		if (ret < 0) {
			printk("tdma_start failed: %d\n", ret);
		}
	}

	while (1) {
		struct tdma_rx_msg msg;

		/* One fresh pattern payload per frame; -EAGAIN just means
		 * the previous one hasn't gone out yet (engine stopped).
		 */
		uint8_t payload[TDMA_PAYLOAD_LEN];

		fill_pattern(payload, seq);
		if (tdma_tx_submit(payload) == 0) {
			seq++;
		}

		while (k_msgq_get(&tdma_rx_msgq, &msg, K_NO_WAIT) == 0) {
			rx_per_slot[msg.slot_id % TDMA_SLOT_COUNT]++;
			last_rssi = msg.rssi;
			last_snr = msg.snr;
			last_frame_ctr = msg.frame_ctr;
		}

		if (k_uptime_get() - last_print >= TELEMETRY_PERIOD_MS) {
			last_print = k_uptime_get();
			print_telemetry();
		}

		k_sleep(K_MSEC(TDMA_FRAME_DURATION_US / 1000U));
	}

	return 0;
}

/* ------------------------------------------------------------------ */
/* Shell commands (M0 bring-up + engine control)                       */
/* ------------------------------------------------------------------ */

static int cmd_tdma_tx(const struct shell *sh, size_t argc, char **argv)
{
	uint8_t payload[TDMA_PAYLOAD_LEN];
	static uint8_t manual_seq;
	int ret;

	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	fill_pattern(payload, manual_seq++);
	ret = tdma_manual_tx(payload);
	if (ret == 0) {
		shell_print(sh, "TX done");
	} else {
		shell_error(sh, "TX failed: %d%s", ret,
			    ret == -EBUSY ? " (engine running — 'tdma stop' first)" : "");
	}
	return ret;
}

static int cmd_tdma_rx(const struct shell *sh, size_t argc, char **argv)
{
	struct tdma_rx_msg msg;
	uint32_t seconds = 5;
	int ret;

	if (argc > 1) {
		seconds = strtoul(argv[1], NULL, 10);
		if (seconds == 0 || seconds > 60) {
			shell_error(sh, "timeout must be 1..60 s");
			return -EINVAL;
		}
	}

	shell_print(sh, "listening for %u s...", seconds);
	ret = tdma_manual_rx(seconds * 1000U, &msg);
	switch (ret) {
	case 0:
		shell_print(sh, "RX: slot=%u ctr=%u rssi=%d dBm snr=%d dB",
			    msg.slot_id, msg.frame_ctr, msg.rssi, msg.snr);
		shell_hexdump(sh, msg.payload, TDMA_PAYLOAD_LEN);
		break;
	case -EAGAIN:
		shell_print(sh, "no packet (timeout)");
		break;
	case -EBUSY:
		shell_error(sh, "engine running — 'tdma stop' first");
		break;
	default:
		shell_error(sh, "RX failed: %d", ret);
		break;
	}
	return ret;
}

static int cmd_tdma_start(const struct shell *sh, size_t argc, char **argv)
{
	int ret = tdma_start();

	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	if (ret == 0) {
		shell_print(sh, "engine started");
	} else {
		shell_error(sh, "start failed: %d", ret);
	}
	return ret;
}

static int cmd_tdma_stop(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	tdma_stop();
	shell_print(sh, "engine stopped");
	return 0;
}

static int cmd_tdma_stats(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(sh);
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	print_telemetry();
	return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(tdma_cmds,
	SHELL_CMD(tx, NULL, "Fire one packet (engine stopped)", cmd_tdma_tx),
	SHELL_CMD(rx, NULL, "Blocking receive: tdma rx [seconds]", cmd_tdma_rx),
	SHELL_CMD(start, NULL, "Start the TDMA engine", cmd_tdma_start),
	SHELL_CMD(stop, NULL, "Stop the TDMA engine", cmd_tdma_stop),
	SHELL_CMD(stats, NULL, "Print telemetry", cmd_tdma_stats),
	SHELL_SUBCMD_SET_END);
SHELL_CMD_REGISTER(tdma, &tdma_cmds, "Intercom TDMA test controls", NULL);
