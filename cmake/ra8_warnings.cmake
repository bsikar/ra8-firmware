# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
#
# Shared warning profile for ra8d2 project-owned code.
#
# Keep third-party source files on their own permissive settings; this profile
# is intended for firmware, examples, ports, tests, and domain libraries that
# this repository owns.
#

# Apply the shared project warning profile to one target.
#
# Adds -Wall -Wextra -Werror plus the project's extended warning set, and
# -Wstack-usage=<STACK_USAGE_BYTES> when that keyword is given. Intended
# for first-party targets only -- vendored code under libs/third_party/
# keeps its own permissive settings.
#
#   target                     the target to configure (positional)
#   STACK_USAGE_BYTES <n>      per-frame stack budget; omitted = no check
function(ra8_target_enable_project_warnings target)
  set(options)
  set(one_value_args STACK_USAGE_BYTES)
  set(multi_value_args)
  cmake_parse_arguments(
    RA8_WARN
    "${options}"
    "${one_value_args}"
    "${multi_value_args}"
    ${ARGN}
  )

  target_compile_options(
    ${target}
    PRIVATE -Wall
            -Wextra
            -Werror
            # Implicit-conversion discipline (#240). For C, -Wconversion also
            # enables -Wsign-conversion and flags float narrowing, so every
            # implicit narrowing / sign-changing / value-changing conversion in
            # first-party code is a hard -Werror. Vendored SOUP translation units
            # opt out via the narrow per-source -Wno sets in ra8_add_app.cmake
            # (firmware) and the blanket -w in tests/CMakeLists.txt (host).
            -Wconversion
            -Wcast-qual
            -Wcast-align
            -Wdouble-promotion
            -Wformat=2
            -Wpointer-arith
            -Wshadow
            -Wundef
            -Wvla
            -Wwrite-strings
            $<$<COMPILE_LANGUAGE:C>:-Wbad-function-cast>
            $<$<COMPILE_LANGUAGE:C>:-Wmissing-declarations>
            $<$<COMPILE_LANGUAGE:C>:-Wmissing-prototypes>
            $<$<COMPILE_LANGUAGE:C>:-Wnested-externs>
            $<$<COMPILE_LANGUAGE:C>:-Wold-style-definition>
            $<$<COMPILE_LANGUAGE:C>:-Wredundant-decls>
            $<$<COMPILE_LANGUAGE:C>:-Wstrict-prototypes>
            $<$<COMPILE_LANG_AND_ID:C,GNU>:-Wduplicated-branches>
            $<$<COMPILE_LANG_AND_ID:C,GNU>:-Wduplicated-cond>
            $<$<COMPILE_LANG_AND_ID:C,GNU>:-Wformat-overflow=2>
            $<$<COMPILE_LANG_AND_ID:C,GNU>:-Wformat-truncation=2>
            $<$<COMPILE_LANG_AND_ID:C,GNU>:-Wlogical-op>
  )

  # Per-app stack-usage budget. Defaults to 2048 bytes per function.
  # Per-app CMakeLists may override by passing STACK_USAGE_BYTES to
  # ra8_target_enable_project_warnings().
  #
  # Two effects, both required for the project-wide stack-bound proof
  # (see docs/STACK_USAGE.md):
  #   * -Wstack-usage=N -- the build-time gate. Any function whose
  #     compile-time stack frame exceeds N bytes triggers -Werror.
  #   * -fstack-usage   -- gcc emits a `<file>.su` next to each `.o`,
  #     consumed by scripts/checks/stack_usage_check.py to aggregate
  #     a project-wide report and to gate the critical-path modules
  #     (ra8_isr, ra8_check, ra8_err, ra8_mpu, ra8_cgc, ra8_pfs).
  if(NOT DEFINED RA8_WARN_STACK_USAGE_BYTES OR RA8_WARN_STACK_USAGE_BYTES STREQUAL "")
    set(RA8_WARN_STACK_USAGE_BYTES 2048)
  endif()
  # STACK_USAGE_BYTES 0 disables the compile-time stack gate. The host
  # unit-test build uses this: the host (x86_64 / arm64) ABI inflates
  # frames well past the Cortex-M85 budget, so -Wstack-usage=N there is a
  # false-positive generator (e.g. book / reflow XML walkers). The
  # real target stack budget is enforced by the firmware build, which
  # passes its per-app STACK_USAGE_BYTES, plus the .su aggregation in
  # scripts/checks/stack_usage_check.py over the ARM `.su` files.
  if(NOT RA8_WARN_STACK_USAGE_BYTES STREQUAL "0")
    target_compile_options(
      ${target} PRIVATE $<$<COMPILE_LANG_AND_ID:C,GNU>:-Wstack-usage=${RA8_WARN_STACK_USAGE_BYTES}>
                        $<$<COMPILE_LANG_AND_ID:C,GNU>:-fstack-usage>
    )
  endif()
endfunction()

# Reject warning demotions applied to an entire first-party target.
#
# A vendored translation unit may need a measured -Wno-<class> exception,
# but that exception belongs on the SOURCE COMPILE_OPTIONS property. Applying
# -Wno-error globally to a mixed app target also demotes the same diagnostic
# in every first-party source compiled into that ELF.
function(ra8_target_reject_warning_demotions target)
  get_target_property(_ra8_target_options ${target} COMPILE_OPTIONS)
  if(NOT _ra8_target_options)
    return()
  endif()

  foreach(_ra8_option IN LISTS _ra8_target_options)
    if(_ra8_option MATCHES "(^|:)-Wno-error($|[=>])")
      message(FATAL_ERROR "${target}: target-wide warning demotion '${_ra8_option}' is forbidden; "
                          "put a measured exception on the vendored SOURCE instead"
      )
    endif()
  endforeach()
endfunction()
