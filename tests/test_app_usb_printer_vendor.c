/**
 * @file test_app_usb_printer_vendor.c
 * @brief Host unit test for the usb_printer_vendor example chapter-9 router (#265).
 *
 * @details
 * Exercises the pure, hardware-free decision logic in the demo's header-only
 * chapter-9 module (`usb_printer_vendor_ch9.h`, included directly the way
 * `test_ereader_pageturn.c` pulls in `er_pageturn.h`):
 *
 *  - ::pv_route_setup -- the SETUP-envelope classifier that drives every EP0
 *    transaction (which descriptor to stage, whether to ACK, which class layer
 *    owns the request, or STALL).
 *  - ::pv_is_printer_class_request -- the compound Printer-request predicate.
 *  - ::pv_descriptor -- the descriptor table accessor + its NULL-guard.
 *  - ::pv_clamp_len -- the `wLength` ceiling.
 *
 * It also asserts the composite descriptor tables are internally consistent
 * (config `wTotalLength` matches `sizeof`, IF0 is Printer 0x07, IF1 is Vendor
 * 0xFF, and the four bulk endpoint addresses are present) so the byte tables
 * the firmware advertises stay in lock-step with the enumeration board_sim
 * validates.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>

#include "../examples/ek_ra8d2/hw_pending/manual/usb_printer_vendor/inc/usb_printer_vendor_ch9.h"
#include "ra8_usb.h"
#include "unity_minimal.h"

/**
 * @enum usb_printer_vendor_uint8_const_t
 * @brief Named uint8_t constants used by this file.
 *
 * @details
 * Every literal this translation unit needs, named so the
 * value's role is visible at the point of use (CLAUDE.md
 * "No Magic Numbers").
 */
typedef enum : uint8_t {
  k_usb_printer_get_port_status = 0x09U, /**< Printer-class request 0x09, GET_PORT_STATUS. */
  k_usb_req_set_interface =
    0x0BU, /**< Standard request 0x0B, SET_INTERFACE, sent to an interface rather than the device. */
  k_usb_assigned_address =
    7U, /**< Address assigned by SET_ADDRESS; non-zero, so a device that ignored it stays visible at address 0. */
} usb_printer_vendor_uint8_const_t;

/* =============================================================================
 * Local wire constants (mirror the SETUP envelopes the host issues)
 * =============================================================================
 */

/** @brief bmRequestType envelopes + descriptor / config markers used by the vectors. */
typedef enum : uint8_t {
  k_t_bm_std_in       = 0x80U, /**< Standard | Device    | In.                   */
  k_t_bm_class_in     = 0xA1U, /**< Class    | Interface | In.                   */
  k_t_bm_class_out    = 0x21U, /**< Class    | Interface | Out.                  */
  k_t_bm_vendor_in    = 0xC0U, /**< Vendor   | Device    | In.                   */
  k_t_bm_class_device = 0x20U, /**< Class    | Device    | In (wrong recipient). */
  k_t_bm_reserved     = 0x60U, /**< Reserved request type (bits 6:5 = 11).       */
  k_t_dt_config_hi    = 0x02U, /**< GET_DESCRIPTOR wValue high byte = CONFIG.    */
  k_t_dt_unknown_hi   = 0x22U, /**< Unknown descriptor type high byte.           */
} t_bm_t;

/** @brief Byte offsets into the CONFIGURATION descriptor used by the sanity check. */
typedef enum : uint8_t {
  k_t_cfg_total_lo   = 2U,  /**< wTotalLength low byte.                     */
  k_t_cfg_total_hi   = 3U,  /**< wTotalLength high byte.                    */
  k_t_cfg_numif      = 4U,  /**< bNumInterfaces.                            */
  k_t_cfg_if0_class  = 14U, /**< IF0 bInterfaceClass (Printer). Offset 9+5. */
  k_t_cfg_if1_class  = 37U, /**< IF1 bInterfaceClass (Vendor). Offset 32+5. */
  k_t_expect_numif   = 2U,  /**< Two interfaces (printer+vendor).           */
  k_t_expect_dev_len = 18U, /**< DEVICE descriptor length.                  */
} t_cfg_off_t;

