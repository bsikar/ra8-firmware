# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie

# mdl -- the STAND-ALONE HOST FORM of the comic/manga/manhwa/webtoon
# downloader: a native DEV MACHINE executable (x86_64 / arm64), NOT an ARM ELF.
#
# The downloader itself is not here. It lives once, at apps/shared_libs/mdl,
# because the same core is about to have a second build form (a loadable
# on-device module under apps/board/threadx_modules/) and a core owned by one
# form is a core the other form has to copy. What IS here is the composition root:
#
#   src/main.c                 process entry, signals, POSIX stream binding,
#                              host path canonicalisation and credential I/O
#   src/mdl_cli*.c             the argv grammar and its diagnostics
#   src/mdl_net_curl*.c        the libcurl backend, injected through the
#                              core's ::mdl_net_iface_t vtable
#   src/mdl_host_credentials.c host cookie-jar / CA-bundle policy
#   src/mdl_compose.c          the composition itself: libcurl bound into the
#                              core's ::mdl_net_provider_t seam, and one
#                              parsed invocation routed to one mdl_app.h entry
#
# The per-mode ACTIONS are deliberately not here. They were, and their shared
# header reached for dirent.h, unistd.h and the libcurl factory, which pinned
# every one of them to this form. Inverting those three dependencies (#725)
# left nothing form-specific in them, so they live with the downloader.
#
# The layering is one-way and is a directory rule: apps/shared_libs MUST NEVER
# include from apps/host or apps/board -- shared libraries are BELOW the forms.
# This listfile therefore names apps/shared_libs; the shared listfile names no
# form.
cmake_minimum_required(VERSION 3.20)
project(mdl C)

set(CMAKE_C_STANDARD 23)
set(CMAKE_C_STANDARD_REQUIRED ON)

# The repository root, stated ONCE. This listfile reaches out of its own tree
# for the shared core, the firmware libraries, the POSIX port, the shared test
# support and the vendored WebP recipe, and it is configured standalone (its
# own CMAKE_SOURCE_DIR) by the tools-build and coverage-tree gates, so
# CMAKE_SOURCE_DIR cannot serve. Every such reference goes through this
# variable: when the product moved from tools/mdl to
# apps/host/mdl, 59 hand-written ../.. prefixes all became wrong
# together and CMake reported only the first.
set(MDL_REPO_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../../..)

# Keep host-tool coverage opt-in, matching tests/cmake/host_config.cmake. The
# flags are added after project() so CMake's compiler probe is not instrumented
# and gcovr never mistakes compiler-identification objects for project code.
# Added here, at the top-level directory scope, so the shared core added below
# inherits them: the coverage-tree ratchet measures core files, and a core
# built without --coverage would report zero instrumented lines for all of them.
option(RA8_COVERAGE "Emit gcov coverage data for mdl and its tests" OFF)
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

# The shared downloader core: its library target (mdl_core), the
# EPUB/RABOOK runtime it needs, its own 17 test binaries, and the source-list
# and include-root variables this form composes with. add_subdirectory rather
# than include() because the core is a project in its own right -- it configures
# and tests standalone -- and its targets must be real targets this form links,
# not source lists it re-compiles. It publishes what a form needs through
# PARENT_SCOPE, which is why the composition below never spells a core path by
# hand.
add_subdirectory(${MDL_REPO_ROOT}/apps/shared_libs/mdl ${CMAKE_CURRENT_BINARY_DIR}/mdl_core)

# Per-source properties are DIRECTORY-scoped, so the SOUP flags the core sets on
# miniz do not follow the file into this listfile. Every target below that
# compiles miniz needs them restated here -- without them the vendored decoder
# meets -Wall -Wextra -Werror and MINIZ_NO_MALLOC silently turns back on.
#
# Measured one flag at a time on miniz.c in THIS form's own target under
# gcc 14.2.0 and clang 18 at the -O0 this build uses: -Wunused-parameter is the
# only class that fires. This form's profile is -Wall -Wextra -Werror, which
# enables neither -Wconversion nor -Wcast-qual, so -Wno-sign-conversion,
# -Wno-conversion, -Wno-implicit-int-conversion and -Wno-cast-qual masked
# nothing; the blanket -Wno-error masked nothing either, because with those
# classes off there was no remaining diagnostic for it to demote. All five are
# deleted.
set(_ra8_wno_1 -Wno-unused-parameter # miniz.c mz_zip_writer_add_mem_ex(last_modified)
               -fno-strict-aliasing
)
set_source_files_properties(
  ${MDL_MINIZ} PROPERTIES COMPILE_OPTIONS "${_ra8_wno_1}" COMPILE_DEFINITIONS
                                                          "MINIZ_NO_STDIO;MINIZ_NO_MALLOC"
)

