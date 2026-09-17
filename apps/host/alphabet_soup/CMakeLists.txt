# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie

cmake_minimum_required(VERSION 3.20)
project(alphabet_soup C)

set(CMAKE_C_STANDARD 23)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(ALPHABET_SOUP_REPO_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../../..)
set(FW ${ALPHABET_SOUP_REPO_ROOT}/libs)

set(ALPHABET_SOUP_PORT_SRC
    ${FW}/if/src/fw_if_fs.c
    ${FW}/if/src/fw_if_fs_dir.c
    ${FW}/ra8_io/src/ra8_io_stream.c
    ${FW}/ra8_io/src/ra8_io_stream_ram.c
    ${FW}/ra8_core/src/ra8_log.c
    ${ALPHABET_SOUP_REPO_ROOT}/port/posix/src/fw_if_fs_posix.c
    ${ALPHABET_SOUP_REPO_ROOT}/port/posix/src/fw_if_fs_posix_bind.c
    ${ALPHABET_SOUP_REPO_ROOT}/port/posix/src/fw_if_fs_posix_common.c
    ${ALPHABET_SOUP_REPO_ROOT}/port/posix/src/fw_if_fs_posix_stream.c
    ${ALPHABET_SOUP_REPO_ROOT}/port/posix/src/ra8_io_stream_posix.c
)

# ---------------------------------------------------------------------------
# Application Executable
# ---------------------------------------------------------------------------
add_executable(
  alphabet_soup
  ${CMAKE_CURRENT_SOURCE_DIR}/src/main.c ${CMAKE_CURRENT_SOURCE_DIR}/src/alphabet_soup.c
  ${CMAKE_CURRENT_SOURCE_DIR}/src/alphabet_soup_cli.c ${ALPHABET_SOUP_PORT_SRC}
)

target_include_directories(
  alphabet_soup
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/inc
          ${CMAKE_CURRENT_SOURCE_DIR}/src
          ${FW}/if/inc
          ${FW}/ra8_core/inc
          ${FW}/ra8_io/inc
          ${ALPHABET_SOUP_REPO_ROOT}/port/posix/inc
          ${ALPHABET_SOUP_REPO_ROOT}/port/posix/src
)

target_compile_definitions(alphabet_soup PRIVATE RA8_OFF_TARGET)

target_compile_options(
  alphabet_soup
  PRIVATE -Wall
          -Wextra
          -Werror
          -Wformat=2
          -Wconversion
          -Wsign-conversion
          -Wstrict-prototypes
          -Wmissing-prototypes
)

# ---------------------------------------------------------------------------
# Unit Tests
# ---------------------------------------------------------------------------
enable_testing()

add_executable(
  test_alphabet_soup
  ${CMAKE_CURRENT_SOURCE_DIR}/tests/src/test_alphabet_soup.c
  ${CMAKE_CURRENT_SOURCE_DIR}/src/alphabet_soup.c
  ${CMAKE_CURRENT_SOURCE_DIR}/src/alphabet_soup_cli.c ${ALPHABET_SOUP_PORT_SRC}
)

target_include_directories(
  test_alphabet_soup
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/inc
          ${CMAKE_CURRENT_SOURCE_DIR}/src
          ${FW}/if/inc
          ${FW}/ra8_core/inc
          ${FW}/ra8_io/inc
          ${ALPHABET_SOUP_REPO_ROOT}/port/posix/inc
          ${ALPHABET_SOUP_REPO_ROOT}/port/posix/src
          ${ALPHABET_SOUP_REPO_ROOT}/tests/support/inc
)

target_compile_definitions(test_alphabet_soup PRIVATE RA8_OFF_TARGET)

target_compile_options(
  test_alphabet_soup
  PRIVATE -Wall
          -Wextra
          -Werror
          -Wformat=2
          -Wconversion
          -Wsign-conversion
          -Wstrict-prototypes
          -Wmissing-prototypes
)

add_test(NAME test_alphabet_soup COMMAND test_alphabet_soup)

# Exercise the hosted wrapper as well as the solver library. These invocations
# deliberately cover every externally selectable path family: absolute and
# relative success, argument/open/parse failures, and the bounded-file refusal.
add_test(NAME test_alphabet_soup_cli_absolute
         COMMAND alphabet_soup ${CMAKE_CURRENT_SOURCE_DIR}/resources/sample_5x5.txt
)
add_test(NAME test_alphabet_soup_cli_relative COMMAND alphabet_soup sample_3x3.txt)
set_tests_properties(
  test_alphabet_soup_cli_relative PROPERTIES WORKING_DIRECTORY
                                             ${CMAKE_CURRENT_SOURCE_DIR}/resources
)

