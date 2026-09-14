/**
 * @file negative_layout.c
 * @brief Deliberately incompatible C layout used by the ABI negative test.
 *
 * SPDX-License-Identifier: MIT
 */

#include "ra8_abi_fixture.h"

static_assert(sizeof(ra8_abi_fixture_config_t) == 7U,
              "ABI contract fixture deliberately requires an incompatible layout");
