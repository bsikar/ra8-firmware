/**
 * @file test_update_abi.c
 * @brief Host C ABI acceptance tests for the shared `ra8_update` pipeline seam
 *
 * @details
 * Exercises ::ra8_update_init, ::ra8_update_stage_begin, ::ra8_update_stage_write,
 * ::ra8_update_stage_finish, ::ra8_update_verify, ::ra8_update_commit,
 * ::ra8_update_confirm, and ::ra8_update_rollback across both canonical OTA
 * (immediate commit + immediate floor advance) and canonical DFU (trial commit
 * with deferred floor advance until confirm, plus rollback on failure).
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "ra8_bundle.h"
#include "ra8_err.h"
#include "ra8_reconcile.h"
#include "ra8_update.h"

typedef enum : uint32_t {
  k_test_buf_bytes    = 512U,
  k_test_scratch_size = 64U,
  k_test_pubkey       = 0xCAFEBABEU,
} test_const_t;

typedef struct {
  uint8_t   storage[k_test_buf_bytes];
  uint32_t  storage_len;
  uint32_t  floor;
  uint32_t  prepare_calls;
  uint32_t  commit_calls;
  uint32_t  confirm_calls;
  uint32_t  rollback_calls;
  uint32_t  commit_floor_calls;
  bool      last_commit_is_trial;
  uint8_t   last_trial_budget;
  ra8_err_t last_rollback_reason;
  bool      force_sig_fail;
  uint8_t   hash_acc[k_ra8_update_sha256_bytes];
  uint32_t  hash_pos;
} test_env_t;

static test_env_t s_env  = {};
static uint32_t   s_fail = 0U;

#define EXPECT_TRUE(cond)                                                                          \
  do {                                                                                             \
    if (!(cond)) {                                                                                 \
      ++s_fail;                                                                                    \
    }                                                                                              \
  } while (0)

#define EXPECT_EQ(exp, act)                                                                        \
  do {                                                                                             \
    const uint64_t e_ = (uint64_t)(exp);                                                           \
    const uint64_t a_ = (uint64_t)(act);                                                           \
    if (e_ != a_) {                                                                                \
      ++s_fail;                                                                                    \
    }                                                                                              \
  } while (0)

static void env_reset(uint32_t initial_floor)
{
  (void)memset(&s_env, 0, sizeof(s_env));
  s_env.floor = initial_floor;
}

static ra8_err_t mock_prepare(void* ctx, uint32_t hint)
{
  test_env_t* env = (test_env_t*)ctx;
  if (hint > k_test_buf_bytes) {
    return k_ra8_err_invalid_size;
  }
  (void)memset(env->storage, 0xFF, hint);
  env->storage_len = hint;
  env->prepare_calls++;
  return k_ra8_ok;
}

static ra8_err_t mock_write(void* ctx, uint32_t off, const uint8_t* data, uint32_t len)
{
  test_env_t* env = (test_env_t*)ctx;
  (void)memcpy(&env->storage[off], data, len);
  return k_ra8_ok;
}

static ra8_err_t mock_readback(void* ctx, uint32_t off, uint8_t* out, uint32_t len)
{
  test_env_t* env = (test_env_t*)ctx;
  (void)memcpy(out, &env->storage[off], len);
  return k_ra8_ok;
}

static ra8_err_t mock_sha_init(void* ctx)
{
  test_env_t* env = (test_env_t*)ctx;
  (void)memset(env->hash_acc, 0x5A, sizeof(env->hash_acc));
  env->hash_pos = 0U;
  return k_ra8_ok;
}

static ra8_err_t mock_sha_update(void* ctx, const uint8_t* data, uint32_t len)
{
  test_env_t* env = (test_env_t*)ctx;
  for (uint32_t i = 0U; i < len; ++i) {
    const uint32_t idx = env->hash_pos % k_ra8_update_sha256_bytes;
    env->hash_acc[idx] = (uint8_t)((env->hash_acc[idx] * 33U) ^ data[i]);
    env->hash_pos++;
  }
  return k_ra8_ok;
}

static ra8_err_t mock_sha_final(void* ctx, uint8_t out[k_ra8_update_sha256_bytes])
{
  test_env_t* env = (test_env_t*)ctx;
  (void)memcpy(out, env->hash_acc, k_ra8_update_sha256_bytes);
  return k_ra8_ok;
}

static ra8_err_t mock_ecdsa_verify(void*          ctx,
                                   uint32_t       pubkey,
                                   const uint8_t  digest[k_ra8_update_sha256_bytes],
                                   const uint8_t* sig,
                                   uint32_t       sig_len)
{
  test_env_t* env = (test_env_t*)ctx;
  if (env->force_sig_fail || pubkey != (uint32_t)k_test_pubkey ||
      sig_len < k_ra8_update_sha256_bytes) {
    return k_ra8_err_validation_failed;
  }
  return (memcmp(digest, sig, k_ra8_update_sha256_bytes) == 0) ? k_ra8_ok
                                                               : k_ra8_err_validation_failed;
}

static ra8_err_t mock_read_floor(void* ctx, uint32_t* out_ver)
{
  *out_ver = ((test_env_t*)ctx)->floor;
  return k_ra8_ok;
}

static ra8_err_t mock_commit_floor(void* ctx, uint32_t new_ver)
{
  test_env_t* env = (test_env_t*)ctx;
  env->commit_floor_calls++;
  if (new_ver > env->floor) {
    env->floor = new_ver;
  }
  return k_ra8_ok;
}

static ra8_err_t
mock_commit(void* ctx, uint32_t bytes, uint32_t seq_or_bank, bool is_trial, uint8_t trial_budget)
{
  (void)bytes;
  (void)seq_or_bank;
  test_env_t* env           = (test_env_t*)ctx;
  env->last_commit_is_trial = is_trial;
  env->last_trial_budget    = trial_budget;
  env->commit_calls++;
  return k_ra8_ok;
}

static ra8_err_t mock_confirm(void* ctx)
{
  ((test_env_t*)ctx)->confirm_calls++;
  return k_ra8_ok;
}

static ra8_err_t mock_rollback(void* ctx, ra8_err_t reason)
{
  test_env_t* env = (test_env_t*)ctx;
  env->rollback_calls++;
  env->last_rollback_reason = reason;
  return k_ra8_ok;
}

static ra8_update_cfg_t make_cfg(ra8_update_transport_t t)
{
  ra8_update_pipeline_decl_t decl = {};
  EXPECT_EQ(k_ra8_ok, ra8_update_decl_canonical(t, &decl));
  return (ra8_update_cfg_t){
    .decl          = decl,
    .pubkey_handle = (uint32_t)k_test_pubkey,
    .align_bytes   = k_ra8_update_page_bytes,
    .max_bytes     = k_test_buf_bytes,
    .reserved0     = 0U,
    .storage       = {.prepare  = mock_prepare,
                      .write    = mock_write,
                      .readback = mock_readback,
                      .ctx      = &s_env},
    .crypto        = {.sha256_init   = mock_sha_init,
                      .sha256_update = mock_sha_update,
                      .sha256_final  = mock_sha_final,
                      .ecdsa_verify  = mock_ecdsa_verify,
                      .ctx           = &s_env},
    .rollback = {.read_floor = mock_read_floor, .commit_floor = mock_commit_floor, .ctx = &s_env},
    .commit   = {.commit   = mock_commit,
                 .confirm  = mock_confirm,
                 .rollback = mock_rollback,
                 .ctx      = &s_env},
  };
}

static void compute_expected_digest(const uint8_t* buf,
                                    uint32_t       len,
                                    const uint8_t* meta,
                                    uint32_t       meta_len,
                                    uint8_t        out_body[k_ra8_update_sha256_bytes],
                                    uint8_t        out_sig[64])
{
  (void)mock_sha_init(&s_env);
  (void)mock_sha_update(&s_env, buf, len);
  (void)mock_sha_final(&s_env, out_body);

  uint8_t bound[k_ra8_update_sha256_bytes] = {};
  if (meta_len > 0U) {
    (void)mock_sha_init(&s_env);
    (void)mock_sha_update(&s_env, meta, meta_len);
    (void)mock_sha_update(&s_env, out_body, k_ra8_update_sha256_bytes);
    (void)mock_sha_final(&s_env, bound);
  } else {
    (void)memcpy(bound, out_body, sizeof(bound));
  }
  (void)memset(out_sig, 0, 64U);
  (void)memcpy(out_sig, bound, sizeof(bound));
}

static void test_ota_immediate_flow(void)
{
  env_reset(4U);
  ra8_update_cfg_t     cfg  = make_cfg(k_ra8_update_transport_ota);
  ra8_update_session_t sess = {};
  EXPECT_EQ(k_ra8_ok, ra8_update_init(&sess, &cfg));

  uint8_t payload[128] = {};
  for (uint32_t i = 0U; i < sizeof(payload); ++i) {
    payload[i] = (uint8_t)(i + 1U);
  }
  const uint8_t meta[8]                                = {1U, 2U, 3U, 4U, 5U, 6U, 7U, 8U};
  uint8_t       body_digest[k_ra8_update_sha256_bytes] = {};
  uint8_t       sig[64]                                = {};
  compute_expected_digest(payload, sizeof(payload), meta, sizeof(meta), body_digest, sig);

  EXPECT_EQ(k_ra8_ok, ra8_update_stage_begin(&sess, sizeof(payload)));
  EXPECT_EQ(k_ra8_ok, ra8_update_stage_write(&sess, 0U, payload, 64U));
  EXPECT_EQ(k_ra8_ok, ra8_update_stage_write(&sess, 64U, &payload[64], 64U));
  EXPECT_EQ(k_ra8_ok, ra8_update_stage_finish(&sess));

  const ra8_update_verify_req_t req = {
    .expected_version = 6U,
    .sig_len          = 64U,
    .meta_prefix_len  = sizeof(meta),
    .reserved0        = 0U,
    .meta_prefix      = meta,
    .expected_sha256  = body_digest,
    .sig              = sig,
  };
  uint8_t scratch[k_test_scratch_size] = {};
  EXPECT_EQ(k_ra8_ok, ra8_update_verify(&sess, &req, scratch, sizeof(scratch)));
  EXPECT_EQ(k_ra8_update_stage_verified, sess.stage);
  EXPECT_EQ(0U, s_env.commit_floor_calls);

  EXPECT_EQ(k_ra8_ok, ra8_update_commit(&sess, 1U));
  EXPECT_EQ(k_ra8_update_stage_confirmed, sess.stage);
  EXPECT_TRUE(!s_env.last_commit_is_trial);
  EXPECT_EQ(6U, s_env.floor);
  EXPECT_EQ(1U, s_env.commit_floor_calls);
  EXPECT_TRUE(ra8_update_stage_is_terminal(sess.stage));
}

static void test_dfu_trial_defers_floor_and_rolls_back_on_bad_verify(void)
{
  env_reset(7U);
  ra8_update_cfg_t     cfg  = make_cfg(k_ra8_update_transport_dfu);
  ra8_update_session_t sess = {};
  EXPECT_EQ(k_ra8_ok, ra8_update_init(&sess, &cfg));

  uint8_t payload[128] = {};
  (void)memset(payload, 0xA5, sizeof(payload));
  uint8_t body_digest[k_ra8_update_sha256_bytes] = {};
  uint8_t sig[64]                                = {};
  compute_expected_digest(payload, sizeof(payload), nullptr, 0U, body_digest, sig);

  EXPECT_EQ(k_ra8_ok, ra8_update_stage_begin(&sess, sizeof(payload)));
  EXPECT_EQ(k_ra8_ok, ra8_update_stage_write(&sess, 0U, payload, sizeof(payload)));
  EXPECT_EQ(k_ra8_ok, ra8_update_stage_finish(&sess));

  const ra8_update_verify_req_t req = {
    .expected_version = 8U,
    .sig_len          = 64U,
    .meta_prefix_len  = 0U,
    .reserved0        = 0U,
    .meta_prefix      = nullptr,
    .expected_sha256  = body_digest,
    .sig              = sig,
  };
  uint8_t scratch[k_test_scratch_size] = {};
  EXPECT_EQ(k_ra8_ok, ra8_update_verify(&sess, &req, scratch, sizeof(scratch)));
  EXPECT_EQ(k_ra8_ok, ra8_update_commit(&sess, 2U));
  EXPECT_EQ(k_ra8_update_stage_trial_pending, sess.stage);
  EXPECT_TRUE(s_env.last_commit_is_trial);
  EXPECT_EQ(3U, s_env.last_trial_budget);
  /* Anti-rollback floor MUST remain at 7 while trial is unconfirmed. */
  EXPECT_EQ(7U, s_env.floor);
  EXPECT_EQ(0U, s_env.commit_floor_calls);

  /* Confirmation advances the floor cleanly and is idempotent. */
  EXPECT_EQ(k_ra8_ok, ra8_update_confirm(&sess));
  EXPECT_EQ(k_ra8_update_stage_confirmed, sess.stage);
  EXPECT_EQ(8U, s_env.floor);
  EXPECT_EQ(1U, s_env.confirm_calls);
  EXPECT_EQ(k_ra8_ok, ra8_update_confirm(&sess));
  EXPECT_EQ(1U, s_env.confirm_calls);

  /* Tampered digest rolls back without touching commit. */
  body_digest[0] ^= 0xFFU;
  EXPECT_EQ(k_ra8_ok, ra8_update_stage_begin(&sess, sizeof(payload)));
  EXPECT_EQ(k_ra8_ok, ra8_update_stage_write(&sess, 0U, payload, sizeof(payload)));
  EXPECT_EQ(k_ra8_ok, ra8_update_stage_finish(&sess));
  const uint32_t prior_commits = s_env.commit_calls;
  EXPECT_EQ(k_ra8_err_crc_mismatch, ra8_update_verify(&sess, &req, scratch, sizeof(scratch)));
  EXPECT_EQ(k_ra8_update_stage_rolled_back, sess.stage);
  EXPECT_EQ(prior_commits, s_env.commit_calls);
  EXPECT_EQ(1U, s_env.rollback_calls);
  EXPECT_EQ(k_ra8_err_invalid_state, ra8_update_commit(&sess, 3U));
}

