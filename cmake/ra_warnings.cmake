# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
#
# Shared warning profile for ra8d2 project-owned code.
#
# Keep third-party source files on their own permissive settings; this profile
# is intended for firmware, examples, ports, tests, and domain libraries that
# this repository owns.
#

function(ra_target_enable_project_warnings target)
    set(options)
    set(one_value_args STACK_USAGE_BYTES)
    set(multi_value_args)
    cmake_parse_arguments(RA_WARN
        "${options}"
        "${one_value_args}"
        "${multi_value_args}"
        ${ARGN})

    target_compile_options(${target} PRIVATE
        -Wall
        -Wextra
        -Werror
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
    # ra_target_enable_project_warnings().
    #
    # Two effects, both required for the project-wide stack-bound proof
    # (see docs/STACK_USAGE.md):
    #   * -Wstack-usage=N -- the build-time gate. Any function whose
    #     compile-time stack frame exceeds N bytes triggers -Werror.
    #   * -fstack-usage   -- gcc emits a `<file>.su` next to each `.o`,
    #     consumed by scripts/utils/stack_usage_check.py to aggregate
    #     a project-wide report and to gate the critical-path modules
    #     (ra_isr, ra_check, ra_err, ra_mpu, ra_cgc, ra_pfs).
    if(NOT DEFINED RA_WARN_STACK_USAGE_BYTES OR RA_WARN_STACK_USAGE_BYTES STREQUAL "")
        set(RA_WARN_STACK_USAGE_BYTES 2048)
    endif()
    target_compile_options(${target} PRIVATE
        $<$<COMPILE_LANG_AND_ID:C,GNU>:-Wstack-usage=${RA_WARN_STACK_USAGE_BYTES}>
        $<$<COMPILE_LANG_AND_ID:C,GNU>:-fstack-usage>)
endfunction()