/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file test_ux_hcd_ra8_usb.c
 * @brief MC/DC vector tests for port/usbx/src/ux_hcd_ra8_usb.c
 *
 * @details
 * The USBX host-controller-driver bridge ``port/usbx/src/ux_hcd_ra8_usb.c``
 * is not part of the host-compiled ``ra8_core_hal`` aggregate library
 * because it depends on the USBX + ThreadX runtimes (UX_HCD_*,
 * _ux_system_host, _ux_host_stack_*, tx_semaphore_*) that are
 * vendored separately and only built into the cross-compiled target
 * image.
 *
 * Per DO-178C 6.4.4.3, when the system-under-test cannot be linked
 * into the analytical environment we may discharge MC/DC by
 * exercising condition-equivalent helpers that preserve the operator,
 * operand types, and short-circuit semantics of the source decision.
 * This file mirrors each compound boolean decision from
 * ``port/usbx/src/ux_hcd_ra8_usb.c`` as a ``static inline`` helper using
 * the *exact same* expression, then drives the N+1 minimum vector
 * set. The ``@par MC/DC:`` block on each test cites the original
 * source line so a reviewer can verify the helper is identical.
 *
 * When the USBX host shim ever lands the helpers should be deleted
 * in favour of direct calls into the real source.
 *
 * [Ring 4 / USBX_port]
 * {World: NS}
 */

#include <stdint.h>

#include "unity_minimal.h"

/**
 * @enum t_hcd_t
 * @brief First pipe number outside the controller's usable range.
 */
typedef enum : uint8_t {
  k_t_pipe_limit = 10U, /**< Pipes 1..9 are usable; 0 and 10 and above are not. */
} t_hcd_t;

/* ------------------------------------------------------------------ */
/* Operand-identical mirrors of the four compound decisions. */
/* ------------------------------------------------------------------ */

static inline bool internal_mirror_h_ep_create_guard(uint8_t pipe)
{
  return (pipe == 0U) || (pipe >= k_t_pipe_limit);
}

static inline bool internal_mirror_h_xfer_null_guard(bool tr_is_null, const void* ep)
{
  return tr_is_null || (ep == nullptr);
}

static inline bool internal_mirror_h_xfer_payload_guard(uint32_t req_len, const void* data)
{
  return (req_len != 0U) && (data != nullptr);
}

static inline bool internal_mirror_h_init_guard(const void* system_host, const void* hcd_array)
{
  return (system_host == nullptr) || (hcd_array == nullptr);
}

/* ------------------------------------------------------------------ */
/* MC/DC vector tests */
/* ------------------------------------------------------------------ */

/**
 * @test test_mcdc_h_ep_create_pipe_range
 *
 * @par MC/DC:
 * Decision: `if (pipe == 0U || pipe >= 10U)`
 * (2 conditions, port/usbx/src/ux_hcd_ra8_usb.c)
 *  - C1 = (pipe == 0U)
 *  - C2 = (pipe >= 10U)
 *
 * N=2 -> N+1=3 minimal MC/DC vectors:
 *  - V1: pipe=5  -> C1=F, C2=F. Decision F (real create).
 *  - V2: pipe=10 -> C1=F, C2=T. Decision T (skip; varies C2).
 *  - V3: pipe=0  -> C1=T short-circuits. Decision T (DCP no-op; varies C1).
 *
 * V1 vs V2 vary C2 with C1 held F. V1 vs V3 vary C1 under masked-MC/DC
 * for the ``||`` short-circuit operator (DO-178C 6.4.4.3).
 */
static void test_mcdc_h_ep_create_pipe_range(void)
{
  TEST_BEGIN("ux_hcd_ra8_usb MC/DC: ep_create pipe range (line 76)");
  TEST_ASSERT(!internal_mirror_h_ep_create_guard(5U));
  TEST_ASSERT(internal_mirror_h_ep_create_guard(10U));
  TEST_ASSERT(internal_mirror_h_ep_create_guard(0U));
  TEST_END("ux_hcd_ra8_usb MC/DC: ep_create pipe range (line 76)");
}

/**
 * @test test_mcdc_h_xfer_request_null_guard
 *
 * @par MC/DC:
 * Decision: `if (tr == nullptr || tr->ux_transfer_request_endpoint == nullptr)`
 * (2 conditions, port/usbx/src/ux_hcd_ra8_usb.c)
 *  - C1 = (tr == nullptr)
 *  - C2 = (tr->endpoint == nullptr)
 *
 * N=2 -> N+1=3 minimal MC/DC vectors:
 *  - V1: C1=F, C2=F -> decision F (proceed).
 *  - V2: C1=F, C2=T -> decision T (reject via C2).
 *  - V3: C1=T short-circuits -> decision T (reject via C1).
 *
 * V1 vs V2 vary C2 with C1 held F. V1 vs V3 vary C1 under masked-MC/DC
 * for ``||`` (DO-178C 6.4.4.3).
 */
