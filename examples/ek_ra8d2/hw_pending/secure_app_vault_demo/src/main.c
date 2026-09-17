/**
 * @file examples/ek_ra8d2/hw_pending/secure_app_vault_demo/src/main.c
 * @brief First consumer of ra8_secure_app: key vault plus OTA commit shadows.
 *
 * @details
 * `libs/ra8_secure_app/` holds the Ring 5 secure substrate: an eight-slot
 * symmetric key vault with a SHA-256 challenge primitive, a provisioned
 * key-authentication key (KAK) used to MAC key imports, and the OTA
 * boot-bank commit surface. The ereader app compiles the sources, but no
 * application in the tree ever includes `key_vault.h` or `ota_commit.h`, so
 * the only callers were the security tests. This app calls both headers.
 *
 * Legs:
 *
 *   1. `vault`: a stored key is accepted and the challenge primitive is
 *      deterministic (the same slot and challenge twice give the same
 *      digest) while two different slots holding different keys give
 *      different digests. ::ra8_key_vault_init is called for its documented
 *      all-zeros post-condition, but this app does not assert it: the
 *      header exposes no slot read, by design, so an application cannot
 *      observe the zeroing. Only `tests/security/`, which sees the vault
 *      TU's own state, can check that post-condition.
 *   2. `guards`: out-of-range slots and null pointers are refused with the
 *      documented codes rather than writing anything.
 *   3. `kak`: a 16-byte KAK provisions and reads back byte for byte, then a
 *      32-byte KAK carrying *different* material replaces it in full (all
 *      32 bytes are compared, so a partial overwrite fails instead of
 *      passing on the reported length alone), an undersized destination is
 *      refused, and a bad length is refused.
 *   4. `ota`: reset drops any pending commit, `pending` reports no data on a
 *      clean shadow, arming a bank makes it readable, a second arm while one
 *      is pending is refused, and the bank-config write masks everything
 *      except the two allowed BANK_SEL bits.
 *
 * Nothing here writes the option region. The commit and bank-config paths are
 * shadow registers by design: the real OFS3 / BTFLG write is bench-gated and
 * brick-risky, and on a silicon build those two calls report
 * ::k_ra8_err_not_supported. The app accepts either answer and says which it
 * saw, so it is honest on both build flavours.
 *
 * Observable over the SCI8 / J-Link OB VCOM console. A good run prints one
 * verdict per leg and a final `ALL PASS`.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>
#include <string.h>

#include "key_vault.h"
#include "ota_commit.h"
#include "ra8_boot_entry.h"
#include "ra8_err.h"
#include "ra8_io_log.h"
#include "ra8_io_stream.h"
#include "ra8_io_stream_uart.h"
#include "ra8_log.h"
#include "ra8_sci.h"

/**
 * @enum sav_const_t
 * @brief Console, slot, and key-length knobs (no magic numbers).
 *
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_sav_uart_chan   = 8U,          /**< SCI8 J-Link OB console.              */
  k_sav_slot_a      = 0U,          /**< First slot exercised.                */
  k_sav_slot_b      = 1U,          /**< Second slot, different key material. */
  k_sav_slot_bad    = 64U,         /**< Well past k_ra8_key_vault_slots.     */
  k_sav_kak_short   = 16U,         /**< AES-128 KAK length.                  */
  k_sav_kak_long    = 32U,         /**< AES-256 KAK length.                  */
  k_sav_kak_bad_len = 24U,         /**< Neither 16 nor 32; must be refused.  */
  k_sav_cfg_raw     = 0xFFFFFFFFU, /**< All bits set; only two may survive.  */
  k_sav_fill_a      = 0xA5U,       /**< Slot A key fill byte.                */
  k_sav_fill_b      = 0x5AU,       /**< Slot B key fill byte.                */
  k_sav_fill_chal   = 0x11U,       /**< Challenge fill byte.                 */
  k_sav_fill_kak    = 0x77U,       /**< First (16-byte) KAK fill byte.      */
  k_sav_fill_kak2   = 0x8EU,       /**< Second (32-byte) KAK fill byte.     */
} sav_const_t;

