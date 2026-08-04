/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file ra8_dfu_antirollback.c
 * @brief DFU anti-rollback (downgrade protection) -- policy + storage seam.
 *
 * @par Tag
 * [Ring 4 / Service] {World: S}
 *
 * @details
 * See ``ra8_dfu_antirollback.h`` for the full contract. This file owns the pure
 * downgrade policy, the verify-and-commit flow over the injected storage
 * vtable, and the non-faking default store. No raw MMIO is performed here: the
 * non-volatile counter is reached only through the
 * ::ra8_rot_antirollback_store_t function pointers.
 *
 * The whole module is opt-in behind ``RA8_ENABLE_ROOT_OF_TRUST`` (default OFF),
 * mirroring ``ra8_rot.c``. With the flag OFF this file compiles to nothing --
 * only the link-free declarations from ``ra8_dfu_antirollback.h`` -- so the
 * default build is byte-for-byte unchanged and pulls in no extra link
 * dependency.
 */

#include "ra8_dfu_antirollback.h"

#ifdef RA8_ENABLE_ROOT_OF_TRUST

#include <string.h>

#include "ra8_attributes.h"
#include "ra8_check.h"
#include "ra8_flash_core.h"
#include "ra8_flash_regs.h"

/**
 * @brief NV anti-rollback backing constants.
 * @details The durable highest-accepted version is a single little-endian
 *          ``uint32_t`` at the base of the extra-MRAM option-setting window
 *          (::k_ra8_flash_extra_start, 0x02E07600 -- HUM Ch 59.7.4.5 Table 59.15
 *          p 3592). A commit is a monotone-advancing write (bits go 1->0 only),
 *          so no erase cycle is needed and there is no power-loss window where
 *          the counter reads back as reset.
 * @note #315 answered the bench question on EK-RA8D2 silicon (M85 r1p1): a virgin
 *       extra-MRAM page in the corrected option-setting window (bench-tested at the
 *       General-Purpose-OTP sub-range 0x02E076A0, HUM Ch 59.7.4.5 Table 59.15
 *       p 3592) programs cleanly write-first -- no prior read -- via the MACI
 *       Program command and reads back byte-exact with valid ECC. A virgin read
 *       returns 0xFFFFFFFF (== ::k_ra8_rot_ar_erased) WITHOUT faulting. Whether the
 *       counter belongs at the window base (the FSBL-setting word) rather than a
 *       dedicated slot (GPOTP, or the ARC structure at 0x02E17930) is a design
 *       choice deferred to #314.
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_ra8_rot_ar_erased = 0xFFFFFFFFU, /**< Erased word: no version stored yet. */
} ra8_rot_ar_nv_t;

#ifdef RA8_OFF_TARGET
/**
 * @var s_fake_ar_counter
 * @brief Host-test RAM shadow of the extra-MRAM anti-rollback counter.
 * @details The host unit-test fake models the flash MACI *registers* but
 *          not the extra-MRAM *data* side, so writes never round-trip through
 *          ``k_ra8_flash_extra_start``. Under RA8_OFF_TARGET the durable read/commit use
 *          this word instead; silicon and ra8_emulator exercise the real
 *          extra-MRAM path in the ``#else`` branch.
 * @note File-private; the anti-rollback host test seeds it directly.
 * @warning Test seam only -- never compiled into a silicon image.
 * @since 0.1.0
 */
static uint32_t s_fake_ar_counter = (uint32_t)k_ra8_rot_ar_erased;
#endif

/**
 * @var s_tag
 * @brief Logger tag for the anti-rollback module.
 * @details Component identifier used by ``ra8_log_*`` / ``RA8_CHECK_*`` so
 *          anti-rollback log lines are easy to grep for in RTT output.
 * @note    File-private; never accessed from outside.
 * @warning Do not modify.
 * @since   0.1.0
 */
static const char* s_tag = "ROLLBACK";

ra8_err_t ra8_rot_antirollback_check(uint32_t image_version, uint32_t stored_min_version)
{
  /* Downgrade: a strictly older image carries since-patched defects -- deny. */
  if (image_version < stored_min_version) {
    ra8_log_error(s_tag, "anti-rollback: image version below stored minimum");
    return k_ra8_err_validation_failed;
  }
  /* Newer or equal: not a downgrade -- accept. */
  return k_ra8_ok;
}

