# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
#
# cmake/zig_package.cmake
#
# Resolves a third-party dependency pinned in build.zig.zon (url + hash) to its
# directory in the Zig global package cache, fetching it once when it is not
# there yet. The hash is Zig's content hash of the unpacked tarball, so a
# directory that resolves is byte-for-byte the pinned upstream tree.
#
#   include(${REPO_ROOT}/cmake/zig_package.cmake)
#   ra8_zig_package_dir(flatbuffers _flatbuffers_dir)

if(DEFINED _RA8_ZIG_PACKAGE_INCLUDED)
  return()
endif()
set(_RA8_ZIG_PACKAGE_INCLUDED TRUE)

get_filename_component(_RA8_ZIG_PACKAGE_ROOT "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)

function(ra8_zig_package_dir name out_var)
  if(NOT ZIG_EXECUTABLE)
    find_program(ZIG_EXECUTABLE NAMES zig)
  endif()
  if(NOT ZIG_EXECUTABLE)
    message(FATAL_ERROR "${name} is pinned in build.zig.zon; resolving it needs zig on PATH")
  endif()

  file(READ "${_RA8_ZIG_PACKAGE_ROOT}/build.zig.zon" _zon)
  string(REGEX MATCH "[.]${name} = [.][{][^}]*[}]" _entry "${_zon}")
  string(REGEX MATCH "[.]url = \"([^\"]+)\"" _url_match "${_entry}")
  set(_url "${CMAKE_MATCH_1}")
  string(REGEX MATCH "[.]hash = \"([^\"]+)\"" _hash_match "${_entry}")
  set(_hash "${CMAKE_MATCH_1}")
  if(_url STREQUAL "" OR _hash STREQUAL "")
    message(FATAL_ERROR "${name}: no .url/.hash entry for it in build.zig.zon")
  endif()

  execute_process(
    COMMAND "${ZIG_EXECUTABLE}" env
    OUTPUT_VARIABLE _env
    RESULT_VARIABLE _env_rc
  )
  if(NOT _env_rc EQUAL 0)
    message(FATAL_ERROR "${name}: `zig env` failed (${_env_rc})")
  endif()
  string(JSON _cache GET "${_env}" global_cache_dir)
  set(_dir "${_cache}/p/${_hash}")

  if(NOT IS_DIRECTORY "${_dir}")
    message(STATUS "${name}: fetching ${_url}")
    execute_process(
      COMMAND "${ZIG_EXECUTABLE}" fetch "${_url}"
      WORKING_DIRECTORY "${CMAKE_BINARY_DIR}"
      OUTPUT_VARIABLE _got
      RESULT_VARIABLE _fetch_rc
      OUTPUT_STRIP_TRAILING_WHITESPACE
    )
    if(NOT _fetch_rc EQUAL 0 OR NOT _got STREQUAL _hash)
      message(FATAL_ERROR "${name}: zig fetch gave '${_got}' (rc ${_fetch_rc}), "
                          "build.zig.zon pins '${_hash}'"
      )
    endif()
  endif()
  if(NOT IS_DIRECTORY "${_dir}")
    message(FATAL_ERROR "${name}: ${_dir} is missing after the fetch")
  endif()
  set(${out_var} "${_dir}" PARENT_SCOPE)
endfunction()
