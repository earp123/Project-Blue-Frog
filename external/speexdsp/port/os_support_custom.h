/*
 * SpeexDSP allocation and diagnostics for the device (Project-Blue-Frog).
 *
 * Every allocation is carved from mic_dsp's static arena (mic_dsp.c), so the
 * preprocessor state is sized at build time and never touches a heap.
 * Allocations happen only in speex_preprocess_state_init(), on the encoder
 * thread at the start of a run; mic_dsp resets the whole arena before the
 * next init, so free is a no-op. The arena is zeroed when reset, which is
 * what speex_alloc() must return.
 */
#ifndef SPEEXDSP_PORT_OS_SUPPORT_CUSTOM_H
#define SPEEXDSP_PORT_OS_SUPPORT_CUSTOM_H

void *mic_dsp_speex_alloc(int size);
void mic_dsp_speex_fatal(const char *str);

#define OVERRIDE_SPEEX_ALLOC
static inline void *speex_alloc(int size)
{
	return mic_dsp_speex_alloc(size);
}

#define OVERRIDE_SPEEX_ALLOC_SCRATCH
static inline void *speex_alloc_scratch(int size)
{
	return mic_dsp_speex_alloc(size);
}

#define OVERRIDE_SPEEX_REALLOC
static inline void *speex_realloc(void *ptr, int size)
{
	(void)ptr;
	(void)size;
	mic_dsp_speex_fatal("realloc");
	return 0;
}

#define OVERRIDE_SPEEX_FREE
static inline void speex_free(void *ptr)
{
	(void)ptr;
}

#define OVERRIDE_SPEEX_FREE_SCRATCH
static inline void speex_free_scratch(void *ptr)
{
	(void)ptr;
}

#define OVERRIDE_SPEEX_FATAL
static inline void _speex_fatal(const char *str, const char *file, int line)
{
	(void)file;
	(void)line;
	mic_dsp_speex_fatal(str);
}

#define OVERRIDE_SPEEX_PUTC
static inline void _speex_putc(int ch, void *file)
{
	(void)ch;
	(void)file;
}

#endif
