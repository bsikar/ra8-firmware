# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie

execute_process(
  COMMAND "${NM}" -g --defined-only "${LIBRARY}"
  RESULT_VARIABLE result
  OUTPUT_VARIABLE symbols
  ERROR_VARIABLE error
)
if(NOT result EQUAL 0)
  message(FATAL_ERROR "firmware_report symbol scan failed: ${error}")
endif()

set(expected firmware_report_create firmware_report_query firmware_report_release)
list(SORT expected)

string(REGEX MATCHALL "[ \t]firmware_report_[A-Za-z0-9_]+" archive_matches "${symbols}")
set(archive_exports)
foreach(match IN LISTS archive_matches)
  string(STRIP "${match}" name)
  list(APPEND archive_exports "${name}")
endforeach()
list(REMOVE_DUPLICATES archive_exports)
list(SORT archive_exports)
if(NOT "${archive_exports}" STREQUAL "${expected}")
  message(FATAL_ERROR "firmware_report archive exports ${archive_exports}; expected ${expected}")
endif()

file(READ "${HEADER}" header)
string(REGEX REPLACE "[ \t\r\n]" "" normalized_header "${header}")
set(
  required_header_contract
  "firmware_report_status_tfirmware_report_create(constuint8_t*data,size_tsize,firmware_report_handle_t**out_handle)"
  "firmware_report_status_tfirmware_report_query(constfirmware_report_handle_t*handle,firmware_report_summary_t*out_summary)"
  "firmware_report_status_tfirmware_report_release(firmware_report_handle_t**in_out_handle)"
  "static_assert(sizeof(firmware_report_status_t)==4U"
  "static_assert(sizeof(firmware_report_summary_t)==32U"
  "static_assert(alignof(firmware_report_summary_t)==8U"
)
foreach(fragment IN LISTS required_header_contract)
  string(FIND "${normalized_header}" "${fragment}" position)
  if(position EQUAL -1)
    message(FATAL_ERROR "firmware_report header is missing ABI contract fragment: ${fragment}")
  endif()
endforeach()
string(REGEX MATCHALL "firmware_report_[A-Za-z0-9_]+[ \t\r\n]*\\(" header_matches "${header}")
set(header_exports)
foreach(match IN LISTS header_matches)
  string(REGEX REPLACE "[ \t\r\n]*\\($" "" name "${match}")
  list(APPEND header_exports "${name}")
endforeach()
list(REMOVE_DUPLICATES header_exports)
list(SORT header_exports)
if(NOT "${header_exports}" STREQUAL "${expected}")
  message(FATAL_ERROR "firmware_report header declares ${header_exports}; expected ${expected}")
endif()

file(READ "${RUST_SOURCE}" rust_source)
string(REGEX REPLACE "[ \t\r\n]" "" normalized_rust "${rust_source}")
set(
  required_rust_contract
  "const_:unsafeextern\"C\"fn(*constu8,usize,*mut*mutReportHandle)->ReportStatus=firmware_report_create"
  "const_:unsafeextern\"C\"fn(*constReportHandle,*mutReportSummary)->ReportStatus=firmware_report_query"
  "const_:unsafeextern\"C\"fn(*mut*mutReportHandle)->ReportStatus=firmware_report_release"
  "assert!(size_of::<ReportStatus>()==4)"
  "assert!(size_of::<ReportSummary>()==32)"
  "assert!(align_of::<ReportSummary>()==8)"
  "assert!(std::mem::offset_of!(ReportSummary,fnv1a64)==24)"
)
foreach(fragment IN LISTS required_rust_contract)
  string(FIND "${normalized_rust}" "${fragment}" position)
  if(position EQUAL -1)
    message(FATAL_ERROR "firmware_report Rust adapter is missing ABI contract fragment: ${fragment}")
  endif()
endforeach()
string(REGEX MATCHALL "extern[ \t\r\n]+\"C\"[ \t\r\n]+fn[ \t\r\n]+firmware_report_[A-Za-z0-9_]+" rust_matches "${rust_source}")
set(rust_exports)
foreach(match IN LISTS rust_matches)
  string(REGEX MATCH "firmware_report_[A-Za-z0-9_]+$" name "${match}")
  list(APPEND rust_exports "${name}")
endforeach()
list(REMOVE_DUPLICATES rust_exports)
list(SORT rust_exports)
if(NOT "${rust_exports}" STREQUAL "${expected}")
  message(FATAL_ERROR "firmware_report Rust adapter defines ${rust_exports}; expected ${expected}")
endif()
message(STATUS "firmware_report header, Rust adapter, and archive agree on ${expected}")
