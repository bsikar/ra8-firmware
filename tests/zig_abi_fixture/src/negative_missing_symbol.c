/**
 * @file negative_missing_symbol.c
 * @brief Deliberately unresolved C symbol used by the ABI negative test.
 * @details Must fail linking to prove that a missing Zig export is detected.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra8_abi_fixture.h"

extern ra8_err_t ra8_abi_fixture_missing(void);

int main(void)
{
  return (int)ra8_abi_fixture_missing();
}
