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
  # The RA8 clock adapter travels with the board layer (#693): the board's
  # clock profile calls fw_clock_ra8_iface(), defined only in libs/if_ra8_cgc,
  # and ra8_board_clock_profile_bind() calls fw_clock_bind() from libs/if. No
  # app names either in LIBS, and none should.
  #
  # On zig/dev both are Zig: libs/if_ra8_cgc has no C left and fw_if_clock.c
  # became libs/if/src/fw_if_clock_abi.zig. So the adapter and the port ride
  # with the board as Zig archives, the same pair the Zig graph links beside
  # the board archive (board_archive.chip_clock_adapter, interface_archive).
  # The entries are appended to _ra8_lib_zig in sources.cmake, after that list
  # is initialised. dev compiles the C units here instead; on a dev -> zig/dev
  # sync keep this side.
  set(_ra8_board_adapter_zig "")
  if(EXISTS "${RA8_REPO_ROOT}/libs/if_ra8_cgc/build.zig")
    list(APPEND _ra8_board_adapter_zig "if_ra8_cgc|${RA8_REPO_ROOT}/libs/if_ra8_cgc")
  endif()
  if(EXISTS "${RA8_REPO_ROOT}/libs/if/build.zig")
    list(APPEND _ra8_board_adapter_zig "if|${RA8_REPO_ROOT}/libs/if")
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
