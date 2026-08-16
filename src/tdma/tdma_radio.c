/*
 * tdma_radio - radio init, slot sequences and IRQ drain. See tdma_radio.h.
 *
 * Layering (docs/tdma_layering.md): the native Zephyr sx126x driver (L0) is
 * used for INIT ONLY — device readiness, reset, TCXO setup, DIO2-as-RF-switch
 * and modulation config via lora_config(). Its lora_send/lora_recv paths are
 * never called. After init, its one autonomous runtime path (the DIO1
 * gpio_callback that submits SPI work to the system workqueue) is detached
 * here and replaced with the TDMA port's callback.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/lora.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/sys/atomic.h>
#include <errno.h>
#include <string.h>

#include "tdma.h"
#include "tdma_radio.h"
#include "tdma_port.h"
#include "tdma_buf.h"
#include "sx126x_cmd.h"

/*
 * Driver-internal header (installed vendored copy under
 * $ZEPHYR_BASE/drivers/lora/native/sx126x, include path added by
 * CMakeLists.txt). Needed for exactly one thing: reaching the driver's
 * struct gpio_callback (data->hal.dio1_cb) so it can be detached with
 * gpio_remove_callback() — no SDK patch required. The installed driver is
 * byte-identical to the repo's vendored copy by construction
 * (scripts/setup_sdk.sh), so this layout is the one actually linked.
 */
#include "sx126x.h"

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(tdma_radio, CONFIG_LOG_DEFAULT_LEVEL);

#define RADIO_NODE DT_ALIAS(lora0)

static const struct device *const lora_dev = DEVICE_DT_GET(RADIO_NODE);

/* RF_SW (P1.00): this shield's antenna switch control — output, held LOW. */
static const struct gpio_dt_spec rf_sw =
	GPIO_DT_SPEC_GET(DT_PATH(zephyr_user), rf_sw_gpios);

#define SLOT_TIMEOUT_TICKS SX126X_TIMEOUT_TICKS_FROM_US(TDMA_SLOT_ACTIVE_US)

/* IRQ sources routed to DIO1. Header CRC errors (HEADER_ERR) are included so
 * corrupted headers are counted instead of silently absorbed by the slot.
 */
#define TDMA_DIO1_MASK (SX126X_IRQ_TX_DONE | SX126X_IRQ_RX_DONE | \
			SX126X_IRQ_RX_TX_TIMEOUT | SX126X_IRQ_CRC_ERR | \
			SX126X_IRQ_HEADER_ERR)

/*
 * Latched in IrqStatus but deliberately NOT routed to DIO1 (SetDioIrqParams
 * takes the enable mask and the pin routing separately, DS 13.3.1): the
 * engine's timing is unchanged, but a window that ends in a bare timeout can
 * now be told apart from one where the receiver actually saw energy.
 */
#define TDMA_IRQ_MASK (TDMA_DIO1_MASK | SX126X_IRQ_PREAMBLE_DETECTED | \
		       SX126X_IRQ_HEADER_VALID)

BUILD_ASSERT(TDMA_PREAMBLE_SYMS >= 12,
	     "SX1262 requires >= 12 preamble symbols at SF5/SF6");

/*
 * Runtime TX power: any thread latches a request; the radio thread applies
 * it immediately before the next SetTx (chip in FS/STDBY, the datasheet-
 * legal window for SetTxParams). Sentinel = no change pending.
 */
#define TX_POWER_NONE INT32_MIN
static atomic_t pending_tx_power = ATOMIC_INIT(TX_POWER_NONE);

void tdma_radio_request_tx_power(int8_t dbm)
{
	atomic_set(&pending_tx_power, (atomic_val_t)dbm);
}

/* Radio thread only. Takes the pending request, if any, and applies it. */
static int apply_pending_tx_power(void)
{
	atomic_val_t p = atomic_set(&pending_tx_power, TX_POWER_NONE);

	if (p == TX_POWER_NONE) {
		return 0;
	}

	return sx126x_cmd_set_tx_params((int8_t)p, SX126X_RAMP_40_US);
}

