/*
 * tdma_port - radio thread, semaphores, DIO1 callback, slot timer, msgqs.
 * See tdma_port.h for the concurrency contract.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/counter.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/clock_control.h>
#include <zephyr/drivers/clock_control/nrf_clock_control.h>
#include <zephyr/sys/atomic.h>
#include <errno.h>

#include "tdma.h"
#include "tdma_port.h"
#include "sx126x_cmd.h"

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(tdma_port, CONFIG_LOG_DEFAULT_LEVEL);

/* 1 MHz slot clock: TIMER2 via the counter API (see the board overlay). */
#define SLOT_TIMER_NODE DT_NODELABEL(timer2)
BUILD_ASSERT(DT_NODE_HAS_STATUS_OKAY(SLOT_TIMER_NODE),
	     "timer2 must be enabled in the overlay for the TDMA slot clock");
#define TDMA_ALARM_CHAN 0

static const struct device *const slot_timer = DEVICE_DT_GET(SLOT_TIMER_NODE);

K_SEM_DEFINE(tdma_slot_tick_sem, 0, K_SEM_MAX_LIMIT);
K_SEM_DEFINE(tdma_dio1_sem, 0, K_SEM_MAX_LIMIT);
K_SEM_DEFINE(tdma_spi_bus_sem, 0, 1);

K_MSGQ_DEFINE(tdma_rx_msgq, sizeof(struct tdma_rx_msg), TDMA_RX_MSGQ_DEPTH, 4);

static atomic_t dio1_timestamp;
static atomic_t last_boundary;
static atomic_t phase_adj_us;
static atomic_t dio1_edges;	/* diagnostic: raw DIO1 edge count */

/* Only touched by the alarm callback and (re)start with the alarm disarmed. */
static uint32_t next_target;
static uint32_t slot_ticks;

static struct gpio_callback dio1_cb_data;

K_MSGQ_DEFINE(manual_msgq, sizeof(struct tdma_manual_req *), 1, 4);

uint32_t tdma_port_now(void)
{
	uint32_t t = 0;

	counter_get_value(slot_timer, &t);
	return t;
}

uint32_t tdma_port_dio1_timestamp(void)
{
	return (uint32_t)atomic_get(&dio1_timestamp);
}

uint32_t tdma_port_last_boundary(void)
{
	return (uint32_t)atomic_get(&last_boundary);
}

void tdma_port_add_phase_adj(int32_t adj_us)
{
	atomic_set(&phase_adj_us, adj_us);
}

/* DIO1 edge: timestamp + flag only; all SPI work happens on the radio thread. */
static void dio1_isr(const struct device *port, struct gpio_callback *cb,
		     uint32_t pins)
{
	uint32_t t = 0;

	counter_get_value(slot_timer, &t);
	atomic_set(&dio1_timestamp, (atomic_val_t)t);
	atomic_inc(&dio1_edges);
	k_sem_give(&tdma_dio1_sem);
}

uint32_t tdma_port_dio1_edges(void)
{
	return (uint32_t)atomic_get(&dio1_edges);
}

static struct counter_alarm_cfg alarm_cfg;

/*
 * Slot boundary alarm. Re-arms itself from an accumulating absolute tick
 * target — never "now + delta" — so command latency and ISR jitter cannot
 * accumulate into the cadence. A pending sync correction is folded in once.
 */
static void slot_alarm_cb(const struct device *dev, uint8_t chan,
			  uint32_t ticks, void *user_data)
{
	atomic_set(&last_boundary, (atomic_val_t)next_target);
	next_target += slot_ticks + (uint32_t)atomic_clear(&phase_adj_us);

	alarm_cfg.ticks = next_target;
	counter_set_channel_alarm(dev, TDMA_ALARM_CHAN, &alarm_cfg);

	k_sem_give(&tdma_slot_tick_sem);
}

/*
 * Park HFCLK on the crystal for the life of the engine.
 *
 * TIMER2 (the slot clock) derives from HFCLK, and nothing in this build uses
 * the nRF's own radio, so no other subsystem ever requests HFXO — HFCLK would
 * otherwise free-run on the internal RC. Measured cost of leaving it there:
 * roughly -1000 ppm between two units' slot clocks, which the secondary's
 * proportional sync can absorb at 50 ms slots but not at the 20 ms production
 * target. The request is never released; the engine owns the clock.
 */
static int request_hfxo(void)
{
	const struct device *clk = DEVICE_DT_GET_ONE(nordic_nrf_clock);
	int ret;

	if (!device_is_ready(clk)) {
		LOG_ERR("nRF clock controller not ready");
		return -ENODEV;
	}

	ret = clock_control_on(clk, CLOCK_CONTROL_NRF_SUBSYS_HF);
	if (ret < 0) {
		LOG_ERR("HFXO request failed: %d", ret);
		return ret;
	}

	/* Bounded wait: HFXO start-up is well under a millisecond. */
	for (int i = 0; i < 100; i++) {
		if (clock_control_get_status(clk, CLOCK_CONTROL_NRF_SUBSYS_HF) ==
		    CLOCK_CONTROL_STATUS_ON) {
			LOG_INF("slot clock running on HFXO");
			return 0;
		}
		k_busy_wait(100);
	}

	LOG_ERR("HFXO did not start");
	return -ETIMEDOUT;
}

