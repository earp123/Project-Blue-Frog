/*
 * radio_cfg - staged LoRa modem configuration. See radio_cfg.h.
 */

#include <zephyr/kernel.h>

#if defined(CONFIG_APP_CONSOLE)

#include "radio_cfg.h"

#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/sys/util.h>
#include <zephyr/logging/log.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

LOG_MODULE_REGISTER(radio_cfg, LOG_LEVEL_INF);

static const struct device *const lora_dev = DEVICE_DT_GET(DT_ALIAS(lora0));
static const struct gpio_dt_spec rf_sw =
	GPIO_DT_SPEC_GET(DT_PATH(zephyr_user), rf_sw_gpios);

/* Shadow = what the UI edits; applied = what last reached the radio. */
static struct lora_modem_config g_shadow;
static struct lora_modem_config g_applied;

/* Step tables for the enum parameters (wrap on step). */
static const enum lora_signal_bandwidth bw_set[] = {
	BW_125_KHZ, BW_250_KHZ, BW_500_KHZ,
};
static const enum lora_coding_rate cr_set[] = {
	CR_4_5, CR_4_6, CR_4_7, CR_4_8,
};

static int bw_index(enum lora_signal_bandwidth bw)
{
	for (int i = 0; i < (int)ARRAY_SIZE(bw_set); i++) {
		if (bw_set[i] == bw) {
			return i;
		}
	}
	return ARRAY_SIZE(bw_set) - 1; /* default to widest if unknown */
}

static int cr_index(enum lora_coding_rate cr)
{
	for (int i = 0; i < (int)ARRAY_SIZE(cr_set); i++) {
		if (cr_set[i] == cr) {
			return i;
		}
	}
	return 0;
}

void radio_cfg_init(void)
{
	/* Defaults: the project's proven-good config (matches src/main.c):
	 * 915 MHz, BW 500 kHz, SF5, CR 4/5, preamble 16, +4 dBm, TX, CRC on.
	 */
	g_shadow = (struct lora_modem_config){
		.frequency = 915000000U,
		.bandwidth = BW_500_KHZ,
		.datarate = SF_5,
		.coding_rate = CR_4_5,
		.preamble_len = 16,
		.tx_power = 4,
		.tx = true,
		.iq_inverted = false,
		.public_network = false,
		.packet_crc_disable = false,
	};

	/* Zeroed applied => dirty until the first successful apply. */
	memset(&g_applied, 0, sizeof(g_applied));

	/* RF_SW held at GND (inactive) for both TX and RX on this Wio-SX1262. */
	if (gpio_is_ready_dt(&rf_sw)) {
		gpio_pin_configure_dt(&rf_sw, GPIO_OUTPUT_INACTIVE);
	} else {
		LOG_WRN("RF_SW GPIO not ready");
	}
}

const struct lora_modem_config *radio_cfg_shadow(void)
{
	return &g_shadow;
}

const struct lora_modem_config *radio_cfg_applied(void)
{
	return &g_applied;
}

bool radio_cfg_dirty(void)
{
	return memcmp(&g_shadow, &g_applied, sizeof(g_shadow)) != 0;
}

bool radio_cfg_radio_ready(void)
{
	return device_is_ready(lora_dev);
}

/* ---- Setters / steppers (shadow only) ---------------------------------- */

bool radio_cfg_set_freq(uint32_t hz)
{
	bool ok = (hz >= RADIO_FREQ_MIN_HZ && hz <= RADIO_FREQ_MAX_HZ);

	g_shadow.frequency = CLAMP(hz, RADIO_FREQ_MIN_HZ, RADIO_FREQ_MAX_HZ);
	return ok;
}

bool radio_cfg_set_sf(uint8_t sf)
{
	bool ok = (sf >= SF_5 && sf <= SF_12);

	g_shadow.datarate = (enum lora_datarate)CLAMP(sf, SF_5, SF_12);
	return ok;
}

void radio_cfg_step_sf(int dir)
{
	int sf = (int)g_shadow.datarate + (dir < 0 ? -1 : 1);

	g_shadow.datarate = (enum lora_datarate)CLAMP(sf, SF_5, SF_12);
}

void radio_cfg_step_bw(int dir)
{
	int n = ARRAY_SIZE(bw_set);
	int i = (bw_index(g_shadow.bandwidth) + (dir < 0 ? -1 : 1) + n) % n;

	g_shadow.bandwidth = bw_set[i];
}

void radio_cfg_step_cr(int dir)
{
	int n = ARRAY_SIZE(cr_set);
	int i = (cr_index(g_shadow.coding_rate) + (dir < 0 ? -1 : 1) + n) % n;

	g_shadow.coding_rate = cr_set[i];
}

bool radio_cfg_set_power(int dbm)
{
	bool ok = (dbm >= RADIO_PWR_MIN_DBM && dbm <= RADIO_PWR_MAX_DBM);

	g_shadow.tx_power = (int8_t)CLAMP(dbm, RADIO_PWR_MIN_DBM, RADIO_PWR_MAX_DBM);
	return ok;
}

void radio_cfg_step_power(int dir)
{
	radio_cfg_set_power(g_shadow.tx_power + (dir < 0 ? -1 : 1));
}

bool radio_cfg_set_preamble(uint32_t n)
{
	bool ok = (n >= RADIO_PREAMBLE_MIN && n <= UINT16_MAX);

	g_shadow.preamble_len = (uint16_t)CLAMP(n, RADIO_PREAMBLE_MIN,
						(uint32_t)UINT16_MAX);
	return ok;
}

