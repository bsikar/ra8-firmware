/**
 * @file test_ra8_board_ek_ra8d2_usb_port.c
 * @brief Host tests for the USB port role facade (RA8FW-305)
 *
 * @details
 * ``ra8_board_usb_port_init`` replaces the eight-step pin choreography
 * nineteen example apps wrote out longhand. Three things about it are
 * worth pinning down from the host, because all three are board facts a
 * future refactor could quietly get wrong:
 *
 *   - The three USB-FS bus pins (P4_07 VBUS sense, P8_14 D+, P8_15 D-)
 *     reach the ``k_ra8_psel_usb_fs`` peripheral function. Asserted by
 *     reading the PSEL field straight out of each pin's PmnPFS register.
 *   - P5_00 VBUSEN does **not**. It has to stay a GPIO output, because
 *     routing it to the peripheral function makes the controller drive it
 *     as host VBUSEN and device enumeration never completes. Asserted by
 *     checking its PSEL field is still zero while its PmnPFS direction bit
 *     says output, and that the output latch follows the role: LOW for
 *     device, HIGH for host.
 *   - An out-of-range port or role is refused rather than defaulted to
 *     something plausible.
 *
 * The high-speed arms are not exercised here. They delegate to
 * ``ra8_board_usbhs_device_init`` / ``ra8_board_usbhs_host_init``, which
 * ``test_ra8_board_ek_ra8d2_audio_usb_cov.c`` already covers, and which
 * reach the CGC and MSTP blocks; re-asserting their behaviour through the
 * facade would test those helpers a second time, not the switch.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>

#include "ra8_board_ek_ra8d2.h"
#include "ra8_err.h"
#include "ra8_fake_mmap.h"
#include "ra8_gpio_constants.h"
#include "ra8_pfs_regs.h"
#include "ra8_pin_validator.h"
#include "ra8_port_constants.h"
#include "ra8_port_utils.h"
#include "unity_minimal.h"

/** @brief Sentinels for the port-facade cases. */
typedef enum : uint16_t {
  k_test_usbport_bad_port = 7U,  /**< Not an ra8_board_usb_port_t enumerator. */
  k_test_usbport_bad_role = 9U,  /**< Not an ra8_board_usb_role_t enumerator. */
  k_test_usbport_psel_off = 24U, /**< PmnPFS PSEL field starts at bit 24.     */
} test_usbport_sentinel_t;

/**
 * @brief Reset fake register windows and pin ownership between cases.
 *
 * @details Zeroes every hardware register window and frees all claimed
 * pins, so a pin one case claims cannot fail the next as a conflict.
 *
 * @pre The host fake-MMIO and pin-validation backends are available.
 * @post Register windows cleared; pin-validator bitmap zeroed.
 * @note Not thread-safe; single-threaded test context only.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_reset_state(void)
{
  ra8_fake_mmap_reset();
  ra8_pin_validator_reset();
}

/**
 * @brief Read the PSEL field of one pin's PmnPFS register.
 *
 * @param[in] pin Packed port/pin pair.
 * @return uint32_t The 5-bit peripheral-select field.
 *
 * @pre The fake PFS window is mapped.
 * @post No register is written.
 * @note Not thread-safe; single-threaded test context only.
 * @since 0.1.0
 */
RA8_INTERNAL static uint32_t internal_psel_of(ra8_port_pin_t pin)
{
  const volatile uint32_t* pfs = ra8_pfs_pmn(RA8_PIN_PORT(pin), RA8_PIN_PIN(pin));
  return (*pfs & (uint32_t)k_ra8_pfs_mask_psel) >> (uint32_t)k_test_usbport_psel_off;
}

/**
 * @brief Read the output-latch level of one pin from PmnPFS.PODR.
 *
 * @param[in] pin Packed port/pin pair.
 * @return bool True when the latch is driving the pin high.
 *
 * @pre The fake PFS window is mapped.
 * @post No register is written.
 * @note Not thread-safe; single-threaded test context only.
 * @since 0.1.0
 */
RA8_INTERNAL static bool internal_latch_high(ra8_port_pin_t pin)
{
  const volatile uint32_t* pfs = ra8_pfs_pmn(RA8_PIN_PORT(pin), RA8_PIN_PIN(pin));
  return (*pfs & (uint32_t)k_ra8_pfs_mask_podr) != 0U;
}

/**
 * @brief Read the direction bit of one pin from PmnPFS.PDR.
 *
 * @param[in] pin Packed port/pin pair.
 * @return bool True when the pin is configured as an output.
 *
 * @pre The fake PFS window is mapped.
 * @post No register is written.
 * @note Not thread-safe; single-threaded test context only.
 * @since 0.1.0
 */
RA8_INTERNAL static bool internal_is_output(ra8_port_pin_t pin)
{
  const volatile uint32_t* pfs = ra8_pfs_pmn(RA8_PIN_PORT(pin), RA8_PIN_PIN(pin));
  return (*pfs & (uint32_t)k_ra8_pfs_mask_pdr) != 0U;
}

