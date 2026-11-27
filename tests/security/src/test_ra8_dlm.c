/**
 * @file test_ra8_dlm.c
 * @brief Unit tests for Device Lifecycle Management (DLM) and Debug Port Lockdown
 * @details Verifies status queries, debug port lockdown, recoverable transitions,
 *          and error guards over the mock DI register seam.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra8_dlm.h"
#include "ra8_dlm_regs.h"
#include "ra8_err.h"
#include "unity_minimal.h"

typedef struct {
  uint32_t dlmmon;
  uint32_t dbgauth0;
  uint32_t dbgstr;
  uint32_t alctrl;
  bool     write_fault;
} mock_dlm_ctx_t;

static uint32_t mock_read32(void* ctx, uintptr_t addr)
{
  const mock_dlm_ctx_t* m = (const mock_dlm_ctx_t*)ctx;
  if (addr == k_ra8_dlmmon_addr) {
    return m->dlmmon;
  }
  if (addr == k_ra8_cpu_dbg_dbgauth0_addr) {
    return m->dbgauth0;
  }
  if (addr == k_ra8_cpu_dbg_dbgstr_addr) {
    return m->dbgstr;
  }
  if (addr == k_ra8_cpu_dbg_alctrl_addr) {
    return m->alctrl;
  }
  return 0U;
}

static void mock_write32(void* ctx, uintptr_t addr, uint32_t val)
{
  mock_dlm_ctx_t* m = (mock_dlm_ctx_t*)ctx;
  if (addr == k_ra8_cpu_dbg_dbgauth0_addr) {
    if (!m->write_fault) {
      m->dbgauth0 = val;
    }
  }
}

static void test_dlm_null_guards(void)
{
  TEST_BEGIN("null guards reject invalid pointers");

  ra8_dlm_status_t status = {};
  bool attached           = false;

  const ra8_dlm_hw_t null_fn_hw = {
    .read32  = nullptr,
    .write32 = nullptr,
    .ctx     = nullptr,
  };

  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_dlm_get_status_hw(nullptr, &status));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_dlm_get_status_hw(&null_fn_hw, &status));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_dlm_get_status(nullptr));

  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_dlm_lockdown_debug_ports_hw(nullptr));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_dlm_lockdown_debug_ports_hw(&null_fn_hw));

  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_dlm_transition_recoverable_hw(nullptr, k_ra8_dlm_lifecycle_oem_auth));

  mock_dlm_ctx_t ctx = {};
  const ra8_dlm_hw_t valid_hw = {
    .read32  = mock_read32,
    .write32 = mock_write32,
    .ctx     = &ctx,
  };

  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_dlm_get_status_hw(&valid_hw, nullptr));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_dlm_is_debugger_attached_hw(nullptr, &attached));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_dlm_is_debugger_attached_hw(&null_fn_hw, &attached));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_dlm_is_debugger_attached_hw(&valid_hw, nullptr));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_dlm_is_debugger_attached(nullptr));

  TEST_END("null guards reject invalid pointers");
}

static void test_dlm_status_decoding(void)
{
  TEST_BEGIN("status queries decode register bitfields accurately");

  mock_dlm_ctx_t ctx = {
    .dlmmon      = 0x00000002U, /* OEM PL1 auth */
    .dbgauth0    = k_ra8_dbgauth0_dbgen0 | k_ra8_dbgauth0_deviceen,
    .dbgstr      = k_ra8_dbgstr_cdbgpwrupreq,
    .alctrl      = k_ra8_alctrl_al1 | (2UL << k_ra8_alctrl_failcnt_shift),
    .write_fault = false,
  };

  const ra8_dlm_hw_t hw = {
    .read32  = mock_read32,
    .write32 = mock_write32,
    .ctx     = &ctx,
  };

  ra8_dlm_status_t status = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_dlm_get_status_hw(&hw, &status));

  TEST_ASSERT_EQ(k_ra8_dlm_lifecycle_oem_auth, status.lifecycle);
  TEST_ASSERT_EQ(k_ra8_dlm_al1_auth, status.auth_level);
  TEST_ASSERT_EQ(2U, status.fail_count);
  TEST_ASSERT(status.debugger_attached);
  TEST_ASSERT(status.debug_ports_open);
  TEST_ASSERT_EQ(0x00000002U, status.raw_dlmmon);

  /* Test OEM Open (PL2) decoding */
  ctx.dlmmon = 0x00000001U;
  ctx.alctrl = k_ra8_alctrl_al2;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_dlm_get_status_hw(&hw, &status));
  TEST_ASSERT_EQ(k_ra8_dlm_lifecycle_oem_open, status.lifecycle);
  TEST_ASSERT_EQ(k_ra8_dlm_al2_auth, status.auth_level);

  /* Test OEM Locked (PL0) decoding */
  ctx.dlmmon   = 0x00000003U;
  ctx.alctrl   = k_ra8_alctrl_al0;
  ctx.dbgstr   = 0U;
  ctx.dbgauth0 = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_dlm_get_status_hw(&hw, &status));
  TEST_ASSERT_EQ(k_ra8_dlm_lifecycle_oem_locked, status.lifecycle);
  TEST_ASSERT_EQ(k_ra8_dlm_al0_locked, status.auth_level);
  TEST_ASSERT(!status.debugger_attached);
  TEST_ASSERT(!status.debug_ports_open);

  /* Test OEM Default / SSD decoding */
  ctx.dlmmon = 0x00000004U;
  ctx.alctrl = 0x12U; /* Unknown AL */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_dlm_get_status_hw(&hw, &status));
  TEST_ASSERT_EQ(k_ra8_dlm_lifecycle_oem_default, status.lifecycle);
  TEST_ASSERT_EQ(k_ra8_dlm_al_other, status.auth_level);

  /* Test unknown lifecycle decoding */
  ctx.dlmmon = 0x00000009U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_dlm_get_status_hw(&hw, &status));
  TEST_ASSERT_EQ(k_ra8_dlm_lifecycle_unknown, status.lifecycle);

  TEST_END("status queries decode register bitfields accurately");
}