void radio_cfg_step_preamble(int dir)
{
	radio_cfg_set_preamble((uint32_t)g_shadow.preamble_len + (dir < 0 ? -1 : 1));
}

void radio_cfg_toggle_crc(void)
{
	g_shadow.packet_crc_disable = !g_shadow.packet_crc_disable;
}

void radio_cfg_toggle_iq(void)
{
	g_shadow.iq_inverted = !g_shadow.iq_inverted;
}

void radio_cfg_toggle_pubnet(void)
{
	g_shadow.public_network = !g_shadow.public_network;
}

/* ---- Formatting -------------------------------------------------------- */

static void freq_str_of(const struct lora_modem_config *c, char *out, size_t n)
{
	uint32_t mhz = c->frequency / 1000000U;
	uint32_t tenths = (c->frequency % 1000000U) / 100000U;

	snprintf(out, n, "%u.%u MHz", (unsigned)mhz, (unsigned)tenths);
}

void radio_cfg_freq_str(char *out, size_t n)
{
	freq_str_of(&g_shadow, out, n);
}

void radio_cfg_sf_str(char *out, size_t n)
{
	snprintf(out, n, "SF%d", (int)g_shadow.datarate);
}

void radio_cfg_bw_str(char *out, size_t n)
{
	snprintf(out, n, "%d kHz", (int)g_shadow.bandwidth);
}

void radio_cfg_cr_str(char *out, size_t n)
{
	snprintf(out, n, "4/%d", (int)g_shadow.coding_rate + 4);
}

void radio_cfg_power_str(char *out, size_t n)
{
	snprintf(out, n, "%+d dBm", (int)g_shadow.tx_power);
}

void radio_cfg_preamble_str(char *out, size_t n)
{
	snprintf(out, n, "%u", (unsigned)g_shadow.preamble_len);
}

void radio_cfg_crc_str(char *out, size_t n)
{
	snprintf(out, n, "%s", g_shadow.packet_crc_disable ? "OFF" : "ON");
}

void radio_cfg_iq_str(char *out, size_t n)
{
	snprintf(out, n, "%s", g_shadow.iq_inverted ? "INV" : "NORM");
}

void radio_cfg_pubnet_str(char *out, size_t n)
{
	snprintf(out, n, "%s", g_shadow.public_network ? "PUB" : "PRIV");
}

void radio_cfg_summary(char *out, size_t n)
{
	const struct lora_modem_config *c = &g_applied;

	if (c->frequency == 0) {
		snprintf(out, n, "(not applied)");
		return;
	}

	uint32_t mhz = c->frequency / 1000000U;
	uint32_t tenths = (c->frequency % 1000000U) / 100000U;

	snprintf(out, n, "%u.%uM SF%d BW%d CR4/%d %+ddBm",
		 (unsigned)mhz, (unsigned)tenths, (int)c->datarate,
		 (int)c->bandwidth, (int)c->coding_rate + 4, (int)c->tx_power);
}

/* ---- Validation / apply ------------------------------------------------ */

int radio_cfg_validate(char *err, size_t errlen)
{
	const struct lora_modem_config *c = &g_shadow;

	if (c->frequency < RADIO_FREQ_MIN_HZ || c->frequency > RADIO_FREQ_MAX_HZ) {
		snprintf(err, errlen, "Freq out of 902-928MHz");
		return -EINVAL;
	}
	if (c->tx_power < RADIO_PWR_MIN_DBM || c->tx_power > RADIO_PWR_MAX_DBM) {
		snprintf(err, errlen, "Power out of -9..+22dBm");
		return -EINVAL;
	}
	if (c->preamble_len < RADIO_PREAMBLE_MIN) {
		snprintf(err, errlen, "Preamble < %u", RADIO_PREAMBLE_MIN);
		return -EINVAL;
	}
	/* SX126x: SF5/SF6 require a preamble of at least 12 symbols (see README).
	 * Additional documented SF/BW pairing constraints would be added here.
	 */
	if ((c->datarate == SF_5 || c->datarate == SF_6) &&
	    c->preamble_len < RADIO_PREAMBLE_MIN_SF56) {
		snprintf(err, errlen, "SF5/6 need preamble >= %u",
			 RADIO_PREAMBLE_MIN_SF56);
		return -EINVAL;
	}

	if (err && errlen) {
		err[0] = '\0';
	}
	return 0;
}

int radio_cfg_apply(void)
{
	char err[40];
	int rc;

	if (!device_is_ready(lora_dev)) {
		LOG_ERR("LoRa device not ready");
		return -ENODEV;
	}

	rc = radio_cfg_validate(err, sizeof(err));
	if (rc < 0) {
		LOG_ERR("cfg invalid: %s", err);
		return rc;
	}

	/* lora_config() takes a non-const pointer; apply to a temp so g_applied
	 * is only updated (and dirty cleared) on success.
	 */
	struct lora_modem_config tmp = g_shadow;

	rc = lora_config(lora_dev, &tmp);
	if (rc < 0) {
		LOG_ERR("lora_config failed: %d", rc);
		return rc;
	}

	g_applied = g_shadow;
	LOG_INF("applied: %uHz SF%d BW%d CR4/%d pre%u %+ddBm",
		(unsigned)g_applied.frequency, (int)g_applied.datarate,
		(int)g_applied.bandwidth, (int)g_applied.coding_rate + 4,
		(unsigned)g_applied.preamble_len, (int)g_applied.tx_power);
	return 0;
}

#endif /* CONFIG_APP_CONSOLE */