static void test_guards_downgrade_and_explicit_rollback(void)
{
  env_reset(9U);
  ra8_update_cfg_t     cfg  = make_cfg(k_ra8_update_transport_dfu);
  ra8_update_session_t sess = {};
  EXPECT_EQ(k_ra8_err_null_ptr, ra8_update_init(nullptr, &cfg));
  EXPECT_EQ(k_ra8_err_null_ptr, ra8_update_init(&sess, nullptr));
  cfg.storage.write = nullptr;
  EXPECT_EQ(k_ra8_err_null_ptr, ra8_update_init(&sess, &cfg));

  cfg = make_cfg(k_ra8_update_transport_dfu);
  EXPECT_EQ(k_ra8_ok, ra8_update_init(&sess, &cfg));

  uint8_t payload[64] = {};
  (void)memset(payload, 0x3C, sizeof(payload));
  uint8_t body_digest[k_ra8_update_sha256_bytes] = {};
  uint8_t sig[64]                                = {};
  compute_expected_digest(payload, sizeof(payload), nullptr, 0U, body_digest, sig);

  /* Downgrade (version 8 < stored floor 9) must roll back cleanly. */
  EXPECT_EQ(k_ra8_ok, ra8_update_stage_begin(&sess, sizeof(payload)));
  EXPECT_EQ(k_ra8_ok, ra8_update_stage_write(&sess, 0U, payload, sizeof(payload)));
  EXPECT_EQ(k_ra8_ok, ra8_update_stage_finish(&sess));
  ra8_update_verify_req_t req = {
    .expected_version = 8U,
    .sig_len          = 64U,
    .meta_prefix_len  = 0U,
    .reserved0        = 0U,
    .meta_prefix      = nullptr,
    .expected_sha256  = body_digest,
    .sig              = sig,
  };
  uint8_t scratch[k_test_scratch_size] = {};
  EXPECT_EQ(k_ra8_err_validation_failed, ra8_update_verify(&sess, &req, scratch, sizeof(scratch)));
  EXPECT_EQ(k_ra8_update_stage_rolled_back, sess.stage);
  EXPECT_EQ(k_ra8_err_validation_failed, s_env.last_rollback_reason);

  /* Signature verification failure also denies and rolls back. */
  s_env.force_sig_fail = true;
  req.expected_version = 10U;
  EXPECT_EQ(k_ra8_ok, ra8_update_stage_begin(&sess, sizeof(payload)));
  EXPECT_EQ(k_ra8_ok, ra8_update_stage_write(&sess, 0U, payload, sizeof(payload)));
  EXPECT_EQ(k_ra8_ok, ra8_update_stage_finish(&sess));
  EXPECT_EQ(k_ra8_err_validation_failed, ra8_update_verify(&sess, &req, scratch, sizeof(scratch)));
  EXPECT_EQ(k_ra8_update_stage_rolled_back, sess.stage);
  s_env.force_sig_fail = false;

  /* Explicit rollback normalizes k_ra8_ok -> k_ra8_fail and clears verified. */
  EXPECT_EQ(k_ra8_err_cancelled, ra8_update_rollback(&sess, k_ra8_err_cancelled));
  EXPECT_EQ(k_ra8_fail, ra8_update_rollback(&sess, k_ra8_ok));
}

