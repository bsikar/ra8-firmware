/**
 * @file ra_tz_secure_boot.c
 * @brief FSP-style TrustZone secure-boot implementation for RA8D2
 *
 * @par Tag
 * [Ring 1 / Boot] {World: S}
 *
 * @details
 * See ``ra_tz_secure_boot.h`` for the full contract. This file owns
 * the actual MMIO writes. The implementation is split into three
 * phases (SAU init, security init, BLXNS) so unit tests can drive
 * each phase against the host-side simulator mmap.
 *
 * The host-build path uses ``RA_SIMULATOR_MODE`` to swap the SAU /
 * CPSCU MMIO writes for in-memory captures, lets the tests assert on
 * the documented PRCR-unlock / IPCSAR-write sequence, and replaces
 * the ``BLXNS`` instruction with a captured-target-then-return path.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra_tz_secure_boot.h"

#include <stdint.h>

#include "ra_attributes.h"
#include "ra_check.h"
#include "ra_err.h"
#include "ra_log.h"
#ifdef RA_ENABLE_ROOT_OF_TRUST
#include "ra_rot.h"
#endif

/**
 * @var s_tag
 * @brief Logger tag for the secure-boot module.
 *
 * @details Component identifier used by ``ra_log_*`` so secure-boot
 *          log lines are easy to grep for in JTAG-captured RTT output.
 * @note    File-private; never accessed from outside.
 * @warning Do not modify.
 * @since   0.1.0
 */
static const char* s_tag = "TZBOOT";

/**
 * @var s_step
 * @brief Progress counter exposed via ``ra_tz_secure_boot_get_step``.
 *
 * @details Stamped at each forward-progress milestone in the secure
 *          boot. Bench scripts read this through SWD to find out which
 *          phase wedged when bring-up never reaches the NS image.
 * @note    File-private; modify only via the internal steppers.
 * @warning Do not modify directly.
 * @since   0.1.0
 */
static volatile ra_tz_secure_boot_step_t s_step = k_ra_tz_secure_boot_step_idle;

/* =============================================================================
 * Address constants (verified against HUM Ch 9 + Ch 3)
 * =============================================================================
 */

/**
 * @enum ra_tz_secure_boot_addr_t
 * @brief Memory-mapped register addresses used by the secure-boot.
 *
 * @details
 * SAU registers live in the System Control Space at 0xE000EDD0
 * (ARMv8-M architectural address). The Cortex-M85 Secure VTOR_NS is
 * at 0xE000ED08 (the regular VTOR; writes to it from Secure state set
 * VTOR_NS when SAU is enabled per ARMv8-M ARM section B3.2.4).
 * CPSCU.IPCSAR / IPCPAR follow the layout in HUM Ch 3.2.1 / 3.2.2.
 * PRCR_S lives in the SYSC block at base 0x4001E000 (HUM Ch 9.2.4).
 */
typedef enum : uintptr_t {
  k_ra_tz_sau_ctrl_addr    = 0xE000EDD0UL, /**< SAU Control.           */
  k_ra_tz_sau_type_addr    = 0xE000EDD4UL, /**< SAU Type.              */
  k_ra_tz_sau_rnr_addr     = 0xE000EDD8UL, /**< Region Num.            */
  k_ra_tz_sau_rbar_addr    = 0xE000EDDCUL, /**< Region Base.           */
  k_ra_tz_sau_rlar_addr    = 0xE000EDE0UL, /**< Region Lim.            */
  k_ra_tz_scb_vtor_ns_addr = 0xE002ED08UL, /**< VTOR Non-Secure alias. */
  k_ra_tz_ipcsar_addr      = 0x40008610UL, /**< CPSCU IPCSAR.          */
  k_ra_tz_ipcpar_addr      = 0x40008614UL, /**< CPSCU IPCPAR.          */
  k_ra_tz_prcr_s_addr      = 0x4001E3FAUL, /**< SYSC PRCR_S (16-bit).  */
} ra_tz_secure_boot_addr_t;

/**
 * @enum ra_tz_secure_boot_sau_bit_t
 * @brief Bit positions used to enable / configure each SAU region.
 *
 * @details
 * Mirrors the ARMv8-M ARM register-field encoding (D.4.5 SAU_CTRL,
 * D.4.7 SAU_RLAR). Names are kept identical to the FSP equivalents
 * so the bench-debug trail is easy to follow.
 */
