/**
 * @file test_ra8_rsip_eddsa.c
 * @brief Dispatch / opcode-selection tests for the RSIP EdDSA + RSA
 *        encrypt / decrypt asymmetric entry points.
 *
 * @details
 * These tests exercise the HAL-side OPCODE SELECTION, command DISPATCH,
 * and ARGUMENT PLUMBING of the asymmetric routines that were added to
 * fix two defects in ``ra8_rsip_asym.c``:
 *
 * 1. Ed25519 was being mis-routed through the ECDSA-sign opcode, which
 *    cannot produce a valid RFC 8032 PureEdDSA signature (issue #181).
 *    The RSIP-E50D on this silicon exposes no documented Ed25519 register
 *    interface, so ``ra8_rsip_eddsa_sign`` / ``ra8_rsip_eddsa_verify`` are
 *    FAIL-CLOSED in a production build (return ``k_ra8_err_not_supported``)
 *    and drive a placeholder EdDSA command path -- separate from, and
 *    never on, the ECDSA opcode -- ONLY under the insecure-stub /
 *    simulator guard. ``ra8_rsip_ecdsa_sign`` / ``ra8_rsip_ecdsa_verify``
 *    reject the Ed25519 curve outright. The host suite compiles under
 *    ``RA8_SIMULATOR_MODE`` (the guarded #if branch), so the cases below
 *    assert that a simulator Ed25519 operation lands on
 *    ``k_ra8_rsip_asym_op_eddsa_*`` and NEVER on
 *    ``k_ra8_rsip_asym_op_ecdsa_sign``; the production fail-closed #else is
 *    proven by ``scripts/checks/check_stub_crypto_guarded.py`` and the ARM
 *    cross-build.
 * 2. RSA (``ra8_rsip_rsa_sign`` / ``..._verify`` / ``..._encrypt`` /
 *    ``..._decrypt``) has no documented RSIP register backend either
 *    (issues #214 + #187), so it too is FAIL-CLOSED in a production build
 *    (``k_ra8_err_not_supported``) and drives the modelled command path only
 *    under the insecure-stub / simulator guard. The cases below assert that a
 *    simulator RSA operation writes the opcode, padding-scheme, and
 *    modulus-size argument registers as expected; the production fail-closed
 *    #else is proven by ``check_stub_crypto_guarded.py`` and the ARM
 *    cross-build.
 *
 * @warning NO REAL Ed25519 BACKEND: the RSIP HAL does not compute Ed25519
 *          on this silicon (the command path modelled here is fiction,
 *          guarded so a production build fails closed), so there is
 *          deliberately no RFC 8032 known-answer test -- a real Ed25519
 *          KAT belongs with tf-psa-crypto (``PSA_ALG_PURE_EDDSA``) if a
 *          consumer ever needs it. RSA is likewise fiction with no RSIP
 *          backend: the ``ra8_sim_mmap`` register window backs MMIO with plain
 *          memory and does NOT model the engine maths, so these host tests
 *          verify the COMMAND PATH only, never the signature / ciphertext
 *          bytes. RSA is FAIL-CLOSED in production, so nothing ships the
 *          fiction; a real RFC 8017 KAT belongs with tf-psa-crypto if a
 *          consumer ever needs it.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 *
 * @since 0.1.0
 */

#include <stdint.h>
#include <stdio.h>

#include "ra8_err.h"
#include "ra8_mstp.h"
#include "ra8_rsip.h"
#include "ra8_rsip_regs.h"
#include "ra8_sim_mmap.h"
#include "unity_minimal.h"

/**
 * @enum t_eddsa_t
 * @brief Signature buffer capacity and the fixture body fill.
 */
typedef enum : uint32_t {
  k_t_sig_cap   = 132U,         /**< Signature scratch, bytes: past the 64-byte
                                    Ed25519 signature so the length guard, not
                                    the buffer, is what limits the write.        */
  k_t_body_fill = 0xB5B50000UL, /**< Alternating high half, ORed with the word
                                     index so each word is unique.               */
} t_eddsa_t;

