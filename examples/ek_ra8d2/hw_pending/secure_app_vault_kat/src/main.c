/**
 * @file examples/ek_ra8d2/hw_pending/secure_app_vault_kat/src/main.c
 * @brief ra8_secure_app consumer: key-vault + OTA-commit contract self-check
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * ``libs/ra8_secure_app`` is first-party and host-tested, but until this app
 * no ``apps/`` or ``examples/`` composition root included ``key_vault.h`` or
 * ``ota_commit.h``, so nothing proved the public C API is usable from a real
 * firmware image and nothing caught an ABI or wiring regression that the host
 * tests' own fakes paper over (#922). This app is that consumer.
 *
 * It drives every published entry point of both headers once, from a
 * secure-world composition root on real silicon, and checks each return
 * against what that function's own documentation promises for the crypto
 * configuration the image was built in:
 *
 *   - Default image (``RA8_INSECURE_STUB_CRYPTO`` off): the placeholder key
 *     vault must **fail closed**. Every vault entry point has to return
 *     ``k_ra8_err_not_supported``, the digest buffer has to come back
 *     untouched, and the NULL-pointer checks still have to fire *ahead* of
 *     the fail-closed return. That is the property #180 added the guard for,
 *     and this is the first place it is checked from an application.
 *   - Declared dev/eval image (``-DRA8_INSECURE_STUB_CRYPTO=ON``): the vault
 *     answers for real, so the challenge path is checked against a SHA-256
 *     known-answer vector computed off-target from FIPS 180-4, plus the
 *     documented range, NULL and KAK-length error paths.
 *
 * ``ota_commit.h`` behaves the same way in both images: its option-region
 * writes are bench-gated, so on silicon ``ra8_ota_commit_swap_bank`` and
 * ``ra8_ota_commit_set_bank_config`` are fail-closed (T5-10) while the reset,
 * read-back and argument-validation paths are live. Both directions are
 * checked: the invalid argument must be rejected *before* the fail-closed
 * return, and the shadow must still read empty afterwards, so a caller can
 * never mistake an unwritten option byte for an armed swap.
 *
 * Bring-up sequence (CGC -> MSTP -> TIME -> console):
 *   1. CGC + SysTick + UART console (SCI8 on PD_02 / PD_03 -> J-Link OB).
 *   2. Run the vault checks, then the OTA-commit checks, logging
 *      ``kat: step=N rc=E PASS|FAIL`` per step (``rc`` is the raw
 *      ``ra8_err_t``, so a bench operator sees which entry point disagreed).
 *   3. Emit ``kat: secure_app PASS`` only when every step matched, then
 *      repeat that verdict once per ``k_kat_period_ms`` for the UART scrape.
 *
 * Bare EK-RA8D2; no expansion board, no external wiring, no fused part. The
 * checks are pure API contract, so nothing here touches key material,
 * signing keys or the option region.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stddef.h>
#include <stdint.h>

#include "key_vault.h"
#include "ota_commit.h"
#include "ra8_board_ek_ra8d2.h"
#include "ra8_boot_entry.h"
#include "ra8_cgc.h"
#include "ra8_err.h"
#include "ra8_mstp.h"
#include "ra8_time.h"

/** @brief Demo tunables. */
typedef enum : uint32_t {
  k_kat_baud      = 115200U, /**< SCI8 console baud.            */
  k_kat_period_ms = 500U,    /**< Delay between verdict prints. */
} kat_const_t;

/** @brief Formatting constants. */
typedef enum : uint8_t {
  k_kat_radix   = 10U, /**< Decimal serialiser radix.          */
  k_kat_dec_max = 10U, /**< Max decimal digits for a uint32_t. */
} kat_fmt_t;

/** @brief Argument values the headers document as invalid. */
typedef enum : uint16_t {
  k_kat_bad_slot      = (uint16_t)k_ra8_key_vault_slots, /**< First out-of-range slot.     */
  k_kat_bad_mac_len   = 24U,                             /**< Neither AES-128 nor AES-256. */
  k_kat_short_mac_cap = 16U,                             /**< Too small for a 32-byte KAK. */
} kat_bad_arg_t;

