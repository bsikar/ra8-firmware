/**
 * @file ra8_tz_secure_boot.c
 * @brief FSP-style TrustZone secure-boot implementation for RA8D2
 *
 * @par Tag
 * [Ring 1 / Boot] {World: S}
 *
 * @details
 * See ``ra8_tz_secure_boot.h`` for the full contract. This file owns
 * the actual MMIO writes. The implementation is split into three
 * phases (SAU init, security init, BLXNS) so unit tests can drive
 * each phase against the host-side fake mmap.
 *
 * The host-build path uses ``RA8_OFF_TARGET`` to swap the SAU /
 * CPSCU MMIO writes for in-memory captures, lets the tests assert on
 * the documented PRCR-unlock / IPCSAR-write sequence, and replaces
 * the ``BLXNS`` instruction with a captured-target-then-return path.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra8_tz_secure_boot.h"

#include <stdint.h>

#include "ra8_attributes.h"
#include "ra8_check.h"
#include "ra8_err.h"
#include "ra8_log.h"
#include "ra8_sau.h"
#ifdef RA8_ENABLE_ROOT_OF_TRUST
#include "ra8_rot.h"
#endif

/**
 * @var s_tag
 * @brief Logger tag for the secure-boot module.
 *
 * @details Component identifier used by ``ra8_log_*`` so secure-boot
 *          log lines are easy to grep for in JTAG-captured RTT output.
 * @note    File-private; never accessed from outside.
 * @warning Do not modify.
 * @since   0.1.0
 */
static const char* const s_tag = "TZBOOT";

/**
 * @var s_step
 * @brief Progress counter exposed via ``ra8_tz_secure_boot_get_step``.
 *
 * @details Stamped at each forward-progress milestone in the secure
 *          boot. Bench scripts read this through SWD to find out which
 *          phase wedged when bring-up never reaches the NS image.
 * @note    File-private; modify only via the internal steppers.
 * @warning Do not modify directly.
 * @since   0.1.0
 */
static volatile ra8_tz_secure_boot_step_t s_step = k_ra8_tz_secure_boot_step_idle;

/* =============================================================================
 * Address constants (verified against HUM Ch 13 + Ch 3)
 * =============================================================================
 */

/**
 * @enum ra8_tz_secure_boot_addr_t
 * @brief Memory-mapped register addresses used by the secure-boot.
 *
 * @details
 * The SAU register block is deliberately absent: attribution is
 * programmed through ``ra8_sau_configure()``, which owns the
 * 0xE000EDD0 block and its RNR / RBAR / RLAR encoding (issue #735).
 * What is left here is the rest of the boot sequence. The Cortex-M85
 * Secure VTOR_NS is at 0xE000ED08 (the regular VTOR; writes to it
 * from Secure state set VTOR_NS when the SAU is enabled per ARMv8-M
 * ARM section B3.2.4). CPSCU.IPCSAR / IPCPAR follow the layout in
 * HUM Ch 3.2.1 / 3.2.2. PRCR_S lives in the SYSC block at base
 * 0x4001E000 (HUM Ch 13.2.1).
 */
typedef enum : uintptr_t {
  k_ra8_tz_scb_vtor_ns_addr = 0xE002ED08UL, /**< VTOR Non-Secure alias. */
  k_ra8_tz_ipcsar_addr      = 0x40008610UL, /**< CPSCU IPCSAR.          */
  k_ra8_tz_ipcpar_addr      = 0x40008614UL, /**< CPSCU IPCPAR.          */
  k_ra8_tz_prcr_s_addr      = 0x4001E3FAUL, /**< SYSC PRCR_S (16-bit).  */
} ra8_tz_secure_boot_addr_t;

/**
 * @enum ra8_tz_secure_boot_prcr_t
 * @brief PRCR_S unlock key + per-group enable bits (HUM Ch 13.2.1).
 *
 * @details
 * PRCR_S is a 16-bit register. The upper byte must equal the write
 * key 0xA5 on every write or the entire transaction is dropped. Bit 4
 * (PRC4) is the gate for CPSCU security-attribution registers.
 */
