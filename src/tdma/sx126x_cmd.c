/*
 * sx126x_cmd - app-owned SX1262 command layer (L1). See sx126x_cmd.h.
 *
 * Owns its own SPI and GPIO dt-specs, resolved from the same lora0
 * devicetree node the native driver uses; the Zephyr SPI subsystem
 * serializes bus access, so the driver's init-time transactions and these
 * runtime transactions can share spi2 safely.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/spi.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/util.h>
#include <errno.h>

#include "sx126x_cmd.h"

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(sx126x_cmd, CONFIG_LOG_DEFAULT_LEVEL);

#define RADIO_NODE DT_ALIAS(lora0)
BUILD_ASSERT(DT_NODE_HAS_STATUS_OKAY(RADIO_NODE), "lora0 alias missing");

static const struct spi_dt_spec spi_bus =
	SPI_DT_SPEC_GET(RADIO_NODE, SPI_WORD_SET(8) | SPI_TRANSFER_MSB, 0);
static const struct gpio_dt_spec busy_gpio =
	GPIO_DT_SPEC_GET(RADIO_NODE, busy_gpios);
static const struct gpio_dt_spec dio1_gpio =
	GPIO_DT_SPEC_GET(RADIO_NODE, dio1_gpios);

const struct gpio_dt_spec *sx126x_cmd_dio1_spec(void)
{
	return &dio1_gpio;
}

int sx126x_cmd_wait_busy(uint32_t timeout_us)
{
	uint32_t waited = 0;

	while (true) {
		int val = gpio_pin_get_dt(&busy_gpio);

		if (val < 0) {
			return val;
		}
		if (val == 0) {
			return 0;
		}
		if (waited >= timeout_us) {
			LOG_ERR("BUSY stuck high for %u us", timeout_us);
			return -ETIMEDOUT;
		}
		k_busy_wait(2);
		waited += 2;
	}
}

/*
 * One SPI transaction: a short command header, then an optional data phase
 * (write if wr != NULL, read into rd if rd != NULL). Always preceded by the
 * BUSY-low wait — the single gating rule (sx126x_cmd.h).
 */
static int xfer(const uint8_t *hdr, size_t hdr_len,
		const uint8_t *wr, uint8_t *rd, size_t len)
{
	uint8_t hdr_rx[4];
	int ret;

	__ASSERT_NO_MSG(hdr_len <= sizeof(hdr_rx));

	struct spi_buf tx_bufs[2] = {
		{ .buf = (uint8_t *)hdr, .len = hdr_len },
		{ .buf = (uint8_t *)wr, .len = len },
	};
	struct spi_buf rx_bufs[2] = {
		{ .buf = hdr_rx, .len = hdr_len },
		{ .buf = rd, .len = len },
	};
	int count = (len == 0) ? 1 : 2;
	struct spi_buf_set tx_set = { .buffers = tx_bufs, .count = count };
	struct spi_buf_set rx_set = { .buffers = rx_bufs, .count = count };

	ret = sx126x_cmd_wait_busy(SX126X_CMD_BUSY_TIMEOUT_US);
	if (ret < 0) {
		return ret;
	}

	return spi_transceive_dt(&spi_bus, &tx_set, (rd != NULL) ? &rx_set : NULL);
}

static int cmd_write(uint8_t opcode, const uint8_t *params, size_t len)
{
	uint8_t hdr[1] = { opcode };

	return xfer(hdr, sizeof(hdr), params, NULL, len);
}

/* Read-type commands clock out a status byte after the opcode, then data
 * (DS 13.5): opcode, NOP(status), data...
 */
static int cmd_read(uint8_t opcode, uint8_t *data, size_t len)
{
	uint8_t hdr[2] = { opcode, 0x00 };

	return xfer(hdr, sizeof(hdr), NULL, data, len);
}

int sx126x_cmd_get_status(uint8_t *status)
{
	/* GetStatus (DS 13.5.1): status returned during the single NOP byte. */
	uint8_t hdr[1] = { SX126X_CMD_GET_STATUS };

	return xfer(hdr, sizeof(hdr), NULL, status, 1);
}

int sx126x_cmd_set_standby(uint8_t mode)
{
	return cmd_write(SX126X_CMD_SET_STANDBY, &mode, 1);
}

