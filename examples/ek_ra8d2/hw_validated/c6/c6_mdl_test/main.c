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
#include "ra8_c6link_mdl.h"

static ra8_err_t dummy_transfer(void* ctx, const uint8_t* tx, uint8_t* rx, uint16_t len)
{
  (void)ctx;
  (void)tx;
  (void)rx;
  (void)len;
  return k_ra8_ok;
}

static bool dummy_handshake(void* ctx)
{
  (void)ctx;
  return true;
}

static void dummy_delay(void* ctx, uint16_t ms)
{
  (void)ctx;
  (void)ms;
}

static uint8_t s_arena_mem[4096];

int main(void)
{
  ra8_c6link_transport_t trans;
  trans.transfer         = dummy_transfer;
  trans.handshake_active = dummy_handshake;
  trans.delay_ms         = dummy_delay;
  trans.ctx              = NULL;

  ra8_c6link_mdl_init(trans);

  ra8_arena_t arena = {0};
  (void)ra8_arena_init(&arena, s_arena_mem, sizeof(s_arena_mem));

  (void)ra8_c6link_mdl_download(&arena, "http://example.com/file", "/fs/file");

  ra8_mdl_download_progress_t prog = {0};
  (void)ra8_c6link_mdl_poll(&arena, &prog);

  (void)ra8_c6link_mdl_cancel();

  return 0;
}
