# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
#
# The chip adapters that travel with the board layer (RA8FW-299). The board's
# clock and GPT profiles bind them, so every app built for the board needs
# them whether or not it names them in LIBS. Both are macros so they set the
# caller's variables in _ra8_app_collect_sources; split out of sources.cmake,
# which is over the file-size cap.

# Append the adapter sources and the port facades they implement to
# _ra8_lib_board.
macro(_ra8_app_board_adapter_sources)
  # The RA8 clock adapter travels with the board layer (RA8FW-299): the board's
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
  # ra8_board_ek_ra8d2_gpt_profile.c (RA8FW-299) binds them, so every board app
  # needs libs/if_ra8_gpt and the two ports' facades. On zig/dev all of it is
  # Zig: libs/if_ra8_gpt has no C left, and fw_if_timer.c / fw_if_pwm.c became
  # libs/if/src/fw_if_{timer,pwm}_abi.zig, already in the "if" archive above.
  # dev compiles the C units here instead; on a dev -> zig/dev sync keep this
  # side.
  if(EXISTS "${RA8_REPO_ROOT}/libs/if_ra8_gpt/build.zig")
    list(APPEND _ra8_board_adapter_zig "if_ra8_gpt|${RA8_REPO_ROOT}/libs/if_ra8_gpt")
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