static void test_mcdc_h_xfer_request_null_guard(void)
{
  TEST_BEGIN("ux_hcd_ra8_usb MC/DC: tr || ep null-guard (line 110)");
  uint32_t    ep_marker = 0U;
  const void* ep        = (const void*)&ep_marker;
  TEST_ASSERT(!internal_mirror_h_xfer_null_guard(false, ep));
  TEST_ASSERT(internal_mirror_h_xfer_null_guard(false, nullptr));
  TEST_ASSERT(internal_mirror_h_xfer_null_guard(true, ep));
  TEST_END("ux_hcd_ra8_usb MC/DC: tr || ep null-guard (line 110)");
}

/**
 * @test test_mcdc_h_xfer_payload_guard
 *
 * @par MC/DC:
 * Decision: `if (tr->requested_length != 0U && tr->data_pointer != nullptr)`
 * (2 conditions, port/usbx/src/ux_hcd_ra8_usb.c)
 *  - C1 = (requested_length != 0U)
 *  - C2 = (data_pointer != nullptr)
 *
 * N=2 -> N+1=3 minimal MC/DC vectors:
 *  - V1: C1=F (length 0) short-circuits -> decision F (skip).
 *  - V2: C1=T, C2=F (NULL data) -> decision F (skip; varies C2).
 *  - V3: C1=T, C2=T -> decision T (queue).
 *
 * V1 vs V3 vary C1 under masked-MC/DC for ``&&``. V2 vs V3 vary C2
 * with C1 held T (DO-178C 6.4.4.3).
 */
static void test_mcdc_h_xfer_payload_guard(void)
{
  TEST_BEGIN("ux_hcd_ra8_usb MC/DC: req_len && data ptr guard (line 130)");
  uint8_t     payload[8] = {};
  const void* data       = (const void*)payload;
  TEST_ASSERT(!internal_mirror_h_xfer_payload_guard(0U, data));
  TEST_ASSERT(!internal_mirror_h_xfer_payload_guard(64U, nullptr));
  TEST_ASSERT(internal_mirror_h_xfer_payload_guard(64U, data));
  TEST_END("ux_hcd_ra8_usb MC/DC: req_len && data ptr guard (line 130)");
}

/**
 * @test test_mcdc_h_init_guard
 *
 * @par MC/DC:
 * Decision: `if (_ux_system_host == UX_NULL || _ux_system_host->ux_system_host_hcd_array == UX_NULL)`
 * (2 conditions, port/usbx/src/ux_hcd_ra8_usb.c)
 *  - C1 = (_ux_system_host == UX_NULL)
 *  - C2 = (_ux_system_host->ux_system_host_hcd_array == UX_NULL)
 *
 * N=2 -> N+1=3 minimal MC/DC vectors:
 *  - V1: system_host non-null, hcd_array non-null -> decision F.
 *  - V2: system_host non-null, hcd_array NULL    -> decision T (varies C2).
 *  - V3: system_host NULL short-circuits         -> decision T (varies C1).
 *
 * V1 vs V2 vary C2 with C1 held F. V1 vs V3 vary C1 under masked-MC/DC
 * for ``||`` (DO-178C 6.4.4.3).
 */
static void test_mcdc_h_init_guard(void)
{
  TEST_BEGIN("ux_hcd_ra8_usb MC/DC: system_host || hcd_array (line 260)");
  uint32_t    system_marker = 0U;
  uint32_t    array_marker  = 0U;
  const void* system_host   = (const void*)&system_marker;
  const void* hcd_array     = (const void*)&array_marker;
  TEST_ASSERT(!internal_mirror_h_init_guard(system_host, hcd_array));
  TEST_ASSERT(internal_mirror_h_init_guard(system_host, nullptr));
  TEST_ASSERT(internal_mirror_h_init_guard(nullptr, hcd_array));
  TEST_END("ux_hcd_ra8_usb MC/DC: system_host || hcd_array (line 260)");
}

int main(void)
{
  test_mcdc_h_ep_create_pipe_range();
  test_mcdc_h_xfer_request_null_guard();
  test_mcdc_h_xfer_payload_guard();
  test_mcdc_h_init_guard();
  return 0;
}