# The include roots this form's own translation units need: the core's public
# contract, plus its own inc/ for the two headers only the composition root
# publishes (mdl_cli.h, mdl_net_curl.h).
set(MDL_FORM_INC ${MDL_PUBLIC_INC} ${CMAKE_CURRENT_SOURCE_DIR}/inc)
set(MDL_FORM_SRC_DIR ${CMAKE_CURRENT_SOURCE_DIR}/src)
set(MDL_FORM_TESTS_DIR ${CMAKE_CURRENT_SOURCE_DIR}/tests)
set(MDL_FORM_TEST_SRC_DIR ${MDL_FORM_TESTS_DIR}/src)
set(MDL_FORM_TEST_INC_DIR ${MDL_FORM_TESTS_DIR}/inc)

add_executable(
  mdl
  ${MDL_FORM_SRC_DIR}/main.c
  ${MDL_FORM_SRC_DIR}/mdl_compose.c
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
  mdl
  PRIVATE ${MDL_FORM_INC}
          ${MDL_CORE_SRC_DIR}
          ${MDL_FORM_SRC_DIR}
          # Reuse the firmware error contract host-side. RA8_OFF_TARGET is the
          # same switch the host unit-test build uses so register-reaching code
          # takes the host-safe path.
          ${MDL_REPO_ROOT}/libs/ra8_core/inc
          ${MDL_REPO_ROOT}/apps/shared_libs/third_party/miniz
          ${MDL_JOF_INC}
          ${MDL_RABOOK_READER_INC}
          ${MDL_FS_INC}
)

target_compile_definitions(mdl PRIVATE _GNU_SOURCE MINIZ_NO_TIME RA8_OFF_TARGET)
target_compile_options(mdl PRIVATE -Wall -Wextra -Werror)
target_link_libraries(mdl PRIVATE CURL::libcurl mdl_core mdl_rabook_runtime)
ra8_webp_attach(mdl ${MDL_REPO_ROOT})

# ---------------------------------------------------------------------------
# Tests OF THIS FORM. Everything that qualifies the downloader itself lives
# with the downloader, under apps/shared_libs/mdl/tests. What is left is what
# only exists because there is a CLI: the argv grammar, the libcurl backend,
# the host credential policy, and the end-to-end HTTP harness that runs the
# built executable.
# ---------------------------------------------------------------------------

# CLI grammar/dispatch qualification stays independent of exporter/network
# implementations so a broken mode matrix cannot be hidden by optional tools.
add_executable(
  test_mdl_cli
  ${MDL_FORM_TEST_SRC_DIR}/test_mdl_cli.c
  ${MDL_FORM_TEST_SRC_DIR}/test_mdl_cli_matrix.c
  ${MDL_FORM_TEST_SRC_DIR}/test_mdl_logic_cli.c
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
  test_mdl_cli
  PRIVATE ${MDL_FORM_INC}
          ${MDL_FORM_TEST_INC_DIR}
          ${MDL_FORM_SRC_DIR}
          ${MDL_CORE_SRC_DIR}
          ${MDL_REPO_ROOT}/libs/ra8_core/inc
          ${MDL_REPO_ROOT}/libs/ra8_io/inc
          ${MDL_REPO_ROOT}/tests/support/inc
          # mdl_cli.h folds argv into the portable ::mdl_run_opts_t, so the
          # grammar now sees the application contract and the fw_if_fs types it
          # names. Header reach only: this target links no filesystem unit.
          ${MDL_FS_INC}
)
target_compile_definitions(test_mdl_cli PRIVATE _GNU_SOURCE RA8_OFF_TARGET)
target_compile_options(test_mdl_cli PRIVATE -Wall -Wextra -Werror)
add_test(NAME test_mdl_cli COMMAND test_mdl_cli)