#ifndef RA8_OFF_TARGET
/**
 * @var g_ra8_rot_ar_probing
 * @brief Set only while ::internal_probe_counter reads the counter word.
 * @details The app BusFault/HardFault handler routes to
 *          ::ra8_rot_antirollback_on_probe_fault, which recovers the deliberate
 *          fault from reading a blank extra-MRAM word only when this is set.
 * @warning Boot-path only; not thread-safe.
 * @since 0.1.0
 */
volatile bool g_ra8_rot_ar_probing = false;

/**
 * @var g_ra8_rot_ar_faulted
 * @brief Set by ::ra8_rot_antirollback_on_probe_fault when a probe read faulted.
 * @warning Boot-path only; not thread-safe.
 * @since 0.1.0
 */
volatile bool g_ra8_rot_ar_faulted = false;

/**
 * @enum ra8_ar_probe_reg_t
 * @brief Cortex-M constants for the counter-probe fault recovery.
 */
typedef enum : uint32_t {
  k_ra8_ar_cfsr_addr    = 0xE000ED28U, /**< SCB Configurable Fault Status Register.   */
  k_ra8_ar_thumb_hw_msk = 0x0000F800U, /**< Thumb first-halfword [15:11] field mask.  */
  k_ra8_ar_thumb32_min  = 0x0000E800U, /**< [15:11] >= this halfword -> 32-bit Thumb. */
} ra8_ar_probe_reg_t;

bool ra8_rot_antirollback_on_probe_fault(uint32_t* exc_frame)
{
  if (exc_frame == nullptr || !g_ra8_rot_ar_probing) {
    return false;
  }
  g_ra8_rot_ar_probing = false;
  g_ra8_rot_ar_faulted = true;
  /* Advance the stacked PC past the faulting load so the exception return resumes
   * at the next instruction. The basic exception frame is
   * [R0 R1 R2 R3 R12 LR PC xPSR]; index 6 is the stacked PC. A Thumb instruction
   * is 32-bit iff its first halfword's bits [15:11] are >= 0b11101. */
  const uint16_t instr = *(const volatile uint16_t*)(uintptr_t)exc_frame[6];
  exc_frame[6] +=
    ((instr & (uint16_t)k_ra8_ar_thumb_hw_msk) >= (uint16_t)k_ra8_ar_thumb32_min) ? 4U : 2U;
  /* W1C-clear the sticky Configurable Fault Status so this deliberate fault does
   * not shadow a later real one. Reading the live value and writing it back is
   * a real read-modify-write on the device: every bit that reads as 1 is
   * written as 1, which clears it (write-1-to-clear semantics). */
  volatile uint32_t* const cfsr        = (volatile uint32_t*)(uintptr_t)k_ra8_ar_cfsr_addr;
  const uint32_t           cfsr_sticky = *cfsr;
  *cfsr                                = cfsr_sticky;
  __asm__ volatile("dsb 0xF\n isb 0xF\n" ::: "memory");
  return true;
}

/**
 * @brief Fault-tolerant read of the durable counter word (blank maps to erased).
 *
 * @details
 * The RA8D2 has no MRAM BlankCheck command, so a blank word cannot be tested for
 * without reading it. #315 bench-proved on silicon that a virgin word in the
 * corrected option-setting window READS BACK as 0xFFFFFFFF (== ::k_ra8_rot_ar_erased)
 * WITHOUT faulting: the bus fault #194 recorded was against the phantom 0x27000000
 * address, which does not decode on this part (an address-decode abort, corrected
 * by #397), not an uncorrectable-ECC fault of a real blank word. The transient
 * fault-catch is therefore retained only as belt-and-braces for the counter's own
 * (untested) location; on the corrected window it is not expected to trigger.
 *
 * @param[out] out_blank Set true when the word was blank (the probe read faulted).
 *
 * @return The raw counter word, or ::k_ra8_rot_ar_erased when blank.
 * @retval k_ra8_rot_ar_erased The word is unprogrammed (a probe fault occurred and
 *         ``*out_blank`` is true).
 *
 * @pre The app fault handler routes to ::ra8_rot_antirollback_on_probe_fault.
 * @pre ``out_blank`` is a valid sink.
 * @post ``*out_blank`` reflects whether the counter word is unprogrammed.
 * @post No flash or RSIP state is mutated.
 *
 * @note Not thread-safe; boot-path only.
 * @since 0.1.0
 */
