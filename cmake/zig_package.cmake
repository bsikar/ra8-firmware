# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
#
# cmake/zig_package.cmake
#
# Resolves a third-party dependency pinned in build.zig.zon (url + hash) to its
# unpacked directory, fetching it once when it is not there yet. The hash is
# Zig's content hash of the unpacked tarball, so a directory that resolves is
# byte-for-byte the pinned upstream tree.
#
# Zig 0.17 keeps only a recompressed p/<hash>.tar.gz in the global cache and
# unpacks packages into zig-pkg/<hash> beside the build.zig.zon that pins them
# (RA8FW-932). This unpacks into that same zig-pkg/, so CMake and `zig build`
# share one copy.
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
  # `zig env` prints a ZON struct on Zig 0.17, not JSON.
  string(REGEX MATCH "[.]global_cache_dir = \"([^\"]+)\"" _cache_match "${_env}")
  set(_cache "${CMAKE_MATCH_1}")
  if(_cache STREQUAL "")
    message(FATAL_ERROR "${name}: no .global_cache_dir in `zig env` output")
  endif()
  set(_pkg_root "${_RA8_ZIG_PACKAGE_ROOT}/zig-pkg")
  set(_dir "${_pkg_root}/${_hash}")
  set(_tarball "${_cache}/p/${_hash}.tar.gz")

  if(NOT IS_DIRECTORY "${_dir}" AND NOT EXISTS "${_tarball}")
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
    if(NOT EXISTS "${_tarball}")
      message(FATAL_ERROR "${name}: ${_tarball} is missing after the fetch")
    endif()
    # Unpack beside the target and rename into place, so two configures
    # racing on the same package never see a half-written directory.
    string(RANDOM LENGTH 12 _tag)
    set(_tmp "${_pkg_root}/tmp-${_tag}")
    file(ARCHIVE_EXTRACT INPUT "${_tarball}" DESTINATION "${_tmp}")
    if(NOT IS_DIRECTORY "${_tmp}/${_hash}")
      file(REMOVE_RECURSE "${_tmp}")
      message(FATAL_ERROR "${name}: ${_tarball} has no top-level ${_hash}/")
    endif()
    # The cached tarball can keep the upstream archive's single top-level
    # directory; Zig hashes the tree inside it, so that is the package root.
    set(_src "${_tmp}/${_hash}")
    file(GLOB _top LIST_DIRECTORIES true "${_src}/*")
    list(LENGTH _top _top_count)
    if(_top_count EQUAL 1 AND IS_DIRECTORY "${_top}")
      set(_src "${_top}")
    endif()
    # Prove the unpacked tree is the pinned one before anything builds from it.
    execute_process(
      COMMAND "${ZIG_EXECUTABLE}" fetch "${_src}"
      WORKING_DIRECTORY "${CMAKE_BINARY_DIR}"
      OUTPUT_VARIABLE _unpacked
      RESULT_VARIABLE _verify_rc
      OUTPUT_STRIP_TRAILING_WHITESPACE
    )
    if(NOT _verify_rc EQUAL 0 OR NOT _unpacked STREQUAL _hash)
      file(REMOVE_RECURSE "${_tmp}")
      message(FATAL_ERROR "${name}: unpacked tree hashes to '${_unpacked}' "
                          "(rc ${_verify_rc}), build.zig.zon pins '${_hash}'"
      )
    endif()
    if(NOT IS_DIRECTORY "${_dir}")
      file(RENAME "${_src}" "${_dir}" RESULT _rename_rc)
    endif()
    file(REMOVE_RECURSE "${_tmp}")
  endif()
  if(NOT IS_DIRECTORY "${_dir}")
    message(FATAL_ERROR "${name}: ${_dir} is missing after unpacking")
  endif()
  set(${out_var} "${_dir}" PARENT_SCOPE)
endfunction()
