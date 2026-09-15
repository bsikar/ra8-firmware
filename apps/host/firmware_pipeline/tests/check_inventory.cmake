# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie

set(expected
    test_firmware_pipeline_abi
    test_firmware_pipeline_cli
    test_firmware_pipeline_command
    test_firmware_pipeline_inventory_selftest
    test_firmware_pipeline_rust
    test_firmware_pipeline_symbols
    test_firmware_pipeline_symbols_selftest
    test_firmware_pipeline_zig
)
list(SORT expected)
set(actual ${ACTUAL_TESTS})
list(SORT actual)
if(NOT "${actual}" STREQUAL "${expected}")
  message(FATAL_ERROR "firmware_pipeline tests ${actual}; required ${expected}")
endif()