int sx126x_cmd_set_fs(void)
{
	return cmd_write(SX126X_CMD_SET_FS, NULL, 0);
}

int sx126x_cmd_set_tx(uint32_t timeout_ticks)
{
	/* SetTx (DS 13.1.4): 24-bit timeout, 15.625 us units, big-endian. */
	uint8_t buf[3];

	sys_put_be24(timeout_ticks, buf);
	return cmd_write(SX126X_CMD_SET_TX, buf, sizeof(buf));
}

int sx126x_cmd_set_rx(uint32_t timeout_ticks)
{
	/* SetRx (DS 13.1.5): 24-bit timeout, 15.625 us units, big-endian. */
	uint8_t buf[3];

	sys_put_be24(timeout_ticks, buf);
	return cmd_write(SX126X_CMD_SET_RX, buf, sizeof(buf));
}

int sx126x_cmd_set_fallback_mode(uint8_t mode)
{
	/* SetRxTxFallbackMode (DS 13.1.15): mode chip enters after TX/RX. */
	return cmd_write(SX126X_CMD_SET_RX_TX_FALLBACK_MODE, &mode, 1);
}

int sx126x_cmd_set_buffer_base_address(uint8_t tx_base, uint8_t rx_base)
{
	/* SetBufferBaseAddress (DS 13.4.8). */
	uint8_t buf[2] = { tx_base, rx_base };

	return cmd_write(SX126X_CMD_SET_BUFFER_BASE_ADDRESS, buf, sizeof(buf));
}

int sx126x_cmd_set_packet_params_lora(uint16_t preamble_syms, uint8_t header_type,
				      uint8_t payload_len, uint8_t crc_mode,
				      uint8_t iq_mode)
{
	/* SetPacketParams, LoRa layout (DS 13.4.6, Table 13-45). */
	uint8_t buf[6];

	sys_put_be16(preamble_syms, &buf[0]);
	buf[2] = header_type;
	buf[3] = payload_len;
	buf[4] = crc_mode;
	buf[5] = iq_mode;

	return cmd_write(SX126X_CMD_SET_PACKET_PARAMS, buf, sizeof(buf));
}

int sx126x_cmd_set_dio_irq_params(uint16_t irq_mask, uint16_t dio1_mask,
				  uint16_t dio2_mask, uint16_t dio3_mask)
{
	/* SetDioIrqParams (DS 13.3.1). */
	uint8_t buf[8];

	sys_put_be16(irq_mask, &buf[0]);
	sys_put_be16(dio1_mask, &buf[2]);
	sys_put_be16(dio2_mask, &buf[4]);
	sys_put_be16(dio3_mask, &buf[6]);

	return cmd_write(SX126X_CMD_SET_DIO_IRQ_PARAMS, buf, sizeof(buf));
}

int sx126x_cmd_get_irq_status(uint16_t *irq)
{
	/* GetIrqStatus (DS 13.3.3). */
	uint8_t buf[2];
	int ret;

	ret = cmd_read(SX126X_CMD_GET_IRQ_STATUS, buf, sizeof(buf));
	if (ret == 0) {
		*irq = sys_get_be16(buf);
	}

	return ret;
}

int sx126x_cmd_clear_irq_status(uint16_t mask)
{
	/* ClearIrqStatus (DS 13.3.4). */
	uint8_t buf[2];

	sys_put_be16(mask, buf);
	return cmd_write(SX126X_CMD_CLR_IRQ_STATUS, buf, sizeof(buf));
}

int sx126x_cmd_get_rx_buffer_status(uint8_t *payload_len, uint8_t *start_offset)
{
	/* GetRxBufferStatus (DS 13.5.3): PayloadLengthRx, RxStartBufferPointer. */
	uint8_t buf[2];
	int ret;

	ret = cmd_read(SX126X_CMD_GET_RX_BUFFER_STATUS, buf, sizeof(buf));
	if (ret == 0) {
		*payload_len = buf[0];
		*start_offset = buf[1];
	}

	return ret;
}

int sx126x_cmd_get_packet_status_lora(int16_t *rssi_dbm, int8_t *snr_db)
{
	/*
	 * GetPacketStatus, LoRa layout (DS 13.5.4): RssiPkt (dBm = -val/2),
	 * SnrPkt (dB = signed val/4), SignalRssiPkt (unused here).
	 */
	uint8_t buf[3];
	int ret;

	ret = cmd_read(SX126X_CMD_GET_PACKET_STATUS, buf, sizeof(buf));
	if (ret == 0) {
		*rssi_dbm = -((int16_t)buf[0]) / 2;
		*snr_db = ((int8_t)buf[1]) / 4;
	}

	return ret;
}

