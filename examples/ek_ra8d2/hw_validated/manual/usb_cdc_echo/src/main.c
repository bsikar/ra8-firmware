/**
 * @file examples/ek_ra8d2/hw_validated/manual/usb_cdc_echo/src/main.c
 * @brief ThreadX + USBX CDC ACM echo for EK-RA8D2 (USB-FS)
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * Brings the chip up via ``ra8_cgc_init()`` (XTAL -> PLL1 -> CPUCLK0 =
 * 1 GHz, PCLKA = 125 MHz), routes the four USB-FS pins per the
 * EK-RA8D2 v1 User's Manual to the on-board USB-FS receptacle, hands
 * control to ThreadX, and brings the CDC ACM device class up via
 * Eclipse USBX (``_ux_device_class_cdc_acm_initialize``). The class
 * sits on top of the project's ``port/usbx/ux_dcd_ra8_usb`` bridge to
 * the hand-written ``ra8_usb`` register-level driver (HUM Ch. 36
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
 *        - Calls ``_ux_device_stack_class_register`` for the CDC-ACM
 *          class.
 *        - Calls ``ux_dcd_ra8_usb_initialize(k_ra8_usb_speed_fs)`` to
 *          plug our DCD bridge into USBX.
 *        - Calls ``ra8_usb_device_attach(true)`` so the host begins
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

#include "ra8_board_ek_ra8d2.h"
#include "ra8_boot_entry.h"
#include "ra8_cgc.h"
#include "ra8_err.h"
#include "ra8_gpio_constants.h"
#include "ra8_isr.h"
#include "ra8_log.h"
#include "ra8_port_constants.h"
#include "ra8_port_utils.h"
#include "ra8_time.h"
#include "ra8_usb.h"

#ifndef RA8_OFF_TARGET
#include "tx_api.h"
#include "ux_api.h"
#include "ux_dcd_ra8_usb.h"
#include "ux_device_class_cdc_acm.h"
#include "ux_device_stack.h"
#include "ux_system.h"

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
 * @brief Compile-time settings for the echo loop and ThreadX worker.
 */
typedef enum : uint32_t {
  k_demo_thread_stack    = 8192U,  /**< Worker thread stack (bytes).                     */
  k_demo_usbx_pool_bytes = 32768U, /**< USBX pool: 32 KiB; CDC-ACM enum exhausts 16 KiB. */
  k_demo_echo_buf_bytes  = 64U,    /**< One bulk-FS packet per recv/send.                */
  k_demo_idle_ticks      = 1U,     /**< Idle back-off when no class active.              */
} demo_config_t;

#ifndef RA8_OFF_TARGET

/* -------------------------------------------------------------------------- */
/* ThreadX worker + USBX pool storage */
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
 * @struct demo_diag_t
 * @brief Demo-loop counters; read via JLink to localise stalls.
 */
typedef struct {
  volatile uint32_t loop_iter;       /**< loop_iter register.       */
  volatile uint32_t loop_cdc_null;   /**< loop_cdc_null register.   */
  volatile uint32_t loop_pre_read;   /**< loop_pre_read register.   */
  volatile uint32_t loop_post_read;  /**< loop_post_read register.  */
  volatile uint32_t loop_read_ok;    /**< loop_read_ok register.    */
  volatile uint32_t loop_read_zero;  /**< loop_read_zero register.  */
  volatile uint32_t loop_pre_write;  /**< loop_pre_write register.  */
  volatile uint32_t loop_post_write; /**< loop_post_write register. */
} demo_diag_t;

/**
 * @var s_demo_diag
 * @brief Externally-readable counters for demo loop progress.
 * @note Increment-only; never cleared at runtime.
 * @since 0.1.0
 */
volatile demo_diag_t s_demo_diag = {};

/**
 * @var s_cdc_active_sem
 * @brief Posted by demo_cdc_activate; demo thread blocks on it instead
 *        of polling s_cdc_acm with tx_thread_sleep (which never returned
 *        on this silicon -- SysTick may be silenced under polled-dispatch
 *        worker load).
 * @note Single-producer (USBX class thread), single-consumer (demo).
 * @since 0.1.0
 */
static TX_SEMAPHORE s_cdc_active_sem;

/* -------------------------------------------------------------------------- */
/* USB descriptors (DEVICE + CONFIG + IAD + CDC interfaces + endpoints) */
/* -------------------------------------------------------------------------- */

