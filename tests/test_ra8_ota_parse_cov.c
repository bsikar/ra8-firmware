/**
 * @file test_ra8_ota_parse_cov.c
 * @brief Coverage-gap tests for libs/ra8_ota/src/ra8_ota_parse.c.
 *
 * @details
 * Companion to test_ra8_ota.c and test_ra8_ota_cov.c. Targets 17 lines in
 * ra8_ota_parse.c left uncovered by the primary suites:
 *
 *   Line 202 -- internal_validate_cfg_flash: bank_size > k_ra8_ota_max_image_bytes.
 *   Line 216 -- priv_ota_validate_cfg: crypto sub-validator fails.
 *   Line 268 -- internal_json_str: opening-value-quote strchr returns NULL.
 *   Line 273 -- internal_json_str: closing-value-quote strchr returns NULL.
 *   Line 277 -- internal_json_str: value length exceeds cap.
 *   Line 313 -- priv_ota_json_u32: key not found in JSON.
 *   Line 334 -- priv_ota_json_u32: no digits after skip loop.
 *   Line 370 -- internal_hex_nibble: uppercase A-F return arm.
 *   Line 402 -- internal_hex_decode: odd-length hex string.
 *   Line 406 -- internal_hex_decode: decoded bytes exceed out_cap.
 *   Line 452 -- internal_manifest_decode_crypto: sha256 key missing.
 *   Line 461 -- internal_manifest_decode_crypto: signature key missing.
 *   Line 465 -- internal_manifest_decode_crypto: signature hex decodes to 0 bytes.
 *   Line 504 -- priv_ota_manifest_decode: url key missing.
 *   Line 508 -- priv_ota_manifest_decode: size key missing.
 *   Line 511 -- priv_ota_manifest_decode: image_size_bytes == 0.
 *   Line 514 -- priv_ota_manifest_decode: image_size_bytes > max.
 *
 * All paths are reached through the promoted internal helpers exported by
 * ra8_ota_internal.h: priv_ota_validate_cfg,
 * priv_ota_manifest_decode, and priv_ota_json_u32. The private
 * helpers (internal_json_str, internal_hex_nibble, internal_hex_decode,
 * internal_manifest_decode_crypto) are exercised transitively.
 *
 * priv_ota_validate_cfg and priv_ota_manifest_decode are pure
 * with respect to module state (documented in ra8_ota_parse.c). No
 * ra8_ota_init call is required.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "ra8_err.h"
#include "ra8_ota.h"
#include "ra8_ota_internal.h"
#include "unity_minimal.h"

/**
 * @enum t_bank_t
 * @brief Flash-bank geometry and the out-parameter seed.
 */
typedef enum : uint32_t {
  k_t_bank_size     = 4096U,        /**< Bank size, bytes.                  */
  k_t_bank_addr     = 0x02080000UL, /**< Base address of the inactive bank. */
  k_t_out_word_seed = 0xDEADBEEFUL, /**< Pre-set output word; a parse that
                                         fails must leave it untouched.        */
} t_bank_t;

/* =============================================================================
 * Dummy function pointers -- never invoked; exist only to satisfy non-NULL
 * checks inside priv_ota_validate_cfg.
 * ============================================================================= */

/* The pointer parameters below cannot be const: this mock implements a
 * function-pointer interface (the DI seam under test), so its signature is
 * fixed by the typedef it is assigned to -- adding const changes the
 * function type and the assignment stops compiling. */
// NOLINTNEXTLINE(readability-non-const-parameter)
static ra8_err_t dummy_net_open(void* ctx, const char* url, uint32_t* out_len)
{
  (void)ctx;
  (void)url;
  (void)out_len;
  return k_ra8_ok;
}

/* The pointer parameters below cannot be const: this mock implements a
 * function-pointer interface (the DI seam under test), so its signature is
 * fixed by the typedef it is assigned to -- adding const changes the
 * function type and the assignment stops compiling. */
// NOLINTNEXTLINE(readability-non-const-parameter)
static ra8_err_t dummy_net_read(void* ctx, uint8_t* dst, uint32_t cap, uint32_t* out_len)
{
  (void)ctx;
  (void)dst;
  (void)cap;
  (void)out_len;
  return k_ra8_ok;
}

static ra8_err_t dummy_net_close(void* ctx)
{
  (void)ctx;
  return k_ra8_ok;
}