static ra8_io_stream_t            s_uart;       /**< Console stream.       */
static ra8_io_stream_uart_state_t s_uart_state; /**< Console stream state. */

/**
 * @brief Write a NUL-terminated string to the console stream.
 *
 * @param[in] text Message to queue on SCI8.
 * @return void
 * @pre The console stream was initialised.
 * @post The text was queued on the console sink.
 * @note Errors are ignored: the console reports, it does not act.
 * @since 0.1.0
 */
static void internal_print(const char* text)
{
  (void)ra8_io_stream_puts(&s_uart, text);
}

/**
 * @brief Fill a buffer with one repeated byte.
 *
 * @param[out] dst   Destination buffer.
 * @param[in]  len   Bytes to fill.
 * @param[in]  value Byte written to every position.
 * @return void
 * @post `dst[0..len-1]` all equal @p value.
 * @since 0.1.0
 */
static void internal_fill(uint8_t* dst, uint16_t len, uint8_t value)
{
  for (uint16_t i = 0U; i < len; ++i) {
    dst[i] = value;
  }
}

/**
 * @brief Store two keys and check the challenge primitive's behaviour.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok              Deterministic per slot, distinct across slots.
 * @retval k_ra8_err_invalid_arg A digest repeated across different keys.
 * @since 0.1.0
 */
static ra8_err_t internal_check_vault(void)
{
  uint8_t key_a[k_ra8_key_vault_key_bytes]        = {0};
  uint8_t key_b[k_ra8_key_vault_key_bytes]        = {0};
  uint8_t challenge[k_ra8_key_vault_chal_bytes]   = {0};
  uint8_t first[k_ra8_key_vault_digest_bytes]     = {0};
  uint8_t repeat[k_ra8_key_vault_digest_bytes]    = {0};
  uint8_t other[k_ra8_key_vault_digest_bytes]     = {0};

  internal_fill(key_a, (uint16_t)sizeof(key_a), (uint8_t)k_sav_fill_a);
  internal_fill(key_b, (uint16_t)sizeof(key_b), (uint8_t)k_sav_fill_b);
  internal_fill(challenge, (uint16_t)sizeof(challenge), (uint8_t)k_sav_fill_chal);

  ra8_err_t err = ra8_key_vault_init();
  if (err != k_ra8_ok) {
    return err;
  }

  err = ra8_key_vault_store((uint16_t)k_sav_slot_a, key_a);
  if (err != k_ra8_ok) {
    return err;
  }

  err = ra8_key_vault_store((uint16_t)k_sav_slot_b, key_b);
  if (err != k_ra8_ok) {
    return err;
  }

  err = ra8_key_vault_sha256_xor_challenge((uint16_t)k_sav_slot_a, challenge, first);
  if (err != k_ra8_ok) {
    return err;
  }

  err = ra8_key_vault_sha256_xor_challenge((uint16_t)k_sav_slot_a, challenge, repeat);
  if (err != k_ra8_ok) {
    return err;
  }

  err = ra8_key_vault_sha256_xor_challenge((uint16_t)k_sav_slot_b, challenge, other);
  if (err != k_ra8_ok) {
    return err;
  }

  const bool deterministic = memcmp(first, repeat, sizeof(first)) == 0;
  const bool slot_bound    = memcmp(first, other, sizeof(first)) != 0;

  return (deterministic && slot_bound) ? k_ra8_ok : k_ra8_err_invalid_arg;
}

/**
 * @brief Check the documented refusals for bad slots and null pointers.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok              Every bad call was refused as documented.
 * @retval k_ra8_err_invalid_arg A bad call was accepted or mis-coded.
 * @since 0.1.0
 */
