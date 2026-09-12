# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
#
# apps/host/cbz2jof -- CBZ to JOF host converter: a native DEV MACHINE
# executable (x86_64 / arm64), NOT an ARM ELF.
#
# The Go CLI owns archive policy (entry selection, limits, atomic page
# publication) and shells out once per image to the jof-worker C binary,
# which runs the firmware's own JOF producer (jof_produce) so a page
# converted here is byte-identical to one the RA8 would produce from the
# same source bytes.
#
#   cmake -B build -S . && cmake --build build -j && ctest --test-dir build
#
# Layout mirrors apps/host/mdl: the C worker interface lives in inc/, its
# implementation in src/, the Go CLI in cmd/cbz2jof/, and the Go tests in
# tests/.
cmake_minimum_required(VERSION 3.20)
project(cbz2jof C)

set(CMAKE_C_STANDARD 23)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

enable_testing()

# The repository root, stated ONCE. This listfile reaches out of its own tree
# for the firmware JOF/JPEG/compress/core libraries and the vendored WebP
# recipe, and it is configured standalone (its own CMAKE_SOURCE_DIR) as well
# as from the top-level RA8_HOST_MODE build, so CMAKE_SOURCE_DIR cannot
# serve.
set(CBZ2JOF_REPO_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../../..)
get_filename_component(CBZ2JOF_REPO_ROOT "${CBZ2JOF_REPO_ROOT}" ABSOLUTE)
set(CBZ2JOF_FW ${CBZ2JOF_REPO_ROOT}/libs)

find_program(CBZ2JOF_GO_EXECUTABLE NAMES go)

# ---------------------------------------------------------------------------
# jof-worker: the per-page C transcode.
#
# The exact firmware members it calls: the JOF producer (with its JPEG / PNG
# / WebP front ends) plus the compression seam they sit on. Naming members
# rather than globbing keeps the HAL and peripheral layers out of a host tool.
# ---------------------------------------------------------------------------
set(CBZ2JOF_FW_SRC
    ${CBZ2JOF_REPO_ROOT}/apps/shared_libs/jof/src/jof.c
    ${CBZ2JOF_REPO_ROOT}/apps/shared_libs/jof/src/jof_produce.c
    ${CBZ2JOF_REPO_ROOT}/apps/shared_libs/jof/src/jof_produce_webp.c
    ${CBZ2JOF_REPO_ROOT}/apps/shared_libs/jof/src/jof_png.c
    ${CBZ2JOF_REPO_ROOT}/apps/shared_libs/jof/src/jof_png_chunk.c
    ${CBZ2JOF_REPO_ROOT}/apps/shared_libs/compress/src/ra8_compress.c
    ${CBZ2JOF_FW}/ra8_core/src/ra8_log.c
    ${CBZ2JOF_FW}/ra8_core/src/ra8_error_handler.c
    ${CBZ2JOF_FW}/ra8_core/src/ra8_decomp_limits.c
)
set(CBZ2JOF_JPEG_SRC
    ${CBZ2JOF_FW}/ra8_jpeg/src/ra8_jpeg_sw.c ${CBZ2JOF_FW}/ra8_jpeg/src/ra8_jpeg_sw_decode.c
    ${CBZ2JOF_FW}/ra8_jpeg/src/ra8_jpeg_sw_stream.c
)
set(CBZ2JOF_MINIZ_SRC ${CBZ2JOF_REPO_ROOT}/apps/shared_libs/third_party/miniz/miniz.c)
set(CBZ2JOF_MINIZ_DEFINITIONS MINIZ_NO_ARCHIVE_APIS MINIZ_NO_MALLOC MINIZ_NO_STDIO MINIZ_NO_TIME)

# Per-source properties are DIRECTORY-scoped, so the miniz quiet flags are
# restated here: without them the vendored decoder meets -Wall -Wextra
# -Werror. Measured on miniz.c under gcc 14.2.0 and clang 18: only
# -Wunused-parameter fires, so only it is silenced.
set(_cbz2jof_wno_miniz -Wno-unused-parameter # miniz.c mz_zip_writer_add_mem_ex(last_modified)
                       -fno-strict-aliasing
)
set_source_files_properties(
  ${CBZ2JOF_MINIZ_SRC} PROPERTIES COMPILE_OPTIONS "${_cbz2jof_wno_miniz}"
                                  COMPILE_DEFINITIONS "MINIZ_NO_MALLOC;MINIZ_NO_STDIO"
)

add_executable(
  jof-worker
  ${CMAKE_CURRENT_SOURCE_DIR}/src/jof_worker.c
  ${CMAKE_CURRENT_SOURCE_DIR}/src/jof_worker_main.c
  ${CBZ2JOF_FW_SRC}
  ${CBZ2JOF_JPEG_SRC}
  ${CBZ2JOF_MINIZ_SRC}
)

