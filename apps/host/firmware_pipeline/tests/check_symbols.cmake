# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie

# Compare one archive's complete public symbol set with its contract.
function(scan_archive archive expected)
  execute_process(
    COMMAND "${NM}" -g --defined-only "${archive}"
    RESULT_VARIABLE result
    OUTPUT_VARIABLE symbols
    ERROR_VARIABLE error
  )
  if(NOT result EQUAL 0)
    message(FATAL_ERROR "symbol scan failed for ${archive}: ${error}")
  endif()
  string(REGEX MATCHALL "[ \t]firmware_pipeline_[A-Za-z0-9_]+" matches "${symbols}")
  set(actual)
  foreach(match IN LISTS matches)
    string(STRIP "${match}" name)
    list(APPEND actual "${name}")
  endforeach()
  list(REMOVE_DUPLICATES actual)
  list(SORT actual)
  if(NOT "${actual}" STREQUAL "${expected}")
    message(FATAL_ERROR "${archive} exports ${actual}; expected ${expected}")
  endif()
endfunction()

scan_archive("${ZIG_LIBRARY}" "firmware_pipeline_analyze")
scan_archive("${RUST_LIBRARY}" "firmware_pipeline_rust_analyze")

file(READ "${PUBLIC_HEADER}" PUBLIC_HEADER_TEXT)
file(READ "${PRIVATE_HEADER}" PRIVATE_HEADER_TEXT)
file(READ "${ZIG_SOURCE}" ZIG_SOURCE_TEXT)
file(READ "${RUST_SOURCE}" RUST_SOURCE_TEXT)
string(REGEX REPLACE "[ \t\r\n]" "" PUBLIC_HEADER_TEXT "${PUBLIC_HEADER_TEXT}")
string(REGEX REPLACE "[ \t\r\n]" "" PRIVATE_HEADER_TEXT "${PRIVATE_HEADER_TEXT}")
string(REGEX REPLACE "[ \t\r\n]" "" ZIG_SOURCE_TEXT "${ZIG_SOURCE_TEXT}")
string(REGEX REPLACE "[ \t\r\n]" "" RUST_SOURCE_TEXT "${RUST_SOURCE_TEXT}")

string(CONCAT PUBLIC_SIGNATURE "firmware_pipeline_status_tfirmware_pipeline_analyze("
              "constfirmware_pipeline_config_t*config,constuint8_t*data,"
              "size_tsize,firmware_pipeline_result_t*out_result)"
)
string(CONCAT PRIVATE_SIGNATURE
              "int32_tfirmware_pipeline_rust_analyze(constuint8_t*data,size_tsize,"
              "firmware_pipeline_rust_summary_t*out_summary)"
)
string(
  CONCAT ZIG_SIGNATURE
         "pubexportfnfirmware_pipeline_analyze("
         "config:?*constc.firmware_pipeline_config_t,data:?[*]constu8,size:usize,"
         "out_result:?*c.firmware_pipeline_result_t,)"
         "callconv(.c)c.firmware_pipeline_status_t"
)
string(CONCAT RUST_SIGNATURE "const_:unsafeextern\"C\"fn(*constu8,usize,*mutRustSummary)"
              "->PipelineStatus=firmware_pipeline_rust_analyze"
)
foreach(PAIR IN
        ITEMS "PUBLIC_HEADER_TEXT;${PUBLIC_SIGNATURE}" "PRIVATE_HEADER_TEXT;${PRIVATE_SIGNATURE}"
              "ZIG_SOURCE_TEXT;${ZIG_SIGNATURE}" "RUST_SOURCE_TEXT;${RUST_SIGNATURE}"
)
  list(GET PAIR 0 VARIABLE)
  list(GET PAIR 1 FRAGMENT)
  string(FIND "${${VARIABLE}}" "${FRAGMENT}" POSITION)
  if(POSITION EQUAL -1)
    message(FATAL_ERROR "${VARIABLE} is missing ABI signature ${FRAGMENT}")
  endif()
endforeach()

foreach(FRAGMENT IN ITEMS "sizeof(firmware_pipeline_result_t)==40U"
                          "offsetof(firmware_pipeline_result_t,zig_xor8)==32U"
)
  string(FIND "${PUBLIC_HEADER_TEXT}" "${FRAGMENT}" POSITION)
  if(POSITION EQUAL -1)
    message(FATAL_ERROR "public header is missing layout contract ${FRAGMENT}")
  endif()
endforeach()
foreach(
  FRAGMENT IN
  ITEMS "sizeof(firmware_pipeline_rust_summary_t)==32U"
        "offsetof(firmware_pipeline_rust_summary_t,fnv1a64)==k_firmware_pipeline_rust_digest_offset"
)
  string(FIND "${PRIVATE_HEADER_TEXT}" "${FRAGMENT}" POSITION)
  if(POSITION EQUAL -1)
    message(FATAL_ERROR "private header is missing layout contract ${FRAGMENT}")
  endif()
endforeach()