typedef enum : uint16_t {
  k_ra8_tz_prcr_s_key       = 0xA500U, /**< Unlock key (top byte).     */
  k_ra8_tz_prcr_s_prc4_open = 0x0010U, /**< PRC4 gate open (bit 4).    */
  k_ra8_tz_prcr_s_open      = 0xA510U, /**< Key | PRC4 (unlock value). */
  k_ra8_tz_prcr_s_close     = 0xA500U, /**< Key with PRC4 = 0 (lock).  */
} ra8_tz_secure_boot_prcr_t;

/**
 * @enum ra8_tz_secure_boot_partition_t
 * @brief Canonical SAU region base / size pairs (HUM Ch 4).
 *
 * @details
 * Region 3 (NSC alias) deliberately targets the unused 0x10000000
 * IDAU alias rather than the actual ``.gnu.sgstubs`` placement. See
 * ``project_sau_sgstubs_brick`` in project memory for the bench
 * fault that drove that choice.
 *
 * These are sizes, not the pre-decremented RLAR limits this file used
 * to carry: ``ra8_sau_configure()`` derives ``base + size - 32`` once,
 * so the window is stated the way the linker script states it and the
 * 32-byte quantum is the driver's arithmetic rather than a constant
 * somebody has to keep correct by hand.
 */
typedef enum : uint32_t {
  k_ra8_tz_part_code_nsc_base = 0x10000000U, /**< Code NSC alias base. */
  k_ra8_tz_part_code_nsc_size = 0x00100000U, /**< Code NSC, 1 MiB.     */
  k_ra8_tz_part_ns_mram_base  = 0x02080000U, /**< NS upper MRAM base.  */
  k_ra8_tz_part_ns_mram_size  = 0x00080000U, /**< NS MRAM, 512 KiB.    */
  k_ra8_tz_part_sram_nsc_base = 0x12000000U, /**< SRAM NSC alias base. */
  k_ra8_tz_part_sram_nsc_size = 0x00010000U, /**< SRAM NSC, 64 KiB.    */
  k_ra8_tz_part_ns_sram_base  = 0x22100000U, /**< NS upper SRAM base.  */
  k_ra8_tz_part_ns_sram_size  = 0x00100000U, /**< NS SRAM, 1 MiB.      */
  /* NS peripheral region needs to cover BOTH the standard Cortex-M
   * peripheral window at 0x40000000 (where IPC lives at 0x40020000)
   * AND the alias window at 0x50000000. With the original 0x5xxxxxxx-
   * only region, CPU1 (NS-only per SECEXT-disabled silicon) cannot
   * reach any IPC channel even after IPCSAR=0x50000 makes channels
   * 0/2 NS-attributed at the peripheral level -- the SAU still has
   * to permit the underlying peripheral address as NS. Bench
   * evidence on 2026-05-27: with 0x5xxxxxxx-only the ping-pong
   * counters stay at zero (CPU0 sends, CPU1 SecureFaults on recv);
   * extending the region down to 0x40000000 unblocks the round-trip. */
  k_ra8_tz_part_ns_per_base = 0x50000000U, /**< NS peripheral base. */
  k_ra8_tz_part_ns_per_size = 0x10000000U, /**< NS periph, 256 MiB. */
} ra8_tz_secure_boot_partition_t;

#ifdef RA8_OFF_TARGET

/* =============================================================================
 * Host-side captures
 * =============================================================================
 *
 * On the host build the SAU / CPSCU / VTOR writes go into the
 * captures below so unit tests can assert on the documented behaviour
 * without touching real memory.
 */

/**
 * @struct ra8_tz_secure_boot_host_state_t
 * @brief Aggregate of every captured boot side-effect on host.
 *
 * @details
 * One field per piece of state the unit tests want to inspect. Tests
 * use ``ra8_tz_secure_boot_host_reset`` to clear it between cases.
 * SAU state is deliberately absent: the partition is programmed
 * through ``ra8_sau_configure()``, whose host build writes the shared
 * fake SAU register block, so a test reads ``ra8_sau_regs()`` and sees
 * the real RBAR / RLAR words rather than a private shadow copy.
 *
 * @invariant ``prcr_unlock_count`` matches ``prcr_relock_count`` after
 *            a successful security-init call.
 */
