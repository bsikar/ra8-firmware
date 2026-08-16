/**
 * @file examples/ek_ra8d2/hw_pending/ota_ab_orchestration/main.c
 * @brief A/B OTA orchestration demo: stage -> verify -> commit / rollback over MRAM.
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * Exercises the ``libs/ra8_ota`` A/B slot state machine end to end on the
 * EK-RA8D2, over the on-chip extra-MRAM (data-flash) bank model that
 * ``tools/ra8_emulator`` already reproduces (board_periph_mram.c, the MACI
 * program/erase sequencer). ``ra8_ota`` is Dependency-Inversion pure: it drives
 * injected ``net`` / ``crypto`` / ``flash`` interfaces, so this app supplies
 * concrete backends and runs BOTH outcomes of the A/B flow in a single boot:
 *
 *  - **COMMIT path** -- a good image is streamed into the inactive bank
 *    (``ra8_ota_download_to_inactive_bank``), the freshly programmed bank is
 *    re-hashed and its signature checked (``ra8_ota_verify_signature``), and the
 *    inactive bank is latched as the next boot bank
 *    (``ra8_ota_commit_and_reboot`` -> ``flash.set_startup``). The persisted
 *    boot-select record flips from bank A to bank B.
 *
 *  - **ROLLBACK path** -- a corrupted image is streamed into the inactive bank;
 *    the re-hash no longer matches the manifest digest, so verification fails
 *    (``k_ra8_err_crc_mismatch``), the machine parks in ``error``, commit is
 *    never reached, and the boot-select record is left pointing at the still-good
 *    active bank A. This is the safe rollback a real bootloader relies on.
 *
 * @par Backends (EIL==HIL):
 *  - ``flash`` routes to the real ``ra8_flash`` extra-MRAM driver
 *    (``ra8_flash_extra_mram_write`` / ``_erase`` + direct read-back), so the
 *    stage / re-hash / boot-select persistence run the exact MACI register
 *    sequence ra8_emulator models -- the same code path a bench run drives.
 *  - ``crypto`` routes SHA-256 to the real software SHA backend
 *    (``ra8_rsip_sha256*``); identical on host, emulator and silicon (there is no
 *    RSIP hash-hardware on this part). Image INTEGRITY (the digest match that
 *    triggers the rollback) is therefore verified for real.
 *  - ``net`` is a local in-RAM image source (no TLS stack is wired yet). The
 *    signature verifier is a DEMO authenticity stub, NOT real ECDSA-P256; a
 *    production build swaps in a tf-psa-crypto / ra8_tls backend. See the
 *    ``TODO`` markers at ::app_ecdsa_verify and ::s_net_source.
 *
 * A successful run prints, once per report cycle, on the J-Link OB VCOM console:
 * ``ota_ab: stage=ok commit=Y rollback=Y ok=Y``.
 *
 * @author Brighton Sikarskie
 * @date 2026-07-16
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "ra8_board_ek_ra8d2.h"
#include "ra8_boot_entry.h"
#include "ra8_cgc.h"
#include "ra8_check.h"
#include "ra8_err.h"
#include "ra8_flash.h"
#include "ra8_ota.h"
#include "ra8_rsip.h"
#include "ra8_secure.h"

/* -------------------------------------------------------------------------- */
/* Tunables (no magic numbers -- every literal is a typed enum / const) */
/* -------------------------------------------------------------------------- */

/**
 * @enum app_const_t
 * @brief Console, MRAM layout, and demo-image sizing constants.
 *
 * @details Centralises every numeric limit the demo references so the flow reads
 * without bare literals (NASA Rule 8 + CLAUDE.md "C23 typed enums").
 */
typedef enum : uint32_t {
  k_app_uart_baud       = 115200U,     /**< J-Link OB VCOM console baud.               */
  k_app_image_bytes     = 256U,        /**< Demo firmware image size (multiple of 32). */
  k_app_bank_size_bytes = 4096U,       /**< Advertised bank capacity for the OTA cfg.  */
  k_app_report_delay    = 4000000U,    /**< Bounded busy-wait between banner prints.   */
  k_app_bootsel_magic   = 0xB007A8B1U, /**< Boot-select record magic ("BOOT A/B").     */
  k_app_demo_pubkey     = 0x0A8D2C0DU, /**< Opaque demo public-key handle.             */
} app_const_t;

/**
 * @enum app_mram_addr_t
 * @brief Extra-MRAM option-setting addresses the demo owns (0x02E07600, 12 KiB).
 *
 * @details The inactive "bank" is staged at the region base; the persistent
 * boot-select record lives in a separate 32-byte block clear of the image so an
 * OTA bank erase never touches it (HUM Ch 59.7.4.5 Table 59.15 p 3592).
 *
 * @warning This window is one-time-programmable option-setting / OTP memory, not
 * a rewritable data-flash bank: the erase + re-stage cycle an A/B updater needs
 * does NOT work on real silicon. ra8_emulator maps the window so the demo passes
 * here, but a real inactive-bank home (OSPI / SD) is tracked by #315.
 */
typedef enum : uintptr_t {
  k_app_bank_addr = k_ra8_flash_extra_start, /**< Inactive-bank base (extra-MRAM start). */
  k_app_bootsel_addr =
    k_ra8_flash_extra_start + 0x2000U, /**< Persistent boot-select record (own block). */
} app_mram_addr_t;

/**
 * @enum app_bank_t
 * @brief A/B slot identifiers persisted in the boot-select record.
 */
