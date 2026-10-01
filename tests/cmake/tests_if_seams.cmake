# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
#
# Host-test wiring for the architecture seams in libs/if: the OSAL contract
# (fw_os) and the intent ports the chip HAL hides behind (fw_if_clock,
# fw_if_timer, #693). Each of these needs per-target include paths rather than
# sources, and the rule is the same in every case: a *port's* own vectors get
# only ${FW_ROOT}/libs/if/inc, so they cannot reach a ra8_cgc_* / ra8_gpt_* /
# tx_* header, while an *adapter's* vectors are allowed the chip headers,
# because bridging to them is the adapter's whole job.
#
# Split out of unit_tests.cmake, which the auto-glob and the per-test source
# lists had grown past the file-size gate. The targets themselves still come
# from the ra8_add_test() auto-glob there; this file only decorates them.
#
# Included from tests/CMakeLists.txt, after unit_tests.cmake so the targets
# exist.

# The OSAL conformance test drives the fw_os contract through its first real
# binding. The binding is a host-only, single-threaded implementation kept in
# tests/support so no firmware object library picks it up.
# libs/ra8_wdt_supervisor speaks `fw_os` rather than tx_* since #693, so its
# host tests link the same binding test_fw_os uses. test_..._rtos_err also
# arms one-shot failures through fw_os_host_test_fail_next, which replaced the
# supervisor's own ThreadX shim.
foreach(_ra8_wdt_sup_t test_ra8_wdt_supervisor test_ra8_wdt_supervisor_cov
                       test_ra8_wdt_supervisor_extended test_ra8_wdt_supervisor_rtos_err)
  if(TARGET ${_ra8_wdt_sup_t})
    target_sources(${_ra8_wdt_sup_t} PRIVATE ${FW_ROOT}/tests/support/src/fw_os_host_test.c)
    target_include_directories(
      ${_ra8_wdt_sup_t} PRIVATE ${FW_ROOT}/libs/if/inc ${FW_ROOT}/tests/support/inc
    )
  endif()
endforeach()

if(TARGET test_fw_os)
  target_sources(test_fw_os PRIVATE ${FW_ROOT}/tests/support/src/fw_os_host_test.c)
  target_include_directories(test_fw_os PRIVATE ${FW_ROOT}/libs/if/inc)
endif()

# The clock-intent port has no chip adapter yet (#693 step 1), so its vectors
# drive the facade through a fake binding declared in the test itself. Only the
# interface include directory is added: these vectors must not reach a
# ra8_cgc_* header, which is the whole point of the seam.
if(TARGET test_fw_if_clock)
  target_include_directories(test_fw_if_clock PRIVATE ${FW_ROOT}/libs/if/inc)
  # The facade itself. ra8_add_test links only $<TARGET_OBJECTS:ra8_core_hal>,
  # and core_hal.cmake carries no libs/if sources -- there is no RA8_IF_SOURCES
  # glob and every library glob in library_sources.cmake is named explicitly --
  # so these vectors had no definition of fw_clock_bind to link against. The
  # six fw_clock_* exports ride in this archive since #2791, the same way
  # test_ra8_devcfg and test_ra8_num_decimal reach theirs.
  target_link_libraries(test_fw_if_clock PRIVATE ra8_zig::fw_if_fs)
endif()

# The timer port, the first half of the timer/PWM split (#693 step 5). Same
# deal as the clock port above: no chip adapter yet, so the vectors drive the
# facade through a fake binding they declare themselves, and only the interface
# include directory is added so they cannot reach a ra8_gpt_* header.
if(TARGET test_fw_if_timer)
  target_include_directories(test_fw_if_timer PRIVATE ${FW_ROOT}/libs/if/inc)
endif()

# The PWM port, the second half of that split. Same rule: interface include
# directory only, so its vectors cannot reach a ra8_gpt_* header.
if(TARGET test_fw_if_pwm)
  target_include_directories(test_fw_if_pwm PRIVATE ${FW_ROOT}/libs/if/inc)
endif()

# The RA8 chip adapter for that same port. Unlike the port's own vectors this
# one is *allowed* the chip headers -- bridging to them is its whole job -- and
# it needs the fake MMIO mock, because ra8_mstp polls a hardware bit for
# read-back and there is no real peripheral block on a host.
if(TARGET test_fw_if_clock_ra8)
  target_include_directories(
    test_fw_if_clock_ra8 PRIVATE ${FW_ROOT}/libs/if/inc ${FW_ROOT}/libs/if_ra8_cgc/inc
                                 ${FW_ROOT}/tests/mocks/inc)
  target_sources(test_fw_if_clock_ra8 PRIVATE ${FW_ROOT}/tests/mocks/src/ra8_fake_mmio.c)
endif()

# The board's own answer to that same port: which chip instance each board-level
# module index lands on. It reaches the adapter and therefore ra8_mstp, so it
# wants the same fake MMIO mock for the module-stop read-back.
if(TARGET test_ra8_board_ek_ra8d2_clock_profile)
  target_include_directories(
    test_ra8_board_ek_ra8d2_clock_profile
    PRIVATE ${FW_ROOT}/libs/if/inc ${FW_ROOT}/libs/if_ra8_cgc/inc
            ${FW_ROOT}/libs/ra8_board_ek_ra8d2/inc ${FW_ROOT}/tests/mocks/inc)
  target_sources(test_ra8_board_ek_ra8d2_clock_profile
                 PRIVATE ${FW_ROOT}/tests/mocks/src/ra8_fake_mmio.c)
endif()

# The ThreadX binding of the same seam cannot run on a host -- it needs a
# scheduler -- but its three mapping functions are pure and live in the port
# header on purpose, so the host build proves them without ThreadX. Only the
# port include directory is added: these vectors must not reach a tx_* header.
if(TARGET test_fw_os_threadx)
  target_include_directories(
    test_fw_os_threadx PRIVATE ${FW_ROOT}/libs/if/inc ${FW_ROOT}/port/threadx/inc
  )
endif()