/*
 * Detach the native driver's DIO1 callback. After this the driver is
 * autonomously quiescent: its only runtime entry points were this callback
 * (-> k_work on the system workqueue -> SPI) and the lora_* API calls we
 * never make. Verified against the vendored source; findings recorded in
 * docs/tdma_layering.md.
 */
static int dio1_handover(void)
{
	struct sx126x_data *drv_data = lora_dev->data;
	const struct sx126x_hal_config *drv_cfg = lora_dev->config;
	int ret;

	ret = gpio_remove_callback(drv_cfg->dio1.port, &drv_data->hal.dio1_cb);
	if (ret < 0) {
		LOG_ERR("failed to detach driver DIO1 callback: %d", ret);
		return ret;
	}

	return tdma_port_attach_dio1();
}

/*
 * Workaround — Modulation Quality with 500 kHz LoRa Bandwidth
 * (DS_SX1261-2 v1.2, chapter 15.1): clear bit 2 of reg 0x0889 for BW 500.
 * The driver applies this in its send path, which we never call, so it is
 * applied once here; the radio never sleeps, so the register survives.
 */
static int apply_tx_modulation_workaround(void)
{
	uint8_t reg;
	int ret;

	ret = sx126x_cmd_read_register(SX126X_REG_TX_MODULATION, &reg, 1);
	if (ret < 0) {
		return ret;
	}

	reg &= ~BIT(2);
	return sx126x_cmd_write_register(SX126X_REG_TX_MODULATION, &reg, 1);
}

int tdma_radio_init(const struct tdma_config *cfg)
{
	int ret;

	ARG_UNUSED(cfg);

	if (!gpio_is_ready_dt(&rf_sw)) {
		LOG_ERR("RF_SW GPIO not ready");
		return -ENODEV;
	}
	ret = gpio_pin_configure_dt(&rf_sw, GPIO_OUTPUT_INACTIVE);
	if (ret < 0) {
		return ret;
	}

	if (!device_is_ready(lora_dev)) {
		LOG_ERR("LoRa radio not ready");
		return -ENODEV;
	}

	/*
	 * L0 does the heavy lifting once: reset, TCXO (DIO3, 1.8 V, 10 ms),
	 * full calibration, DIO2-as-TX-switch, regulator, packet type,
	 * frequency, PA config, modulation params, sync word, RX gain — and
	 * image calibration for the 902-928 MHz band (sx126x_lora_config()
	 * runs CalibrateImage after the TCXO is up, so L1 need not repeat it).
	 */
	struct lora_modem_config modem_cfg = {
		.frequency = TDMA_RF_FREQ_HZ,
		.bandwidth = BW_500_KHZ,
		.datarate = SF_5,
		.coding_rate = CR_4_5,
		.preamble_len = TDMA_PREAMBLE_SYMS,
		.tx_power = TDMA_TX_POWER_DBM,
		.tx = true,
		.iq_inverted = false,
		.public_network = false,
		.packet_crc_disable = false,
	};

	ret = lora_config(lora_dev, &modem_cfg);
	if (ret < 0) {
		LOG_ERR("lora_config failed: %d", ret);
		return ret;
	}

	/* From here on the driver must never touch the bus on its own. */
	ret = dio1_handover();
	if (ret < 0) {
		return ret;
	}

	/* Remaining L1 init (plan M0 sequence). */
	ret = sx126x_cmd_set_fallback_mode(SX126X_FALLBACK_FS);
	if (ret < 0) {
		return ret;
	}

	ret = sx126x_cmd_set_buffer_base_address(TDMA_TX_BASE_ADDR,
						 TDMA_RX_BASE_ADDR);
	if (ret < 0) {
		return ret;
	}

	ret = sx126x_cmd_set_packet_params_lora(TDMA_PREAMBLE_SYMS,
						SX126X_LORA_HEADER_EXPLICIT,
						TDMA_ON_AIR_LEN,
						SX126X_LORA_CRC_ON,
						SX126X_LORA_IQ_STANDARD);
	if (ret < 0) {
		return ret;
	}

	ret = sx126x_cmd_set_dio_irq_params(TDMA_IRQ_MASK, TDMA_DIO1_MASK, 0, 0);
	if (ret < 0) {
		return ret;
	}

	/* The driver configured a 200 us PA ramp; the locked invariant is
	 * +22 dBm with a 40 us ramp (the shield's 33 uF bulk cap handles the
	 * PA transient). PA config first, then TxParams (DS 13.1.14).
	 */
	ret = sx126x_cmd_set_pa_config(SX1262_PA_DUTY_CYCLE_22DBM,
				       SX1262_HP_MAX_22DBM,
				       SX126X_DEVICE_SEL_SX1262,
				       SX126X_PA_LUT);
	if (ret < 0) {
		return ret;
	}

	ret = sx126x_cmd_set_tx_params(TDMA_TX_POWER_DBM, SX126X_RAMP_40_US);
	if (ret < 0) {
		return ret;
	}

	ret = apply_tx_modulation_workaround();
	if (ret < 0) {
		return ret;
	}

	ret = sx126x_cmd_clear_irq_status(SX126X_IRQ_ALL);
	if (ret < 0) {
		return ret;
	}

	/* Move to STDBY_XOSC so the TCXO keeps running; the first slot's
	 * SetTx/SetRx starts from a warm oscillator and the FS fallback keeps
	 * the PLL locked between slots thereafter. The radio never sleeps.
	 */
	ret = sx126x_cmd_set_standby(SX126X_STANDBY_XOSC);
	if (ret < 0) {
		return ret;
	}

	LOG_INF("radio init complete (915.0 MHz, SF5/BW500/CR4-5, +22 dBm)");
	return 0;
}