typedef struct {
  uint16_t prcr_s_last;       /**< Last value written to PRCR_S.      */
  uint8_t  prcr_unlock_count; /**< # of PRC4-open writes.             */
  uint8_t  prcr_relock_count; /**< # of PRC4-close writes.            */
  uint32_t ipcsar_value;      /**< Latest IPCSAR write (post-unlock). */
  uint32_t ipcpar_value;      /**< Latest IPCPAR write (post-unlock). */
  uint32_t blxns_target;      /**< Captured BLXNS reset vector.       */
  uint32_t blxns_msp_ns;      /**< Captured MSP_NS value.             */
  uint32_t vtor_ns;           /**< Captured VTOR_NS value.            */
} ra8_tz_secure_boot_host_state_t;

/**
 * @var s_host
 * @brief Host-side fake state (zero-initialised by BSS).
 *
 * @details See ``ra8_tz_secure_boot_host_state_t`` for the field set.
 * @note    File-private; cleared by ``ra8_tz_secure_boot_host_reset``.
 * @warning Test-only; never accessed from production code paths.
 * @since   0.1.0
 */
static ra8_tz_secure_boot_host_state_t s_host = {};

void ra8_tz_secure_boot_host_reset(void)
{
  /* Pre 1: the host state struct lives in BSS so its address is always
   *        non-NULL on any hosted environment we run on. */
  /* Pre 2: ``s_tag`` is a static const string used to silence the
   *        unused-symbol warning on the target build. */
  (void)s_tag;
  s_host = (ra8_tz_secure_boot_host_state_t){};
  s_step = k_ra8_tz_secure_boot_step_idle;
  /* Post 1: tests get a deterministic baseline. */
  /* Post 2: progress counter is back at idle. */
}

uint32_t ra8_tz_secure_boot_host_blxns_target(void)
{
  /* Pre + post: pure accessor; no state change. */
  return s_host.blxns_target;
}

#else /* !RA8_OFF_TARGET */

void ra8_tz_secure_boot_host_reset(void)
{
  /* Pre + post: target build exposes the symbol but the body is a
   * no-op so unit-test fixtures that link against the target stub do
   * not crash. */
  (void)s_tag;
}

uint32_t ra8_tz_secure_boot_host_blxns_target(void)
{
  /* Pre + post: target build returns 0 because BLXNS never returns. */
  return 0U;
}

#endif /* RA8_OFF_TARGET */

/* =============================================================================
 * Small register helpers (host-aware)
 * =============================================================================
 */

/**
 * @brief Write a 32-bit MMIO register (or capture on host).
 *
 * @details On target the value is stored directly into the MMIO register
 *          at ``addr``. On host (``RA8_OFF_TARGET``) the value is
 *          captured in ``s_host`` so unit tests can inspect the write.
 *
 * @param[in] addr Target address.
 * @param[in] value Value to write.
 *
 * @pre ``addr`` is one of the documented CPSCU / VTOR addresses.
 * @pre Caller is in Secure state (target) / unit-test context (host).
 * @post Target write lands; host capture updated.
 * @post Caller can verify via the host-state accessors.
 * @note Not thread-safe; runs only during secure boot.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_write32(uintptr_t addr, uint32_t value)
{
#ifdef RA8_OFF_TARGET
  if (addr == (uintptr_t)k_ra8_tz_ipcsar_addr) {
    s_host.ipcsar_value = value;
  } else if (addr == (uintptr_t)k_ra8_tz_ipcpar_addr) {
    s_host.ipcpar_value = value;
  } else if (addr == (uintptr_t)k_ra8_tz_scb_vtor_ns_addr) {
    s_host.vtor_ns = value;
  } else {
    /* No other 32-bit register is written through this helper. */
  }
#else
  /* HUM Ch 3.2.1 "IPCSAR" p 205 and HUM Ch 13.2.1 "PRCR_S" p 521 for
   * the secure-only writes routed through this helper. Generic 32-bit
   * MMIO store; the called sites cite their own register page. */
  *(volatile uint32_t*)addr = value;
#endif
}