/**
 * @enum ra8_rsip_eddsa_test_const_t
 * @brief Named magic numbers used by the dispatch tests.
 */
typedef enum : uint32_t {
  k_eddsa_test_msg_bytes   = 32U,          /**< Ed25519 test-message length.     */
  k_eddsa_test_sig_bytes   = 64U,          /**< Ed25519 signature (R||S) bytes.  */
  k_eddsa_test_sentinel    = 0xEEEEEEEEUL, /**< Pre-call ASYM_CTRL marker.       */
  k_rsa_test_modulus_bytes = 256U,         /**< RSA-2048 modulus width (2048/8). */
  k_rsa_test_pt_bytes      = 16U,          /**< RSA short-message length.        */
  k_rsa_test_bad_size      = 777U,         /**< Unsupported RSA size selector.   */
  k_rsa_test_bad_pad       = 99U,          /**< Unsupported RSAES pad selector.  */
  k_rsa_test_oversize_pt   = 300U,         /**< Plaintext > modulus length.      */
} ra8_rsip_eddsa_test_const_t;

/**
 * @brief Bring the simulated RSIP engine up to a running state.
 * @since 0.1.0
 */
static void prep_running(void)
{
  ra8_sim_mmap_reset();
  (void)ra8_mstp_init();
  const ra8_rsip_config_t cfg = {.run_bist = true};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_rsip_init(&cfg));
}

/**
 * @brief Build a wrapped-key handle with a recognisable body fill.
 *
 * @param[in] alg   Install opcode to stamp into ``alg``.
 * @param[in] words Body-word count.
 * @return Populated handle.
 * @since 0.1.0
 */
static ra8_rsip_key_handle_t make_handle(ra8_rsip_oem_cmd_t alg, uint32_t words)
{
  ra8_rsip_key_handle_t h = {.alg = (uint32_t)alg, .body_words = words};
  for (uint32_t i = 0U; i < words; ++i) {
    h.body[i] = k_t_body_fill | i;
  }
  return h;
}

/**
 * @brief Ed25519 sign selects the EdDSA opcode, not the ECDSA opcode.
 *
 * @par MC/DC:
 * Decision: `if (msg == nullptr && msg_len != 0)` (2 conditions) inside
 * `ra8_rsip_eddsa_sign`. This case is the control vector:
 * - Vector 1 (this test): msg=valid, msg_len=32 -> false (proceeds).
 * The varying vectors live in `test_eddsa_null_and_alg_matrix`:
 * - Vector 2: msg=NULL, msg_len=32 -> true  (varies msg, control vs V1).
 * - Vector 3: msg=NULL, msg_len=0  -> false (varies msg_len, control vs V2).
 * Vectors 1+2 prove `msg` independence; 2+3 prove `msg_len` independence.
 */
static void test_eddsa_sign_selects_eddsa_opcode(void)
{
  TEST_BEGIN("rsip eddsa sign selects eddsa opcode");
  prep_running();

  ra8_rsip_key_handle_t key =
    make_handle(k_ra8_rsip_oem_cmd_ecc_ed25519_priv, (uint32_t)k_ra8_rsip_handle_words_ecc256_priv);
  const uint8_t msg[k_eddsa_test_msg_bytes] = {};
  uint8_t       sig[k_eddsa_test_sig_bytes] = {};

  TEST_ASSERT_EQ(k_ra8_ok, ra8_rsip_eddsa_sign(&key, msg, sizeof(msg), sig));

  /* The crux of the fix: Ed25519 lands on the dedicated EdDSA opcode. */
  TEST_ASSERT_EQ(k_ra8_rsip_asym_op_eddsa_sign, *ra8_rsip_reg32(k_ra8_rsip_off_asym_ctrl));
  TEST_ASSERT_EQ(k_ra8_rsip_asym_op_eddsa_sign, *ra8_rsip_reg32(k_ra8_rsip_off_mbox_op));
  /* It must NOT have been mis-routed to ECDSA-sign. */
  TEST_ASSERT(*ra8_rsip_reg32(k_ra8_rsip_off_asym_ctrl) != (uint32_t)k_ra8_rsip_asym_op_ecdsa_sign);
  /* Curve selector carries Ed25519. */
  TEST_ASSERT_EQ(k_ra8_rsip_curve_ed25519, *ra8_rsip_reg32(k_ra8_rsip_off_asym_curve));

  TEST_END("rsip eddsa sign selects eddsa opcode");
}