# #310 network-seam tests: a scripted fake ::mdl_net_iface_t backend drives the
# core's vtable dispatchers with no network, and the libcurl backend's pure
# transfer classifier + bounded write callback are exercised directly. This
# binary links the dispatchers, the politeness clock seam, the libcurl backend
# and its URL guards (so it needs libcurl); both src/ roots are on the include
# path for the promoted helpers in mdl_net_curl_internal.h.
add_executable(
  test_mdl_net
  ${MDL_FORM_TEST_SRC_DIR}/test_mdl_net.c
  ${MDL_FORM_TEST_SRC_DIR}/test_mdl_net_curl.c
  ${MDL_CORE_SRC_DIR}/mdl_net.c
  ${MDL_FORM_SRC_DIR}/mdl_net_curl.c
  ${MDL_FORM_SRC_DIR}/mdl_net_curl_credentials.c
  ${MDL_CORE_SRC_DIR}/mdl_politeness.c
  ${MDL_CORE_SRC_DIR}/mdl_url_guard.c
  ${MDL_FS_SRC}
)
target_include_directories(
  test_mdl_net
  PRIVATE ${MDL_FORM_INC}
          ${MDL_FORM_TEST_INC_DIR}
          ${MDL_FORM_SRC_DIR}
          ${MDL_CORE_SRC_DIR}
          ${MDL_REPO_ROOT}/libs/ra8_core/inc
          ${MDL_FS_INC}
          ${MDL_REPO_ROOT}/tests/support/inc
)
target_compile_definitions(test_mdl_net PRIVATE _GNU_SOURCE RA8_OFF_TARGET)
target_compile_options(test_mdl_net PRIVATE -Wall -Wextra -Werror)
target_link_libraries(test_mdl_net PRIVATE CURL::libcurl)
add_test(NAME test_mdl_net COMMAND test_mdl_net)

# Host credential policy tests keep path opening at composition while proving
# strict cookie/CA byte validation and nanosecond mutation detection.
add_executable(
  test_mdl_credentials
  ${MDL_FORM_TEST_SRC_DIR}/test_mdl_credentials.c
  ${MDL_CORE_SRC_DIR}/mdl_net.c
  ${MDL_FORM_SRC_DIR}/mdl_net_curl.c
  ${MDL_FORM_SRC_DIR}/mdl_net_curl_credentials.c
  ${MDL_FORM_SRC_DIR}/mdl_host_credentials.c
  ${MDL_CORE_SRC_DIR}/mdl_url_guard.c
)
target_include_directories(
  test_mdl_credentials
  PRIVATE ${MDL_FORM_INC}
          ${MDL_FORM_TEST_INC_DIR}
          ${MDL_FORM_SRC_DIR}
          ${MDL_CORE_SRC_DIR}
          ${MDL_REPO_ROOT}/libs/ra8_core/inc
          ${MDL_REPO_ROOT}/tests/support/inc
)
target_compile_definitions(test_mdl_credentials PRIVATE _GNU_SOURCE RA8_OFF_TARGET)
target_compile_options(test_mdl_credentials PRIVATE -Wall -Wextra -Werror)
target_link_libraries(test_mdl_credentials PRIVATE CURL::libcurl)
add_test(NAME test_mdl_credentials COMMAND test_mdl_credentials)

# Real socket/libcurl orchestration: deterministic local HTTP, no internet.
find_program(MDL_BASH_EXECUTABLE NAMES bash REQUIRED)
find_program(MDL_OPENSSL_EXECUTABLE NAMES openssl REQUIRED)
find_package(Python3 REQUIRED COMPONENTS Interpreter)
add_test(
  NAME test_mdl_http_integration
  COMMAND ${MDL_BASH_EXECUTABLE} ${MDL_FORM_TESTS_DIR}/http_integration.sh $<TARGET_FILE:mdl>
          ${Python3_EXECUTABLE} ${CMAKE_COMMAND} ${MDL_OPENSSL_EXECUTABLE}
)
set_tests_properties(test_mdl_http_integration PROPERTIES TIMEOUT 30 LABELS "integration;network")
