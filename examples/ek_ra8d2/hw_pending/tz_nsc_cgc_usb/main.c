/**
 * @file examples/ek_ra8d2/tz_nsc_cgc_usb/main.c
 * @brief NSC-veneer CGC + USBX CDC ACM echo demo for EK-RA8D2
 *
 * @par Tag
 * [Ring 6 / APP] {World: NS}
 *
 * @par Background:
 * Variant of ``examples/ek_ra8d2/usb_cdc_echo`` that runs the
 * application code in the Non-Secure world and proves the NSC-veneer
 * fix for the CGC-from-NS silent failure. The CGC register block lives
 * in the always-Secure System Control region: writes from a NS context
 * complete silently, so a TZ-on app cannot call ``ra_cgc_*`` directly.
 * Instead the bring-up here calls ``ra_nsc_cgc_pll2_enable``,
 * ``ra_nsc_cgc_usbfs_clock_enable`` and ``ra_nsc_cgc_get_clock_hz``,
 * each of which is a Non-Secure Callable veneer
 * (``cmse_nonsecure_entry``) that traps to the Secure world and
 * forwards to the underlying ``ra_cgc_*`` function.
 *
 * @details
 * Brings the chip up via ``ra_cgc_init()`` (XTAL -> PLL1 -> CPUCLK0 =
 * 1 GHz, PCLKA = 125 MHz), routes the four USB-FS pins per the
 * EK-RA8D2 v1 User's Manual to the on-board USB-FS receptacle, hands
 * control to ThreadX, and brings the CDC ACM device class up via
 * Eclipse USBX (``_ux_device_class_cdc_acm_initialize``). The class
 * sits on top of the project's ``port/usbx/ux_dcd_ra_usb`` bridge to
 * the hand-written ``ra_usb`` register-level driver (HUM Ch. 36
 * USBFS, sec. 36.2.x for SYSCFG / DCPCFG / DCPMAXP / PIPECFG /
 * CFIFO). The host actually enumerates the device because USBX's
 * chapter-9 state machine answers SETUP packets through the DCD
 * bridge.
 *
 * Once enumerated, the worker thread loops on
 * ``_ux_device_class_cdc_acm_read`` -> ``_ux_device_class_cdc_acm_write``
 * (echo). LED1 toggles per byte echoed.
 *
 * ## Pinout (USB-FS, FSP-aligned)
 *
 * | Net           | Pin    | PFS PSEL                |
 * |---------------|--------|-------------------------|
 * | USB_FS_VBUS   | P4_07  | k_ra_psel_usb_fs (0x13) |
 * | USB_FS_VBUSEN | P5_00  | k_ra_psel_usb_fs (0x13) |
 * | USB_FS_DP     | P8_14  | k_ra_psel_usb_fs (0x13) |
 * | USB_FS_DM     | P8_15  | k_ra_psel_usb_fs (0x13) |
 *
 * ## Sequence
 *
 *   1. ``ra_cgc_init()`` -- standard FSP-quickstart clock tree.
 *   2. ``ra_time_init`` for back-off delays.
 *   3. ``ra_pfs_route_peripheral`` for the four USB-FS pins.
 *   4. ``ra_board_led_init(k_ra_board_led1)`` for visual heartbeat.
 *   5. ThreadX ``tx_kernel_enter()`` -- spins the scheduler.
 *   6. ``tx_application_define`` -- spawns one worker thread that:
 *        - Allocates USBX memory pool and calls
 *          ``_ux_system_initialize`` + ``_ux_device_stack_initialize``.
 *        - Calls ``_ux_device_stack_class_register`` for the CDC-ACM
 *          class.
 *        - Calls ``ux_dcd_ra_usb_initialize(k_ra_usb_speed_fs)`` to
 *          plug our DCD bridge into USBX.
 *        - Calls ``ra_usb_device_attach(true)`` so the host begins
 *          enumeration.
 *        - Drops into the echo loop.
 *
 * ## Verification (macOS)
 *
 * After flashing, the EK-RA8D2's USB-FS receptacle (J11) enumerates
 * as ``/dev/cu.usbmodem*``. Open it RDWR with picocom or screen and
 * type characters; every byte echoes back and LED1 toggles per byte.
 *
 * @author Brighton Sikarskie
 * @date 2026-05-02
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>
#include <string.h>

#include "ra8d2_cgc_regs.h"
#include "ra_board_ek_ra8d2.h"
#include "ra_cgc.h"
#include "ra_err.h"
#include "ra_gpio_constants.h"
#include "ra_isr.h"
#include "ra_nsc_cgc.h"
#include "ra_port_constants.h"
#include "ra_port_utils.h"
#include "ra_time.h"
#include "ra_usb.h"

#ifndef RA_SIMULATOR_MODE
#include "tx_api.h"
#include "ux_api.h"
#include "ux_dcd_ra_usb.h"
#include "ux_device_class_cdc_acm.h"
#include "ux_device_stack.h"
#endif

/* -------------------------------------------------------------------------- */
/* Pinout (FSP-aligned, EK-RA8D2 v1 User's Manual)                            */
/* -------------------------------------------------------------------------- */

