# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
#
# Compare the namespaced symbols in the Rust archive with both authoritative
# declarations. Extraction is self-tested with realistic comment decoys so a
# detector regression cannot silently turn the ABI check into a no-op.

# Remove comment decoys before declaration extraction.
function(ra8_strip_comments out_var input)
  string(REGEX REPLACE "/\\*([^*]|\\*+[^*/])*\\*+/" "" _ra8_clean "${input}")
  string(REGEX REPLACE "//[^\n\r]*" "" _ra8_clean "${_ra8_clean}")
  set(${out_var}
      "${_ra8_clean}"
      PARENT_SCOPE
  )
endfunction()

# Extract and sort namespaced function declarations from the public header.
function(ra8_header_inventory out_var input)
  ra8_strip_comments(_ra8_clean "${input}")
  string(REGEX MATCHALL "ra8_rust_abi_fixture_[A-Za-z0-9_]+\\(" _ra8_names "${_ra8_clean}")
  list(TRANSFORM _ra8_names REPLACE "\\($" "")
  list(REMOVE_DUPLICATES _ra8_names)
  list(SORT _ra8_names)
  set(${out_var}
      "${_ra8_names}"
      PARENT_SCOPE
  )
endfunction()

# Extract names and count unmangled versus explicit C declarations in Rust.
function(
  ra8_rust_inventory
  out_names
  out_attributes
  out_signatures
  input
)
  ra8_strip_comments(_ra8_clean "${input}")
  string(REGEX MATCHALL "unsafe\\(no_mangle\\)" _ra8_attributes "${_ra8_clean}")
  string(CONCAT _ra8_signature_regex
                "pub[ \t\r\n]+(unsafe[ \t\r\n]+)?extern[ \t\r\n]+\"C\"[ \t\r\n]+fn"
                "[ \t\r\n]+ra8_rust_abi_fixture_[A-Za-z0-9_]+"
  )
  string(REGEX MATCHALL "${_ra8_signature_regex}" _ra8_signatures "${_ra8_clean}")
  list(LENGTH _ra8_attributes _ra8_attribute_count)
  list(LENGTH _ra8_signatures _ra8_signature_count)
  list(TRANSFORM _ra8_signatures REPLACE ".*fn[ \t\r\n]+" "")
  list(SORT _ra8_signatures)
  set(${out_names}
      "${_ra8_signatures}"
      PARENT_SCOPE
  )
  set(${out_attributes}
      ${_ra8_attribute_count}
      PARENT_SCOPE
  )
  set(${out_signatures}
      ${_ra8_signature_count}
      PARENT_SCOPE
  )
endfunction()

# Report whether binary, header, source, and calling-convention inventories agree.
function(
  ra8_inventory_agrees
  out_var
  actual
  header
  rust
  extern_valid
)
  if("${actual}" STREQUAL "${header}"
     AND "${actual}" STREQUAL "${rust}"
     AND extern_valid
  )
    set(${out_var}
        TRUE
        PARENT_SCOPE
    )
  else()
    set(${out_var}
        FALSE
        PARENT_SCOPE
    )
  endif()
endfunction()

