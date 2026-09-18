/**
 * @file test_ra8_usb_pal_event_map.c
 * @brief Unit tests for the ra8_usb_pal INTSTS0 -> event-bit translation
 * @details Drives ::priv_usb_pal_translate_event with composed INTSTS0
 * snapshots so every published ra8_usb_pal_event_t bit has an asserted
 * producer.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>

#include "ra8_attributes.h"
#include "ra8_usb_pal.h"
#include "ra8_usb_pal_internal.h"
#include "ra8_usb_regs.h"
#include "unity_minimal.h"

/**
 * @enum t_evt_bit_t
 * @brief INTSTS0 edge bits as whole words, named at the point of use.
 *
 * @details
 * ``ra8_usb_intenb0_bit_t`` names bit POSITIONS; every vector below
 * wants the shifted word, so each position is shifted once here
 * rather than at twenty call sites.
 */
typedef enum : uint16_t {
  k_t_brdy = (uint16_t)(1U << (uint8_t)k_ra8_int0_bit_brdy), /**< Buffer ready.    */
  k_t_nrdy = (uint16_t)(1U << (uint8_t)k_ra8_int0_bit_nrdy), /**< Buffer not ready.*/
  k_t_bemp = (uint16_t)(1U << (uint8_t)k_ra8_int0_bit_bemp), /**< Buffer empty.    */
  k_t_ctrt = (uint16_t)(1U << (uint8_t)k_ra8_int0_bit_ctrt), /**< Control stage.   */
  k_t_dvst = (uint16_t)(1U << (uint8_t)k_ra8_int0_bit_dvst), /**< Device state.    */
  k_t_sofr = (uint16_t)(1U << (uint8_t)k_ra8_int0_bit_sofr), /**< Start of frame.  */
  k_t_rsme = (uint16_t)(1U << (uint8_t)k_ra8_int0_bit_rsme), /**< Resume.          */
  k_t_vbse = (uint16_t)(1U << (uint8_t)k_ra8_int0_bit_vbse), /**< VBUS change.     */
} t_evt_bit_t;

/**
 * @test internal_test_bus_bits_have_producers
 * @brief Assert each bus-event bit is produced by its own source bit.
 *
 * @details
 * One vector per arm of the mapping, each carrying exactly the source
 * bit under test so a mis-ordered arm cannot pass on a neighbour's
 * output. SOFR is asserted first because it is the bit that made the
 * previous collapse-to-error behaviour visible: it fires every 1 ms
 * on FS, so reporting it as a controller error meant a healthy bus
 * looked permanently broken.
 *
 * @pre None; the translation is pure and needs no fixture.
 * @pre No MMIO mapping is required.
 * @post Every expected event mask has been asserted.
 * @post No state is left behind for the next vector.
 * @note Assertions terminate the hosted test on the first mismatch.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_bus_bits_have_producers(void)
{
  TEST_BEGIN("usb_pal translate: bus bits have producers");
  TEST_ASSERT_EQ((uint16_t)k_ra8_usb_pal_event_sof,
                 priv_usb_pal_translate_event((uint16_t)k_t_sofr));
  TEST_ASSERT_EQ((uint16_t)k_ra8_usb_pal_event_resume,
                 priv_usb_pal_translate_event((uint16_t)k_t_rsme));
  TEST_ASSERT_EQ((uint16_t)k_ra8_usb_pal_event_ep_in,
                 priv_usb_pal_translate_event((uint16_t)k_t_bemp));
  TEST_ASSERT_EQ((uint16_t)k_ra8_usb_pal_event_ep_out,
                 priv_usb_pal_translate_event((uint16_t)k_t_brdy));
  TEST_ASSERT_EQ((uint16_t)k_ra8_usb_pal_event_error,
                 priv_usb_pal_translate_event((uint16_t)k_t_nrdy));
  /* A zero snapshot must stay none: internal_usb_event drops none, so
   * a stray bit here would turn every event-less dispatch into a
   * callback. */
  TEST_ASSERT_EQ((uint16_t)k_ra8_usb_pal_event_none, priv_usb_pal_translate_event(0U));
  TEST_END("usb_pal translate: bus bits have producers");
}