/**
 * @brief USB-FS pin identifiers, packed ``ra_port_pin_t`` (port << 8 | pin).
 * @details Built as a runtime cast so clang-tidy's enum-range check
 * is happy with the otherwise out-of-enum value.
 * @since 0.1.0
 */
static const ra_port_pin_t k_demo_pin_vbus =
  (ra_port_pin_t)(((uint16_t)k_ra_port_4 << 8) | (uint16_t)k_ra_pin_7);
static const ra_port_pin_t k_demo_pin_vbusen =
  (ra_port_pin_t)(((uint16_t)k_ra_port_5 << 8) | (uint16_t)k_ra_pin_0);
static const ra_port_pin_t k_demo_pin_dp =
  (ra_port_pin_t)(((uint16_t)k_ra_port_8 << 8) | (uint16_t)k_ra_pin_14);
static const ra_port_pin_t k_demo_pin_dm =
  (ra_port_pin_t)(((uint16_t)k_ra_port_8 << 8) | (uint16_t)k_ra_pin_15);

/* -------------------------------------------------------------------------- */
/* Tunables                                                                   */
/* -------------------------------------------------------------------------- */

/**
 * @enum demo_config_t
 * @brief Compile-time settings for the echo loop and ThreadX worker.
 */
typedef enum : uint32_t {
  k_demo_thread_stack    = 4096U,  /**< Worker thread stack (bytes).        */
  k_demo_usbx_pool_bytes = 16384U, /**< USBX memory pool (bytes).           */
  k_demo_echo_buf_bytes  = 64U,    /**< One bulk-FS packet per recv/send.   */
  k_demo_idle_ticks      = 1U,     /**< Idle back-off when no class active. */
} demo_config_t;

/**
 * @enum demo_pll2_t
 * @brief PLL2 bring-up parameters used through the NSC veneer.
 *
 * @details VCO = 24 MHz / 2 (fixed) * 80 = 960 MHz; with PLODIV /4 the
 *   PLL2P output is 240 MHz, which USBCKDIVCR /5 brings down to the
 *   spec-mandated 48 MHz USB-FS reference (HUM Ch 9, USBCKCR p 365).
 */
typedef enum : uint8_t {
  k_demo_pll2_mul_int      = 80U, /**< PLL2 multiplier integer part. */
  k_demo_pll2_mul_quarters = 0U,  /**< PLL2 multiplier quarter part. */
} demo_pll2_t;

#ifndef RA_SIMULATOR_MODE

/* -------------------------------------------------------------------------- */
/* ThreadX worker + USBX pool storage                                         */
/* -------------------------------------------------------------------------- */