/**
 * @brief Ed25519 verify selects the EdDSA-verify opcode.
 *
 * @par MC/DC:
 * (no compound decision reached on the happy path -- the
 * `msg == nullptr && msg_len != 0` decision evaluates false with a
 * valid message; its independence vectors are in
 * `test_eddsa_null_and_alg_matrix`.)
 */
static void test_eddsa_verify_selects_eddsa_opcode(void)
{
  TEST_BEGIN("rsip eddsa verify selects eddsa opcode");
  prep_running();

  ra8_rsip_key_handle_t key =
    make_handle(k_ra8_rsip_oem_cmd_ecc_ed25519_priv, (uint32_t)k_ra8_rsip_handle_words_ecc256_priv);
  const uint8_t msg[k_eddsa_test_msg_bytes] = {};
  const uint8_t sig[k_eddsa_test_sig_bytes] = {};

  TEST_ASSERT_EQ(k_ra8_ok, ra8_rsip_eddsa_verify(&key, msg, sizeof(msg), sig));

  TEST_ASSERT_EQ(k_ra8_rsip_asym_op_eddsa_verify, *ra8_rsip_reg32(k_ra8_rsip_off_asym_ctrl));
  TEST_ASSERT_EQ(k_ra8_rsip_asym_op_eddsa_verify, *ra8_rsip_reg32(k_ra8_rsip_off_mbox_op));
  TEST_ASSERT(*ra8_rsip_reg32(k_ra8_rsip_off_asym_ctrl) !=
              (uint32_t)k_ra8_rsip_asym_op_ecdsa_verify);

  TEST_END("rsip eddsa verify selects eddsa opcode");
}

/**
 * @brief Null-pointer + wrong-algorithm rejection for the EdDSA entries.
 *
 * @par MC/DC:
 * Decision: `if (msg == nullptr && msg_len != 0)` (2 conditions) in
 * `ra8_rsip_eddsa_sign` / `ra8_rsip_eddsa_verify`:
 * - Vector 2: msg=NULL, msg_len=32 -> true  -> k_ra8_err_null_ptr.
 * - Vector 3: msg=NULL, msg_len=0  -> false -> proceeds to alg check.
 * Combined with Vector 1 (valid msg) from the happy-path tests, these
 * give the N+1 = 3 vectors: 1+2 vary `msg` (outcome changes), 2+3 vary
 * `msg_len` (outcome changes). The trailing single-condition guard
 * `if (key->alg != ed25519_priv)` is exercised both ways: true (wrong
 * alg -> invalid_arg) and false (Vector 3 -> proceeds to k_ra8_ok).
 */