static void sign_bundle(uint8_t* container, uint16_t image_count, uint32_t meta_bytes)
{
  const uint32_t sig_off = (uint32_t)k_ra8_bundle_header_bytes +
                           ((uint32_t)image_count * (uint32_t)k_ra8_bundle_desc_bytes);
  (void)meta_bytes;
  uint8_t meta_digest[k_ra8_update_sha256_bytes] = {};
  (void)mock_sha_init(&s_env);
  (void)mock_sha_update(&s_env, container, sig_off);
  (void)mock_sha_final(&s_env, meta_digest);
  (void)memset(&container[sig_off], 0, k_ra8_bundle_sig_bytes);
  (void)memcpy(&container[sig_off], meta_digest, sizeof(meta_digest));
}

static void test_ra8u_bundle_pipeline_single_and_multi(void)
{
  env_reset(5U);
  uint8_t        container[k_test_buf_bytes] = {};
  const uint32_t pay_off =
    k_ra8_bundle_header_bytes + (2U * k_ra8_bundle_desc_bytes) + k_ra8_bundle_sig_bytes;
  const uint32_t   total = pay_off + 96U;
  ra8_bundle_hdr_t hdr   = {
    .magic           = k_ra8_bundle_magic,
    .format_version  = k_ra8_bundle_format_ver,
    .header_bytes    = k_ra8_bundle_header_bytes,
    .bundle_version  = 11U,
    .image_count     = 2U,
    .flags           = 0U,
    .total_bytes     = total,
    .payloads_offset = pay_off,
    .sig_len         = k_ra8_bundle_sig_bytes,
    .reserved        = 0U,
  };
  (void)memcpy(container, &hdr, sizeof(hdr));
  (void)memset(&container[pay_off], 0x11, 48U);
  (void)memset(&container[pay_off + 64U], 0x22, 32U);

  ra8_bundle_image_desc_t descs[2] = {};
  descs[0].target                  = k_ra8_bundle_target_ra8_app;
  descs[0].format                  = k_ra8_bundle_fmt_rot1_img;
  descs[0].image_version           = 11U;
  descs[0].compat_proto_ver        = 4U;
  descs[0].offset                  = pay_off;
  descs[0].size                    = 48U;
  (void)mock_sha_init(&s_env);
  (void)mock_sha_update(&s_env, &container[descs[0].offset], descs[0].size);
  (void)mock_sha_final(&s_env, descs[0].sha256);

  descs[1].target           = k_ra8_bundle_target_esp32_c6;
  descs[1].format           = k_ra8_bundle_fmt_c6_img;
  descs[1].image_version    = 11U;
  descs[1].compat_proto_ver = 4U;
  descs[1].offset           = pay_off + 64U;
  descs[1].size             = 32U;
  (void)mock_sha_init(&s_env);
  (void)mock_sha_update(&s_env, &container[descs[1].offset], descs[1].size);
  (void)mock_sha_final(&s_env, descs[1].sha256);

  (void)memcpy(&container[k_ra8_bundle_header_bytes], descs, sizeof(descs));
  sign_bundle(container, 2U, pay_off);

  const ra8_update_crypto_iface_t crypto = {
    .sha256_init   = mock_sha_init,
    .sha256_update = mock_sha_update,
    .sha256_final  = mock_sha_final,
    .ecdsa_verify  = mock_ecdsa_verify,
    .ctx           = &s_env,
  };
  ra8_bundle_hdr_t        out_hdr                      = {};
  ra8_bundle_image_desc_t out_descs[4]                 = {};
  uint8_t                 scratch[k_test_scratch_size] = {};
  EXPECT_EQ(k_ra8_ok,
            ra8_bundle_verify_mem(container,
                                  total,
                                  (uint32_t)k_test_pubkey,
                                  5U,
                                  &crypto,
                                  scratch,
                                  sizeof(scratch),
                                  &out_hdr,
                                  out_descs,
                                  4U));
  ra8_bundle_image_desc_t ra8_hit = {};
  EXPECT_EQ(
    k_ra8_ok,
    ra8_bundle_find_target(out_descs, out_hdr.image_count, k_ra8_bundle_target_ra8_app, &ra8_hit));
  EXPECT_EQ(pay_off, ra8_hit.offset);

  /* Drive the full DFU trial pipeline on the multi-image RA8U bundle. */
  ra8_update_cfg_t     cfg  = make_cfg(k_ra8_update_transport_dfu);
  ra8_update_session_t sess = {};
  EXPECT_EQ(k_ra8_ok, ra8_update_init(&sess, &cfg));
  EXPECT_EQ(k_ra8_ok, ra8_update_stage_begin(&sess, total));
  EXPECT_EQ(k_ra8_ok, ra8_update_stage_write(&sess, 0U, container, total));
  EXPECT_EQ(k_ra8_ok, ra8_update_stage_finish(&sess));
  EXPECT_EQ(k_ra8_ok,
            ra8_update_verify_bundle(&sess, 4U, scratch, sizeof(scratch), &out_hdr, out_descs, 4U));
  EXPECT_EQ(k_ra8_ok, ra8_update_commit(&sess, 9U));
  EXPECT_EQ(k_ra8_update_stage_trial_pending, sess.stage);
  EXPECT_EQ(5U, s_env.floor);
  EXPECT_EQ(k_ra8_ok, ra8_update_confirm(&sess));
  EXPECT_EQ(11U, s_env.floor);

  /* Companion protocol mismatch rolls back cleanly before commit. */
  EXPECT_EQ(k_ra8_ok, ra8_update_stage_begin(&sess, total));
  EXPECT_EQ(k_ra8_ok, ra8_update_stage_write(&sess, 0U, container, total));
  EXPECT_EQ(k_ra8_ok, ra8_update_stage_finish(&sess));
  EXPECT_EQ(
    k_ra8_err_validation_failed,
    ra8_update_verify_bundle(&sess, 99U, scratch, sizeof(scratch), &out_hdr, out_descs, 4U));
  EXPECT_EQ(k_ra8_update_stage_rolled_back, sess.stage);

  /* Tampered payload byte fails per-image SHA-256 and rolls back. */
  container[pay_off] ^= 0x80U;
  EXPECT_EQ(k_ra8_ok, ra8_update_stage_begin(&sess, total));
  EXPECT_EQ(k_ra8_ok, ra8_update_stage_write(&sess, 0U, container, total));
  EXPECT_EQ(k_ra8_ok, ra8_update_stage_finish(&sess));
  EXPECT_EQ(k_ra8_err_crc_mismatch,
            ra8_update_verify_bundle(&sess, 4U, scratch, sizeof(scratch), &out_hdr, out_descs, 4U));
  EXPECT_EQ(k_ra8_update_stage_rolled_back, sess.stage);
}

