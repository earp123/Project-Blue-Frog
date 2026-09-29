/*
 * c2_prof - where Codec 2's encode and decode time goes, on the device
 * (CONFIG_SOAK_C2_PROFILE). docs/live_audio_wm8960.md, G0 follow-up.
 *
 * The vendored codec stays untouched: the linker's --wrap redirects the
 * codec's cross-file calls (CMakeLists.txt lists them) to the __wrap_*
 * functions here, which time the real call with the Cortex-M33 cycle
 * counter. Calls inside one source file cannot be wrapped (analyse_one_frame
 * and synthesise_one_frame are file-local to codec2.c), so their own glue is
 * what "total" has that the functions below do not.
 *
 * Every function is reported four ways, per 80 ms chunk:
 *   incl  the whole call;
 *   fft   the kiss_fft* calls made inside it (counted at the outermost FFT
 *         only, since kiss_fftr calls kiss_fft);
 *   libm  the float maths calls made inside it (cosf, powf, ...);
 *   excl  incl minus both, and minus any wrapped function it calls.
 * Each wrapper's own cost, measured once, is subtracted per call so small
 * functions like cosf are not inflated.
 *
 * c2_dec_bench() drives it: reset, encode all chunks (phase ENC), decode
 * them (phase DEC), then read per-chunk figures with c2_prof_line().
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>
#include <cmsis_core.h>
#include <stdio.h>
#include <string.h>

#include <codec2.h>
#include "defines.h"
#include "comp.h"
#include "codec2_fft.h"
#include "kiss_fft.h"
#include "kiss_fftr.h"

#include "c2_prof.h"

enum prof_id {
	P_NONE,
	P_ENCODE, P_DECODE,
	P_DFT_SPEECH, P_NLP, P_PITCH_REFINE, P_EST_AMPS, P_VOICING,
	P_TO_LSPS, P_ENC_LSPD,
	P_DEC_LSPD, P_LSP_TO_LPC, P_AKS_TO_M2, P_PHASE_SYNTH, P_POSTFILTER,
	P_SYNTHESISE,
	P_COUNT
};

static const char *const prof_name[P_COUNT] = {
	[P_NONE] = "glue",
	[P_ENCODE] = "total", [P_DECODE] = "total",
	[P_DFT_SPEECH] = "dft_speech", [P_NLP] = "nlp",
	[P_PITCH_REFINE] = "pitch_refine", [P_EST_AMPS] = "est_amps",
	[P_VOICING] = "voicing", [P_TO_LSPS] = "to_lsps",
	[P_ENC_LSPD] = "enc_lspd", [P_DEC_LSPD] = "dec_lspd",
	[P_LSP_TO_LPC] = "lsp_to_lpc", [P_AKS_TO_M2] = "aks_to_M2",
	[P_PHASE_SYNTH] = "phase_synth", [P_POSTFILTER] = "postfilter",
	[P_SYNTHESISE] = "synthesise",
};

/* Per phase, per function: inclusive cycles, calls, and the FFT / libm /
 * wrapped-callee cycles spent inside it.
 */
struct acc {
	uint64_t incl;
	uint64_t fft;
	uint64_t libm;
	uint64_t callee;
	uint32_t calls;
};

static struct acc acc[C2_PROF_PHASES][P_COUNT];
static uint32_t fft_calls[C2_PROF_PHASES];
static uint32_t libm_calls[C2_PROF_PHASES];
static int phase;
static int ctx = P_NONE;	/* innermost wrapped codec function running */
static int fft_depth;
static uint32_t overhead;	/* cycles one wrapper adds, measured once */

static inline uint32_t now(void)
{
	return DWT->CYCCNT;
}

static inline uint32_t since(uint32_t t0)
{
	uint32_t d = now() - t0;

	return (d > overhead) ? d - overhead : 0;
}

void c2_prof_reset(void)
{
	memset(acc, 0, sizeof(acc));
	memset(fft_calls, 0, sizeof(fft_calls));
	memset(libm_calls, 0, sizeof(libm_calls));
	fft_depth = 0;
	ctx = P_NONE;

	/* DWT is enabled by the timing functions; make sure, then measure
	 * what an empty timed call costs.
	 */
	CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
	DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
	overhead = 0;

	uint32_t best = UINT32_MAX;

	for (int i = 0; i < 64; i++) {
		uint32_t t0 = now();

		__asm__ volatile("" ::: "memory");
		best = MIN(best, now() - t0);
	}
	overhead = best;
}