static ra8_err_t dummy_sha_init(void* ctx)
{
  (void)ctx;
  return k_ra8_ok;
}

static ra8_err_t dummy_sha_update(void* ctx, const uint8_t* data, uint32_t len)
{
  (void)ctx;
  (void)data;
  (void)len;
  return k_ra8_ok;
}

/* The pointer parameters below cannot be const: this mock implements a
 * function-pointer interface (the DI seam under test), so its signature is
 * fixed by the typedef it is assigned to -- adding const changes the
 * function type and the assignment stops compiling. */
// NOLINTNEXTLINE(readability-non-const-parameter)
static ra8_err_t dummy_sha_final(void* ctx, uint8_t out[k_ra8_ota_sha256_bytes])
{
  (void)ctx;
  (void)out;
  return k_ra8_ok;
}

static ra8_err_t dummy_ecdsa_verify(void*          ctx,
                                    uint32_t       key,
                                    const uint8_t  digest[k_ra8_ota_sha256_bytes],
                                    const uint8_t* sig,
                                    uint32_t       sig_len)
{
  (void)ctx;
  (void)key;
  (void)digest;
  (void)sig;
  (void)sig_len;
  return k_ra8_ok;
}

static ra8_err_t dummy_flash_erase(void* ctx, uint32_t addr, uint32_t len)
{
  (void)ctx;
  (void)addr;
  (void)len;
  return k_ra8_ok;
}

static ra8_err_t dummy_flash_program(void* ctx, uint32_t addr, const uint8_t* src, uint32_t len)
{
  (void)ctx;
  (void)addr;
  (void)src;
  (void)len;
  return k_ra8_ok;
}

static ra8_err_t dummy_flash_set_startup(void* ctx, uint8_t which, bool persistent)
{
  (void)ctx;
  (void)which;
  (void)persistent;
  return k_ra8_ok;
}

/* The pointer parameters below cannot be const: this mock implements a
 * function-pointer interface (the DI seam under test), so its signature is
 * fixed by the typedef it is assigned to -- adding const changes the
 * function type and the assignment stops compiling. */
// NOLINTNEXTLINE(readability-non-const-parameter)
static ra8_err_t dummy_flash_readback(void* ctx, uint32_t addr, uint8_t* dst, uint32_t len)
{
  (void)ctx;
  (void)addr;
  (void)dst;
  (void)len;
  return k_ra8_ok;
}

/* =============================================================================
 * Fixture helper
 * ============================================================================= */

/**
 * @brief Build a minimal fully-populated ra8_ota_cfg_t using dummy stubs.
 *
 * @details All function pointers reference the local dummy stubs above.
 *          bank_size_bytes is 4096 (well within k_ra8_ota_max_image_bytes).
 *          manifest_url is non-empty.
 *
 * @return ra8_ota_cfg_t A valid, fully populated configuration.
 *
 * @pre  None.
 * @post Returned struct passes priv_ota_validate_cfg.
 * @note Helper for local test functions only; not for production use.
 * @since 0.1.0
 */
static ra8_ota_cfg_t priv_make_cfg(void)
{
  ra8_ota_cfg_t c = {};
  (void)snprintf(c.manifest_url, k_ra8_ota_url_max_bytes, "https://example.test/manifest.json");
  c.pubkey_handle             = 0x1U;
  c.on_progress               = nullptr;
  c.run_as_thread             = false;
  c.net.open                  = dummy_net_open;
  c.net.read                  = dummy_net_read;
  c.net.close                 = dummy_net_close;
  c.crypto.sha256_init        = dummy_sha_init;
  c.crypto.sha256_update      = dummy_sha_update;
  c.crypto.sha256_final       = dummy_sha_final;
  c.crypto.ecdsa_verify       = dummy_ecdsa_verify;
  c.flash.erase               = dummy_flash_erase;
  c.flash.program             = dummy_flash_program;
  c.flash.set_startup         = dummy_flash_set_startup;
  c.flash.readback            = dummy_flash_readback;
  c.flash.inactive_bank_addr  = k_t_bank_addr;
  c.flash.bank_size_bytes     = k_t_bank_size;
  c.flash.inactive_bank_index = 1U;
  return c;
}

/* =============================================================================
 * Tests
 * ============================================================================= */