typedef enum : uint8_t {
  k_app_bank_a = 0U, /**< Slot A (the initially-active bank).  */
  k_app_bank_b = 1U, /**< Slot B (the inactive/update target). */
} app_bank_t;

/**
 * @enum app_size_t
 * @brief Small byte-width and record-layout constants.
 */
typedef enum : uint8_t {
  k_app_mram_block_bytes = 32U, /**< Extra-MRAM erase/program granularity.  */
  k_app_sig_bytes        = 32U, /**< Demo signature length.                 */
  k_app_rec_off_magic    = 0U,  /**< Boot-select record: magic byte offset. */
  k_app_rec_off_bank     = 4U,  /**< Boot-select record: bank byte offset.  */
  k_app_rec_off_seq      = 5U,  /**< Boot-select record: seq byte offset.   */
  k_app_octet_bits       = 8U,  /**< Bits per octet for LE spreads.         */
} app_size_t;

/**
 * @enum app_flash_freq_t
 * @brief MRAM controller advertised clock rates for ``ra8_flash_init``.
 */
typedef enum : uint16_t {
  k_app_mrcfreq_mhz = 200U, /**< Code-MRAM advertised clock (MHz).  */
  k_app_mrefreq_mhz = 100U, /**< Extra-MRAM advertised clock (MHz). */
} app_flash_freq_t;

/**
 * @enum app_pattern_t
 * @brief Deterministic demo-image byte-pattern + tag constants.
 *
 * @details The golden image is ``byte[i] = i*mul + add``; the corrupt image
 * flips byte 0 with an all-ones mask; the demo signature tag is a fixed base
 * XOR-ed with its index.
 */
typedef enum : uint8_t {
  k_app_img_seed_mul  = 7U,    /**< Golden-image pattern multiplier.      */
  k_app_img_seed_add  = 3U,    /**< Golden-image pattern additive bias.   */
  k_app_sig_tag_base  = 0xA5U, /**< Demo signature tag base byte.         */
  k_app_byte_all_ones = 0xFFU, /**< Corrupt-flip mask / erased MRAM byte. */
} app_pattern_t;

/**
 * @enum app_outcome_t
 * @brief Classified result of one A/B update attempt.
 *
 * @details Produced by ::app_ab_classify from the state-machine return code and
 * final state; the demo checks that the good image yields ``committed`` and the
 * corrupted image yields ``rolled_back``.
 */
typedef enum : uint8_t {
  k_app_outcome_committed     = 0U, /**< Verify passed and the bank swap latched.  */
  k_app_outcome_rolled_back   = 1U, /**< Verify failed; no swap; active bank kept. */
  k_app_outcome_indeterminate = 2U, /**< Neither terminal shape matched.           */
} app_outcome_t;

/** @brief Module log/console tag. */
static const char* const s_tag = "ota_ab";

/* -------------------------------------------------------------------------- */
/* Local image source (stands in for a TLS download) */
/* -------------------------------------------------------------------------- */

/**
 * @struct app_net_source_t
 * @brief In-RAM "download" cursor handed to the OTA net interface.
 *
 * @details TODO(real network source): a production build binds ``ra8_ota``'s
 * net interface to a TLS stream (ra8_tls, once it lands). Here the "image" is a
 * baked-in RAM blob so the orchestration runs deterministically and offline.
 *
 * @invariant ``pos`` <= ``len``.
 */
typedef struct {
  const uint8_t* data; /**< Image bytes being served.        */
  uint32_t       len;  /**< Total image length in bytes.     */
  uint32_t       pos;  /**< Bytes already handed to the OTA. */
} app_net_source_t;

/** @brief The staged image source (rebound per scenario). */
static app_net_source_t s_net_source;

/** @brief Streaming SHA-256 context backing the OTA crypto interface. */
static ra8_rsip_sha256_ctx_t s_sha_ctx;

/** @brief Monotonic sequence number stamped into each boot-select record. */
static uint32_t s_bootsel_seq = 0U;

/** @brief Golden image bytes for the commit path (deterministic pattern). */
static uint8_t s_image_good[k_app_image_bytes];

/** @brief Corrupted image bytes for the rollback path (one flipped byte). */
static uint8_t s_image_bad[k_app_image_bytes];

/** @brief SHA-256 of ::s_image_good, published as the manifest digest. */
static uint8_t s_image_good_digest[k_ra8_ota_sha256_bytes];

/* -------------------------------------------------------------------------- */
/* Console helpers */
/* -------------------------------------------------------------------------- */

/**
 * @brief Bounded length of a NUL-terminated ASCII string.
 *
 * @param[in] text NUL-terminated string; non-NULL.
 * @return Bytes before the NUL, capped at ``k_app_bank_size_bytes``.
 * @retval 0 Empty string.
 * @pre @p text is non-NULL and NUL-terminated.
 * @pre @p text fits within the cap.
 * @post No state changes.
 * @post Return value never exceeds the cap.
 * @note Pure bounded scan.
 * @since 0.1.0
 */
static size_t app_strlen(const char* text)
{
  size_t n = 0U;
  while (n < (size_t)k_app_bank_size_bytes) {
    if (text[n] == '\0') {
      break;
    }
    ++n;
  }
  return n;
}

/**
 * @brief Print a NUL-terminated ASCII string on the board VCOM console.
 *
 * @param[in] text NUL-terminated string (CR/LF supplied by the caller); non-NULL.
 * @return Nothing.
 * @pre ``ra8_board_uart_console_init`` succeeded.
 * @pre @p text is non-NULL.
 * @post The bytes of @p text are queued on SCI8.
 * @post No other state changes.
 * @note Blocking polled TX; not interrupt-safe.
 * @since 0.1.0
 */
