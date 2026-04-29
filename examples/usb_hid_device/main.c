/**
 * @file examples/usb_hid_device/main.c
 * @brief USB HID boot-protocol mouse smoke test for EK-RA8D2 (USB-FS)
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * Brings the chip up via ``ra_cgc_init()`` (XTAL -> PLL1 -> CPUCLK0 =
 * 1 GHz, PCLKA = 125 MHz), routes the four USB-FS pins per the
 * EK-RA8D2 v1 User's Manual ("USB Full-Speed Connector" pinout table)
 * to the on-board USB-FS receptacle, then brings the device-mode HID
 * stack up via ``ra_usb_phid``. Once the host enumerates the device
 * the firmware drops into a 1 Hz loop that pushes a 4-pixel cursor
 * jiggle (right, down, left, up) on the interrupt-IN pipe. LED1
 * toggles per send so the bench operator can see traffic on the wire.
 *
 * The HID Report Descriptor is the canonical 3-button + X/Y boot-
 * protocol mouse described in USB HID 1.11 sec E.10 "Sample Report
 * Descriptor (Mouse)". Reports are 3 bytes:
 *
 *   - byte 0: button bitmap (B1=left, B2=right, B3=middle).
 *   - byte 1: signed X delta (-127 .. +127).
 *   - byte 2: signed Y delta (-127 .. +127).
 *
 * The HID class descriptor (HID 1.11 sec 6.2.1) is hand-rolled in
 * ``s_hid_descriptor`` to advertise the report-descriptor length so
 * GET_DESCRIPTOR(HID) returns a coherent reply during enumeration.
 *
 * ## Pinout (USB-FS, FSP-aligned)
 *
 * | Net           | Pin    | PFS PSEL                | Notes                         |
 * |---------------|--------|-------------------------|-------------------------------|
 * | USB_FS_VBUS   | P4_07  | k_ra_psel_usb_fs (0x13) | VBUS sense (input).           |
 * | USB_FS_VBUSEN | P5_00  | k_ra_psel_usb_fs (0x13) | VBUS-enable drive (output).   |
 * | USB_FS_DP     | P8_14  | k_ra_psel_usb_fs (0x13) | D+ data line (analog buffer). |
 * | USB_FS_DM     | P8_15  | k_ra_psel_usb_fs (0x13) | D- data line (analog buffer). |
 *
 * ## Verification
 *
 *   - Linux: ``lsusb`` shows ``Brighton Sikarskie EK-RA8D2 HID Mouse``;
 *     ``xinput list`` adds a "pointer" device; the cursor jiggles in a
 *     small square on the desktop, LED1 toggles at 1 Hz.
 *   - macOS: ``system_profiler SPUSBDataType`` lists the device under
 *     "USB Bus" with class HID. The cursor moves on screen the same
 *     way; no driver install needed (the OS uses its built-in boot-
 *     mouse class driver).
 *
 * @par Architectural ring
 * [Ring 6 / APP] {World: S} -- application-layer code that runs in
 * the Secure world.
 *
 * @author Brighton Sikarskie
 * @date 2026-04-29
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>

#include "ra_cgc.h"
#include "ra_err.h"
#include "ra_gpio_constants.h"
#include "ra_isr.h"
#include "ra_nsc_comms.h"
#include "ra_port_constants.h"
#include "ra_port_utils.h"
#include "ra_time.h"
#include "ra_usb.h"
#include "ra_usb_phid.h"

/* =============================================================================
 * USB-FS pinout (mirrors usb_cdc_echo)
 * =============================================================================
 */

/**
 * @brief USB-FS pin identifiers, packed ``ra_port_pin_t`` (port << 8 | pin).
 *
 * @details Built as a runtime cast so clang-tidy's enum-range check
 * is happy with the otherwise out-of-enum value.
 */
static const ra_port_pin_t k_usb_hid_pin_vbus =
  (ra_port_pin_t)(((uint16_t)k_ra_port_4 << 8) | (uint16_t)k_ra_pin_7);
