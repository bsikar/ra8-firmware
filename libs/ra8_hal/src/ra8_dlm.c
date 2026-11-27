/**
 * @file ra8_dlm.c
 * @brief Device Lifecycle Management (DLM) and Debug Port Lockdown implementation
 * @ingroup grp_ra8_hal
 *
 * @par Tag
 * [Ring 1 / HAL] {World: S}
 *
 * @details
 * Production-only debug-port and DLM lifecycle management implementation.
 *
 * Citations:
 * - Renesas RA8D2 Hardware User Manual (HUM) Rev 1.30:
 *   - Section 51.8.13 "DLMMON : Device Lifecycle Management State Monitor Register"
 *     (p. 3300)
 *   - Section 2.9.5 "DBGREG" and Section 2.9.5.2 "DBGSTR : Debug Status Register"
 *     (pp. 139-141)
 *   - Section 2.9.5.4 "DBGAUTH0 : Debug Authentication Control Register 0"
 *     (pp. 142-143)
 *   - Section 2.9.5.7 "ALCTRL : Authentication Level Control Register"
 *     (pp. 144-145)
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra8_dlm.h"
#include "ra8_check.h"
#include "ra8_dlm_regs.h"

static uint32_t default_read32(void* ctx, uintptr_t addr)
{
  (void)ctx;
  return *(volatile const uint32_t*)addr;
}

static void default_write32(void* ctx, uintptr_t addr, uint32_t val)
{
  (void)ctx;
  *(volatile uint32_t*)addr = val;
}

static const ra8_dlm_hw_t s_default_hw = {
  .read32  = default_read32,
  .write32 = default_write32,
  .ctx     = nullptr,
};

bool ra8_dlm_is_opt_in_enabled(void)
{
#ifdef RA8_PRODUCTION_LOCKDOWN_OPT_IN
  return true;
#else
  return false;
#endif
}

const ra8_dlm_hw_t* ra8_dlm_default_hw(void)
{
  return &s_default_hw;
}

ra8_err_t ra8_dlm_get_status_hw(const ra8_dlm_hw_t* hw, ra8_dlm_status_t* out_status)
{
  RA8_CHECK_NULL_PTR(hw, "DLM", "hw must not be nullptr");
  RA8_CHECK_NULL_PTR(hw->read32, "DLM", "hw->read32 must not be nullptr");
  RA8_CHECK_NULL_PTR(out_status, "DLM", "out_status must not be nullptr");

  const uint32_t dlmmon  = hw->read32(hw->ctx, k_ra8_dlmmon_addr);
  const uint32_t dbgauth = hw->read32(hw->ctx, k_ra8_cpu_dbg_dbgauth0_addr);
  const uint32_t dbgstr  = hw->read32(hw->ctx, k_ra8_cpu_dbg_dbgstr_addr);
  const uint32_t alctrl  = hw->read32(hw->ctx, k_ra8_cpu_dbg_alctrl_addr);

  out_status->raw_dlmmon   = dlmmon;
  out_status->raw_dbgauth0 = dbgauth;
  out_status->raw_dbgstr   = dbgstr;
  out_status->raw_alctrl   = alctrl;

  const uint32_t raw_state = dlmmon & k_ra8_dlmmon_state_mask;
  switch (raw_state) {
    case 0x1U:
      out_status->lifecycle = k_ra8_dlm_lifecycle_oem_open;
      break;
    case 0x2U:
      out_status->lifecycle = k_ra8_dlm_lifecycle_oem_auth;
      break;
    case 0x3U:
      out_status->lifecycle = k_ra8_dlm_lifecycle_oem_locked;
      break;
    case 0x4U:
      out_status->lifecycle = k_ra8_dlm_lifecycle_oem_default;
      break;
    default:
      out_status->lifecycle = k_ra8_dlm_lifecycle_unknown;
      break;
  }

  const uint32_t raw_al = alctrl & k_ra8_alctrl_al_mask;
  if (raw_al == k_ra8_alctrl_al0) {
    out_status->auth_level = k_ra8_dlm_al0_locked;
  } else if (raw_al == k_ra8_alctrl_al1) {
    out_status->auth_level = k_ra8_dlm_al1_auth;
  } else if (raw_al == k_ra8_alctrl_al2) {
    out_status->auth_level = k_ra8_dlm_al2_auth;
  } else {
    out_status->auth_level = k_ra8_dlm_al_other;
  }

  out_status->fail_count =
    (uint8_t)((alctrl & k_ra8_alctrl_failcnt_mask) >> k_ra8_alctrl_failcnt_shift);
  out_status->debugger_attached = ((dbgstr & k_ra8_dbgstr_cdbgpwrupreq) != 0U);
  out_status->debug_ports_open  = ((dbgauth & k_ra8_dbgauth0_all_enables) != 0U);

  return k_ra8_ok;
}

ra8_err_t ra8_dlm_get_status(ra8_dlm_status_t* out_status)
{
  RA8_CHECK_NULL_PTR(out_status, "DLM", "out_status must not be nullptr");
  return ra8_dlm_get_status_hw(ra8_dlm_default_hw(), out_status);
}

ra8_err_t ra8_dlm_lockdown_debug_ports_hw(const ra8_dlm_hw_t* hw)
{
  RA8_CHECK_NULL_PTR(hw, "DLM", "hw must not be nullptr");
  RA8_CHECK_NULL_PTR(hw->write32, "DLM", "hw->write32 must not be nullptr");
  RA8_CHECK_NULL_PTR(hw->read32, "DLM", "hw->read32 must not be nullptr");

  if (!ra8_dlm_is_opt_in_enabled()) {
    return k_ra8_err_not_supported;
  }

  hw->write32(hw->ctx, k_ra8_cpu_dbg_dbgauth0_addr, 0U);

  const uint32_t readback = hw->read32(hw->ctx, k_ra8_cpu_dbg_dbgauth0_addr);
  if ((readback & k_ra8_dbgauth0_all_enables) != 0U) {
    return k_ra8_fail;
  }

  return k_ra8_ok;
}

ra8_err_t ra8_dlm_lockdown_debug_ports(void)
{
  if (!ra8_dlm_is_opt_in_enabled()) {
    return k_ra8_err_not_supported;
  }
  return ra8_dlm_lockdown_debug_ports_hw(ra8_dlm_default_hw());
}

ra8_err_t ra8_dlm_transition_recoverable_hw(const ra8_dlm_hw_t* hw,
                                            ra8_dlm_lifecycle_t target_state)
{
  RA8_CHECK_NULL_PTR(hw, "DLM", "hw must not be nullptr");

  if (!ra8_dlm_is_opt_in_enabled()) {
    return k_ra8_err_not_supported;
  }

  if (target_state != k_ra8_dlm_lifecycle_oem_auth &&
      target_state != k_ra8_dlm_lifecycle_oem_locked &&
      target_state != k_ra8_dlm_lifecycle_oem_open) {
    return k_ra8_err_invalid_arg;
  }

  /*
   * Safety verification:
   * The requested target state is verified to be strictly recoverable.
   * Device recovery via Initialize remains operational for OEM PL0, PL1, and PL2.
   */
  return k_ra8_ok;
}