typedef struct {
  bool                     has_nv;
  ra8_reconcile_nv_state_t nv;
  uint32_t                 c6_ver;
  uint32_t                 disp_ver;
  uint32_t                 c6_applies;
  uint32_t                 disp_applies;
  uint32_t                 safe_calls;
  uint32_t                 last_delay;
  bool                     cut_after_c6_flash;
  bool                     c6_stuck;
} c_reconcile_ctx_t;

static ra8_err_t c_rec_nv_load(void* ctx, ra8_reconcile_nv_state_t* out_state)
{
  c_reconcile_ctx_t* r = (c_reconcile_ctx_t*)ctx;
  if (!r->has_nv) {
    return k_ra8_err_not_found;
  }
  *out_state = r->nv;
  return k_ra8_ok;
}

static ra8_err_t c_rec_nv_save(void* ctx, const ra8_reconcile_nv_state_t* state)
{
  c_reconcile_ctx_t* r = (c_reconcile_ctx_t*)ctx;
  r->nv                = *state;
  r->has_nv            = true;
  return k_ra8_ok;
}

static ra8_err_t
c_rec_query(void* ctx, ra8_bundle_target_t target, uint32_t* out_ver, uint32_t* out_proto)
{
  c_reconcile_ctx_t* r = (c_reconcile_ctx_t*)ctx;
  if (out_proto != nullptr) {
    *out_proto = 4U;
  }
  if (target == k_ra8_bundle_target_esp32_c6) {
    *out_ver = r->c6_ver;
    return k_ra8_ok;
  }
  if (target == k_ra8_bundle_target_display) {
    *out_ver = r->disp_ver;
    return k_ra8_ok;
  }
  return k_ra8_err_not_found;
}

