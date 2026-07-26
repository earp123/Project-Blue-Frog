/*
 * soak_role - bench role selection from a hardware jumper.
 *
 * The soak firmware is one image for both units; which one becomes the TDMA
 * timing master is decided at boot by the role-select jumper wired in
 * src/soak/role_select.overlay (P1.10 -> 3V3 = master).
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef SOAK_ROLE_H_
#define SOAK_ROLE_H_

#include <stdint.h>

#include "tdma.h"

/*
 * Read the role-select jumper. Jumper P1.10 -> 3V3 => TDMA_ROLE_MASTER; open
 * (internal pull-down) => TDMA_ROLE_SECONDARY. Fails safe to SECONDARY if the
 * GPIO is unavailable, so a unit never assumes the timing-master role by
 * accident.
 */
enum tdma_role soak_role_get(void);

/* Fixed slot assignment: master transmits in slot 0, secondary in slot 1. */
uint8_t soak_role_slot(enum tdma_role role);

#endif /* SOAK_ROLE_H_ */