static void app_print(const char* text)
{
  if (text == nullptr) {
    return;
  }
  (void)ra8_board_uart_console_write((const uint8_t*)text, app_strlen(text));
}

/* -------------------------------------------------------------------------- */
/* OTA net interface (local image source) */
/* -------------------------------------------------------------------------- */

/**
 * @brief Begin serving the staged image (OTA ``net.open``).
 *
 * @param[in]  ctx     ::app_net_source_t handle; non-NULL.
 * @param[in]  url     Requested URL (ignored -- local source); unused.
 * @param[out] out_content_len Receives the total image length; non-NULL.
 * @return ra8_err_t outcome.
 * @retval k_ra8_ok           Cursor reset; ``*out_content_len`` set.
 * @retval k_ra8_err_null_ptr A required pointer was NULL.
 * @pre @p ctx and @p out_content_len are non-NULL.
 * @pre The source blob was bound by ::app_bind_source.
 * @post ``ctx->pos == 0`` and ``*out_content_len == ctx->len``.
 * @post No image bytes are consumed yet.
 * @note Not thread-safe; single OTA owner.
 * @since 0.1.0
 */
static ra8_err_t app_net_open(void* ctx, const char* url, uint32_t* out_content_len)
{
  (void)url;
  RA8_CHECK_NULL_PTR(ctx, s_tag, "net.open ctx");
  RA8_CHECK_NULL_PTR(out_content_len, s_tag, "net.open out_len");
  app_net_source_t* src = (app_net_source_t*)ctx;
  src->pos              = 0U;
  *out_content_len      = src->len;
  return k_ra8_ok;
}

/**
 * @brief Copy up to ``cap`` bytes from the staged image (OTA ``net.read``).
 *
 * @param[in]  ctx     ::app_net_source_t handle; non-NULL.
 * @param[out] dst     Destination buffer; non-NULL.
 * @param[in]  cap     Destination capacity in bytes.
 * @param[out] out_len Bytes actually copied (0 == EOF); non-NULL.
 * @return ra8_err_t outcome.
 * @retval k_ra8_ok           Bytes copied (possibly 0 at EOF).
 * @retval k_ra8_err_null_ptr A required pointer was NULL.
 * @pre @p ctx, @p dst and @p out_len are non-NULL.
 * @pre ``ctx->pos <= ctx->len``.
 * @post ``ctx->pos`` advanced by ``*out_len``.
 * @post ``*out_len <= cap``.
 * @note Not thread-safe; single OTA owner.
 * @since 0.1.0
 */
static ra8_err_t app_net_read(void* ctx, uint8_t* dst, uint32_t cap, uint32_t* out_len)
{
  RA8_CHECK_NULL_PTR(ctx, s_tag, "net.read ctx");
  RA8_CHECK_NULL_PTR(dst, s_tag, "net.read dst");
  RA8_CHECK_NULL_PTR(out_len, s_tag, "net.read out_len");
  app_net_source_t* src       = (app_net_source_t*)ctx;
  const uint32_t    remaining = src->len - src->pos;
  const uint32_t    n         = (remaining < cap) ? remaining : cap;
  if (n > 0U) {
    (void)memcpy(dst, &src->data[src->pos], n);
    src->pos += n;
  }
  *out_len = n;
  return k_ra8_ok;
}

/**
 * @brief Tear down the local stream (OTA ``net.close``); a no-op here.
 *
 * @param[in] ctx ::app_net_source_t handle; non-NULL.
 * @return ra8_err_t outcome.
 * @retval k_ra8_ok           Nothing to release.
 * @retval k_ra8_err_null_ptr @p ctx was NULL.
 * @pre @p ctx is non-NULL.
 * @pre The stream was opened by ::app_net_open.
 * @post No state changes (RAM source needs no teardown).
 * @post Subsequent ``open`` calls remain valid.
 * @note Not thread-safe; single OTA owner.
 * @since 0.1.0
 */
/* cppcheck-suppress constParameterCallback ; ctx must stay non-const to match
 * the ra8_ota_net_iface_t.close function-pointer signature. */
static ra8_err_t app_net_close(void* ctx)
{
  RA8_CHECK_NULL_PTR(ctx, s_tag, "net.close ctx");
  return k_ra8_ok;
}

/* -------------------------------------------------------------------------- */
/* OTA crypto interface (real software SHA-256 + demo authenticity) */
/* -------------------------------------------------------------------------- */

/**
 * @brief Begin a streaming SHA-256 (OTA ``crypto.sha256_init``).
 * @param[in] ctx ::ra8_rsip_sha256_ctx_t handle; non-NULL.
 * @return ra8_err_t from ``ra8_rsip_sha256_init``.
 * @retval k_ra8_ok           Context primed.
 * @retval k_ra8_err_null_ptr @p ctx was NULL.
 * @pre @p ctx is non-NULL.
 * @pre The software SHA backend is linked (always, on this part).
 * @post ``ctx`` is ready for ``update``.
 * @post No engine MMIO touched (software backend).
 * @note Not thread-safe; single OTA owner.
 * @since 0.1.0
 */
static ra8_err_t app_sha_init(void* ctx)
{
  RA8_CHECK_NULL_PTR(ctx, s_tag, "sha.init ctx");
  return ra8_rsip_sha256_init((ra8_rsip_sha256_ctx_t*)ctx);
}

