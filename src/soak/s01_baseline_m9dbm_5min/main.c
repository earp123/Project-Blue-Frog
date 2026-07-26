/*
 * Soak test s01 - baseline link check.
 *
 * Both radios at -9 dBm (SX1262 minimum PA), the locked SF5 / BW500 / CR4-5 /
 * 915 MHz PHY, 5 minutes of continuous 4-slot TDMA exchange. Role is taken
 * from the P1.10 jumper (see src/soak/role_select.overlay), so the same image
 * flashes to both units. Emits CSV over UART for automated ingestion; see
 * src/soak/README.md for the build/capture/ingest flow.
 *
 * Guarded by CONFIG_SOAK_TEST_BASELINE: this main() only links when this test
 * is the selected one, so every soak test can own a main() without collision.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>

#if defined(CONFIG_SOAK_TEST_BASELINE)

#include "soak_run.h"

int main(void)
{
	const struct soak_params p = {
		.test_id = "s01",
		.tx_power_dbm = -9,
		.duration_s = 300, /* 5 minutes */
		.sample_period_s = 5,
	};

	soak_run(&p);

	/* Idle after the run. The "SOAK,s01,END" sentinel already told the
	 * capture side the log is complete; power-cycle (or reset) to re-run.
	 */
	while (1) {
		k_sleep(K_FOREVER);
	}

	return 0;
}

#endif /* CONFIG_SOAK_TEST_BASELINE */
