# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(lora_send)

FILE(GLOB app_sources src/*.c)
target_sources(app PRIVATE ${app_sources})

# Intercom TDMA variants: the L1/L2 radio layer is shared; each variant
# links its own main() (UART-shell bench test vs. TFT field console).
if(CONFIG_APP_TDMA_TEST OR CONFIG_APP_TDMA_CONSOLE)
  FILE(GLOB tdma_sources src/tdma/*.c)
  target_sources(app PRIVATE ${tdma_sources})
  target_include_directories(app PRIVATE src/tdma)
  # Driver-internal header, needed once at init to detach the native
  # driver's DIO1 gpio_callback (docs/tdma_layering.md). The installed
  # driver *is* the repo's vendored copy (scripts/setup_sdk.sh), so the
  # struct layout is the one this build links against.
  target_include_directories(app PRIVATE ${ZEPHYR_BASE}/drivers/lora/native/sx126x)
endif()

if(CONFIG_APP_TDMA_TEST)
  FILE(GLOB tdma_app_sources src/tdma_app/*.c)
  target_sources(app PRIVATE ${tdma_app_sources})
endif()

if(CONFIG_APP_TDMA_CONSOLE)
  FILE(GLOB tdma_console_sources src/tdma_console/*.c)
  target_sources(app PRIVATE ${tdma_console_sources})
  # For the shared display toolkit / touch cal headers (src/ui_widgets.h ...).
  target_include_directories(app PRIVATE src)
endif()
