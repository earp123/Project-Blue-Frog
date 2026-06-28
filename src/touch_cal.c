/*
 * touch_cal - touchscreen affine calibration. See touch_cal.h.
 *
 * The least-squares fit is done in mean-centred coordinates: centring zeroes the
 * cross terms with the constant column, so each axis reduces to a well
 * conditioned 2x2 solve plus an offset. That keeps the numbers small enough for
 * single-precision float (no matrix library, FPU optional).
 */

#include <zephyr/kernel.h>

#if defined(CONFIG_APP_CONSOLE)

#include "touch_cal.h"

/* screen_x = A*raw_x + B*raw_y + C ; screen_y = D*raw_x + E*raw_y + F */
static float A = 1.0f, B = 0.0f, C = 0.0f;
static float D = 0.0f, E = 1.0f, F = 0.0f;

void touch_cal_reset(void)
{
	A = 1.0f; B = 0.0f; C = 0.0f;
	D = 0.0f; E = 1.0f; F = 0.0f;
}

void touch_cal_init(void)
{
	touch_cal_reset();
}

bool touch_cal_is_default(void)
{
	return A == 1.0f && B == 0.0f && C == 0.0f &&
	       D == 0.0f && E == 1.0f && F == 0.0f;
}

static int round_to_int(float v)
{
	return (int)(v + (v >= 0.0f ? 0.5f : -0.5f));
}

void touch_cal_apply(int raw_x, int raw_y, int *screen_x, int *screen_y)
{
	*screen_x = round_to_int(A * raw_x + B * raw_y + C);
	*screen_y = round_to_int(D * raw_x + E * raw_y + F);
}

bool touch_cal_solve(const int *raw_x, const int *raw_y,
		     const int *scr_x, const int *scr_y, int n)
{
	if (n < 3) {
		return false;
	}

	float mx = 0, my = 0, mu = 0, mv = 0;

	for (int i = 0; i < n; i++) {
		mx += raw_x[i];
		my += raw_y[i];
		mu += scr_x[i];
		mv += scr_y[i];
	}
	mx /= n; my /= n; mu /= n; mv /= n;

	/* Centred sums: Sxx/Sxy/Syy form the 2x2 normal matrix shared by both
	 * axes; XU/YU and XV/YV are the right-hand sides for x and y outputs.
	 */
	float Sxx = 0, Sxy = 0, Syy = 0;
	float XU = 0, YU = 0, XV = 0, YV = 0;

	for (int i = 0; i < n; i++) {
		float X = raw_x[i] - mx;
		float Y = raw_y[i] - my;
		float U = scr_x[i] - mu;
		float V = scr_y[i] - mv;

		Sxx += X * X;
		Sxy += X * Y;
		Syy += Y * Y;
		XU += X * U;
		YU += Y * U;
		XV += X * V;
		YV += Y * V;
	}

	float det = Sxx * Syy - Sxy * Sxy;

	if (det > -1e-3f && det < 1e-3f) {
		return false; /* collinear / degenerate sample set */
	}

	float a = (Syy * XU - Sxy * YU) / det;
	float b = (Sxx * YU - Sxy * XU) / det;
	float d = (Syy * XV - Sxy * YV) / det;
	float e = (Sxx * YV - Sxy * XV) / det;

	A = a; B = b; C = mu - a * mx - b * my;
	D = d; E = e; F = mv - d * mx - e * my;
	return true;
}

#endif /* CONFIG_APP_CONSOLE */