/* VID/PID matches the prior bare-metal app (pid.codes test range). The
 * configuration is one CDC ACM communications interface + one CDC data
 * interface, with EP3 IN (interrupt) for notifications and EP2 OUT /
 * EP1 IN (bulk, 64-byte MPS) for the data pipes. Layout per CDC 1.20
 * sec 5 + USB 2.0 sec 9.6.
 */
static UCHAR s_device_framework_fs[] = {
  /* Device descriptor (USB 2.0 sec 9.6.1) -- 18 bytes.
     bcdUSB = 0x0200 (USB 2.0); macOS rejects IAD-based composite
     devices that advertise USB 1.1 (IAD ECN was added in USB 2.0). */
  0x12U,
  0x01U,
  0x00U,
  0x02U,
  0xEFU, /* class      = MISC   */
  0x02U, /* subclass   = common */
  0x01U, /* protocol   = IAD    */
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
     7 EP1 = 75 = 0x4B).  Host reads wTotalLength then requests that
     many bytes; truncating to 0x43 silently dropped EP1 IN, which
     caused USBX to dereference a NULL endpoint after SET_CONFIG and
     escalate to lockup (PC=0xEFFFFFFE). */
  0x09U,
  0x02U,
  0x4BU,
  0x00U,
  0x02U,
  0x01U,
  0x00U,
  0x80U, /* bmAttributes = bus-powered (bit 7 reserved-1).  Was 0xC0
            (self-powered) which conflicted with bMaxPower=100mA. */
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
  /* CDC header functional descriptor.  bcdCDC = 0x0120 (CDC 1.20). */
  0x05U,
  0x24U,
  0x00U,
  0x20U,
  0x01U,
  /* Call-management functional descriptor.
     bmCapabilities = 0x01 (device handles call management itself);
     bDataInterface=0x01 only makes sense if D0 is set. */
  0x05U,
  0x24U,
  0x01U,
  0x01U,
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
  /* idx 2: "EK-RA8D2 CDC Echo!" (18 ASCII bytes). */
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

/* USBX LANGID descriptor 0x0409 (English-US), little-endian byte pair. */
typedef enum : uint8_t {
  k_usb_langid_en_us_lo = 0x09U, /**< LANGID 0x0409 low byte.  */
  k_usb_langid_en_us_hi = 0x04U, /**< LANGID 0x0409 high byte. */
} usb_langid_byte_t;

/**
 * @var s_language_id_framework
 * @brief USBX language-id table -- US English.
 * @since 0.1.0
 */
static UCHAR s_language_id_framework[] = {k_usb_langid_en_us_lo, k_usb_langid_en_us_hi};

/* -------------------------------------------------------------------------- */
/* CDC-ACM activate / deactivate callbacks */
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
  /* USBX writes ux_slave_device_state = CONFIGURED in
   * _ux_device_stack_configuration_set just before invoking this
   * activate callback. Pin it here so any concurrent IRQ/poll-driven
   * write in the dispatch worker observes the chapter-9 result and
   * does not demote it back to ATTACHED, which would break the
   * subsequent cdc_acm_read state gate. */
  if (_ux_system_slave != UX_NULL) {
    _ux_system_slave->ux_system_slave_device.ux_slave_device_state =
      (unsigned long)UX_DEVICE_CONFIGURED;
  }
  /* Wake the demo loop -- it blocks on s_cdc_active_sem instead of
   * polling s_cdc_acm with tx_thread_sleep, which never returned on
   * this hardware. */
  (void)tx_semaphore_put(&s_cdc_active_sem);
  /* CDC bulk endpoints: EP2 OUT -> pipe 2, EP1 IN -> pipe 1. Enable
   * the bridge's ISR-side auto-echo, which mirrors OUT data back on
   * the IN pipe directly inside the ISR -- ~10x faster than the
   * worker-thread _read/_write loop. demo_worker's _read/_write call
   * is suppressed under the matching s_dcd_auto_echo_enable check so
   * the two paths cannot race on the bulk-OUT pipe. */
  ux_dcd_ra8_usb_auto_echo_enable(2U, 1U);
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
/* Worker thread: bring USBX up + echo loop */
/* -------------------------------------------------------------------------- */