/**
 * @brief Absorb bytes into the SHA-256 (OTA ``crypto.sha256_update``).
 * @param[in] ctx  ::ra8_rsip_sha256_ctx_t handle; non-NULL.
 * @param[in] data Bytes to absorb; non-NULL when @p len > 0.
 * @param[in] len  Byte count.
 * @return ra8_err_t from ``ra8_rsip_sha256_update``.
 * @retval k_ra8_ok           Bytes accumulated.
 * @retval k_ra8_err_null_ptr @p ctx was NULL.
 * @pre @p ctx is non-NULL and was init'd.
 * @pre Either @p len is 0 or @p data is non-NULL.
 * @post ``ctx->used`` grew by @p len on success.
 * @post No engine MMIO touched (software backend).
 * @note Not thread-safe; single OTA owner.
 * @since 0.1.0
 */
static ra8_err_t app_sha_update(void* ctx, const uint8_t* data, uint32_t len)
{
  RA8_CHECK_NULL_PTR(ctx, s_tag, "sha.update ctx");
  return ra8_rsip_sha256_update((ra8_rsip_sha256_ctx_t*)ctx, data, len);
}

/**
 * @brief Finalise the SHA-256 digest (OTA ``crypto.sha256_final``).
 * @param[in]  ctx ::ra8_rsip_sha256_ctx_t handle; non-NULL.
 * @param[out] out 32-byte digest destination; non-NULL.
 * @return ra8_err_t from ``ra8_rsip_sha256_final``.
 * @retval k_ra8_ok           Digest written to @p out.
 * @retval k_ra8_err_null_ptr A pointer argument was NULL.
 * @pre @p ctx is non-NULL and was init'd.
 * @pre @p out addresses 32 writable bytes.
 * @post ``out[0..31]`` holds the digest on success.
 * @post ``ctx`` is consumed (must be re-init'd to reuse).
 * @note Not thread-safe; single OTA owner.
 * @since 0.1.0
 */
static ra8_err_t app_sha_final(void* ctx, uint8_t out[k_ra8_ota_sha256_bytes])
{
  RA8_CHECK_NULL_PTR(ctx, s_tag, "sha.final ctx");
  RA8_CHECK_NULL_PTR(out, s_tag, "sha.final out");
  return ra8_rsip_sha256_final((ra8_rsip_sha256_ctx_t*)ctx, out);
}

/**
 * @brief DEMO authenticity check standing in for ECDSA-P256 (OTA ``crypto.ecdsa_verify``).
 *
 * @details TODO(real ECDSA-P256): a production OTA verifies an asymmetric server
 * signature over @p bound (the metadata-bound digest ``ra8_ota_verify_signature``
 * computes) through a tf-psa-crypto / ra8_psa_crypto backend, which is already
 * proven in ``secure_boot_hil`` / ``psa_crypto_hil``. This example is an
 * orchestration demo, so it accepts iff the caller presented the configured demo
 * key handle and a full-width tag -- exercising the verify dispatch that GATES
 * the commit without pulling a full PKI into the example. Update INTEGRITY (the
 * digest match that drives the rollback path) is verified for real by the SHA
 * re-hash upstream of this call, so the rollback demonstration is genuine.
 *
 * @param[in] ctx     Unused opaque context.
 * @param[in] pubkey  Public-key handle presented by the manifest.
 * @param[in] bound   Metadata-bound digest (unused by the demo stub); non-NULL.
 * @param[in] sig     Signature bytes; non-NULL.
 * @param[in] sig_len Signature length in bytes.
 * @return ra8_err_t outcome.
 * @retval k_ra8_ok           Demo key + tag width accepted.
 * @retval k_ra8_err_hw_error Wrong key handle or tag width (verify rejects).
 * @retval k_ra8_err_null_ptr A pointer argument was NULL.
 * @pre @p bound and @p sig are non-NULL.
 * @pre @p pubkey is the handle set in the OTA config.
 * @post No state changes.
 * @post Return solely determined by the inputs.
 * @note Not thread-safe; single OTA owner. Pure decision.
 * @since 0.1.0
 */
static ra8_err_t app_ecdsa_verify(void*          ctx,
                                  uint32_t       pubkey,
                                  const uint8_t  bound[k_ra8_ota_sha256_bytes],
                                  const uint8_t* sig,
                                  uint32_t       sig_len)
{
  (void)ctx;
  (void)bound;
  RA8_CHECK_NULL_PTR(sig, s_tag, "ecdsa.sig");
  if ((pubkey != (uint32_t)k_app_demo_pubkey) || (sig_len != (uint32_t)k_app_sig_bytes)) {
    return k_ra8_err_hw_error;
  }
  return k_ra8_ok;
}

/* -------------------------------------------------------------------------- */
/* OTA flash interface (real extra-MRAM driver) */
/* -------------------------------------------------------------------------- */

/**
 * @brief Erase the inactive bank region (OTA ``flash.erase``).
 *
 * @details Walks ``len`` bytes from ``addr`` one ``k_app_mram_block_bytes`` block
 * at a time through ``ra8_flash_extra_mram_erase`` (each block back to 0xFF).
 *
 * @param[in] ctx  Unused opaque context.
 * @param[in] addr Region base; 32-byte aligned.
 * @param[in] len  Byte count; multiple of ``k_app_mram_block_bytes``.
 * @return ra8_err_t outcome.
 * @retval k_ra8_ok The whole region is erased.
 * @retval other   The first failing block erase's code.
 * @pre @p addr is block-aligned and @p len is a block multiple.
 * @pre The MRAM controller was brought up (``ra8_flash_init``).
 * @post On success ``[addr, addr+len)`` reads back all-ones.
 * @post On failure the region may be partially erased.
 * @note Not thread-safe; single OTA owner.
 * @since 0.1.0
 */
