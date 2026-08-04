/**
 * @file examples/ek_ra8d2/hw_validated/manual/usb_hid_device/main.c
 * @brief ThreadX + USBX HID boot-mouse demo for EK-RA8D2 (USB-FS)
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * Brings the chip up via ``ra8_cgc_init()`` (XTAL -> PLL1 -> CPUCLK0 =
 * 1 GHz, PCLKA = 125 MHz), routes the four USB-FS pins per the
 * EK-RA8D2 v1 User's Manual to the on-board USB-FS receptacle, hands
 * control to ThreadX, and brings the HID class up through Eclipse
 * USBX's class layer (``ux_device_class_hid_initialize``). Same
 * hardware test as the previous bare-metal version of this app, but
 * driving USBX's class abstraction (which sits on top of
 * ``port/usbx/ux_dcd_ra8_usb`` -> ``ra8_usb`` -- HUM Ch. 36) instead of
 * the hand-rolled ``ra8_usb_phid`` layer. The host actually enumerates
 * the device because USBX's chapter-9 state machine answers SETUP
 * packets through the DCD bridge.
 *
 * Once enumerated, the worker thread pushes a 4-pixel cursor jiggle
 * (right, down, left, up) to the host every second on the interrupt-IN
 * pipe via ``_ux_device_class_hid_event_set``. LED1 toggles per send.
 *
 * The HID Report Descriptor is the canonical 3-button + X/Y boot-
 * protocol mouse described in USB HID 1.11 sec E.10. Reports are
 * 3 bytes:
 *
 *   - byte 0: button bitmap (B1=left, B2=right, B3=middle).
 *   - byte 1: signed X delta (-127 .. +127).
 *   - byte 2: signed Y delta (-127 .. +127).
 *
 * ## Pinout (USB-FS, FSP-aligned)
 *
 * | Net           | Pin    | PFS PSEL                |
 * |---------------|--------|-------------------------|
 * | USB_FS_VBUS   | P4_07  | k_ra8_psel_usb_fs (0x13) |
 * | USB_FS_VBUSEN | P5_00  | k_ra8_psel_usb_fs (0x13) |
 * | USB_FS_DP     | P8_14  | k_ra8_psel_usb_fs (0x13) |
 * | USB_FS_DM     | P8_15  | k_ra8_psel_usb_fs (0x13) |
 *
 * ## Sequence
 *
 *   1. ``ra8_cgc_init()`` -- standard FSP-quickstart clock tree.
 *   2. ``ra8_time_init`` for back-off delays.
 *   3. ``ra8_pfs_route_peripheral`` for the four USB-FS pins.
 *   4. ``ra8_board_led_init(k_ra8_board_led1)`` for visual heartbeat.
 *   5. ThreadX ``tx_kernel_enter()`` -- spins the scheduler.
 *   6. ``tx_application_define`` -- spawns one worker thread that:
 *        - Allocates USBX memory pool and calls
 *          ``_ux_system_initialize`` + ``_ux_device_stack_initialize``.
 *        - Calls ``_ux_device_stack_class_register`` for the HID class.
 *        - Calls ``ux_dcd_ra8_usb_initialize(k_ra8_usb_speed_fs)`` to
 *          plug our DCD bridge into USBX.
 *        - Calls ``ra8_usb_device_attach(true)`` so the host begins
 *          enumeration.
 *        - Drops into the jiggle loop:
 *          ``_ux_device_class_hid_event_set`` once per second, LED1
 *          toggle per send.
 *
 * ## Verification (macOS)
 *
 * After flashing, the EK-RA8D2's USB-FS receptacle (J11) enumerates
 * as an HID Mouse. ``system_profiler SPUSBDataType`` lists the device
 * under "USB Bus" with class HID. The cursor moves on screen in a
 * 4-pixel square; no driver install needed (the OS uses its built-in
 * boot-mouse class driver).
 *
 * @author Brighton Sikarskie
 * @date 2026-05-02
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>
#include <string.h>

#include "ra8_board_ek_ra8d2.h"
#include "ra8_cgc.h"
#include "ra8_err.h"
#include "ra8_gpio_constants.h"
#include "ra8_isr.h"
#include "ra8_port_constants.h"
#include "ra8_port_utils.h"
#include "ra8_time.h"
#include "ra8_usb.h"

#ifndef RA8_OFF_TARGET
#include "tx_api.h"
#include "ux_api.h"
#include "ux_dcd_ra8_usb.h"
#include "ux_device_class_hid.h"
#include "ux_device_stack.h"

/* SysTick handler lives in libs/ra8_core/src/ra8_time.c -- the project's
 * shared weak SysTick_Handler dispatches into ThreadX (via a weak
 * extern to `_tx_timer_interrupt`) AND re-arms the USB storm-guard
 * NVIC line (via a weak extern to `ux_dcd_ra8_usb_irq_reenable`), so
 * no per-app override is needed. Closes Issue #8. */