int tdma_radio_slot_tx_enter(void)
{
	int ret;

	ret = sx126x_cmd_clear_irq_status(SX126X_IRQ_ALL);
	if (ret < 0) {
		return ret;
	}

	/*
	 * RX slots now run continuously, so the chip no longer drops to the FS
	 * fallback on its own before a TX slot. Step through FS explicitly so
	 * SetTx starts from a locked PLL. This whole sequence is part of the
	 * boundary-to-air delay TDMA_TX_START_LATENCY_US measures.
	 */
	ret = sx126x_cmd_set_fs();
	if (ret < 0) {
		return ret;
	}

	ret = apply_pending_tx_power();
	if (ret < 0) {
		return ret;
	}

	return sx126x_cmd_set_tx(SLOT_TIMEOUT_TICKS);
}

int tdma_radio_slot_rx_enter(void)
{
	int ret;

	ret = sx126x_cmd_clear_irq_status(SX126X_IRQ_ALL);
	if (ret < 0) {
		return ret;
	}

	/*
	 * Continuous RX instead of a per-slot timeout. The slot boundary is
	 * already the end of the window, so the chip's timeout was redundant —
	 * and it was measured leaving every other window dead: armed, reported
	 * in RX by GetStatus, but never raising a DIO1 edge (see the arm/slot
	 * vs evt/slot tallies). Consequence: RX slots no longer raise
	 * RX_TX_TIMEOUT, so slot_timeouts only counts TX-side timeouts now.
	 */
	return sx126x_cmd_set_rx(SX126X_RX_CONTINUOUS);
}

int tdma_radio_probe_mode(uint8_t *mode)
{
	uint8_t status = 0;
	int ret = sx126x_cmd_get_status(&status);

	if (ret < 0) {
		return ret;
	}

	*mode = SX126X_STATUS_MODE(status);
	return 0;
}

int tdma_radio_stage_payload(const uint8_t payload[TDMA_PAYLOAD_LEN])
{
	return sx126x_cmd_write_buffer(TDMA_TX_BASE_ADDR + TDMA_HDR_LEN,
				       payload, TDMA_PAYLOAD_LEN);
}

int tdma_radio_write_hdr(uint8_t slot_id, uint16_t frame_ctr)
{
	uint8_t hdr[TDMA_HDR_LEN];

	tdma_buf_pack_hdr(hdr, slot_id, frame_ctr);
	return sx126x_cmd_write_buffer(TDMA_TX_BASE_ADDR, hdr, sizeof(hdr));
}

