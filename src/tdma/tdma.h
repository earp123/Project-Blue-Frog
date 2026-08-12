/*
 * tdma - public API of the intercom TDMA radio layer (L2).
 *
 * This is the only header the application / future L3 sees. The radio is a
 * fixed 4-slot TDMA broadcast flood: every unit transmits one 44-byte packet
 * in its own slot and receives in the other three, every frame, forever.
 * Frame timing is anchored by the master unit's slot-0 packets.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef APP_TDMA_H_
#define APP_TDMA_H_

#include <zephyr/kernel.h>
#include <stdint.h>

/* ------------------------------------------------------------------ */
/* Locked radio invariants — single source of truth, never scatter.   */
/* ------------------------------------------------------------------ */

/* PHY: SF5 / 500 kHz / CR 4-5, locked by the time-on-air math. */
#define TDMA_RF_FREQ_HZ		915000000UL	/* 902-928 MHz band, single channel */

/* TX power: boot default and the runtime-adjustable range (the one PHY
 * parameter that is not locked — see tdma_set_tx_power()). SX1262 SetTxParams
 * accepts -9..+22 dBm (DS 13.4.4).
 */
#define TDMA_TX_POWER_DBM	22		/* SX1262 high-power PA max */
#define TDMA_TX_POWER_MIN_DBM	(-9)
#define TDMA_TX_POWER_MAX_DBM	22

/*
 * SX1262 datasheet minimum preamble for SF5/SF6 is 12 symbols
 * (DS.SX1261-2.W.APP §6.1.1); enforced wherever packet params are set.
 */
#define TDMA_PREAMBLE_SYMS	12

/* Fixed on-air payload: 4-byte L2 header + 40 bytes of opaque L3 payload. */
#define TDMA_HDR_LEN		4
#define TDMA_PAYLOAD_LEN	40
#define TDMA_ON_AIR_LEN		(TDMA_HDR_LEN + TDMA_PAYLOAD_LEN)

/* SX1262 data buffer split: staged TX at 0x00, RX landing zone at 0x80.
 * The fixed 44-byte length means RX can never overrun into the TX region.
 */
#define TDMA_TX_BASE_ADDR	0x00
#define TDMA_RX_BASE_ADDR	0x80

/*
 * Time-on-air at SF5 / BW 500 kHz / CR 4-5, explicit header, CRC on,
 * LDRO off, 12-symbol preamble, 44-byte payload:
 *   t_sym      = 2^5 / 500 kHz                = 64 us
 *   t_preamble = (12 + 4.25) * t_sym          = 1040 us
 *   n_payload  = 8 + ceil((8*44 - 4*5 + 28 + 16) / (4*5)) * (4+1)
 *              = 8 + 19*5                     = 103 symbols
 *   t_payload  = 103 * t_sym                  = 6592 us
 *   total                                     = 7632 us
 */
#define TDMA_TOA_US		7632

/*
 * Test-build slot width: fat guard bands for bring-up. Production target is
 * 20 ms; tightening must stay a one-line change here. Frame is always 4 slots.
 */
#define TDMA_SLOT_DURATION_US	50000
#define TDMA_SLOT_COUNT		4
#define TDMA_FRAME_DURATION_US	(TDMA_SLOT_COUNT * TDMA_SLOT_DURATION_US)

/* TX/RX command timeout: 90 % of the slot, leaving margin to drain the IRQ
 * and re-command before the next boundary.
 */
#define TDMA_SLOT_ACTIVE_US	((TDMA_SLOT_DURATION_US / 10) * 9)

/*
 * Fixed latency between SetTx and first preamble symbol on air, with the
 * radio parked in FS (PLL locked) and a 40 us PA ramp. Used by the secondary
 * to derive the master's slot-0 boundary from an RxDone timestamp.
 * TODO(M2+): measure with a logic analyzer and refine; a constant error here
 * offsets all units identically, so the fat guard bands absorb it.
 */
#define TDMA_TX_START_LATENCY_US 100

/*
 * There is deliberately no RX guard lead. An earlier revision biased the
 * secondary's boundary 1.5 ms ahead of the master's TX so a per-slot SetRx
 * could complete before the preamble arrived. That bias applied to the whole
 * frame, so the secondary's own TX slot fired 1.5 ms early too and the
 * master decoded none of it. RX slots now run continuously (see
 * tdma_radio_slot_rx_enter), so the receiver is already listening when the
 * boundary arrives and the two units' frames align exactly.
 */

/* Secondary phase alignment (deliberately naive, see tdma_core_sync_feed). */
#define TDMA_SYNC_STEP_CLAMP_US	500	/* max correction per frame */
#define TDMA_SYNC_LOCK_ERR_US	1000	/* |error| below this counts toward lock */
#define TDMA_SYNC_LOCK_STREAK	3	/* consecutive good beacons to reach RUNNING */

/* ------------------------------------------------------------------ */
/* Public types & API                                                 */
/* ------------------------------------------------------------------ */

enum tdma_role {
	TDMA_ROLE_MASTER,
	TDMA_ROLE_SECONDARY,
};

