/*
 * Build configuration for the vendored SpeexDSP subset (Project-Blue-Frog).
 * Stands in for the config.h that upstream's configure script generates;
 * every source sees it through HAVE_CONFIG_H (CMakeLists.txt).
 */
#ifndef SPEEXDSP_PORT_CONFIG_H
#define SPEEXDSP_PORT_CONFIG_H

/* Float build: the M33 has a single-precision FPU, and the preprocessor's
 * AGC exists only in the float build.
 */
#define FLOATING_POINT

/* Speex's own real FFT. The preprocessor's FFT is twice the frame size:
 * 160 points for our 80-sample frames. Not a power of two, so CMSIS
 * arm_rfft_fast_f32 cannot take it. smallft factors 160 as 4 * 4 * 2 * 5.
 * KISS FFT (Speex's other option) measured no faster on the device: the
 * FFTs are ~23 % of the preprocessor's time either way.
 */
#define USE_SMALLFT

#define EXPORT

/* Allocation and diagnostics go through os_support_custom.h. */
#define OS_SUPPORT_CUSTOM
#define DISABLE_WARNINGS
#define DISABLE_NOTIFICATIONS

#endif