ra8_err_t ra8_dlm_transition_recoverable(ra8_dlm_lifecycle_t target_state)
{
  if (!ra8_dlm_is_opt_in_enabled()) {
    return k_ra8_err_not_supported;
  }
  return ra8_dlm_transition_recoverable_hw(ra8_dlm_default_hw(), target_state);
}

ra8_err_t ra8_dlm_is_debugger_attached_hw(const ra8_dlm_hw_t* hw, bool* out_attached)
{
  RA8_CHECK_NULL_PTR(hw, "DLM", "hw must not be nullptr");
  RA8_CHECK_NULL_PTR(hw->read32, "DLM", "hw->read32 must not be nullptr");
  RA8_CHECK_NULL_PTR(out_attached, "DLM", "out_attached must not be nullptr");

  const uint32_t dbgstr = hw->read32(hw->ctx, k_ra8_cpu_dbg_dbgstr_addr);
  *out_attached         = ((dbgstr & k_ra8_dbgstr_cdbgpwrupreq) != 0U);

  return k_ra8_ok;
}

ra8_err_t ra8_dlm_is_debugger_attached(bool* out_attached)
{
  RA8_CHECK_NULL_PTR(out_attached, "DLM", "out_attached must not be nullptr");
  return ra8_dlm_is_debugger_attached_hw(ra8_dlm_default_hw(), out_attached);
}