static ra8_err_t app_flash_erase(void* ctx, uint32_t addr, uint32_t len)
{
  (void)ctx;
  for (uint32_t off = 0U; off < len; off += (uint32_t)k_app_mram_block_bytes) {
    const ra8_err_t e = ra8_flash_extra_mram_erase(addr + off);
    if (e != k_ra8_ok) {
      return e;
    }
  }
  return k_ra8_ok;
}

/**
 * @brief Program bytes into the inactive bank (OTA ``flash.program``).
 *
 * @details Splits ``len`` into <= ``k_app_mram_block_bytes`` chunks, each a
 * single ``ra8_flash_extra_mram_write`` (the MACI Program command ra8_emulator
 * models). Callers stage 32-aligned offsets, so no write crosses a page.
 *
 * @param[in] ctx  Unused opaque context.
 * @param[in] addr Destination base; 32-byte aligned.
 * @param[in] src  Source bytes; non-NULL.
 * @param[in] len  Byte count.
 * @return ra8_err_t outcome.
 * @retval k_ra8_ok           All bytes programmed.
 * @retval k_ra8_err_null_ptr @p src was NULL.
 * @retval other             The first failing block write's code.
 * @pre @p addr is 32-byte aligned and @p src is non-NULL.
 * @pre ``[addr, addr+len)`` lies inside the extra-MRAM window.
 * @post On success the region holds @p src.
 * @post On failure the region is left partially programmed.
 * @note Not thread-safe; single OTA owner.
 * @since 0.1.0
 */
static ra8_err_t app_flash_program(void* ctx, uint32_t addr, const uint8_t* src, uint32_t len)
{
  (void)ctx;
  RA8_CHECK_NULL_PTR(src, s_tag, "flash.program src");
  for (uint32_t off = 0U; off < len; off += (uint32_t)k_app_mram_block_bytes) {
    const uint32_t remaining = len - off;
    const uint32_t n =
      (remaining < (uint32_t)k_app_mram_block_bytes) ? remaining : (uint32_t)k_app_mram_block_bytes;
    const ra8_err_t e = ra8_flash_extra_mram_write(addr + off, &src[off], n);
    if (e != k_ra8_ok) {
      return e;
    }
  }
  return k_ra8_ok;
}

/**
 * @brief Read the inactive bank back for re-hash (OTA ``flash.readback``).
 *
 * @details Extra-MRAM is directly readable once programmed, so this is a plain
 * copy from the mapped data-flash address -- the same access a bench build makes.
 *
 * @param[in]  ctx Unused opaque context.
 * @param[in]  addr Source address in the extra-MRAM window.
 * @param[out] dst Destination buffer; non-NULL.
 * @param[in]  len Byte count.
 * @return ra8_err_t outcome.
 * @retval k_ra8_ok           Bytes copied.
 * @retval k_ra8_err_null_ptr @p dst was NULL.
 * @pre @p dst is non-NULL and @p addr is inside the extra-MRAM window.
 * @pre ``[addr, addr+len)`` was programmed (no blank cells).
 * @post ``dst[0..len)`` mirrors the bank contents.
 * @post No MRAM state changes.
 * @note Not thread-safe; single OTA owner.
 * @since 0.1.0
 */
static ra8_err_t app_flash_readback(void* ctx, uint32_t addr, uint8_t* dst, uint32_t len)
{
  (void)ctx;
  RA8_CHECK_NULL_PTR(dst, s_tag, "flash.readback dst");
  (void)memcpy(dst, (const void*)(uintptr_t)addr, (size_t)len);
  return k_ra8_ok;
}

/**
 * @brief Persist the A/B boot-select record for @p bank.
 *
 * @details Erases the boot-select block and programs a 32-byte record
 * ``[magic:4][bank:1][seq:4]`` into extra-MRAM through the real driver. Standing
 * in for the BTFLG boot-area swap (HUM Ch 7.2 "BTFLG Boot-Area Swap" p 282), but
 * on brick-safe data-flash: a demo never touches the option-setting anchors.
 *
 * @param[in] bank Slot to record as the next boot bank.
 * @return ra8_err_t outcome.
 * @retval k_ra8_ok The record was persisted.
 * @retval other   The first failing erase/program code.
 * @pre The MRAM controller was brought up.
 * @pre @p bank is ::k_app_bank_a or ::k_app_bank_b.
 * @post ``[k_app_bootsel_addr]`` holds a valid record naming @p bank.
 * @post ``s_bootsel_seq`` advanced by 1.
 * @note Not thread-safe; single OTA owner.
 * @since 0.1.0
 */
static ra8_err_t app_write_bootsel(uint8_t bank)
{
  uint8_t rec[k_app_mram_block_bytes] = {};
  for (uint8_t i = 0U; i < (uint8_t)k_app_rec_off_bank; ++i) {
    rec[(uint8_t)k_app_rec_off_magic + i] =
      (uint8_t)((uint32_t)k_app_bootsel_magic >> ((uint32_t)i * (uint32_t)k_app_octet_bits));
  }
  rec[k_app_rec_off_bank] = bank;
  ++s_bootsel_seq;
  for (uint8_t i = 0U; i < (uint8_t)k_app_rec_off_bank; ++i) {
    rec[(uint8_t)k_app_rec_off_seq + i] =
      (uint8_t)(s_bootsel_seq >> ((uint32_t)i * (uint32_t)k_app_octet_bits));
  }
  const ra8_err_t e = ra8_flash_extra_mram_erase((uint32_t)k_app_bootsel_addr);
  if (e != k_ra8_ok) {
    return e;
  }
  return ra8_flash_extra_mram_write((uint32_t)k_app_bootsel_addr, rec, (uint32_t)sizeof rec);
}