/**
 * @var s_demo_thread
 * @brief ThreadX TCB for the USBX worker thread.
 * @note Single-writer (worker only).
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
 * @var s_cdc_acm
 * @brief Active CDC-ACM class instance, captured by activate callback.
 * @note Read by worker; written by USBX class thread.
 * @since 0.1.0
 */
static UX_SLAVE_CLASS_CDC_ACM* s_cdc_acm = UX_NULL;

/**
 * @var g_tz_nsc_cgc_usb_match
 * @brief HIL liveness counter -- incremented on every worker-thread
 *        iteration once the NSC-veneer-mediated CGC bring-up has
 *        finished and the USB device stack is live.
 *
 * @details
 * Read externally by scripts/hil_jlink_memprobe.sh via SWD. The probe
 * asserts this counter advances by >= HIL_PROBE_MIN_ADVANCE over the
 * sample window, proving:
 *   1. The Secure-side ra_cgc_init ran during Reset_Handler.
 *   2. The three NSC veneer calls in main() (pll2_enable,
 *      usbfs_clock_enable, get_clock_hz) returned k_ra_ok -- i.e. the
 *      cross-world transition actually delivered the expected return
 *      value, not the silent-drop failure mode that motivates this
 *      demo.
 *   3. tx_kernel_enter dispatched the worker, which is now iterating.
 *
 * The alive-mode check could only prove the chip didn't crash, not
 * that the NSC trampolines correctly funnelled CGC writes into the
 * Secure world.
 *
 * @note Read externally by J-Link only; firmware never reads back.
 * @since 0.1.0
 */
volatile uint32_t g_tz_nsc_cgc_usb_match = 0U;

/**
 * @var g_tz_nsc_cgc_usb_mismatch
 * @brief HIL failure counter -- incremented when USBX stack bring-up,
 *        class registration, DCD initialize, or device attach fail in
 *        the worker thread, or when a CDC-ACM read/write returns a
 *        non-UX_SUCCESS status after enumeration.
 *
 * @details
 * The memprobe asserts this stays at 0 (or below HIL_PROBE_MAX_FAILURE).
 * Catches the silent-failure mode where the NSC veneer succeeded but
 * the downstream USBX stack failed to come up -- previously invisible
 * because the worker thread returned cleanly on init failure and the
 * chip kept the ThreadX scheduler running with no live worker.
 *
 * @note Read externally by J-Link only; firmware never reads back.
 * @since 0.1.0
 */
volatile uint32_t g_tz_nsc_cgc_usb_mismatch = 0U;

/**
 * @var g_tz_nsc_cgc_usb_init_step
 * @brief Boot-step tracker. Reads via JLink memprobe localize which
 *        init call stalled or panic-halted.
 * @details
 * 0 = pre-main; 1 = before PLL2 enable; 2 = before USBFS clock;
 * 3 = before clock-hz query; 4 = before time_init; 5 = before
 * board_led_init; 6 = before pins_init; 7 = init complete, entering
 * ThreadX.
 * @note Read externally only.
 * @since 0.1.0
 */
/**
 * @brief Boot-step breadcrumb values for ::g_tz_nsc_cgc_usb_init_step.
 * @details Each value names the NSC/init operation in progress; read
 *          back via J-Link memprobe to localize a bring-up hang.
 */
typedef enum : uint32_t {
  k_tz_nsc_step_start       = 0U, /**< Before any init op. */
  k_tz_nsc_step_pll2_enable = 1U, /**< PLL2 enable via NSC. */
  k_tz_nsc_step_usbfs_clock = 2U, /**< USB-FS clock enable via NSC. */
  k_tz_nsc_step_clock_query = 3U, /**< CPUCLK0 frequency query via NSC. */
  k_tz_nsc_step_time_init   = 4U, /**< System tick init. */
  k_tz_nsc_step_led_init    = 5U, /**< Board LED init. */
  k_tz_nsc_step_pins_init   = 6U, /**< USB pin routing. */
  k_tz_nsc_step_irq_enable  = 7U, /**< Global IRQ enable (init done). */
} tz_nsc_init_step_t;