#endif

/* -------------------------------------------------------------------------- */
/* Pinout (FSP-aligned, EK-RA8D2 v1 User's Manual) */
/* -------------------------------------------------------------------------- */

/**
 * @brief USB-FS pin identifiers, packed ``ra8_port_pin_t`` (port << 8 | pin).
 *
 * @details Built as a runtime cast so clang-tidy's enum-range check
 * is happy with the otherwise out-of-enum value.
 * @since 0.1.0
 */
static const ra8_port_pin_t k_demo_pin_vbus   = (ra8_port_pin_t)k_ra8_board_usbfs_pin_vbus;
static const ra8_port_pin_t k_demo_pin_vbusen = (ra8_port_pin_t)k_ra8_board_usbfs_pin_vbusen;
static const ra8_port_pin_t k_demo_pin_dp     = (ra8_port_pin_t)k_ra8_board_usbfs_pin_dp;
static const ra8_port_pin_t k_demo_pin_dm     = (ra8_port_pin_t)k_ra8_board_usbfs_pin_dm;

/* -------------------------------------------------------------------------- */
/* Tunables */
/* -------------------------------------------------------------------------- */

/**
 * @enum demo_config_t
 * @brief Compile-time settings for the worker thread + USBX pool.
 */
typedef enum : uint32_t {
  k_demo_thread_stack        = 4096U,  /**< Worker thread stack (bytes).                 */
  k_demo_usbx_pool_bytes     = 32768U, /**< USBX pool: 32 KiB; HID enum exhausts 16 KiB. */
  k_demo_jiggle_period_ticks = 100U,   /**< ThreadX ticks between sends.                 */
} demo_config_t;

/**
 * @enum demo_hid_report_t
 * @brief Sizing for the boot-protocol mouse input report.
 *
 * @details Per USB HID 1.11 sec E.10 the boot-mouse input report is
 * 3 bytes wide: { buttons, dx, dy }.
 */
typedef enum : uint8_t {
  k_demo_hid_report_bytes = 3U, /**< Boot mouse report width. */
  k_demo_hid_idx_buttons  = 0U, /**< Byte offset for buttons. */
  k_demo_hid_idx_dx       = 1U, /**< Byte offset for X delta. */
  k_demo_hid_idx_dy       = 2U, /**< Byte offset for Y delta. */
} demo_hid_report_t;

/**
 * @enum demo_jiggle_t
 * @brief Per-step cursor delta values for the autonomous jiggle.
 *
 * @details A 4-pixel right / down / left / up square so the cursor
 * drifts in place. ``int8_t`` because the report field is signed
 * 8-bit per HID Usage Tables sec 4 "Generic Desktop Usage Page".
 */
typedef enum : int8_t {
  k_demo_jiggle_step = 4,  /**< Magnitude of each jiggle step.  */
  k_demo_jiggle_zero = 0,  /**< No motion on the inactive axis. */
  k_demo_jiggle_neg  = -4, /**< Reverse of ``k_step``.          */
} demo_jiggle_t;

/**
 * @enum demo_phase_t
 * @brief Index into the four-step jiggle pattern.
 */