/**
 * @brief Brings USBX system + device stack up with the FS framework.
 *
 * @return UINT UX_SUCCESS on success, propagated USBX error otherwise.
 * @retval UX_SUCCESS Stack initialized.
 *
 * @pre USBX memory pool ``s_usbx_pool`` is at file scope.
 * @pre Caller is in thread context (USBX requires ThreadX services).
 * @post On success, the device stack accepts class registrations.
 * @post On failure, USBX state is undefined; caller should bail.
 *
 * @note Single-call; not idempotent.
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
 * @return UINT UX_SUCCESS on success, propagated USBX error otherwise.
 * @retval UX_SUCCESS Class registered.
 *
 * @pre ``demo_usbx_stack_up`` has succeeded.
 * @pre ``demo_cdc_activate`` / ``demo_cdc_deactivate`` are defined.
 * @post CDC-ACM class is bound to configuration 1, interface 0.
 * @post Activation callback will post ``s_cdc_active_sem``.
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
  static UCHAR s_class_name[] = "ux_slave_class_cdc_acm";

  return _ux_device_stack_class_register(s_class_name,
                                         _ux_device_class_cdc_acm_entry,
                                         1,
                                         0,
                                         &cdc_params);
}

/**
 * @brief Pin USBX peripheral state at CONFIGURED.
 *
 * @details Works around a residual DVSQ-poll race on this silicon that can
 * leave ``ux_slave_device_state`` at ATTACHED after SET_CONFIGURATION,
 * breaking ``_ux_device_class_cdc_acm_read``'s state gate. Safe to assert
 * once ``s_cdc_acm`` is non-NULL.
 *
 * @pre Called from worker thread context.
 * @pre ``s_cdc_acm`` is non-NULL (CDC class activated).
 * @post ``_ux_system_slave->...ux_slave_device_state`` is CONFIGURED.
 * @post Read/write APIs accept transfers.
 *
 * @note Inline candidate; called every iteration.
 * @since 0.1.0
 */
static void demo_pin_configured_state(void)
{
  if (_ux_system_slave != UX_NULL) {
    _ux_system_slave->ux_system_slave_device.ux_slave_device_state =
      (unsigned long)UX_DEVICE_CONFIGURED;
  }
}

/**
 * @brief One iteration of the CDC echo loop.
 *
 * @param[in,out] buf Scratch buffer (echo data).
 * @param[in]     cap Buffer capacity in bytes.
 *
 * @pre Worker is running and ``s_cdc_acm`` is non-NULL.
 * @pre ``buf`` is non-NULL with ``cap`` bytes.
 * @post Diag counters updated.
 * @post LED1 toggled once per echoed byte on success.
 *
 * @note Returns on each iteration; outer loop reinvokes.
 * @since 0.1.0
 */
static void demo_echo_iter(UCHAR* buf, ULONG cap)
{
  demo_pin_configured_state();
  ULONG n = 0UL;
  s_demo_diag.loop_pre_read++;
  UINT read_status = _ux_device_class_cdc_acm_read(s_cdc_acm, buf, cap, &n);
  s_demo_diag.loop_post_read++;
  if (read_status != UX_SUCCESS) {
    tx_thread_sleep(k_demo_idle_ticks);
    return;
  }
  s_demo_diag.loop_read_ok++;
  if (n == 0UL) {
    s_demo_diag.loop_read_zero++;
    return;
  }
  s_demo_diag.loop_pre_write++;
  if (_ux_device_class_cdc_acm_write(s_cdc_acm, buf, n, &n) != UX_SUCCESS) {
    return;
  }
  s_demo_diag.loop_post_write++;
  for (ULONG i = 0UL; i < n; i++) {
    (void)ra8_board_led_toggle(k_ra8_board_led1);
  }
}

/**
 * @brief Worker thread entry. Brings USBX + CDC up, then echoes forever.
 *
 * @param[in] arg Unused (ThreadX entry signature).
 *
 * @pre ``tx_application_define`` started this thread auto-start.
 * @post Thread loops forever; never returns.
 *
 * @note Single-instance worker; not designed for re-entry.
 * @since 0.1.0
 */
