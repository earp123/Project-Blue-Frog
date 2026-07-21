/*
 * sx126x_cmd - app-owned SX1262 command layer (L1).
 *
 * Thin, stateless wrappers around the raw SPI opcodes the TDMA engine needs,
 * with all BUSY-line gating in one choke point. No sequencing, no policy —
 * L2 (tdma_radio/tdma_core) owns that. This is deliberate duplication of the
 * native driver's transport (~150 lines) rather than patching the vendored
 * driver to export internals: the patch surface stays tiny and the
 * timing-critical path is owned outright.
 *
 * Opcode values and command formats per the SX1262 datasheet,
 * DS_SX1261-2 v1.2 ("DS"), chapter 13; section numbers cited per command.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef APP_TDMA_SX126X_CMD_H_
#define APP_TDMA_SX126X_CMD_H_

#include <zephyr/drivers/gpio.h>
#include <stdint.h>
#include <stddef.h>

/* Operational mode commands (DS 13.1) */
#define SX126X_CMD_SET_STANDBY			0x80	/* DS 13.1.2 */
#define SX126X_CMD_SET_FS			0xC1	/* DS 13.1.3 */
#define SX126X_CMD_SET_TX			0x83	/* DS 13.1.4 */
#define SX126X_CMD_SET_RX			0x82	/* DS 13.1.5 */
#define SX126X_CMD_CALIBRATE_IMAGE		0x98	/* DS 13.1.13 */
#define SX126X_CMD_SET_PA_CONFIG		0x95	/* DS 13.1.14 */
#define SX126X_CMD_SET_RX_TX_FALLBACK_MODE	0x93	/* DS 13.1.15 */

/* Register / buffer access (DS 13.2) */
#define SX126X_CMD_WRITE_REGISTER		0x0D	/* DS 13.2.1 */
#define SX126X_CMD_READ_REGISTER		0x1D	/* DS 13.2.2 */
#define SX126X_CMD_WRITE_BUFFER			0x0E	/* DS 13.2.3 */
#define SX126X_CMD_READ_BUFFER			0x1E	/* DS 13.2.4 */

/* DIO / IRQ control (DS 13.3) */
#define SX126X_CMD_SET_DIO_IRQ_PARAMS		0x08	/* DS 13.3.1 */
#define SX126X_CMD_GET_IRQ_STATUS		0x12	/* DS 13.3.3 */
#define SX126X_CMD_CLR_IRQ_STATUS		0x02	/* DS 13.3.4 */

/* RF, modulation, packet (DS 13.4) */
#define SX126X_CMD_SET_TX_PARAMS		0x8E	/* DS 13.4.4 */
#define SX126X_CMD_SET_PACKET_PARAMS		0x8C	/* DS 13.4.6 */
#define SX126X_CMD_SET_BUFFER_BASE_ADDRESS	0x8F	/* DS 13.4.8 */

/* Status (DS 13.5) */
#define SX126X_CMD_GET_STATUS			0xC0	/* DS 13.5.1 */
#define SX126X_CMD_GET_RX_BUFFER_STATUS		0x13	/* DS 13.5.3 */
#define SX126X_CMD_GET_PACKET_STATUS		0x14	/* DS 13.5.4 */

/* IRQ flag bits (DS 13.3.1, Table 13-29) */
#define SX126X_IRQ_TX_DONE		BIT(0)
#define SX126X_IRQ_RX_DONE		BIT(1)
#define SX126X_IRQ_PREAMBLE_DETECTED	BIT(2)
#define SX126X_IRQ_SYNC_WORD_VALID	BIT(3)
#define SX126X_IRQ_HEADER_VALID		BIT(4)
#define SX126X_IRQ_HEADER_ERR		BIT(5)
#define SX126X_IRQ_CRC_ERR		BIT(6)
#define SX126X_IRQ_CAD_DONE		BIT(7)
#define SX126X_IRQ_CAD_DETECTED		BIT(8)
#define SX126X_IRQ_RX_TX_TIMEOUT	BIT(9)
#define SX126X_IRQ_ALL			0x03FF

/*
 * GetStatus byte layout (DS 13.5.1, Table 13-76): current chip mode in bits
 * 6:4, status of the last command in bits 3:1.
 */
#define SX126X_STATUS_MODE(s)		(((s) >> 4) & 0x07)
#define SX126X_STATUS_CMD(s)		(((s) >> 1) & 0x07)

#define SX126X_MODE_STBY_RC		0x02
#define SX126X_MODE_STBY_XOSC		0x03
#define SX126X_MODE_FS			0x04
#define SX126X_MODE_RX			0x05
#define SX126X_MODE_TX			0x06

/* SetStandby modes (DS 13.1.2, Table 13-2) */
#define SX126X_STANDBY_RC		0x00
#define SX126X_STANDBY_XOSC		0x01

/* SetRxTxFallbackMode values (DS 13.1.15, Table 13-23) */
#define SX126X_FALLBACK_FS		0x40
#define SX126X_FALLBACK_STDBY_XOSC	0x30
#define SX126X_FALLBACK_STDBY_RC	0x20