RA8_INTERNAL static uint32_t internal_probe_counter(bool* out_blank)
{
  g_ra8_rot_ar_faulted = false;
  g_ra8_rot_ar_probing = true;
  __asm__ volatile("dsb 0xF\n isb 0xF\n" ::: "memory");
  const volatile uint32_t raw = *(const volatile uint32_t*)(uintptr_t)k_ra8_flash_extra_start;
  __asm__ volatile("dsb 0xF\n isb 0xF\n" ::: "memory");
  g_ra8_rot_ar_probing = false;
  *out_blank           = g_ra8_rot_ar_faulted;
  return g_ra8_rot_ar_faulted ? (uint32_t)k_ra8_rot_ar_erased : raw;
}
#endif /* !RA8_OFF_TARGET */

/**
 * @brief Default-store read: load the durable highest-accepted version.
 *
 * @details
 * Reads the little-endian ``uint32_t`` counter at the base of the extra-MRAM
 * (data-flash) window (::k_ra8_flash_extra_start) through the fault-tolerant
 * ::internal_probe_counter on silicon: a never-programmed word reads back as
 * 0xFFFFFFFF (the RA8D2 has no BlankCheck, but #315 bench-proved a virgin read does
 * not fault -- #194's fault was the phantom 0x27000000 address, corrected by #397),
 * mapping to ::k_ra8_rot_ar_erased and version 0 -- a fresh device has accepted
 * nothing, so any image version is ``>= 0`` and passes the pure policy.
 *
 * @param[out] out_min_version Receives the stored highest-accepted version, or 0
 *             when the counter has never been programmed.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok           ``*out_min_version`` holds the durable version.
 * @retval k_ra8_err_null_ptr ``out_min_version`` is NULL.
 *
 * @pre ``out_min_version`` is the caller's stored-version sink.
 * @pre On silicon, the app fault handler routes to
 *      ::ra8_rot_antirollback_on_probe_fault for the blank-word probe.
 * @post ``*out_min_version`` holds the durable counter (0 when erased).
 * @post No flash or RSIP state is mutated.
 *
 * @note Thread-safe: no (shares the extra-MRAM counter with the commit path).
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_default_store_read(uint32_t* out_min_version)
{
  RA8_CHECK_NULL_PTR(out_min_version, s_tag, "out_min_version");
#ifdef RA8_OFF_TARGET
  const uint32_t raw = s_fake_ar_counter;
#else
  bool           blank = false;
  const uint32_t raw   = internal_probe_counter(&blank);
#endif
  *out_min_version = (raw == (uint32_t)k_ra8_rot_ar_erased) ? 0U : raw;
  return k_ra8_ok;
}

/**
 * @brief Default-store commit: durably advance the highest-accepted version.
 *
 * @details
 * The caller reaches this only after the pure policy accepted ``new_version``
 * (``new_version >= stored``). Re-reads the durable counter and writes
 * ``new_version`` to extra-MRAM only when it is strictly newer, so a same- or
 * lower-version reboot performs no write (extra-MRAM is bit-alterable, so the
 * write is a plain program with no erase cycle). A program fault propagates so
 * ::ra8_rot_antirollback_verify default-denies the launch.
 *
 * @param[in] new_version The accepted version to persist as the new floor.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok                 Counter is at or above ``new_version``.
 * @retval k_ra8_err_invalid_arg    Flash write rejected the request.
 * @retval k_ra8_err_hw_error       Flash reported an error after the program.
 * @retval k_ra8_err_hw_timeout     Flash MACI never returned ready.
 *
 * @pre The caller has already accepted the image via the pure policy.
 * @pre Extra-MRAM is programmable (module powered, not locked).
 * @post On success the durable counter is ``>= new_version``.
 * @post On failure the durable counter is unchanged.
 *
 * @note Thread-safe: no (shares the extra-MRAM counter with the read path).
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_default_store_commit(uint32_t new_version)
{
#ifdef RA8_OFF_TARGET
  const uint32_t raw   = s_fake_ar_counter;
  const bool     blank = false;
#else
  bool           blank = false;
  const uint32_t raw   = internal_probe_counter(&blank);
#endif
  const uint32_t stored = (raw == (uint32_t)k_ra8_rot_ar_erased) ? 0U : raw;
  if (new_version <= stored) {
    return k_ra8_ok; /* already at or above the floor -- nothing to persist */
  }