/**
 * @test internal_test_vbus_direction
 * @brief Assert VBSE splits into attach / detach on the VBSTS level.
 *
 * @details
 * VBSE is an edge and carries no direction; VBSTS is the VBUS input
 * level sampled with it. Both vectors carry the same edge bit and
 * differ only in VBSTS, so an arm that ignored the level would fail
 * one of them.
 *
 * @pre None; the translation is pure and needs no fixture.
 * @pre No MMIO mapping is required.
 * @post Both VBUS directions have been asserted.
 * @post No state is left behind for the next vector.
 * @note Assertions terminate the hosted test on the first mismatch.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_vbus_direction(void)
{
  TEST_BEGIN("usb_pal translate: VBSE attach vs detach");
  const uint16_t cable_in = (uint16_t)((uint16_t)k_t_vbse | (uint16_t)k_ra8_intsts0_mask_vbsts);
  TEST_ASSERT_EQ((uint16_t)k_ra8_usb_pal_event_attach, priv_usb_pal_translate_event(cable_in));
  TEST_ASSERT_EQ((uint16_t)k_ra8_usb_pal_event_detach,
                 priv_usb_pal_translate_event((uint16_t)k_t_vbse));
  TEST_END("usb_pal translate: VBSE attach vs detach");
}

/**
 * @test internal_test_device_state_transitions
 * @brief Assert DVST maps through DVSQ, including the unnamed states.
 *
 * @details
 * Mirrors ``internal_dvst_map_dvsq_to_ux_state``
 * (port/usbx/src/ux_dcd_ra8_usb_dvst.c): the suspend flag wins over
 * the three-bit state, Default is the post-bus-reset state, and
 * Address / Configured / Powered have no bit in the taxonomy. The
 * suspend-with-Configured vector is the one that catches an arm
 * written as a plain switch on DVSQ, which would report a reset or
 * nothing at all while the bus is actually suspended.
 *
 * @pre None; the translation is pure and needs no fixture.
 * @pre No MMIO mapping is required.
 * @post Every DVSQ value has an asserted translation.
 * @post No state is left behind for the next vector.
 * @note Assertions terminate the hosted test on the first mismatch.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_device_state_transitions(void)
{
  TEST_BEGIN("usb_pal translate: DVST device states");
  const uint16_t dvst = (uint16_t)k_t_dvst;
  TEST_ASSERT_EQ(
    (uint16_t)k_ra8_usb_pal_event_reset,
    priv_usb_pal_translate_event((uint16_t)(dvst | (uint16_t)k_ra8_dvsq_default)));
  TEST_ASSERT_EQ(
    (uint16_t)k_ra8_usb_pal_event_suspend,
    priv_usb_pal_translate_event((uint16_t)(dvst | (uint16_t)k_ra8_dvsq_suspend)));
  /* Suspend rides alongside the sub-state; the flag must still win. */
  const uint16_t susp_cfg =
    (uint16_t)(dvst | (uint16_t)k_ra8_dvsq_suspend | (uint16_t)k_ra8_dvsq_configured);
  TEST_ASSERT_EQ((uint16_t)k_ra8_usb_pal_event_suspend, priv_usb_pal_translate_event(susp_cfg));
  /* Named by INTSTS0, not by the taxonomy: no bit, so no callback. */
  TEST_ASSERT_EQ(
    (uint16_t)k_ra8_usb_pal_event_none,
    priv_usb_pal_translate_event((uint16_t)(dvst | (uint16_t)k_ra8_dvsq_address)));
  TEST_ASSERT_EQ(
    (uint16_t)k_ra8_usb_pal_event_none,
    priv_usb_pal_translate_event((uint16_t)(dvst | (uint16_t)k_ra8_dvsq_configured)));
  TEST_ASSERT_EQ(
    (uint16_t)k_ra8_usb_pal_event_none,
    priv_usb_pal_translate_event((uint16_t)(dvst | (uint16_t)k_ra8_dvsq_powered)));
  /* DVSQ without the DVST edge is a level, not a transition. */
  TEST_ASSERT_EQ((uint16_t)k_ra8_usb_pal_event_none,
                 priv_usb_pal_translate_event((uint16_t)k_ra8_dvsq_default));
  TEST_END("usb_pal translate: DVST device states");
}

