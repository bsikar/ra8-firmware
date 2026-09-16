# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie

set(EXPECTED
    test_firmware_report_abi
    test_firmware_report_cli
    test_firmware_report_command
    test_firmware_report_inventory_selftest
    test_firmware_report_rust
    test_firmware_report_symbols
    test_firmware_report_symbols_selftest
)
list(SORT EXPECTED)
set(ACTUAL ${ACTUAL_TESTS})
list(SORT ACTUAL)
if(NOT "${ACTUAL}" STREQUAL "${EXPECTED}")
  message(FATAL_ERROR "firmware_report CTest inventory ${ACTUAL}; required ${EXPECTED}")
endif()
