# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
cmake_minimum_required(VERSION 3.20)
project(xml C)

set(CMAKE_C_STANDARD 23)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(XML_REPO_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../../..)

file(GLOB_RECURSE SRC_FILES src/*.c)
if(SRC_FILES)
  add_library(xml STATIC ${SRC_FILES})
  target_include_directories(
    xml
    PUBLIC inc
    PRIVATE src ${XML_REPO_ROOT}/libs/ra8_core/inc
  )
endif()
