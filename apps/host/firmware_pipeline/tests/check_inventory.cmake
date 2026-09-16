# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie

set(EXPECTED
    test_firmware_pipeline_abi
    test_firmware_pipeline_cli
    test_firmware_pipeline_command
    test_firmware_pipeline_inventory_selftest
    test_firmware_pipeline_rust
    test_firmware_pipeline_symbols
    test_firmware_pipeline_symbols_selftest
    test_firmware_pipeline_zig
)
list(SORT EXPECTED)
set(ACTUAL ${ACTUAL_TESTS})
list(SORT ACTUAL)
if(NOT "${ACTUAL}" STREQUAL "${EXPECTED}")
  message(FATAL_ERROR "firmware_pipeline tests ${ACTUAL}; required ${EXPECTED}")
endif()
