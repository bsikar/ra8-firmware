# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie

# media_dl -- the STAND-ALONE HOST FORM of the comic/manga/manhwa/webtoon
# downloader: a native DEV MACHINE executable (x86_64 / arm64), NOT an ARM ELF.
#
# The downloader itself is not here. It lives once, at apps/shared/media_dl,
# because the same core is about to have a second build form (a loadable
# on-device module under apps/threadx_modules/) and a core owned by one form is
# a core the other form has to copy. What IS here is the composition root:
#
#   src/main.c                 process entry, signals, POSIX stream binding
#   src/mdl_cli*.c             the argv grammar and its diagnostics
#   src/mdl_net_curl*.c        the libcurl backend, injected through the
#                              core's ::mdl_net_iface_t vtable
#   src/mdl_host_credentials.c host cookie-jar / CA-bundle policy
#   src/mdl_app_*.c            the per-mode action wiring that binds a POSIX
#                              filesystem and a libcurl transport into the core
#
# The layering is one-way and is a directory rule: apps/shared MUST NEVER
# include from apps/stand_alone or apps/threadx_modules -- shared is BELOW the
# forms. This listfile therefore names apps/shared; the shared listfile names
# no form.
cmake_minimum_required(VERSION 3.20)
project(media_dl C)

set(CMAKE_C_STANDARD 23)
set(CMAKE_C_STANDARD_REQUIRED ON)

# The repository root, stated ONCE. This listfile reaches out of its own tree
# for the shared core, the firmware libraries, the POSIX port, the shared test
# support and the vendored WebP recipe, and it is configured standalone (its
# own CMAKE_SOURCE_DIR) by the tools-build and coverage-tree gates, so
# CMAKE_SOURCE_DIR cannot serve. Every such reference goes through this
# variable: when the product moved from tools/media_dl to
# apps/stand_alone/media_dl, 59 hand-written ../.. prefixes all became wrong
# together and CMake reported only the first.
set(MDL_REPO_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../../..)

# Keep host-tool coverage opt-in, matching tests/cmake/host_config.cmake. The
# flags are added after project() so CMake's compiler probe is not instrumented
# and gcovr never mistakes compiler-identification objects for project code.
# Added here, at the top-level directory scope, so the shared core added below
# inherits them: the coverage-tree ratchet measures core files, and a core
# built without --coverage would report zero instrumented lines for all of them.
option(RA8_COVERAGE "Emit gcov coverage data for media_dl and its tests" OFF)
if(RA8_COVERAGE)
  add_compile_options(
    --coverage
    -fprofile-arcs
    -ftest-coverage
    -O0
    -g
  )
  add_link_options(--coverage -fprofile-arcs -ftest-coverage)
endif()

# libcurl is a dependency OF THIS FORM, not of the downloader. That the shared
# listfile has no find_package(CURL) is the load-bearing evidence that the core
# is portable, so keep it here.
find_package(CURL REQUIRED)

# Testing is enabled before the core is added so ctest walks into it: CTest only
# descends into subdirectories of a directory where testing is on.
enable_testing()

# The shared downloader core: its library target (media_dl_core), the
# EPUB/RABOOK runtime it needs, its own 17 test binaries, and the source-list
# and include-root variables this form composes with. add_subdirectory rather
# than include() because the core is a project in its own right -- it configures
# and tests standalone -- and its targets must be real targets this form links,
# not source lists it re-compiles. It publishes what a form needs through
# PARENT_SCOPE, which is why the composition below never spells a core path by
# hand.
add_subdirectory(${MDL_REPO_ROOT}/apps/shared/media_dl ${CMAKE_CURRENT_BINARY_DIR}/media_dl_core)

# Per-source properties are DIRECTORY-scoped, so the SOUP flags the core sets on
# miniz do not follow the file into this listfile. Every target below that
# compiles miniz needs them restated here -- without them the vendored decoder
# meets -Wall -Wextra -Werror and MINIZ_NO_MALLOC silently turns back on.
set_source_files_properties(
  ${MDL_MINIZ} PROPERTIES COMPILE_OPTIONS "-w;-fno-strict-aliasing"
                          COMPILE_DEFINITIONS "MINIZ_NO_STDIO;MINIZ_NO_MALLOC"
)