typedef enum : uint8_t {
  k_demo_phase_right = 0U, /**< +X, 0Y.  */
  k_demo_phase_down  = 1U, /**< 0X, +Y.  */
  k_demo_phase_left  = 2U, /**< -X, 0Y.  */
  k_demo_phase_up    = 3U, /**< 0X, -Y.  */
  k_demo_phase_count = 4U, /**< Modulus. */
} demo_phase_t;

#ifndef RA8_OFF_TARGET

/* -------------------------------------------------------------------------- */
/* ThreadX worker + USBX pool storage */
/* -------------------------------------------------------------------------- */

/**
 * @var s_demo_thread
 * @brief ThreadX TCB for the USBX worker thread.
 * @details Owned by ``tx_application_define``; one thread services
 *          USBX init + the jiggle loop.
 * @note Single-writer (worker only); readers must not mutate.
 * @since 0.1.0
 */
static TX_THREAD s_demo_thread;

/**
 * @var s_demo_stack
 * @brief Stack backing storage for ``s_demo_thread``.
 * @since 0.1.0
 */
static UCHAR s_demo_stack[k_demo_thread_stack];

/**
 * @var s_usbx_pool
 * @brief USBX memory pool (USBX uses ``tx_byte_pool`` internally).
 * @since 0.1.0
 */
static UCHAR s_usbx_pool[k_demo_usbx_pool_bytes];

/**
 * @var s_hid_class
 * @brief Active HID class instance, captured by the activate callback.
 * @note Read by worker thread; written by USBX class thread.
 * @since 0.1.0
 */
static UX_SLAVE_CLASS_HID* s_hid_class = UX_NULL;

/**
 * @var g_usb_hid_match
 * @brief HIL liveness counter -- incremented on every successful
 *        ``_ux_device_class_hid_event_set`` (a HID report queued to
 *        the host).
 * @details Read externally via SWD by scripts/hil/jlink_memprobe.sh.
 *          If the host has enumerated the device and the worker is
 *          pumping reports, this advances at the jiggle-period
 *          cadence (~50 Hz). If USBX bring-up failed or the host
 *          isn't attached, it stays at 0.
 * @note Read externally only.
 * @since 0.1.0
 */
volatile uint32_t g_usb_hid_match = 0U;

/**
 * @var g_usb_hid_mismatch
 * @brief HIL failure counter -- incremented when
 *        ``_ux_device_class_hid_event_set`` returns a non-success
 *        status (queue full, class disconnected mid-loop, ...).
 * @note Read externally only.
 * @since 0.1.0
 */
volatile uint32_t g_usb_hid_mismatch = 0U;

/* -------------------------------------------------------------------------- */
/* HID Report Descriptor (3-button + X/Y boot mouse) */
/* -------------------------------------------------------------------------- */

/**
 * @var s_report_descriptor
 * @brief HID Report Descriptor for the boot mouse.
 *
 * @details Decoded sequence per USB HID 1.11 sec E.10:
 *   05 01           Usage Page (Generic Desktop)
 *   09 02           Usage      (Mouse)
 *   A1 01           Collection (Application)
 *     09 01           Usage      (Pointer)
 *     A1 00           Collection (Physical)
 *       05 09           Usage Page (Buttons)
 *       19 01           Usage Min  (1)
 *       29 03           Usage Max  (3)
 *       15 00           Logical Min (0)
 *       25 01           Logical Max (1)
 *       95 03           Report Count (3)
 *       75 01           Report Size  (1)
 *       81 02           Input (Data,Var,Abs)
 *       95 01           Report Count (1)
 *       75 05           Report Size  (5)
 *       81 03           Input (Cnst,Var,Abs)        -- 5 padding bits
 *       05 01           Usage Page (Generic Desktop)
 *       09 30           Usage      (X)
 *       09 31           Usage      (Y)
 *       15 81           Logical Min (-127)
 *       25 7F           Logical Max ( 127)
 *       75 08           Report Size  (8)
 *       95 02           Report Count (2)
 *       81 06           Input (Data,Var,Rel)
 *     C0              End Collection (Physical)
 *   C0              End Collection (Application)
 *
 * @note Pure 7-bit ASCII; bytes are hex literals.
 * @since 0.1.0
 */
