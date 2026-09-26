# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(blue_frog)

# One firmware per unit type, each with its own overlay + conf pair:
#
#   TFT unit     CONFIG_APP_TDMA_CONSOLE  src/tdma_console  boards/..._tft.*
#   shield unit  CONFIG_APP_TDMA_FIELD    src/tdma_field    boards/..._shield.*
#
# A build given only one file of a pair, or neither, would link a front end
# the devicetree does not have and fail somewhere obscure. Stop here instead,
# with the two build lines.
if(CONFIG_APP_TDMA_CONSOLE)
  dt_nodelabel(unit_front_end NODELABEL ili9341)
elseif(CONFIG_APP_TDMA_FIELD)
  dt_nodelabel(unit_front_end NODELABEL ssd1306)
endif()
if(NOT unit_front_end)
  message(FATAL_ERROR
    "Build one unit type, passing both of its files:\n"
    "  TFT unit:    -DEXTRA_DTC_OVERLAY_FILE=boards/nrf5340dk_nrf5340_cpuapp_tft.overlay"
    " -DEXTRA_CONF_FILE=boards/nrf5340dk_nrf5340_cpuapp_tft.conf\n"
    "  shield unit: -DEXTRA_DTC_OVERLAY_FILE=boards/nrf5340dk_nrf5340_cpuapp_shield.overlay"
    " -DEXTRA_CONF_FILE=boards/nrf5340dk_nrf5340_cpuapp_shield.conf\n"
    "(see README.md, \"Building and flashing\").")
endif()

# Shared by both units: the L1/L2 TDMA radio layer, the SD card + soak
# loggers and the tone payload source, none of which depend on the display.
# tone_src is referenced only when the tone can be sent (CONFIG_SOAK_RTT or
# CONFIG_SOAK_PAYLOAD_TONE, see payload_src.h); otherwise the linker drops
# the code, so ramp builds link none of it.
FILE(GLOB tdma_sources src/tdma/*.c)
target_sources(app PRIVATE ${tdma_sources}
  src/tdma_console/sd_log.c
  src/tdma_console/soak_log.c
  src/tdma_console/tone_src.c)
# The RTT bench port (CONFIG_SOAK_RTT), shared the same way, and the Codec 2
# clip source it uploads into (the clip mode depends on it).
target_sources_ifdef(CONFIG_SOAK_RTT app PRIVATE
  src/tdma_console/rtt_link.c
  src/tdma_console/clip_src.c)
target_include_directories(app PRIVATE src/tdma src/tdma_console)
# Driver-internal header, needed once at init to detach the native driver's
# DIO1 gpio_callback (docs/tdma_layering.md). The installed driver *is* the
# repo's vendored copy (scripts/setup_sdk.sh), so the struct layout is the
# one this build links against.
target_include_directories(app PRIVATE ${ZEPHYR_BASE}/drivers/lora/native/sx126x)

if(CONFIG_APP_TDMA_CONSOLE)
  target_sources(app PRIVATE
    src/tdma_console/main.c
    src/tdma_console/ui_widgets.c
    src/tdma_console/touch_cal.c)
elseif(CONFIG_APP_TDMA_FIELD)
  FILE(GLOB tdma_field_sources src/tdma_field/*.c)
  target_sources(app PRIVATE ${tdma_field_sources})
endif()