int tdma_radio_drain_dio1(struct tdma_radio_event *ev)
{
	int ret;

	memset(ev, 0, sizeof(*ev));

	/* Mandatory post-IRQ BUSY wait before any transaction (BUSY rule 2). */
	ret = sx126x_cmd_wait_busy(SX126X_CMD_BUSY_TIMEOUT_US);
	if (ret < 0) {
		return ret;
	}

	ret = sx126x_cmd_get_irq_status(&ev->irq);
	if (ret < 0) {
		return ret;
	}

	ret = sx126x_cmd_clear_irq_status(ev->irq);
	if (ret < 0) {
		return ret;
	}

	/* Read out a packet only on a clean RxDone. */
	if ((ev->irq & SX126X_IRQ_RX_DONE) &&
	    !(ev->irq & (SX126X_IRQ_CRC_ERR | SX126X_IRQ_HEADER_ERR))) {
		uint8_t len = 0, offset = 0;

		ret = sx126x_cmd_get_rx_buffer_status(&len, &offset);
		if (ret < 0) {
			return ret;
		}
		ev->raw_len = len;

		if (len == TDMA_ON_AIR_LEN) {
			ret = sx126x_cmd_read_buffer(offset, ev->raw, len);
			if (ret < 0) {
				return ret;
			}

			ret = sx126x_cmd_get_packet_status_lora(&ev->rssi,
								&ev->snr);
			if (ret < 0) {
				return ret;
			}
			ev->have_rx = true;
		}
	}

	return 0;
}

int tdma_radio_standby(void)
{
	int ret;

	ret = sx126x_cmd_clear_irq_status(SX126X_IRQ_ALL);
	if (ret < 0) {
		return ret;
	}

	return sx126x_cmd_set_standby(SX126X_STANDBY_XOSC);
}

int tdma_radio_manual_tx(uint8_t slot_id, uint16_t frame_ctr,
			 const uint8_t payload[TDMA_PAYLOAD_LEN])
{
	struct tdma_radio_event ev;
	int ret;

	ret = tdma_radio_write_hdr(slot_id, frame_ctr);
	if (ret < 0) {
		return ret;
	}

	ret = tdma_radio_stage_payload(payload);
	if (ret < 0) {
		return ret;
	}

	k_sem_reset(&tdma_dio1_sem);

	ret = sx126x_cmd_clear_irq_status(SX126X_IRQ_ALL);
	if (ret < 0) {
		return ret;
	}

	ret = apply_pending_tx_power();
	if (ret < 0) {
		return ret;
	}

	ret = sx126x_cmd_set_tx(SX126X_TIMEOUT_TICKS_FROM_US(500000));
	if (ret < 0) {
		return ret;
	}

	if (k_sem_take(&tdma_dio1_sem, K_MSEC(1000)) != 0) {
		return -ETIMEDOUT;
	}

	ret = tdma_radio_drain_dio1(&ev);
	if (ret < 0) {
		return ret;
	}

	return (ev.irq & SX126X_IRQ_TX_DONE) ? 0 : -EIO;
}

int tdma_radio_manual_rx(uint32_t timeout_ms, struct tdma_radio_event *ev)
{
	int ret;

	k_sem_reset(&tdma_dio1_sem);

	ret = sx126x_cmd_clear_irq_status(SX126X_IRQ_ALL);
	if (ret < 0) {
		return ret;
	}

	ret = sx126x_cmd_set_rx(SX126X_TIMEOUT_TICKS_FROM_US(
					(uint64_t)timeout_ms * 1000U));
	if (ret < 0) {
		return ret;
	}

	if (k_sem_take(&tdma_dio1_sem, K_MSEC(timeout_ms + 500)) != 0) {
		/* No IRQ at all — abort the window and park the radio. */
		(void)tdma_radio_standby();
		return -ETIMEDOUT;
	}

	ret = tdma_radio_drain_dio1(ev);
	if (ret < 0) {
		return ret;
	}

	if (ev->irq & SX126X_IRQ_RX_TX_TIMEOUT) {
		return -EAGAIN;
	}

	return ev->have_rx ? 0 : -EIO;
}
