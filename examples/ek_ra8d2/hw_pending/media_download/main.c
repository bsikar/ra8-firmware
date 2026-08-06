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
  ra8_c6link_t link = {0};

  /* 1. Initialize c6link. */
  /* ra8_c6link_cfg_t cfg = { ... }; */
  /* ra8_err_t err = ra8_c6link_open(&link, &cfg); */

  /* 2. Request a book download via RPC. */
  const char* url         = "https://example.com/book.rabook";
  const char* output_path = "/sd/book.rabook";
  ra8_err_t   err         = ra8_c6link_mdl_download(&link, url, output_path);
  if (err != k_ra8_ok) {
    (void)printf("Failed to start download\n");
    return -1;
  }

  /* 3. Show progress on the display and write to SD card. */
  ra8_mdl_download_progress_t progress = {0};
  while (progress.status == 0U) {
    err = ra8_c6link_mdl_poll(&link, &progress);
    if (err == k_ra8_ok) {
      (void)printf("Progress: %u / %u bytes\n",
                   (unsigned)progress.bytes_received,
                   (unsigned)progress.total_bytes);
    }
    /* TODO: Sleep or yield. */
  }

  /* ra8_c6link_close(&link); */
  return 0;
}
