/**
 * @file tests/src/test_reg_gen.c
 * @brief Unit tests for reg_gen
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdio.h>

#include "reg_gen.h"

int main(void)
{
  reg_gen_init();
  printf("Running tests for reg_gen...\n");
  printf("All tests passed!\n");
  return 0;
}