void c2_prof_phase(int p)
{
	phase = p;
}

/* "prof enc <name>=incl/fft/libm/excl/calls ..." in us per chunk (calls x10
 * per chunk), n = chunks. Returns false once all lines are out.
 */
bool c2_prof_line(int p, int line, uint32_t n, char *buf, size_t len)
{
	static const int enc_ids[] = {
		P_ENCODE, P_DFT_SPEECH, P_NLP, P_PITCH_REFINE, P_EST_AMPS,
		P_VOICING, P_TO_LSPS, P_ENC_LSPD,
	};
	static const int dec_ids[] = {
		P_DECODE, P_DEC_LSPD, P_LSP_TO_LPC, P_AKS_TO_M2,
		P_PHASE_SYNTH, P_POSTFILTER, P_SYNTHESISE,
	};
	const int *ids = (p == C2_PROF_ENC) ? enc_ids : dec_ids;
	int nids = (p == C2_PROF_ENC) ? ARRAY_SIZE(enc_ids) : ARRAY_SIZE(dec_ids);
	const int per_line = 3;
	int first = line * per_line;
	uint64_t div = (uint64_t)n * (SystemCoreClock / 1000000U);
	int w = 0;

	if (n == 0 || first >= nids + per_line) {
		return false;
	}
	w += snprintf(buf, len, "prof %s", p == C2_PROF_ENC ? "enc" : "dec");
	if (first >= nids) {
		/* Last line: FFT and libm call counts for the phase. */
		snprintf(buf + w, len - w, " fft_calls=%u libm_calls=%u "
			 "(x10 per chunk)",
			 (uint32_t)((uint64_t)fft_calls[p] * 10U / n),
			 (uint32_t)((uint64_t)libm_calls[p] * 10U / n));
		return true;
	}
	for (int i = first; i < MIN(first + per_line, nids) && w < (int)len; i++) {
		const struct acc *a = &acc[p][ids[i]];
		uint64_t excl = a->incl - MIN(a->incl, a->fft + a->libm +
						   a->callee);

		w += snprintf(buf + w, len - w, " %s=%u/%u/%u/%u/%u",
			      prof_name[ids[i]], (uint32_t)(a->incl / div),
			      (uint32_t)(a->fft / div),
			      (uint32_t)(a->libm / div), (uint32_t)(excl / div),
			      (uint32_t)((uint64_t)a->calls * 10U / n));
	}
	return true;
}

/* ---- The wrappers ------------------------------------------------------- */

/* A wrapped codec function: inclusive time to itself, and the same time
 * charged to its wrapped caller as callee time.
 */
#define ENTER(id)							\
	int saved = ctx;						\
	uint32_t t0 = now();						\
	ctx = (id)
#define LEAVE(id)							\
	uint32_t d = since(t0);						\
	ctx = saved;							\
	acc[phase][id].incl += d;					\
	acc[phase][id].calls++;						\
	if (saved != P_NONE) {						\
		acc[phase][saved].callee += d;				\
	}

#define WRAP_VOID(id, name, params, args)				\
	void __real_##name params;					\
	void __wrap_##name params					\
	{								\
		ENTER(id);						\
		__real_##name args;					\
		LEAVE(id);						\
	}

#define WRAP_RET(id, type, name, params, args)				\
	type __real_##name params;					\
	type __wrap_##name params					\
	{								\
		ENTER(id);						\
		type r = __real_##name args;				\
		LEAVE(id);						\
		return r;						\
	}

WRAP_VOID(P_ENCODE, codec2_encode,
	  (struct CODEC2 *c, unsigned char bytes[], short speech[]),
	  (c, bytes, speech))
WRAP_VOID(P_DECODE, codec2_decode,
	  (struct CODEC2 *c, short speech[], const unsigned char bytes[]),
	  (c, speech, bytes))
WRAP_VOID(P_DFT_SPEECH, dft_speech,
	  (C2CONST *c2const, codec2_fft_cfg cfg, COMP Sw[], float Sn[],
	   float w[]),
	  (c2const, cfg, Sw, Sn, w))
WRAP_RET(P_NLP, float, nlp,
	 (void *st, float Sn[], int n, float *pitch, COMP Sw[], float W[],
	  float *prev_f0),
	 (st, Sn, n, pitch, Sw, W, prev_f0))