enum tdma_sync_state {
	TDMA_SYNC_STOPPED,
	TDMA_SYNC_SYNCING,
	TDMA_SYNC_RUNNING,
};

struct tdma_config {
	enum tdma_role role;
	uint8_t slot_id;		/* this unit's TX slot, 0-3 */
	uint32_t slot_duration_us;	/* normally TDMA_SLOT_DURATION_US */
};

struct tdma_rx_msg {
	uint8_t payload[TDMA_PAYLOAD_LEN];
	uint8_t slot_id;		/* transmitter's slot from the header */
	uint16_t frame_ctr;
	int16_t rssi;			/* dBm */
	int8_t snr;			/* dB */
	uint32_t timestamp_us;		/* DIO1 RxDone time, 1 MHz slot clock */
};

struct tdma_telemetry {
	uint32_t tx_done;
	uint32_t rx_done;
	uint32_t rx_crc_err;		/* payload CRC or header CRC errors */
	uint32_t rx_bad_header;		/* length != 44 or magic/version mismatch */
	uint32_t slot_timeouts;		/* RX slots that saw no packet */
	uint32_t stale_retx;		/* TX slots entered without a fresh payload */
	uint32_t busy_timeouts;		/* fatal: BUSY never deasserted */
	uint8_t sync_state;		/* enum tdma_sync_state */
	int32_t last_phase_err_us;	/* last beacon phase error (secondary) */

	/*
	 * Bench diagnostics (M2 bring-up). The engine's own counters cannot
	 * distinguish "receiver never armed" from "armed but heard nothing"
	 * from "heard energy but never completed a packet"; these can. Strip
	 * once the link is proven.
	 */
	uint32_t rx_arm;		/* SetRx issued at an RX slot entry */
	uint32_t dio1_edges;		/* DIO1 edges counted in the ISR */
	uint32_t drain_empty;		/* drains that read IrqStatus == 0 */
	uint32_t preamble_det;		/* windows that latched PreambleDetected */
	uint32_t header_valid;		/* windows that latched HeaderValid */
	uint32_t rx_mode_bad;		/* post-SetRx GetStatus not in RX mode */
	uint8_t last_chip_mode;		/* last GetStatus chip-mode nibble */
	int32_t last_ppm;		/* secondary: local-vs-master clock error */

	/*
	 * Per-slot arm/event tallies and the boundary-to-edge delay. Together
	 * these say whether the RX windows that produce no IRQ are a specific
	 * slot (implicating the extra SPI done in that slot's slack) or spread
	 * evenly, and whether a timeout lands at the programmed 45 ms or later.
	 */
	uint32_t arm_by_slot[TDMA_SLOT_COUNT];
	uint32_t evt_by_slot[TDMA_SLOT_COUNT];
	uint32_t last_evt_dt_us;

	/*
	 * TX-start latency measurement (card BlfSswKD). dt_by_slot is
	 * last_evt_dt_us attributed per slot: the boundary-to-DIO1 delay of
	 * the most recent event in each slot. tx_evt_dt_us latches that dt
	 * only for events carrying TxDone, so on either role it is this
	 * unit's own boundary -> TxDone delay (L_tx + TOA), independent of
	 * sync alignment. Cross-slot RxDone dt additionally carries the
	 * peer's boundary offset; comparing the two separates a real TX
	 * latency asymmetry from a sync offset caused by a wrong constant.
	 */
	uint32_t dt_by_slot[TDMA_SLOT_COUNT];
	uint32_t tx_evt_dt_us;
};

int tdma_init(const struct tdma_config *cfg);
int tdma_start(void);
void tdma_stop(void);
int tdma_tx_submit(const uint8_t payload[TDMA_PAYLOAD_LEN]); /* -EAGAIN if pending */
const struct tdma_telemetry *tdma_get_telemetry(void);

/*
 * Request a TX power change (-9..+22 dBm; -EINVAL outside that). Callable
 * from any thread at any time, including before tdma_init(): the value is
 * latched and SetTxParams is issued on the radio thread immediately before
 * the next transmit (chip in FS, the datasheet-legal window), so it takes
 * effect on that packet.
 */
int tdma_set_tx_power(int8_t dbm);

/*
 * Received-packet stream, drop-oldest on overflow.
 *
 * Depth is sized for the consumer's worst stall, not the average rate. At the
 * 20 ms production slot (80 ms frame) a 4-unit frame delivers 3 packets, and
 * the console's consumer is the main loop, which can be held off by an SD
 * flush. 32 entries covers roughly a second of that worst case; at depth 8 a
 * couple of hundred milliseconds of consumer stall silently drops packets
 * before any logger can see them.
 */
#define TDMA_RX_MSGQ_DEPTH	32

extern struct k_msgq tdma_rx_msgq;

/*
 * M0 bring-up helpers (engine must be stopped): fire one packet / one
 * blocking receive through L2/L1. Used by the "tdma" shell commands.
 */
int tdma_manual_tx(const uint8_t payload[TDMA_PAYLOAD_LEN]);
int tdma_manual_rx(uint32_t timeout_ms, struct tdma_rx_msg *msg);

#endif /* APP_TDMA_H_ */