add_test(NAME test_alphabet_soup_cli_usage COMMAND alphabet_soup)
add_test(NAME test_alphabet_soup_cli_empty_path COMMAND alphabet_soup "")
add_test(NAME test_alphabet_soup_cli_missing COMMAND alphabet_soup does-not-exist.txt)
add_test(NAME test_alphabet_soup_cli_root COMMAND alphabet_soup /)
add_test(NAME test_alphabet_soup_cli_empty_file COMMAND alphabet_soup /dev/null)
string(REPEAT "A" 65535 _alphabet_soup_oversize_fixture)
file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/alphabet_soup_oversize.txt
     "${_alphabet_soup_oversize_fixture}"
)
unset(_alphabet_soup_oversize_fixture)
add_test(NAME test_alphabet_soup_cli_oversize
         COMMAND alphabet_soup ${CMAKE_CURRENT_BINARY_DIR}/alphabet_soup_oversize.txt
)
add_test(NAME test_alphabet_soup_cli_malformed
         COMMAND alphabet_soup ${CMAKE_CURRENT_SOURCE_DIR}/resources/malformed.txt
)
string(REPEAT "a" 512 _alphabet_soup_long_path)
add_test(NAME test_alphabet_soup_cli_long_path COMMAND alphabet_soup ${_alphabet_soup_long_path})
unset(_alphabet_soup_long_path)
add_test(NAME test_alphabet_soup_cli_missing_parent COMMAND alphabet_soup
                                                            /ra8-alphabet-soup-missing/file.txt
)
add_test(NAME test_alphabet_soup_cli_read_error COMMAND alphabet_soup /proc/self/mem)
add_test(NAME test_alphabet_soup_cli_closed_stdout
         COMMAND /bin/sh -c "exec 1>&-; '$<TARGET_FILE:alphabet_soup>'"
)
# These are negative CLI-contract cases: every listed input must make the
# program reject the request with a non-zero status. CTest still treats a
# fatal process exception as a test failure, so WILL_FAIL does not turn a
# crash into an accepted rejection.
set_tests_properties(
  test_alphabet_soup_cli_usage
  test_alphabet_soup_cli_empty_path
  test_alphabet_soup_cli_missing
  test_alphabet_soup_cli_root
  test_alphabet_soup_cli_empty_file
  test_alphabet_soup_cli_oversize
  test_alphabet_soup_cli_malformed
  test_alphabet_soup_cli_long_path
  test_alphabet_soup_cli_missing_parent
  PROPERTIES WILL_FAIL TRUE
)
# The read-error and closed-stdout cases exercise the same negative contract,
# but are split from the portable inputs because their registration is kept on
# non-Linux hosts while execution is disabled below.
set_tests_properties(
  test_alphabet_soup_cli_read_error test_alphabet_soup_cli_closed_stdout PROPERTIES WILL_FAIL TRUE
)

# Keep one structural test inventory on every host while acknowledging that
# `/proc/self/mem` and the closed-descriptor shell behavior are Linux-specific.
# Qualification claims macOS registration parity, not execution of these two
# cases, so non-Linux configurations retain their names as disabled entries.
if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
  # `/proc/self/mem` and the closed-descriptor shell behavior are Linux-only;
  # retaining disabled registrations proves inventory parity without claiming
  # those platform-specific paths executed elsewhere.
  set_tests_properties(
    test_alphabet_soup_cli_read_error test_alphabet_soup_cli_closed_stdout PROPERTIES DISABLED TRUE
  )
endif()

# Guard both halves of that contract at configure time: neither test may fall
# out of the inventory, and only Linux may schedule it for execution.
get_property(
  _alphabet_soup_registered_tests
  DIRECTORY
  PROPERTY TESTS
)
foreach(_alphabet_soup_linux_test IN ITEMS test_alphabet_soup_cli_read_error
                                           test_alphabet_soup_cli_closed_stdout
)
  if(NOT _alphabet_soup_linux_test IN_LIST _alphabet_soup_registered_tests)
    message(FATAL_ERROR "${_alphabet_soup_linux_test} is missing from the host test inventory")
  endif()
  get_property(
    _alphabet_soup_linux_test_disabled
    TEST ${_alphabet_soup_linux_test}
    PROPERTY DISABLED
  )
  if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND _alphabet_soup_linux_test_disabled)
    message(FATAL_ERROR "${_alphabet_soup_linux_test} must execute on Linux")
  endif()
  if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux" AND NOT _alphabet_soup_linux_test_disabled)
    message(FATAL_ERROR "${_alphabet_soup_linux_test} must be disabled off Linux")
  endif()
endforeach()
unset(_alphabet_soup_linux_test_disabled)
unset(_alphabet_soup_linux_test)
unset(_alphabet_soup_registered_tests)
