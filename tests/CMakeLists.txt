cmake_minimum_required(VERSION 3.20)

#
# ra8d2-firmware host-compiled unit test build.
#
# Compiles every library source file with the host (x86_64) compiler
# so clang-tidy has a valid compile_commands.json and so actual unit
# tests can link against mock peripheral implementations.
#
# No target cross-compilation happens here -- that lives in the
# top-level CMakeLists.txt.
#
# Usage from the firmware root:
#
#     cmake -B build/tidy -S tests -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
#     cmake --build build/tidy
#
# Or via:
#
#     bash scripts/clang_tidy.sh --check
#

project(ra8d2-firmware-tests
    LANGUAGES C
    VERSION 0.1.0
    DESCRIPTION "Host-compiled unit tests for ra8d2-firmware"
)

# Host C23 build -- the target uses the same standard.
set(CMAKE_C_STANDARD 23)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

include(${CMAKE_CURRENT_SOURCE_DIR}/../cmake/ra_warnings.cmake)

# Every TU is compiled with RA_SIMULATOR_MODE so code that normally
# reaches into MCU register blocks takes the host-safe path instead.
add_compile_definitions(RA_SIMULATOR_MODE UNIT_TEST)

# Host warnings are applied per target below. We deliberately do NOT use
# -Wstack-usage=2200 here because the host compiler ABI pushes wider
# arguments than Cortex-M85.

# Coverage instrumentation -- gcovr / lcov friendly.
option(RA_COVERAGE "Emit gcov/lcov coverage data" ON)
if(RA_COVERAGE)
  add_compile_options(-O0 -g --coverage -fprofile-arcs -ftest-coverage)
  add_link_options(--coverage -fprofile-arcs -ftest-coverage)
endif()

# Firmware root (one directory up from tests/).
get_filename_component(FW_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)