# The include roots this form's own translation units need: the core's public
# contract, plus its own inc/ for the two headers only the composition root
# publishes (mdl_cli.h, mdl_net_curl.h).
set(MDL_FORM_INC ${MDL_PUBLIC_INC} ${CMAKE_CURRENT_SOURCE_DIR}/inc)
set(MDL_FORM_SRC_DIR ${CMAKE_CURRENT_SOURCE_DIR}/src)
set(MDL_FORM_TESTS_DIR ${CMAKE_CURRENT_SOURCE_DIR}/tests)

add_executable(
  media_dl
  ${MDL_FORM_SRC_DIR}/main.c
  ${MDL_FORM_SRC_DIR}/mdl_app_admin.c
  ${MDL_FORM_SRC_DIR}/mdl_app_cover.c
  ${MDL_FORM_SRC_DIR}/mdl_app_direct.c
  ${MDL_FORM_SRC_DIR}/mdl_app_library.c
  ${MDL_FORM_SRC_DIR}/mdl_app_prepare.c
  ${MDL_FORM_SRC_DIR}/mdl_app_series.c
  ${MDL_FORM_SRC_DIR}/mdl_app_storage.c
  ${MDL_FORM_SRC_DIR}/mdl_cli.c
  ${MDL_FORM_SRC_DIR}/mdl_cli_nums.c
  ${MDL_FORM_SRC_DIR}/mdl_cli_stream.c
  ${MDL_FORM_SRC_DIR}/mdl_cli_usage.c
  ${MDL_FORM_SRC_DIR}/mdl_net_curl.c
  ${MDL_FORM_SRC_DIR}/mdl_net_curl_credentials.c
  ${MDL_FORM_SRC_DIR}/mdl_host_credentials.c
  ${MDL_JOF_SRC}
  ${MDL_FS_SRC}
  ${MDL_STREAM_CORE_SRC}
  ${MDL_STREAM_POSIX_SRC}
  ${MDL_MINIZ}
)

target_include_directories(
  media_dl
  PRIVATE ${MDL_FORM_INC}
          ${MDL_CORE_SRC_DIR}
          ${MDL_FORM_SRC_DIR}
          # Reuse the firmware error contract host-side. RA8_OFF_TARGET is the
          # same switch the host unit-test build uses so register-reaching code
          # takes the host-safe path.
          ${MDL_REPO_ROOT}/libs/ra8_core/inc
          ${MDL_REPO_ROOT}/libs/third_party/miniz
          ${MDL_JOF_INC}
          ${MDL_RABOOK_READER_INC}
          ${MDL_FS_INC}
)

target_compile_definitions(media_dl PRIVATE _GNU_SOURCE MINIZ_NO_TIME RA8_OFF_TARGET)
target_compile_options(media_dl PRIVATE -Wall -Wextra -Werror)
target_link_libraries(media_dl PRIVATE CURL::libcurl media_dl_core mdl_rabook_runtime)
ra8_webp_attach(media_dl ${MDL_REPO_ROOT})

# ---------------------------------------------------------------------------
# Tests OF THIS FORM. Everything that qualifies the downloader itself lives
# with the downloader, under apps/shared/media_dl/tests. What is left is what
# only exists because there is a CLI: the argv grammar, the libcurl backend,
# the host credential policy, and the end-to-end HTTP harness that runs the
# built executable.
# ---------------------------------------------------------------------------

# CLI grammar/dispatch qualification stays independent of exporter/network
# implementations so a broken mode matrix cannot be hidden by optional tools.
add_executable(
  test_media_dl_cli
  ${MDL_FORM_TESTS_DIR}/test_media_dl_cli.c
  ${MDL_FORM_TESTS_DIR}/test_media_dl_cli_matrix.c
  ${MDL_FORM_TESTS_DIR}/test_media_dl_logic_cli.c
  ${MDL_FORM_SRC_DIR}/mdl_cli.c
  ${MDL_FORM_SRC_DIR}/mdl_cli_nums.c
  ${MDL_FORM_SRC_DIR}/mdl_cli_stream.c
  ${MDL_FORM_SRC_DIR}/mdl_cli_usage.c
  ${MDL_STREAM_CORE_SRC}
  ${MDL_STREAM_RAM_SRC}
  ${FW}/ra8_core/src/ra8_log.c
  ${FW}/ra8_core/src/ra8_error_handler.c
)
target_include_directories(
  test_media_dl_cli
  PRIVATE ${MDL_FORM_INC}
          ${MDL_FORM_SRC_DIR}
          ${MDL_CORE_SRC_DIR}
          ${MDL_REPO_ROOT}/libs/ra8_core/inc
          ${MDL_REPO_ROOT}/libs/ra8_io/inc
          ${MDL_REPO_ROOT}/tests
)
target_compile_definitions(test_media_dl_cli PRIVATE _GNU_SOURCE RA8_OFF_TARGET)
target_compile_options(test_media_dl_cli PRIVATE -Wall -Wextra -Werror)
add_test(NAME test_media_dl_cli COMMAND test_media_dl_cli)