/**
 * @test internal_test_control_stage
 * @brief Assert CTRT reports SETUP only while VALID is latched.
 *
 * @details
 * The SETUP readers clear VALID once they drain the request
 * registers (libs/ra8_hal/src/ra8_usb_xfer.c), so a stage transition
 * with VALID already gone is a data or status step and not a new
 * SETUP. CTSQ = SQER is the hardware's sequence-error report and is
 * the one control-path condition the taxonomy calls an error; the
 * last vector pins that a SETUP arriving on a sequence-errored pipe
 * reports both bits rather than one hiding the other.
 *
 * @pre None; the translation is pure and needs no fixture.
 * @pre No MMIO mapping is required.
 * @post Every control-stage arm has an asserted translation.
 * @post No state is left behind for the next vector.
 * @note Assertions terminate the hosted test on the first mismatch.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_control_stage(void)
{
  TEST_BEGIN("usb_pal translate: CTRT setup + sequence error");
  const uint16_t ctrt  = (uint16_t)k_t_ctrt;
  const uint16_t valid = (uint16_t)k_ra8_intsts0_mask_valid;
  TEST_ASSERT_EQ((uint16_t)k_ra8_usb_pal_event_setup,
                 priv_usb_pal_translate_event((uint16_t)(ctrt | valid)));
  TEST_ASSERT_EQ((uint16_t)k_ra8_usb_pal_event_none, priv_usb_pal_translate_event(ctrt));
  /* VALID without the transition is not a SETUP event either. */
  TEST_ASSERT_EQ((uint16_t)k_ra8_usb_pal_event_none, priv_usb_pal_translate_event(valid));
  TEST_ASSERT_EQ(
    (uint16_t)k_ra8_usb_pal_event_error,
    priv_usb_pal_translate_event((uint16_t)(ctrt | (uint16_t)k_ra8_ctsq_sqer)));
  const uint16_t both = (uint16_t)(ctrt | valid | (uint16_t)k_ra8_ctsq_sqer);
  TEST_ASSERT_EQ(
    (uint16_t)((uint16_t)k_ra8_usb_pal_event_setup | (uint16_t)k_ra8_usb_pal_event_error),
    priv_usb_pal_translate_event(both));
  /* A benign data stage (CTSQ = control read data) is not an error. */
  TEST_ASSERT_EQ(
    (uint16_t)k_ra8_usb_pal_event_none,
    priv_usb_pal_translate_event((uint16_t)(ctrt | (uint16_t)k_ra8_ctsq_rdds)));
  TEST_END("usb_pal translate: CTRT setup + sequence error");
}

