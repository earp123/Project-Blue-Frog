/*
 * radio_cfg - staged LoRa modem configuration for the telemetry console.
 *
 * Holds an in-RAM "shadow" struct lora_modem_config that the UI edits freely.
 * Nothing reaches the radio until radio_cfg_apply() runs lora_config() once,
 * which is the commit point (lora_config re-inits the modem and is not free to
 * spam). The shadow is "dirty" whenever it differs from the last applied config.
 *
 * Setters mutate the shadow only and clamp to range. All SX126x datasheet
 * constraints live in radio_cfg_validate() so the UI can stay dumb.
 */

#ifndef RADIO_CFG_H
#define RADIO_CFG_H

#include <zephyr/drivers/lora.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

/* Documented operating limits, enforced by the setters and validate():
 *  - Frequency band: US 902-928 MHz ISM.
 *  - TX power: SX1262 PA range -9..+22 dBm.
 *  - Preamble: >= 6 generally; >= 12 for SF5/SF6 (SX126x minimum, see README).
 */
#define RADIO_FREQ_MIN_HZ      902000000U
#define RADIO_FREQ_MAX_HZ      928000000U
#define RADIO_PWR_MIN_DBM      (-9)
#define RADIO_PWR_MAX_DBM      (22)
#define RADIO_PREAMBLE_MIN     6U
#define RADIO_PREAMBLE_MIN_SF56 12U

/* Load sane defaults into the shadow and bring up RF_SW. Does not touch the
 * radio config registers; call radio_cfg_apply() to push the shadow.
 */
void radio_cfg_init(void);

const struct lora_modem_config *radio_cfg_shadow(void);
const struct lora_modem_config *radio_cfg_applied(void);

/* True when the shadow differs from the last successfully applied config. */
bool radio_cfg_dirty(void);

/* True when the LoRa device is ready (device_is_ready). */
bool radio_cfg_radio_ready(void);

/* Shadow setters. Numeric setters clamp to range and return false if the
 * requested value was out of range (the clamped value is still stored).
 * Steppers move by dir (-1/+1): enums wrap, numeric ranges clamp.
 */
bool radio_cfg_set_freq(uint32_t hz);
bool radio_cfg_set_sf(uint8_t sf);
void radio_cfg_step_sf(int dir);
void radio_cfg_step_bw(int dir);
void radio_cfg_step_cr(int dir);
bool radio_cfg_set_power(int dbm);
void radio_cfg_step_power(int dir);
bool radio_cfg_set_preamble(uint32_t n);
void radio_cfg_step_preamble(int dir);
void radio_cfg_toggle_crc(void);
void radio_cfg_toggle_iq(void);
void radio_cfg_toggle_pubnet(void);

/* Value strings for the config screen (written into the caller's buffer). */
void radio_cfg_freq_str(char *out, size_t n);
void radio_cfg_sf_str(char *out, size_t n);
void radio_cfg_bw_str(char *out, size_t n);
void radio_cfg_cr_str(char *out, size_t n);
void radio_cfg_power_str(char *out, size_t n);
void radio_cfg_preamble_str(char *out, size_t n);
void radio_cfg_crc_str(char *out, size_t n);
void radio_cfg_iq_str(char *out, size_t n);
void radio_cfg_pubnet_str(char *out, size_t n);

/* One-line summary of the last applied config, e.g. "915.0M SF5 BW500 CR4/5
 * +4dBm". Shows "(not applied)" before the first successful apply.
 */
void radio_cfg_summary(char *out, size_t n);

/* Returns 0 if the shadow is valid, negative otherwise; err gets the reason. */
int radio_cfg_validate(char *err, size_t errlen);

/* validate -> lora_config(shadow). On success copies shadow->applied and clears
 * dirty. Returns 0 on success, -ENODEV if the radio isn't ready, -EINVAL if the
 * shadow is invalid, or the driver's negative return code.
 */
int radio_cfg_apply(void);

#endif /* RADIO_CFG_H */