typedef enum : uint32_t {
  k_ra_tz_sau_ctrl_enable = 0x00000001U, /**< CTRL.ENABLE.           */
  k_ra_tz_sau_ctrl_allns  = 0x00000002U, /**< CTRL.ALLNS (kept 0).   */
  k_ra_tz_sau_rlar_enable = 0x00000001U, /**< RLAR.ENABLE.           */
  k_ra_tz_sau_rlar_nsc    = 0x00000002U, /**< RLAR.NSC.              */
  k_ra_tz_sau_type_mask   = 0x000000FFU, /**< TYPE.SREGION lower 8b. */
} ra_tz_secure_boot_sau_bit_t;

/**
 * @enum ra_tz_secure_boot_prcr_t
 * @brief PRCR_S unlock key + per-group enable bits (HUM Ch 9.2.4).
 *
 * @details
 * PRCR_S is a 16-bit register. The upper byte must equal the write
 * key 0xA5 on every write or the entire transaction is dropped. Bit 4
 * (PRC4) is the gate for CPSCU security-attribution registers.
 */
typedef enum : uint16_t {
  k_ra_tz_prcr_s_key       = 0xA500U, /**< Unlock key (top byte).     */
  k_ra_tz_prcr_s_prc4_open = 0x0010U, /**< PRC4 gate open (bit 4).    */
  k_ra_tz_prcr_s_open      = 0xA510U, /**< Key | PRC4 (unlock value). */
  k_ra_tz_prcr_s_close     = 0xA500U, /**< Key with PRC4 = 0 (lock).  */
} ra_tz_secure_boot_prcr_t;

/**
 * @enum ra_tz_secure_boot_partition_t
 * @brief Canonical SAU region base / limit addresses (HUM Ch 4).
 *
 * @details
 * Region 3 (NSC alias) deliberately targets the unused 0x10000000
 * IDAU alias rather than the actual ``.gnu.sgstubs`` placement. See
 * ``project_sau_sgstubs_brick`` in project memory for the bench
 * fault that drove that choice. RLAR limits are the upper bound
 * minus the ARMv8-M 32-byte region quantum.
 */
typedef enum : uint32_t {
  k_ra_tz_part_code_nsc_base  = 0x10000000U,
  k_ra_tz_part_code_nsc_limit = 0x100FFFE0U,
  k_ra_tz_part_ns_mram_base   = 0x02080000U,
  k_ra_tz_part_ns_mram_limit  = 0x020FFFE0U,
  k_ra_tz_part_sram_nsc_base  = 0x12000000U,
  k_ra_tz_part_sram_nsc_limit = 0x1200FFE0U,
  k_ra_tz_part_ns_sram_base   = 0x22100000U,
  k_ra_tz_part_ns_sram_limit  = 0x221FFFE0U,
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
  k_ra_tz_part_ns_per_base  = 0x50000000U,
  k_ra_tz_part_ns_per_limit = 0x5FFFFFE0U,
} ra_tz_secure_boot_partition_t;

#ifdef RA_SIMULATOR_MODE

/* =============================================================================
 * Host-side captures
 * =============================================================================
 *
 * On the host build the SAU / CPSCU / VTOR writes go into the
 * captures below so unit tests can assert on the documented behaviour
 * without touching real memory.
 */

/**
 * @struct ra_tz_secure_boot_host_state_t
 * @brief Aggregate of every captured boot side-effect on host.
 *
 * @details
 * One field per piece of state the unit tests want to inspect. Tests
 * use ``ra_tz_secure_boot_host_reset`` to clear it between cases.
 *
 * @invariant ``prcr_unlock_count`` matches ``prcr_relock_count`` after
 *            a successful security-init call.
 */