static UCHAR s_report_descriptor[] = {
  0x05U, 0x01U, /* Usage Page (Generic Desktop) */
  0x09U, 0x02U, /* Usage (Mouse)                */
  0xA1U, 0x01U, /* Collection (Application)     */
  0x09U, 0x01U, /* Usage (Pointer)              */
  0xA1U, 0x00U, /* Collection (Physical)        */
  0x05U, 0x09U, /* Usage Page (Buttons)         */
  0x19U, 0x01U, /* Usage Min (1)                */
  0x29U, 0x03U, /* Usage Max (3)                */
  0x15U, 0x00U, /* Logical Min (0)              */
  0x25U, 0x01U, /* Logical Max (1)              */
  0x95U, 0x03U, /* Report Count (3)             */
  0x75U, 0x01U, /* Report Size  (1)             */
  0x81U, 0x02U, /* Input (Data,Var,Abs)         */
  0x95U, 0x01U, /* Report Count (1)             */
  0x75U, 0x05U, /* Report Size  (5)             */
  0x81U, 0x03U, /* Input (Cnst,Var,Abs) padding */
  0x05U, 0x01U, /* Usage Page (Generic Desktop) */
  0x09U, 0x30U, /* Usage (X)                    */
  0x09U, 0x31U, /* Usage (Y)                    */
  0x15U, 0x81U, /* Logical Min (-127)           */
  0x25U, 0x7FU, /* Logical Max ( 127)           */
  0x75U, 0x08U, /* Report Size  (8)             */
  0x95U, 0x02U, /* Report Count (2)             */
  0x81U, 0x06U, /* Input (Data,Var,Rel)         */
  0xC0U,        /* End Collection (Physical)    */
  0xC0U,        /* End Collection (Application) */
};

/* -------------------------------------------------------------------------- */
/* USB descriptors (DEVICE + CONFIG + INTERFACE + HID + EP IN) */
/* -------------------------------------------------------------------------- */

/* VID/PID matches the prior bare-metal app (pid.codes test range). The
 * configuration is one HID interface, one interrupt-IN endpoint, no
 * OUT endpoint -- the boot mouse class profile.
 *
 * Total config-blob length:
 *   9 (config) + 9 (interface) + 9 (HID class) + 7 (EP IN) = 34 bytes.
 *
 * Layout per USB 2.0 sec 9.6 and HID 1.11 sec 6.2.1.
 */
static UCHAR s_device_framework_fs[] = {
  /* Device descriptor (USB 2.0 sec 9.6.1) -- 18 bytes. */
  0x12U,
  0x01U,
  0x00U,
  0x02U,
  0x00U,
  0x00U,
  0x00U,
  0x40U,
  0x09U,
  0x12U,
  0x01U,
  0x00U,
  0x00U,
  0x01U,
  0x01U,
  0x02U,
  0x03U,
  0x01U,
  /* Configuration descriptor (USB 2.0 sec 9.6.3) -- 9 bytes.
   * bmAttributes = 0x80 (bus-powered). 0xC0 (self-powered)
   * conflicted with bMaxPower=100mA (50 * 2). */
  0x09U,
  0x02U,
  0x22U,
  0x00U,
  0x01U,
  0x01U,
  0x00U,
  0x80U,
  0x32U,
  /* Interface descriptor -- HID, boot subclass, mouse protocol. */
  0x09U,
  0x04U,
  0x00U,
  0x00U,
  0x01U,
  0x03U,
  0x01U,
  0x02U,
  0x00U,
  /* HID class descriptor (HID 1.11 sec 6.2.1) -- 9 bytes.
   * Report-descriptor length is sizeof(s_report_descriptor) = 52,
   * little-endian (0x34, 0x00). */
  0x09U,
  0x21U,
  0x11U,
  0x01U,
  0x00U,
  0x01U,
  0x22U,
  0x34U,
  0x00U,
  /* Endpoint descriptor: EP1 IN, interrupt, 8-byte MPS, 10 ms poll. */
  0x07U,
  0x05U,
  0x81U,
  0x03U,
  0x08U,
  0x00U,
  0x0AU,
};