/* SetPacketParams LoRa fields (DS 13.4.6) */
#define SX126X_LORA_HEADER_EXPLICIT	0x00
#define SX126X_LORA_HEADER_IMPLICIT	0x01
#define SX126X_LORA_CRC_OFF		0x00
#define SX126X_LORA_CRC_ON		0x01
#define SX126X_LORA_IQ_STANDARD		0x00
#define SX126X_LORA_IQ_INVERTED		0x01

/* SetTxParams ramp times (DS 13.4.4, Table 13-41) */
#define SX126X_RAMP_10_US		0x00
#define SX126X_RAMP_20_US		0x01
#define SX126X_RAMP_40_US		0x02
#define SX126X_RAMP_200_US		0x04

/* SetPaConfig values for the SX1262 +22 dBm PA (DS 13.1.14, Table 13-21) */
#define SX1262_PA_DUTY_CYCLE_22DBM	0x04
#define SX1262_HP_MAX_22DBM		0x07
#define SX126X_DEVICE_SEL_SX1262	0x00
#define SX126X_PA_LUT			0x01

/* Registers (DS chapter 12 / errata workarounds in chapter 15) */
#define SX126X_REG_TX_MODULATION	0x0889	/* DS 15.1 (500 kHz BW quality) */

/* SetTx/SetRx timeout ticks are 15.625 us each (DS 13.1.4/13.1.5):
 * ticks = us / 15.625 = us * 64 / 1000, exact in integer math.
 */
#define SX126X_TIMEOUT_TICKS_FROM_US(us)	((uint32_t)(((uint64_t)(us) * 64U) / 1000U))

/*
 * SetRx timeout special values (DS 13.1.5, Table 13-3):
 *   0x000000 - single mode, no timeout: stays in RX until a packet lands;
 *   0xFFFFFF - continuous mode: stays in RX and keeps receiving past RxDone.
 */
#define SX126X_RX_CONTINUOUS			0xFFFFFFUL

/* Bounded BUSY spin budget. Plain commands deassert BUSY in < 1 ms;
 * calibration can hold it for a few ms. Timing out here is fatal to the
 * engine (L2 stops and counts busy_timeouts).
 */
#define SX126X_CMD_BUSY_TIMEOUT_US	20000

int sx126x_cmd_init(void);

/*
 * BUSY gating (all of it lives here):
 *  1. every transaction waits for BUSY low before driving NSS;
 *  2. after a DIO1 IRQ, L2 calls this explicitly before anything else;
 *  3. back-to-back reads need no wait — the pre-transaction poll then falls
 *     straight through, so the single rule stays cheap and universal.
 */
int sx126x_cmd_wait_busy(uint32_t timeout_us);

int sx126x_cmd_get_status(uint8_t *status);
int sx126x_cmd_set_standby(uint8_t mode);
int sx126x_cmd_set_fs(void);
int sx126x_cmd_set_tx(uint32_t timeout_ticks);
int sx126x_cmd_set_rx(uint32_t timeout_ticks);
int sx126x_cmd_set_fallback_mode(uint8_t mode);
int sx126x_cmd_set_buffer_base_address(uint8_t tx_base, uint8_t rx_base);
int sx126x_cmd_set_packet_params_lora(uint16_t preamble_syms, uint8_t header_type,
				      uint8_t payload_len, uint8_t crc_mode,
				      uint8_t iq_mode);
int sx126x_cmd_set_dio_irq_params(uint16_t irq_mask, uint16_t dio1_mask,
				  uint16_t dio2_mask, uint16_t dio3_mask);
int sx126x_cmd_get_irq_status(uint16_t *irq);
int sx126x_cmd_clear_irq_status(uint16_t mask);
int sx126x_cmd_get_rx_buffer_status(uint8_t *payload_len, uint8_t *start_offset);
int sx126x_cmd_get_packet_status_lora(int16_t *rssi_dbm, int8_t *snr_db);
int sx126x_cmd_write_buffer(uint8_t offset, const uint8_t *data, size_t len);
int sx126x_cmd_read_buffer(uint8_t offset, uint8_t *data, size_t len);
int sx126x_cmd_write_register(uint16_t addr, const uint8_t *data, size_t len);
int sx126x_cmd_read_register(uint16_t addr, uint8_t *data, size_t len);
int sx126x_cmd_calibrate_image(uint32_t freq_hz);
int sx126x_cmd_set_pa_config(uint8_t pa_duty_cycle, uint8_t hp_max,
			     uint8_t device_sel, uint8_t pa_lut);
int sx126x_cmd_set_tx_params(int8_t power_dbm, uint8_t ramp_time);

/* DIO1 devicetree spec (same lora0 node), for the L2 port's IRQ handover. */
const struct gpio_dt_spec *sx126x_cmd_dio1_spec(void);

#endif /* APP_TDMA_SX126X_CMD_H_ */
