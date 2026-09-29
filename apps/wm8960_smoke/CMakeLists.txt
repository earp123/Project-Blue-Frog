# SPDX-License-Identifier: Apache-2.0
#
# WM8960 HAT smoke test (temporary branch wm8960_smoke). A standalone app, not
# a unit firmware: one DK, one HAT, no radio, no display. See README.md here.

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(wm8960_smoke)

target_sources(app PRIVATE src/main.c src/wm8960.c)

# The two speech clips the soak tests use, first 8 s of each OSR WAV as raw
# 8 kHz 16-bit mono, linked into flash for playback.
set(CLIPS ${CMAKE_CURRENT_SOURCE_DIR}/../../soaks/clips)
set(GEN ${ZEPHYR_BINARY_DIR}/include/generated)
generate_inc_file_for_target(app ${CLIPS}/OSR_0010_F.pcm ${GEN}/clip_f.inc)
generate_inc_file_for_target(app ${CLIPS}/OSR_0030_M.pcm ${GEN}/clip_m.inc)
# The same two, band-limited to 300-3400 Hz for small speakers
# (tools/speech_filter.py).
generate_inc_file_for_target(app ${CLIPS}/OSR_0010_F_bp.pcm ${GEN}/clip_f_bp.inc)
generate_inc_file_for_target(app ${CLIPS}/OSR_0030_M_bp.pcm ${GEN}/clip_m_bp.inc)
