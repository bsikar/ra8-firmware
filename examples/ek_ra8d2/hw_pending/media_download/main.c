/**
 * @file main.c
 * @brief Example application for media_dl RPC Service.
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>
#include <stdio.h>

#include "ra8_c6link.h"
#include "ra8_c6link_mdl.h"

/* TODO: Define transport and configurations properly. */

int main(void)
{
  ra8_c6link_t link = {};

  /* 1. Initialize c6link. */
  /* ra8_c6link_cfg_t cfg = { ... }; */
  /* ra8_err_t err = ra8_c6link_open(&link, &cfg); */

  /* 2. Request a book download via RPC. */
  const char*       url         = "https://example.com/book.rabook";
  const char*       output_path = "/sd/book.rabook"; /* RA8-local; never sent to the C6. */
  ra8_mdl_session_t session     = {};
  ra8_err_t         err = ra8_c6link_mdl_start(&link, url, k_ra8_mdl_format_rabook, &session);
  if (err != k_ra8_ok) {
    (void)printf("Failed to start download\n");
    return -1;
  }

  /* 3. Show progress on the display and write to SD card. */
  while (session.active) {
    ra8_mdl_chunk_t chunk = {};
    err                   = ra8_c6link_mdl_next(&link, &session, RA8_MDL_CHUNK_DATA_MAX, &chunk);
    if (err == k_ra8_ok) {
      /* TODO: append chunk.data to an RA8 temp file for output_path, verify the
       * final digest, fsync, then atomically rename the temp file. */
      (void)printf("Progress: %llu / %llu bytes -> %s\n",
                   (unsigned long long)(chunk.offset + chunk.data_len),
                   (unsigned long long)chunk.total_bytes,
                   output_path);
    } else {
      (void)ra8_c6link_mdl_cancel(&link, &session);
      return -1;
    }
    /* TODO: Sleep or yield. */
  }

  /* ra8_c6link_close(&link); */
  return 0;
}