static void test_eddsa_null_and_alg_matrix(void)
{
  TEST_BEGIN("rsip eddsa null and alg matrix");
  prep_running();

  ra8_rsip_key_handle_t key =
    make_handle(k_ra8_rsip_oem_cmd_ecc_ed25519_priv, (uint32_t)k_ra8_rsip_handle_words_ecc256_priv);
  ra8_rsip_key_handle_t wrong_alg = make_handle(k_ra8_rsip_oem_cmd_ecc_secp256r1_priv,
                                                (uint32_t)k_ra8_rsip_handle_words_ecc256_priv);
  const uint8_t         msg[k_eddsa_test_msg_bytes] = {};
  uint8_t               sig[k_eddsa_test_sig_bytes] = {};

  /* Null pointers. */
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_rsip_eddsa_sign(nullptr, msg, sizeof(msg), sig));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_rsip_eddsa_sign(&key, msg, sizeof(msg), nullptr));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_rsip_eddsa_verify(nullptr, msg, sizeof(msg), sig));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_rsip_eddsa_verify(&key, msg, sizeof(msg), nullptr));

  /* Vector 2: msg=NULL, msg_len!=0 -> compound true -> null_ptr. */
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_rsip_eddsa_sign(&key, nullptr, sizeof(msg), sig));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_rsip_eddsa_verify(&key, nullptr, sizeof(msg), sig));

  /* Vector 3: msg=NULL, msg_len==0 -> compound false -> proceeds, k_ra8_ok. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_rsip_eddsa_sign(&key, nullptr, 0U, sig));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_rsip_eddsa_verify(&key, nullptr, 0U, sig));

  /* Wrong algorithm tag (alg-guard true) -> invalid_arg. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_rsip_eddsa_sign(&wrong_alg, msg, sizeof(msg), sig));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_rsip_eddsa_verify(&wrong_alg, msg, sizeof(msg), sig));

  TEST_END("rsip eddsa null and alg matrix");
}

/**
 * @brief ECDSA sign / verify reject the Ed25519 curve (stop mis-routing).
 *
 * @par MC/DC:
 * Decision: `if (curve == k_ra8_rsip_curve_ed25519)` (1 condition) in
 * `ra8_rsip_ecdsa_sign` / `ra8_rsip_ecdsa_verify`:
 * - Vector A: curve=ed25519    -> true  -> invalid_arg, opcode NOT written.
 * - Vector B: curve=secp256r1  -> false -> proceeds, ecdsa opcode written.
 * 2 vectors exercise the single-condition guard both ways and prove the
 * Ed25519 curve no longer reaches the ECDSA modular-exponentiation path.
 */
static void test_ecdsa_rejects_ed25519(void)
{
  TEST_BEGIN("rsip ecdsa rejects ed25519");
  prep_running();

  ra8_rsip_key_handle_t ecc = make_handle(k_ra8_rsip_oem_cmd_ecc_secp256r1_priv,
                                          (uint32_t)k_ra8_rsip_handle_words_ecc256_priv);
  const uint8_t         digest[k_eddsa_test_msg_bytes] = {};
  uint8_t               sig[k_t_sig_cap]               = {};

  /* Vector A: ed25519 -> reject before any opcode write. */
  *ra8_rsip_reg32(k_ra8_rsip_off_asym_ctrl) = (uint32_t)k_eddsa_test_sentinel;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_rsip_ecdsa_sign(&ecc, k_ra8_rsip_curve_ed25519, digest, sizeof(digest), sig));
  TEST_ASSERT_EQ(k_eddsa_test_sentinel, *ra8_rsip_reg32(k_ra8_rsip_off_asym_ctrl));

  *ra8_rsip_reg32(k_ra8_rsip_off_asym_ctrl) = (uint32_t)k_eddsa_test_sentinel;
  TEST_ASSERT_EQ(
    k_ra8_err_invalid_arg,
    ra8_rsip_ecdsa_verify(&ecc, k_ra8_rsip_curve_ed25519, digest, sizeof(digest), sig));
  TEST_ASSERT_EQ(k_eddsa_test_sentinel, *ra8_rsip_reg32(k_ra8_rsip_off_asym_ctrl));

  /* Vector B: a Weierstrass curve still reaches the ECDSA opcode. */
  TEST_ASSERT_EQ(
    k_ra8_ok,
    ra8_rsip_ecdsa_sign(&ecc, k_ra8_rsip_curve_secp256r1, digest, sizeof(digest), sig));
  TEST_ASSERT_EQ(k_ra8_rsip_asym_op_ecdsa_sign, *ra8_rsip_reg32(k_ra8_rsip_off_asym_ctrl));

  TEST_END("rsip ecdsa rejects ed25519");
}

