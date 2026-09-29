/*
 * c2_prof - per-function Codec 2 timing on the device (CONFIG_SOAK_C2_PROFILE).
 * See c2_prof.c.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef C2_PROF_H_
#define C2_PROF_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define C2_PROF_ENC	0
#define C2_PROF_DEC	1
#define C2_PROF_PHASES	2

void c2_prof_reset(void);
void c2_prof_phase(int phase);

/* Line `line` (0, 1, ...) of phase p's per-chunk figures for n chunks into
 * buf; false once there are no more lines.
 */
bool c2_prof_line(int p, int line, uint32_t n, char *buf, size_t len);

#endif /* C2_PROF_H_ */