# Validate complete C and Rust signatures against the ABI fixture policy.
function(ra8_signatures_match_policy out_var header rust)
  ra8_strip_comments(_ra8_clean_header "${header}")
  ra8_strip_comments(_ra8_clean_rust "${rust}")
  string(REGEX REPLACE "[ \t\r\n]+" " " _ra8_clean_header "${_ra8_clean_header}")
  string(REGEX REPLACE "[ \t\r\n]+" " " _ra8_clean_rust "${_ra8_clean_rust}")
  string(CONCAT _ra8_header_apply "ra8_err_t ra8_rust_abi_fixture_apply("
                "const ra8_rust_abi_fixture_config_t* config, uint32_t* out_result)"
  )
  string(CONCAT _ra8_rust_apply "pub unsafe extern \"C\" fn ra8_rust_abi_fixture_apply( "
                "config: *const AbiConfig, out_result: *mut u32, ) -> AbiResult"
  )
  string(CONCAT _ra8_rust_create "pub unsafe extern \"C\" fn ra8_rust_abi_fixture_create( "
                "out_handle: *mut *mut AbiFixture, ) -> AbiResult"
  )
  string(CONCAT _ra8_rust_destroy "pub unsafe extern \"C\" fn ra8_rust_abi_fixture_destroy( "
                "in_out_handle: *mut *mut AbiFixture, ) -> AbiResult"
  )
  set(_ra8_header_signatures
      "${_ra8_header_apply}"
      "ra8_err_t ra8_rust_abi_fixture_create(ra8_rust_abi_fixture_t** out_handle)"
      "void ra8_rust_abi_fixture_test_fail_next_allocation(void)"
      "void ra8_rust_abi_fixture_test_reset_apply_calls(void)"
      "uint32_t ra8_rust_abi_fixture_test_apply_calls(void)"
      "uint32_t ra8_rust_abi_fixture_test_live_handles(void)"
      "ra8_err_t ra8_rust_abi_fixture_destroy(ra8_rust_abi_fixture_t** in_out_handle)"
  )
  set(_ra8_rust_signatures
      "${_ra8_rust_apply}"
      "${_ra8_rust_create}"
      "pub extern \"C\" fn ra8_rust_abi_fixture_test_fail_next_allocation()"
      "pub extern \"C\" fn ra8_rust_abi_fixture_test_reset_apply_calls()"
      "pub extern \"C\" fn ra8_rust_abi_fixture_test_apply_calls() -> u32"
      "pub extern \"C\" fn ra8_rust_abi_fixture_test_live_handles() -> u32"
      "${_ra8_rust_destroy}"
  )
  set(_ra8_matches TRUE)
  foreach(_ra8_signature IN LISTS _ra8_header_signatures)
    string(FIND "${_ra8_clean_header}" "${_ra8_signature}" _ra8_position)
    if(_ra8_position EQUAL -1)
      set(_ra8_matches FALSE)
    endif()
  endforeach()
  foreach(_ra8_signature IN LISTS _ra8_rust_signatures)
    string(FIND "${_ra8_clean_rust}" "${_ra8_signature}" _ra8_position)
    if(_ra8_position EQUAL -1)
      set(_ra8_matches FALSE)
    endif()
  endforeach()
  set(${out_var}
      ${_ra8_matches}
      PARENT_SCOPE
  )
endfunction()

string(CONCAT _ra8_selftest_header "/* ra8_rust_abi_fixture_comment_fake( */\n"
              "ra8_err_t ra8_rust_abi_fixture_apply(void);\n"
              "ra8_err_t ra8_rust_abi_fixture_create(void);"
)
string(
  CONCAT _ra8_selftest_rust
         "// unsafe(no_mangle) pub extern \"C\" fn ra8_rust_abi_fixture_comment_fake\n"
         "#[unsafe(no_mangle)] pub unsafe extern \"C\" fn ra8_rust_abi_fixture_apply() {}\n"
         "#[unsafe(no_mangle)] pub extern \"C\" fn ra8_rust_abi_fixture_create() {}"
)
set(_ra8_selftest_symbols "ra8_rust_abi_fixture_apply;ra8_rust_abi_fixture_create")
ra8_header_inventory(_ra8_selftest_header_names "${_ra8_selftest_header}")
ra8_rust_inventory(
  _ra8_selftest_rust_names _ra8_selftest_attributes _ra8_selftest_signatures
  "${_ra8_selftest_rust}"
)
if(_ra8_selftest_attributes EQUAL _ra8_selftest_signatures)
  set(_ra8_selftest_extern_valid TRUE)
else()
  set(_ra8_selftest_extern_valid FALSE)
endif()
ra8_inventory_agrees(
  _ra8_selftest_ok
  "${_ra8_selftest_symbols}"
  "${_ra8_selftest_header_names}"
  "${_ra8_selftest_rust_names}"
  ${_ra8_selftest_extern_valid}
)
if(NOT _ra8_selftest_ok)
  message(FATAL_ERROR "Rust ABI checker selftest rejected valid declarations with comment decoys")
