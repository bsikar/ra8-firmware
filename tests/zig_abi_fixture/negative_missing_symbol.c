/**
 * @file negative_missing_symbol.c
 * @brief Deliberately unresolved C symbol used by the ABI negative test.
 *
 * SPDX-License-Identifier: MIT
 */

#include "ra8_abi_fixture.h"

extern ra8_err_t ra8_abi_fixture_missing(void);

int main(void)
{
  return (int)ra8_abi_fixture_missing();
}
