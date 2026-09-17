# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
#
# Build-time consumption of a first-party build tool whose implementation has
# moved to Zig (epic #858). The tool is built for the HOST with the pinned Zig
# toolchain even inside an Arm cross build, because it runs on the build
# machine, not on the target.
#
# Fail-closed on purpose: a missing Zig toolchain is a configure-time error, so
# a build cannot silently skip a post-build image step and hand back a hex that
# is half an image.

include_guard(GLOBAL)
include(CMakeParseArguments)

# Build one migrated Zig tool and expose it as an IMPORTED executable named
# ra8_zig::<NAME>, plus the target ra8_zig_tool_<NAME> for build ordering.
function(ra8_add_zig_host_tool)
  cmake_parse_arguments(RA8_ZIG_TOOL "" "NAME;ZIG_ROOT" "" ${ARGN})

  foreach(_ra8_required IN ITEMS NAME ZIG_ROOT)
    if(NOT RA8_ZIG_TOOL_${_ra8_required})
      message(FATAL_ERROR "ra8_add_zig_host_tool requires ${_ra8_required}")
    endif()
  endforeach()

  if(TARGET ra8_zig::${RA8_ZIG_TOOL_NAME})
    return()
  endif()

  if(NOT ZIG_EXECUTABLE)
    find_program(ZIG_EXECUTABLE NAMES zig)
  endif()
  if(NOT ZIG_EXECUTABLE)
    message(FATAL_ERROR "${RA8_ZIG_TOOL_NAME}: migrated Zig build tool requires zig on PATH")
  endif()

  set(_ra8_output_dir "${CMAKE_BINARY_DIR}/zig_tools/${RA8_ZIG_TOOL_NAME}")
  if(CMAKE_HOST_WIN32)
    set(_ra8_host_suffix ".exe")
  else()
    set(_ra8_host_suffix "")
  endif()
  set(_ra8_executable
      "${_ra8_output_dir}/bin/${RA8_ZIG_TOOL_NAME}${_ra8_host_suffix}"
  )

  # The tool's own sources are the command's inputs. Without them the output
  # exists after the first build and the generator never reruns zig again, so
  # an edit under src/ would ship a stale tool for the rest of the build tree's
  # life. CONFIGURE_DEPENDS re-globs at build time, so a new file counts too.
  file(GLOB_RECURSE _ra8_tool_sources CONFIGURE_DEPENDS
       "${RA8_ZIG_TOOL_ZIG_ROOT}/src/*.zig" "${RA8_ZIG_TOOL_ZIG_ROOT}/build.zig"
       "${RA8_ZIG_TOOL_ZIG_ROOT}/build.zig.zon"
  )

  add_custom_command(
    OUTPUT "${_ra8_executable}"
    COMMAND
      "${ZIG_EXECUTABLE}" build -Doptimize=ReleaseSafe --prefix "${_ra8_output_dir}" --cache-dir
      "${_ra8_output_dir}/cache" --global-cache-dir "${_ra8_output_dir}/global-cache"
    DEPENDS ${_ra8_tool_sources}
    WORKING_DIRECTORY "${RA8_ZIG_TOOL_ZIG_ROOT}"
    COMMENT "Building Zig build tool ${RA8_ZIG_TOOL_NAME}"
    VERBATIM
  )

  add_custom_target(ra8_zig_tool_${RA8_ZIG_TOOL_NAME} DEPENDS "${_ra8_executable}")

  add_executable(ra8_zig::${RA8_ZIG_TOOL_NAME} IMPORTED GLOBAL)
  set_target_properties(
    ra8_zig::${RA8_ZIG_TOOL_NAME} PROPERTIES IMPORTED_LOCATION "${_ra8_executable}"
  )
  add_dependencies(ra8_zig::${RA8_ZIG_TOOL_NAME} ra8_zig_tool_${RA8_ZIG_TOOL_NAME})
endfunction()

# Convenience wrapper for the Intel HEX merge tool, so each of its four callers
# names the tool once instead of repeating the path to its build root.
function(ra8_use_merge_ihex target)
  ra8_add_zig_host_tool(NAME merge_ihex ZIG_ROOT "${RA8_REPO_ROOT}/tools/merge_ihex")
  add_dependencies(${target} ra8_zig_tool_merge_ihex)
endfunction()
