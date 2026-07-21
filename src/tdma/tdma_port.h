/*
 * tdma_port - kernel plumbing for the TDMA engine: the radio thread,
 * semaphores, DIO1 callback, slot timer (counter API) and message queues.
 *
 * Concurrency contract:
 *  - ISRs (DIO1 GPIO callback, counter alarm) set atomics and give
 *    semaphores only; every SPI transaction happens on the radio thread.
 *  - The radio thread's first act is taking tdma_spi_bus_sem, given exactly
 *    once when tdma_init() finishes; it is held forever — from that point
 *    the radio thread is the sole runtime SPI owner.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef APP_TDMA_PORT_H_
#define APP_TDMA_PORT_H_

#include <zephyr/kernel.h>
#include <stdint.h>

#include "tdma.h"

/*
 * Radio thread priority: cooperative, so a slot's command sequence is never
 * preempted by application threads (main loop, shell, logging); it yields
 * only at its k_poll/semaphore wait points, and every SPI burst it issues is
 * well under a millisecond at the 500 kHz bus clock.
 */
#define TDMA_RADIO_THREAD_PRIO	K_PRIO_COOP(4)
#define TDMA_RADIO_STACK_SIZE	2048

extern struct k_sem tdma_slot_tick_sem;
extern struct k_sem tdma_dio1_sem;
extern struct k_sem tdma_spi_bus_sem;

/* Counter/alarm setup; verifies the 1 MHz slot clock. */
int tdma_port_init(void);

/* Register L2's DIO1 GPIO callback (after the driver's has been detached). */
int tdma_port_attach_dio1(void);

/* Arm / disarm the free-running slot boundary alarm. */
int tdma_port_schedule_start(void);
void tdma_port_schedule_stop(void);

uint32_t tdma_port_now(void);		 /* current slot-clock time, us */
uint32_t tdma_port_dio1_timestamp(void); /* slot-clock time of last DIO1 edge */
uint32_t tdma_port_last_boundary(void);	 /* slot-clock time of last slot tick */
uint32_t tdma_port_dio1_edges(void);	 /* diagnostic: raw DIO1 edge count */

/*
 * One-shot phase adjustment (us, signed) folded into the next alarm target
 * by the alarm ISR; the accumulating absolute target keeps it permanent.
 */
void tdma_port_add_phase_adj(int32_t adj_us);

/* M0 manual operations, executed on the radio thread. */
enum tdma_manual_op {
	TDMA_MANUAL_TX,
	TDMA_MANUAL_RX,
	TDMA_MANUAL_STANDBY,	/* used by tdma_stop() */
};

struct tdma_manual_req {
	enum tdma_manual_op op;
	uint32_t timeout_ms;			/* RX window */
	const uint8_t *payload;			/* TX payload (40 B) */
	struct tdma_rx_msg *rx;			/* RX result out */
	int result;
	struct k_sem done;
};

/* Blocking: hands the request to the radio thread and waits. */
int tdma_port_manual_submit(struct tdma_manual_req *req);

/* Engine handlers, implemented by tdma_core, called from the radio thread. */
void tdma_core_on_slot_tick(void);
void tdma_core_on_dio1(void);
void tdma_core_on_manual(struct tdma_manual_req *req);

/* Secondary phase alignment hook, fed from the RX drain path. */
void tdma_core_sync_feed(uint32_t rx_timestamp_us, uint16_t frame_ctr);

#endif /* APP_TDMA_PORT_H_ */
