/**
 * @file main.c
 * @brief Test for C6 media downloader RPC client.
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stddef.h>
#include <stdint.h>

#include "ra8_arena.h"
#include "ra8_c6link.h"
#include "ra8_c6link_mdl.h"

static uint8_t s_arena_mem[4096];

int main(void)
{
  ra8_arena_t arena = {0};
  (void)ra8_arena_init(&arena, s_arena_mem, sizeof(s_arena_mem));

  /* The c6link handle is initialised by the board-level bring-up;
   * for this compile-only test we just need a placeholder. */
  ra8_c6link_t link = {0};

  (void)ra8_c6link_mdl_download(&link, "http://example.com/file", "/fs/file");

  ra8_mdl_download_progress_t prog = {0};
  (void)ra8_c6link_mdl_poll(&link, &prog);

  (void)ra8_c6link_mdl_cancel(&link);

  return 0;
}