# Collect every library source. Each .c file becomes part of the
# `ra_core_hal` static library. clang-tidy then walks the associated
# compile_commands.json.
file(GLOB_RECURSE RA_CORE_SOURCES    CONFIGURE_DEPENDS ${FW_ROOT}/libs/ra_core/src/*.c)
file(GLOB_RECURSE RA_HAL_SOURCES     CONFIGURE_DEPENDS ${FW_ROOT}/libs/ra_hal/src/*.c)
file(GLOB_RECURSE RA_NET_PAL_SOURCES CONFIGURE_DEPENDS ${FW_ROOT}/libs/ra_net_pal/src/*.c)
file(GLOB_RECURSE RA_MODEM_AT_SOURCES CONFIGURE_DEPENDS ${FW_ROOT}/libs/ra_modem_at/src/*.c)
file(GLOB_RECURSE RA_NET_SOURCES     CONFIGURE_DEPENDS ${FW_ROOT}/libs/ra_net/src/*.c)
file(GLOB_RECURSE RA_BLE_HOST_SOURCES CONFIGURE_DEPENDS ${FW_ROOT}/libs/ra_ble_host/src/*.c)
file(GLOB_RECURSE RA_TLS_SOURCES     CONFIGURE_DEPENDS ${FW_ROOT}/libs/ra_tls/src/*.c)
file(GLOB_RECURSE RA_USB_PAL_SOURCES CONFIGURE_DEPENDS ${FW_ROOT}/libs/ra_usb_pal/src/*.c)
file(GLOB_RECURSE RA_FS_SOURCES      CONFIGURE_DEPENDS ${FW_ROOT}/libs/ra_fs/src/*.c)
file(GLOB_RECURSE RA_GFX_SOURCES     CONFIGURE_DEPENDS ${FW_ROOT}/libs/ra_gfx/src/*.c)
file(GLOB_RECURSE RA_NSC_SOURCES     CONFIGURE_DEPENDS ${FW_ROOT}/libs/ra_nsc/src/*.c)
file(GLOB_RECURSE RA_OTA_SOURCES     CONFIGURE_DEPENDS ${FW_ROOT}/libs/ra_ota/src/*.c)
file(GLOB_RECURSE RA_POWER_PROFILE_SOURCES CONFIGURE_DEPENDS ${FW_ROOT}/libs/ra_power_profile/src/*.c)
file(GLOB_RECURSE RA_EPUB_C_SOURCES  CONFIGURE_DEPENDS ${FW_ROOT}/libs/ra_epub/src/*.c)
file(GLOB_RECURSE RA_EPUB_CPP_SOURCES CONFIGURE_DEPENDS ${FW_ROOT}/libs/ra_epub/src/*.cpp)
# ra_reflow has two implementations: v1 (hand-rolled, default) and v2
# (LiteHTML-backed, RA_REFLOW_USE_LITEHTML=ON). Only one set is compiled.
option(RA_REFLOW_USE_LITEHTML "Use the LiteHTML v2 ra_reflow engine" OFF)
if(RA_REFLOW_USE_LITEHTML)
    set(RA_REFLOW_C_SOURCES    "")
    set(RA_REFLOW_CPP_SOURCES  ${FW_ROOT}/libs/ra_reflow/v2/ra_reflow_v2.cpp)
else()
    file(GLOB_RECURSE RA_REFLOW_C_SOURCES   CONFIGURE_DEPENDS ${FW_ROOT}/libs/ra_reflow/src/*.c)
    file(GLOB_RECURSE RA_REFLOW_CPP_SOURCES CONFIGURE_DEPENDS ${FW_ROOT}/libs/ra_reflow/src/*.cpp)
endif()
set(RA_EPUB_THIRD_PARTY
    ${FW_ROOT}/libs/third_party/miniz/miniz.c
    ${FW_ROOT}/libs/third_party/tinyxml2/tinyxml2.cpp
    ${FW_ROOT}/libs/third_party/stb/stb_truetype_impl.c
)
file(GLOB_RECURSE RA_SECURE_APP_SOURCES CONFIGURE_DEPENDS ${FW_ROOT}/src/secure_app/*.c)
file(GLOB_RECURSE RA_TOUCH_CAL_SOURCES  CONFIGURE_DEPENDS ${FW_ROOT}/libs/ra_touch_cal/src/*.c)
file(GLOB_RECURSE RA_PSA_CRYPTO_SOURCES CONFIGURE_DEPENDS ${FW_ROOT}/libs/ra_psa_crypto/src/*.c)
file(GLOB_RECURSE RA_WDT_SUPERVISOR_SOURCES CONFIGURE_DEPENDS ${FW_ROOT}/libs/ra_wdt_supervisor/src/*.c)
file(GLOB_RECURSE RA_MPU_SOURCES        CONFIGURE_DEPENDS ${FW_ROOT}/libs/ra_mpu/src/*.c)

add_library(ra_core_hal OBJECT
    ${RA_CORE_SOURCES}
    ${RA_HAL_SOURCES}
    ${RA_NET_PAL_SOURCES}
    ${RA_MODEM_AT_SOURCES}
    ${RA_NET_SOURCES}
    ${RA_BLE_HOST_SOURCES}
    ${RA_TLS_SOURCES}
    ${RA_USB_PAL_SOURCES}
    ${RA_FS_SOURCES}
    ${RA_GFX_SOURCES}
    ${RA_NSC_SOURCES}
    ${RA_OTA_SOURCES}
    ${RA_POWER_PROFILE_SOURCES}
    ${RA_EPUB_C_SOURCES}
    ${RA_EPUB_CPP_SOURCES}
    ${RA_REFLOW_C_SOURCES}
    ${RA_REFLOW_CPP_SOURCES}
    ${RA_EPUB_THIRD_PARTY}
    ${RA_SECURE_APP_SOURCES}
    ${RA_TOUCH_CAL_SOURCES}
    ${RA_MPU_SOURCES}
    ${RA_PSA_CRYPTO_SOURCES}
    ${RA_WDT_SUPERVISOR_SOURCES}
    ${CMAKE_CURRENT_SOURCE_DIR}/mocks/ra_sim_mmap.c
    ${CMAKE_CURRENT_SOURCE_DIR}/mocks/ra_sim_irq.c
    ${CMAKE_CURRENT_SOURCE_DIR}/mocks/ra_sim_dma.c
    ${CMAKE_CURRENT_SOURCE_DIR}/mocks/ra_sim_time.c
    ${CMAKE_CURRENT_SOURCE_DIR}/mocks/ra_sim_world.c
)
ra_target_enable_project_warnings(ra_core_hal)
target_compile_options(ra_core_hal PRIVATE
    -Wno-unused-function
    -Wno-unused-parameter
    -Wno-unused-variable
    -Wno-address-of-packed-member
)
target_include_directories(ra_core_hal PUBLIC
    ${FW_ROOT}/libs/ra_core/inc
    ${FW_ROOT}/libs/ra_hal/inc
    ${FW_ROOT}/libs/ra_net_pal/inc
    ${FW_ROOT}/libs/ra_modem_at/inc
    ${FW_ROOT}/libs/ra_net/inc
    ${FW_ROOT}/libs/ra_ble_host/inc
    ${FW_ROOT}/libs/ra_usb_pal/inc
    ${FW_ROOT}/libs/ra_fs/inc
    ${FW_ROOT}/libs/ra_gfx/inc
    ${FW_ROOT}/libs/ra_tls/inc
    ${FW_ROOT}/libs/ra_nsc/inc
    ${FW_ROOT}/libs/ra_ota/inc
    ${FW_ROOT}/libs/ra_power_profile/inc
    ${FW_ROOT}/libs/ra_epub/inc
    ${FW_ROOT}/libs/ra_reflow/inc
    ${FW_ROOT}/libs/ra_touch_cal/inc
    ${FW_ROOT}/libs/ra_mpu/inc
    ${FW_ROOT}/libs/ra_psa_crypto/inc
    ${FW_ROOT}/libs/ra_wdt_supervisor/inc
    ${FW_ROOT}/libs/third_party/miniz
    ${FW_ROOT}/libs/third_party/tinyxml2
    ${FW_ROOT}/libs/third_party/stb
    ${FW_ROOT}/src
    ${FW_ROOT}/src/inc
    ${FW_ROOT}/src/secure_app
    ${CMAKE_CURRENT_SOURCE_DIR}/mocks
)
# tinyxml2.cpp + ra_epub_xml_shim.cpp are C++; everything else is C.
# Override per-source language flags so cmake doesn't pass -std=c23 to the
# C++ compiler.
set_source_files_properties(
    ${FW_ROOT}/libs/third_party/tinyxml2/tinyxml2.cpp
    ${RA_EPUB_CPP_SOURCES}
    ${RA_REFLOW_CPP_SOURCES}
    PROPERTIES LANGUAGE CXX
)
set_source_files_properties(
    ${RA_EPUB_THIRD_PARTY}
    PROPERTIES COMPILE_OPTIONS -w
)
target_compile_features(ra_core_hal PUBLIC cxx_std_17)

# C++ enabled here only (the top-level project is C/ASM-only since the
# cross-compile path doesn't ship a C++ runtime). tinyxml2 + ra_epub_xml_shim
# in libs/ra_epub need C++17 for tinyxml2.
enable_language(CXX)

# Enable ctest so `cmake --build . --target test` and bash helpers work.
enable_testing()

# ---------------------------------------------------------------------------
# Unit tests -- one executable per test_*.c file. Each test links
# against the ra_core_hal OBJECT library and runs standalone.
# ---------------------------------------------------------------------------

function(ra_add_test name)
  add_executable(${name} ${name}.c $<TARGET_OBJECTS:ra_core_hal>)
  target_compile_options(${name} PRIVATE
    -Wall
    -Wextra
    -Werror
    -Wno-unused-function
    -Wno-unused-parameter
    -Wno-unused-variable
    -Wno-address-of-packed-member
  )
  set_target_properties(${name} PROPERTIES LINKER_LANGUAGE CXX)
  target_include_directories(${name} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${FW_ROOT}/libs/ra_core/inc
    ${FW_ROOT}/libs/ra_hal/inc
    ${FW_ROOT}/libs/ra_net_pal/inc
    ${FW_ROOT}/libs/ra_modem_at/inc
    ${FW_ROOT}/libs/ra_net/inc
    ${FW_ROOT}/libs/ra_ble_host/inc
    ${FW_ROOT}/libs/ra_usb_pal/inc
    ${FW_ROOT}/libs/ra_fs/inc
    ${FW_ROOT}/libs/ra_gfx/inc
    ${FW_ROOT}/libs/ra_tls/inc
    ${FW_ROOT}/libs/ra_nsc/inc
    ${FW_ROOT}/libs/ra_ota/inc
    ${FW_ROOT}/libs/ra_power_profile/inc
    ${FW_ROOT}/libs/ra_epub/inc
    ${FW_ROOT}/libs/ra_reflow/inc
    ${FW_ROOT}/libs/ra_touch_cal/inc
    ${FW_ROOT}/libs/ra_mpu/inc
    ${FW_ROOT}/libs/ra_psa_crypto/inc
    ${FW_ROOT}/libs/ra_wdt_supervisor/inc
    ${FW_ROOT}/libs/third_party/miniz
    ${FW_ROOT}/libs/third_party/tinyxml2
    ${FW_ROOT}/libs/third_party/stb
    ${FW_ROOT}/src/secure_app
    ${CMAKE_CURRENT_SOURCE_DIR}/mocks
  )
  if(APPLE)
    target_link_options(${name} PRIVATE
      "-Wl,-pagezero_size,0x4000"
      "-Wl,-segalign,0x4000"
    )
  endif()
  add_test(NAME ${name} COMMAND ${name})
endfunction()

# Auto-discover every test_*.c file in this directory and register it
# via ra_add_test(). Dropping a new test_foo.c file is enough -- no
# manual list edit required. (CONFIGURE_DEPENDS means CMake re-globs
# on the next build if the directory contents changed.)
file(GLOB RA_TEST_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/test_*.c)
foreach(src ${RA_TEST_SOURCES})
  get_filename_component(name ${src} NAME_WE)
  ra_add_test(${name})
endforeach()