static const ra_port_pin_t k_usb_hid_pin_vbusen =
  (ra_port_pin_t)(((uint16_t)k_ra_port_5 << 8) | (uint16_t)k_ra_pin_0);
static const ra_port_pin_t k_usb_hid_pin_dp =
  (ra_port_pin_t)(((uint16_t)k_ra_port_8 << 8) | (uint16_t)k_ra_pin_14);
static const ra_port_pin_t k_usb_hid_pin_dm =
  (ra_port_pin_t)(((uint16_t)k_ra_port_8 << 8) | (uint16_t)k_ra_pin_15);

/* =============================================================================
 * Tunables
 * =============================================================================
 */

/**
 * @enum usb_hid_report_t
 * @brief Sizing for the boot-protocol mouse input report.
 *
 * @details Per USB HID 1.11 sec E.10 the boot-mouse input report is
 * 3 bytes wide: { buttons, dx, dy }.
 */
typedef enum : uint8_t {
  k_usb_hid_report_bytes = 3U, /**< Boot mouse report width. */
  k_usb_hid_idx_buttons  = 0U, /**< Byte offset for buttons. */
  k_usb_hid_idx_dx       = 1U, /**< Byte offset for X delta. */
  k_usb_hid_idx_dy       = 2U, /**< Byte offset for Y delta. */
} usb_hid_report_t;

/**
 * @enum usb_hid_jiggle_t
 * @brief Per-step cursor delta values for the autonomous jiggle.
 *
 * @details A 4-pixel right / down / left / up square so the cursor
 * drifts in place. ``int8_t`` because the report field is signed
 * 8-bit per HID Usage Tables sec 4 "Generic Desktop Usage Page".
 */
typedef enum : int8_t {
  k_usb_hid_jiggle_step = 4,  /**< Magnitude of each jiggle step.  */
  k_usb_hid_jiggle_zero = 0,  /**< No motion on the inactive axis. */
  k_usb_hid_jiggle_neg  = -4, /**< Reverse of ``k_step``.          */
} usb_hid_jiggle_t;

/**
 * @enum usb_hid_phase_t
 * @brief Index into the four-step jiggle pattern.
 */
typedef enum : uint8_t {
  k_usb_hid_phase_right = 0U, /**< +X, 0Y. */
  k_usb_hid_phase_down  = 1U, /**< 0X, +Y. */
  k_usb_hid_phase_left  = 2U, /**< -X, 0Y. */
  k_usb_hid_phase_up    = 3U, /**< 0X, -Y. */
  k_usb_hid_phase_count = 4U, /**< Modulus. */
} usb_hid_phase_t;

/**
 * @enum usb_hid_timing_t
 * @brief Loop timing.
 */
typedef enum : uint16_t {
  k_usb_hid_send_period_ms = 1000U, /**< 1 second between jiggle sends. */
} usb_hid_timing_t;

/**
 * @enum usb_hid_report_id_t
 * @brief HID report-ID space.
 *
 * @details Boot-protocol mouse uses a single unnamed report (no
 * leading report ID byte), per USB HID 1.11 sec 7.2.5
 * "GetProtocol/SetProtocol" notes for the boot subclass.
 */
typedef enum : uint8_t {
  k_usb_hid_report_id_none = 0U, /**< Single unnamed report. */
} usb_hid_report_id_t;

/* =============================================================================
 * HID Report Descriptor (3-button + X/Y boot mouse)
 * =============================================================================
 *
 * Built byte-for-byte from USB HID 1.11 sec E.10 "Sample Report
 * Descriptor (Mouse)". Each row is annotated with the prefix-byte +
 * decoded item per HID 1.11 sec 6.2.2 "Report Descriptor".
 */