/**
 * @brief Thin wrapper over `ra8_rsip_rsa_encrypt` for the opcode-reach test.
 * @param[in]  key   Key handle (nullptr exercises the key guard).
 * @param[in]  size  RSA modulus-size selector.
 * @param[in]  pad   Padding scheme selector.
 * @param[in]  pt    Plaintext input (nullptr exercises the input guard).
 * @param[in]  ptlen Plaintext length in bytes.
 * @param[out] ct    Ciphertext output (nullptr exercises the output guard).
 * @return The `ra8_rsip_rsa_encrypt` result code.
 * @pre The RSIP mock is running.
 * @post No state beyond @p ct is modified.
 * @note Not thread-safe; single-threaded host-test helper.
 * @since 0.1.0
 */
static ra8_err_t rsa_enc(ra8_rsip_key_handle_t* key,
                         ra8_rsip_rsa_size_t    size,
                         ra8_rsip_rsa_pad_t     pad,
                         const uint8_t*         pt,
                         uint32_t               ptlen,
                         uint8_t*               ct)
{
  return ra8_rsip_rsa_encrypt(key, size, pad, pt, ptlen, ct);
}

/**
 * @brief Thin wrapper over `ra8_rsip_rsa_decrypt` for the opcode-reach test.
 * @param[in]  key   Key handle (nullptr exercises the key guard).
 * @param[in]  size  RSA modulus-size selector.
 * @param[in]  pad   Padding scheme selector.
 * @param[in]  ct    Ciphertext input (nullptr exercises the input guard).
 * @param[out] pt    Plaintext output (nullptr exercises the output guard).
 * @param[in]  cap   Plaintext buffer capacity in bytes.
 * @param[out] rlen  Recovered-length sink (nullptr exercises the length guard).
 * @return The `ra8_rsip_rsa_decrypt` result code.
 * @pre The RSIP mock is running.
 * @post No state beyond @p pt / @p rlen is modified.
 * @note Not thread-safe; single-threaded host-test helper.
 * @since 0.1.0
 */
static ra8_err_t rsa_dec(ra8_rsip_key_handle_t* key,
                         ra8_rsip_rsa_size_t    size,
                         ra8_rsip_rsa_pad_t     pad,
                         const uint8_t*         ct,
                         uint8_t*               pt,
                         uint32_t               cap,
                         uint32_t*              rlen)
{
  return ra8_rsip_rsa_decrypt(key, size, pad, ct, pt, cap, rlen);
}

/**
 * @brief RSA public-encrypt reaches its opcode and plumbs size + pad.
 *
 * @par MC/DC:
 * The encrypt validators are all single-condition guards (no `&&` /
 * `||`); each is exercised both ways:
 * - `modulus_bytes == 0` : true via bad size -> invalid_arg;
 *                          false via RSA-2048 -> proceeds.
 * - `!internal_rsa_pad_ok`: true via bad pad -> invalid_arg;
 *                          false via OAEP -> proceeds.
 * - `plaintext_len == 0` : true -> invalid_arg; false -> proceeds.
 * - `plaintext_len > modulus_bytes`: true via 300 > 256 -> invalid_arg;
 *                          false via 16 <= 256 -> proceeds.
 */
