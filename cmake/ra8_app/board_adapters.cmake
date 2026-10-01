# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
#
# The chip adapters that travel with the board layer (#693). The board's
# clock and GPT profiles bind them, so every app built for the board needs
# them whether or not it names them in LIBS. Both are macros so they set the
# caller's variables in _ra8_app_collect_sources; split out of sources.cmake,
# which is over the file-size cap.

# Append the adapter sources and the port facades they implement to
# _ra8_lib_board.
macro(_ra8_app_board_adapter_sources)
  # The RA8 clock adapter travels with the board layer. Since d0ec9994 (#693)
  # ra8_board_ek_ra8d2_clock_profile.c includes fw_if_clock_ra8.h and calls
  # fw_clock_ra8_iface(), whose only definition is libs/if_ra8_cgc. No app
  # names if_ra8_cgc in LIBS, and none should: it is the adapter the board
  # binds to, not a library an app picks. Without this block every app for
  # the board stopped compiling at that #include. The Zig graph links the
  # same adapter beside the board archive (board_archive.chip_clock_adapter).
  #
  # The port it implements comes along for the same reason: the board's
  # ra8_board_clock_profile_bind() calls fw_clock_bind(), and since 950d187b
  # the HIL examples call fw_clock_rate_for(ra8_board_clock(), ...). Both are
  # defined in libs/if/src/fw_if_clock.c, which no app compiled unless it
  # named `if` in LIBS. Only that one unit, not the whole of libs/if; an app
  # that does name `if` lists the same path again and CMake builds it once.
  if(EXISTS "${RA8_REPO_ROOT}/libs/if_ra8_cgc/src")
    file(GLOB _ra8_lib_clock_adapter CONFIGURE_DEPENDS ${RA8_REPO_ROOT}/libs/if_ra8_cgc/src/*.c)
    list(APPEND _ra8_lib_board ${_ra8_lib_clock_adapter})
    if(EXISTS "${RA8_REPO_ROOT}/libs/if/src/fw_if_clock.c")
      list(APPEND _ra8_lib_board ${RA8_REPO_ROOT}/libs/if/src/fw_if_clock.c)
    endif()
  endif()
  # The GPT timer and PWM adapters travel with the board for the same reason:
  # ra8_board_ek_ra8d2_gpt_profile.c (#693) binds them, so every board app
  # needs libs/if_ra8_gpt and the two ports' facades, fw_if_timer.c and
  # fw_if_pwm.c. Only those two units of libs/if, as with fw_if_clock.c.
  if(EXISTS "${RA8_REPO_ROOT}/libs/if_ra8_gpt/src")
    file(GLOB _ra8_lib_gpt_adapter CONFIGURE_DEPENDS ${RA8_REPO_ROOT}/libs/if_ra8_gpt/src/*.c)
    list(APPEND _ra8_lib_board ${_ra8_lib_gpt_adapter})
    foreach(_ra8_gpt_port fw_if_timer.c fw_if_pwm.c)
      if(EXISTS "${RA8_REPO_ROOT}/libs/if/src/${_ra8_gpt_port}")
        list(APPEND _ra8_lib_board ${RA8_REPO_ROOT}/libs/if/src/${_ra8_gpt_port})
      endif()
    endforeach()
  endif()
endmacro()

# Append the adapter include directories to _ra8_lib_inc.
macro(_ra8_app_board_adapter_includes)
  # The clock adapter's header rides with the board too: both the board's
  # clock_profile.c and its public clock_profile.h include fw_if_clock_ra8.h
  # (see the adapter block above).
  if(EXISTS "${RA8_REPO_ROOT}/libs/if_ra8_cgc/inc")
    list(APPEND _ra8_lib_inc ${RA8_REPO_ROOT}/libs/if_ra8_cgc/inc)
  endif()
  # And the GPT adapters' header, which the board's gpt_profile.c includes.
  if(EXISTS "${RA8_REPO_ROOT}/libs/if_ra8_gpt/inc")
    list(APPEND _ra8_lib_inc ${RA8_REPO_ROOT}/libs/if_ra8_gpt/inc)
  endif()
endmacro()
