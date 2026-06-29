/*
 * pingpong - symmetric LoRa ping/pong link test for the telemetry console.
 *
 * Both radios in a pair run identical firmware in this mode; there are no roles.
 * Entering the mode puts the radio in continuous RX (on the currently-applied RF
 * config). Pressing SEND PING transmits a 44-byte ping; the peer echoes a pong
 * with the same sequence number, letting the originator measure round-trip time.
 *
 * Threading: a single dedicated RX thread owns ALL radio I/O while the mode is
 * active (it both receives and transmits). The UI thread only calls the control
 * functions below and reads a stats snapshot via pingpong_get_stats(). This
 * module never touches the display (single-owner drawing rule).
 *
 * Stats are reset on every pingpong_start() (no persistence across exit).
 */

#ifndef PINGPONG_H
#define PINGPONG_H

#include <stdint.h>
#include <stdbool.h>

typedef struct {
	uint32_t pings_sent;
	uint32_t pongs_received;
	uint32_t pings_received;
	uint32_t pongs_sent;
	uint32_t timeouts;
	uint32_t last_rtt_us;
	uint32_t min_rtt_us;
	uint32_t max_rtt_us;
	uint64_t rtt_sum_us;     /* for computing average */
	uint32_t rtt_count;      /* denominator for average */
	bool outstanding_valid;
	uint32_t outstanding_seq;
	uint32_t outstanding_tx_us;
} pingpong_stats_t;

/* Start/stop the mode. Start resets stats and puts the radio into continuous RX
 * (via the RX thread). Stop leaves the chip idle. Both return 0 on success.
 */
int pingpong_start(void);
int pingpong_stop(void);

/* Request a ping send (non-blocking). The RX thread performs the actual TX on
 * its next slice; if a ping is already outstanding it is abandoned first.
 */
void pingpong_send_ping(void);

/* Clear all counters (also done implicitly by pingpong_start()). */
void pingpong_reset_stats(void);

/* Copy a consistent snapshot of the stats (taken under the stats mutex). */
void pingpong_get_stats(pingpong_stats_t *out);

bool pingpong_is_running(void);

/* Microsecond timestamp on the same clock the module timestamps packets with,
 * so the UI can compute "ping in flight" elapsed = now - outstanding_tx_us.
 */
uint32_t pingpong_now_us(void);

#endif /* PINGPONG_H */
