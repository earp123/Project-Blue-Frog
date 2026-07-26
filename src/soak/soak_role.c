/*
 * soak_role - read the bench master/secondary jumper. See soak_role.h.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>

#if defined(CONFIG_APP_SOAK)

#include "soak_role.h"

#include <zephyr/drivers/gpio.h>

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(soak_role, CONFIG_LOG_DEFAULT_LEVEL);

/* Bias (pull-down) and polarity (active-high) come from the DT flags in
 * src/soak/role_select.overlay; configuring as GPIO_INPUT applies them.
 */
static const struct gpio_dt_spec role_sel =
	GPIO_DT_SPEC_GET(DT_PATH(zephyr_user), role_select_gpios);

enum tdma_role soak_role_get(void)
{
	int val;

	if (!gpio_is_ready_dt(&role_sel)) {
		LOG_ERR("role-select GPIO not ready; defaulting to SECONDARY");
		return TDMA_ROLE_SECONDARY;
	}

	if (gpio_pin_configure_dt(&role_sel, GPIO_INPUT) < 0) {
		LOG_ERR("role-select GPIO configure failed; defaulting to SECONDARY");
		return TDMA_ROLE_SECONDARY;
	}

	/* Logical (active) level: jumper -> 3V3 reads 1 (master). */
	val = gpio_pin_get_dt(&role_sel);
	return (val > 0) ? TDMA_ROLE_MASTER : TDMA_ROLE_SECONDARY;
}

uint8_t soak_role_slot(enum tdma_role role)
{
	return (role == TDMA_ROLE_MASTER) ? 0 : 1;
}

#endif /* CONFIG_APP_SOAK */
