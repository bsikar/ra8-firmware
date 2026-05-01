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

    if(RA_WARN_STACK_USAGE_BYTES)
        target_compile_options(${target} PRIVATE
            $<$<COMPILE_LANGUAGE:C>:-Wstack-usage=${RA_WARN_STACK_USAGE_BYTES}>)
    endif()
endfunction()
