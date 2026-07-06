/*
 * tdma_radio - slot TX/RX command sequences, radio init and IRQ drain
 * policy, built on the L1 command layer. All functions here perform SPI and
 * must only run on the radio thread (or, during tdma_init(), on the
 * initializing thread before tdma_spi_bus_sem is given).
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef APP_TDMA_RADIO_H_
#define APP_TDMA_RADIO_H_

#include <stdint.h>
#include <stdbool.h>

#include "tdma.h"

/* Full L0 init + DIO1 handover + L1 command-layer init (plan M0 sequence). */
int tdma_radio_init(const struct tdma_config *cfg);

/* Slot entry sequences: clear all IRQs, then SetTx/SetRx at 90 % of slot. */
int tdma_radio_slot_tx_enter(void);
int tdma_radio_slot_rx_enter(void);

/* RX-slot slack work: copy payload / header into the chip's TX region. */
int tdma_radio_stage_payload(const uint8_t payload[TDMA_PAYLOAD_LEN]);
int tdma_radio_write_hdr(uint8_t slot_id, uint16_t frame_ctr);

/* Result of draining one DIO1 event. */
struct tdma_radio_event {
	uint16_t irq;
	bool have_rx;			/* raw[] holds a full 44-byte packet */
	uint8_t raw_len;		/* on-air length reported by the chip */
	uint8_t raw[TDMA_ON_AIR_LEN];
	int16_t rssi;
	int8_t snr;
};

int tdma_radio_drain_dio1(struct tdma_radio_event *ev);

/* Park the radio (used by tdma_stop): clear IRQs, STDBY_XOSC (TCXO warm). */
int tdma_radio_standby(void);

/* M0 manual one-shots (engine stopped). */
int tdma_radio_manual_tx(uint8_t slot_id, uint16_t frame_ctr,
			 const uint8_t payload[TDMA_PAYLOAD_LEN]);
int tdma_radio_manual_rx(uint32_t timeout_ms, struct tdma_radio_event *ev);

#endif /* APP_TDMA_RADIO_H_ */
