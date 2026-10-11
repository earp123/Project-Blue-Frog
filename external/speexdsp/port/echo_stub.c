/*
 * preprocess.c calls speex_echo_get_residual() (mdf.c, the echo canceller)
 * when an echo state is attached. None ever is: the earpieces have no
 * acoustic path back to the mics, so there is no echo canceller to link.
 * This keeps the call resolvable without pulling in mdf.c (Project-Blue-Frog).
 */
#include "config.h"
#include "arch.h"
#include "speex/speex_echo.h"

/* As preprocess.c declares it (float build: spx_word32_t is float). */
void speex_echo_get_residual(SpeexEchoState *st, spx_word32_t *Yout, int len);

void speex_echo_get_residual(SpeexEchoState *st, spx_word32_t *Yout, int len)
{
	(void)st;
	for (int i = 0; i < len; i++) {
		Yout[i] = 0;
	}
}
