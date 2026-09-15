# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
#
# Reusable host-only proof that a Zig static library is consumable through its
# hand-authored C23 ABI, rather than only through Zig unit tests.

include(CMakeParseArguments)

# Register C and Rust consumers of one Zig-built public C ABI artifact.
function(ra8_add_zig_c_abi_contract)
  set(_ra8_options "")
  set(_ra8_one_value_args
      NAME
      ZIG_ROOT
      PUBLIC_INCLUDE_DIR
      C_FIXTURE
      RUST_CRATE
      LIBRARY_NAME
  )
  cmake_parse_arguments(
    RA8_ABI
    "${_ra8_options}"
    "${_ra8_one_value_args}"
    ""
    ${ARGN}
  )

  foreach(_ra8_required IN LISTS _ra8_one_value_args)
    if(NOT RA8_ABI_${_ra8_required})
      message(FATAL_ERROR "ra8_add_zig_c_abi_contract requires ${_ra8_required}")
    endif()
  endforeach()

  if(NOT ZIG_EXECUTABLE)
    find_program(ZIG_EXECUTABLE NAMES zig)
  endif()
  if(NOT ZIG_EXECUTABLE)
    message(FATAL_ERROR "${RA8_ABI_NAME}: Zig C ABI contract requires zig on PATH")
  endif()

  set(_ra8_output_dir "${CMAKE_CURRENT_BINARY_DIR}/zig_abi/${RA8_ABI_NAME}")
  set(_ra8_library_file
      "${CMAKE_STATIC_LIBRARY_PREFIX}${RA8_ABI_LIBRARY_NAME}${CMAKE_STATIC_LIBRARY_SUFFIX}"
  )
  set(_ra8_library "${_ra8_output_dir}/lib/${_ra8_library_file}")

  add_custom_target(
    ${RA8_ABI_NAME}_zig_library ALL
    COMMAND "${ZIG_EXECUTABLE}" build -Doptimize=Debug --prefix "${_ra8_output_dir}" --cache-dir
            "${_ra8_output_dir}/cache" --global-cache-dir "${_ra8_output_dir}/global-cache"
    WORKING_DIRECTORY "${RA8_ABI_ZIG_ROOT}"
    BYPRODUCTS "${_ra8_library}"
    COMMENT "Building Zig C ABI fixture ${RA8_ABI_NAME}"
    VERBATIM
  )

  add_executable(${RA8_ABI_NAME}_c_consumer "${RA8_ABI_C_FIXTURE}")
  target_compile_options(${RA8_ABI_NAME}_c_consumer PRIVATE -Wall -Wextra -Werror)
  target_include_directories(
    ${RA8_ABI_NAME}_c_consumer PRIVATE "${RA8_ABI_PUBLIC_INCLUDE_DIR}"
                                       "${FW_ROOT}/libs/ra8_core/inc"
  )
  target_link_libraries(${RA8_ABI_NAME}_c_consumer PRIVATE "${_ra8_library}")
  add_dependencies(${RA8_ABI_NAME}_c_consumer ${RA8_ABI_NAME}_zig_library)
  add_test(NAME ${RA8_ABI_NAME}_c_consumer COMMAND ${RA8_ABI_NAME}_c_consumer)

  find_program(CARGO_EXECUTABLE NAMES cargo REQUIRED)
  set(_ra8_rust_target_dir "${_ra8_output_dir}/rust-target")
  set(_ra8_rust_environment "RA8_ABI_FIXTURE_LIB_DIR=${_ra8_output_dir}/lib"
                            "CARGO_TARGET_DIR=${_ra8_rust_target_dir}"
  )
  add_custom_target(
    ${RA8_ABI_NAME}_rust_consumer ALL
    COMMAND "${CMAKE_COMMAND}" -E env ${_ra8_rust_environment} "${CARGO_EXECUTABLE}" test --locked
            --all-features --no-run --manifest-path "${RA8_ABI_RUST_CRATE}/Cargo.toml"
    DEPENDS ${RA8_ABI_NAME}_zig_library
    COMMENT "Building Rust consumer of Zig C ABI fixture ${RA8_ABI_NAME}"
    VERBATIM
  )
  add_test(NAME ${RA8_ABI_NAME}_rust_consumer
           COMMAND "${CMAKE_COMMAND}" -E env ${_ra8_rust_environment} "${CARGO_EXECUTABLE}" test
                   --locked --all-features --manifest-path "${RA8_ABI_RUST_CRATE}/Cargo.toml"
  )

  foreach(_ra8_negative_kind IN ITEMS layout missing_symbol)
    add_test(
      NAME ${RA8_ABI_NAME}_negative_${_ra8_negative_kind}
      COMMAND
        "${CMAKE_COMMAND}" "-DRA8_C_COMPILER=${CMAKE_C_COMPILER}"
        "-DRA8_INCLUDE_DIR=${RA8_ABI_PUBLIC_INCLUDE_DIR}"
        "-DRA8_CORE_INCLUDE_DIR=${FW_ROOT}/libs/ra8_core/inc" "-DRA8_LIBRARY=${_ra8_library}"
        "-DRA8_SOURCE=${RA8_ABI_ZIG_ROOT}/negative_${_ra8_negative_kind}.c"
        "-DRA8_KIND=${_ra8_negative_kind}" -P
        "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/expect_c_failure.cmake"
    )
  endforeach()
endfunction()