/**
 * @brief Verify internal_validate_cfg_flash rejects bank_size > k_ra8_ota_max_image_bytes.
 *
 * @details Covers libs/ra8_ota/src/ra8_ota_parse.c line 202.
 *          The existing test suite tests bank_size == 0; this test adds the
 *          upper-bound guard (bank_size > 0x80000 = 524288).
 *
 * @pre  None (pure validator; no module init required).
 * @pre  priv_make_cfg() returns a valid config.
 * @post priv_ota_validate_cfg returns k_ra8_err_invalid_arg.
 * @post No module state mutated.
 *
 * @par MC/DC:
 * Decision: `cfg->flash.bank_size_bytes > k_ra8_ota_max_image_bytes` (1 condition).
 * - This test: bank_size = k_ra8_ota_max_image_bytes + 1 -> true -> line 202.
 * - Existing test_ra8_ota.c test_init_validation (bank_size = 4096) covers false.
 *
 * @since 0.1.0
 */
static void test_validate_cfg_flash_bank_too_large(void)
{
  TEST_BEGIN("parse_cov: validate_cfg flash bank_size > max -> line 202");
  ra8_ota_cfg_t c         = priv_make_cfg();
  c.flash.bank_size_bytes = (uint32_t)k_ra8_ota_max_image_bytes + 1U;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, priv_ota_validate_cfg(&c));
  TEST_END("parse_cov: validate_cfg flash bank_size > max -> line 202");
}

/**
 * @brief Verify priv_ota_validate_cfg propagates a crypto sub-validator error.
 *
 * @details Covers libs/ra8_ota/src/ra8_ota_parse.c line 216.
 *          Net pointers are all valid so internal_validate_cfg_net returns k_ra8_ok.
 *          crypto.sha256_init is NULL so internal_validate_cfg_crypto returns
 *          k_ra8_err_null_ptr which is propagated at line 216.
 *
 * @pre  None (pure validator; no module init required).
 * @pre  priv_make_cfg() returns a valid config.
 * @post priv_ota_validate_cfg returns k_ra8_err_null_ptr.
 * @post No module state mutated.
 *
 * @par MC/DC:
 * Decision: `e != k_ra8_ok` after internal_validate_cfg_crypto (1 condition).
 * - This test: crypto.sha256_init NULL -> e = k_ra8_err_null_ptr -> true -> line 216.
 * - Existing test_ra8_ota.c (full validation success) covers the false arm.
 *
 * @since 0.1.0
 */
static void test_validate_cfg_crypto_null(void)
{
  TEST_BEGIN("parse_cov: validate_cfg crypto null propagates -> line 216");
  ra8_ota_cfg_t c      = priv_make_cfg();
  c.crypto.sha256_init = nullptr;
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, priv_ota_validate_cfg(&c));
  TEST_END("parse_cov: validate_cfg crypto null propagates -> line 216");
}

/**
 * @brief Verify internal_json_str returns invalid_arg when no opening quote follows
 *        the key (line 268).
 *
 * @details Covers libs/ra8_ota/src/ra8_ota_parse.c line 268.
 *          The JSON contains the key `"version"` but the value is unquoted and
 *          no further `"` appears in the buffer, so strchr for the opening
 *          value-quote returns NULL.
 *          Exercised transitively: priv_ota_manifest_decode calls
 *          internal_json_str for the version field first.
 *
 * @pre  None (pure parser; no module init required).
 * @pre  json is NUL-terminated.
 * @post priv_ota_manifest_decode returns k_ra8_err_invalid_arg.
 * @post No module state mutated.
 *
 * @par MC/DC:
 * Decision: `p == nullptr` after strchr for opening value-quote (1 condition).
 * - This test: no `"` after key -> NULL -> true -> line 268.
 * - Normal parsing path covers the false arm.
 *
 * @since 0.1.0
 */