WRAP_VOID(P_PITCH_REFINE, two_stage_pitch_refinement,
	  (C2CONST *c2const, MODEL *model, COMP Sw[]), (c2const, model, Sw))
WRAP_VOID(P_EST_AMPS, estimate_amplitudes,
	  (MODEL *model, COMP Sw[], float W[], int est_phase),
	  (model, Sw, W, est_phase))
WRAP_RET(P_VOICING, float, est_voicing_mbe,
	 (C2CONST *c2const, MODEL *model, COMP Sw[], float W[]),
	 (c2const, model, Sw, W))
WRAP_RET(P_TO_LSPS, float, speech_to_uq_lsps,
	 (float lsp[], float ak[], float Sn[], float w[], int m_pitch,
	  int order),
	 (lsp, ak, Sn, w, m_pitch, order))
WRAP_VOID(P_ENC_LSPD, encode_lspds_scalar,
	  (int indexes[], float lsp[], int order), (indexes, lsp, order))
WRAP_VOID(P_DEC_LSPD, decode_lspds_scalar,
	  (float lsp[], int indexes[], int order), (lsp, indexes, order))
WRAP_VOID(P_LSP_TO_LPC, lsp_to_lpc, (float *freq, float *ak, int order),
	  (freq, ak, order))
WRAP_VOID(P_AKS_TO_M2, aks_to_M2,
	  (codec2_fftr_cfg cfg, float ak[], int order, MODEL *model, float E,
	   float *snr, int dump, int sim_pf, int pf, int bass_boost,
	   float beta, float gamma, COMP Aw[]),
	  (cfg, ak, order, model, E, snr, dump, sim_pf, pf, bass_boost, beta,
	   gamma, Aw))
WRAP_VOID(P_PHASE_SYNTH, phase_synth_zero_order,
	  (int n_samp, MODEL *model, float *ex_phase, COMP filter_phase[]),
	  (n_samp, model, ex_phase, filter_phase))
WRAP_VOID(P_POSTFILTER, postfilter, (MODEL *model, float *bg_est),
	  (model, bg_est))
WRAP_VOID(P_SYNTHESISE, synthesise,
	  (int n_samp, codec2_fftr_cfg cfg, float Sn_[], MODEL *model,
	   float Pn[], int shift),
	  (n_samp, cfg, Sn_, model, Pn, shift))

/* FFTs: charged to the running codec function, outermost call only
 * (kiss_fftr calls kiss_fft).
 */
#define WRAP_FFT(name, params, args)					\
	void __real_##name params;					\
	void __wrap_##name params					\
	{								\
		uint32_t t0 = now();					\
		fft_depth++;						\
		__real_##name args;					\
		if (--fft_depth == 0) {					\
			acc[phase][ctx].fft += since(t0);		\
			fft_calls[phase]++;				\
		}							\
	}

WRAP_FFT(kiss_fft, (kiss_fft_cfg cfg, const kiss_fft_cpx *fin,
		    kiss_fft_cpx *fout), (cfg, fin, fout))
WRAP_FFT(kiss_fftr, (kiss_fftr_cfg st, const kiss_fft_scalar *in,
		     kiss_fft_cpx *out), (st, in, out))
WRAP_FFT(kiss_fftri, (kiss_fftr_cfg st, const kiss_fft_cpx *in,
		      kiss_fft_scalar *out), (st, in, out))

/* Float maths from libm, charged to the running codec function. */
#define WRAP_M1(name)							\
	float __real_##name(float x);					\
	float __wrap_##name(float x)					\
	{								\
		uint32_t t0 = now();					\
		float r = __real_##name(x);				\
		acc[phase][ctx].libm += since(t0);			\
		libm_calls[phase]++;					\
		return r;						\
	}
#define WRAP_M2(name)							\
	float __real_##name(float x, float y);				\
	float __wrap_##name(float x, float y)				\
	{								\
		uint32_t t0 = now();					\
		float r = __real_##name(x, y);				\
		acc[phase][ctx].libm += since(t0);			\
		libm_calls[phase]++;					\
		return r;						\
	}

WRAP_M1(cosf)
WRAP_M1(sinf)
WRAP_M1(expf)
WRAP_M1(logf)
WRAP_M1(log10f)
WRAP_M1(sqrtf)
WRAP_M1(floorf)
WRAP_M2(atan2f)
WRAP_M2(powf)