target_include_directories(
  jof-worker
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/inc
          ${CBZ2JOF_REPO_ROOT}/apps/shared_libs/jof/inc
          ${CBZ2JOF_REPO_ROOT}/apps/shared_libs/jof/src
          ${CBZ2JOF_FW}/ra8_jpeg/inc
          ${CBZ2JOF_FW}/ra8_jpeg/src
          ${CBZ2JOF_FW}/ra8_io/inc
          ${CBZ2JOF_REPO_ROOT}/apps/shared_libs/compress/inc
          ${CBZ2JOF_FW}/ra8_core/inc
          ${CBZ2JOF_REPO_ROOT}/apps/shared_libs/third_party/miniz
)

# WebP decode (vendored libwebp + the ra8_webp facade), so the producer's
# WebP arm above is the real transcode rather than a link-satisfying stub.
# One call: the recipe lives in cmake/ra8_webp_vendor.cmake, shared with the
# firmware app recipe and the host test build.
include(${CBZ2JOF_REPO_ROOT}/cmake/ra8_webp_vendor.cmake)
ra8_webp_attach(jof-worker ${CBZ2JOF_REPO_ROOT})

target_compile_definitions(jof-worker PRIVATE RA8_OFF_TARGET ${CBZ2JOF_MINIZ_DEFINITIONS})
target_compile_options(jof-worker PRIVATE -Wall -Wextra -Werror)

# The worker target binary stays in its own lane; the copy below is the one
# the Go CLI resolves as its executable sibling.
set_target_properties(
  jof-worker PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/worker
)

# ---------------------------------------------------------------------------
# cbz2jof: the Go CLI. Its output is an ALL target so a plain build produces
# the runnable pair; the worker copy lands beside it so sibling resolution
# finds jof-worker with no flags or environment.
# ---------------------------------------------------------------------------
if(CBZ2JOF_GO_EXECUTABLE)
  set(CBZ2JOF_CLI ${CMAKE_CURRENT_BINARY_DIR}/cbz2jof)
  set(CBZ2JOF_GO_DIR ${CMAKE_CURRENT_SOURCE_DIR}/go)
  add_custom_command(
    OUTPUT ${CBZ2JOF_CLI}
    COMMAND ${CBZ2JOF_GO_EXECUTABLE} build -trimpath -o ${CBZ2JOF_CLI} ./cmd/cbz2jof
    WORKING_DIRECTORY ${CBZ2JOF_GO_DIR}
    DEPENDS ${CBZ2JOF_GO_DIR}/go.mod ${CBZ2JOF_GO_DIR}/cbz2jof.go
            ${CBZ2JOF_GO_DIR}/cmd/cbz2jof/main.go
    COMMENT "Building cbz2jof CLI (go build -trimpath)"
    VERBATIM
  )
  add_custom_command(
    OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/jof-worker
    COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:jof-worker>
            ${CMAKE_CURRENT_BINARY_DIR}/jof-worker
    DEPENDS jof-worker
    COMMENT "Staging jof-worker beside the cbz2jof CLI"
    VERBATIM
  )
  add_custom_target(
    cbz2jof_cli ALL DEPENDS ${CBZ2JOF_CLI} ${CMAKE_CURRENT_BINARY_DIR}/jof-worker
    COMMENT "Building the cbz2jof CLI and staging its worker"
  )

  # ---------------------------------------------------------------------------
  # Go tests. The unit suite is untagged; the real-worker suite carries the
  # `realworker` build tag and runs the just-built worker through the
  # CBZ2JOF_WORKER environment, so it never depends on an installed binary.
  # ---------------------------------------------------------------------------
  add_test(
    NAME cbz2jof_go_unit
    COMMAND ${CBZ2JOF_GO_EXECUTABLE} test ./...
    WORKING_DIRECTORY ${CBZ2JOF_GO_DIR}
  )
  set_tests_properties(cbz2jof_go_unit PROPERTIES TIMEOUT 120)

  add_test(
    NAME cbz2jof_go_realworker
    COMMAND ${CBZ2JOF_GO_EXECUTABLE} test -tags realworker ./tests/
    WORKING_DIRECTORY ${CBZ2JOF_GO_DIR}
  )
  set_tests_properties(
    cbz2jof_go_realworker
    PROPERTIES TIMEOUT
               180
               LABELS
               "integration"
               ENVIRONMENT
               "CBZ2JOF_WORKER=$<TARGET_FILE:jof-worker>"
  )
else()
  message(STATUS "cbz2jof: Go toolchain not found; building jof-worker C target only")
endif()