int tdma_port_init(void)
{
	uint32_t freq;
	int ret;

	ret = request_hfxo();
	if (ret < 0) {
		return ret;
	}

	if (!device_is_ready(slot_timer)) {
		LOG_ERR("slot timer not ready");
		return -ENODEV;
	}

	freq = counter_get_frequency(slot_timer);
	if (freq != USEC_PER_SEC) {
		/* Everything below equates ticks and microseconds. */
		LOG_ERR("slot timer runs at %u Hz, expected 1 MHz", freq);
		return -EINVAL;
	}

	alarm_cfg.flags = COUNTER_ALARM_CFG_ABSOLUTE |
			  COUNTER_ALARM_CFG_EXPIRE_WHEN_LATE;
	alarm_cfg.callback = slot_alarm_cb;
	alarm_cfg.user_data = NULL;

	return counter_start(slot_timer);
}

int tdma_port_attach_dio1(void)
{
	const struct gpio_dt_spec *dio1 = sx126x_cmd_dio1_spec();
	int ret;

	gpio_init_callback(&dio1_cb_data, dio1_isr, BIT(dio1->pin));
	ret = gpio_add_callback(dio1->port, &dio1_cb_data);
	if (ret < 0) {
		return ret;
	}

	return gpio_pin_interrupt_configure_dt(dio1, GPIO_INT_EDGE_TO_ACTIVE);
}

int tdma_port_schedule_start(void)
{
	slot_ticks = TDMA_SLOT_DURATION_US;

	k_sem_reset(&tdma_slot_tick_sem);
	atomic_set(&phase_adj_us, 0);

	next_target = tdma_port_now() + slot_ticks;
	alarm_cfg.ticks = next_target;

	return counter_set_channel_alarm(slot_timer, TDMA_ALARM_CHAN, &alarm_cfg);
}

void tdma_port_schedule_stop(void)
{
	counter_cancel_channel_alarm(slot_timer, TDMA_ALARM_CHAN);
}

int tdma_port_manual_submit(struct tdma_manual_req *req)
{
	struct tdma_manual_req *ptr = req;
	int ret;

	k_sem_init(&req->done, 0, 1);
	req->result = -EIO;

	ret = k_msgq_put(&manual_msgq, &ptr, K_NO_WAIT);
	if (ret < 0) {
		return -EBUSY;
	}

	k_sem_take(&req->done, K_FOREVER);
	return req->result;
}

static void radio_thread_fn(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	/* Wait for tdma_init() to finish the L0->L1 handoff, then own the
	 * bus forever.
	 */
	k_sem_take(&tdma_spi_bus_sem, K_FOREVER);
	LOG_INF("radio thread owns the SPI bus");

	struct k_poll_event events[3] = {
		K_POLL_EVENT_STATIC_INITIALIZER(K_POLL_TYPE_SEM_AVAILABLE,
						K_POLL_MODE_NOTIFY_ONLY,
						&tdma_dio1_sem, 0),
		K_POLL_EVENT_STATIC_INITIALIZER(K_POLL_TYPE_SEM_AVAILABLE,
						K_POLL_MODE_NOTIFY_ONLY,
						&tdma_slot_tick_sem, 0),
		K_POLL_EVENT_STATIC_INITIALIZER(K_POLL_TYPE_MSGQ_DATA_AVAILABLE,
						K_POLL_MODE_NOTIFY_ONLY,
						&manual_msgq, 0),
	};

	for (;;) {
		k_poll(events, ARRAY_SIZE(events), K_FOREVER);

		/* Drain the radio event (belongs to the current/previous
		 * slot) before advancing to the next slot.
		 */
		if (k_sem_take(&tdma_dio1_sem, K_NO_WAIT) == 0) {
			tdma_core_on_dio1();
		}

		if (k_sem_take(&tdma_slot_tick_sem, K_NO_WAIT) == 0) {
			tdma_core_on_slot_tick();
		}

		struct tdma_manual_req *req;

		if (k_msgq_get(&manual_msgq, &req, K_NO_WAIT) == 0) {
			tdma_core_on_manual(req);
		}

		for (size_t i = 0; i < ARRAY_SIZE(events); i++) {
			events[i].state = K_POLL_STATE_NOT_READY;
		}
	}
}

K_THREAD_DEFINE(tdma_radio_tid, TDMA_RADIO_STACK_SIZE, radio_thread_fn,
		NULL, NULL, NULL, TDMA_RADIO_THREAD_PRIO, 0, 0);