/**
 * @test internal_test_composed_snapshot
 * @brief Assert one snapshot carrying several events raises every bit.
 *
 * @details
 * ``ra8_usb_dispatch`` snapshots INTSTS0 once per interrupt, so a
 * tick can hold a SOF, a bus reset and a drained IN buffer together.
 * The composed vector is what a collapse-to-single-event translation
 * would fail: it asserts the exact OR, so an arm that returned early
 * loses a bit and an arm that over-reports gains one. The full-mask
 * vector then pins the widest snapshot the controller can present,
 * where the suspend flag and VBSTS are both set.
 *
 * @pre None; the translation is pure and needs no fixture.
 * @pre No MMIO mapping is required.
 * @post Both composed masks have been asserted exactly.
 * @post No state is left behind for the next vector.
 * @note Assertions terminate the hosted test on the first mismatch.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_composed_snapshot(void)
{
  TEST_BEGIN("usb_pal translate: composed snapshot");
  const uint16_t tick =
    (uint16_t)((uint16_t)k_t_sofr | (uint16_t)k_t_dvst | (uint16_t)k_ra8_dvsq_default |
               (uint16_t)k_t_bemp);
  const uint16_t want = (uint16_t)((uint16_t)k_ra8_usb_pal_event_sof |
                                   (uint16_t)k_ra8_usb_pal_event_reset |
                                   (uint16_t)k_ra8_usb_pal_event_ep_in);
  TEST_ASSERT_EQ(want, priv_usb_pal_translate_event(tick));

  const uint16_t all_bits =
    (uint16_t)((uint16_t)k_t_brdy | (uint16_t)k_t_nrdy | (uint16_t)k_t_bemp |
               (uint16_t)k_t_ctrt | (uint16_t)k_t_dvst | (uint16_t)k_t_sofr |
               (uint16_t)k_t_rsme | (uint16_t)k_t_vbse | (uint16_t)k_ra8_intsts0_mask_valid |
               (uint16_t)k_ra8_intsts0_mask_vbsts | (uint16_t)k_ra8_dvsq_suspend);
  const uint16_t want_all =
    (uint16_t)((uint16_t)k_ra8_usb_pal_event_sof | (uint16_t)k_ra8_usb_pal_event_resume |
               (uint16_t)k_ra8_usb_pal_event_attach | (uint16_t)k_ra8_usb_pal_event_suspend |
               (uint16_t)k_ra8_usb_pal_event_setup | (uint16_t)k_ra8_usb_pal_event_ep_out |
               (uint16_t)k_ra8_usb_pal_event_ep_in | (uint16_t)k_ra8_usb_pal_event_error);
  TEST_ASSERT_EQ(want_all, priv_usb_pal_translate_event(all_bits));
  TEST_END("usb_pal translate: composed snapshot");
}

/**
 * @test internal_test_every_published_bit_is_reachable
 * @brief Assert no published event bit is left without a producer.
 *
 * @details
 * The regression this file exists for: before #1204 eight of the ten
 * ``k_ra8_usb_pal_event_*`` bits had no producer anywhere in the
 * tree. This vector ORs the translation of every single-source
 * snapshot and asserts the union covers each published bit, so a
 * future edit that drops an arm fails here even if it keeps every
 * per-arm vector above passing.
 *
 * @pre None; the translation is pure and needs no fixture.
 * @pre No MMIO mapping is required.
 * @post The union of all arms has been asserted against the taxonomy.
 * @post No state is left behind for the next vector.
 * @note Assertions terminate the hosted test on the first mismatch.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_every_published_bit_is_reachable(void)
{
  TEST_BEGIN("usb_pal translate: every published bit reachable");
  const uint16_t snapshots[] = {
    (uint16_t)((uint16_t)k_t_dvst | (uint16_t)k_ra8_dvsq_default),
    (uint16_t)((uint16_t)k_t_dvst | (uint16_t)k_ra8_dvsq_suspend),
    (uint16_t)k_t_rsme,
    (uint16_t)((uint16_t)k_t_ctrt | (uint16_t)k_ra8_intsts0_mask_valid),
    (uint16_t)k_t_bemp,
    (uint16_t)k_t_brdy,
    (uint16_t)k_t_sofr,
    (uint16_t)((uint16_t)k_t_vbse | (uint16_t)k_ra8_intsts0_mask_vbsts),
    (uint16_t)k_t_vbse,
    (uint16_t)k_t_nrdy,
  };
  uint16_t union_mask = (uint16_t)k_ra8_usb_pal_event_none;
  for (uint16_t i = 0U; i < (uint16_t)(sizeof(snapshots) / sizeof(snapshots[0])); ++i) {
    union_mask = (uint16_t)(union_mask | priv_usb_pal_translate_event(snapshots[i]));
  }
  const uint16_t published =
    (uint16_t)((uint16_t)k_ra8_usb_pal_event_reset | (uint16_t)k_ra8_usb_pal_event_suspend |
               (uint16_t)k_ra8_usb_pal_event_resume | (uint16_t)k_ra8_usb_pal_event_setup |
               (uint16_t)k_ra8_usb_pal_event_ep_in | (uint16_t)k_ra8_usb_pal_event_ep_out |
               (uint16_t)k_ra8_usb_pal_event_sof | (uint16_t)k_ra8_usb_pal_event_attach |
               (uint16_t)k_ra8_usb_pal_event_detach | (uint16_t)k_ra8_usb_pal_event_error);
  TEST_ASSERT_EQ(published, union_mask);
  TEST_END("usb_pal translate: every published bit reachable");
}

int main(void)
{
  internal_test_bus_bits_have_producers();
  internal_test_vbus_direction();
  internal_test_device_state_transitions();
  internal_test_control_stage();
  internal_test_composed_snapshot();
  internal_test_every_published_bit_is_reachable();
  return 0;
}
