cmake_minimum_required(VERSION 3.16)
project(aeroprobe_ui_language LANGUAGES C CXX)
get_filename_component(ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../.." ABSOLUTE)
set(LVGL_DIR "${ROOT}/.pio/libdeps/esp32s3/lvgl")
file(GLOB_RECURSE LVGL_SOURCES CONFIGURE_DEPENDS "${LVGL_DIR}/src/*.c")
add_library(lvgl_native STATIC ${LVGL_SOURCES})
target_compile_definitions(lvgl_native PUBLIC LV_CONF_INCLUDE_SIMPLE LV_CONF_PATH=${CMAKE_CURRENT_SOURCE_DIR}/lv_conf.h)
target_include_directories(lvgl_native PUBLIC "${LVGL_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}")
set_target_properties(lvgl_native PROPERTIES C_STANDARD 99)
file(GLOB MONO_FONTS "${ROOT}/src/jetbrains_mono_*.c")
add_executable(ui_language ../ui_language_test.cpp "${ROOT}/src/noto_sans_sc_14.c" ${MONO_FONTS})
target_link_libraries(ui_language PRIVATE lvgl_native m)
target_include_directories(ui_language PRIVATE "${ROOT}/src")
target_compile_definitions(ui_language PRIVATE UI_SOURCE_ROOT="${ROOT}")
set_target_properties(ui_language PROPERTIES CXX_STANDARD 11 CXX_STANDARD_REQUIRED YES)

add_executable(ui_system_settings ../system_settings_test.cpp
    "${ROOT}/src/display_lcd.cpp" "${ROOT}/src/system_settings.cpp"
    "${ROOT}/src/runtime_log.cpp" "${ROOT}/src/audio.cpp"
    "${ROOT}/src/noto_sans_sc_14.c" ${MONO_FONTS})
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/aero_build_version.h" "#define AERO_BUILD_VERSION \"native-test\"\n")
target_include_directories(ui_system_settings PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_CURRENT_BINARY_DIR}" "../mocks" "${ROOT}/src")
target_link_libraries(ui_system_settings PRIVATE lvgl_native m)
target_compile_definitions(ui_system_settings PRIVATE ARDUINO=10800)
target_compile_options(ui_system_settings PRIVATE
    -include "${CMAKE_CURRENT_SOURCE_DIR}/lv_conf.h")
set_target_properties(ui_system_settings PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED YES)