/**
 * @brief The full-speed device arm routes three pins and straps VBUSEN low.
 *
 * @par MC/DC:
 * Supplies the taken vector for the ``port == fs`` decision and the
 * not-taken vector for the ``role == host`` ternary inside the strap
 * helper; the host case below supplies the other.
 *
 * @pre Clean fake state.
 * @post The three bus pins carry PSEL = USBFS and VBUSEN is a low output.
 * @note Not thread-safe; single-threaded test context.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_fs_device(void)
{
  TEST_BEGIN("usb_port_init(fs, device) routes the bus pins and straps VBUSEN low");
  internal_reset_state();

  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_board_usb_port_init(k_ra8_board_usb_port_fs, k_ra8_board_usb_role_device));

  TEST_ASSERT_EQ((uint32_t)k_ra8_psel_usb_fs,
                 internal_psel_of((ra8_port_pin_t)k_ra8_board_usbfs_pin_vbus));
  TEST_ASSERT_EQ((uint32_t)k_ra8_psel_usb_fs,
                 internal_psel_of((ra8_port_pin_t)k_ra8_board_usbfs_pin_dp));
  TEST_ASSERT_EQ((uint32_t)k_ra8_psel_usb_fs,
                 internal_psel_of((ra8_port_pin_t)k_ra8_board_usbfs_pin_dm));

  /* The whole point of the helper: VBUSEN stays a GPIO. */
  TEST_ASSERT_EQ(0U, internal_psel_of((ra8_port_pin_t)k_ra8_board_usbfs_pin_vbusen));
  TEST_ASSERT_EQ(true, internal_is_output((ra8_port_pin_t)k_ra8_board_usbfs_pin_vbusen));
  TEST_ASSERT_EQ(false, internal_latch_high((ra8_port_pin_t)k_ra8_board_usbfs_pin_vbusen));

  TEST_END("usb_port_init(fs, device) routes the bus pins and straps VBUSEN low");
}

/**
 * @brief The full-speed host arm drives VBUSEN high and routes the same pins.
 *
 * @par MC/DC:
 * Supplies the taken vector for the ``role == host`` ternary in the strap
 * helper; the device case above supplies the not-taken vector.
 *
 * @pre Clean fake state.
 * @post VBUSEN is a high output; the bus pins carry PSEL = USBFS.
 * @note Not thread-safe; single-threaded test context.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_fs_host(void)
{
  TEST_BEGIN("usb_port_init(fs, host) drives VBUSEN high");
  internal_reset_state();

  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_board_usb_port_init(k_ra8_board_usb_port_fs, k_ra8_board_usb_role_host));

  TEST_ASSERT_EQ((uint32_t)k_ra8_psel_usb_fs,
                 internal_psel_of((ra8_port_pin_t)k_ra8_board_usbfs_pin_dp));
  TEST_ASSERT_EQ(true, internal_is_output((ra8_port_pin_t)k_ra8_board_usbfs_pin_vbusen));
  TEST_ASSERT_EQ(true, internal_latch_high((ra8_port_pin_t)k_ra8_board_usbfs_pin_vbusen));

  TEST_END("usb_port_init(fs, host) drives VBUSEN high");
}

/**
 * @brief A port or role outside the enumerator set is refused.
 *
 * @par MC/DC:
 * Supplies the taken vector for the role guard and, with a valid role and
 * an unknown port, the fall-through past both port branches.
 *
 * @pre Clean fake state.
 * @post Nothing is routed; both calls return invalid_arg.
 * @note Not thread-safe; single-threaded test context.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_refusals(void)
{
  TEST_BEGIN("usb_port_init refuses a port or role it does not define");
  internal_reset_state();

  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_board_usb_port_init(k_ra8_board_usb_port_fs,
                                         (ra8_board_usb_role_t)k_test_usbport_bad_role));
  /* The role guard runs first, so nothing was routed. */
  TEST_ASSERT_EQ(0U, internal_psel_of((ra8_port_pin_t)k_ra8_board_usbfs_pin_dp));

  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_board_usb_port_init((ra8_board_usb_port_t)k_test_usbport_bad_port,
                                         k_ra8_board_usb_role_device));
  TEST_ASSERT_EQ(0U, internal_psel_of((ra8_port_pin_t)k_ra8_board_usbfs_pin_dp));

  TEST_END("usb_port_init refuses a port or role it does not define");
}

/**
 * @brief A pin another driver already owns propagates as a conflict.
 *
 * @par MC/DC:
 * Supplies the taken vector for the first ``err != k_ra8_ok`` guard in the
 * routing helper, which the two success cases leave not-taken.
 *
 * @pre Clean fake state, then P4_07 claimed by a different owner.
 * @post The call reports the conflict rather than routing over the owner.
 * @note Not thread-safe; single-threaded test context.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_conflict_propagates(void)
{
  TEST_BEGIN("usb_port_init propagates a pin already owned by another driver");
  internal_reset_state();

  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_gpio_output_init((ra8_port_pin_t)k_ra8_board_usbfs_pin_vbus, k_ra8_level_low));

  const ra8_err_t err =
    ra8_board_usb_port_init(k_ra8_board_usb_port_fs, k_ra8_board_usb_role_device);
  TEST_ASSERT_EQ(k_ra8_err_gpio_conflict, err);

  TEST_END("usb_port_init propagates a pin already owned by another driver");
}

int main(void)
{
  internal_test_fs_device();
  internal_test_fs_host();
  internal_test_refusals();
  internal_test_conflict_propagates();
  return 0;
}
