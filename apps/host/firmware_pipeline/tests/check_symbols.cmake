# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie

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

file(READ "${PUBLIC_HEADER}" public_header)
file(READ "${PRIVATE_HEADER}" private_header)
file(READ "${ZIG_SOURCE}" zig_source)
file(READ "${RUST_SOURCE}" rust_source)
string(REGEX REPLACE "[ \t\r\n]" "" public_header "${public_header}")
string(REGEX REPLACE "[ \t\r\n]" "" private_header "${private_header}")
string(REGEX REPLACE "[ \t\r\n]" "" zig_source "${zig_source}")
string(REGEX REPLACE "[ \t\r\n]" "" rust_source "${rust_source}")

set(public_signature "firmware_pipeline_status_tfirmware_pipeline_analyze(constfirmware_pipeline_config_t*config,constuint8_t*data,size_tsize,firmware_pipeline_result_t*out_result)")
set(private_signature "int32_tfirmware_pipeline_rust_analyze(constuint8_t*data,size_tsize,firmware_pipeline_rust_summary_t*out_summary)")
set(zig_signature "pubexportfnfirmware_pipeline_analyze(config:?*constc.firmware_pipeline_config_t,data:?[*]constu8,size:usize,out_result:?*c.firmware_pipeline_result_t,)callconv(.c)c.firmware_pipeline_status_t")
set(rust_signature "const_:unsafeextern\"C\"fn(*constu8,usize,*mutRustSummary)->PipelineStatus=firmware_pipeline_rust_analyze")
foreach(pair IN ITEMS "public_header;${public_signature}" "private_header;${private_signature}" "zig_source;${zig_signature}" "rust_source;${rust_signature}")
  list(GET pair 0 variable)
  list(GET pair 1 fragment)
  string(FIND "${${variable}}" "${fragment}" position)
  if(position EQUAL -1)
    message(FATAL_ERROR "${variable} is missing ABI signature ${fragment}")
  endif()
endforeach()

foreach(fragment IN ITEMS "sizeof(firmware_pipeline_result_t)==40U" "offsetof(firmware_pipeline_result_t,zig_xor8)==32U")
  string(FIND "${public_header}" "${fragment}" position)
  if(position EQUAL -1)
    message(FATAL_ERROR "public header is missing layout contract ${fragment}")
  endif()
endforeach()
foreach(fragment IN ITEMS "sizeof(firmware_pipeline_rust_summary_t)==32U" "offsetof(firmware_pipeline_rust_summary_t,fnv1a64)==24U")
  string(FIND "${private_header}" "${fragment}" position)
  if(position EQUAL -1)
    message(FATAL_ERROR "private header is missing layout contract ${fragment}")
  endif()
endforeach()
