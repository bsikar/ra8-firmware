/**
 * @file src/main.c
 * @brief macOS Host App: reg_gen
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdio.h>

#include "reg_gen.h"

int main(int argc, char** argv)
{
  (void)argc;
  (void)argv;
  reg_gen_init();
  printf("Hello from reg_gen!\n");
  return 0;
}
