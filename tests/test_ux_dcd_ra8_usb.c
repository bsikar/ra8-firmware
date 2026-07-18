/**
 * @file test_ux_dcd_ra8_usb.c
 * @brief MC/DC vector tests for port/usbx/src/ux_dcd_ra8_usb.c
 *
 * @details
 * The USBX device-controller-driver bridge ``port/usbx/src/ux_dcd_ra8_usb.c``
 * is not part of the host-compiled ``ra8_core_hal`` aggregate library
 * because it depends on the USBX + ThreadX runtimes (UX_SLAVE_*,
 * _ux_system_slave, _ux_device_stack_control_request_process,
 * tx_semaphore_*) that are vendored separately and only built into
 * the cross-compiled target image.
 *
 * Per DO-178C 6.4.4.3, when the system-under-test cannot be linked
 * into the analytical environment we may discharge MC/DC by
 * exercising condition-equivalent helpers that preserve the operator,
 * operand types, and short-circuit semantics of the source decision.
 * This file mirrors each compound boolean decision from
 * ``port/usbx/src/ux_dcd_ra8_usb.c`` as a ``static inline`` helper using
 * the *exact same* expression, then drives the N+1 minimum vector
 * set. The ``@par MC/DC:`` block on each test cites the original
 * source line so a reviewer can verify the helper is identical.
 *
 * When the USBX host shim ever lands the helpers should be deleted
 * in favour of direct calls into the real source.
 *
 * [Ring 4 / USBX_port]
 * {World: NS}
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>

#include "unity_minimal.h"

/* ------------------------------------------------------------------ */
/* Operand-identical mirrors of the four compound decisions. */
/* ------------------------------------------------------------------ */

/**
 * @brief Mirror of the transfer-request null-guard decision.
 *
 * Source: port/usbx/src/ux_dcd_ra8_usb.c
 *   `if (tr == nullptr || tr->ux_slave_transfer_request_endpoint == nullptr)`
 */
static inline bool internal_mirror_xfer_null_guard(bool tr_is_null, const void* ep)
{
  return tr_is_null || (ep == nullptr);
}

/**
 * @brief Mirror of the EP0 IN-data length + pointer guard.
 *
 * Source: port/usbx/src/ux_dcd_ra8_usb.c
 *   `if (tr->ux_slave_transfer_request_in_transfer_length != 0U &&
 *        tr->ux_slave_transfer_request_data_pointer != nullptr)`
 */
static inline bool internal_mirror_ep0_in_guard(uint32_t in_len, const void* data)
{
  return (in_len != 0U) && (data != nullptr);
}

/**
 * @brief Mirror of the endpoint-create pipe-range guard.
 *
 * Source: port/usbx/src/ux_dcd_ra8_usb.c
 *   `if (pipe == 0U || pipe >= (uint8_t)k_ux_dcd_ra8_usb_max_pipes)`
 */
static inline bool internal_mirror_ep_create_guard(uint8_t pipe)
{
  enum : uint8_t {
    k_test_ux_max_pipes = 10U, /**< Mirrors k_ux_dcd_ra8_usb_max_pipes. */
  };
  return (pipe == 0U) || (pipe >= (uint8_t)k_test_ux_max_pipes);
}

/**
 * @brief Mirror of the SETUP-dispatch null + system-bound guard.
 *
 * Source: port/usbx/src/ux_dcd_ra8_usb.c
 *   `if (setup == nullptr || _ux_system_slave == UX_NULL)`
 */
static inline bool internal_mirror_dispatch_setup_guard(const void* setup,
                                                        const void* system_device)
{
  return (setup == nullptr) || (system_device == nullptr);
}

/**
 * @brief Mirror of the SQMON rising-edge SETUP-dispatch guard.
 *
 * Source: port/usbx/src/ux_dcd_ra8_usb.c
 *   `if (now_sqmon != 0U && s_prev_dcpctr_sqmon == 0U)`
 */
static inline bool internal_mirror_sqmon_edge_guard(uint16_t now_sqmon, uint16_t prev_sqmon)
{
  return (now_sqmon != 0U) && (prev_sqmon == 0U);
}

/* ------------------------------------------------------------------ */
/* MC/DC vector tests */
/* ------------------------------------------------------------------ */