/**
 * @brief Write a 16-bit MMIO register (or capture on host).
 *
 * @details On target the value is stored at ``addr``. On host the
 *          PRCR_S write is captured in ``s_host`` and the unlock /
 *          relock counts are incremented based on the PRC4 bit.
 *
 * @param[in] addr Target address.
 * @param[in] value Value to write.
 *
 * @pre PRCR_S is the only 16-bit writer; ``addr`` must equal
 *      ``k_ra8_tz_prcr_s_addr``.
 * @pre Caller is in Secure state.
 * @post Target write lands; host capture updated.
 * @post Caller can verify via the host-state accessors.
 * @note Not thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_write16(uintptr_t addr, uint16_t value)
{
#ifdef RA8_OFF_TARGET
  s_host.prcr_s_last = value;
  if ((value & (uint16_t)k_ra8_tz_prcr_s_prc4_open) != 0U) {
    s_host.prcr_unlock_count = (uint8_t)(s_host.prcr_unlock_count + 1U);
  } else {
    s_host.prcr_relock_count = (uint8_t)(s_host.prcr_relock_count + 1U);
  }
  (void)addr;
#else
  /* HUM Ch 13.2.1 "PRCR_S" p 521 */
  *(volatile uint16_t*)addr = value;
#endif
}

/**
 * @brief Emit a Data Synchronisation Barrier (no-op on host).
 *
 * @details Wraps the ``dsb 0xF`` inline-asm so the file's hot path
 *          stays readable. Host build compiles to a true no-op.
 *
 * @pre  Caller is in Secure state (any context valid on host).
 * @pre  Used only inside the secure-boot sequence.
 * @post All outstanding stores have completed before the next access.
 * @post On host: no-op.
 * @note Not thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL static inline void internal_dsb(void)
{
#ifndef RA8_OFF_TARGET
  __asm__ volatile("dsb 0xF" ::: "memory");
#endif
}

/* =============================================================================
 * SAU region programming
 * =============================================================================
 */

/**
 * @var s_sau_regions
 * @brief The canonical five-region secure-boot partition, declared.
 *
 * @details
 * The table this file used to write by hand through RNR / RBAR / RLAR,
 * now stated as intent: a base, a size and NS-or-NSC per window, in the
 * region order ``ra8_tz_sau_region_t`` fixes. ``ra8_sau_configure()``
 * derives every register word from it (issue #735).
 *
 * @note    File-private; lives in ``.rodata`` so the reset path can read
 *          it before ``.data`` / ``.bss`` are initialised.
 * @warning Region order is a published contract: it is what a bench SWD
 *          dump of SAU_RNR indexes against.
 * @since   0.1.0
 */
static const ra8_sau_region_t s_sau_regions[k_ra8_tz_sau_region_count] = {
  [k_ra8_tz_sau_region_code_nsc]  = {.base = (uintptr_t)k_ra8_tz_part_code_nsc_base,
                                     .size = (uint32_t)k_ra8_tz_part_code_nsc_size,
                                     .attr = k_ra8_sau_attr_nsc},
  [k_ra8_tz_sau_region_ns_mram]   = {.base = (uintptr_t)k_ra8_tz_part_ns_mram_base,
                                     .size = (uint32_t)k_ra8_tz_part_ns_mram_size,
                                     .attr = k_ra8_sau_attr_ns},
  [k_ra8_tz_sau_region_sram_nsc]  = {.base = (uintptr_t)k_ra8_tz_part_sram_nsc_base,
                                     .size = (uint32_t)k_ra8_tz_part_sram_nsc_size,
                                     .attr = k_ra8_sau_attr_nsc},
  [k_ra8_tz_sau_region_ns_sram]   = {.base = (uintptr_t)k_ra8_tz_part_ns_sram_base,
                                     .size = (uint32_t)k_ra8_tz_part_ns_sram_size,
                                     .attr = k_ra8_sau_attr_ns},
  [k_ra8_tz_sau_region_ns_periph] = {.base = (uintptr_t)k_ra8_tz_part_ns_per_base,
                                     .size = (uint32_t)k_ra8_tz_part_ns_per_size,
                                     .attr = k_ra8_sau_attr_ns},
};

/**
 * @var s_sau_cfg
 * @brief Partition descriptor handed to ``ra8_sau_configure()``.
 *
 * @details ``all_ns`` stays false: unmapped memory remains Secure, which
 *          is the default-deny posture this boot depends on.
 * @note    File-private.
 * @warning Do not modify.
 * @since   0.1.0
 */
static const ra8_sau_cfg_t s_sau_cfg = {
  .regions      = s_sau_regions,
  .region_count = (uint8_t)k_ra8_tz_sau_region_count,
  .all_ns       = false,
};