volatile uint32_t g_tz_nsc_cgc_usb_init_step = k_tz_nsc_step_start;

/* -------------------------------------------------------------------------- */
/* USB descriptors (DEVICE + CONFIG + IAD + CDC interfaces + endpoints)       */
/* -------------------------------------------------------------------------- */

/* VID/PID matches the prior bare-metal app (pid.codes test range). The
 * configuration is one CDC ACM communications interface + one CDC data
 * interface, with EP3 IN (interrupt) for notifications and EP2 OUT /
 * EP1 IN (bulk, 64-byte MPS) for the data pipes. Layout per CDC 1.20
 * sec 5 + USB 2.0 sec 9.6.
 */
static UCHAR s_device_framework_fs[] = {
  /* Device descriptor (USB 2.0 sec 9.6.1) -- 18 bytes.
     bcdUSB = 0x0200; macOS rejects IAD composite devices on USB 1.1. */
  0x12U,
  0x01U,
  0x00U,
  0x02U,
  0xEFU, /* class      = MISC                 */
  0x02U, /* subclass   = common               */
  0x01U, /* protocol   = IAD                  */
  0x40U,
  0x09U,
  0x12U,
  0x0AU,
  0x00U,
  0x00U,
  0x01U,
  0x01U,
  0x02U,
  0x03U,
  0x01U,
  /* Configuration descriptor (75 bytes total: 9 cfg + 8 IAD + 9 CCI +
     5 hdr + 5 call-mgmt + 4 ACM + 5 union + 7 EP3 + 9 DCI + 7 EP2 +
     7 EP1 = 75 = 0x4B). Truncating to 0x43 silently dropped EP1 IN,
     causing USBX to dereference a NULL endpoint after SET_CONFIG and
     escalate to lockup (PC=0xEFFFFFFE).  bmAttributes = 0x80 (bus-
     powered) -- 0xC0 (self-powered) conflicted with bMaxPower=100mA. */
  0x09U,
  0x02U,
  0x4BU,
  0x00U,
  0x02U,
  0x01U,
  0x00U,
  0x80U,
  0x32U,
  /* Interface association (CDC). */
  0x08U,
  0x0BU,
  0x00U,
  0x02U,
  0x02U,
  0x02U,
  0x01U,
  0x00U,
  /* Communications interface (CDC ACM). */
  0x09U,
  0x04U,
  0x00U,
  0x00U,
  0x01U,
  0x02U,
  0x02U,
  0x01U,
  0x00U,
  /* CDC header functional descriptor. */
  0x05U,
  0x24U,
  0x00U,
  0x10U,
  0x01U,
  /* Call-management functional descriptor. */
  0x05U,
  0x24U,
  0x01U,
  0x00U,
  0x01U,
  /* ACM functional descriptor. */
  0x04U,
  0x24U,
  0x02U,
  0x02U,
  /* Union functional descriptor. */
  0x05U,
  0x24U,
  0x06U,
  0x00U,
  0x01U,
  /* Interrupt-IN endpoint (EP3 IN, 8-byte MPS, 255 ms poll). */
  0x07U,
  0x05U,
  0x83U,
  0x03U,
  0x08U,
  0x00U,
  0xFFU,
  /* Data-class interface. */
  0x09U,
  0x04U,
  0x01U,
  0x00U,
  0x02U,
  0x0AU,
  0x00U,
  0x00U,
  0x00U,
  /* Bulk-OUT endpoint (EP2 OUT, 64-byte MPS). */
  0x07U,
  0x05U,
  0x02U,
  0x02U,
  0x40U,
  0x00U,
  0x00U,
  /* Bulk-IN endpoint (EP1 IN, 64-byte MPS). */
  0x07U,
  0x05U,
  0x81U,
  0x02U,
  0x40U,
  0x00U,
  0x00U,
};