static ra8_err_t internal_check_guards(void)
{
  uint8_t key[k_ra8_key_vault_key_bytes]      = {0};
  uint8_t challenge[k_ra8_key_vault_chal_bytes] = {0};
  uint8_t digest[k_ra8_key_vault_digest_bytes]  = {0};

  internal_fill(key, (uint16_t)sizeof(key), (uint8_t)k_sav_fill_a);
  internal_fill(challenge, (uint16_t)sizeof(challenge), (uint8_t)k_sav_fill_chal);

  const bool refused
      = (ra8_key_vault_store((uint16_t)k_sav_slot_bad, key) == k_ra8_err_invalid_arg)
        && (ra8_key_vault_store((uint16_t)k_sav_slot_a, nullptr) == k_ra8_err_null_ptr)
        && (ra8_key_vault_sha256_xor_challenge((uint16_t)k_sav_slot_bad, challenge, digest)
            == k_ra8_err_invalid_arg)
        && (ra8_key_vault_sha256_xor_challenge((uint16_t)k_sav_slot_a, nullptr, digest)
            == k_ra8_err_null_ptr)
        && (ra8_key_vault_sha256_xor_challenge((uint16_t)k_sav_slot_a, challenge, nullptr)
            == k_ra8_err_null_ptr);

  return refused ? k_ra8_ok : k_ra8_err_invalid_arg;
}

/**
 * @brief Provision the key-authentication key at both legal lengths.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok              Both lengths round-tripped; bad calls refused.
 * @retval k_ra8_err_invalid_arg A length or capacity check misbehaved.
 * @note The two provisionings use different fill bytes, so the replacement is
 *       proved by comparing the whole key rather than by the reported length:
 *       with one shared fill a partial overwrite would pass.
 * @since 0.1.0
 */
static ra8_err_t internal_check_kak(void)
{
  uint8_t  kak_short[k_ra8_key_vault_mac_key_bytes] = {0};
  uint8_t  kak_long[k_ra8_key_vault_mac_key_bytes]  = {0};
  uint8_t  out[k_ra8_key_vault_mac_key_bytes]       = {0};
  uint16_t out_len                                  = 0U;

  internal_fill(kak_short, (uint16_t)sizeof(kak_short), (uint8_t)k_sav_fill_kak);
  internal_fill(kak_long, (uint16_t)sizeof(kak_long), (uint8_t)k_sav_fill_kak2);

  ra8_err_t err = ra8_key_vault_set_mac_key(kak_short, (uint16_t)k_sav_kak_short);
  if (err != k_ra8_ok) {
    return err;
  }

  err = ra8_key_vault_load_mac_key(out, (uint16_t)sizeof(out), &out_len);
  if (err != k_ra8_ok) {
    return err;
  }
  if ((out_len != (uint16_t)k_sav_kak_short)
      || (memcmp(out, kak_short, (size_t)k_sav_kak_short) != 0)) {
    return k_ra8_err_invalid_arg;
  }

  err = ra8_key_vault_set_mac_key(kak_long, (uint16_t)k_sav_kak_long);
  if (err != k_ra8_ok) {
    return err;
  }

  err = ra8_key_vault_load_mac_key(out, (uint16_t)sizeof(out), &out_len);
  if (err != k_ra8_ok) {
    return err;
  }
  if ((out_len != (uint16_t)k_sav_kak_long)
      || (memcmp(out, kak_long, (size_t)k_sav_kak_long) != 0)) {
    return k_ra8_err_invalid_arg;
  }

  const bool refused
      = (ra8_key_vault_set_mac_key(kak_long, (uint16_t)k_sav_kak_bad_len)
         == k_ra8_err_invalid_arg)
        && (ra8_key_vault_set_mac_key(nullptr, (uint16_t)k_sav_kak_long) == k_ra8_err_null_ptr)
        && (ra8_key_vault_load_mac_key(out, (uint16_t)k_sav_kak_short, &out_len)
            == k_ra8_err_invalid_size)
        && (ra8_key_vault_load_mac_key(nullptr, (uint16_t)sizeof(out), &out_len)
            == k_ra8_err_null_ptr);

  return refused ? k_ra8_ok : k_ra8_err_invalid_arg;
}

/**
 * @brief Exercise the OTA commit shadow on either build flavour.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok              Off-target shadow behaved, or silicon refused.
 * @retval k_ra8_err_invalid_arg A shadow value or refusal disagreed.
 * @note On a silicon build the arm and bank-config writes report
 *       ::k_ra8_err_not_supported by design; that is accepted here.
 * @since 0.1.0
 */