/**
 * @brief Latch the inactive bank as the next boot bank (OTA ``flash.set_startup``).
 *
 * @details Thin adapter: ``ra8_ota_commit_and_reboot`` calls this on a verified
 * update, so it persists the boot-select record for @p which_bank.
 *
 * @param[in] ctx        Unused opaque context.
 * @param[in] which_bank Bank index the OTA config named as inactive.
 * @param[in] persistent Whether the swap should survive reset (always true here).
 * @return ra8_err_t from ::app_write_bootsel.
 * @retval k_ra8_ok The boot-select record was written.
 * @retval other   The first failing erase/program code.
 * @pre The MRAM controller was brought up.
 * @pre @p which_bank is a valid A/B index.
 * @post The boot-select record names @p which_bank.
 * @post ``s_bootsel_seq`` advanced by 1.
 * @note Not thread-safe; single OTA owner.
 * @since 0.1.0
 */
static ra8_err_t app_flash_set_startup(void* ctx, uint8_t which_bank, bool persistent)
{
  (void)ctx;
  (void)persistent;
  return app_write_bootsel(which_bank);
}

/**
 * @brief Read the persisted boot-select record.
 *
 * @param[out] out_bank  Receives the recorded bank; non-NULL.
 * @param[out] out_valid Receives whether the magic matched; non-NULL.
 * @return ra8_err_t outcome.
 * @retval k_ra8_ok           Record read (validity in ``*out_valid``).
 * @retval k_ra8_err_null_ptr A pointer argument was NULL.
 * @pre @p out_bank and @p out_valid are non-NULL.
 * @pre The boot-select block was programmed by ::app_write_bootsel.
 * @post ``*out_valid`` is true iff the magic matched.
 * @post ``*out_bank`` holds the recorded bank when valid.
 * @note Not thread-safe; single OTA owner.
 * @since 0.1.0
 */
static ra8_err_t app_read_bootsel(uint8_t* out_bank, bool* out_valid)
{
  RA8_CHECK_NULL_PTR(out_bank, s_tag, "bootsel out_bank");
  RA8_CHECK_NULL_PTR(out_valid, s_tag, "bootsel out_valid");
  const uint8_t* rec   = (const uint8_t*)(uintptr_t)k_app_bootsel_addr;
  uint32_t       magic = 0U;
  for (uint8_t i = 0U; i < (uint8_t)k_app_rec_off_bank; ++i) {
    magic |= ((uint32_t)rec[(uint8_t)k_app_rec_off_magic + i])
             << ((uint32_t)i * (uint32_t)k_app_octet_bits);
  }
  *out_valid = (magic == (uint32_t)k_app_bootsel_magic);
  *out_bank  = rec[k_app_rec_off_bank];
  return k_ra8_ok;
}

/* -------------------------------------------------------------------------- */
/* Pure A/B decision logic (unit-tested with MC/DC) */
/* -------------------------------------------------------------------------- */

/**
 * @brief Classify one update attempt from its return code and final state.
 *
 * @details The A/B contract: a clean run ends ``(k_ra8_ok, done)`` = committed;
 * a rejected image ends ``(!= k_ra8_ok, error)`` = rolled back; anything else is
 * indeterminate (a bug or an aborted run).
 *
 * @param[in] result Return code from the driven OTA sequence.
 * @param[in] state  Final ::ra8_ota_state_t reported by the machine.
 * @return app_outcome_t classification.
 * @retval k_app_outcome_committed     ``result == ok`` and ``state == done``.
 * @retval k_app_outcome_rolled_back   ``result != ok`` and ``state == error``.
 * @retval k_app_outcome_indeterminate Neither terminal shape matched.
 * @pre None.
 * @pre None.
 * @post No state changes.
 * @post Return solely determined by the two inputs.
 * @note Pure function; test-mirrored for MC/DC.
 * @since 0.1.0
 */
static app_outcome_t app_ab_classify(ra8_err_t result, ra8_ota_state_t state)
{
  if ((result == k_ra8_ok) && (state == k_ra8_ota_state_done)) {
    return k_app_outcome_committed;
  }
  if ((result != k_ra8_ok) && (state == k_ra8_ota_state_error)) {
    return k_app_outcome_rolled_back;
  }
  return k_app_outcome_indeterminate;
}

/**
 * @brief Final demo verdict: both A/B paths behaved as designed.
 *
 * @param[in] committed    The good-image attempt classified as committed.
 * @param[in] rolled_back  The corrupt-image attempt classified as rolled back.
 * @return Boolean verdict.
 * @retval true  Both paths matched their expected outcome.
 * @retval false Either path deviated.
 * @pre None.
 * @pre None.
 * @post No state changes.
 * @post Return solely determined by the two inputs.
 * @note Pure function; test-mirrored for MC/DC.
 * @since 0.1.0
 */
static bool app_ab_ok(bool committed, bool rolled_back)
{
  return committed && rolled_back;
}

/* -------------------------------------------------------------------------- */
/* Scenario driver + manifest construction */
/* -------------------------------------------------------------------------- */