ra8_err_t ra8_tz_secure_boot_sau_init(void)
{
  /* Pre: SAU_TYPE.SREGION must report >= 5 implemented regions. Checked
   * here rather than left to ra8_sau_configure() so a shortfall keeps
   * reporting k_ra8_err_not_supported, which is this function's
   * published contract, not the driver's k_ra8_err_invalid_arg. */
  if (ra8_sau_region_count() < (uint8_t)k_ra8_tz_sau_region_count) {
    ra8_log_error(s_tag, "SAU_TYPE.SREGION below required count");
    return k_ra8_err_not_supported;
  }

  const ra8_err_t err = ra8_sau_configure(&s_sau_cfg);
  if (err != k_ra8_ok) {
    ra8_log_error(s_tag, "SAU partition refused");
    return err;
  }

  /* Post: SAU enabled with the canonical layout, default-deny, and every
   * region above the fifth cleared by the driver. */
  s_step = k_ra8_tz_secure_boot_step_sau_done;
  return k_ra8_ok;
}

ra8_err_t ra8_tz_secure_boot_security_init(uint32_t ipcsar_value, uint32_t ipcpar_value)
{
  /* Pre 1: caller must be in Secure state (architectural; cannot be
   * checked from C, documented in the header). */
  /* Pre 2: PRCR_S unlock must observe the documented key + bit
   * pattern, otherwise the chip silently drops the write. */

  /* Open the PRC4 gate so the next IPCSAR / IPCPAR writes land. */
  /* HUM Ch 13.2.1 "PRCR_S" p 521 */
  internal_write16(k_ra8_tz_prcr_s_addr, (uint16_t)k_ra8_tz_prcr_s_open);
  s_step = k_ra8_tz_secure_boot_step_prcr_unlocked;

  /* HUM Ch 3.2.1 "IPCSAR" p 205-207 */
  internal_write32(k_ra8_tz_ipcsar_addr, ipcsar_value);
  /* HUM Ch 3.2.2 "IPCPAR" p 208-209 */
  internal_write32(k_ra8_tz_ipcpar_addr, ipcpar_value);
  s_step = k_ra8_tz_secure_boot_step_ipcsar_written;

  /* Close the PRC4 gate to restore write-protect on CPSCU. */
  /* HUM Ch 13.2.1 "PRCR_S" p 521 */
  internal_write16(k_ra8_tz_prcr_s_addr, (uint16_t)k_ra8_tz_prcr_s_close);
  s_step = k_ra8_tz_secure_boot_step_prcr_relocked;

  internal_dsb();
  /* Post 1: IPCSAR landed (verifiable via JTAG read-back). */
  /* Post 2: PRCR_S.PRC4 clear (write-protect restored). */
  return k_ra8_ok;
}

uint32_t ra8_tz_ns_signed_body_len(const uint32_t* ns_vector_table)
{
  /* Validation 1: reject a NULL image base (returns the deny sentinel 0). */
  if (ns_vector_table == nullptr) {
    ra8_log_error(s_tag, "ns_vector_table is NULL");
    return 0U;
  }
  /* The NS linker emits an ::ra8_ns_rot_header_t at a fixed small offset from the
   * NS base (just past the 16-slot ARMv8-M vector table -- see
   * ::k_ra8_tz_ns_rot_header_offset), self-describing the signed body length.
   * This is a plain memory read of the (already-flashed/copied) NS image; there
   * is no MMIO register access here, so no HUM citation applies. The reinterpret
   * casts route through a named `const void*` seam (as `epub` does): a direct
   * `const uint8_t* -> ra8_ns_rot_header_t*` cast trips -Wcast-align, and a
   * one-expression `(T*)(const void*)p` cast trips bugprone-casting-through-void. */
  const void* const          ns_image  = ns_vector_table;
  const uint8_t*             base      = (const uint8_t*)ns_image;
  const void* const          header_at = base + (uintptr_t)k_ra8_tz_ns_rot_header_offset;
  const ra8_ns_rot_header_t* header    = (const ra8_ns_rot_header_t*)header_at;
  /* Validation 2: reject a missing / wrong-magic header (returns 0 -> deny). */
  if (header->magic != (uint32_t)k_ra8_tz_ns_rot_header_magic) {
    ra8_log_error(s_tag, "NS RoT header magic mismatch");
    return 0U;
  }
  return header->body_len;
}

