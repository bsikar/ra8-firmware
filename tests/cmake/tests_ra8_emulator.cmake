# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
#
# ra8_emulator peripheral-model host tests.
#
# test_ra8_emulator_mstp_gate (#405): the module-stop gate table. ra8_emulator now
# consults MSTPCRA..E state before answering an MMIO access to an owning
# peripheral block, so a peripheral the firmware never ungated is inert in the
# emulator exactly as on silicon. The engine-free half of that model
# (tools/ra8_emulator/src/periph/board_periph_mstp_model.c) takes NO Unicorn dependency by
# design, so it is compiled straight into this host executable and its
# address->module-stop-bit table is exercised directly. It links no ra8_core_hal
# (the model is self-contained: stdint + ra8_mstp_regs.h + ra8_attributes.h), so
# it is registered by hand here rather than through the ra8_add_test() auto-glob,
# which would splice in the shared object library (and its C++ TUs) it does not
# need. It is REMOVE_ITEM-ed from the auto-glob in unit_tests.cmake.
#
# Included from tests/CMakeLists.txt. CMake include() is textual within the same
# directory scope, so FW_ROOT (from library_sources.cmake) is visible here.

add_executable(
  test_ra8_emulator_mstp_gate ${CMAKE_CURRENT_SOURCE_DIR}/misc/src/test_ra8_emulator_mstp_gate.c
                              ${FW_ROOT}/tools/ra8_emulator/src/periph/board_periph_mstp_model.c
)
target_compile_options(test_ra8_emulator_mstp_gate PRIVATE -Wall -Wextra -Werror)
target_include_directories(
  test_ra8_emulator_mstp_gate PRIVATE ${RA8_TEST_SHARED_INCLUDE_DIRS} ${FW_ROOT}/libs/ra8_core/inc
                                      ${FW_ROOT}/libs/ra8_hal/inc ${FW_ROOT}/tools/ra8_emulator/inc
)
add_test(NAME test_ra8_emulator_mstp_gate COMMAND test_ra8_emulator_mstp_gate)