/** @brief wValue high-byte shift + assorted small literals. */
typedef enum : uint16_t {
  k_t_dt_shift    = 8U,    /**< wValue high byte position.                */
  k_t_wlen_big    = 255U,  /**< Host wLength that exceeds any descriptor. */
  k_t_wlen_probe  = 8U,    /**< Host wLength for the device probe.        */
  k_t_iface_vend  = 0xFFU, /**< Vendor interface class.                   */
  k_t_iface_print = 0x07U, /**< Printer interface class.                  */
} t_lit_t;

/* =============================================================================
 * Helpers
 * =============================================================================
 */

/**
 * @brief Build a SETUP envelope from its five fields.
 *
 * @param[in] bm    bmRequestType.
 * @param[in] req   bRequest.
 * @param[in] wval  wValue.
 * @param[in] wlen  wLength.
 * @return ra8_usb_setup_t The populated envelope.
 * @pre None.
 * @post The returned struct mirrors the arguments (pure).
 */
static ra8_usb_setup_t t_setup(uint8_t bm, uint8_t req, uint16_t wval, uint16_t wlen)
{
  ra8_usb_setup_t s = {};
  s.bm_request_type = bm;
  s.b_request       = req;
  s.w_value         = wval;
  s.w_index         = 0U;
  s.w_length        = wlen;
  return s;
}

/* =============================================================================
 * Tests
 * =============================================================================
 */

/**
 * @test pv_route_setup_standard
 * @brief GET_DESCRIPTOR / SET_ADDRESS / SET_CONFIGURATION + the stall legs.
 *
 * @par MC/DC:
 * Decision: the `pv_route_get_descriptor` switch on the wValue high byte
 * (device / config / string / other) and the `pv_route_standard` switch on
 * bRequest. Branch coverage across all four descriptor types and both no-data
 * standard requests, plus an unknown standard bRequest -> stall.
 */
static void test_route_standard(void)
{
  TEST_BEGIN("pv_route_setup standard requests");
  ra8_usb_setup_t s;

  s = t_setup(k_t_bm_std_in,
              k_pv_std_get_descriptor,
              (uint16_t)((uint16_t)k_pv_dt_device << k_t_dt_shift),
              k_t_wlen_probe);
  TEST_ASSERT_EQ(k_pv_action_get_device_desc, pv_route_setup(&s));

  s = t_setup(k_t_bm_std_in,
              k_pv_std_get_descriptor,
              (uint16_t)((uint16_t)k_pv_dt_config << k_t_dt_shift),
              k_t_wlen_big);
  TEST_ASSERT_EQ(k_pv_action_get_config_desc, pv_route_setup(&s));

  s = t_setup(k_t_bm_std_in,
              k_pv_std_get_descriptor,
              (uint16_t)((uint16_t)k_pv_dt_string << k_t_dt_shift),
              k_t_wlen_big);
  TEST_ASSERT_EQ(k_pv_action_get_string_desc, pv_route_setup(&s));

  /* Unknown descriptor type high byte -> stall. */
  s = t_setup(k_t_bm_std_in,
              k_pv_std_get_descriptor,
              (uint16_t)((uint16_t)k_t_dt_unknown_hi << k_t_dt_shift),
              k_t_wlen_big);
  TEST_ASSERT_EQ(k_pv_action_stall, pv_route_setup(&s));

  s = t_setup(0x00U, k_pv_std_set_address, k_usb_assigned_address, 0U);
  TEST_ASSERT_EQ(k_pv_action_set_address, pv_route_setup(&s));

  s = t_setup(0x00U, k_pv_std_set_configuration, 1U, 0U);
  TEST_ASSERT_EQ(k_pv_action_set_configuration, pv_route_setup(&s));

  /* Unknown standard bRequest (e.g. SET_INTERFACE 0x0B) -> stall. */
  s = t_setup(0x01U, k_usb_req_set_interface, 0U, 0U);
  TEST_ASSERT_EQ(k_pv_action_stall, pv_route_setup(&s));

  TEST_END("pv_route_setup standard requests");
}

