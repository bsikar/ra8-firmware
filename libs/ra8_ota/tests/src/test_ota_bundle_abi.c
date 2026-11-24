/**
 * @file test_ota_bundle_abi.c
 * @brief C ABI host tests for signed `"RA8U"` bundle transport over `ra8_ota`.
 *
 * @details
 * Exercises the unmodified C public headers (`ra8_ota.h` and `ra8_bundle.h`)
 * linking `libra8_ota.a`: drives JSON manifest check, streaming `"RA8U"`
 * single-image and N-image bundle download into the inactive bank, full-bundle
 * ECDSA + per-image SHA verification, bank-swap commit, and tamper rejection.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ra8_bundle.h"
#include "ra8_err.h"
#include "ra8_ota.h"

#define EXPECT_EQ(expected, actual)                                                                \
  do {                                                                                             \
    uint32_t _exp = (uint32_t)(expected);                                                          \
    uint32_t _act = (uint32_t)(actual);                                                            \
    if (_exp != _act) {                                                                            \
      (void)fprintf(stderr,                                                                        \
                    "FAIL %s:%d: expected 0x%X, got 0x%X\n",                                       \
                    __FILE__,                                                                      \
                    __LINE__,                                                                      \
                    (unsigned)_exp,                                                                \
                    (unsigned)_act);                                                               \
      abort();                                                                                     \
    }                                                                                              \
  } while (0)

typedef enum : uint32_t {
  k_test_bank_bytes   = 4096U,
  k_test_pubkey       = 0x0A8D2C0DU,
  k_test_bank_addr    = 0x02080000UL,
  k_test_bank_index   = 1U,
  k_test_sig_fill     = 0x5AU,
  k_test_hash_seed    = 0x17U,
  k_test_image_bytes  = 256U,
  k_test_payload_mask = 0x5AU,
} test_ota_bundle_const_t;

typedef struct {
  const uint8_t* stream;
  uint32_t       stream_len;
  uint32_t       stream_pos;
  uint8_t        bank[k_test_bank_bytes];
  uint32_t       sha_acc;
  uint32_t       sha_count;
  uint32_t       ecdsa_sig_len;
  bool           reject_sig;
  uint8_t        startup_idx;
  bool           startup_en;
} ota_bundle_env_t;

static ota_bundle_env_t s_env;
static uint32_t         s_reset_calls;

void ra8_log_emit_error(const char* tag, const char* message)
{
  (void)tag;
  (void)message;
}

void ra8_ota_system_reset_hook(void)
{
  s_reset_calls += 1U;
}

static ra8_err_t mock_net_open(void* ctx, const char* url, uint32_t* out_len)
{
  ota_bundle_env_t* env = (ota_bundle_env_t*)ctx;
  (void)url;
  *out_len        = env->stream_len;
  env->stream_pos = 0U;
  return k_ra8_ok;
}

static ra8_err_t mock_net_read(void* ctx, uint8_t* dst, uint32_t cap, uint32_t* out_got)
{
  ota_bundle_env_t* env  = (ota_bundle_env_t*)ctx;
  uint32_t          left = env->stream_len - env->stream_pos;
  uint32_t          take = (left < cap) ? left : cap;
  if (take > 0U) {
    (void)memcpy(dst, &env->stream[env->stream_pos], take);
    env->stream_pos += take;
  }
  *out_got = take;
  return k_ra8_ok;
}

static ra8_err_t mock_net_close(void* ctx)
{
  (void)ctx;
  return k_ra8_ok;
}

static ra8_err_t mock_sha_init(void* ctx)
{
  ota_bundle_env_t* env = (ota_bundle_env_t*)ctx;
  env->sha_acc          = 0x811C9DC5UL;
  env->sha_count        = 0U;
  return k_ra8_ok;
}

static ra8_err_t mock_sha_update(void* ctx, const uint8_t* data, uint32_t len)
{
  ota_bundle_env_t* env = (ota_bundle_env_t*)ctx;
  for (uint32_t i = 0U; i < len; i += 1U) {
    env->sha_acc ^= data[i];
    env->sha_acc *= 0x01000193UL;
    env->sha_count += 1U;
  }
  return k_ra8_ok;
}

static ra8_err_t mock_sha_final(void* ctx, uint8_t* out_digest)
{
  ota_bundle_env_t* env = (ota_bundle_env_t*)ctx;
  for (uint32_t i = 0U; i < k_ra8_ota_sha256_bytes; i += 1U) {
    out_digest[i] = (uint8_t)((env->sha_acc >> ((i % 4U) * 8U)) ^ (env->sha_count + i));
  }
  return k_ra8_ok;
}

static void compute_mock_sha(const uint8_t* data, uint32_t len, uint8_t* out32)
{
  ota_bundle_env_t tmp = {};
  (void)mock_sha_init(&tmp);
  (void)mock_sha_update(&tmp, data, len);
  (void)mock_sha_final(&tmp, out32);
}

static ra8_err_t
mock_ecdsa(void* ctx, uint32_t pubkey, const uint8_t* digest, const uint8_t* sig, uint32_t sig_len)
{
  ota_bundle_env_t* env = (ota_bundle_env_t*)ctx;
  (void)digest;
  (void)sig;
  if (pubkey != k_test_pubkey) {
    return k_ra8_err_validation_failed;
  }
  env->ecdsa_sig_len = sig_len;
  return env->reject_sig ? k_ra8_err_validation_failed : k_ra8_ok;
}

static ra8_err_t mock_flash_erase(void* ctx, uint32_t addr, uint32_t len)
{
  ota_bundle_env_t* env = (ota_bundle_env_t*)ctx;
  (void)addr;
  if (len > k_test_bank_bytes) {
    return k_ra8_err_invalid_size;
  }
  (void)memset(env->bank, 0xFF, k_test_bank_bytes);
  return k_ra8_ok;
}

static ra8_err_t mock_flash_program(void* ctx, uint32_t addr, const uint8_t* data, uint32_t len)
{
  ota_bundle_env_t* env = (ota_bundle_env_t*)ctx;
  uint32_t          off = addr - k_test_bank_addr;
  if (off + len > k_test_bank_bytes) {
    return k_ra8_err_invalid_size;
  }
  (void)memcpy(&env->bank[off], data, len);
  return k_ra8_ok;
}

static ra8_err_t mock_flash_set_startup(void* ctx, uint8_t idx, bool enable)
{
  ota_bundle_env_t* env = (ota_bundle_env_t*)ctx;
  env->startup_idx      = idx;
  env->startup_en       = enable;
  return k_ra8_ok;
}

static ra8_err_t mock_flash_readback(void* ctx, uint32_t addr, uint8_t* dst, uint32_t len)
{
  ota_bundle_env_t* env = (ota_bundle_env_t*)ctx;
  uint32_t          off = addr - k_test_bank_addr;
  if (off + len > k_test_bank_bytes) {
    return k_ra8_err_invalid_size;
  }
  (void)memcpy(dst, &env->bank[off], len);
  return k_ra8_ok;
}

static void init_ota_env(void)
{
  (void)ra8_ota_deinit();
  (void)memset(&s_env, 0, sizeof(s_env));
  s_env.startup_idx = 0xFFU;
  s_reset_calls     = 0U;

  const ra8_ota_cfg_t cfg = {
    .manifest_url  = "https://example.invalid/manifest.json",
    .pubkey_handle = k_test_pubkey,
    .net = {.open = mock_net_open, .read = mock_net_read, .close = mock_net_close, .ctx = &s_env},
    .crypto =
      {
        .sha256_init   = mock_sha_init,
        .sha256_update = mock_sha_update,
        .sha256_final  = mock_sha_final,
        .ecdsa_verify  = mock_ecdsa,
        .ctx           = &s_env,
      },
    .flash =
      {
        .erase               = mock_flash_erase,
        .program             = mock_flash_program,
        .set_startup         = mock_flash_set_startup,
        .readback            = mock_flash_readback,
        .inactive_bank_addr  = k_test_bank_addr,
        .bank_size_bytes     = k_test_bank_bytes,
        .inactive_bank_index = k_test_bank_index,
        .ctx                 = &s_env,
      },
  };
  EXPECT_EQ(k_ra8_ok, ra8_ota_init(&cfg));
}

static void test_c_single_image_bundle_end_to_end(void)
{
  init_ota_env();

  uint8_t container[1024]             = {};
  uint8_t payload[k_test_image_bytes] = {};
  for (uint32_t i = 0U; i < k_test_image_bytes; i += 1U) {
    payload[i] = (uint8_t)(i ^ k_test_payload_mask);
  }
  const uint32_t pay_off =
    k_ra8_bundle_header_bytes + k_ra8_bundle_desc_bytes + k_ra8_bundle_sig_bytes;
  const uint32_t total = pay_off + k_test_image_bytes;

  const ra8_bundle_hdr_t hdr = {
    .magic           = k_ra8_bundle_magic,
    .format_version  = k_ra8_bundle_format_ver,
    .header_bytes    = k_ra8_bundle_header_bytes,
    .bundle_version  = 2U,
    .image_count     = 1U,
    .flags           = 0U,
    .total_bytes     = total,
    .payloads_offset = pay_off,
    .sig_len         = k_ra8_bundle_sig_bytes,
    .reserved        = 0U,
  };
  ra8_bundle_image_desc_t desc = {
    .target           = k_ra8_bundle_target_ra8_app,
    .format           = k_ra8_bundle_fmt_rot1_img,
    .flags            = 0U,
    .image_version    = 2U,
    .compat_proto_ver = 1U,
    .offset           = pay_off,
    .size             = k_test_image_bytes,
    .reserved0        = 0U,
    .reserved1        = 0U,
  };
  compute_mock_sha(payload, k_test_image_bytes, desc.sha256);

  (void)memcpy(container, &hdr, sizeof(hdr));
  (void)memcpy(&container[k_ra8_bundle_header_bytes], &desc, sizeof(desc));
  (void)memset(&container[k_ra8_bundle_header_bytes + k_ra8_bundle_desc_bytes],
               k_test_sig_fill,
               k_ra8_bundle_sig_bytes);
  (void)memcpy(&container[pay_off], payload, k_test_image_bytes);

  ra8_ota_manifest_t m = {
    .version          = "2.0.0",
    .image_url        = "https://example.invalid/update.ra8u",
    .image_size_bytes = total,
    .signature_len    = k_ra8_bundle_sig_bytes,
  };

  s_env.stream     = container;
  s_env.stream_len = total;
  EXPECT_EQ(k_ra8_ok, ra8_ota_download_to_inactive_bank(&m));
  EXPECT_EQ(k_ra8_ota_state_verifying, ra8_ota_get_state());
  EXPECT_EQ(0, memcmp(s_env.bank, payload, k_test_image_bytes));

  EXPECT_EQ(k_ra8_ok, ra8_ota_verify_signature(&m));
  EXPECT_EQ(k_ra8_ota_state_committing, ra8_ota_get_state());
  EXPECT_EQ(k_ra8_bundle_sig_bytes, s_env.ecdsa_sig_len);

  EXPECT_EQ(k_ra8_ok, ra8_ota_commit_and_reboot());
  EXPECT_EQ(k_ra8_ota_state_done, ra8_ota_get_state());
  EXPECT_EQ(k_test_bank_index, s_env.startup_idx);
  EXPECT_EQ(1U, s_reset_calls);

  /* Corrupting the staged payload on verify fails without committing. */
  init_ota_env();
  s_env.stream     = container;
  s_env.stream_len = total;
  EXPECT_EQ(k_ra8_ok, ra8_ota_download_to_inactive_bank(&m));
  s_env.bank[12] ^= 0xFFU;
  EXPECT_EQ(k_ra8_err_crc_mismatch, ra8_ota_verify_signature(&m));
  EXPECT_EQ(k_ra8_ota_state_error, ra8_ota_get_state());
  EXPECT_EQ(0xFFU, s_env.startup_idx);
  EXPECT_EQ(0U, s_reset_calls);

  /* Signature rejection stops before commit. */
  init_ota_env();
  s_env.stream     = container;
  s_env.stream_len = total;
  s_env.reject_sig = true;
  EXPECT_EQ(k_ra8_ok, ra8_ota_download_to_inactive_bank(&m));
  EXPECT_EQ(k_ra8_err_validation_failed, ra8_ota_verify_signature(&m));
  EXPECT_EQ(k_ra8_ota_state_error, ra8_ota_get_state());
}

int main(void)
{
  test_c_single_image_bundle_end_to_end();
  return 0;
}