static ra8_err_t c_rec_apply(void* ctx, const ra8_bundle_image_desc_t* desc)
{
  c_reconcile_ctx_t* r = (c_reconcile_ctx_t*)ctx;
  if (desc->target == k_ra8_bundle_target_esp32_c6) {
    ++r->c6_applies;
    if (r->c6_stuck) {
      return k_ra8_err_hw_error;
    }
    r->c6_ver = desc->image_version;
    if (r->cut_after_c6_flash) {
      r->cut_after_c6_flash = false;
      return k_ra8_err_hw_error;
    }
    return k_ra8_ok;
  }
  if (desc->target == k_ra8_bundle_target_display) {
    ++r->disp_applies;
    r->disp_ver = desc->image_version;
    return k_ra8_ok;
  }
  return k_ra8_err_invalid_arg;
}

static ra8_err_t
c_rec_delay(void* ctx, ra8_bundle_target_t target, uint8_t attempt, uint32_t delay_ms)
{
  (void)target;
  (void)attempt;
  ((c_reconcile_ctx_t*)ctx)->last_delay = delay_ms;
  return k_ra8_ok;
}

static void c_rec_safe(void* ctx, ra8_bundle_target_t target, const ra8_reconcile_nv_state_t* state)
{
  (void)target;
  (void)state;
  ++((c_reconcile_ctx_t*)ctx)->safe_calls;
}