/* String descriptors -- vendor / product / serial. Each entry:
 *   2 bytes lang-id, 1 byte string index, 1 byte string length,
 *   then ASCII bytes. */
static UCHAR s_string_framework[] = {
  /* idx 1: "Brighton Sikarskie" (18 chars). */
  0x09U,
  0x04U,
  0x01U,
  0x12U,
  'B',
  'r',
  'i',
  'g',
  'h',
  't',
  'o',
  'n',
  ' ',
  'S',
  'i',
  'k',
  'a',
  'r',
  's',
  'k',
  'i',
  'e',
  /* idx 2: "EK-RA8D2 HID Mouse" (18 chars). */
  0x09U,
  0x04U,
  0x02U,
  0x12U,
  'E',
  'K',
  '-',
  'R',
  'A',
  '8',
  'D',
  '2',
  ' ',
  'H',
  'I',
  'D',
  ' ',
  'M',
  'o',
  'u',
  's',
  'e',
  /* idx 3: "00000001" (8 chars). */
  0x09U,
  0x04U,
  0x03U,
  0x08U,
  '0',
  '0',
  '0',
  '0',
  '0',
  '0',
  '0',
  '1',
};

/* USBX LANGID descriptor 0x0409 (English-US), little-endian byte pair. */
typedef enum : uint8_t {
  k_usb_langid_en_us_lo = 0x09U, /**< LANGID 0x0409 low byte.  */
  k_usb_langid_en_us_hi = 0x04U, /**< LANGID 0x0409 high byte. */
} usb_langid_byte_t;

static UCHAR s_language_id_framework[] = {k_usb_langid_en_us_lo, k_usb_langid_en_us_hi};

/* -------------------------------------------------------------------------- */
/* HID activate / deactivate callbacks */
/* -------------------------------------------------------------------------- */

/**
 * @brief HID activate callback. Captures the live class instance.
 *
 * @param[in] hid_instance Pointer to ``UX_SLAVE_CLASS_HID``.
 *
 * @pre Called from the USBX class thread.
 * @post ``s_hid_class`` points at the live HID class.
 *
 * @note USBX guarantees serialization with the deactivate callback.
 * @since 0.1.0
 */
static VOID demo_hid_activate(VOID* hid_instance)
{
  s_hid_class = (UX_SLAVE_CLASS_HID*)hid_instance;
}

/**
 * @brief HID deactivate callback. Drops the live-class pointer.
 *
 * @param[in] hid_instance Pointer to ``UX_SLAVE_CLASS_HID`` (unused).
 *
 * @pre Called from the USBX class thread.
 * @post ``s_hid_class`` is ``UX_NULL``.
 *
 * @note USBX guarantees serialization with the activate callback.
 * @since 0.1.0
 */
static VOID demo_hid_deactivate(VOID* hid_instance)
{
  (void)hid_instance;
  s_hid_class = UX_NULL;
}

/**
 * @brief HID GET_REPORT / SET_REPORT control-pipe callback.
 *
 * @details Boot mouse: hosts that issue GET_REPORT(input) get a zero-
 * deltas neutral report so they don't see stale state. SET_REPORT
 * (output / feature) is silently consumed -- the boot-mouse profile
 * has no LED bitmap.
 *
 * @param[in,out] hid       USBX HID class instance (unused).
 * @param[in,out] hid_event Pre-allocated event slot to fill.
 *
 * @return ``UX_SUCCESS`` always.
 *
 * @pre ``hid_event`` is non-NULL (USBX guarantee).
 * @post Event buffer holds a 3-byte neutral report.
 *
 * @note Called from USBX's control-pipe thread.
 * @since 0.1.0
 */