/**
 * @brief Authenticate the Non-Secure image before BLXNS (default-deny gate).
 *
 * @details
 * Root-of-trust gate factored out of ::ra8_tz_secure_boot_jump_ns so the caller
 * stays within the function-length budget. On a target build with
 * ``RA8_ENABLE_ROOT_OF_TRUST`` enabled it reads the NS image's self-describing
 * signed-body length via ::ra8_tz_ns_signed_body_len (an ::ra8_ns_rot_header_t the
 * NS linker emits at ::k_ra8_tz_ns_rot_header_offset), locates the trailer that
 * the signing tool appended immediately after that body via
 * ::ra8_rot_trailer_after, then re-computes SHA-256 and verifies the NS image's
 * ECDSA-P256 signature against the provisioned root public key via
 * ::ra8_rot_verify_image. This mirrors the copy-to-run boundary exactly (the
 * trailer sits at ``ns_vector_table + body_len``). Any failure -- a missing /
 * bad header, or a failed verify -- returns a non-``k_ra8_ok`` error and the
 * caller must NOT BLXNS.
 *
 * @par Security:
 * ``body_len`` is read from the untrusted NS image, but this is safe for the
 * identical reason it is safe in copy-to-run (which reads it from the untrusted
 * trailer): a lie about ``body_len`` merely changes which bytes get hashed, and
 * the attacker still cannot produce a valid ECDSA-P256 signature over any body
 * without the held-out private key. A wrong ``body_len`` therefore fails the
 * signature check and default-denies. (Here ``body_len`` is additionally covered
 * by the signature, since the header word sits inside the signed body.)
 *
 * With the flag OFF (default) -- or under ``RA8_OFF_TARGET``, where the NS
 * image and trailer do not exist at a real address on the unit-test host -- the
 * gate is absent and this returns ``k_ra8_ok`` so the jump proceeds unverified,
 * exactly as before. The gate's decision logic is covered directly in
 * ``tests/security/src/test_ra8_root_of_trust.c`` and the header read in
 * ``tests/security/src/test_tz_secure_boot.c``.
 *
 * @param[in] ns_vector_table Base of the NS image (its vector table); non-NULL.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok                   Image authentic, or verification is disabled.
 * @retval k_ra8_err_null_ptr         ``ns_vector_table`` is NULL.
 * @retval k_ra8_err_validation_failed NS RoT header missing / wrong magic.
 * @retval k_ra8_err_invalid_size     Header body length out of range.
 * @retval k_ra8_err_*                Root-of-trust gate denied the NS image.
 *
 * @pre ``ns_vector_table`` is non-NULL.
 * @pre On an enabled target build, the NS image carries an ::ra8_ns_rot_header_t
 *      at ::k_ra8_tz_ns_rot_header_offset and a signed ::ra8_rot_trailer_t at
 *      ``ns_vector_table + body_len``.
 * @post On any non-``k_ra8_ok`` return the caller does NOT BLXNS.
 * @post No NS image bytes are modified.
 *
 * @note Not thread-safe; runs once at boot.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_ns_verify_or_deny(const uint32_t* ns_vector_table)
{
  RA8_CHECK_NULL_PTR(ns_vector_table, s_tag, "ns_vector_table");
#if !defined(RA8_OFF_TARGET) && defined(RA8_ENABLE_ROOT_OF_TRUST)
  const uint32_t ns_body_len = ra8_tz_ns_signed_body_len(ns_vector_table);
  if (ns_body_len == 0U) {
    ra8_log_error(s_tag, "NS RoT header missing/invalid -- denying BLXNS");
    return k_ra8_err_validation_failed;
  }
  const ra8_rot_trailer_t* ns_trailer = ra8_rot_trailer_after(ns_vector_table, ns_body_len);
  if (ns_trailer == nullptr) {
    ra8_log_error(s_tag, "NS RoT body_len out of range -- denying BLXNS");
    return k_ra8_err_invalid_size;
  }
  return ra8_rot_verify_image((const uint8_t*)(const void*)ns_vector_table,
                              ns_body_len,
                              ns_trailer);
#else
  (void)ns_vector_table;
  return k_ra8_ok; /* root of trust disabled (or host build): no verification */