static void test_rsa_encrypt_reaches_opcode(void)
{
  TEST_BEGIN("rsip rsa encrypt reaches opcode");
  prep_running();

  ra8_rsip_key_handle_t key =
    make_handle(k_ra8_rsip_oem_cmd_rsa2048_priv, (uint32_t)k_ra8_rsip_handle_words_rsa2048_priv);
  const uint8_t pt[k_rsa_test_pt_bytes]      = {};
  uint8_t       ct[k_rsa_test_modulus_bytes] = {};

  /* Null matrix. */
  TEST_ASSERT_EQ(
    k_ra8_err_null_ptr,
    rsa_enc(nullptr, k_ra8_rsip_rsa_2048, k_ra8_rsip_rsa_pad_oaep, pt, sizeof(pt), ct));
  TEST_ASSERT_EQ(
    k_ra8_err_null_ptr,
    rsa_enc(&key, k_ra8_rsip_rsa_2048, k_ra8_rsip_rsa_pad_oaep, nullptr, sizeof(pt), ct));
  TEST_ASSERT_EQ(
    k_ra8_err_null_ptr,
    rsa_enc(&key, k_ra8_rsip_rsa_2048, k_ra8_rsip_rsa_pad_oaep, pt, sizeof(pt), nullptr));

  /* Single-condition guards, true side. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 rsa_enc(&key,
                         (ra8_rsip_rsa_size_t)k_rsa_test_bad_size,
                         k_ra8_rsip_rsa_pad_oaep,
                         pt,
                         sizeof(pt),
                         ct));
  TEST_ASSERT_EQ(
    k_ra8_err_invalid_arg,
    rsa_enc(&key, k_ra8_rsip_rsa_2048, (ra8_rsip_rsa_pad_t)k_rsa_test_bad_pad, pt, sizeof(pt), ct));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 rsa_enc(&key, k_ra8_rsip_rsa_2048, k_ra8_rsip_rsa_pad_oaep, pt, 0U, ct));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 rsa_enc(&key,
                         k_ra8_rsip_rsa_2048,
                         k_ra8_rsip_rsa_pad_oaep,
                         pt,
                         (uint32_t)k_rsa_test_oversize_pt,
                         ct));

  /* Happy path: all guards false -> opcode + arguments reach the engine. */
  TEST_ASSERT_EQ(k_ra8_ok,
                 rsa_enc(&key, k_ra8_rsip_rsa_2048, k_ra8_rsip_rsa_pad_oaep, pt, sizeof(pt), ct));
  TEST_ASSERT_EQ(k_ra8_rsip_asym_op_rsa_encrypt, *ra8_rsip_reg32(k_ra8_rsip_off_asym_ctrl));
  TEST_ASSERT_EQ(k_ra8_rsip_asym_op_rsa_encrypt, *ra8_rsip_reg32(k_ra8_rsip_off_mbox_op));
  TEST_ASSERT_EQ(k_ra8_rsip_rsa_pad_oaep, *ra8_rsip_reg32(k_ra8_rsip_off_asym_arg));
  TEST_ASSERT_EQ(k_ra8_rsip_rsa_2048, *ra8_rsip_reg32(k_ra8_rsip_off_asym_rsa_size));

  TEST_END("rsip rsa encrypt reaches opcode");
}

/**
 * @brief RSA private-decrypt reaches its opcode and reports a length.
 *
 * @details
 * The recovered-message length is read back from ASYM_ARG, which the
 * silicon engine overwrites with the unpadded length. The host
 * ``ra8_sim_mmap`` window does NOT model the engine, so ASYM_ARG retains
 * the padding descriptor that the driver wrote -- hence the recovered
 * length observed here equals the pad selector value. This is a sim
 * artifact, asserted only to confirm the read-back plumbing; the real
 * length is a HARDWARE-GATED value.
 *
 * @par MC/DC:
 * Single-condition guards, each exercised both ways:
 * - `modulus_bytes == 0` : true via bad size; false via RSA-2048.
 * - `!internal_rsa_pad_ok`: true via bad pad; false via OAEP.
 * - `recovered > plaintext_cap`: true via cap=1 (< pad=2) -> invalid_arg;
 *                          false via cap>=2 -> proceeds.
 */