/**
 * @brief Assemble the OTA config wired to this app's backends.
 *
 * @param[out] cfg Config to populate; non-NULL.
 * @return Nothing.
 * @pre @p cfg is non-NULL.
 * @pre ::s_net_source / ::s_sha_ctx are usable statics.
 * @post Every net/crypto/flash pointer + bank metadata is set.
 * @post ``cfg->manifest_url`` is a non-empty local placeholder.
 * @note Single-threaded init only.
 * @since 0.1.0
 */
static void app_make_cfg(ra8_ota_cfg_t* cfg)
{
  (void)memset(cfg, 0, sizeof *cfg);
  (void)memcpy(cfg->manifest_url,
               "local://demo/manifest",
               app_strlen("local://demo/manifest") + 1U);
  cfg->pubkey_handle = (uint32_t)k_app_demo_pubkey;

  cfg->net.open  = app_net_open;
  cfg->net.read  = app_net_read;
  cfg->net.close = app_net_close;
  cfg->net.ctx   = &s_net_source;

  cfg->crypto.sha256_init   = app_sha_init;
  cfg->crypto.sha256_update = app_sha_update;
  cfg->crypto.sha256_final  = app_sha_final;
  cfg->crypto.ecdsa_verify  = app_ecdsa_verify;
  cfg->crypto.ctx           = &s_sha_ctx;

  cfg->flash.erase               = app_flash_erase;
  cfg->flash.program             = app_flash_program;
  cfg->flash.set_startup         = app_flash_set_startup;
  cfg->flash.readback            = app_flash_readback;
  cfg->flash.inactive_bank_addr  = (uint32_t)k_app_bank_addr;
  cfg->flash.bank_size_bytes     = (uint32_t)k_app_bank_size_bytes;
  cfg->flash.inactive_bank_index = (uint8_t)k_app_bank_b;
  cfg->flash.ctx                 = nullptr;
}

/**
 * @brief Build the demo manifest describing the good image.
 *
 * @param[out] m Manifest to populate; non-NULL.
 * @return Nothing.
 * @pre @p m is non-NULL and ::s_image_good_digest is computed.
 * @pre The demo signature width matches ``k_app_sig_bytes``.
 * @post ``m->image_sha256`` equals the golden image digest.
 * @post ``m->signature_len == k_app_sig_bytes``.
 * @note Single-threaded init only.
 * @since 0.1.0
 */
static void app_make_manifest(ra8_ota_manifest_t* m)
{
  (void)memset(m, 0, sizeof *m);
  (void)memcpy(m->version, "1.0.1", app_strlen("1.0.1") + 1U);
  (void)memcpy(m->image_url, "local://demo/app.bin", app_strlen("local://demo/app.bin") + 1U);
  m->image_size_bytes = (uint32_t)k_app_image_bytes;
  (void)memcpy(m->image_sha256, s_image_good_digest, sizeof m->image_sha256);
  /* Demo authenticity tag: a fixed full-width pattern the demo verifier
   * accepts. TODO(real ECDSA-P256): replace with a server ECDSA signature. */
  for (uint8_t i = 0U; i < (uint8_t)k_app_sig_bytes; ++i) {
    m->signature[i] = (uint8_t)((uint8_t)k_app_sig_tag_base ^ i);
  }
  m->signature_len = (uint16_t)k_app_sig_bytes;
}

/**
 * @brief Drive one full A/B attempt against @p source and classify it.
 *
 * @details Resets the boot-select record to bank A, (re)initialises ``ra8_ota``,
 * then runs download -> verify -> commit. Verify fails on a corrupted source, so
 * commit is skipped and the machine parks in ``error``.
 *
 * @param[in]  source     Image bytes to stream (good or corrupted); non-NULL.
 * @param[in]  m          Manifest describing the expected (good) image; non-NULL.
 * @param[out] out_bank   Bank the boot-select record names afterwards; non-NULL.
 * @return app_outcome_t classification of the attempt.
 * @retval k_app_outcome_committed     Good image: verified and swap latched.
 * @retval k_app_outcome_rolled_back   Corrupt image: verify failed, no swap.
 * @retval k_app_outcome_indeterminate Setup failure (reported via ``out_bank``).
 * @pre Pointers are non-NULL and the MRAM controller is up.
 * @pre @p m describes the good image.
 * @post ``*out_bank`` reflects the persisted boot-select record.
 * @post ``ra8_ota`` is left de-initialised.
 * @note Not thread-safe; single OTA owner.
 * @since 0.1.0
 */
static app_outcome_t
app_run_attempt(const uint8_t* source, const ra8_ota_manifest_t* m, uint8_t* out_bank)
{
  *out_bank = (uint8_t)k_app_bank_a;
  if (app_write_bootsel((uint8_t)k_app_bank_a) != k_ra8_ok) {
    return k_app_outcome_indeterminate;
  }
  s_net_source.data = source;
  s_net_source.len  = (uint32_t)k_app_image_bytes;
  s_net_source.pos  = 0U;

  ra8_ota_cfg_t cfg;
  app_make_cfg(&cfg);
  (void)ra8_ota_deinit();
  if (ra8_ota_init(&cfg) != k_ra8_ok) {
    return k_app_outcome_indeterminate;
  }

  ra8_err_t result = ra8_ota_download_to_inactive_bank(m);
  if (result == k_ra8_ok) {
    result = ra8_ota_verify_signature(m);
  }
  if (result == k_ra8_ok) {
    result = ra8_ota_commit_and_reboot();
  }
  const ra8_ota_state_t state = ra8_ota_get_state();

  bool valid = false;
  (void)app_read_bootsel(out_bank, &valid);
  (void)ra8_ota_deinit();
  return app_ab_classify(result, state);
}