/** @brief Bank selector outside ``ra8_ota_bank_t``. */
typedef enum : uint8_t {
  k_kat_bad_bank = 2U, /**< One past k_ra8_ota_bank_b. */
} kat_bad_bank_t;

/**
 * @brief Vault key programmed into slot 0 by this app.
 *
 * @details Fixed vector ``key[i] = 0xA5 ^ i``; paired with
 * ::k_kat_challenge and ::k_kat_expect_digest.
 *
 * @since 0.1.0
 */
static const uint8_t k_kat_key[k_ra8_key_vault_key_bytes] = {
  0xA5U, 0xA4U, 0xA7U, 0xA6U, 0xA1U, 0xA0U, 0xA3U, 0xA2U, 0xADU, 0xACU, 0xAFU,
  0xAEU, 0xA9U, 0xA8U, 0xABU, 0xAAU, 0xB5U, 0xB4U, 0xB7U, 0xB6U, 0xB1U, 0xB0U,
  0xB3U, 0xB2U, 0xBDU, 0xBCU, 0xBFU, 0xBEU, 0xB9U, 0xB8U, 0xBBU, 0xBAU,
};

/**
 * @brief Challenge handed to the vault.
 *
 * @details Fixed vector ``challenge[i] = 0x5A + i``.
 *
 * @since 0.1.0
 */
static const uint8_t k_kat_challenge[k_ra8_key_vault_chal_bytes] = {
  0x5AU, 0x5BU, 0x5CU, 0x5DU, 0x5EU, 0x5FU, 0x60U, 0x61U, 0x62U, 0x63U, 0x64U,
  0x65U, 0x66U, 0x67U, 0x68U, 0x69U, 0x6AU, 0x6BU, 0x6CU, 0x6DU, 0x6EU, 0x6FU,
  0x70U, 0x71U, 0x72U, 0x73U, 0x74U, 0x75U, 0x76U, 0x77U, 0x78U, 0x79U,
};

#if defined(RA8_INSECURE_STUB_CRYPTO)
/**
 * @brief Expected ``SHA-256(key XOR challenge)`` for the two vectors above.
 *
 * @details Computed off-target from FIPS 180-4 over the 32-byte XOR of
 * ::k_kat_key and ::k_kat_challenge, independently of the sponge in
 * ``key_vault.c``, so this is a genuine known-answer test rather than a
 * self-consistency check.
 *
 * @since 0.1.0
 */
static const uint8_t k_kat_expect_digest[k_ra8_key_vault_digest_bytes] = {
  0xD5U, 0xF1U, 0xECU, 0x45U, 0x7EU, 0xE9U, 0x1AU, 0x9BU, 0xC2U, 0x30U, 0x02U,
  0x03U, 0x70U, 0xA7U, 0x73U, 0x55U, 0xD7U, 0x04U, 0xC3U, 0xC9U, 0xAFU, 0xE4U,
  0x10U, 0x07U, 0x43U, 0xCBU, 0xD8U, 0x4BU, 0x75U, 0xDFU, 0x48U, 0x77U,
};
#endif /* RA8_INSECURE_STUB_CRYPTO: the KAT vector is only reachable there. */

/**
 * @brief Key-authentication key (KAK) provisioned by this app.
 *
 * @details Fixed 32-byte vector ``kak[i] = 0x11 + i``; only ever handed to
 * ``ra8_key_vault_set_mac_key`` and compared against the read-back.
 *
 * @since 0.1.0
 */
static const uint8_t k_kat_mac_key[k_ra8_key_vault_mac_key_bytes] = {
  0x11U, 0x12U, 0x13U, 0x14U, 0x15U, 0x16U, 0x17U, 0x18U, 0x19U, 0x1AU, 0x1BU,
  0x1CU, 0x1DU, 0x1EU, 0x1FU, 0x20U, 0x21U, 0x22U, 0x23U, 0x24U, 0x25U, 0x26U,
  0x27U, 0x28U, 0x29U, 0x2AU, 0x2BU, 0x2CU, 0x2DU, 0x2EU, 0x2FU, 0x30U,
};

