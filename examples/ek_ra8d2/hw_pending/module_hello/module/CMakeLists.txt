# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
#
# Builds the module-side binary for the hello_module ThreadX Module.
# This produces a .bin that the kernel-side module_hello app loads.

cmake_minimum_required(VERSION 3.20)
project(hello_module LANGUAGES C ASM)

# Resolve repo root
set(RA8_REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}")
while(NOT EXISTS "${RA8_REPO_ROOT}/cmake/toolchain-ra8d2.cmake" AND NOT "${RA8_REPO_ROOT}" STREQUAL "/")
  get_filename_component(RA8_REPO_ROOT "${RA8_REPO_ROOT}" DIRECTORY)
endwhile()

set(RA8_THREADX_ROOT "${RA8_REPO_ROOT}/libs/third_party/threadx")
set(TXM_PORT_DIR "${RA8_THREADX_ROOT}/ports_module/cortex_m85/gnu")

# C standard
set(CMAKE_C_STANDARD 23)
set(CMAKE_C_STANDARD_REQUIRED ON)

# Collect all module-lib sources (the module-side ThreadX API wrappers)
file(GLOB TXM_MODULE_LIB_SRCS
  "${RA8_THREADX_ROOT}/common_modules/module_lib/src/*.c"
  "${TXM_PORT_DIR}/module_lib/src/*.c"
)

# Module binary
add_executable(hello_module.elf
  main.c
  ${TXM_PORT_DIR}/example_build/txm_module_preamble.S
  ${TXM_PORT_DIR}/example_build/gcc_setup.s
  ${TXM_MODULE_LIB_SRCS}
)

target_compile_definitions(hello_module.elf PRIVATE
  TXM_MODULE
  TX_INCLUDE_USER_DEFINE_FILE
)

target_include_directories(hello_module.elf SYSTEM PRIVATE
  ${RA8_THREADX_ROOT}/common/inc
  ${RA8_THREADX_ROOT}/common_modules/inc
  ${RA8_THREADX_ROOT}/common_modules/module_lib/inc
  ${TXM_PORT_DIR}/inc
  ${RA8_REPO_ROOT}/port/threadx/inc
)

target_compile_options(hello_module.elf PRIVATE
  -mcpu=cortex-m85
  -mthumb
  -mfloat-abi=hard
  -mfpu=fpv5-sp-d16
  -fdata-sections
  -ffunction-sections
  -Os
  -g3
  -w
)

target_link_options(hello_module.elf PRIVATE
  -mcpu=cortex-m85
  -mthumb
  -mfloat-abi=hard
  -mfpu=fpv5-sp-d16
  -nostartfiles
  -nodefaultlibs
  -nostdlib
  -T${CMAKE_CURRENT_SOURCE_DIR}/module.ld
  -Wl,--gc-sections
  -Wl,--Map=hello_module.map
  -Wl,--entry=__txm_module_preamble
)

# Generate .bin from .elf
add_custom_command(TARGET hello_module.elf POST_BUILD
  BYPRODUCTS hello_module.bin
  COMMAND ${CMAKE_OBJCOPY} -O binary hello_module.elf hello_module.bin
  COMMAND ${CMAKE_SIZE} hello_module.elf
  COMMENT "Generating hello_module.bin and showing size"
)