static UINT demo_hid_get_callback(UX_SLAVE_CLASS_HID* hid, UX_SLAVE_CLASS_HID_EVENT* hid_event)
{
  (void)hid;
  hid_event->ux_device_class_hid_event_length      = (ULONG)k_demo_hid_report_bytes;
  hid_event->ux_device_class_hid_event_report_id   = 0UL;
  hid_event->ux_device_class_hid_event_report_type = (ULONG)UX_DEVICE_CLASS_HID_REPORT_TYPE_INPUT;
  hid_event->ux_device_class_hid_event_buffer[k_demo_hid_idx_buttons] = 0U;
  hid_event->ux_device_class_hid_event_buffer[k_demo_hid_idx_dx]      = 0U;
  hid_event->ux_device_class_hid_event_buffer[k_demo_hid_idx_dy]      = 0U;
  return UX_SUCCESS;
}

/* -------------------------------------------------------------------------- */
/* Worker thread: bring USBX up + jiggle loop */
/* -------------------------------------------------------------------------- */

/**
 * @brief Build the boot-mouse report for one phase of the jiggle.
 *
 * @param[in]  phase  Position in the 4-step square pattern.
 * @param[out] report Destination buffer; must hold ``k_demo_hid_report_bytes``.
 *
 * @pre ``report`` is non-NULL.
 * @pre ``phase < k_demo_phase_count``.
 * @post All three report bytes written.
 *
 * @note Reentrant; no shared state.
 * @since 0.1.0
 */
static void demo_build_jiggle(demo_phase_t phase, UCHAR* report)
{
  int8_t dx = k_demo_jiggle_zero;
  int8_t dy = k_demo_jiggle_zero;
  switch (phase) {
    case k_demo_phase_right:
      dx = k_demo_jiggle_step;
      break;
    case k_demo_phase_down:
      dy = k_demo_jiggle_step;
      break;
    case k_demo_phase_left:
      dx = k_demo_jiggle_neg;
      break;
    case k_demo_phase_up:
      dy = k_demo_jiggle_neg;
      break;
    case k_demo_phase_count:
    default:
      break;
  }
  report[k_demo_hid_idx_buttons] = 0U;
  report[k_demo_hid_idx_dx]      = (UCHAR)dx;
  report[k_demo_hid_idx_dy]      = (UCHAR)dy;
}

/**
 * @brief Brings the USBX system and FS device stack up.
 *
 * @return UINT UX_SUCCESS on success.
 * @retval UX_SUCCESS Stack ready.
 *
 * @pre File-scope ``s_usbx_pool`` is reserved.
 * @pre Caller is in thread context.
 * @post Device stack accepts class registrations.
 * @post On failure, USBX state is undefined.
 *
 * @note Call once per worker bring-up.
 * @since 0.1.0
 */
static UINT demo_usbx_stack_up(void)
{
  if (_ux_system_initialize(s_usbx_pool, k_demo_usbx_pool_bytes, UX_NULL, 0) != UX_SUCCESS) {
    return UX_ERROR;
  }
  return _ux_device_stack_initialize((UCHAR*)UX_NULL,
                                     0,
                                     s_device_framework_fs,
                                     sizeof(s_device_framework_fs),
                                     s_string_framework,
                                     sizeof(s_string_framework),
                                     s_language_id_framework,
                                     sizeof(s_language_id_framework),
                                     UX_NULL);
}

/**
 * @brief Registers the HID class with the configured report descriptor.
 *
 * @return UINT UX_SUCCESS on success, propagated USBX error otherwise.
 * @retval UX_SUCCESS Class registered.
 *
 * @pre ``demo_usbx_stack_up`` has succeeded.
 * @pre ``s_report_descriptor`` is at file scope and valid.
 * @post HID class bound to configuration 1, interface 0.
 * @post ``demo_hid_activate`` will fire on SET_CONFIGURATION.
 *
 * @note Not re-entrant.
 * @since 0.1.0
 */