static void test_dlm_debugger_attached_query(void)
{
  TEST_BEGIN("debugger attachment detection");

  mock_dlm_ctx_t ctx = {
    .dbgstr = k_ra8_dbgstr_cdbgpwrupreq,
  };
  const ra8_dlm_hw_t hw = {
    .read32  = mock_read32,
    .write32 = mock_write32,
    .ctx     = &ctx,
  };

  bool attached = false;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_dlm_is_debugger_attached_hw(&hw, &attached));
  TEST_ASSERT(attached);

  ctx.dbgstr = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_dlm_is_debugger_attached_hw(&hw, &attached));
  TEST_ASSERT(!attached);

  TEST_END("debugger attachment detection");
}

static void test_dlm_lockdown_debug_ports(void)
{
  TEST_BEGIN("debug ports lockdown operation");

  mock_dlm_ctx_t ctx = {
    .dbgauth0    = k_ra8_dbgauth0_all_enables,
    .write_fault = false,
  };
  const ra8_dlm_hw_t hw = {
    .read32  = mock_read32,
    .write32 = mock_write32,
    .ctx     = &ctx,
  };

  if (!ra8_dlm_is_opt_in_enabled()) {
    /* When opt-in is not enabled, lockdown must fail closed without writing */
    TEST_ASSERT_EQ(k_ra8_err_not_supported, ra8_dlm_lockdown_debug_ports_hw(&hw));
    TEST_ASSERT_EQ(k_ra8_err_not_supported, ra8_dlm_lockdown_debug_ports());
    TEST_ASSERT_EQ(k_ra8_dbgauth0_all_enables, ctx.dbgauth0);
  } else {
    /* When opt-in is enabled, lockdown clears DBGAUTH0 */
    TEST_ASSERT_EQ(k_ra8_ok, ra8_dlm_lockdown_debug_ports_hw(&hw));
    TEST_ASSERT_EQ(0U, ctx.dbgauth0);

    /* Hardware fault readback simulation */
    ctx.dbgauth0    = k_ra8_dbgauth0_dbgen0;
    ctx.write_fault = true;
    TEST_ASSERT_EQ(k_ra8_fail, ra8_dlm_lockdown_debug_ports_hw(&hw));
  }

  TEST_END("debug ports lockdown operation");
}

static void test_dlm_transition_recoverable(void)
{
  TEST_BEGIN("recoverable transition validation and safety checks");

  mock_dlm_ctx_t ctx = {};
  const ra8_dlm_hw_t hw = {
    .read32  = mock_read32,
    .write32 = mock_write32,
    .ctx     = &ctx,
  };

  if (!ra8_dlm_is_opt_in_enabled()) {
    TEST_ASSERT_EQ(k_ra8_err_not_supported,
                   ra8_dlm_transition_recoverable_hw(&hw, k_ra8_dlm_lifecycle_oem_auth));
    TEST_ASSERT_EQ(k_ra8_err_not_supported,
                   ra8_dlm_transition_recoverable(k_ra8_dlm_lifecycle_oem_auth));
  } else {
    /* Permitted recoverable transitions */
    TEST_ASSERT_EQ(k_ra8_ok,
                   ra8_dlm_transition_recoverable_hw(&hw, k_ra8_dlm_lifecycle_oem_auth));
    TEST_ASSERT_EQ(k_ra8_ok,
                   ra8_dlm_transition_recoverable_hw(&hw, k_ra8_dlm_lifecycle_oem_locked));
    TEST_ASSERT_EQ(k_ra8_ok,
                   ra8_dlm_transition_recoverable_hw(&hw, k_ra8_dlm_lifecycle_oem_open));

    /* Forbidden unrecoverable or invalid states must be rejected with invalid_arg */
    TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                   ra8_dlm_transition_recoverable_hw(&hw, k_ra8_dlm_lifecycle_unknown));
    TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                   ra8_dlm_transition_recoverable_hw(&hw, k_ra8_dlm_lifecycle_oem_default));
    TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                   ra8_dlm_transition_recoverable_hw(&hw, (ra8_dlm_lifecycle_t)0x05U));
    TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                   ra8_dlm_transition_recoverable_hw(&hw, (ra8_dlm_lifecycle_t)0xFFU));
  }

  TEST_END("recoverable transition validation and safety checks");
}

static void test_dlm_default_hw_interface(void)
{
  TEST_BEGIN("default hardware accessor validation");

  const ra8_dlm_hw_t* hw = ra8_dlm_default_hw();
  TEST_ASSERT(hw != nullptr);
  TEST_ASSERT(hw->read32 != nullptr);
  TEST_ASSERT(hw->write32 != nullptr);

  TEST_END("default hardware accessor validation");
}

int main(void)
{
  test_dlm_null_guards();
  test_dlm_status_decoding();
  test_dlm_debugger_attached_query();
  test_dlm_lockdown_debug_ports();
  test_dlm_transition_recoverable();
  test_dlm_default_hw_interface();
  return 0;
}
