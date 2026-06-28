/*
 * Copyright (c) 2019 Manivannan Sadhasivam
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/device.h>
#include <zephyr/drivers/lora.h>
#include <zephyr/drivers/spi.h>
#include <zephyr/drivers/gpio.h>
#include <errno.h>
#include <zephyr/sys/util.h>
#include <zephyr/kernel.h>

/* This file is the LoRa (Wio-SX1262) firmware variant. The telemetry console
 * lives in console.c. Exactly one is selected via the application Kconfig
 * choice so only one main() is linked.
 */
#if defined(CONFIG_APP_LORA_SEND)

#define DEFAULT_RADIO_NODE DT_ALIAS(lora0)
BUILD_ASSERT(DT_NODE_HAS_STATUS_OKAY(DEFAULT_RADIO_NODE),
	     "No default LoRa radio specified in DT");

#define MAX_DATA_LEN 16

/* Spreading factor: SF_5 or SF_6.
 * Both require a preamble of at least 12 symbols on the SX1262.
 */
#define LORA_SPREADING_FACTOR SF_5

#define LOG_LEVEL CONFIG_LOG_DEFAULT_LEVEL
#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(lora_send);

char data[MAX_DATA_LEN];

static const struct spi_dt_spec lora_spi =
	SPI_DT_SPEC_GET(DEFAULT_RADIO_NODE, SPI_WORD_SET(8) | SPI_TRANSFER_MSB, 0);

static const struct gpio_dt_spec rf_sw =
	GPIO_DT_SPEC_GET(DT_PATH(zephyr_user), rf_sw_gpios);

static void dump_radio_status(void)
{
	struct spi_buf tb, rb;
	struct spi_buf_set txs = { .buffers = &tb, .count = 1 };
	struct spi_buf_set rxs = { .buffers = &rb, .count = 1 };

	/* Wake the radio: any NSS falling edge in sleep wakes it.
	 * Send GET_STATUS, then wait for BUSY to deassert.
	 */
	uint8_t wake_tx[] = { 0xC0, 0x00 };
	uint8_t wake_rx[2] = { 0 };
	tb.buf = wake_tx; tb.len = sizeof(wake_tx);
	rb.buf = wake_rx; rb.len = sizeof(wake_rx);
	spi_transceive_dt(&lora_spi, &txs, &rxs);
	k_sleep(K_MSEC(2));

	/* Standby on RC so the chip is responsive */
	uint8_t stby_tx[] = { 0x80, 0x00 };
	tb.buf = stby_tx; tb.len = sizeof(stby_tx);
	struct spi_buf_set txs_only = { .buffers = &tb, .count = 1 };
	spi_write_dt(&lora_spi, &txs_only);
	k_sleep(K_MSEC(2));

	uint8_t irq_tx[] = { 0x12, 0x00, 0x00, 0x00 };
	uint8_t irq_rx[4] = { 0 };
	tb.buf = irq_tx; tb.len = sizeof(irq_tx);
	rb.buf = irq_rx; rb.len = sizeof(irq_rx);
	spi_transceive_dt(&lora_spi, &txs, &rxs);
	uint16_t irq = ((uint16_t)irq_rx[2] << 8) | irq_rx[3];
	printk("IRQ status: 0x%04x  (TxDone=bit0, Timeout=bit9)\n", irq);

	/* Clear any latched device errors first. XOSC_START_ERR (0x20) latches
	 * during cold-start calibration even when TX works, so wipe it here and
	 * read back a clean state. ClearDeviceErrors = 0x07, two dummy bytes.
	 */
	uint8_t clr_tx[] = { 0x07, 0x00, 0x00 };
	tb.buf = clr_tx; tb.len = sizeof(clr_tx);
	spi_write_dt(&lora_spi, &txs_only);
	k_sleep(K_MSEC(1));

	uint8_t err_tx[] = { 0x17, 0x00, 0x00, 0x00 };
	uint8_t err_rx[4] = { 0 };
	tb.buf = err_tx; tb.len = sizeof(err_tx);
	rb.buf = err_rx; rb.len = sizeof(err_rx);
	spi_transceive_dt(&lora_spi, &txs, &rxs);
	uint16_t err = ((uint16_t)err_rx[2] << 8) | err_rx[3];
	printk("Device errors: 0x%04x  (XOSC=0x20, PLL=0x40, PA=0x100)\n", err);
}

int main(void)
{
	const struct device *const lora_dev = DEVICE_DT_GET(DEFAULT_RADIO_NODE);
	struct lora_modem_config config;
	int ret;

	if (!device_is_ready(lora_dev)) {
		LOG_ERR("%s Device not ready", lora_dev->name);
		k_sleep(K_FOREVER);
	}

	/* Hold RF_SW at GND for both TX and RX on this Wio-SX1262 module */
	if (!gpio_is_ready_dt(&rf_sw)) {
		LOG_ERR("RF_SW GPIO not ready");
		k_sleep(K_FOREVER);
	}
	gpio_pin_configure_dt(&rf_sw, GPIO_OUTPUT_INACTIVE);

	config.frequency = 915000000;
	config.bandwidth = BW_500_KHZ;
	config.datarate = LORA_SPREADING_FACTOR;
	config.preamble_len = 16;
	config.coding_rate = CR_4_5;
	config.iq_inverted = false;
	config.public_network = false;
	config.tx_power = 4;
	config.tx = true;

	ret = lora_config(lora_dev, &config);
	if (ret < 0) {
		LOG_ERR("LoRa config failed");
		k_sleep(K_FOREVER);
	}

	
		ret = lora_send(lora_dev, data, MAX_DATA_LEN);
		if (ret < 0) {
			LOG_ERR("LoRa send failed");
		} else {
			LOG_INF("Data sent!");
		}
		dump_radio_status();
		k_sleep(K_MSEC(500));

}

#endif /* CONFIG_APP_LORA_SEND */
