# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
#
# CTest helper for negative C ABI fixtures. Its messages are deliberately
# stable so a missing layout assertion or missing-link failure is actionable.

foreach(
  _ra8_required IN
  ITEMS RA8_C_COMPILER
        RA8_INCLUDE_DIR
        RA8_CORE_INCLUDE_DIR
        RA8_SOURCE
        RA8_KIND
)
  if(NOT DEFINED ${_ra8_required})
    message(FATAL_ERROR "ra8 ABI negative fixture: missing ${_ra8_required}")
  endif()
endforeach()

set(_ra8_output "${CMAKE_CURRENT_BINARY_DIR}/ra8_abi_negative_${RA8_KIND}")
set(_ra8_command "${RA8_C_COMPILER}" -std=gnu2x -I "${RA8_INCLUDE_DIR}" -I
                 "${RA8_CORE_INCLUDE_DIR}" "${RA8_SOURCE}" -o "${_ra8_output}"
)

if(RA8_KIND STREQUAL "layout")
  list(APPEND _ra8_command -c)
  set(_ra8_expected "ABI contract fixture deliberately requires an incompatible layout")
elseif(RA8_KIND STREQUAL "missing_symbol")
  if(NOT EXISTS "${RA8_LIBRARY}")
    message(FATAL_ERROR "ra8 ABI negative missing_symbol: Zig library was not built")
  endif()
  list(APPEND _ra8_command "${RA8_LIBRARY}")
  set(_ra8_expected "ra8_abi_fixture_missing")
else()
  message(FATAL_ERROR "ra8 ABI negative fixture: unsupported kind ${RA8_KIND}")
endif()

execute_process(
  COMMAND ${_ra8_command}
  RESULT_VARIABLE _ra8_result
  OUTPUT_VARIABLE _ra8_stdout
  ERROR_VARIABLE _ra8_stderr
)

if(_ra8_result EQUAL 0)
  message(FATAL_ERROR "ra8 ABI negative ${RA8_KIND}: expected failure but compiler succeeded")
endif()
if(NOT "${_ra8_stderr}" MATCHES "${_ra8_expected}")
  message(FATAL_ERROR "ra8 ABI negative ${RA8_KIND}: failure omitted expected diagnostic "
                      "'${_ra8_expected}':\n${_ra8_stderr}"
  )
endif()

message(STATUS "ra8 ABI negative ${RA8_KIND}: expected failure observed")
