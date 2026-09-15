# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie

function(
  run_case
  name
  expected_result
  expected_stdout
  expected_stderr
)
  execute_process(
    COMMAND "${EXECUTABLE}" ${ARGN}
    RESULT_VARIABLE result
    OUTPUT_VARIABLE stdout
    ERROR_VARIABLE stderr
  )
  if(NOT "${result}" STREQUAL "${expected_result}")
    message(FATAL_ERROR "${name}: exit ${result}; expected ${expected_result}")
  endif()
  if(NOT "${stdout}" STREQUAL "${expected_stdout}")
    message(FATAL_ERROR "${name}: stdout [${stdout}]; expected [${expected_stdout}]")
  endif()
  if(NOT "${stderr}" STREQUAL "${expected_stderr}")
    message(FATAL_ERROR "${name}: stderr [${stderr}]; expected [${expected_stderr}]")
  endif()
endfunction()

run_case(
  success
  0
  "bytes=5\nzero=0\nerased=0\nfnv1a64=a430d84680aabd0b\nzig_xor8=62\nzig_stage=5a\n"
  ""
  "${FIXTURE}"
)
run_case(usage 2 "" "usage: firmware_pipeline <firmware-image>\n")
run_case(
  empty
  2
  ""
  "firmware_pipeline: Rust rejected empty image\n"
  "${EMPTY_FIXTURE}"
)
run_case(
  missing
  2
  ""
  "firmware_pipeline: cannot read bounded input\n"
  "${FIXTURE}.missing"
)