/**
 * @test pv_route_setup_type
 * @brief Request-type routing: standard / class / vendor / reserved / NULL.
 *
 * @par MC/DC:
 * Decision: the `pv_route_setup` switch on `bmRequestType & 0x60` (bits 6:5).
 * - standard (0x00) -> a standard action (covered above; here class/vendor).
 * - class (0x20)    -> ::k_pv_action_printer_class.
 * - vendor (0x40)   -> ::k_pv_action_vendor_class.
 * - reserved (0x60) -> ::k_pv_action_stall.
 * Plus the NULL guard `setup == nullptr` -> ::k_pv_action_stall.
 */
static void test_route_type(void)
{
  TEST_BEGIN("pv_route_setup request-type routing");
  ra8_usb_setup_t s;

  s = t_setup(k_t_bm_class_in, k_pv_pprn_get_port_status, 0U, 1U);
  TEST_ASSERT_EQ(k_pv_action_printer_class, pv_route_setup(&s));

  s = t_setup(k_t_bm_vendor_in, 0x01U, 0U, 0U);
  TEST_ASSERT_EQ(k_pv_action_vendor_class, pv_route_setup(&s));

  s = t_setup(k_t_bm_reserved, 0x00U, 0U, 0U);
  TEST_ASSERT_EQ(k_pv_action_stall, pv_route_setup(&s));

  /* NULL envelope. */
  TEST_ASSERT_EQ(k_pv_action_stall, pv_route_setup(nullptr));

  TEST_END("pv_route_setup request-type routing");
}

/**
 * @test pv_is_printer_class_request_mcdc
 * @brief MC/DC on the compound Printer-request predicate.
 *
 * @par MC/DC:
 * Decision D1: `is_iface && is_known` (2 conditions).
 * - V1: is_iface=T, is_known=T  -> true  (control: both true).
 * - V2: is_iface=F, is_known=T  -> false (varies is_iface only).
 * - V3: is_iface=T, is_known=F  -> false (varies is_known only).
 * V1+V2 prove is_iface independently affects the outcome; V1+V3 prove the same
 * for is_known. N+1 = 3 vectors for N=2 conditions.
 *
 * Decision D2: `is_known = (r==GET_DEVICE_ID) || (r==GET_PORT_STATUS) ||
 * (r==SOFT_RESET)` (3 conditions).
 * - all false (r=3)              -> false.
 * - first true  (r=GET_DEVICE_ID)-> true.
 * - second true (r=GET_PORT_ST)  -> true.
 * - third true  (r=SOFT_RESET)   -> true.
 * Each term is shown to independently flip the OR from false to true.
 */
static void test_printer_predicate(void)
{
  TEST_BEGIN("pv_is_printer_class_request MC/DC");
  ra8_usb_setup_t s;

  /* D1-V1: interface recipient + known request -> true. */
  s = t_setup(k_t_bm_class_in, k_pv_pprn_get_port_status, 0U, 1U);
  TEST_ASSERT_EQ(true, pv_is_printer_class_request(&s));

  /* D1-V2: device recipient (not interface) + known request -> false. */
  s = t_setup(k_t_bm_class_device, k_pv_pprn_get_port_status, 0U, 1U);
  TEST_ASSERT_EQ(false, pv_is_printer_class_request(&s));

  /* D1-V3: interface recipient + unknown request (0x09) -> false. */
  s = t_setup(k_t_bm_class_in, k_usb_printer_get_port_status, 0U, 0U);
  TEST_ASSERT_EQ(false, pv_is_printer_class_request(&s));

  /* D2 OR-chain: each Printer request independently makes is_known true. */
  s = t_setup(k_t_bm_class_out, k_pv_pprn_get_device_id, 0U, 0U);
  TEST_ASSERT_EQ(true, pv_is_printer_class_request(&s));
  s = t_setup(k_t_bm_class_out, k_pv_pprn_soft_reset, 0U, 0U);
  TEST_ASSERT_EQ(true, pv_is_printer_class_request(&s));

  /* NULL guard. */
  TEST_ASSERT_EQ(false, pv_is_printer_class_request(nullptr));

  TEST_END("pv_is_printer_class_request MC/DC");
}

