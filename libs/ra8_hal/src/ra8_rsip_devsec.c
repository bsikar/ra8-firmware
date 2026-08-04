/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file ra8_rsip_devsec.c
 * @brief RSIP-E50D device-security path (lifecycle / debug / tamper / DPA), fail-closed
 *
 * @par Tag
 * [Ring 3 / HAL] {World: S}
 *
 * @details
 * Device-security slice of the RA8D2 RSIP-E50D HAL: the device-lifecycle
 * state, the three debug-authorisation levels (AL0/AL1/AL2), the tamper
 * subsystem, and the SPA / DPA side-channel arm. It was split out of
 * ``ra8_rsip_asym.c`` (issue #216) both to keep every translation unit under
 * the file-size budget and because this cluster is FAIL-CLOSED for a reason
 * distinct from the hash / key fiction in ``ra8_rsip_asym.c``.
 *
 * These functions used to drive an invented "RSIP security-state" register
 * block (``LIFE_STATE`` @ 0xC0, ``DEBUG_LEVEL`` @ 0xC4, ``TAMPER_CTRL`` @ 0x14,
 * ``TAMPER_STATUS`` @ 0x18, ``DPA_CTRL`` @ 0x1C, ``CTRL.DPA_ARM``) inside the
 * RSIP window (base 0x403B0000) and cited HUM Ch 51 "Security Features". That
 * register model is fiction, verified against the manual for issue #216:
 *
 * - HUM Ch 51 "Security Features" (p 3263-3301) is a prose feature INDEX, not
 *   a register map. 51.1 lists "Device lifecycle management" and the "Three
 *   debug levels" (AL0/AL1/AL2) in words; 51.2 "Tamper Detection" describes
 *   tamper as an I/O-port feature whose effects are handled by other blocks
 *   and cross-references RTC (Ch 26 timestamp), MRAM (Ch 59 HUK zeroize), and
 *   the Battery Backup Function (Ch 12 VBATT zeroize); 51.3 documents the Arm
 *   TrustZone IDAU / SAU / MSAU. None of it defines a lifecycle / debug-level /
 *   tamper register at an RSIP offset.
 * - HUM Ch 52 "Renesas Secure IP (RSIP-E50D)" (p 3302-3307) is a six-page
 *   conceptual overview (block diagrams + an "input key, input data, read
 *   result" procedure) with no command-register map at all -- the same
 *   invented-MMIO finding as #214 / #215 / #181.
 *
 * The real RA8D2 device-security state does NOT live behind an RSIP MMIO read:
 * the device lifecycle and the secure-debug (AL0/AL1/AL2) authorisation are
 * governed by Device Lifecycle Management (DLM) via the debug / serial-
 * programming interface and the option-setting memory, the TrustZone boundary
 * by the SAU / IDAU registers, and tamper response by the RTC / MRAM / VBATT
 * blocks. A security-state query that fabricates a plausible-looking "secure"
 * answer is worse than one that refuses -- a caller could trust a lie about
 * the lifecycle or debug level -- so every entry point here FAILS CLOSED in a
 * production build: it returns ``k_ra8_err_not_supported`` and writes no
 * fabricated state. The insecure off-target command path (which only round-trips
 * the register fake) is retained behind the established stub-crypto guard
 * so ``ra8_rsip_devsec.c`` is enforced by ``check_stub_crypto_guarded.py``; a
 * production image compiles the fail-closed ``#else`` and can never mistake the
 * fake bytes for a real lifecycle / debug / tamper state.
 *
 * @note TODO(no in-tree DLM / option-byte / secure-debug driver): there is no
 * real RA8D2 device-lifecycle / debug-authorisation / tamper backend in this
 * tree today, and this path has zero production consumers. When a consumer
 * genuinely needs real security state, wire it to the DLM / option-setting-
 * memory / SAU source rather than re-enabling the invented RSIP registers.
 *
 *
 * @since 0.1.0
 */

#include <stdint.h>

#include "ra8_check.h"
#include "ra8_err.h"
#include "ra8_log.h"
#include "ra8_rsip.h"
#include "ra8_rsip_regs.h"

/**
 * @var s_tag
 * @brief Logger tag used by every ``ra8_log_*`` call in this TU.
 *
 * @details
 * Kept short ("RSIP") so it fits in the fixed-width log prefix without
 * truncation. Each RSIP translation unit keeps its own private copy.
 *
 * @note Static, file-scope.
 * @since 0.1.0
 */
static const char* s_tag = "RSIP";

/*
 * The device-security registers modelled below (lifecycle state, debug level,
 * tamper control / status, DPA arm) are NOT documented RA8D2 registers: HUM
 * Ch 51 "Security Features" (p 3263-3301) is a prose feature index and HUM
 * Ch 52 "Renesas Secure IP (RSIP-E50D)" (p 3302-3307) is a conceptual overview
 * with no register map -- the real state lives in the DLM / option-setting
 * memory / SAU, not an RSIP MMIO word. The command-path bodies here only
 * round-trip the host register fake; they compute no real security state.
 * They compile only under the insecure-stub / off-target guard so a production
 * image gets the fail-closed #else and can never mistake these bytes for a
 * genuine lifecycle / debug / tamper reading. The register pokes below
 * therefore carry NO HUM citation -- there is no real register map to cite; the
 * former "HUM Ch 51.1 / 51.5 / 51.6" citations were fabricated and are removed.
 */
#if defined(RA8_INSECURE_STUB_CRYPTO) || defined(RA8_OFF_TARGET)

ra8_err_t ra8_rsip_life_get(ra8_rsip_life_state_t* out)
{
  RA8_CHECK_NULL_PTR(out, s_tag, "out must not be nullptr");
  *out = (ra8_rsip_life_state_t)*ra8_rsip_reg32(k_ra8_rsip_off_life_state);
  return k_ra8_ok;
}

ra8_err_t ra8_rsip_life_advance(ra8_rsip_life_state_t state)
{
  if ((uint32_t)state > k_ra8_rsip_life_rma) {
    return k_ra8_err_invalid_arg;
  }
  const uint32_t cur = *ra8_rsip_reg32(k_ra8_rsip_off_life_state);
  if ((uint32_t)state < cur) {
    return k_ra8_err_invalid_state;
  }
  *ra8_rsip_reg32(k_ra8_rsip_off_life_state) = (uint32_t)state;
  return k_ra8_ok;
}

ra8_err_t ra8_rsip_debug_level_get(ra8_rsip_debug_level_t* out)
{
  RA8_CHECK_NULL_PTR(out, s_tag, "out must not be nullptr");
  *out = (ra8_rsip_debug_level_t)*ra8_rsip_reg32(k_ra8_rsip_off_debug_level);
  return k_ra8_ok;
}

ra8_err_t ra8_rsip_debug_level_set(ra8_rsip_debug_level_t level)
{
  if ((uint32_t)level > (uint32_t)k_ra8_rsip_debug_al2) {
    return k_ra8_err_invalid_arg;
  }
  *ra8_rsip_reg32(k_ra8_rsip_off_debug_level) = (uint32_t)level;
  return k_ra8_ok;
}

ra8_err_t ra8_rsip_tamper_enable(uint32_t sources)
{
  if ((sources & ~k_ra8_rsip_tamper_src_all) != 0U) {
    return k_ra8_err_invalid_arg;
  }
  *ra8_rsip_reg32(k_ra8_rsip_off_tamper_ctrl) = sources;
  return k_ra8_ok;
}

ra8_err_t ra8_rsip_tamper_status(uint32_t* out)
{
  RA8_CHECK_NULL_PTR(out, s_tag, "out must not be nullptr");
  *out = *ra8_rsip_reg32(k_ra8_rsip_off_tamper_status);
  return k_ra8_ok;
}

ra8_err_t ra8_rsip_tamper_ack(uint32_t mask)
{
  if (mask == 0U) {
    return k_ra8_err_invalid_arg;
  }
  if ((mask & ~k_ra8_rsip_tamper_src_all) != 0U) {
    return k_ra8_err_invalid_arg;
  }
  *ra8_rsip_reg32(k_ra8_rsip_off_tamper_status) = mask;
  return k_ra8_ok;
}

ra8_err_t ra8_rsip_dpa_arm(bool enable)
{
  volatile uint32_t* ctrl = ra8_rsip_reg32(k_ra8_rsip_off_ctrl);
  if (enable) {
    *ctrl |= k_ra8_rsip_mask_ctrl_dpa_arm;
  } else {
    *ctrl &= ~k_ra8_rsip_mask_ctrl_dpa_arm;
  }
  *ra8_rsip_reg32(k_ra8_rsip_off_dpa_ctrl) = (uint32_t)enable;
  return k_ra8_ok;
}

#else /* production build: neither RA8_INSECURE_STUB_CRYPTO nor RA8_OFF_TARGET */

/*
 * Fail-closed production variant. With no documented RA8D2 device-security
 * register interface (the real lifecycle / debug / tamper state lives in the
 * DLM / option-setting memory / SAU, not an RSIP MMIO word), every entry point
 * returns a hard error (never k_ra8_ok) and writes no fabricated state, so a
 * production image cannot mistake the fake command-path for a real
 * lifecycle, debug-authorisation, tamper, or side-channel reading.
 */

ra8_err_t ra8_rsip_life_get(ra8_rsip_life_state_t* out)
{
  RA8_CHECK_NULL_PTR(out, s_tag, "life_get: out must not be nullptr");
  return k_ra8_err_not_supported;
}

ra8_err_t ra8_rsip_life_advance(ra8_rsip_life_state_t state)
{
  (void)state;
  return k_ra8_err_not_supported;
}

ra8_err_t ra8_rsip_debug_level_get(ra8_rsip_debug_level_t* out)
{
  RA8_CHECK_NULL_PTR(out, s_tag, "debug_level_get: out must not be nullptr");
  return k_ra8_err_not_supported;
}

ra8_err_t ra8_rsip_debug_level_set(ra8_rsip_debug_level_t level)
{
  (void)level;
  return k_ra8_err_not_supported;
}

ra8_err_t ra8_rsip_tamper_enable(uint32_t sources)
{
  (void)sources;
  return k_ra8_err_not_supported;
}

ra8_err_t ra8_rsip_tamper_status(uint32_t* out)
{
  RA8_CHECK_NULL_PTR(out, s_tag, "tamper_status: out must not be nullptr");
  return k_ra8_err_not_supported;
}

ra8_err_t ra8_rsip_tamper_ack(uint32_t mask)
{
  (void)mask;
  return k_ra8_err_not_supported;
}

ra8_err_t ra8_rsip_dpa_arm(bool enable)
{
  (void)enable;
  return k_ra8_err_not_supported;
}

#endif /* RA8_INSECURE_STUB_CRYPTO || RA8_OFF_TARGET */
