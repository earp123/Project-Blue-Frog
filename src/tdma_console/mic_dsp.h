/*
 * mic_dsp - conditioning of the encoder's 20 ms frames ahead of Codec 2
 * (CONFIG_SOAK_MIC_DSP). Task and results: docs/mic_dsp_frontend.md.
 *
 *   frame -> HPF -> [ NS -> AGC ] -> codec2_encode()
 *
 * HPF: 2nd-order Butterworth high-pass at 150 Hz (rumble, wind, handling).
 * NS and AGC: the SpeexDSP preprocessor (external/speexdsp, BSD), run on
 * two 80-sample (10 ms) sub-frames per frame. Its overlap-add delays the
 * audio by one sub-frame, 10 ms, whenever NS or AGC is on.
 *
 * Runs on the encoder thread, in both payload modes that encode: mic (live
 * HAT frames) and pcm (the uploaded clip, which makes a repeatable,
 * sample-aligned A/B). With the DSP off, or both Speex stages off and the
 * HPF off, the frame is not touched at all, so the encoder sees exactly
 * what it saw before this module existed.
 *
 * Every allocation comes from a static arena (os_support_custom.h), sized
 * at build time; mic_dsp_start() rebuilds the state from scratch each run.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef MIC_DSP_H_
#define MIC_DSP_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define MIC_DSP_HPF	0x1
#define MIC_DSP_NS	0x2
#define MIC_DSP_AGC	0x4
#define MIC_DSP_ALL	(MIC_DSP_HPF | MIC_DSP_NS | MIC_DSP_AGC)

/* Preprocessor settings (docs/mic_dsp_frontend.md, decisions 5 and 6).
 * AGC level is Speex's target loudness, in its own units; NS is the noise
 * attenuation, dB.
 */
#define MIC_DSP_AGC_LEVEL	8000
#define MIC_DSP_AGC_MAX_GAIN_DB	20
#define MIC_DSP_NS_DB		15

/* Run-time switches ("dsp on|off", "dsp hpf|ns|agc on|off"). Any thread;
 * the encoder applies them at its next frame. Default: on, all stages.
 */
void mic_dsp_set_on(bool on);
bool mic_dsp_get_on(void);
void mic_dsp_set_stages(uint32_t mask);
uint32_t mic_dsp_get_stages(void);

/* Tuning ("dsp level <n>", "dsp ns <dB>"), from the next run. */
void mic_dsp_set_agc_level(uint32_t level);
void mic_dsp_set_ns_db(uint32_t db);

/* Encoder thread, start of a run: fresh filter and preprocessor state.
 * Returns 0, or -ENOMEM if the arena is too small for the state.
 */
int mic_dsp_start(void);

/*
 * Encoder thread: condition one frame (n = 160 samples, 20 ms) in place.
 * Returns true if any stage ran, i.e. pcm may have changed.
 */
bool mic_dsp_frame(int16_t *pcm, size_t n);

struct mic_dsp_stats {
	bool on;
	uint32_t stages;	/* MIC_DSP_* applied at the last frame */
	uint32_t frames;	/* frames conditioned this run */
	uint32_t us_avg;	/* per 20 ms frame, this run */
	uint32_t us_max;
	uint32_t state_bytes;	/* arena used by the preprocessor */
	uint32_t arena_bytes;	/* arena size */
	int32_t agc_gain_db;	/* the AGC's current gain */
	int32_t noise_db;	/* mean noise estimate, dB re 1 LSB^2/bin */
	uint32_t speech_pct;	/* speech probability of the last frame, % */
	uint32_t clipped;	/* output samples at full scale, this run */
	int16_t peak;		/* output peak since the previous stats call */
};

/* Snapshot; clears the peak. */
void mic_dsp_get_stats(struct mic_dsp_stats *out);

#endif /* MIC_DSP_H_ */