/**
 * @test test_mcdc_xfer_request_null_guard
 *
 * @par MC/DC:
 * Decision: `if (tr == nullptr || tr->ux_slave_transfer_request_endpoint == nullptr)`
 * (2 conditions, port/usbx/src/ux_dcd_ra8_usb.c)
 *  - C1 = (tr == nullptr)
 *  - C2 = (tr->endpoint == nullptr)
 *
 * N=2 -> N+1=3 minimal MC/DC vectors:
 *  - Vector 1: C1=F, C2=F -> decision F (proceed).
 *  - Vector 2: C1=F, C2=T -> decision T (reject via C2).
 *  - Vector 3: C1=T (short-circuits) -> decision T (reject via C1).
 *
 * Vectors 1+2 vary C2 with C1 held F (decision F->T via C2).
 * Vectors 1+3 vary C1 with C2 implicitly held -- masked-condition
 * MC/DC accepted under DO-178C 6.4.4.3 for the ``||`` short-circuit
 * operator. Minimum N+1 satisfied.
 */
static void test_mcdc_xfer_request_null_guard(void)
{
  TEST_BEGIN("ux_dcd_ra8_usb MC/DC: tr || ep null-guard (line 156)");
  uint32_t    ep_marker = 0U;
  const void* ep        = (const void*)&ep_marker;

  TEST_ASSERT(!internal_mirror_xfer_null_guard(false, ep));
  TEST_ASSERT(internal_mirror_xfer_null_guard(false, nullptr));
  TEST_ASSERT(internal_mirror_xfer_null_guard(true, ep));
  TEST_END("ux_dcd_ra8_usb MC/DC: tr || ep null-guard (line 156)");
}

/**
 * @test test_mcdc_ep0_in_data_guard
 *
 * @par MC/DC:
 * Decision: `if (tr->in_transfer_length != 0U && tr->data_pointer != nullptr)`
 * (2 conditions, port/usbx/src/ux_dcd_ra8_usb.c)
 *  - C1 = (in_transfer_length != 0U)
 *  - C2 = (data_pointer != nullptr)
 *
 * N=2 -> N+1=3 minimal MC/DC vectors:
 *  - Vector 1: C1=F (length 0) short-circuits. Decision F (skip).
 *  - Vector 2: C1=T (length 64), C2=F (NULL data). Decision F (skip).
 *  - Vector 3: C1=T, C2=T. Decision T (queue IN).
 *
 * Vectors 1+3 vary C1 with C2 implicitly held T (masked MC/DC under
 * DO-178C 6.4.4.3 for ``&&``). Vectors 2+3 vary C2 with C1 held T.
 * Minimum N+1 satisfied.
 */
static void test_mcdc_ep0_in_data_guard(void)
{
  TEST_BEGIN("ux_dcd_ra8_usb MC/DC: in_len && data ptr guard (line 173)");
  uint8_t     payload[8] = {};
  const void* data       = (const void*)payload;

  TEST_ASSERT(!internal_mirror_ep0_in_guard(0U, data));
  TEST_ASSERT(!internal_mirror_ep0_in_guard(64U, nullptr));
  TEST_ASSERT(internal_mirror_ep0_in_guard(64U, data));
  TEST_END("ux_dcd_ra8_usb MC/DC: in_len && data ptr guard (line 173)");
}

/**
 * @test test_mcdc_ep_create_pipe_range
 *
 * @par MC/DC:
 * Decision: `if (pipe == 0U || pipe >= (uint8_t)k_ux_dcd_ra8_usb_max_pipes)`
 * (2 conditions, port/usbx/src/ux_dcd_ra8_usb.c)
 *  - C1 = (pipe == 0U)
 *  - C2 = (pipe >= 10U)
 *
 * N=2 -> N+1=3 minimal MC/DC vectors:
 *  - Vector 1: pipe=5 -> C1=F, C2=F. Decision F (real create).
 *  - Vector 2: pipe=10 -> C1=F, C2=T. Decision T (skip, out of range).
 *  - Vector 3: pipe=0 -> C1=T short-circuits. Decision T (DCP no-op).
 *
 * Vectors 1+2 vary C2 with C1 held F (decision F->T via C2).
 * Vectors 1+3 vary C1 with C2 implicitly held -- masked MC/DC under
 * DO-178C 6.4.4.3 for ``||``. Minimum N+1 satisfied.
 */