/* Console line fragments (kept short so each write is one shift-register
 * fill; the periodic verdict is the only repeated output path). */
static const uint8_t k_kat_step_prefix[]   = "kat: step=";
static const uint8_t k_kat_rc_sep[]        = " rc=";
static const uint8_t k_kat_pass_sfx[]      = " PASS\r\n";
static const uint8_t k_kat_fail_sfx[]      = " FAIL\r\n";
#if defined(RA8_INSECURE_STUB_CRYPTO)
static const uint8_t k_kat_mode[]          = "kat: mode=dev-eval (stub crypto on)\r\n";
#else
static const uint8_t k_kat_mode[]          = "kat: mode=production (fail-closed)\r\n";
#endif
static const uint8_t k_kat_verdict_pass[]  = "kat: secure_app PASS\r\n";
static const uint8_t k_kat_verdict_fail[]  = "kat: secure_app FAIL\r\n";

/*
 * Expected returns for the key-vault group. The placeholder vault body in
 * libs/ra8_secure_app/src/key_vault.c is compiled only under
 * RA8_INSECURE_STUB_CRYPTO (or off-target); its #else fails every entry point
 * closed (#180). This app checks whichever half it was built against, so the
 * same source proves the dev/eval answer AND the production refusal.
 */
#if defined(RA8_INSECURE_STUB_CRYPTO)
#define KAT_VAULT_OK          k_ra8_ok
#define KAT_VAULT_BAD_SLOT    k_ra8_err_invalid_arg
#define KAT_VAULT_BAD_MAC_LEN k_ra8_err_invalid_arg
#define KAT_VAULT_UNSET_MAC   k_ra8_err_not_found
#define KAT_VAULT_SHORT_CAP   k_ra8_err_invalid_size
#else
#define KAT_VAULT_OK          k_ra8_err_not_supported
#define KAT_VAULT_BAD_SLOT    k_ra8_err_not_supported
#define KAT_VAULT_BAD_MAC_LEN k_ra8_err_not_supported
#define KAT_VAULT_UNSET_MAC   k_ra8_err_not_supported
#define KAT_VAULT_SHORT_CAP   k_ra8_err_not_supported
#endif

/**
 * @brief Park forever after a fatal bring-up error.
 *
 * @pre Called only after an unrecoverable bring-up failure.
 * @post CPU is parked; only a debugger or reset wakes it.
 * @since 0.1.0
 */
static void kat_panic_halt(void)
{
  while (1) {
    __asm__ volatile("wfi");
  }
}

/**
 * @brief Write a byte span to the SCI8 console, discarding the status.
 *
 * @param[in] data Non-NULL byte span to transmit.
 * @param[in] len  Byte count (0 is a no-op).
 *
 * @pre ``ra8_board_uart_console_init`` has succeeded.
 * @pre ``data`` points at ``len`` readable bytes.
 * @post ``len`` bytes have been queued to the console UART.
 * @since 0.1.0
 */
static void kat_write(const uint8_t* data, uint32_t len)
{
  (void)ra8_board_uart_console_write(data, (size_t)len);
}

/**
 * @brief Serialise an unsigned 32-bit value into decimal ASCII.
 *
 * @param[out] buf Destination, at least ``k_kat_dec_max`` bytes.
 * @param[in]  val Value to serialise.
 *
 * @return Number of digits written (1..10).
 *
 * @pre ``buf`` is non-NULL and sized for the widest uint32_t.
 * @pre No trailing NUL is required by the caller.
 * @post ``buf`` holds the most-significant digit first.
 * @post The return value is in ``[1, k_kat_dec_max]``.
 * @since 0.1.0
 */
static uint32_t kat_u32_to_dec(uint8_t* buf, uint32_t val)
{
  if (val == 0U) {
    buf[0] = (uint8_t)'0';
    return 1U;
  }
  uint8_t  tmp[k_kat_dec_max] = {};
  uint32_t n                  = 0U;
  uint32_t v                  = val;
  while (v != 0U) {
    tmp[n] = (uint8_t)('0' + (uint8_t)(v % (uint32_t)k_kat_radix));
    v      = v / (uint32_t)k_kat_radix;
    n++;
  }
  for (uint32_t i = 0U; i < n; i++) {
    buf[i] = tmp[n - 1U - i];
  }
  return n;
}

