/*
 * WM8960 on the BFab/Waveshare WM8960 Audio HAT: I2C control only.
 *
 * The codec is the I2S master. Its PLL makes SYSCLK = 12.288 MHz from the
 * HAT's 24 MHz crystal, and the ADC and DAC run at 8 kHz, I2S with 24-bit
 * words: BCLK = SYSCLK / 32 = 384 kHz is 48 bit clocks per frame, exactly
 * two 24-bit words (16-bit words misframe on the nRF slave). Mics: the HAT's two onboard mics on
 * LINPUT1 / RINPUT1 (Waveshare's ALSA defaults). Output: headphones on
 * LOUT1 / ROUT1 and speakers on the class D outputs.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef WM8960_H_
#define WM8960_H_

#include <stdbool.h>
#include <stdint.h>

/* Reset and configure the codec. 0, or a negative errno if the codec does
 * not ACK at 0x1A (wiring, 5 V, or pull-ups). */
int wm8960_init(void);

/* Writes that were NACKed and retried, since boot. */
extern uint32_t wm8960_nacks;

/* Raw register write (9-bit value). */
int wm8960_write(uint8_t reg, uint16_t val);

/* Headphone volume, LOUT1/ROUT1 code: 0x79 = 0 dB, 1 dB steps, <= 0x2F mute. */
int wm8960_hp_vol(uint8_t code);

/* Speaker volume, same codes as the headphones (0x7F = +6 dB). Kept, and
 * applied on unmute, while the speakers are muted. */
int wm8960_spk_vol(uint8_t code);

/* Mute or restore the speakers (the headphones are unaffected). */
int wm8960_spk_mute(bool mute);

/* Mic gain: PGA code 0..63 (0x17 = 0 dB, 0.75 dB steps, 0x3F = +30 dB) and
 * boost 0..3 (0, +13, +20, +29 dB). */
int wm8960_mic_gain(uint8_t pga, uint8_t boost);

#endif /* WM8960_H_ */