/* -------------------------------------------------------------------------- */
/* Setup + banner */
/* -------------------------------------------------------------------------- */

/**
 * @brief Fill the golden/corrupt image blobs and precompute the manifest digest.
 *
 * @return ra8_err_t from the digest computation.
 * @retval k_ra8_ok           Blobs staged and digest ready.
 * @retval other             SHA backend error.
 * @pre The software SHA backend is linked.
 * @pre ::s_image_good / ::s_image_bad are writable statics.
 * @post ::s_image_good_digest holds SHA-256 of ::s_image_good.
 * @post ::s_image_bad differs from ::s_image_good in exactly one byte.
 * @note Single-threaded init only.
 * @since 0.1.0
 */
static ra8_err_t app_stage_images(void)
{
  for (uint32_t i = 0U; i < (uint32_t)k_app_image_bytes; ++i) {
    s_image_good[i] = (uint8_t)(i * (uint32_t)k_app_img_seed_mul + (uint32_t)k_app_img_seed_add);
    s_image_bad[i]  = s_image_good[i];
  }
  /* Corrupt the rollback image so its re-hash cannot match the manifest digest
   * -- the "bad download" that must NOT be committed. */
  s_image_bad[0] = (uint8_t)(s_image_good[0] ^ (uint8_t)k_app_byte_all_ones);
  return ra8_rsip_sha256(s_image_good, (uint32_t)k_app_image_bytes, s_image_good_digest);
}

/**
 * @brief Bring up CGC + the board VCOM console + the MRAM controller.
 *
 * @return ra8_err_t outcome.
 * @retval k_ra8_ok The console and MRAM controller are live.
 * @retval other   The first failing bring-up step's code.
 * @pre SystemInit configured VTOR / FPU.
 * @pre Runs single-threaded during early boot.
 * @post On success the console prints and extra-MRAM programs.
 * @post On failure the caller halts.
 * @note Not thread-safe; boot-context only.
 * @since 0.1.0
 */
static ra8_err_t app_setup(void)
{
  if (ra8_cgc_init() != k_ra8_ok) {
    return k_ra8_err_hw_error;
  }
  if (ra8_board_uart_console_init((uint32_t)k_app_uart_baud) != k_ra8_ok) {
    return k_ra8_err_hw_error;
  }
  const ra8_flash_cfg_t fcfg = {
    .mrcfreq_mhz        = (uint16_t)k_app_mrcfreq_mhz,
    .mrefreq_mhz        = (uint8_t)k_app_mrefreq_mhz,
    .prefetch_en        = true,
    .ecc_encoder_enable = true,
    .ecc_decoder_enable = true,
  };
  return ra8_flash_init(&fcfg);
}

/**
 * @brief Print the one-line A/B verdict banner over the console.
 *
 * @param[in] staged      Whether image staging + digest precompute succeeded.
 * @param[in] committed   Whether the good-image attempt committed.
 * @param[in] rolled_back Whether the corrupt-image attempt rolled back.
 * @return Nothing.
 * @pre The console is initialised.
 * @pre The three verdict flags are final.
 * @post One ``ota_ab: ...`` line is queued on SCI8.
 * @post No other state changes.
 * @note Blocking polled TX.
 * @since 0.1.0
 */
static void app_print_banner(bool staged, bool committed, bool rolled_back)
{
  app_print("ota_ab: stage=");
  app_print(staged ? "ok" : "fail");
  app_print(" commit=");
  app_print(committed ? "Y" : "N");
  app_print(" rollback=");
  app_print(rolled_back ? "Y" : "N");
  app_print(" ok=");
  app_print(app_ab_ok(committed, rolled_back) ? "Y" : "N");
  app_print("\r\n");
}

/**
 * @brief Application entry: run both A/B paths once, then report forever.
 *
 * @pre Reset_Handler initialised the C runtime; SystemInit ran.
 * @pre The EK-RA8D2 J-Link OB VCOM console is attached for the banner.
 * @post The commit + rollback attempts have executed against extra-MRAM.
 * @post The verdict banner is emitted once per report cycle.
 * @note Single entry point; not re-entrant.
 * @since 0.1.0
 */
void main(void)
{
  if (app_setup() != k_ra8_ok) {
    /* NASA Rule 2 exemption: terminal panic spin on a fatal bring-up error. */
    while (true) {
    }
  }

  const bool staged = (app_stage_images() == k_ra8_ok);

  bool committed   = false;
  bool rolled_back = false;
  if (staged) {
    ra8_ota_manifest_t m;
    app_make_manifest(&m);

    uint8_t             bank_after = (uint8_t)k_app_bank_a;
    const app_outcome_t good       = app_run_attempt(s_image_good, &m, &bank_after);
    committed = (good == k_app_outcome_committed) && (bank_after == (uint8_t)k_app_bank_b);

    const app_outcome_t bad = app_run_attempt(s_image_bad, &m, &bank_after);
    rolled_back = (bad == k_app_outcome_rolled_back) && (bank_after == (uint8_t)k_app_bank_a);
  }

  (void)app_print("\r\nra8d2 ota_ab_orchestration (A/B stage/verify/commit/rollback)\r\n");

  /* NASA Rule 2 exemption: deliberate report loop; the verdict is latched, the
   * banner just repeats so a UART scrape reliably captures it. */
  while (true) {
    app_print_banner(staged, committed, rolled_back);
    for (volatile uint32_t d = 0U; d < (uint32_t)k_app_report_delay; ++d) {
      /* coarse inter-report busy delay (bounded). */
    }
  }
}