#ifdef RA8_OFF_TARGET
  (void)blank;
  s_fake_ar_counter = new_version;
  return k_ra8_ok;
#else
  uint8_t le[sizeof(uint32_t)] = {};
  (void)memcpy(le, &new_version, sizeof(le)); /* both toolchains little-endian */
  const ra8_err_t wr =
    ra8_flash_extra_mram_write((uint32_t)k_ra8_flash_extra_start, le, (uint32_t)sizeof(le));
  /* #315 bench-proved on silicon that a virgin extra-MRAM page CAN be programmed at
   * runtime write-first (no prior read) and reads back byte-exact with valid ECC, so
   * the commit programs the counter directly and returns the write result. The
   * earlier "#194: the read leaves the controller unable to program a fresh word"
   * claim was an artifact of the phantom 0x27000000 window (corrected by #397). The
   * ``blank`` leg is retained defensively -- it is not expected on the corrected
   * window (a virgin read returns 0xFFFFFFFF without faulting, so ``blank`` stays
   * false) -- and lets a signature-authenticated fresh device launch its first
   * authentic image without a persisted floor. On a real write error the commit
   * default-denies the launch. */
  return blank ? k_ra8_ok : wr;
#endif
}

/**
 * @var s_default_store
 * @brief The process-lifetime extra-MRAM-backed default store.
 * @details Wires the durable read/commit above (extra-MRAM counter at
 *          ::k_ra8_flash_extra_start). Returned by
 *          ::ra8_rot_antirollback_default_store so the launch path enforces a
 *          real, reboot-persistent anti-rollback floor.
 * @note    File-private; exposed only through the accessor.
 * @warning Do not modify; both members must reference the durable NV backing.
 * @since   0.1.0
 */
static const ra8_rot_antirollback_store_t s_default_store = {
  .read   = internal_default_store_read,
  .commit = internal_default_store_commit,
};

ra8_err_t ra8_rot_antirollback_verify(const ra8_rot_antirollback_store_t* store,
                                      uint32_t                            image_version)
{
  RA8_CHECK_NULL_PTR(store, s_tag, "store");
  RA8_CHECK_NULL_PTR(store->read, s_tag, "store->read");
  RA8_CHECK_NULL_PTR(store->commit, s_tag, "store->commit");

  /* Read the stored highest-accepted version. A read fault is DEFAULT-DENY. */
  uint32_t        stored_min = 0U;
  const ra8_err_t read_err   = store->read(&stored_min);
  RA8_RETURN_ON_ERROR(read_err, s_tag, "anti-rollback: stored-version read failed");

  /* Apply the pure downgrade policy. A downgrade returns validation_failed. */
  const ra8_err_t policy_err = ra8_rot_antirollback_check(image_version, stored_min);
  RA8_RETURN_ON_ERROR(policy_err, s_tag, "anti-rollback: downgrade rejected");

  /* Accepted: advance the durable counter. A commit fault is DEFAULT-DENY. */
  const ra8_err_t commit_err = store->commit(image_version);
  RA8_RETURN_ON_ERROR(commit_err, s_tag, "anti-rollback: counter commit failed");

  return k_ra8_ok;
}

const ra8_rot_antirollback_store_t* ra8_rot_antirollback_default_store(void)
{
  return &s_default_store;
}

#endif /* RA8_ENABLE_ROOT_OF_TRUST */