typedef struct {
  uint32_t sau_ctrl;                                   /**< Last value written to SAU_CTRL.    */
  uint32_t sau_region_base[k_ra_tz_sau_region_count];  /**< Per-region base.                   */
  uint32_t sau_region_limit[k_ra_tz_sau_region_count]; /**< Per-region lim.                    */
  uint8_t  sau_region_nsc[k_ra_tz_sau_region_count];   /**< NSC flag.                          */
  uint16_t prcr_s_last;                                /**< Last value written to PRCR_S.      */
  uint8_t  prcr_unlock_count;                          /**< # of PRC4-open writes.             */
  uint8_t  prcr_relock_count;                          /**< # of PRC4-close writes.            */
  uint32_t ipcsar_value;                               /**< Latest IPCSAR write (post-unlock). */
  uint32_t ipcpar_value;                               /**< Latest IPCPAR write (post-unlock). */
  uint32_t blxns_target;                               /**< Captured BLXNS reset vector.       */
  uint32_t blxns_msp_ns;                               /**< Captured MSP_NS value.             */
  uint32_t vtor_ns;                                    /**< Captured VTOR_NS value.            */
} ra_tz_secure_boot_host_state_t;

/**
 * @var s_host
 * @brief Host-side simulation state (zero-initialised by BSS).
 *
 * @details See ``ra_tz_secure_boot_host_state_t`` for the field set.
 * @note    File-private; cleared by ``ra_tz_secure_boot_host_reset``.
 * @warning Test-only; never accessed from production code paths.
 * @since   0.1.0
 */
static ra_tz_secure_boot_host_state_t s_host = {};

void ra_tz_secure_boot_host_reset(void)
{
  /* Pre 1: the host state struct lives in BSS so its address is always
   *        non-NULL on any hosted environment we run on. */
  /* Pre 2: ``s_tag`` is a static const string used to silence the
   *        unused-symbol warning on the target build. */
  (void)s_tag;
  s_host = (ra_tz_secure_boot_host_state_t){};
  s_step = k_ra_tz_secure_boot_step_idle;
  /* Post 1: tests get a deterministic baseline. */
  /* Post 2: progress counter is back at idle. */
}

uint32_t ra_tz_secure_boot_host_blxns_target(void)
{
  /* Pre + post: pure accessor; no state change. */
  return s_host.blxns_target;
}

#else /* !RA_SIMULATOR_MODE */

void ra_tz_secure_boot_host_reset(void)
{
  /* Pre + post: target build exposes the symbol but the body is a
   * no-op so unit-test fixtures that link against the target stub do
   * not crash. */
  (void)s_tag;
}

uint32_t ra_tz_secure_boot_host_blxns_target(void)
{
  /* Pre + post: target build returns 0 because BLXNS never returns. */
  return 0U;
}

#endif /* RA_SIMULATOR_MODE */

/* =============================================================================
 * Small register helpers (host-aware)
 * =============================================================================
 */

/**
 * @brief Write a 32-bit MMIO register (or capture on host).
 *
 * @details On target the value is stored directly into the MMIO register
 *          at ``addr``. On host (``RA_SIMULATOR_MODE``) the value is
 *          captured in ``s_host`` so unit tests can inspect the write.
 *
 * @param[in] addr Target address.
 * @param[in] value Value to write.
 *
 * @pre ``addr`` is one of the documented SAU / CPSCU / VTOR addresses.
 * @pre Caller is in Secure state (target) / unit-test context (host).
 * @post Target write lands; host capture updated.
 * @post Caller can verify via the host-state accessors.
 * @note Not thread-safe; runs only during secure boot.
 * @since 0.1.0
 */
static void internal_write32(uintptr_t addr, uint32_t value)
{
#ifdef RA_SIMULATOR_MODE
  if (addr == (uintptr_t)k_ra_tz_ipcsar_addr) {
    s_host.ipcsar_value = value;
  } else if (addr == (uintptr_t)k_ra_tz_ipcpar_addr) {
    s_host.ipcpar_value = value;
  } else if (addr == (uintptr_t)k_ra_tz_scb_vtor_ns_addr) {
    s_host.vtor_ns = value;
  } else if (addr == (uintptr_t)k_ra_tz_sau_ctrl_addr) {
    s_host.sau_ctrl = value;
  }
  /* SAU RBAR / RLAR writes are routed through internal_sau_set_region. */
#else
  /* HUM Ch 3.2.1 "IPCSAR" p 205 and HUM Ch 9.2.4 "PRCR_S" p 397 for
   * the secure-only writes routed through this helper. Generic 32-bit
   * MMIO store; the called sites cite their own register page. */
  *(volatile uint32_t*)addr = value;
#endif
}

