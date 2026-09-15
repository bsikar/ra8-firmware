# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie

execute_process(
  COMMAND "${RUST_EXECUTABLE}" "${FIXTURE}"
  RESULT_VARIABLE rust_result
  OUTPUT_VARIABLE rust_stdout
  ERROR_VARIABLE rust_stderr
)
execute_process(
  COMMAND "${ZIG_EXECUTABLE}" "${FIXTURE}"
  RESULT_VARIABLE zig_result
  OUTPUT_VARIABLE zig_stdout
  ERROR_VARIABLE zig_stderr
)
if(NOT rust_result EQUAL 0 OR NOT zig_result EQUAL 0)
  message(FATAL_ERROR "main parity execution failed: Rust=${rust_result}, Zig=${zig_result}")
endif()
if(NOT "${rust_stdout}" STREQUAL "${zig_stdout}")
  message(FATAL_ERROR "main stdout differs: Rust=[${rust_stdout}], Zig=[${zig_stdout}]")
endif()
if(NOT "${rust_stderr}" STREQUAL "${zig_stderr}")
  message(FATAL_ERROR "main stderr differs: Rust=[${rust_stderr}], Zig=[${zig_stderr}]")
endif()