/**
 * @var s_report_descriptor
 * @brief HID Report Descriptor for the boot mouse.
 *
 * @details Decoded sequence:
 *   05 01           Usage Page (Generic Desktop)
 *   09 02           Usage      (Mouse)
 *   A1 01           Collection (Application)
 *     09 01           Usage      (Pointer)
 *     A1 00           Collection (Physical)
 *       05 09           Usage Page (Buttons)
 *       19 01           Usage Min  (Button 1)
 *       29 03           Usage Max  (Button 3)
 *       15 00           Logical Min (0)
 *       25 01           Logical Max (1)
 *       95 03           Report Count (3)
 *       75 01           Report Size  (1 bit)
 *       81 02           Input (Data, Var, Abs)        -- 3 button bits
 *       95 01           Report Count (1)
 *       75 05           Report Size  (5 bits)
 *       81 03           Input (Cnst, Var, Abs)        -- 5 padding bits
 *       05 01           Usage Page (Generic Desktop)
 *       09 30           Usage      (X)
 *       09 31           Usage      (Y)
 *       15 81           Logical Min (-127)
 *       25 7F           Logical Max ( 127)
 *       75 08           Report Size  (8 bits)
 *       95 02           Report Count (2)
 *       81 06           Input (Data, Var, Rel)        -- dx, dy
 *     C0              End Collection (Physical)
 *   C0              End Collection (Application)
 *
 * @note Pure 7-bit ASCII; bytes are hex literals, not multi-byte
 *       Unicode.
 * @since 0.1.0
 */
static const uint8_t s_report_descriptor[] = {
  0x05U, 0x01U, /* Usage Page (Generic Desktop)        */
  0x09U, 0x02U, /* Usage (Mouse)                       */
  0xA1U, 0x01U, /* Collection (Application)            */
  0x09U, 0x01U, /*   Usage (Pointer)                   */
  0xA1U, 0x00U, /*   Collection (Physical)             */
  0x05U, 0x09U, /*     Usage Page (Buttons)            */
  0x19U, 0x01U, /*     Usage Min (1)                   */
  0x29U, 0x03U, /*     Usage Max (3)                   */
  0x15U, 0x00U, /*     Logical Min (0)                 */
  0x25U, 0x01U, /*     Logical Max (1)                 */
  0x95U, 0x03U, /*     Report Count (3)                */
  0x75U, 0x01U, /*     Report Size  (1)                */
  0x81U, 0x02U, /*     Input (Data,Var,Abs)            */
  0x95U, 0x01U, /*     Report Count (1)                */
  0x75U, 0x05U, /*     Report Size  (5)                */
  0x81U, 0x03U, /*     Input (Cnst,Var,Abs) padding    */
  0x05U, 0x01U, /*     Usage Page (Generic Desktop)    */
  0x09U, 0x30U, /*     Usage (X)                       */
  0x09U, 0x31U, /*     Usage (Y)                       */
  0x15U, 0x81U, /*     Logical Min (-127)              */
  0x25U, 0x7FU, /*     Logical Max ( 127)              */
  0x75U, 0x08U, /*     Report Size  (8)                */
  0x95U, 0x02U, /*     Report Count (2)                */
  0x81U, 0x06U, /*     Input (Data,Var,Rel)            */
  0xC0U,        /*   End Collection (Physical)         */
  0xC0U,        /* End Collection (Application)        */
};

/**
 * @enum usb_hid_desc_layout_t
 * @brief Layout of the 9-byte HID class descriptor (HID 1.11 sec 6.2.1).
 */
typedef enum : uint8_t {
  k_usb_hid_class_desc_len      = 9U,    /**< bLength.                  */
  k_usb_hid_class_desc_type     = 0x21U, /**< bDescriptorType (HID).    */
  k_usb_hid_class_desc_bcd_lo   = 0x11U, /**< bcdHID low (1.11).        */
  k_usb_hid_class_desc_bcd_hi   = 0x01U, /**< bcdHID high (1.11).       */
  k_usb_hid_class_desc_country  = 0x00U, /**< bCountryCode (none).      */
  k_usb_hid_class_desc_num_desc = 1U,    /**< bNumDescriptors.          */
  k_usb_hid_class_desc_rep_type = 0x22U, /**< bDescriptorType (Report). */
} usb_hid_desc_layout_t;

/**
 * @var s_hid_descriptor
 * @brief HID class descriptor advertised in GET_DESCRIPTOR(HID).
 *
 * @details Layout per USB HID 1.11 sec 6.2.1 "HID Descriptor". The
 * report-descriptor length field is little-endian.
 */