/**
 * @var s_string_framework
 * @brief USBX string descriptor table (vendor / product / serial).
 * @details Each entry: 2 bytes lang-id, 1 byte string index, 1 byte
 *          length, then ASCII bytes.
 * @since 0.1.0
 */
static UCHAR s_string_framework[] = {
  /* idx 1: "Brighton Sikarskie". */
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
  /* idx 2: "EK-RA8D2 CDC Echo!". */
  0x09U,
  0x04U,
  0x02U,
  0x14U,
  'E',
  'K',
  '-',
  'R',
  'A',
  '8',
  'D',
  '2',
  ' ',
  'C',
  'D',
  'C',
  ' ',
  'E',
  'c',
  'h',
  'o',
  '!',
  /* idx 3: serial. */
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

/**
 * @var s_language_id_framework
 * @brief USBX language-id table -- US English.
 * @since 0.1.0
 */
/* USBX LANGID descriptor 0x0409 (English-US), little-endian byte pair. */
typedef enum : uint8_t {
  k_usb_langid_en_us_lo = 0x09U, /**< LANGID 0x0409 low byte.  */
  k_usb_langid_en_us_hi = 0x04U, /**< LANGID 0x0409 high byte. */
} usb_langid_byte_t;

static UCHAR s_language_id_framework[] = {k_usb_langid_en_us_lo, k_usb_langid_en_us_hi};

/* -------------------------------------------------------------------------- */
/* CDC-ACM activate / deactivate callbacks                                    */
/* -------------------------------------------------------------------------- */

/**
 * @brief CDC-ACM activate callback. Captures the live class instance.
 *
 * @param[in] cdc_instance Pointer to ``UX_SLAVE_CLASS_CDC_ACM``.
 *
 * @pre Called from the USBX class thread.
 * @post ``s_cdc_acm`` points at the live CDC-ACM class.
 *
 * @note USBX guarantees serialization with the deactivate callback.
 * @since 0.1.0
 */
static VOID demo_cdc_activate(VOID* cdc_instance)
{
  s_cdc_acm = (UX_SLAVE_CLASS_CDC_ACM*)cdc_instance;
}

/**
 * @brief CDC-ACM deactivate callback. Drops the live class pointer.
 *
 * @param[in] cdc_instance Unused.
 *
 * @pre Called from the USBX class thread.
 * @post ``s_cdc_acm`` is ``UX_NULL``.
 *
 * @note USBX guarantees serialization with the activate callback.
 * @since 0.1.0
 */
static VOID demo_cdc_deactivate(VOID* cdc_instance)
{
  (void)cdc_instance;
  s_cdc_acm = UX_NULL;
}

/* -------------------------------------------------------------------------- */
/* Worker thread: bring USBX up + echo loop                                   */
/* -------------------------------------------------------------------------- */

/**
 * @brief Brings the USBX system + FS device stack up.
 *
 * @return UINT UX_SUCCESS on success.
 * @retval UX_SUCCESS Stack ready.
 *
 * @pre File-scope ``s_usbx_pool`` is reserved.
 * @pre Caller is in thread context.
 * @post Stack accepts class registrations.
 * @post On failure, USBX state is undefined.
 *
 * @note Single-call.
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
 * @brief Registers the CDC-ACM class against the device-stack configuration.
 *
 * @return UINT UX_SUCCESS on success.
 * @retval UX_SUCCESS Class registered.
 *
 * @pre ``demo_usbx_stack_up`` has succeeded.
 * @pre Activate/deactivate callbacks are defined at file scope.
 * @post CDC class bound to configuration 1, interface 0.
 * @post Activation callback fires on SET_CONFIGURATION.
 *
 * @note Not re-entrant.
 * @since 0.1.0
 */
static UINT demo_cdc_class_register(void)
{
  UX_SLAVE_CLASS_CDC_ACM_PARAMETER cdc_params = {
    .ux_slave_class_cdc_acm_instance_activate   = demo_cdc_activate,
    .ux_slave_class_cdc_acm_instance_deactivate = demo_cdc_deactivate,
    .ux_slave_class_cdc_acm_parameter_change    = UX_NULL,
  };
  return _ux_device_stack_class_register((UCHAR*)"ux_slave_class_cdc_acm",
                                         _ux_device_class_cdc_acm_entry,
                                         1,
                                         0,
                                         &cdc_params);
}

/**
 * @brief One iteration of the CDC echo loop.
 *
 * @param[in,out] buf Scratch buffer.
 * @param[in]     cap Buffer capacity in bytes.
 *
 * @pre ``s_cdc_acm`` is non-NULL.
 * @pre ``buf`` is non-NULL with ``cap`` bytes.
 * @post On success, ``buf[0..n)`` is echoed back to host.
 * @post LED1 toggled once per echoed byte.
 *
 * @note Outer loop reinvokes this every iteration.
 * @since 0.1.0
 */
static void demo_echo_iter(UCHAR* buf, ULONG cap)
{
  ULONG n = 0UL;
  if (_ux_device_class_cdc_acm_read(s_cdc_acm, buf, cap, &n) != UX_SUCCESS) {
    g_tz_nsc_cgc_usb_mismatch += 1U;
    tx_thread_sleep(k_demo_idle_ticks);
    return;
  }
  if (n == 0UL) {
    return;
  }
  if (_ux_device_class_cdc_acm_write(s_cdc_acm, buf, n, &n) != UX_SUCCESS) {
    g_tz_nsc_cgc_usb_mismatch += 1U;
    return;
  }
  for (ULONG i = 0UL; i < n; i++) {
    (void)ra_board_led_toggle(k_ra_board_led1);
  }
}

static VOID demo_worker(ULONG arg)
{
  (void)arg;

  if (demo_usbx_stack_up() != UX_SUCCESS) {
    g_tz_nsc_cgc_usb_mismatch += 1U;
    return;
  }
  if (demo_cdc_class_register() != UX_SUCCESS) {
    g_tz_nsc_cgc_usb_mismatch += 1U;
    return;
  }
  if (ux_dcd_ra_usb_initialize(k_ra_usb_speed_fs) != k_ra_ok) {
    g_tz_nsc_cgc_usb_mismatch += 1U;
    return;
  }
  if (ra_usb_device_attach(k_ra_usb_speed_fs, true) != k_ra_ok) {
    g_tz_nsc_cgc_usb_mismatch += 1U;
    return;
  }

  /* Past this point all NSC-veneer-mediated CGC + USB stack bring-up
   * succeeded. Each loop iteration past here proves the cross-world
   * trampolines delivered correct return values and the worker thread
   * is actually scheduled. The counter advances even without a USB
   * host connected (idle-tick path), so the probe works on a stock
   * HIL board with no USB-FS host attached. */
  UCHAR buf[k_demo_echo_buf_bytes];
  while (1) {
    g_tz_nsc_cgc_usb_match += 1U;
    if (s_cdc_acm == UX_NULL) {
      tx_thread_sleep(k_demo_idle_ticks);
      continue;
    }
    demo_echo_iter(buf, (ULONG)sizeof(buf));
  }
}

/* -------------------------------------------------------------------------- */
/* ThreadX kernel entry: spawn the worker                                     */
/* -------------------------------------------------------------------------- */

/**
 * @brief ThreadX application-define hook. Spawns the demo worker.
 *
 * @param[in] first_unused_memory Sentinel (unused; static stacks).
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
                         "usb_cdc_echo",
                         demo_worker,
                         0UL,
                         s_demo_stack,
                         k_demo_thread_stack,
                         8U, /* priority         */
                         8U, /* preempt threshold */
                         TX_NO_TIME_SLICE,
                         TX_AUTO_START);
}
#endif /* !RA_SIMULATOR_MODE */

