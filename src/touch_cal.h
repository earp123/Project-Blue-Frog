/*
 * touch_cal - touchscreen calibration (affine transform).
 *
 * The XPT2046 reports coordinates that, on a rotated panel, are offset / swapped
 * / inverted relative to the display's pixel axes. A single 6-parameter affine
 * transform corrects all of that (rotation, axis swap, inversion, scale,
 * offset):
 *
 *     screen_x = A*raw_x + B*raw_y + C
 *     screen_y = D*raw_x + E*raw_y + F
 *
 * The coefficients are fitted on-device from several (raw -> known screen point)
 * samples collected by the calibration UI (see SCR_CALIBRATE in console.c).
 *
 * State is in-RAM only (re-run after each boot/reflash); persistence to
 * settings/NVS is a follow-up.
 */

#ifndef TOUCH_CAL_H
#define TOUCH_CAL_H

#include <stdbool.h>

/* Load the identity transform (raw == screen). */
void touch_cal_init(void);
void touch_cal_reset(void);

/* True while the transform is still the identity (i.e. uncalibrated). */
bool touch_cal_is_default(void);

/* Map a raw controller coordinate to a screen pixel coordinate. */
void touch_cal_apply(int raw_x, int raw_y, int *screen_x, int *screen_y);

/* Fit the affine transform from n (>= 3) point pairs by least squares.
 * raw_x[i]/raw_y[i] are the controller samples; scr_x[i]/scr_y[i] are the known
 * on-screen target pixels. Returns false (and leaves the transform unchanged)
 * if the points are degenerate / collinear.
 */
bool touch_cal_solve(const int *raw_x, const int *raw_y,
		     const int *scr_x, const int *scr_y, int n);

#endif /* TOUCH_CAL_H */