static const uint8_t s_hid_descriptor[] = {
  k_usb_hid_class_desc_len,
  k_usb_hid_class_desc_type,
  k_usb_hid_class_desc_bcd_lo,
  k_usb_hid_class_desc_bcd_hi,
  k_usb_hid_class_desc_country,
  k_usb_hid_class_desc_num_desc,
  k_usb_hid_class_desc_rep_type,
  (uint8_t)(sizeof(s_report_descriptor) & 0xFFU),
  (uint8_t)((sizeof(s_report_descriptor) >> 8U) & 0xFFU),
};

/* =============================================================================
 * Helpers
 * =============================================================================
 */

/**
 * @brief Halt forever in WFI -- panic stop on init failure.
 *
 * @pre Called only after a fatal error during boot.
 *
 * @post CPU parked; only debugger / external reset wakes it.
 *
 * @since 0.1.0
 */
static void usb_hid_panic_halt(void)
{
  while (1) {
    __asm__ volatile("wfi");
  }
}

/**
 * @brief Route the four USB-FS pins (VBUS, VBUSEN, D+, D-) to the
 *        USBFS controller via the PFS PSEL field.
 *
 * @return Error code.
 * @retval k_ra_ok All four pins routed.
 * @retval other   PFS routing failure.
 *
 * @pre IOPORT module reachable.
 * @pre Single-threaded init context.
 *
 * @post On success the four USB-FS pins are in USB peripheral mode.
 *
 * @since 0.1.0
 */
[[nodiscard]] static ra_err_t usb_hid_pins_init(void)
{
  ra_err_t err = ra_pfs_route_peripheral(k_usb_hid_pin_vbus, k_ra_psel_usb_fs, "usb_hid.vbus");
  if (err != k_ra_ok) {
    return err;
  }
  err = ra_pfs_route_peripheral(k_usb_hid_pin_vbusen, k_ra_psel_usb_fs, "usb_hid.vbusen");
  if (err != k_ra_ok) {
    return err;
  }
  err = ra_pfs_route_peripheral(k_usb_hid_pin_dp, k_ra_psel_usb_fs, "usb_hid.dp");
  if (err != k_ra_ok) {
    return err;
  }
  return ra_pfs_route_peripheral(k_usb_hid_pin_dm, k_ra_psel_usb_fs, "usb_hid.dm");
}

/**
 * @brief Bring up CGC, SysTick, USB pin mux, LED1, USB controller, and
 *        the device-HID class. Panic-halts on any failure.
 *
 * @details
 * Mirrors the structure of usb_cdc_echo's setup helper but installs
 * ``ra_usb_phid_*`` instead of ``ra_usb_cdc_*``. After
 * ``ra_usb_phid_set_descriptors`` the D+ pull-up is raised by
 * ``ra_nsc_usb_attach`` so the host begins enumeration.
 *
 * @since 0.1.0
 */
