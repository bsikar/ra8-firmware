/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 *
 * Test-only accessors over the committed .xz fixtures
 * (tests/inc/unarch_xz_fixture.h): the Zig package test reaches the
 * fixture bytes through these, keeping the generated header the single
 * source of truth.
 */
#include <stddef.h>
#include <stdint.h>

#include "unarch_xz_fixture.h"

const uint8_t* unarch_xz_fx_crc64_4k(size_t* len)
{
  if (len != NULL) {
    *len = sizeof(k_fx_xz_crc64_4k);
  }
  return k_fx_xz_crc64_4k;
}

const uint8_t* unarch_xz_fx_crc32(size_t* len)
{
  if (len != NULL) {
    *len = sizeof(k_fx_xz_crc32);
  }
  return k_fx_xz_crc32;
}

const uint8_t* unarch_xz_fx_sha256(size_t* len)
{
  if (len != NULL) {
    *len = sizeof(k_fx_xz_special.sha256);
  }
  return k_fx_xz_special.sha256;
}

const uint8_t* unarch_xz_fx_bigdict(size_t* len)
{
  if (len != NULL) {
    *len = sizeof(k_fx_xz_special.bigdict);
  }
  return k_fx_xz_special.bigdict;
}

const uint8_t* unarch_xz_fx_bomb_1m(size_t* len)
{
  if (len != NULL) {
    *len = sizeof(k_fx_xz_special.bomb_1m);
  }
  return k_fx_xz_special.bomb_1m;
}