# #310 network-seam tests: a scripted fake ::mdl_net_iface_t backend drives the
# core's vtable dispatchers with no network, and the libcurl backend's pure
# transfer classifier + bounded write callback are exercised directly. This
# binary links the dispatchers, the politeness clock seam, the libcurl backend
# and its URL guards (so it needs libcurl); both src/ roots are on the include
# path for the promoted helpers in mdl_net_curl_internal.h.
add_executable(
  test_media_dl_net
  ${MDL_FORM_TESTS_DIR}/test_media_dl_net.c
  ${MDL_FORM_TESTS_DIR}/test_media_dl_net_curl.c
  ${MDL_CORE_SRC_DIR}/mdl_net.c
  ${MDL_FORM_SRC_DIR}/mdl_net_curl.c
  ${MDL_FORM_SRC_DIR}/mdl_net_curl_credentials.c
  ${MDL_CORE_SRC_DIR}/mdl_politeness.c
  ${MDL_CORE_SRC_DIR}/mdl_url_guard.c
  ${MDL_FS_SRC}
)
target_include_directories(
  test_media_dl_net
  PRIVATE ${MDL_FORM_INC}
          ${MDL_FORM_SRC_DIR}
          ${MDL_CORE_SRC_DIR}
          ${MDL_REPO_ROOT}/libs/ra8_core/inc
          ${MDL_FS_INC}
          ${MDL_REPO_ROOT}/tests
)
target_compile_definitions(test_media_dl_net PRIVATE _GNU_SOURCE RA8_OFF_TARGET)
target_compile_options(test_media_dl_net PRIVATE -Wall -Wextra -Werror)
target_link_libraries(test_media_dl_net PRIVATE CURL::libcurl)
add_test(NAME test_media_dl_net COMMAND test_media_dl_net)

# Host credential policy tests keep path opening at composition while proving
# strict cookie/CA byte validation and nanosecond mutation detection.
add_executable(
  test_media_dl_credentials
  ${MDL_FORM_TESTS_DIR}/test_media_dl_credentials.c
  ${MDL_CORE_SRC_DIR}/mdl_net.c
  ${MDL_FORM_SRC_DIR}/mdl_net_curl.c
  ${MDL_FORM_SRC_DIR}/mdl_net_curl_credentials.c
  ${MDL_FORM_SRC_DIR}/mdl_host_credentials.c
  ${MDL_CORE_SRC_DIR}/mdl_url_guard.c
)
target_include_directories(
  test_media_dl_credentials
  PRIVATE ${MDL_FORM_INC}
          ${MDL_FORM_SRC_DIR}
          ${MDL_CORE_SRC_DIR}
          ${MDL_REPO_ROOT}/libs/ra8_core/inc
          ${MDL_REPO_ROOT}/tests
)
target_compile_definitions(test_media_dl_credentials PRIVATE _GNU_SOURCE RA8_OFF_TARGET)
target_compile_options(test_media_dl_credentials PRIVATE -Wall -Wextra -Werror)
target_link_libraries(test_media_dl_credentials PRIVATE CURL::libcurl)
add_test(NAME test_media_dl_credentials COMMAND test_media_dl_credentials)

# Real socket/libcurl orchestration: deterministic local HTTP, no internet.
find_program(MDL_BASH_EXECUTABLE NAMES bash REQUIRED)
find_program(MDL_OPENSSL_EXECUTABLE NAMES openssl REQUIRED)
find_package(Python3 REQUIRED COMPONENTS Interpreter)
add_test(
  NAME test_media_dl_http_integration
  COMMAND ${MDL_BASH_EXECUTABLE} ${MDL_FORM_TESTS_DIR}/http_integration.sh $<TARGET_FILE:media_dl>
          ${Python3_EXECUTABLE} ${CMAKE_COMMAND} ${MDL_OPENSSL_EXECUTABLE}
)
set_tests_properties(
  test_media_dl_http_integration PROPERTIES TIMEOUT 30 LABELS "integration;network"
)
