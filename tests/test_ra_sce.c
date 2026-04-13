/**
 * @file test_ra_sce.c
 * @brief Unit tests for ra_sce.c (Secure Cryptographic Engine driver)
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <string.h>

#include "ra8d2_sce_regs.h"
#include "ra_err.h"
#include "ra_sce.h"
#include "ra_sim_mmap.h"
#include "unity_minimal.h"

typedef enum : uint8_t {
  k_ra_sce_test_block_bytes = 16U,
} ra_sce_test_const_t;

static const uint8_t s_key[16]   = {0x2BU,
                                    0x7EU,
                                    0x15U,
                                    0x16U,
                                    0x28U,
                                    0xAEU,
                                    0xD2U,
                                    0xA6U,
                                    0xABU,
                                    0xF7U,
                                    0x15U,
                                    0x88U,
                                    0x09U,
                                    0xCFU,
                                    0x4FU,
                                    0x3CU};
static const uint8_t s_plain[16] = {0x6BU,
                                    0xC1U,
                                    0xBEU,
                                    0xE2U,
                                    0x2EU,
                                    0x40U,
                                    0x9FU,
                                    0x96U,
                                    0xE9U,
                                    0x3DU,
                                    0x7EU,
                                    0x11U,
                                    0x73U,
                                    0x93U,
                                    0x17U,
                                    0x2AU};

static void test_init_enables_engine(void)
{
  TEST_BEGIN("sce init enables engine");
  ra_sim_mmap_reset();

  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_sce_init());
  volatile r_sce_regs_t* reg = ra_sce();
  TEST_ASSERT_EQ((int)k_ra_sce_mask_en, (int)(reg->CR & (uint32_t)k_ra_sce_mask_en));
  TEST_END("sce init enables engine");
}

static void test_aes128_encrypt_happy(void)
{
  TEST_BEGIN("sce aes128 encrypt happy");
  ra_sim_mmap_reset();

  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_sce_init());

  uint8_t cipher[16] = {};
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_sce_aes128_encrypt(s_key, s_plain, cipher));

  /* Simulator path copies plaintext into ciphertext. */
  TEST_ASSERT_EQ(0, memcmp(cipher, s_plain, (size_t)k_ra_sce_test_block_bytes));

  volatile r_sce_regs_t* reg = ra_sce();
  TEST_ASSERT_EQ((int)k_ra_sce_cmd_aes128_encrypt, (int)reg->CMD);
  TEST_END("sce aes128 encrypt happy");
}

static void test_aes128_encrypt_null_key(void)
{
  TEST_BEGIN("sce aes128 encrypt null key");
  ra_sim_mmap_reset();

  uint8_t cipher[16] = {};
  TEST_ASSERT_EQ((int)k_ra_err_null_ptr, (int)ra_sce_aes128_encrypt(nullptr, s_plain, cipher));
  TEST_END("sce aes128 encrypt null key");
}

static void test_aes128_encrypt_null_plain(void)
{
  TEST_BEGIN("sce aes128 encrypt null plain");
  ra_sim_mmap_reset();

  uint8_t cipher[16] = {};
  TEST_ASSERT_EQ((int)k_ra_err_null_ptr, (int)ra_sce_aes128_encrypt(s_key, nullptr, cipher));
  TEST_END("sce aes128 encrypt null plain");
}

static void test_aes128_encrypt_null_cipher(void)
{
  TEST_BEGIN("sce aes128 encrypt null cipher");
  ra_sim_mmap_reset();

  TEST_ASSERT_EQ((int)k_ra_err_null_ptr, (int)ra_sce_aes128_encrypt(s_key, s_plain, nullptr));
  TEST_END("sce aes128 encrypt null cipher");
}

int32_t main(void)
{
  test_init_enables_engine();
  test_aes128_encrypt_happy();
  test_aes128_encrypt_null_key();
  test_aes128_encrypt_null_plain();
  test_aes128_encrypt_null_cipher();
  (void)fprintf(stderr, "[OK  ] test_ra_sce.c\n");
  return 0;
}