static void test_json_str_no_open_quote(void)
{
  TEST_BEGIN("parse_cov: internal_json_str no opening quote -> line 268");
  ra8_ota_manifest_t m = {};
  /* "version" key present; value is unquoted; no further `"` in the buffer. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, priv_ota_manifest_decode("{\"version\":nostring}", &m));
  TEST_END("parse_cov: internal_json_str no opening quote -> line 268");
}

/**
 * @brief Verify internal_json_str returns invalid_arg when the value has no closing
 *        quote (line 273).
 *
 * @details Covers libs/ra8_ota/src/ra8_ota_parse.c line 273.
 *          The opening quote for the version value is found; the string is then
 *          unterminated (no closing `"`), so the second strchr returns NULL.
 *
 * @pre  None (pure parser; no module init required).
 * @pre  json is NUL-terminated.
 * @post priv_ota_manifest_decode returns k_ra8_err_invalid_arg.
 * @post No module state mutated.
 *
 * @par MC/DC:
 * Decision: `q == nullptr` after strchr for closing value-quote (1 condition).
 * - This test: opening quote found, no closing quote -> NULL -> true -> line 273.
 * - Normal parsing path covers the false arm.
 *
 * @since 0.1.0
 */
static void test_json_str_no_close_quote(void)
{
  TEST_BEGIN("parse_cov: internal_json_str no closing quote -> line 273");
  ra8_ota_manifest_t m = {};
  /* Opening quote of value found; buffer ends without a closing quote. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, priv_ota_manifest_decode("{\"version\":\"unclosed}", &m));
  TEST_END("parse_cov: internal_json_str no closing quote -> line 273");
}

/**
 * @brief Verify internal_json_str returns invalid_size when the string value
 *        exceeds the destination capacity (line 277).
 *
 * @details Covers libs/ra8_ota/src/ra8_ota_parse.c line 277.
 *          k_ra8_ota_version_str_bytes = 32 bytes. A version value of 32
 *          characters gives n = 32, n + 1 = 33 > cap (32) -> line 277.
 *
 * @pre  None (pure parser; no module init required).
 * @pre  json is NUL-terminated.
 * @post priv_ota_manifest_decode returns k_ra8_err_invalid_size.
 * @post No module state mutated.
 *
 * @par MC/DC:
 * Decision: `n + 1U > cap` (1 condition).
 * - This test: n = 32, cap = 32 -> 33 > 32 -> true -> line 277.
 * - Normal path (n <= 30) covers the false arm.
 *
 * @since 0.1.0
 */
static void test_json_str_value_too_long(void)
{
  TEST_BEGIN("parse_cov: internal_json_str value exceeds cap -> line 277");
  ra8_ota_manifest_t m = {};
  /* Version value is 32 chars; cap is 32; n+1 = 33 overflows. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_size,
                 priv_ota_manifest_decode("{\"version\":\"12345678901234567890123456789012\","
                                          "\"url\":\"x\"}",
                                          &m));
  TEST_END("parse_cov: internal_json_str value exceeds cap -> line 277");
}

/**
 * @brief Verify priv_ota_json_u32 returns invalid_arg when the key is
 *        absent from the JSON document (line 313).
 *
 * @details Covers libs/ra8_ota/src/ra8_ota_parse.c line 313.
 *          Called directly (exported through ra8_ota_internal.h).
 *
 * @pre  None (pure parser; no module init required).
 * @pre  json and key are NUL-terminated; out_v is non-NULL.
 * @post priv_ota_json_u32 returns k_ra8_err_invalid_arg.
 * @post out_v is unchanged on failure.
 *
 * @par MC/DC:
 * Decision: `p == nullptr` after strstr for key (1 condition).
 * - This test: "size" absent from document -> NULL -> true -> line 313.
 * - test_ra8_ota.c covers the false arm (key present).
 *
 * @since 0.1.0
 */
static void test_json_u32_key_not_found(void)
{
  TEST_BEGIN("parse_cov: priv_ota_json_u32 key not found -> line 313");
  uint32_t v = k_t_out_word_seed;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, priv_ota_json_u32("{\"foo\":42}", "\"size\"", &v));
  /* out_v must be unchanged on failure. */
  TEST_ASSERT_EQ(0xDEADBEEFUL, v);
  TEST_END("parse_cov: priv_ota_json_u32 key not found -> line 313");
}

/**
 * @brief Verify priv_ota_json_u32 returns invalid_arg when there are no
 *        digit characters after the skip loop (line 334).
 *
 * @details Covers libs/ra8_ota/src/ra8_ota_parse.c line 334.
 *          The key `"size"` is found; the skip loop advances past `: "`,
 *          then `n` (first char of "notanumber") is not a digit -> i stays 0.
 *
 * @pre  None (pure parser; no module init required).
 * @pre  json and key are NUL-terminated; out_v is non-NULL.
 * @post priv_ota_json_u32 returns k_ra8_err_invalid_arg.
 * @post out_v is unchanged on failure.
 *
 * @par MC/DC:
 * Decision: `i == 0U` after digit loop (1 condition).
 * - This test: first post-skip char is `n` (not a digit) -> i=0 -> true -> line 334.
 * - test_ra8_ota.c covers the false arm (at least one digit consumed).
 *
 * @since 0.1.0
 */
static void test_json_u32_no_digits(void)
{
  TEST_BEGIN("parse_cov: priv_ota_json_u32 no digits -> line 334");
  uint32_t v = 0U;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 priv_ota_json_u32("{\"size\":\"notanumber\"}", "\"size\"", &v));
  TEST_END("parse_cov: priv_ota_json_u32 no digits -> line 334");
}

/**
 * @brief Verify internal_hex_nibble covers the uppercase A-F return path (line 370).
 *
 * @details Covers libs/ra8_ota/src/ra8_ota_parse.c line 370.
 *          Reached transitively through priv_ota_manifest_decode when the
 *          sha256 and signature fields contain uppercase A-F characters.
 *          The manifest is well-formed so the decode succeeds and the returned
 *          fields reflect the uppercase-decoded bytes.
 *
 * @pre  None (pure parser; no module init required).
 * @pre  json is NUL-terminated; out is non-NULL.
 * @post priv_ota_manifest_decode returns k_ra8_ok.
 * @post image_sha256[0] == 0xAA, image_sha256[1] == 0xBB, signature_len == 2.
 *
 * @par MC/DC:
 * Decision: `priv_ota_char_in_range(c, 'A', 'F')` (1 condition).
 * - This test: c in 'A'..'F' -> true -> uppercase return at line 370.
 * - test_mcdc_hex_nibble_pair_completion in test_ra8_ota.c (c = ':') covers false.
 *
 * @since 0.1.0
 */
static void test_hex_nibble_uppercase(void)
{
  TEST_BEGIN("parse_cov: internal_hex_nibble uppercase A-F arm -> line 370");
  ra8_ota_manifest_t m = {};
  /* sha256: 64 uppercase hex chars (32 bytes). signature: "CAFE" (2 bytes). */
  const ra8_err_t e = priv_ota_manifest_decode("{ \"version\": \"1.0.0\","
                                               " \"url\": \"https://x\","
                                               " \"size\": 256,"
                                               " \"sha256\": \"AABBCCDDEEFF00112233445566778899"
                                               "AABBCCDDEEFF00112233445566778899\","
                                               " \"signature\": \"CAFE\" }",
                                               &m);
  TEST_ASSERT_EQ(k_ra8_ok, e);
  TEST_ASSERT_EQ(0xAAU, m.image_sha256[0]);
  TEST_ASSERT_EQ(0xBBU, m.image_sha256[1]);
  TEST_ASSERT_EQ(256U, m.image_size_bytes);
  TEST_ASSERT_EQ(2U, m.signature_len);
  TEST_END("parse_cov: internal_hex_nibble uppercase A-F arm -> line 370");
}

/**
 * @brief Verify internal_hex_decode returns 0 for an odd-length hex string (line 402).
 *
 * @details Covers libs/ra8_ota/src/ra8_ota_parse.c line 402.
 *          Reached transitively through priv_ota_manifest_decode: the
 *          sha256 field value is "abc" (3 chars, odd length), which causes
 *          internal_hex_decode to return 0 immediately.
 *
 * @pre  None (pure parser; no module init required).
 * @pre  json is NUL-terminated; out is non-NULL.
 * @post priv_ota_manifest_decode returns k_ra8_err_invalid_arg.
 * @post No module state mutated.
 *
 * @par MC/DC:
 * Decision: `(in_len % 2) != 0U` (1 condition).
 * - This test: sha256 hex "abc" -> len=3, 3%2=1 -> true -> line 402.
 * - Normal even-length path covers the false arm.
 *
 * @since 0.1.0
 */
static void test_hex_decode_odd_length(void)
{
  TEST_BEGIN("parse_cov: internal_hex_decode odd-length hex -> line 402");
  ra8_ota_manifest_t m = {};
  /* sha256 hex value "abc" has odd length -> internal_hex_decode returns 0. */
  const ra8_err_t e = priv_ota_manifest_decode("{ \"version\": \"1.0.0\","
                                               " \"url\": \"https://x\","
                                               " \"size\": 256,"
                                               " \"sha256\": \"abc\","
                                               " \"signature\": \"aabb\" }",
                                               &m);
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, e);
  TEST_END("parse_cov: internal_hex_decode odd-length hex -> line 402");
}