static void test_reconcile_desired_state_and_safe_halt(void)
{
  const uint32_t          pay_off  = k_ra8_bundle_header_bytes + 3U * k_ra8_bundle_desc_bytes + 64U;
  ra8_bundle_hdr_t        hdr      = {.magic           = k_ra8_bundle_magic,
                                      .format_version  = k_ra8_bundle_format_ver,
                                      .header_bytes    = k_ra8_bundle_header_bytes,
                                      .bundle_version  = 40U,
                                      .image_count     = 3U,
                                      .flags           = 0U,
                                      .total_bytes     = pay_off + 192U,
                                      .payloads_offset = pay_off,
                                      .sig_len         = 64U,
                                      .reserved        = 0U};
  ra8_bundle_image_desc_t descs[3] = {
    {.target        = k_ra8_bundle_target_ra8_app,
     .format        = k_ra8_bundle_fmt_rot1_img,
     .image_version = 100U,
     .offset        = pay_off,
     .size          = 64U},
    {.target           = k_ra8_bundle_target_esp32_c6,
     .format           = k_ra8_bundle_fmt_c6_img,
     .image_version    = 200U,
     .compat_proto_ver = 4U,
     .offset           = pay_off + 64U,
     .size             = 64U},
    {.target        = k_ra8_bundle_target_display,
     .format        = k_ra8_bundle_fmt_raw,
     .image_version = 300U,
     .offset        = pay_off + 128U,
     .size          = 64U},
  };
  ra8_reconcile_nv_state_t nv_check = {};
  EXPECT_EQ(k_ra8_ok, ra8_reconcile_nv_init(40U, descs, 3U, &nv_check));
  EXPECT_EQ(k_ra8_ok, ra8_reconcile_nv_validate(&nv_check));
  nv_check.bundle_version ^= 1U;
  EXPECT_EQ(k_ra8_err_crc_mismatch, ra8_reconcile_nv_validate(&nv_check));

  ra8_reconcile_policy_t pol = {
    .base_backoff_ms     = 50U,
    .max_backoff_ms      = 200U,
    .max_retries         = 2U,
    .backoff_shift       = 2U,
    .reprobe_after_apply = true,
  };
  uint32_t delay = 0U;
  EXPECT_EQ(k_ra8_ok, ra8_reconcile_compute_backoff_ms(&pol, 2U, &delay));
  EXPECT_EQ(100U, delay);

  c_reconcile_ctx_t   ctx = {.c6_ver = 10U, .disp_ver = 15U, .cut_after_c6_flash = true};
  ra8_reconcile_ops_t ops = {
    .nv_load         = c_rec_nv_load,
    .nv_save         = c_rec_nv_save,
    .query_companion = c_rec_query,
    .apply_companion = c_rec_apply,
    .backoff_delay   = c_rec_delay,
    .on_safe_state   = c_rec_safe,
    .ctx             = &ctx,
  };
  ra8_reconcile_report_t rpt = {};
  EXPECT_EQ(k_ra8_ok, ra8_reconcile_boot(&hdr, descs, &pol, &ops, &rpt));
  EXPECT_EQ(k_ra8_reconcile_state_pending_retry, rpt.overall_state);
  EXPECT_EQ(1U, ctx.c6_applies);

  /* Next boot skips already-converged C6 and finishes Display. */
  EXPECT_EQ(k_ra8_ok, ra8_reconcile_boot(&hdr, descs, &pol, &ops, &rpt));
  EXPECT_EQ(k_ra8_reconcile_state_converged, rpt.overall_state);
  EXPECT_EQ(1U, ctx.c6_applies);
  EXPECT_EQ(1U, ctx.disp_applies);

  /* New bundle targeting permanently failing C6 hits safe_halted after 2 retries. */
  hdr.bundle_version     = 41U;
  descs[1].image_version = 201U;
  ctx.c6_stuck           = true;
  EXPECT_EQ(k_ra8_ok, ra8_reconcile_boot(&hdr, descs, &pol, &ops, &rpt));
  EXPECT_EQ(k_ra8_reconcile_state_pending_retry, rpt.overall_state);
  EXPECT_EQ(k_ra8_ok, ra8_reconcile_boot(&hdr, descs, &pol, &ops, &rpt));
  EXPECT_EQ(k_ra8_reconcile_state_safe_halted, rpt.overall_state);
  EXPECT_EQ(50U, ctx.last_delay);
  EXPECT_EQ(1U, ctx.safe_calls);
  EXPECT_EQ(3U, ctx.c6_applies);
}

int main(void)
{
  test_ota_immediate_flow();
  test_dfu_trial_defers_floor_and_rolls_back_on_bad_verify();
  test_guards_downgrade_and_explicit_rollback();
  test_ra8u_bundle_pipeline_single_and_multi();
  test_reconcile_desired_state_and_safe_halt();
  return (s_fail == 0U) ? 0 : 1;
}