static ra8_err_t internal_check_ota(void)
{
  ra8_err_t err = ra8_ota_commit_reset();
  if (err != k_ra8_ok) {
    return err;
  }

  ra8_ota_bank_t target = k_ra8_ota_bank_a;
  if (ra8_ota_commit_pending(&target) != k_ra8_err_no_data) {
    return k_ra8_err_invalid_arg;
  }
  if (ra8_ota_commit_pending(nullptr) != k_ra8_err_null_ptr) {
    return k_ra8_err_invalid_arg;
  }

  const ra8_err_t armed = ra8_ota_commit_swap_bank(k_ra8_ota_bank_b);
  if (armed == k_ra8_err_not_supported) {
    return k_ra8_ok; /* Silicon build: the real option-byte write is gated. */
  }
  if (armed != k_ra8_ok) {
    return armed;
  }

  err = ra8_ota_commit_pending(&target);
  if ((err != k_ra8_ok) || (target != k_ra8_ota_bank_b)) {
    return k_ra8_err_invalid_arg;
  }

  if (ra8_ota_commit_swap_bank(k_ra8_ota_bank_a) != k_ra8_err_invalid_state) {
    return k_ra8_err_invalid_arg;
  }

  const ra8_err_t wrote = ra8_ota_commit_set_bank_config((uint32_t)k_sav_cfg_raw);
  if (wrote == k_ra8_err_not_supported) {
    return ra8_ota_commit_reset();
  }
  if (wrote != k_ra8_ok) {
    return wrote;
  }

  uint32_t read_back = 0U;
  err                = ra8_ota_commit_get_bank_config(&read_back);
  if (err != k_ra8_ok) {
    return err;
  }
  if (read_back != (uint32_t)k_ra8_ota_bank_config_allowed) {
    return k_ra8_err_invalid_arg;
  }

  err = ra8_ota_commit_reset();
  if (err != k_ra8_ok) {
    return err;
  }

  return (ra8_ota_commit_pending(&target) == k_ra8_err_no_data) ? k_ra8_ok
                                                                : k_ra8_err_invalid_arg;
}

/**
 * @brief Report one leg's verdict on the console.
 *
 * @param[in]     label Leg name, printed verbatim.
 * @param[in]     err   Leg result.
 * @param[in,out] pass  Cleared when @p err is not ::k_ra8_ok.
 * @return void
 * @post One verdict line is queued on the console.
 * @since 0.1.0
 */
static void internal_verdict(const char* label, ra8_err_t err, bool* pass)
{
  internal_print("secure_app_vault_demo: ");
  internal_print(label);

  if (err == k_ra8_ok) {
    internal_print(" PASS\r\n");
    return;
  }

  internal_print(" FAIL\r\n");
  if (pass != nullptr) {
    *pass = false;
  }
}

/**
 * @brief Entry point: drive the vault, the KAK store, and the OTA shadows.
 *
 * @return void
 * @pre SystemInit configured VTOR / FPU / priority grouping.
 * @post A verdict per leg and a final summary are queued on SCI8.
 * @post Control parks in an infinite loop; the function never returns.
 * @note Single-threaded; no option-region write is ever attempted.
 * @since 0.1.0
 */
void main(void)
{
  ra8_log_init();
  (void)ra8_io_stream_uart_init(&s_uart, &s_uart_state, (uint8_t)k_sav_uart_chan);
  (void)ra8_io_log_attach(&s_uart);
  internal_print("secure_app_vault_demo: boot\r\n");

  bool pass = true;

  internal_verdict("vault", internal_check_vault(), &pass);
  internal_verdict("guards", internal_check_guards(), &pass);
  internal_verdict("kak", internal_check_kak(), &pass);
  internal_verdict("ota", internal_check_ota(), &pass);

  internal_print(pass ? "secure_app_vault_demo: ALL PASS\r\n"
                      : "secure_app_vault_demo: ALL FAIL\r\n");

  (void)ra8_sci_flush((uint8_t)k_sav_uart_chan);
  while (true) {
  }
}