static void usb_hid_setup_or_halt(void)
{
  uint32_t cpuclk0_hz = 0U;

  if (ra_cgc_init() != k_ra_ok) {
    usb_hid_panic_halt();
  }
  if (ra_cgc_get_clock_hz(k_ra_clock_id_cpuclk0, &cpuclk0_hz) != k_ra_ok) {
    usb_hid_panic_halt();
  }
  if (ra_time_init(cpuclk0_hz) != k_ra_ok) {
    usb_hid_panic_halt();
  }
  if (ra_gpio_output_init(k_ra_pin_led1, k_ra_level_low) != k_ra_ok) {
    usb_hid_panic_halt();
  }
  if (usb_hid_pins_init() != k_ra_ok) {
    usb_hid_panic_halt();
  }

  /* USB controller bring-up via the NSC veneer -- releases USBFS MSTP
   * gate, drives SCKE high, clears DRPD, sets USBE, programs the DCP
   * max-packet to 64, and unmasks the device-mode IRQ set. */
  if (ra_nsc_usb_init(k_ra_usb_speed_fs) != k_ra_ok) {
    usb_hid_panic_halt();
  }

  /* HID class layer: configures PIPE6 (intr IN, EP1 IN) and PIPE7
   * (intr OUT, EP2 OUT) at the FS default packet size, primes idle
   * rate to 0 (report-on-change) and protocol to "report". */
  if (ra_usb_phid_init(k_ra_usb_speed_fs) != k_ra_ok) {
    usb_hid_panic_halt();
  }

  if (ra_usb_phid_set_descriptors(s_report_descriptor,
                                  (uint16_t)sizeof(s_report_descriptor),
                                  s_hid_descriptor,
                                  (uint16_t)sizeof(s_hid_descriptor)) != k_ra_ok) {
    usb_hid_panic_halt();
  }

  /* Raise D+ pull-up so the host begins SET_ADDRESS / GET_DESCRIPTOR
   * / SET_CONFIGURATION. VID/PID/strings come from the secure-side
   * standard-request handler (same VID 0x1209, locally chosen PID). */
  if (ra_nsc_usb_attach(k_ra_usb_speed_fs, true) != k_ra_ok) {
    usb_hid_panic_halt();
  }
}

/**
 * @brief Build the boot-mouse report for one phase of the jiggle.
 *
 * @details No buttons pressed; only X/Y deltas vary by phase.
 *
 * @param[in]  phase Position in the 4-step square pattern.
 * @param[out] report Destination buffer, must hold ``k_report_bytes``.
 *
 * @pre ``report`` is non-NULL.
 * @pre ``phase < k_usb_hid_phase_count``.
 *
 * @post All three report bytes are written.
 *
 * @since 0.1.0
 */
static void usb_hid_build_jiggle(usb_hid_phase_t phase, uint8_t* report)
{
  int8_t dx = k_usb_hid_jiggle_zero;
  int8_t dy = k_usb_hid_jiggle_zero;
  switch (phase) {
    case k_usb_hid_phase_right:
      dx = k_usb_hid_jiggle_step;
      break;
    case k_usb_hid_phase_down:
      dy = k_usb_hid_jiggle_step;
      break;
    case k_usb_hid_phase_left:
      dx = k_usb_hid_jiggle_neg;
      break;
    case k_usb_hid_phase_up:
      dy = k_usb_hid_jiggle_neg;
      break;
    case k_usb_hid_phase_count:
    default:
      break;
  }

  report[k_usb_hid_idx_buttons] = 0U;
  report[k_usb_hid_idx_dx]      = (uint8_t)dx;
  report[k_usb_hid_idx_dy]      = (uint8_t)dy;
}

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmain"
/**
 * @brief Application entry. Brings up CGC + USB-FS + HID, then jiggles
 *        the host cursor in a 4-pixel square at 1 Hz forever.
 *
 * @return Never returns.
 *
 * @pre Reset_Handler has copied .data and zeroed .bss.
 * @pre SystemInit has set VTOR, FPU, and priority grouping.
 *
 * @post On clean entry the CPU stays in the send-jiggle loop forever.
 * @post On any HAL init failure the function halts in WFI.
 *
 * @since 0.1.0
 */
int32_t main(void)
{
  usb_hid_setup_or_halt();
  ra_isr_globals_enable();

  uint8_t         report[k_usb_hid_report_bytes] = {};
  usb_hid_phase_t phase                          = k_usb_hid_phase_right;

  while (1) {
    usb_hid_build_jiggle(phase, report);

    if (ra_usb_phid_send_report(k_usb_hid_report_id_none,
                                report,
                                (uint16_t)k_usb_hid_report_bytes) != k_ra_ok) {
      break;
    }

    if (ra_gpio_toggle(k_ra_pin_led1) != k_ra_ok) {
      break;
    }

    phase = (usb_hid_phase_t)(((uint8_t)(phase + 1U)) % k_usb_hid_phase_count);
    ra_delay_ms((uint32_t)k_usb_hid_send_period_ms);
  }

  usb_hid_panic_halt();
  return 0;
}
#pragma GCC diagnostic pop