/**
 * @brief Log one unsigned 32-bit value as decimal ASCII.
 *
 * @param[in] val Value to print.
 *
 * @pre The console has been initialised.
 * @pre ``val`` fits in a uint32_t (always true).
 * @post The decimal digits of ``val`` have been queued to the console.
 * @since 0.1.0
 */
static void kat_write_u32(uint32_t val)
{
  uint8_t        buf[k_kat_dec_max] = {};
  const uint32_t n                  = kat_u32_to_dec(buf, val);
  kat_write(buf, n);
}

/**
 * @brief Record and log one step: expected versus actual ``ra8_err_t``.
 *
 * @param[in] step   Step number, 1-based, stable across builds.
 * @param[in] expect Return the called function's documentation promises.
 * @param[in] actual Return the call produced.
 *
 * @return True when ``actual == expect``.
 *
 * @pre The console has been initialised.
 * @pre ``step`` is the caller's own sequence number.
 * @post One ``kat: step=N rc=E PASS|FAIL`` line has been queued.
 * @post No library state is touched.
 * @since 0.1.0
 */
[[nodiscard]] static bool kat_step(uint32_t step, ra8_err_t expect, ra8_err_t actual)
{
  const bool ok = (actual == expect);
  kat_write(k_kat_step_prefix, (uint32_t)(sizeof(k_kat_step_prefix) - 1U));
  kat_write_u32(step);
  kat_write(k_kat_rc_sep, (uint32_t)(sizeof(k_kat_rc_sep) - 1U));
  kat_write_u32((uint32_t)actual);
  if (ok) {
    kat_write(k_kat_pass_sfx, (uint32_t)(sizeof(k_kat_pass_sfx) - 1U));
  } else {
    kat_write(k_kat_fail_sfx, (uint32_t)(sizeof(k_kat_fail_sfx) - 1U));
  }
  return ok;
}

/**
 * @brief Log one boolean step that is not an ``ra8_err_t`` comparison.
 *
 * @param[in] step Step number, 1-based.
 * @param[in] ok   Verdict for this step.
 *
 * @return ``ok``, unchanged.
 *
 * @pre The console has been initialised.
 * @pre ``step`` is the caller's own sequence number.
 * @post One step line has been queued with ``rc=0``.
 * @since 0.1.0
 */
[[nodiscard]] static bool kat_step_bool(uint32_t step, bool ok)
{
  return kat_step(step, k_ra8_ok, ok ? k_ra8_ok : k_ra8_err_invalid_state);
}

/**
 * @brief Compare two equal-length byte spans.
 *
 * @param[in] a   First span.
 * @param[in] b   Second span.
 * @param[in] len Byte count.
 *
 * @return True when every byte matches.
 *
 * @pre ``a`` and ``b`` point at ``len`` readable bytes.
 * @post Neither span is modified.
 * @since 0.1.0
 */
[[nodiscard]] static bool kat_bytes_equal(const uint8_t* a, const uint8_t* b, uint32_t len)
{
  for (uint32_t i = 0U; i < len; i++) {
    if (a[i] != b[i]) {
      return false;
    }
  }
  return true;
}

/**
 * @brief Drive every ``key_vault.h`` entry point and check its contract.
 *
 * @details
 * Steps 1..16. The expected returns come from the ``KAT_VAULT_*`` macros, so
 * the production image asserts the fail-closed refusals while a declared
 * dev/eval image asserts the real answers, including the SHA-256 KAT. The
 * NULL-pointer steps expect ``k_ra8_err_null_ptr`` in **both** images: the
 * guard must not swallow argument validation.
 *
 * @return True when all 16 steps matched.
 *
 * @pre The console has been initialised.
 * @pre Called once per boot, before ::kat_run_ota_group.
 * @post No key material leaves the secure world; the digest buffer is local.
 * @since 0.1.0
 */
