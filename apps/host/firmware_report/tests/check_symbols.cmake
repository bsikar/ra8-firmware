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

set(EXPECTED firmware_report_create firmware_report_query firmware_report_release)
list(SORT EXPECTED)

string(REGEX MATCHALL "[ \t]firmware_report_[A-Za-z0-9_]+" archive_matches "${symbols}")
set(ARCHIVE_EXPORTS)
foreach(match IN LISTS archive_matches)
  string(STRIP "${match}" name)
  list(APPEND ARCHIVE_EXPORTS "${name}")
endforeach()
list(REMOVE_DUPLICATES ARCHIVE_EXPORTS)
list(SORT ARCHIVE_EXPORTS)
if(NOT "${ARCHIVE_EXPORTS}" STREQUAL "${EXPECTED}")
  message(FATAL_ERROR "firmware_report archive exports ${ARCHIVE_EXPORTS}; expected ${EXPECTED}")
endif()

file(READ "${HEADER}" header)
string(REGEX REPLACE "[ \t\r\n]" "" normalized_header "${header}")
string(CONCAT HEADER_CREATE_SIGNATURE
              "firmware_report_status_tfirmware_report_create(constuint8_t*data,size_tsize,"
              "firmware_report_handle_t**out_handle)"
)
string(CONCAT HEADER_QUERY_SIGNATURE "firmware_report_status_tfirmware_report_query("
              "constfirmware_report_handle_t*handle,firmware_report_summary_t*out_summary)"
)
set(REQUIRED_HEADER_CONTRACT
    "${HEADER_CREATE_SIGNATURE}"
    "${HEADER_QUERY_SIGNATURE}"
    "firmware_report_status_tfirmware_report_release(firmware_report_handle_t**in_out_handle)"
    "static_assert(sizeof(firmware_report_status_t)==4U"
    "static_assert(sizeof(firmware_report_summary_t)==32U"
    "static_assert(alignof(firmware_report_summary_t)==8U"
)
foreach(fragment IN LISTS REQUIRED_HEADER_CONTRACT)
  string(FIND "${normalized_header}" "${fragment}" position)
  if(position EQUAL -1)
    message(FATAL_ERROR "firmware_report header is missing ABI contract fragment: ${fragment}")
  endif()
endforeach()
string(REGEX MATCHALL "firmware_report_[A-Za-z0-9_]+[ \t\r\n]*\\(" header_matches "${header}")
set(HEADER_EXPORTS)
foreach(match IN LISTS header_matches)
  string(REGEX REPLACE "[ \t\r\n]*\\($" "" name "${match}")
  list(APPEND HEADER_EXPORTS "${name}")
endforeach()
list(REMOVE_DUPLICATES HEADER_EXPORTS)
list(SORT HEADER_EXPORTS)
if(NOT "${HEADER_EXPORTS}" STREQUAL "${EXPECTED}")
  message(FATAL_ERROR "firmware_report header declares ${HEADER_EXPORTS}; expected ${EXPECTED}")
endif()

file(READ "${RUST_SOURCE}" rust_source)
string(REGEX REPLACE "[ \t\r\n]" "" normalized_rust "${rust_source}")
string(CONCAT RUST_CREATE_SIGNATURE
              "const_:unsafeextern\"C\"fn(*constu8,usize,*mut*mutReportHandle)->ReportStatus="
              "firmware_report_create"
)
string(CONCAT RUST_QUERY_SIGNATURE
              "const_:unsafeextern\"C\"fn(*constReportHandle,*mutReportSummary)->ReportStatus="
              "firmware_report_query"
)
set(REQUIRED_RUST_CONTRACT
    "${RUST_CREATE_SIGNATURE}"
    "${RUST_QUERY_SIGNATURE}"
    "const_:unsafeextern\"C\"fn(*mut*mutReportHandle)->ReportStatus=firmware_report_release"
    "assert!(size_of::<ReportStatus>()==4)"
    "assert!(size_of::<ReportSummary>()==32)"
    "assert!(align_of::<ReportSummary>()==8)"
    "assert!(std::mem::offset_of!(ReportSummary,fnv1a64)==24)"
)
foreach(fragment IN LISTS REQUIRED_RUST_CONTRACT)
  string(FIND "${normalized_rust}" "${fragment}" position)
  if(position EQUAL -1)
    message(
      FATAL_ERROR "firmware_report Rust adapter is missing ABI contract fragment: ${fragment}"
    )
  endif()
endforeach()
string(REGEX MATCHALL "extern[ \t\r\n]+\"C\"[ \t\r\n]+fn[ \t\r\n]+firmware_report_[A-Za-z0-9_]+"
             rust_matches "${rust_source}"
)
set(RUST_EXPORTS)
foreach(match IN LISTS rust_matches)
  string(REGEX MATCH "firmware_report_[A-Za-z0-9_]+$" name "${match}")
  list(APPEND RUST_EXPORTS "${name}")
endforeach()
list(REMOVE_DUPLICATES RUST_EXPORTS)
list(SORT RUST_EXPORTS)
if(NOT "${RUST_EXPORTS}" STREQUAL "${EXPECTED}")
  message(FATAL_ERROR "firmware_report Rust adapter defines ${RUST_EXPORTS}; expected ${EXPECTED}")
endif()
message(STATUS "firmware_report header, Rust adapter, and archive agree on ${EXPECTED}")
