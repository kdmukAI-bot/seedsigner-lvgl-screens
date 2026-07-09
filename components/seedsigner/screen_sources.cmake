# ── Single source of truth for the per-screen entry-point translation units ──
#
# Every file matching screens/*_screen.cpp is exactly one screen entry point
# (`void <name>_screen(void *ctx_json)`), whose basename equals the entry symbol,
# the name->function registry key, and the scenario name — one canonical string,
# zero transform. Globbing that directory tracks the corpus mechanically: adding
# or moving a screen needs zero edits to the build lists that include this file.
#
# CONFIGURE_DEPENDS re-globs at build time, so a newly added screen file is picked
# up without a manual re-configure — the same pattern this repo already uses for
# LVGL_SOURCES in the desktop-tool CMakeLists. It is ILLEGAL in CMake script mode
# (cmake -P), though, and ESP-IDF include()s each component's CMakeLists.txt in
# script mode during its early requirements-extraction pass
# (component_get_requirements.cmake) before the normal build configure. So guard it:
# a plain glob in script mode (that pass only reads REQUIRES, not this SRCS list),
# CONFIGURE_DEPENDS in the real configure (desktop tools + the ESP-IDF build pass).
#
# CMAKE_CURRENT_LIST_DIR resolves to THIS file's directory (components/seedsigner)
# even when include()d from another CMakeLists, so the glob yields absolute paths.
# Both idf_component_register(SRCS ...) (ESP32) and add_executable(...) (desktop
# tools) accept absolute source paths, so this one list serves both build styles.
if(CMAKE_SCRIPT_MODE_FILE)
    file(GLOB SEEDSIGNER_SCREEN_SOURCES
        "${CMAKE_CURRENT_LIST_DIR}/screens/*_screen.cpp")
else()
    file(GLOB SEEDSIGNER_SCREEN_SOURCES CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_LIST_DIR}/screens/*_screen.cpp")
endif()