[[nodiscard]] static bool kat_run_vault_group(void)
{
  bool ok = true;

  ok = kat_step(1U, KAT_VAULT_OK, ra8_key_vault_init()) && ok;
  ok = kat_step(2U, KAT_VAULT_OK, ra8_key_vault_store(0U, k_kat_key)) && ok;
  ok = kat_step(3U, KAT_VAULT_BAD_SLOT,
                ra8_key_vault_store((uint16_t)k_kat_bad_slot, k_kat_key))
       && ok;
  ok = kat_step(4U, k_ra8_err_null_ptr, ra8_key_vault_store(0U, NULL)) && ok;

  uint8_t digest[k_ra8_key_vault_digest_bytes] = {};
  ok = kat_step(5U, KAT_VAULT_OK,
                ra8_key_vault_sha256_xor_challenge(0U, k_kat_challenge, digest))
       && ok;
#if defined(RA8_INSECURE_STUB_CRYPTO)
  /* The vault answered, so the digest must be the FIPS 180-4 vector. */
  ok = kat_step_bool(
         6U,
         kat_bytes_equal(digest, k_kat_expect_digest, (uint32_t)k_ra8_key_vault_digest_bytes))
       && ok;
#else
  /* The vault refused, so it must not have written the caller's buffer. */
  const uint8_t zeroes[k_ra8_key_vault_digest_bytes] = {};
  ok = kat_step_bool(
         6U, kat_bytes_equal(digest, zeroes, (uint32_t)k_ra8_key_vault_digest_bytes))
       && ok;
#endif
  ok = kat_step(7U, k_ra8_err_null_ptr,
                ra8_key_vault_sha256_xor_challenge(0U, NULL, digest))
       && ok;
  ok = kat_step(8U, k_ra8_err_null_ptr,
                ra8_key_vault_sha256_xor_challenge(0U, k_kat_challenge, NULL))
       && ok;
  ok = kat_step(9U, KAT_VAULT_BAD_SLOT,
                ra8_key_vault_sha256_xor_challenge((uint16_t)k_kat_bad_slot, k_kat_challenge,
                                                   digest))
       && ok;

  uint8_t  mac_read[k_ra8_key_vault_mac_key_bytes] = {};
  uint16_t mac_len                                 = 0U;
  ok = kat_step(10U, KAT_VAULT_UNSET_MAC,
                ra8_key_vault_load_mac_key(mac_read, (uint16_t)sizeof(mac_read), &mac_len))
       && ok;
  ok = kat_step(11U, k_ra8_err_null_ptr, ra8_key_vault_set_mac_key(NULL, 0U)) && ok;
  ok = kat_step(12U, KAT_VAULT_BAD_MAC_LEN,
                ra8_key_vault_set_mac_key(k_kat_mac_key, (uint16_t)k_kat_bad_mac_len))
       && ok;
  ok = kat_step(13U, KAT_VAULT_OK,
                ra8_key_vault_set_mac_key(k_kat_mac_key,
                                          (uint16_t)k_ra8_key_vault_mac_key_bytes))
       && ok;
  ok = kat_step(14U, KAT_VAULT_SHORT_CAP,
                ra8_key_vault_load_mac_key(mac_read, (uint16_t)k_kat_short_mac_cap, &mac_len))
       && ok;
  const ra8_err_t load_err =
    ra8_key_vault_load_mac_key(mac_read, (uint16_t)sizeof(mac_read), &mac_len);
  ok = kat_step(15U, KAT_VAULT_OK, load_err) && ok;
#if defined(RA8_INSECURE_STUB_CRYPTO)
  ok = kat_step_bool(16U,
                     (mac_len == (uint16_t)k_ra8_key_vault_mac_key_bytes)
                       && kat_bytes_equal(mac_read, k_kat_mac_key,
                                          (uint32_t)k_ra8_key_vault_mac_key_bytes))
       && ok;
#else
  ok = kat_step_bool(16U, mac_len == 0U) && ok;
#endif

  return ok;
}