/* -------------------------------------------------------------------------- */
/* Startup helpers                                                            */
/* -------------------------------------------------------------------------- */

/**
 * @brief Halt forever in WFI -- panic stop on init failure.
 *
 * @pre Called only after a fatal error in boot.
 * @post CPU is parked.
 *
 * @note Not reachable post-boot.
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
 * @return Error from the first failing route call, or k_ra_ok.
 * @retval k_ra_ok All four pins routed.
 *
 * @pre IOPORT module is reachable.
 * @pre Single-threaded init context.
 * @post On success the four USB-FS pins are in USB peripheral mode.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] static ra_err_t demo_pins_init(void)
{
  ra_err_t err = ra_pfs_route_peripheral(k_demo_pin_vbus, k_ra_psel_usb_fs, "usb_cdc.vbus");
  if (err != k_ra_ok) {
    return err;
  }
  /* VBUSEN as GPIO output LOW for device mode. Peripheral routing
   * makes the USB module drive VBUSEN HIGH (host-mode supply enable),
   * which blocks device-side enumeration. See usb_cdc_echo /
   * tz_secure_only_usb_fs for the same fix. */
  err = ra_gpio_output_init(k_demo_pin_vbusen, k_ra_level_low);
  if (err != k_ra_ok) {
    return err;
  }
  err = ra_pfs_route_peripheral(k_demo_pin_dp, k_ra_psel_usb_fs, "usb_cdc.dp");
  if (err != k_ra_ok) {
    return err;
  }
  return ra_pfs_route_peripheral(k_demo_pin_dm, k_ra_psel_usb_fs, "usb_cdc.dm");
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

  /* CGC bring-up (PLL2 + USBCKCR + clock query) must traverse the NSC
   * veneer wall: the CGC register block is Secure and direct NS writes
   * are silently dropped. The Secure-side ra_cgc_init() ran out of
   * Reset_Handler before this NS main() was entered, so we only need
   * to issue the PLL2 + USB-FS clock + query operations here, all via
   * NSC entries that trap into the Secure world. */
  g_tz_nsc_cgc_usb_init_step = k_tz_nsc_step_pll2_enable;
  if (ra_nsc_cgc_pll2_enable((uint8_t)k_demo_pll2_mul_int,
                             (uint8_t)k_demo_pll2_mul_quarters,
                             k_ra_plodiv_div4) != k_ra_ok) {
    demo_panic_halt();
  }
  g_tz_nsc_cgc_usb_init_step = k_tz_nsc_step_usbfs_clock;
  if (ra_nsc_cgc_usbfs_clock_enable() != k_ra_ok) {
    demo_panic_halt();
  }
  g_tz_nsc_cgc_usb_init_step = k_tz_nsc_step_clock_query;
  if (ra_nsc_cgc_get_clock_hz(k_ra_clock_id_cpuclk0, &cpuclk0_hz) != k_ra_ok) {
    demo_panic_halt();
  }
  g_tz_nsc_cgc_usb_init_step = k_tz_nsc_step_time_init;
  if (ra_time_init(cpuclk0_hz) != k_ra_ok) {
    demo_panic_halt();
  }
  g_tz_nsc_cgc_usb_init_step = k_tz_nsc_step_led_init;
  if (ra_board_led_init(k_ra_board_led1) != k_ra_ok) {
    demo_panic_halt();
  }
  g_tz_nsc_cgc_usb_init_step = k_tz_nsc_step_pins_init;
  if (demo_pins_init() != k_ra_ok) {
    demo_panic_halt();
  }
  g_tz_nsc_cgc_usb_init_step = k_tz_nsc_step_irq_enable;

  ra_isr_globals_enable();

#ifndef RA_SIMULATOR_MODE
  /* tx_kernel_enter is __noreturn -- it never comes back. */
  tx_kernel_enter();
#endif

  demo_panic_halt();
  return 0;
}
#pragma GCC diagnostic pop