static UINT demo_hid_class_register(void)
{
  UX_SLAVE_CLASS_HID_PARAMETER hid_params = {
    .ux_slave_class_hid_instance_activate         = demo_hid_activate,
    .ux_slave_class_hid_instance_deactivate       = demo_hid_deactivate,
    .ux_device_class_hid_parameter_report_address = s_report_descriptor,
    .ux_device_class_hid_parameter_report_id      = 0UL,
    .ux_device_class_hid_parameter_report_length  = (ULONG)sizeof(s_report_descriptor),
    .ux_device_class_hid_parameter_callback       = UX_NULL,
    .ux_device_class_hid_parameter_get_callback   = demo_hid_get_callback,
  };
  return _ux_device_stack_class_register((UCHAR*)"ux_slave_class_hid",
                                         _ux_device_class_hid_entry,
                                         1,
                                         0,
                                         &hid_params);
}

/**
 * @brief Push a single HID jiggle event in the given phase.
 *
 * @param[in,out] phase Current jiggle phase; advanced on successful send.
 *
 * @pre Worker thread context; ``s_hid_class`` is non-NULL.
 * @pre ``phase`` points to a valid ``demo_phase_t``.
 * @post On UX_SUCCESS: LED1 toggled and ``*phase`` advanced.
 * @post On failure: ``*phase`` unchanged.
 *
 * @note Caller paces invocations with ``tx_thread_sleep``.
 * @since 0.1.0
 */
static void demo_jiggle_send(demo_phase_t* phase)
{
  UX_SLAVE_CLASS_HID_EVENT hid_event;
  (void)memset(&hid_event, 0, sizeof(hid_event));
  hid_event.ux_device_class_hid_event_length      = (ULONG)k_demo_hid_report_bytes;
  hid_event.ux_device_class_hid_event_report_id   = 0UL;
  hid_event.ux_device_class_hid_event_report_type = (ULONG)UX_DEVICE_CLASS_HID_REPORT_TYPE_INPUT;
  demo_build_jiggle(*phase, hid_event.ux_device_class_hid_event_buffer);

  if (_ux_device_class_hid_event_set(s_hid_class, &hid_event) == UX_SUCCESS) {
    (void)ra8_board_led_toggle(k_ra8_board_led1);
    *phase = (demo_phase_t)(((uint8_t)(*phase + 1U)) % k_demo_phase_count);
    g_usb_hid_match += 1U;
  } else {
    g_usb_hid_mismatch += 1U;
  }
}

static VOID demo_worker(ULONG arg)
{
  (void)arg;

  if (demo_usbx_stack_up() != UX_SUCCESS) {
    return;
  }
  if (demo_hid_class_register() != UX_SUCCESS) {
    return;
  }
  if (ux_dcd_ra8_usb_initialize(k_ra8_usb_speed_fs) != k_ra8_ok) {
    return;
  }
  if (ra8_usb_device_attach(k_ra8_usb_speed_fs, true) != k_ra8_ok) {
    return;
  }

  /* Jiggle loop. INTSTS0 is drained by the bridge's own polled-dispatch
   * worker (spawned inside ux_dcd_ra8_usb_initialize BEFORE the
   * ra8_usb_device_attach(true) above raised DPRPU), so this thread
   * only needs to push HID reports once per period. */
  demo_phase_t phase = k_demo_phase_right;
  while (1) {
    if (s_hid_class == UX_NULL) {
      tx_thread_sleep(k_demo_jiggle_period_ticks);
      continue;
    }
    demo_jiggle_send(&phase);
    tx_thread_sleep(k_demo_jiggle_period_ticks);
  }
}

/* -------------------------------------------------------------------------- */
/* ThreadX kernel entry: spawn the worker */
/* -------------------------------------------------------------------------- */

/**
 * @brief ThreadX application-define hook. Spawns the demo worker.
 *
 * @param[in] first_unused_memory Sentinel (unused; we use static stacks).
 *
 * @pre Called from ``tx_kernel_enter`` after scheduler init.
 * @post One auto-start worker thread is queued.
 *
 * @note Called once at boot; not thread-safe.
 * @since 0.1.0
 */
