# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
#
# CMakeLists.txt for module_hello.
#
# Builds the kernel-side Module Manager host app.
#

cmake_minimum_required(VERSION 3.20)

get_directory_property(_ra8_has_parent PARENT_DIRECTORY)
if(NOT _ra8_has_parent)
  project(module_hello LANGUAGES C ASM)
endif()

# Dynamically resolve repository root directory
set(RA8_REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}")
while(NOT EXISTS "${RA8_REPO_ROOT}/cmake/ra8_add_app.cmake" AND NOT "${RA8_REPO_ROOT}" STREQUAL "/")
  get_filename_component(RA8_REPO_ROOT "${RA8_REPO_ROOT}" DIRECTORY)
endwhile()

# Include the shared per-app build macro
include("${RA8_REPO_ROOT}/cmake/ra8_add_app.cmake")

# Enable ThreadX and ThreadX Modules
set(RA8_USE_THREADX ON)
include("${RA8_REPO_ROOT}/cmake/threadx.cmake")
include("${RA8_REPO_ROOT}/cmake/threadx_modules.cmake")

# ---- Kernel-side app -------------------------------------------------------
ra8_add_app(
  NAME module_hello
  STACK_BYTES 4096
  DESCRIPTION "ThreadX Module Manager hello-world PoC"
  USES threadx
  EXTRA_SRCS tz_stubs.c
)

# Link the module manager library (not yet a recognised USES keyword).
# --allow-multiple-definition: the M85 module port defines MPU-aware
# MemManage_Handler/BusFault_Handler that must override vector_table.c.
target_link_libraries(module_hello.elf PRIVATE threadx_modules)
target_link_options(module_hello.elf PRIVATE -Wl,--allow-multiple-definition)

target_compile_definitions(module_hello.elf PRIVATE RA8_APP_UART_LOG)

# ---- Embed the module binary -----------------------------------------------
# Convert hello_module.bin → hello_module_bin.o via objcopy, then link it in.
# Run objcopy from the directory containing the .bin so symbols get short
# names: _binary_hello_module_bin_{start,end,size}.
set(MODULE_BIN_DIR "${CMAKE_CURRENT_SOURCE_DIR}/module/build")
set(MODULE_OBJ "${CMAKE_CURRENT_BINARY_DIR}/hello_module_bin.o")

add_custom_command(
  OUTPUT  ${MODULE_OBJ}
  COMMAND ${CMAKE_OBJCOPY} -I binary -O elf32-littlearm -B arm
          --rename-section .data=.rodata,alloc,load,readonly,data,contents
          hello_module.bin ${MODULE_OBJ}
  WORKING_DIRECTORY ${MODULE_BIN_DIR}
  DEPENDS ${MODULE_BIN_DIR}/hello_module.bin
  COMMENT "Embedding hello_module.bin into kernel ELF"
)
target_sources(module_hello.elf PRIVATE ${MODULE_OBJ})