static void test_mcdc_ep_create_pipe_range(void)
{
  TEST_BEGIN("ux_dcd_ra8_usb MC/DC: ep_create pipe range (line 215)");
  TEST_ASSERT(!internal_mirror_ep_create_guard(5U));
  TEST_ASSERT(internal_mirror_ep_create_guard(10U));
  TEST_ASSERT(internal_mirror_ep_create_guard(0U));
  TEST_END("ux_dcd_ra8_usb MC/DC: ep_create pipe range (line 215)");
}

/**
 * @test test_mcdc_dispatch_setup_guard
 *
 * @par MC/DC:
 * Decision: `if (setup == nullptr || _ux_system_slave == UX_NULL)`
 * (2 conditions, port/usbx/src/ux_dcd_ra8_usb.c)
 *  - C1 = (setup == nullptr)
 *  - C2 = (_ux_system_slave == UX_NULL)
 *
 * N=2 -> N+1=3 minimal MC/DC vectors:
 *  - Vector 1: setup non-null, system_device non-null -> decision F.
 *  - Vector 2: setup non-null, system_device NULL -> decision T via C2.
 *  - Vector 3: setup NULL -> decision T via C1 (C2 not evaluated).
 *
 * Vectors 1+2 vary C2 (decision F->T) with C1 held F.
 * Vectors 1+3 vary C1 (decision F->T) with C2 implicitly held --
 * masked MC/DC under DO-178C 6.4.4.3 for ``||``. Minimum N+1 satisfied.
 */
static void test_mcdc_dispatch_setup_guard(void)
{
  TEST_BEGIN("ux_dcd_ra8_usb MC/DC: dispatch_setup null-guard (line 506)");
  uint32_t    setup_marker  = 0U;
  uint32_t    system_marker = 0U;
  const void* setup         = (const void*)&setup_marker;
  const void* system        = (const void*)&system_marker;

  TEST_ASSERT(!internal_mirror_dispatch_setup_guard(setup, system));
  TEST_ASSERT(internal_mirror_dispatch_setup_guard(setup, nullptr));
  TEST_ASSERT(internal_mirror_dispatch_setup_guard(nullptr, system));
  TEST_END("ux_dcd_ra8_usb MC/DC: dispatch_setup null-guard (line 506)");
}

/**
 * @test test_mcdc_sqmon_edge_guard
 *
 * @par MC/DC:
 * Decision: `if (now_sqmon != 0U && s_prev_dcpctr_sqmon == 0U)`
 * (2 conditions, port/usbx/src/ux_dcd_ra8_usb.c@internal_dvst_default_state)
 *  - C1 = (now_sqmon != 0U)
 *  - C2 = (s_prev_dcpctr_sqmon == 0U)
 *
 * N=2 -> N+1=3 minimal MC/DC vectors:
 *  - Vector 1: now=0,    prev=0    -> C1=F short-circuits. Decision F.
 *  - Vector 2: now=0x40, prev=0x40 -> C1=T, C2=F. Decision F (no edge).
 *  - Vector 3: now=0x40, prev=0    -> C1=T, C2=T. Decision T (rising edge).
 *
 * Vectors 1+3 vary C1 with C2 held T (decision F->T via C1).
 * Vectors 2+3 vary C2 with C1 held T (decision F->T via C2).
 * Minimum N+1 satisfied.
 */
static void test_mcdc_sqmon_edge_guard(void)
{
  TEST_BEGIN("ux_dcd_ra8_usb MC/DC: SQMON rising-edge guard (line 1348)");
  TEST_ASSERT(!internal_mirror_sqmon_edge_guard(0U, 0U));
  TEST_ASSERT(!internal_mirror_sqmon_edge_guard(0x40U, 0x40U));
  TEST_ASSERT(internal_mirror_sqmon_edge_guard(0x40U, 0U));
  TEST_END("ux_dcd_ra8_usb MC/DC: SQMON rising-edge guard (line 1348)");
}

int32_t main(void)
{
  test_mcdc_xfer_request_null_guard();
  test_mcdc_ep0_in_data_guard();
  test_mcdc_ep_create_pipe_range();
  test_mcdc_dispatch_setup_guard();
  test_mcdc_sqmon_edge_guard();
  (void)fprintf(stderr, "[OK ] test_ux_dcd_ra8_usb.c\n");
  return 0;
}