/**
 * @brief Verify internal_hex_decode returns 0 when decoded bytes exceed out_cap
 *        (line 406).
 *
 * @details Covers libs/ra8_ota/src/ra8_ota_parse.c line 406.
 *          k_ra8_ota_sha256_bytes = 32 is the out_cap for the sha256 field.
 *          A 66-char sha256 hex value decodes to 33 bytes, exceeding the cap.
 *
 * @pre  None (pure parser; no module init required).
 * @pre  json is NUL-terminated; out is non-NULL.
 * @post priv_ota_manifest_decode returns k_ra8_err_invalid_arg.
 * @post No module state mutated.
 *
 * @par MC/DC:
 * Decision: `bytes > out_cap` (1 condition).
 * - This test: 66-char sha256 hex -> 33 bytes > 32 (out_cap) -> true -> line 406.
 * - Normal 64-char sha256 (32 bytes == out_cap) covers the false arm.
 *
 * @since 0.1.0
 */
static void test_hex_decode_exceeds_cap(void)
{
  TEST_BEGIN("parse_cov: internal_hex_decode bytes > out_cap -> line 406");
  ra8_ota_manifest_t m = {};
  /* sha256 hex: 66 even-length chars -> 33 bytes > k_ra8_ota_sha256_bytes (32). */
  const ra8_err_t e = priv_ota_manifest_decode("{ \"version\": \"1.0.0\","
                                               " \"url\": \"https://x\","
                                               " \"size\": 256,"
                                               " \"sha256\": \"aabbccddeeff00112233445566778899"
                                               "aabbccddeeff00112233445566778899aa\","
                                               " \"signature\": \"aabb\" }",
                                               &m);
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, e);
  TEST_END("parse_cov: internal_hex_decode bytes > out_cap -> line 406");
}

