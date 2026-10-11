/*
 * Force-included into every vendored SpeexDSP source (CMakeLists.txt), after
 * codec2_opt.h: the float build calls the double-precision libm functions
 * (exp, log, pow, sqrt, ...) on float data, and the M33's FPU is single
 * precision only, so each call ran in software. The preprocessor took
 * 10.7 ms per 20 ms frame that way. math.h is included first, so its own
 * declarations are untouched; every later call in the sources maps to the
 * float function (Project-Blue-Frog).
 */
#ifndef SPEEXDSP_PORT_FLOAT_MATH_H
#define SPEEXDSP_PORT_FLOAT_MATH_H

#include <math.h>

#define sqrt(x)		sqrtf(x)
#define exp(x)		expf(x)
#define log(x)		logf(x)
#define log10(x)	log10f(x)
#define pow(x, y)	powf(x, y)
#define floor(x)	floorf(x)
#define cos(x)		cosf(x)
#define sin(x)		sinf(x)
#define atan(x)		atanf(x)
#define fabs(x)		fabsf(x)

#endif
