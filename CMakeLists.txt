# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(lora_send)

FILE(GLOB app_sources src/*.c)
target_sources(app PRIVATE ${app_sources})

# Intercom TDMA test variant: L1/L2 radio layer + its own main().
if(CONFIG_APP_TDMA_TEST)
  FILE(GLOB tdma_sources src/tdma/*.c src/tdma_app/*.c)
  target_sources(app PRIVATE ${tdma_sources})
  target_include_directories(app PRIVATE src/tdma)
  # Driver-internal header, needed once at init to detach the native
  # driver's DIO1 gpio_callback (docs/tdma_layering.md). The installed
  # driver *is* the repo's vendored copy (scripts/setup_sdk.sh), so the
  # struct layout is the one this build links against.
  target_include_directories(app PRIVATE ${ZEPHYR_BASE}/drivers/lora/native/sx126x)
endif()