int sx126x_cmd_write_buffer(uint8_t offset, const uint8_t *data, size_t len)
{
	/* WriteBuffer (DS 13.2.3): offset is absolute in the 256-byte buffer. */
	uint8_t hdr[2] = { SX126X_CMD_WRITE_BUFFER, offset };

	return xfer(hdr, sizeof(hdr), data, NULL, len);
}

int sx126x_cmd_read_buffer(uint8_t offset, uint8_t *data, size_t len)
{
	/* ReadBuffer (DS 13.2.4): opcode, offset, NOP, then data. */
	uint8_t hdr[3] = { SX126X_CMD_READ_BUFFER, offset, 0x00 };

	return xfer(hdr, sizeof(hdr), NULL, data, len);
}

int sx126x_cmd_write_register(uint16_t addr, const uint8_t *data, size_t len)
{
	/* WriteRegister (DS 13.2.1): opcode, 16-bit address, data. */
	uint8_t hdr[3] = { SX126X_CMD_WRITE_REGISTER };

	sys_put_be16(addr, &hdr[1]);
	return xfer(hdr, sizeof(hdr), data, NULL, len);
}

int sx126x_cmd_read_register(uint16_t addr, uint8_t *data, size_t len)
{
	/* ReadRegister (DS 13.2.2): opcode, 16-bit address, NOP, then data. */
	uint8_t hdr[4] = { SX126X_CMD_READ_REGISTER };

	sys_put_be16(addr, &hdr[1]);
	hdr[3] = 0x00;
	return xfer(hdr, sizeof(hdr), NULL, data, len);
}

int sx126x_cmd_calibrate_image(uint32_t freq_hz)
{
	/* CalibrateImage (DS 13.1.13, Table 13-19: band edge bytes). */
	uint8_t buf[2];

	if (freq_hz > 900000000UL) {
		buf[0] = 0xE1;		/* 902-928 MHz */
		buf[1] = 0xE9;
	} else if (freq_hz > 850000000UL) {
		buf[0] = 0xD7;		/* 863-870 MHz */
		buf[1] = 0xDB;
	} else if (freq_hz > 770000000UL) {
		buf[0] = 0xC1;		/* 779-787 MHz */
		buf[1] = 0xC5;
	} else if (freq_hz > 460000000UL) {
		buf[0] = 0x75;		/* 470-510 MHz */
		buf[1] = 0x81;
	} else {
		buf[0] = 0x6B;		/* 430-440 MHz */
		buf[1] = 0x6F;
	}

	return cmd_write(SX126X_CMD_CALIBRATE_IMAGE, buf, sizeof(buf));
}

int sx126x_cmd_set_pa_config(uint8_t pa_duty_cycle, uint8_t hp_max,
			     uint8_t device_sel, uint8_t pa_lut)
{
	/* SetPaConfig (DS 13.1.14). */
	uint8_t buf[4] = { pa_duty_cycle, hp_max, device_sel, pa_lut };

	return cmd_write(SX126X_CMD_SET_PA_CONFIG, buf, sizeof(buf));
}

int sx126x_cmd_set_tx_params(int8_t power_dbm, uint8_t ramp_time)
{
	/* SetTxParams (DS 13.4.4). */
	uint8_t buf[2] = { (uint8_t)power_dbm, ramp_time };

	return cmd_write(SX126X_CMD_SET_TX_PARAMS, buf, sizeof(buf));
}

int sx126x_cmd_init(void)
{
	if (!spi_is_ready_dt(&spi_bus)) {
		LOG_ERR("SPI bus not ready");
		return -ENODEV;
	}
	if (!gpio_is_ready_dt(&busy_gpio) || !gpio_is_ready_dt(&dio1_gpio)) {
		LOG_ERR("BUSY/DIO1 GPIO not ready");
		return -ENODEV;
	}

	/* BUSY/DIO1 pin directions are configured by the driver's own init
	 * (L0), which runs first; nothing to re-configure here.
	 */
	return 0;
}
