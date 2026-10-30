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

# Codec 2 on the device (CONFIG_SOAK_C2_ENCODE): the vendored subset in
# external/codec2 (LGPL-2.1, see its README) as its own static library, and
# the encoder thread that drives it. __EMBEDDED__ keeps the codebooks const
# (flash) and routes the codec's MALLOC to codec2_malloc() (c2_enc.c);
# -ffp-contract=off stops
# GCC fusing multiply-adds, so the device computes what the host reference
# (pycodec2) computes. Upstream code, upstream warnings: silenced.
if(CONFIG_SOAK_C2_ENCODE)
  FILE(GLOB codec2_sources external/codec2/src/*.c)
  add_library(codec2 STATIC ${codec2_sources})
  target_link_libraries(codec2 PRIVATE zephyr_interface)
  # FFTs on CMSIS-DSP (CONFIG_SOAK_C2_ARM_FFT): upstream's FDV_ARM_MATH path.
  # PUBLIC, so every file that includes codec2_fft.h agrees on the types.
  if(CONFIG_SOAK_C2_ARM_FFT)
    target_compile_definitions(codec2 PUBLIC FDV_ARM_MATH)
  endif()
  # Only mode 3200 is compiled in; the others' tables never reach flash.
  target_compile_definitions(codec2 PUBLIC CODEC2_MODE_EN_DEFAULT=0
    CODEC2_MODE_3200_EN=1)
  target_compile_definitions(codec2 PUBLIC __EMBEDDED__)
  # Upstream builds in GNU C; Zephyr's -std=c99 hides M_PI and friends.
  target_compile_definitions(codec2 PRIVATE _DEFAULT_SOURCE)
  # -O2 through a force-included pragma (external/codec2_opt.h): Zephyr's
  # -Os lands after target options, so -O2 here would lose.
  # -fsingle-precision-constant: the sources write constants like 2.0*PI,
  # which promote float maths to double, and the M33's FPU is single
  # precision only, so every such expression ran in software
  # (__aeabi_dmul & co. in every hot file).
  target_compile_options(codec2 PRIVATE -w -ffp-contract=off
    -fsingle-precision-constant
    -include ${CMAKE_CURRENT_SOURCE_DIR}/external/codec2_opt.h)
  target_include_directories(codec2 PUBLIC external/codec2/src
    external/codec2/include)
  target_link_libraries(app PRIVATE codec2)
  target_sources(app PRIVATE src/tdma_console/c2_enc.c
    src/tdma_console/c2_dec.c)
  # Mic DSP front end (CONFIG_SOAK_MIC_DSP): the vendored SpeexDSP
  # preprocessor subset in external/speexdsp (BSD, see its README) as its
  # own static library. Same -O2 pragma and single-precision constants as
  # Codec 2, and float_math.h maps its double libm calls to the float ones;
  # config.h and the allocator hooks are in external/speexdsp/port.
  if(CONFIG_SOAK_MIC_DSP)
    add_library(speexdsp STATIC
      external/speexdsp/libspeexdsp/preprocess.c
      external/speexdsp/libspeexdsp/filterbank.c
      external/speexdsp/libspeexdsp/fftwrap.c
      external/speexdsp/libspeexdsp/smallft.c
      external/speexdsp/port/echo_stub.c)
    target_link_libraries(speexdsp PRIVATE zephyr_interface)
    target_compile_definitions(speexdsp PRIVATE HAVE_CONFIG_H)
    # SHELL: keeps CMake from merging the two -include flags into one.
    target_compile_options(speexdsp PRIVATE -w -fsingle-precision-constant
      "SHELL:-include ${CMAKE_CURRENT_SOURCE_DIR}/external/codec2_opt.h"
      "SHELL:-include ${CMAKE_CURRENT_SOURCE_DIR}/external/speexdsp/port/float_math.h")
    target_include_directories(speexdsp PRIVATE external/speexdsp/port
      external/speexdsp/libspeexdsp)
    target_include_directories(speexdsp PUBLIC external/speexdsp/include)
    target_link_libraries(app PRIVATE speexdsp)
    target_sources(app PRIVATE src/tdma_console/mic_dsp.c)
  endif()
  # Per-function profile (CONFIG_SOAK_C2_PROFILE): the linker sends the
  # codec's cross-file calls to timing wrappers in c2_prof.c.
  if(CONFIG_SOAK_C2_PROFILE)
    target_sources(app PRIVATE src/tdma_console/c2_prof.c)
    foreach(fn codec2_encode codec2_decode dft_speech nlp
        two_stage_pitch_refinement estimate_amplitudes est_voicing_mbe
        speech_to_uq_lsps encode_lspds_scalar decode_lspds_scalar lsp_to_lpc
        aks_to_M2 phase_synth_zero_order postfilter synthesise
        kiss_fft kiss_fftr kiss_fftri arm_cfft_f32 arm_rfft_fast_f32
        cosf sinf expf logf log10f sqrtf floorf atan2f powf)
      zephyr_ld_options(-Wl,--wrap=${fn})
    endforeach()
  endif()
endif()
# Opus on the device (CONFIG_SOAK_OPUS_DEC): the vendored libopus 1.5.2
# subset in external/opus (BSD-3-Clause, see its README) as its own static
# library. Fixed point, no float API, no DNN extensions; scratch on the
# calling thread's stack (VAR_ARRAYS), so decode needs no heap. Same -O2
# pragma as Codec 2. Upstream code, upstream warnings: silenced.
if(CONFIG_SOAK_OPUS_DEC)
  FILE(GLOB opus_sources external/opus/celt/*.c external/opus/silk/*.c
    external/opus/silk/fixed/*.c external/opus/src/*.c)
  add_library(opus STATIC ${opus_sources})
  target_link_libraries(opus PRIVATE zephyr_interface)
  target_compile_definitions(opus PRIVATE OPUS_BUILD FIXED_POINT
    DISABLE_FLOAT_API VAR_ARRAYS "PACKAGE_VERSION=\"1.5.2\"")
  target_compile_options(opus PRIVATE -w
    -include ${CMAKE_CURRENT_SOURCE_DIR}/external/codec2_opt.h)
  target_include_directories(opus PRIVATE external/opus/celt
    external/opus/silk external/opus/silk/fixed external/opus/src)
  target_include_directories(opus PUBLIC external/opus/include)
  target_link_libraries(app PRIVATE opus)
  target_sources(app PRIVATE src/tdma_console/opus_dec.c)
endif()
# The WM8960 HAT (CONFIG_SOAK_AUDIO_HAT): mic and earpieces, for either
# codec's decoder.
target_sources_ifdef(CONFIG_SOAK_AUDIO_HAT app PRIVATE
  src/tdma_console/hat_audio.c src/tdma_console/wm8960.c)
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