endif()
foreach(_ra8_bad_symbols IN ITEMS "ra8_rust_abi_fixture_apply"
                                  "${_ra8_selftest_symbols};ra8_rust_abi_fixture_unexpected"
)
  ra8_inventory_agrees(
    _ra8_selftest_ok
    "${_ra8_bad_symbols}"
    "${_ra8_selftest_header_names}"
    "${_ra8_selftest_rust_names}"
    ${_ra8_selftest_extern_valid}
  )
  if(_ra8_selftest_ok)
    message(FATAL_ERROR "Rust ABI checker selftest accepted a missing or unexpected symbol")
  endif()
endforeach()
string(REPLACE "extern \"C\" " "" _ra8_non_c_rust "${_ra8_selftest_rust}")
ra8_rust_inventory(
  _ra8_non_c_names _ra8_non_c_attributes _ra8_non_c_signatures "${_ra8_non_c_rust}"
)
if(_ra8_non_c_attributes EQUAL _ra8_non_c_signatures)
  set(_ra8_non_c_extern_valid TRUE)
else()
  set(_ra8_non_c_extern_valid FALSE)
endif()
ra8_inventory_agrees(
  _ra8_selftest_ok
  "${_ra8_selftest_symbols}"
  "${_ra8_selftest_header_names}"
  "${_ra8_non_c_names}"
  ${_ra8_non_c_extern_valid}
)
if(_ra8_selftest_ok)
  message(FATAL_ERROR "Rust ABI checker selftest accepted a non-extern C export")
endif()

execute_process(
  COMMAND "${RA8_NM}" -g --defined-only "${RA8_LIBRARY}"
  RESULT_VARIABLE _ra8_nm_result
  OUTPUT_VARIABLE _ra8_symbols
  ERROR_VARIABLE _ra8_nm_error
)
if(NOT _ra8_nm_result EQUAL 0)
  message(FATAL_ERROR "Rust ABI symbol inspection failed: ${_ra8_nm_error}")
endif()
string(REGEX MATCHALL "ra8_rust_abi_fixture_[A-Za-z0-9_]+" _ra8_actual "${_ra8_symbols}")
list(REMOVE_DUPLICATES _ra8_actual)
list(SORT _ra8_actual)

file(READ "${RA8_HEADER}" _ra8_header)
ra8_header_inventory(_ra8_header_symbols "${_ra8_header}")
file(READ "${RA8_RUST_SOURCE}" _ra8_rust_source)
ra8_signatures_match_policy(_ra8_signatures_valid "${_ra8_header}" "${_ra8_rust_source}")
if(NOT _ra8_signatures_valid)
  message(FATAL_ERROR "Rust ABI provider/header signature policy mismatch")
endif()
string(REPLACE "config: *const AbiConfig" "config: *mut AbiConfig" _ra8_bad_signature
               "${_ra8_rust_source}"
)
ra8_signatures_match_policy(_ra8_bad_signature_accepted "${_ra8_header}" "${_ra8_bad_signature}")
if(_ra8_bad_signature_accepted)
  message(FATAL_ERROR "Rust ABI checker selftest accepted pointer-qualification drift")
endif()
ra8_rust_inventory(
  _ra8_c_signatures _ra8_unmangled_count _ra8_c_signature_count "${_ra8_rust_source}"
)
if(_ra8_unmangled_count EQUAL _ra8_c_signature_count)
  set(_ra8_extern_valid TRUE)
else()
  set(_ra8_extern_valid FALSE)
endif()
ra8_inventory_agrees(
  _ra8_contract_ok
  "${_ra8_actual}"
  "${_ra8_header_symbols}"
  "${_ra8_c_signatures}"
  ${_ra8_extern_valid}
)
if(NOT _ra8_contract_ok)
  message(
    FATAL_ERROR
      "Rust ABI mismatch: binary=${_ra8_actual}; header=${_ra8_header_symbols};"
      " source=${_ra8_c_signatures}; unmangled=${_ra8_unmangled_count};"
      " extern-C=${_ra8_c_signature_count}"
  )
endif()