/**
 * @brief Verify internal_manifest_decode_crypto propagates error when sha256 key is
 *        absent (line 452).
 *
 * @details Covers libs/ra8_ota/src/ra8_ota_parse.c line 452.
 *          The version, url, and size fields parse and validate successfully so
 *          internal_manifest_decode_crypto is entered. The sha256 key is absent ->
 *          internal_json_str returns k_ra8_err_invalid_arg -> line 452.
 *
 * @pre  None (pure parser; no module init required).
 * @pre  json is NUL-terminated; out is non-NULL.
 * @post priv_ota_manifest_decode returns k_ra8_err_invalid_arg.
 * @post No module state mutated.
 *
 * @par MC/DC:
 * Decision: `e != k_ra8_ok` after internal_json_str for sha256 (1 condition).
 * - This test: sha256 key absent -> error -> true -> line 452.
 * - Normal path (sha256 present and valid) covers the false arm.
 *
 * @since 0.1.0
 */
static void test_manifest_decode_no_sha256(void)
{
  TEST_BEGIN("parse_cov: internal_manifest_decode_crypto sha256 missing -> line 452");
  ra8_ota_manifest_t m = {};
  /* version, url, size present; sha256 absent. */
  const ra8_err_t e = priv_ota_manifest_decode("{ \"version\": \"1.0.0\","
                                               " \"url\": \"https://x\","
                                               " \"size\": 256 }",
                                               &m);
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, e);
  TEST_END("parse_cov: internal_manifest_decode_crypto sha256 missing -> line 452");
}

/**
 * @brief Verify internal_manifest_decode_crypto propagates error when signature key
 *        is absent (line 461).
 *
 * @details Covers libs/ra8_ota/src/ra8_ota_parse.c line 461.
 *          sha256 is a valid 64-char lowercase hex string (32 bytes, n_d == 32).
 *          The signature key is absent -> internal_json_str returns
 *          k_ra8_err_invalid_arg -> line 461.
 *
 * @pre  None (pure parser; no module init required).
 * @pre  json is NUL-terminated; out is non-NULL.
 * @post priv_ota_manifest_decode returns k_ra8_err_invalid_arg.
 * @post No module state mutated.
 *
 * @par MC/DC:
 * Decision: `e != k_ra8_ok` after internal_json_str for signature (1 condition).
 * - This test: signature key absent -> error -> true -> line 461.
 * - Normal path (signature present) covers the false arm.
 *
 * @since 0.1.0
 */