/**
 * @brief Read a 32-bit MMIO register (or canned host value).
 *
 * @details On target the function dereferences the MMIO address.
 *          On host (``RA_SIMULATOR_MODE``) it returns the canned values
 *          tests expect for SAU_TYPE (= 8 regions) and IPCSAR.
 *
 * @param[in] addr Target address.
 * @return Read value (host: documented constant for SAU_TYPE).
 * @retval 8U          When ``addr == k_ra_tz_sau_type_addr`` on host.
 * @retval s_host.ipcsar_value  When ``addr == k_ra_tz_ipcsar_addr`` on host.
 * @retval 0U          For any other addr on host.
 *
 * @pre  ``addr`` is one of the documented SAU / CPSCU addresses.
 * @pre  Caller is in Secure state on target.
 * @post No state change.
 * @post Returned value matches the most recent write on host.
 * @note Not thread-safe.
 * @since 0.1.0
 */
static uint32_t internal_read32(uintptr_t addr)
{
#ifdef RA_SIMULATOR_MODE
  if (addr == (uintptr_t)k_ra_tz_sau_type_addr) {
    /* Cortex-M85 implements 8 SAU regions; tests rely on that. */
    return 8U;
  }
  if (addr == (uintptr_t)k_ra_tz_ipcsar_addr) {
    return s_host.ipcsar_value;
  }
  return 0U;
#else
  /* HUM Ch 3.2.1 "IPCSAR" p 205 -- only IPCSAR is ever read back here
   * after the write; everything else (SAU_TYPE) is architectural. */
  return *(volatile uint32_t*)addr;
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
 *      ``k_ra_tz_prcr_s_addr``.
 * @pre Caller is in Secure state.
 * @post Target write lands; host capture updated.
 * @post Caller can verify via the host-state accessors.
 * @note Not thread-safe.
 * @since 0.1.0
 */
static void internal_write16(uintptr_t addr, uint16_t value)
{
#ifdef RA_SIMULATOR_MODE
  s_host.prcr_s_last = value;
  if ((value & (uint16_t)k_ra_tz_prcr_s_prc4_open) != 0U) {
    s_host.prcr_unlock_count = (uint8_t)(s_host.prcr_unlock_count + 1U);
  } else {
    s_host.prcr_relock_count = (uint8_t)(s_host.prcr_relock_count + 1U);
  }
  (void)addr;
#else
  /* HUM Ch 9.2.4 "PRCR_S" p 397 */
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
static inline void internal_dsb(void)
{
#ifndef RA_SIMULATOR_MODE
  __asm__ volatile("dsb 0xF" ::: "memory");
#endif
}

/**
 * @brief Emit an Instruction Synchronisation Barrier (no-op on host).
 *
 * @details Wraps the ``isb 0xF`` inline-asm so the file's hot path
 *          stays readable. Host build compiles to a true no-op.
 *
 * @pre  Caller is in Secure state (any context valid on host).
 * @pre  Used only inside the secure-boot sequence.
 * @post Pipeline flushed; subsequent fetches see post-write SAU state.
 * @post On host: no-op.
 * @note Not thread-safe.
 * @since 0.1.0
 */
static inline void internal_isb(void)
{
#ifndef RA_SIMULATOR_MODE
  __asm__ volatile("isb 0xF" ::: "memory");
#endif
}

/* =============================================================================
 * SAU region programming
 * =============================================================================
 */

/**
 * @brief Programme one SAU region via RNR/RBAR/RLAR.
 *
 * @details Selects region via SAU_RNR, writes the base address into
 *          SAU_RBAR, and writes ``limit | ENABLE [| NSC]`` into
 *          SAU_RLAR. Captures the values into ``s_host`` on host so
 *          the layout-sanity unit test can inspect them.
 *
 * @param[in] region Region index (0..SAU_TYPE.SREGION - 1).
 * @param[in] base   Base address (32-byte aligned).
 * @param[in] limit  Upper-bound minus 32 (32-byte aligned).
 * @param[in] is_nsc ``true`` to mark the region as Non-Secure Callable.
 *
 * @pre region < k_ra_tz_sau_region_count.
 * @pre Caller is in Secure state.
 * @post Region's RBAR + RLAR loaded.
 * @post Region enabled (RLAR.ENABLE = 1).
 * @note Not thread-safe.
 * @since 0.1.0
 */
static void internal_sau_set_region(uint8_t region, uint32_t base, uint32_t limit, bool is_nsc)
{
  internal_write32(k_ra_tz_sau_rnr_addr, (uint32_t)region);
  internal_write32(k_ra_tz_sau_rbar_addr, base);
  uint32_t rlar = limit | (uint32_t)k_ra_tz_sau_rlar_enable;
  if (is_nsc) {
    rlar |= (uint32_t)k_ra_tz_sau_rlar_nsc;
  }
  internal_write32(k_ra_tz_sau_rlar_addr, rlar);

#ifdef RA_SIMULATOR_MODE
  if ((uint32_t)region < (uint32_t)k_ra_tz_sau_region_count) {
    s_host.sau_region_base[region]  = base;
    s_host.sau_region_limit[region] = limit;
    s_host.sau_region_nsc[region]   = (uint8_t)(is_nsc ? 1U : 0U);
  }
#endif
}

ra_err_t ra_tz_secure_boot_sau_init(void)
{
  /* Pre: SAU_TYPE.SREGION must report >= 5 implemented regions. */
  const uint32_t sau_type = internal_read32(k_ra_tz_sau_type_addr);
  if ((sau_type & (uint32_t)k_ra_tz_sau_type_mask) < (uint32_t)k_ra_tz_sau_region_count) {
    ra_log_error(s_tag, "SAU_TYPE.SREGION below required count");
    return k_ra_err_not_supported;
  }

  internal_sau_set_region((uint8_t)k_ra_tz_sau_region_code_nsc,
                          (uint32_t)k_ra_tz_part_code_nsc_base,
                          (uint32_t)k_ra_tz_part_code_nsc_limit,
                          /*is_nsc=*/true);
  internal_sau_set_region((uint8_t)k_ra_tz_sau_region_ns_mram,
                          (uint32_t)k_ra_tz_part_ns_mram_base,
                          (uint32_t)k_ra_tz_part_ns_mram_limit,
                          /*is_nsc=*/false);
  internal_sau_set_region((uint8_t)k_ra_tz_sau_region_sram_nsc,
                          (uint32_t)k_ra_tz_part_sram_nsc_base,
                          (uint32_t)k_ra_tz_part_sram_nsc_limit,
                          /*is_nsc=*/true);
  internal_sau_set_region((uint8_t)k_ra_tz_sau_region_ns_sram,
                          (uint32_t)k_ra_tz_part_ns_sram_base,
                          (uint32_t)k_ra_tz_part_ns_sram_limit,
                          /*is_nsc=*/false);
  internal_sau_set_region((uint8_t)k_ra_tz_sau_region_ns_periph,
                          (uint32_t)k_ra_tz_part_ns_per_base,
                          (uint32_t)k_ra_tz_part_ns_per_limit,
                          /*is_nsc=*/false);

  internal_dsb();
  internal_write32(k_ra_tz_sau_ctrl_addr, (uint32_t)k_ra_tz_sau_ctrl_enable);
  internal_dsb();
  internal_isb();

  /* Post: SAU enabled with the canonical layout, default-deny. */
  s_step = k_ra_tz_secure_boot_step_sau_done;
  return k_ra_ok;
}

ra_err_t ra_tz_secure_boot_security_init(uint32_t ipcsar_value, uint32_t ipcpar_value)
{
  /* Pre 1: caller must be in Secure state (architectural; cannot be
   * checked from C, documented in the header). */
  /* Pre 2: PRCR_S unlock must observe the documented key + bit
   * pattern, otherwise the chip silently drops the write. */

  /* Open the PRC4 gate so the next IPCSAR / IPCPAR writes land. */
  /* HUM Ch 9.2.4 "PRCR_S" p 397 */
  internal_write16(k_ra_tz_prcr_s_addr, (uint16_t)k_ra_tz_prcr_s_open);
  s_step = k_ra_tz_secure_boot_step_prcr_unlocked;

  /* HUM Ch 3.2.1 "IPCSAR" p 205-207 */
  internal_write32(k_ra_tz_ipcsar_addr, ipcsar_value);
  /* HUM Ch 3.2.2 "IPCPAR" p 208-209 */
  internal_write32(k_ra_tz_ipcpar_addr, ipcpar_value);
  s_step = k_ra_tz_secure_boot_step_ipcsar_written;

  /* Close the PRC4 gate to restore write-protect on CPSCU. */
  /* HUM Ch 9.2.4 "PRCR_S" p 397 */
  internal_write16(k_ra_tz_prcr_s_addr, (uint16_t)k_ra_tz_prcr_s_close);
  s_step = k_ra_tz_secure_boot_step_prcr_relocked;

  internal_dsb();
  /* Post 1: IPCSAR landed (verifiable via JTAG read-back). */
  /* Post 2: PRCR_S.PRC4 clear (write-protect restored). */
  return k_ra_ok;
}

uint32_t ra_tz_ns_signed_body_len(const uint32_t* ns_vector_table)
{
  /* Validation 1: reject a NULL image base (returns the deny sentinel 0). */
  if (ns_vector_table == nullptr) {
    ra_log_error(s_tag, "ns_vector_table is NULL");
    return 0U;
  }
  /* The NS linker emits an ::ra_ns_rot_header_t at a fixed small offset from the
   * NS base (just past the 16-slot ARMv8-M vector table -- see
   * ::k_ra_tz_ns_rot_header_offset), self-describing the signed body length.
   * This is a plain memory read of the (already-flashed/copied) NS image; there
   * is no MMIO register access here, so no HUM citation applies. */
  const uint8_t*            base = (const uint8_t*)(const void*)ns_vector_table;
  const ra_ns_rot_header_t* header =
    (const ra_ns_rot_header_t*)(const void*)(base + (uintptr_t)k_ra_tz_ns_rot_header_offset);
  /* Validation 2: reject a missing / wrong-magic header (returns 0 -> deny). */
  if (header->magic != (uint32_t)k_ra_tz_ns_rot_header_magic) {
    ra_log_error(s_tag, "NS RoT header magic mismatch");
    return 0U;
  }
  return header->body_len;
}

/**
 * @brief Authenticate the Non-Secure image before BLXNS (default-deny gate).
 *
 * @details
 * Root-of-trust gate factored out of ::ra_tz_secure_boot_jump_ns so the caller
 * stays within the function-length budget. On a target build with
 * ``RA_ENABLE_ROOT_OF_TRUST`` enabled it reads the NS image's self-describing
 * signed-body length via ::ra_tz_ns_signed_body_len (an ::ra_ns_rot_header_t the
 * NS linker emits at ::k_ra_tz_ns_rot_header_offset), locates the trailer that
 * the signing tool appended immediately after that body via
 * ::ra_rot_trailer_after, then re-computes SHA-256 and verifies the NS image's
 * ECDSA-P256 signature against the provisioned root public key via
 * ::ra_rot_verify_image. This mirrors the copy-to-run boundary exactly (the
 * trailer sits at ``ns_vector_table + body_len``). Any failure -- a missing /
 * bad header, or a failed verify -- returns a non-``k_ra_ok`` error and the
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
 * With the flag OFF (default) -- or under ``RA_SIMULATOR_MODE``, where the NS
 * image and trailer do not exist at a real address on the unit-test host -- the
 * gate is absent and this returns ``k_ra_ok`` so the jump proceeds unverified,
 * exactly as before. The gate's decision logic is covered directly in
 * ``tests/test_ra_root_of_trust.c`` and the header read in
 * ``tests/test_tz_secure_boot.c``.
 *
 * @param[in] ns_vector_table Base of the NS image (its vector table); non-NULL.
 *
 * @return ra_err_t Error code.
 * @retval k_ra_ok                   Image authentic, or verification is disabled.
 * @retval k_ra_err_null_ptr         ``ns_vector_table`` is NULL.
 * @retval k_ra_err_validation_failed NS RoT header missing / wrong magic.
 * @retval k_ra_err_invalid_size     Header body length out of range.
 * @retval k_ra_err_*                Root-of-trust gate denied the NS image.
 *
 * @pre ``ns_vector_table`` is non-NULL.
 * @pre On an enabled target build, the NS image carries an ::ra_ns_rot_header_t
 *      at ::k_ra_tz_ns_rot_header_offset and a signed ::ra_rot_trailer_t at
 *      ``ns_vector_table + body_len``.
 * @post On any non-``k_ra_ok`` return the caller does NOT BLXNS.
 * @post No NS image bytes are modified.
 *
 * @note Not thread-safe; runs once at boot.
 * @since 0.1.0
 */
static ra_err_t internal_ns_verify_or_deny(const uint32_t* ns_vector_table)
{
  RA_CHECK_NULL_PTR(ns_vector_table, s_tag, "ns_vector_table");
#if !defined(RA_SIMULATOR_MODE) && defined(RA_ENABLE_ROOT_OF_TRUST)
  const uint32_t ns_body_len = ra_tz_ns_signed_body_len(ns_vector_table);
  if (ns_body_len == 0U) {
    ra_log_error(s_tag, "NS RoT header missing/invalid -- denying BLXNS");
    return k_ra_err_validation_failed;
  }
  const ra_rot_trailer_t* ns_trailer = ra_rot_trailer_after(ns_vector_table, ns_body_len);
  if (ns_trailer == nullptr) {
    ra_log_error(s_tag, "NS RoT body_len out of range -- denying BLXNS");
    return k_ra_err_invalid_size;
  }
  return ra_rot_verify_image((const uint8_t*)(const void*)ns_vector_table, ns_body_len, ns_trailer);
#else
  (void)ns_vector_table;
  return k_ra_ok; /* root of trust disabled (or host build): no verification */
#endif /* !RA_SIMULATOR_MODE && RA_ENABLE_ROOT_OF_TRUST */
}

ra_err_t ra_tz_secure_boot_jump_ns(const uint32_t* ns_vector_table)
{
  RA_CHECK_NULL_PTR(ns_vector_table, s_tag, "ns_vector_table");
  /* Pre 2: 4-byte alignment. */
  if (((uintptr_t)ns_vector_table & 0x3U) != 0U) {
    ra_log_error(s_tag, "ns_vector_table misaligned");
    return k_ra_err_invalid_arg;
  }

  const uint32_t initial_sp  = ns_vector_table[0];
  const uint32_t reset_entry = ns_vector_table[1];

  /* Reject obviously bogus reset vectors -- 0 (all zeros) or
   * 0xFFFFFFFF (erased MRAM). FSP guards the same way. */
  if (reset_entry == 0U || reset_entry == UINT32_MAX) {
    ra_log_error_val(s_tag, "NS reset vector invalid", reset_entry);
    return k_ra_err_invalid_arg;
  }

  /* Root-of-trust gate before BLXNS: deny the jump on a failed authentication
   * (no-op when RA_ENABLE_ROOT_OF_TRUST is off -- see internal_ns_verify_or_deny). */
  const ra_err_t auth_err = internal_ns_verify_or_deny(ns_vector_table);
  if (auth_err != k_ra_ok) {
    ra_log_error(s_tag, "NS image authentication failed -- denying BLXNS");
    return auth_err;
  }

  /* HUM Ch 4 "Option Setting Memory" + ARMv8-M ARM B3.2.4: VTOR_NS
   * accessed via the 0xE002... NS alias of SCB.VTOR (=0xE002ED08). */
  internal_write32(k_ra_tz_scb_vtor_ns_addr, (uint32_t)(uintptr_t)ns_vector_table);
  s_step = k_ra_tz_secure_boot_step_blxns_armed;

#ifdef RA_SIMULATOR_MODE
  s_host.blxns_target = reset_entry;
  s_host.blxns_msp_ns = initial_sp;
  s_step              = k_ra_tz_secure_boot_step_branched;
  return k_ra_ok;
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
  return k_ra_ok;
#endif
}

ra_err_t
ra_tz_secure_boot_run(uint32_t ipcsar_value, uint32_t ipcpar_value, const uint32_t* ns_vector_table)
{
  RA_CHECK_NULL_PTR(ns_vector_table, s_tag, "ns_vector_table");

  ra_err_t err = ra_tz_secure_boot_sau_init();
  if (err != k_ra_ok) {
    return err;
  }

  err = ra_tz_secure_boot_security_init(ipcsar_value, ipcpar_value);
  if (err != k_ra_ok) {
    return err;
  }

  /* Post: the next call BLXNS-es out and never returns on target. */
  return ra_tz_secure_boot_jump_ns(ns_vector_table);
}

ra_tz_secure_boot_step_t ra_tz_secure_boot_get_step(void)
{
  /* Pre: progress counter lives in BSS so it is always readable. */
  /* Pre: single 32-bit volatile load -- atomic on all targets. */
  /* Post: returned value matches the most recent step write. */
  /* Post: no state change. */
  return s_step;
}
