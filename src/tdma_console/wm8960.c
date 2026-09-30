/*
 * WM8960 control. Register numbers and bits from the WM8960 datasheet
 * (Rev 4.2). The codec's registers are write-only; the I2C ACK is the only
 * sign of life.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "wm8960.h"

#include <zephyr/device.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

#define WM8960_ADDR	0x1A

uint32_t wm8960_nacks;

static uint8_t spk_code = 0x6F;		/* -10 dB */
static bool spk_muted;

static int spk_write(uint8_t code);

static const struct device *const i2c = DEVICE_DT_GET(DT_NODELABEL(i2c1));

int wm8960_write(uint8_t reg, uint16_t val)
{
	/* 7-bit register address, 9-bit data: B15..9 address, B8..0 data. */
	uint8_t b[2] = { (uint8_t)((reg << 1) | ((val >> 8) & 1)),
			 (uint8_t)(val & 0xff) };

	int err;

	/* The bus runs on the nRF's weak internal pull-ups over jumpers and
	 * drops the odd byte: retry, and count it so it stays visible. */
	for (int tries = 0; tries < 5; tries++) {
		err = i2c_write(i2c, b, sizeof(b), WM8960_ADDR);
		if (!err) {
			return 0;
		}
		wm8960_nacks++;
		k_usleep(200);
	}
	printk("wm8960: R%u = 0x%03x failed 5 times (%d)\n", reg, val, err);
	return err;
}

int wm8960_hp_vol(uint8_t code)
{
	int err = wm8960_write(0x02, code & 0x7f);		/* LOUT1 */

	/* OUT1VU on the second write latches both. */
	return err ? err : wm8960_write(0x03, 0x100 | (code & 0x7f));
}

int wm8960_mic_gain(uint8_t pga, uint8_t boost)
{
	/* R32/R33 signal path: LMN1 (INPUT1 to the PGA's inverting input; the
	 * non-inverting input sits at VMID), boost, LMIC2B (PGA to boost). */
	uint16_t path = 0x100 | ((boost & 3) << 4) | 0x008;
	int err;

	err = wm8960_write(0x20, path);
	err = err ? err : wm8960_write(0x21, path);
	/* R0/R1: PGA volume, unmuted (bit 7 clear), IPVU on the second. */
	err = err ? err : wm8960_write(0x00, pga & 0x3f);
	return err ? err : wm8960_write(0x01, 0x100 | (pga & 0x3f));
}

int wm8960_init(void)
{
	/* reg, value */
	static const uint16_t seq[][2] = {
		/* Clocks: PLL from the 24 MHz crystal. MCLK / 2 = 12 MHz,
		 * R = 8.192 (N = 8, K = 0.192 * 2^24 = 0x3126E9): f2 =
		 * 98.304 MHz, / 4 fixed = 24.576, SYSCLKDIV / 2 = 12.288 MHz. */
		{ 0x34, 0x038 },	/* R52 PLL1: SDM, PRESCALE /2, N = 8 */
		{ 0x35, 0x031 },	/* R53..55 PLL K */
		{ 0x36, 0x026 },
		{ 0x37, 0x0E9 },
		/* Power. R25: VMID 2 x 50k, VREF, AINL/R, ADCL/R, MICB. */
		{ 0x19, 0x0FE },
		/* R26: DACL/R, LOUT1/ROUT1, SPKL/R, PLL on. OUT3 off. */
		{ 0x1A, 0x1F9 },
		/* R47: left/right input PGAs, left/right output mixers. */
		{ 0x2F, 0x03C },
	};
	static const uint16_t seq2[][2] = {
		/* R4 clocking: ADCDIV = DACDIV = /6 (SYSCLK / (6 x 256) =
		 * 8 kHz), SYSCLKDIV /2, CLKSEL = PLL. */
		{ 0x04, 0x1B5 },
		/* R8: BCLKDIV /32 -> 384 kHz. DCLKDIV /16 (default): the
		 * class D switching clock, 768 kHz. */
		{ 0x08, 0x1CD },
		/* R7: master, 24-bit words (48 BCLKs per frame, exactly filled;
		 * see main.c), I2S format. */
		{ 0x07, 0x04A },
		/* R34/R37: DAC to the left/right output mixers only. */
		{ 0x22, 0x100 },
		{ 0x25, 0x100 },
		/* R21/R22 ADC volume 0 dB, R10/R11 DAC volume 0 dB. */
		{ 0x15, 0x0C3 },
		{ 0x16, 0x1C3 },
		{ 0x0A, 0x0FF },
		{ 0x0B, 0x1FF },
		/* R5: DAC soft mute off, ADC high-pass on (default). */
		{ 0x05, 0x000 },
		/* Speaker terminals: R49 class D outputs L and R on; R51 class
		 * D DC and AC gain 1.52x (the HAT runs SPKVDD from 5 V). */
		{ 0x31, 0x0F7 },
		{ 0x33, 0x09B },
	};
	int err;

	if (!device_is_ready(i2c)) {
		return -ENODEV;
	}

	err = wm8960_write(0x0F, 0x000);			/* R15 reset */
	if (err) {
		return err;
	}
	k_msleep(10);

	for (size_t i = 0; i < ARRAY_SIZE(seq); i++) {
		err = wm8960_write(seq[i][0], seq[i][1]);
		if (err) {
			return err;
		}
	}
	k_msleep(20);		/* PLL lock, VMID settle */
	for (size_t i = 0; i < ARRAY_SIZE(seq2); i++) {
		err = wm8960_write(seq2[i][0], seq2[i][1]);
		if (err) {
			return err;
		}
	}

	/* Mics: Waveshare's ALSA defaults, PGA +12 dB (39), boost +29 dB (3).
	 * Headphones -12 dB (109). Speakers -10 dB (0x6F; 0 dB was loud). */
	err = wm8960_mic_gain(39, 3);
	err = err ? err : wm8960_hp_vol(109);
	return err ? err : wm8960_spk_vol(spk_code);
}

int wm8960_spk_vol(uint8_t code)
{
	spk_code = code & 0x7f;
	return spk_muted ? 0 : spk_write(spk_code);
}

int wm8960_spk_mute(bool mute)
{
	spk_muted = mute;
	return spk_write(mute ? 0 : spk_code);	/* <= 0x2F mutes */
}

static int spk_write(uint8_t code)
{
	int err = wm8960_write(0x28, code & 0x7f);		/* LOUT2 */

	/* SPKVU on the second write latches both. */
	return err ? err : wm8960_write(0x29, 0x100 | (code & 0x7f));
}