static void test_manifest_decode_no_signature(void)
{
  TEST_BEGIN("parse_cov: internal_manifest_decode_crypto signature missing -> line 461");
  ra8_ota_manifest_t m = {};
  /* sha256: 64 lowercase hex chars (32 bytes). signature key absent. */
  const ra8_err_t e = priv_ota_manifest_decode("{ \"version\": \"1.0.0\","
                                               " \"url\": \"https://x\","
                                               " \"size\": 256,"
                                               " \"sha256\": \"aabbccddeeff00112233445566778899"
                                               "aabbccddeeff00112233445566778899\" }",
                                               &m);
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, e);
  TEST_END("parse_cov: internal_manifest_decode_crypto signature missing -> line 461");
}

/**
 * @brief Verify internal_manifest_decode_crypto returns invalid_arg when the
 *        signature hex string decodes to zero bytes (line 465).
 *
 * @details Covers libs/ra8_ota/src/ra8_ota_parse.c line 465.
 *          sha256 decodes successfully (n_d == 32). The signature value "abc"
 *          is odd-length so internal_hex_decode returns 0 -> n_s == 0 -> line 465.
 *
 * @pre  None (pure parser; no module init required).
 * @pre  json is NUL-terminated; out is non-NULL.
 * @post priv_ota_manifest_decode returns k_ra8_err_invalid_arg.
 * @post No module state mutated.
 *
 * @par MC/DC:
 * Decision: `n_s == 0U` (1 condition).
 * - This test: odd-length signature hex -> internal_hex_decode = 0 -> true -> line 465.
 * - Normal path (even-length, non-empty signature hex) covers the false arm.
 *
 * @since 0.1.0
 */
static void test_manifest_decode_sig_malformed(void)
{
  TEST_BEGIN("parse_cov: internal_manifest_decode_crypto n_s==0 -> line 465");
  ra8_ota_manifest_t m = {};
  /* sha256 OK (64 chars); signature "abc" is odd-length -> n_s = 0. */
  const ra8_err_t e = priv_ota_manifest_decode("{ \"version\": \"1.0.0\","
                                               " \"url\": \"https://x\","
                                               " \"size\": 256,"
                                               " \"sha256\": \"aabbccddeeff00112233445566778899"
                                               "aabbccddeeff00112233445566778899\","
                                               " \"signature\": \"abc\" }",
                                               &m);
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, e);
  TEST_END("parse_cov: internal_manifest_decode_crypto n_s==0 -> line 465");
}

/**
 * @brief Verify priv_ota_manifest_decode propagates error when the url
 *        key is absent (line 504).
 *
 * @details Covers libs/ra8_ota/src/ra8_ota_parse.c line 504.
 *          The version field parses successfully; the url key is absent ->
 *          internal_json_str returns k_ra8_err_invalid_arg -> line 504.
 *
 * @pre  None (pure parser; no module init required).
 * @pre  json is NUL-terminated; out is non-NULL.
 * @post priv_ota_manifest_decode returns k_ra8_err_invalid_arg.
 * @post No module state mutated.
 *
 * @par MC/DC:
 * Decision: `e != k_ra8_ok` after internal_json_str for url (1 condition).
 * - This test: url key absent -> error -> true -> line 504.
 * - Normal path (url present) covers the false arm.
 *
 * @since 0.1.0
 */
static void test_manifest_decode_no_url(void)
{
  TEST_BEGIN("parse_cov: manifest_decode url missing -> line 504");
  ra8_ota_manifest_t m = {};
  /* version present; url absent. */
  const ra8_err_t e = priv_ota_manifest_decode("{ \"version\": \"1.0.0\" }", &m);
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, e);
  TEST_END("parse_cov: manifest_decode url missing -> line 504");
}

/**
 * @brief Verify priv_ota_manifest_decode propagates error when the size
 *        key is absent (line 508).
 *
 * @details Covers libs/ra8_ota/src/ra8_ota_parse.c line 508.
 *          version and url parse successfully; the size key is absent ->
 *          priv_ota_json_u32 returns k_ra8_err_invalid_arg -> line 508.
 *
 * @pre  None (pure parser; no module init required).
 * @pre  json is NUL-terminated; out is non-NULL.
 * @post priv_ota_manifest_decode returns k_ra8_err_invalid_arg.
 * @post No module state mutated.
 *
 * @par MC/DC:
 * Decision: `e != k_ra8_ok` after priv_ota_json_u32 for size (1 condition).
 * - This test: size key absent -> error -> true -> line 508.
 * - Normal path (size present) covers the false arm.
 *
 * @since 0.1.0
 */