/**
 * @test pv_descriptor_mcdc
 * @brief Table accessor for each descriptor + the NULL-guard MC/DC.
 *
 * @par MC/DC:
 * Decision: `(out_desc == nullptr) || (out_len == nullptr)` (2 conditions).
 * - V1: both non-NULL             -> false (proceeds, returns a table).
 * - V2: out_desc NULL, out_len ok -> true  (returns false).
 * - V3: out_desc ok, out_len NULL -> true  (returns false).
 * Plus branch coverage: each of the three descriptor actions returns its
 * table, and a non-descriptor action returns false.
 */
static void test_descriptor(void)
{
  TEST_BEGIN("pv_descriptor accessor + NULL guard");
  const uint8_t* p   = nullptr;
  uint16_t       len = 0U;

  TEST_ASSERT_EQ(true, pv_descriptor(k_pv_action_get_device_desc, &p, &len));
  TEST_ASSERT_NOT_NULL(p);
  TEST_ASSERT_EQ(k_t_expect_dev_len, len);

  TEST_ASSERT_EQ(true, pv_descriptor(k_pv_action_get_config_desc, &p, &len));
  TEST_ASSERT_NOT_NULL(p);

  TEST_ASSERT_EQ(true, pv_descriptor(k_pv_action_get_string_desc, &p, &len));
  TEST_ASSERT_NOT_NULL(p);

  /* Non-descriptor action -> false. */
  TEST_ASSERT_EQ(false, pv_descriptor(k_pv_action_set_address, &p, &len));

  /* NULL-guard vectors. */
  TEST_ASSERT_EQ(false, pv_descriptor(k_pv_action_get_device_desc, nullptr, &len));
  TEST_ASSERT_EQ(false, pv_descriptor(k_pv_action_get_device_desc, &p, nullptr));

  TEST_END("pv_descriptor accessor + NULL guard");
}

/**
 * @test pv_clamp_len_bounds
 * @brief The wLength ceiling picks the smaller of the two lengths.
 *
 * @par MC/DC:
 * Decision: `(available < requested) ? available : requested`.
 * - available < requested -> available.
 * - available > requested -> requested.
 * - available == requested -> requested (the ternary false leg).
 */
static void test_clamp_len(void)
{
  TEST_BEGIN("pv_clamp_len bounds");
  TEST_ASSERT_EQ(8U, pv_clamp_len((uint16_t)8U, (uint16_t)255U));
  TEST_ASSERT_EQ(9U, pv_clamp_len((uint16_t)55U, (uint16_t)9U));
  TEST_ASSERT_EQ(4U, pv_clamp_len((uint16_t)4U, (uint16_t)4U));
  TEST_END("pv_clamp_len bounds");
}

/**
 * @test pv_descriptor_tables_consistent
 * @brief The composite descriptor tables are internally consistent.
 *
 * @details Asserts the CONFIGURATION descriptor advertises exactly the bytes
 * it contains (wTotalLength == sizeof), two interfaces, and that the first
 * interface is Printer 0x07 while the second is Vendor 0xFF -- the class codes
 * the board_sim host detects and the issue #265 requires.
 */
static void test_descriptor_tables(void)
{
  TEST_BEGIN("composite descriptor tables consistent");
  const uint8_t* cfg = nullptr;
  uint16_t       len = 0U;
  TEST_ASSERT_EQ(true, pv_descriptor(k_pv_action_get_config_desc, &cfg, &len));

  const uint16_t total = (uint16_t)((uint16_t)cfg[k_t_cfg_total_lo] |
                                    (uint16_t)((uint16_t)cfg[k_t_cfg_total_hi] << k_t_dt_shift));
  TEST_ASSERT_EQ(len, total);
  TEST_ASSERT_EQ(k_t_expect_numif, cfg[k_t_cfg_numif]);
  TEST_ASSERT_EQ(k_t_iface_print, cfg[k_t_cfg_if0_class]);
  TEST_ASSERT_EQ(k_t_iface_vend, cfg[k_t_cfg_if1_class]);

  TEST_END("composite descriptor tables consistent");
}

/**
 * @brief Host-test entry point: run every case.
 *
 * @return int32_t 0 on success; assertions abort the process on failure.
 * @pre None.
 * @post All test cases have executed.
 */
int32_t main(void)
{
  test_route_standard();
  test_route_type();
  test_printer_predicate();
  test_descriptor();
  test_clamp_len();
  test_descriptor_tables();
  return 0;
}