/**
 * @brief Drive every ``ota_commit.h`` entry point and check its contract.
 *
 * @details
 * Steps 17..27, identical in both images: the option-region writes are
 * bench-gated, so on silicon ``swap_bank`` and ``set_bank_config`` are
 * fail-closed (``k_ra8_err_not_supported``) while reset, read-back and
 * argument validation are live. The read-backs after each refused write are
 * the point: nothing may look armed or persisted.
 *
 * @return True when all 11 steps matched.
 *
 * @pre The console has been initialised.
 * @pre No swap has been requested earlier in this boot.
 * @post The OTA shadow is left empty; no option byte is written.
 * @since 0.1.0
 */
[[nodiscard]] static bool kat_run_ota_group(void)
{
  bool ok = true;

  ok = kat_step(17U, k_ra8_ok, ra8_ota_commit_reset()) && ok;

  ra8_ota_bank_t pending = k_ra8_ota_bank_a;
  ok = kat_step(18U, k_ra8_err_no_data, ra8_ota_commit_pending(&pending)) && ok;
  ok = kat_step(19U, k_ra8_err_null_ptr, ra8_ota_commit_pending(NULL)) && ok;
  ok = kat_step(20U, k_ra8_err_invalid_arg,
                ra8_ota_commit_swap_bank((ra8_ota_bank_t)k_kat_bad_bank))
       && ok;
  /* Bench-gated option-byte write: fail-closed on silicon, never a fake OK. */
  ok = kat_step(21U, k_ra8_err_not_supported, ra8_ota_commit_swap_bank(k_ra8_ota_bank_b)) && ok;
  ok = kat_step(22U, k_ra8_err_no_data, ra8_ota_commit_pending(&pending)) && ok;

  uint32_t bank_cfg = 0U;
  ok = kat_step(23U, k_ra8_ok, ra8_ota_commit_get_bank_config(&bank_cfg)) && ok;
  ok = kat_step_bool(24U, bank_cfg == 0U) && ok;
  ok = kat_step(25U, k_ra8_err_not_supported, ra8_ota_commit_set_bank_config(UINT32_MAX)) && ok;
  const ra8_err_t cfg_err = ra8_ota_commit_get_bank_config(&bank_cfg);
  ok = kat_step_bool(26U, (cfg_err == k_ra8_ok) && (bank_cfg == 0U)) && ok;
  ok = kat_step(27U, k_ra8_err_null_ptr, ra8_ota_commit_get_bank_config(NULL)) && ok;

  return ok;
}

/**
 * @brief Bring up clocks, MSTP, SysTick and the console, or park.
 *
 * @pre Reset default clock tree.
 * @post CGC, MSTP, SysTick and the SCI8 console are ready.
 * @post On any failure the CPU is parked instead of logging blind.
 * @since 0.1.0
 */
static void kat_setup_or_halt(void)
{
  uint32_t cpuclk0_hz = 0U;
  if (ra8_cgc_init() != k_ra8_ok) {
    kat_panic_halt();
  }
  if (ra8_cgc_get_clock_hz(k_ra8_clock_id_cpuclk0, &cpuclk0_hz) != k_ra8_ok) {
    kat_panic_halt();
  }
  if (ra8_mstp_init() != k_ra8_ok) {
    kat_panic_halt();
  }
  if (ra8_time_init(cpuclk0_hz) != k_ra8_ok) {
    kat_panic_halt();
  }
  if (ra8_board_uart_console_init((uint32_t)k_kat_baud) != k_ra8_ok) {
    kat_panic_halt();
  }
}

void main(void)
{
  kat_setup_or_halt();

  kat_write(k_kat_mode, (uint32_t)(sizeof(k_kat_mode) - 1U));

  bool ok = kat_run_vault_group();
  ok      = kat_run_ota_group() && ok;

  while (1) {
    if (ok) {
      kat_write(k_kat_verdict_pass, (uint32_t)(sizeof(k_kat_verdict_pass) - 1U));
    } else {
      kat_write(k_kat_verdict_fail, (uint32_t)(sizeof(k_kat_verdict_fail) - 1U));
    }
    ra8_delay_ms((uint32_t)k_kat_period_ms);
  }
}
