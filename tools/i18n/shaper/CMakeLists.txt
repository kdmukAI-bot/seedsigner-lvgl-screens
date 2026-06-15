cmake_minimum_required(VERSION 3.16)
project(lv_shape LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# ---------------------------------------------------------------------------
# lv_shape — Arabic/Persian shaping oracle for the font-pack builder.
#
# Links the SAME third_party/lvgl all ecosystem consumers share and compiles
# lv_text_ap.c (LV_USE_ARABIC_PERSIAN_CHARS=1), so the presentation forms it
# emits for a corpus are exactly what each consumer's renderer will request.
# Mirrors the LVGL detection + LV_CONF_SKIP compile flags of the desktop apps.
# ---------------------------------------------------------------------------

# This file lives at tools/i18n/shaper; repo root is three levels up.
set(PROJECT_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../..")

if(NOT DEFINED LVGL_ROOT OR LVGL_ROOT STREQUAL "")
    # Preferred: pinned submodule in third_party/
    set(LVGL_ROOT "${PROJECT_ROOT}/third_party/lvgl")
endif()
if(NOT EXISTS "${LVGL_ROOT}/lvgl.h")
    set(LVGL_ROOT "${PROJECT_ROOT}/../micropython/ports/esp32/managed_components/lvgl__lvgl")
endif()
if(NOT EXISTS "${LVGL_ROOT}/lvgl.h")
    set(LVGL_ROOT "${PROJECT_ROOT}/references/micropython/ports/esp32/managed_components/lvgl__lvgl")
endif()
if(NOT EXISTS "${LVGL_ROOT}/lvgl.h")
    message(FATAL_ERROR "LVGL root not found. Pass -DLVGL_ROOT=<path to .../lvgl__lvgl>")
endif()

file(GLOB_RECURSE LVGL_SOURCES CONFIGURE_DEPENDS "${LVGL_ROOT}/src/*.c")

add_executable(lv_shape
    "${CMAKE_CURRENT_LIST_DIR}/lv_shape.c"
    ${LVGL_SOURCES}
)

target_compile_definitions(lv_shape PRIVATE
    LV_CONF_SKIP
    # No ARM NEON on x86 desktop builds.
    LV_USE_DRAW_SW_ASM=LV_DRAW_SW_ASM_NONE
    # Real malloc/free instead of LVGL's tiny built-in fixed pool.
    LV_USE_STDLIB_MALLOC=LV_STDLIB_CLIB
    # Mirror the render-time i18n config: shaping (the reason this tool exists)
    # plus bidi, so the LVGL we link matches what the consumers render with.
    LV_USE_ARABIC_PERSIAN_CHARS=1
    LV_USE_BIDI=1
)

target_include_directories(lv_shape PRIVATE "${LVGL_ROOT}")