VOID tx_application_define(VOID* first_unused_memory)
{
  (void)first_unused_memory;
  (void)tx_thread_create(&s_demo_thread,
                         "usb_hid_device",
                         demo_worker,
                         0UL,
                         s_demo_stack,
                         k_demo_thread_stack,
                         8U, /* priority          */
                         8U, /* preempt threshold */
                         TX_NO_TIME_SLICE,
                         TX_AUTO_START);
}
#endif /* !RA8_OFF_TARGET */

/* -------------------------------------------------------------------------- */
/* Startup helpers */
/* -------------------------------------------------------------------------- */

/**
 * @brief Halt forever in WFI -- panic stop on init failure.
 *
 * @pre Called only after a fatal error in boot.
 * @post CPU is parked; only debugger / external reset wakes it.
 *
 * @note Not thread-safe; not reachable post-boot.
 * @since 0.1.0
 */
static void demo_panic_halt(void)
{
  while (1) {
    __asm__ volatile("wfi");
  }
}

/**
 * @brief Route the four USB-FS pins to the USBFS controller.
 *
 * @return Error from the first failing route call, or k_ra8_ok.
 * @retval k_ra8_ok All four pins routed.
 *
 * @pre IOPORT module is reachable.
 * @pre Single-threaded init context.
 * @post On success the four USB-FS pins are in USB peripheral mode.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] static ra8_err_t demo_pins_init(void)
{
  ra8_err_t err = ra8_pfs_route_peripheral(k_demo_pin_vbus, k_ra8_psel_usb_fs, "usb_hid.vbus");
  if (err != k_ra8_ok) {
    return err;
  }
  /* VBUSEN as GPIO output LOW for USB device mode. Peripheral routing
   * forces VBUSEN HIGH (host mode) which blocks device enumeration. */
  err = ra8_gpio_output_init(k_demo_pin_vbusen, k_ra8_level_low);
  if (err != k_ra8_ok) {
    return err;
  }
  err = ra8_pfs_route_peripheral(k_demo_pin_dp, k_ra8_psel_usb_fs, "usb_hid.dp");
  if (err != k_ra8_ok) {
    return err;
  }
  return ra8_pfs_route_peripheral(k_demo_pin_dm, k_ra8_psel_usb_fs, "usb_hid.dm");
}

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmain"
/**
 * @brief Application entry. Brings up CGC + USB-FS pins + LED1 + ThreadX.
 *
 * @return Never returns (``tx_kernel_enter`` is __noreturn).
 *
 * @pre Reset_Handler has copied .data and zeroed .bss.
 * @pre SystemInit has set VTOR, FPU, and priority grouping.
 * @post On clean entry the CPU stays in tx_kernel_enter forever.
 * @post On any HAL init failure the function halts in WFI.
 *
 * @note Single entry point; not re-entrant.
 * @since 0.1.0
 */
int32_t main(void)
{
  uint32_t cpuclk0_hz = 0U;

  if (ra8_cgc_init() != k_ra8_ok) {
    demo_panic_halt();
  }
  /* Bring up PLL2 -> USBCKCR / USBCKDIVCR so USBFS sees a spec-compliant
   * 48 MHz reference (PLL2P 240 MHz / 5). Without this the SIE never
   * leaves Powered after host bus-reset. */
  if (ra8_cgc_usbfs_clock_enable() != k_ra8_ok) {
    demo_panic_halt();
  }
  if (ra8_cgc_get_clock_hz(k_ra8_clock_id_cpuclk0, &cpuclk0_hz) != k_ra8_ok) {
    demo_panic_halt();
  }
  if (ra8_time_init(cpuclk0_hz) != k_ra8_ok) {
    demo_panic_halt();
  }
  if (ra8_board_led_init(k_ra8_board_led1) != k_ra8_ok) {
    demo_panic_halt();
  }
  if (demo_pins_init() != k_ra8_ok) {
    demo_panic_halt();
  }

  ra8_isr_globals_enable();

#ifndef RA8_OFF_TARGET
  /* tx_kernel_enter is __noreturn -- it never comes back. */
  tx_kernel_enter();
#endif

  demo_panic_halt();
  return 0;
}
#pragma GCC diagnostic pop