static void test_rsa_decrypt_reaches_opcode(void)
{
  TEST_BEGIN("rsip rsa decrypt reaches opcode");
  prep_running();

  ra8_rsip_key_handle_t key =
    make_handle(k_ra8_rsip_oem_cmd_rsa2048_priv, (uint32_t)k_ra8_rsip_handle_words_rsa2048_priv);
  const uint8_t ct[k_rsa_test_modulus_bytes] = {};
  uint8_t       pt[k_rsa_test_modulus_bytes] = {};
  uint32_t      rlen                         = 0U;

  /* Null matrix. */
  TEST_ASSERT_EQ(
    k_ra8_err_null_ptr,
    rsa_dec(nullptr, k_ra8_rsip_rsa_2048, k_ra8_rsip_rsa_pad_oaep, ct, pt, sizeof(pt), &rlen));
  TEST_ASSERT_EQ(
    k_ra8_err_null_ptr,
    rsa_dec(&key, k_ra8_rsip_rsa_2048, k_ra8_rsip_rsa_pad_oaep, nullptr, pt, sizeof(pt), &rlen));
  TEST_ASSERT_EQ(
    k_ra8_err_null_ptr,
    rsa_dec(&key, k_ra8_rsip_rsa_2048, k_ra8_rsip_rsa_pad_oaep, ct, nullptr, sizeof(pt), &rlen));
  TEST_ASSERT_EQ(
    k_ra8_err_null_ptr,
    rsa_dec(&key, k_ra8_rsip_rsa_2048, k_ra8_rsip_rsa_pad_oaep, ct, pt, sizeof(pt), nullptr));

  /* Single-condition guards, true side. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 rsa_dec(&key,
                         (ra8_rsip_rsa_size_t)k_rsa_test_bad_size,
                         k_ra8_rsip_rsa_pad_oaep,
                         ct,
                         pt,
                         sizeof(pt),
                         &rlen));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 rsa_dec(&key,
                         k_ra8_rsip_rsa_2048,
                         (ra8_rsip_rsa_pad_t)k_rsa_test_bad_pad,
                         ct,
                         pt,
                         sizeof(pt),
                         &rlen));

  /* recovered (== pad descriptor in sim == 2) > cap=1 -> invalid_arg. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 rsa_dec(&key, k_ra8_rsip_rsa_2048, k_ra8_rsip_rsa_pad_oaep, ct, pt, 1U, &rlen));

  /* Happy path: opcode + arguments reach the engine, length read back. */
  TEST_ASSERT_EQ(
    k_ra8_ok,
    rsa_dec(&key, k_ra8_rsip_rsa_2048, k_ra8_rsip_rsa_pad_oaep, ct, pt, sizeof(pt), &rlen));
  TEST_ASSERT_EQ(k_ra8_rsip_asym_op_rsa_decrypt, *ra8_rsip_reg32(k_ra8_rsip_off_asym_ctrl));
  TEST_ASSERT_EQ(k_ra8_rsip_asym_op_rsa_decrypt, *ra8_rsip_reg32(k_ra8_rsip_off_mbox_op));
  TEST_ASSERT_EQ(k_ra8_rsip_rsa_2048, *ra8_rsip_reg32(k_ra8_rsip_off_asym_rsa_size));
  /* Sim artifact: ASYM_ARG retains the pad descriptor as the "length". */
  TEST_ASSERT_EQ(k_ra8_rsip_rsa_pad_oaep, rlen);

  TEST_END("rsip rsa decrypt reaches opcode");
}

/**
 * @brief Test entry point.
 * @return 0 on success (failures call exit(1) inside the asserts).
 * @since 0.1.0
 */
int32_t main(void)
{
  test_eddsa_sign_selects_eddsa_opcode();
  test_eddsa_verify_selects_eddsa_opcode();
  test_eddsa_null_and_alg_matrix();
  test_ecdsa_rejects_ed25519();
  test_rsa_encrypt_reaches_opcode();
  test_rsa_decrypt_reaches_opcode();
  (void)fprintf(stderr, "[OK  ] test_ra8_rsip_eddsa.c\n");
  return 0;
}