static VOID demo_worker(ULONG arg)
{
  (void)arg;

  if (demo_usbx_stack_up() != UX_SUCCESS) {
    return;
  }
  if (demo_cdc_class_register() != UX_SUCCESS) {
    return;
  }
  if (ux_dcd_ra8_usb_initialize(k_ra8_usb_speed_fs) != k_ra8_ok) {
    return;
  }
  if (ra8_usb_device_attach(k_ra8_usb_speed_fs, true) != k_ra8_ok) {
    return;
  }

  UCHAR buf[k_demo_echo_buf_bytes];
  (void)buf;            /* Auto-echo owns the data path; buf reserved for future fallback. */
  (void)demo_echo_iter; /* Kept for the optional non-auto-echo fallback path.              */
  while (1) {
    s_demo_diag.loop_iter++;
    if (s_cdc_acm == UX_NULL) {
      s_demo_diag.loop_cdc_null++;
      /* Block until demo_cdc_activate posts the semaphore. Avoids
       * tx_thread_sleep, which we observed never returning on this
       * silicon under polled-dispatch worker load. */
      (void)tx_semaphore_get(&s_cdc_active_sem, TX_WAIT_FOREVER);
      continue;
    }
    /* Auto-echo handles bulk OUT -> IN mirroring inside the ISR.
     * Invoking demo_echo_iter here would race with auto-echo on the
     * IN pipe and deliver out-of-order data for MPS-aligned packets. */
    tx_thread_sleep(k_demo_idle_ticks);
  }
}

/* -------------------------------------------------------------------------- */
/* ThreadX kernel entry: spawn the worker */
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
  static CHAR s_semaphore_name[] = "cdc_active";
  static CHAR s_thread_name[]    = "usb_cdc_echo";

  (void)first_unused_memory;
  (void)tx_semaphore_create(&s_cdc_active_sem, s_semaphore_name, 0U);
  (void)tx_thread_create(&s_demo_thread,
                         s_thread_name,
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
  ra8_err_t err = ra8_pfs_route_peripheral(k_demo_pin_vbus, k_ra8_psel_usb_fs, "usb_cdc.vbus");
  if (err != k_ra8_ok) {
    return err;
  }
  /* P500 is the EK-RA8D2 USB-FS role-select GPIO per the v1 User's
   * Manual section "USB Full Speed" (J11 connector): drive LOW for
   * device mode, HIGH for host mode. Routing P500 to the USBFS
   * peripheral function lets the USB module drive it as VBUSEN
   * (host-mode VBUS supply enable, default HIGH) which keeps the chip
   * in HOST mode and prevents enumeration. For this device-mode demo
   * we instead claim P500 as a GPIO output and drive it LOW so the
   * board's role-select circuitry presents a USB device to the host. */
  err = ra8_gpio_output_init(k_demo_pin_vbusen, k_ra8_level_low);
  if (err != k_ra8_ok) {
    return err;
  }
  err = ra8_pfs_route_peripheral(k_demo_pin_dp, k_ra8_psel_usb_fs, "usb_cdc.dp");
  if (err != k_ra8_ok) {
    return err;
  }
  return ra8_pfs_route_peripheral(k_demo_pin_dm, k_ra8_psel_usb_fs, "usb_cdc.dm");
}

/**
 * @brief Application entry. Brings up CGC + USB-FS pins + LED1 + ThreadX.
 *
 * @pre Reset_Handler has copied .data and zeroed .bss.
 * @pre SystemInit has set VTOR, FPU, and priority grouping.
 * @post On clean entry the CPU stays in tx_kernel_enter forever.
 * @post On any HAL init failure the function halts in WFI.
 *
 * @note Single entry point; not re-entrant.
 * @since 0.1.0
 */
void main(void)
{
  uint32_t cpuclk0_hz = 0U;

  if (ra8_cgc_init() != k_ra8_ok) {
    demo_panic_halt();
  }

  /* Bring up PLL2 -> USBCKCR / USBCKDIVCR so USBFS sees a spec-compliant
   * 48 MHz reference (PLL2P 240 MHz / 5). Must run BEFORE any caller
   * releases MSTPB11 (USBFS) -- the SREQ -> SRDY handshake silently
   * hangs otherwise (HUM Ch 9 "Clock selection switching procedure"
   * step 1). Without this the SIE never sees a 48 MHz clock and the
   * host never enumerates the device. The init-order audit
   * (scripts/checks/audit_init_order.py) requires CGC bring-up to
   * land BEFORE peripheral inits like ra8_log_init, so the RTT
   * heart-beat moves down to right after the time/board bring-up. */
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
  ra8_log_init();
  ra8_log_info("USBCDC", "usb_cdc_echo boot, CGC PLL2 enable OK");

  ra8_isr_globals_enable();

#ifndef RA8_OFF_TARGET
  /* tx_kernel_enter is __noreturn -- it never comes back. */
  tx_kernel_enter();
#endif

  demo_panic_halt();
}