static void test_manifest_decode_no_size(void)
{
  TEST_BEGIN("parse_cov: manifest_decode size missing -> line 508");
  ra8_ota_manifest_t m = {};
  /* version and url present; size absent. */
  const ra8_err_t e =
    priv_ota_manifest_decode("{ \"version\": \"1.0.0\", \"url\": \"https://x\" }", &m);
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, e);
  TEST_END("parse_cov: manifest_decode size missing -> line 508");
}

/**
 * @brief Verify priv_ota_manifest_decode returns invalid_arg when
 *        image_size_bytes is zero (line 511).
 *
 * @details Covers libs/ra8_ota/src/ra8_ota_parse.c line 511.
 *          version, url, and size all parse successfully; size value is 0 ->
 *          image_size_bytes == 0 -> line 511.
 *
 * @pre  None (pure parser; no module init required).
 * @pre  json is NUL-terminated; out is non-NULL.
 * @post priv_ota_manifest_decode returns k_ra8_err_invalid_arg.
 * @post No module state mutated.
 *
 * @par MC/DC:
 * Decision: `out->image_size_bytes == 0U` (1 condition).
 * - This test: size = 0 -> true -> line 511.
 * - Normal path (size > 0) covers the false arm.
 *
 * @since 0.1.0
 */
static void test_manifest_decode_zero_size(void)
{
  TEST_BEGIN("parse_cov: manifest_decode image_size_bytes == 0 -> line 511");
  ra8_ota_manifest_t m = {};
  const ra8_err_t    e = priv_ota_manifest_decode("{ \"version\": \"1.0.0\","
                                                  " \"url\": \"https://x\","
                                                  " \"size\": 0 }",
                                                  &m);
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, e);
  TEST_END("parse_cov: manifest_decode image_size_bytes == 0 -> line 511");
}

/**
 * @brief Verify priv_ota_manifest_decode returns invalid_size when
 *        image_size_bytes exceeds k_ra8_ota_max_image_bytes (line 514).
 *
 * @details Covers libs/ra8_ota/src/ra8_ota_parse.c line 514.
 *          k_ra8_ota_max_image_bytes = 0x80000 = 524288. size = 524289 exceeds
 *          the cap -> line 514.
 *
 * @pre  None (pure parser; no module init required).
 * @pre  json is NUL-terminated; out is non-NULL.
 * @post priv_ota_manifest_decode returns k_ra8_err_invalid_size.
 * @post No module state mutated.
 *
 * @par MC/DC:
 * Decision: `out->image_size_bytes > k_ra8_ota_max_image_bytes` (1 condition).
 * - This test: size = 524289 (0x80001) > 524288 (0x80000) -> true -> line 514.
 * - Normal path (size <= 524288) covers the false arm.
 *
 * @since 0.1.0
 */
static void test_manifest_decode_size_exceeds_max(void)
{
  TEST_BEGIN("parse_cov: manifest_decode image_size > max -> line 514");
  ra8_ota_manifest_t m = {};
  /* 524289 = 0x80001 > k_ra8_ota_max_image_bytes (0x80000 = 524288). */
  const ra8_err_t e = priv_ota_manifest_decode("{ \"version\": \"1.0.0\","
                                               " \"url\": \"https://x\","
                                               " \"size\": 524289 }",
                                               &m);
  TEST_ASSERT_EQ(k_ra8_err_invalid_size, e);
  TEST_END("parse_cov: manifest_decode image_size > max -> line 514");
}

/* =============================================================================
 * main
 * ============================================================================= */

int main(void)
{
  test_validate_cfg_flash_bank_too_large();
  test_validate_cfg_crypto_null();
  test_json_str_no_open_quote();
  test_json_str_no_close_quote();
  test_json_str_value_too_long();
  test_json_u32_key_not_found();
  test_json_u32_no_digits();
  test_hex_nibble_uppercase();
  test_hex_decode_odd_length();
  test_hex_decode_exceeds_cap();
  test_manifest_decode_no_sha256();
  test_manifest_decode_no_signature();
  test_manifest_decode_sig_malformed();
  test_manifest_decode_no_url();
  test_manifest_decode_no_size();
  test_manifest_decode_zero_size();
  test_manifest_decode_size_exceeds_max();
  return 0;
}