#endif /* !RA8_OFF_TARGET && RA8_ENABLE_ROOT_OF_TRUST */
}

ra8_err_t ra8_tz_secure_boot_jump_ns(const uint32_t* ns_vector_table)
{
  RA8_CHECK_NULL_PTR(ns_vector_table, s_tag, "ns_vector_table");
  /* Pre 2: 4-byte alignment. */
  if (((uintptr_t)ns_vector_table & 0x3U) != 0U) {
    ra8_log_error(s_tag, "ns_vector_table misaligned");
    return k_ra8_err_invalid_arg;
  }

  const uint32_t initial_sp  = ns_vector_table[0];
  const uint32_t reset_entry = ns_vector_table[1];

  /* Reject obviously bogus reset vectors -- 0 (all zeros) or
   * 0xFFFFFFFF (erased MRAM). FSP guards the same way. */
  if (reset_entry == 0U || reset_entry == UINT32_MAX) {
    ra8_log_error_val(s_tag, "NS reset vector invalid", reset_entry);
    return k_ra8_err_invalid_arg;
  }

  /* Root-of-trust gate before BLXNS: deny the jump on a failed authentication
   * (no-op when RA8_ENABLE_ROOT_OF_TRUST is off -- see internal_ns_verify_or_deny). */
  const ra8_err_t auth_err = internal_ns_verify_or_deny(ns_vector_table);
  if (auth_err != k_ra8_ok) {
    ra8_log_error(s_tag, "NS image authentication failed -- denying BLXNS");
    return auth_err;
  }

  /* HUM Ch 4 "Option Setting Memory" + ARMv8-M ARM B3.2.4: VTOR_NS
   * accessed via the 0xE002... NS alias of SCB.VTOR (=0xE002ED08). */
  internal_write32(k_ra8_tz_scb_vtor_ns_addr, (uint32_t)(uintptr_t)ns_vector_table);
  s_step = k_ra8_tz_secure_boot_step_blxns_armed;

#ifdef RA8_OFF_TARGET
  s_host.blxns_target = reset_entry;
  s_host.blxns_msp_ns = initial_sp;
  s_step              = k_ra8_tz_secure_boot_step_branched;
  return k_ra8_ok;
#else
  /* ARMv8-M MSR_NS / BLXNS sequence -- set MSP_NS, then branch into
   * the NS world via BLXNS. The compiler does NOT emit BLXNS through
   * a regular function-pointer call; we need inline asm.
   *
   * BLXNS switches to Non-Secure state only when bit[0] of the target
   * register is 0; bit[0] == 1 keeps the call Secure (it is the
   * function-descriptor "Secure" marker, not the Thumb bit). The NS reset
   * vector carries the Thumb bit set, so it MUST be cleared here -- otherwise
   * the "NS" image executes in Secure state on the Secure stack and every
   * NS-pointer cmse check in the NSC veneers misfires. */
  const uint32_t ns_entry = reset_entry & ~(uint32_t)1U;
  __asm__ volatile("msr msp_ns, %0\n"
                   "blxns %1\n"
                   :
                   : "r"(initial_sp), "r"(ns_entry)
                   : "memory");
  /* Unreachable on target. */
  return k_ra8_ok;
#endif
}

ra8_err_t ra8_tz_secure_boot_run(uint32_t        ipcsar_value,
                                 uint32_t        ipcpar_value,
                                 const uint32_t* ns_vector_table)
{
  RA8_CHECK_NULL_PTR(ns_vector_table, s_tag, "ns_vector_table");

  ra8_err_t err = ra8_tz_secure_boot_sau_init();
  if (err != k_ra8_ok) {
    return err;
  }

  err = ra8_tz_secure_boot_security_init(ipcsar_value, ipcpar_value);
  if (err != k_ra8_ok) {
    return err;
  }

  /* Post: the next call BLXNS-es out and never returns on target. */
  return ra8_tz_secure_boot_jump_ns(ns_vector_table);
}

ra8_tz_secure_boot_step_t ra8_tz_secure_boot_get_step(void)
{
  /* Pre: progress counter lives in BSS so it is always readable. */
  /* Pre: single 32-bit volatile load -- atomic on all targets. */
  /* Post: returned value matches the most recent step write. */
  /* Post: no state change. */
  return s_step;
}
