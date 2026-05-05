/**
 * @file examples/ek_ra8d2/tz_secure_only_usb_hs/main.c
 * @brief Secure-world-only ThreadX + USBX CDC ACM echo for EK-RA8D2 (USB-HS)
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * High-Speed sibling of ``tz_secure_only_usb`` (USB-FS on J11). This
 * variant brings up the USB 2.0 High-Speed controller (USBHS @
 * 0x40351000, HUM Ch 37) so the EK-RA8D2 USB-C receptacle (J7)
 * enumerates as a CDC-ACM device on the host. The class layer, the
 * USBX bridge in ``port/usbx/ux_dcd_ra_usb.c`` and the register-level
 * driver in ``libs/ra_hal/src/ra_usb.c`` are already speed-
 * parameterised; this app simply selects ``k_ra_usb_speed_hs`` and
 * uses the HS-specific board bring-up entry
 * ``ra_board_usbhs_device_init`` (which arms the PHY 12 MHz reference
 * via ``ra_cgc_usbhs_pll_enable`` and ungates MSTPB12 USBHS before
 * calling ``ra_usb_device_init(k_ra_usb_speed_hs)``).
 *
 * Once enumerated, the worker thread loops on
 * ``_ux_device_class_cdc_acm_read`` -> ``_ux_device_class_cdc_acm_write``
 * (echo). LED1 toggles per byte echoed.
 *
 * ## Pinout (USB-HS, EK-RA8D2 v1 User's Manual Rev 1.01 sec 6.2 p 34)
 *
 * The HS PHY data lines (USBH_P / USBH_N / USBHSRREF) are dedicated
 * package balls on the BGA and bypass the PFS PSEL path entirely.
 * Only one PFS-muxed pin needs routing for HS device-mode operation:
 *
 * | Net           | Pin    | PFS PSEL                |
 * |---------------|--------|-------------------------|
 * | USBHS_VBUS    | P4_08  | k_ra_psel_usb_hs (0x14) |
 *
 * The board's J7 role-select GPIO (PD07, set LOW for device mode) is
 * pulled LOW by default on the EK-RA8D2 v1 -- the firmware does not
 * have to drive it. VBUS / VBUSEN / OVRCUR are sourced from the
 * on-board USB-PD controller, not from RA8D2 port pins, so no further
 * GPIO setup is required.
 *
 * ## Sequence
 *
 *   1. ``ra_cgc_init()`` -- standard FSP-quickstart clock tree.
 *   2. ``ra_time_init`` for back-off delays.
 *   3. ``ra_pfs_route_peripheral`` for P4_08 -> USBHS_VBUS.
 *   4. ``ra_board_led_init(k_ra_board_led1)`` for visual heartbeat.
 *   5. ThreadX ``tx_kernel_enter()`` -- spins the scheduler.
 *   6. ``tx_application_define`` -- spawns one worker thread that:
 *        - Allocates USBX memory pool and calls
 *          ``_ux_system_initialize`` + ``_ux_device_stack_initialize``.
 *        - Calls ``_ux_device_stack_class_register`` for the CDC-ACM
 *          class.
 *        - Calls ``ra_board_usbhs_device_init()`` to arm the HS PHY
 *          clock + MSTP and bring up the USBHS controller.
 *        - Calls ``ux_dcd_ra_usb_initialize(k_ra_usb_speed_hs)`` to
 *          plug our DCD bridge into USBX (which announces the
 *          highest-speed class as ``UX_HIGH_SPEED_DEVICE``).
 *        - Calls ``ra_usb_device_attach(k_ra_usb_speed_hs, true)`` so
 *          the host begins enumeration.
 *        - Drops into the echo loop.
 *
 * ## Verification (macOS)
 *
 * After flashing, the EK-RA8D2's USB-HS receptacle (J7) enumerates
 * as ``/dev/cu.usbmodem*``. Open it RDWR with picocom or screen and
 * type characters; every byte echoes back and LED1 toggles per byte.
 *
 * @author Brighton Sikarskie
 * @date 2026-05-03
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>
#include <string.h>

#include "ra8d2_usb_regs.h"
#include "ra_board_ek_ra8d2.h"
#include "ra_cgc.h"
#include "ra_err.h"
#include "ra_gpio_constants.h"
#include "ra_isr.h"
#include "ra_log.h"
#include "ra_port_constants.h"
#include "ra_port_utils.h"
#include "ra_time.h"
#include "ra_usb.h"

/**
 * @var s_demo_tag
 * @brief Log tag for this experiment's diagnostics on SCI8 / RTT.
 * @note File-scope, read-only after init.
 * @since 0.1.0
 */
static const char* s_demo_tag = "TZSECONLYHS";

#ifndef RA_SIMULATOR_MODE
#include "tx_api.h"
#include "ux_api.h"
#include "ux_dcd_ra_usb.h"
#include "ux_device_class_cdc_acm.h"
#include "ux_device_stack.h"
#include "ux_system.h"
#endif

/* -------------------------------------------------------------------------- */
/* Pinout (USBHS, EK-RA8D2 v1 User's Manual Rev 1.01 sec 6.2 p 34)            */
/* -------------------------------------------------------------------------- */

/**
 * @brief USBHS_VBUS sense pin (P4_08), packed ``ra_port_pin_t``.
 *
 * @details
 * Built as a runtime cast so clang-tidy's enum-range check is happy
 * with the otherwise out-of-enum value. Cross-checked against the FSP
 * example ``ra-fsp-examples/example_projects/ek_ra8d2/usb_hcdc/``
 * which sets ``p408.usbhs.usbhs_vbus`` and nothing else for USB-HS.
 *
 * @since 0.1.0
 */
static const ra_port_pin_t k_demo_pin_hs_vbus =
  (ra_port_pin_t)(((uint16_t)k_ra_port_4 << 8) | (uint16_t)k_ra_pin_8);

/**
 * @brief J7 USB-HS role-select strap (PD07), packed ``ra_port_pin_t``.
 *
 * @details
 * EK-RA8D2 v1 UM Rev 1.01 Section 6.2 p 34: PD07 (port 13 / pin 7) is
 * the J7 USB-HS role select line. Driving it LOW selects Device mode;
 * driving it HIGH selects Host mode. The board does not pull this pin
 * to any default level by hardware -- the firmware MUST own it.
 *
 * Built as a runtime cast so clang-tidy's enum-range check is happy
 * with the otherwise out-of-enum value.
 *
 * @since 0.1.0
 */
static const ra_port_pin_t k_demo_pin_pd07_role =
  (ra_port_pin_t)(((uint16_t)k_ra_port_13 << 8) | (uint16_t)k_ra_pin_7);

/* -------------------------------------------------------------------------- */
/* Tunables                                                                   */
/* -------------------------------------------------------------------------- */

/**
 * @enum demo_config_t
 * @brief Compile-time settings for the echo loop and ThreadX worker.
 */
typedef enum : uint32_t {
  k_demo_thread_stack    = 8192U,  /**< Worker thread stack (bytes).        */
  k_demo_usbx_pool_bytes = 16384U, /**< USBX memory pool (bytes).           */
  k_demo_echo_buf_bytes  = 64U,    /**< One bulk packet per recv/send. HS
                                        bulk MPS could go to 512 but 64
                                        keeps first-pass parity with the
                                        FS demo. */
  k_demo_idle_ticks      = 1U,     /**< Idle back-off when no class active. */
} demo_config_t;

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
 * @var s_intenb0_watchdog_thread
 * @brief ThreadX TCB for the INTENB0 re-arm watchdog.
 *
 * @details
 * USB Bus Reset on the HS controller auto-clears INTENB0 to 0. The
 * driver's ``busreset_rearm`` is supposed to re-set INTENB0 = 0xFF00
 * from the ISR's DVST(=Default) handler, but the ISR cannot fire while
 * INTENB0 is zero (chicken-and-egg). This dedicated low-priority
 * thread polls INTENB0 every ThreadX tick and re-asserts the device-
 * mode interrupt mask whenever the IP has cleared it. With the mask
 * back in place, the next bus event raises USBI0 and the ISR fires.
 *
 * @note Single-writer (this thread); read via JLink for diagnostics.
 * @since 0.1.0
 */
static TX_THREAD s_intenb0_watchdog_thread;

/**
 * @var s_intenb0_watchdog_stack
 * @brief Stack backing storage for the INTENB0 watchdog thread.
 * @since 0.1.0
 */
static UCHAR s_intenb0_watchdog_stack[1024U];

/**
 * @var s_intenb0_rearm_count
 * @brief Number of times the watchdog has re-asserted INTENB0 = 0xFF00.
 *
 * @details
 * Increments once per polling tick where ``INTENB0 != 0xFF00`` was
 * observed. A non-zero value here proves the watchdog is firing and
 * that the IP is clearing INTENB0 (typically across a USB Bus Reset).
 * Read via JLink: ``mem32 &s_intenb0_rearm_count``.
 *
 * @note Single-writer (watchdog thread); read-only elsewhere.
 * @since 0.1.0
 */
volatile uint32_t s_intenb0_rearm_count = 0U;

/**
 * @var s_intenb0_watchdog_started
 * @brief Set to 1 once the watchdog thread has begun polling.
 * @note Single-writer (watchdog thread).
 * @since 0.1.0
 */
volatile uint32_t s_intenb0_watchdog_started = 0U;

/**
 * @var s_host_kick_done
 * @brief Set to 1 once the host-mode PHY analog kick has been applied.
 *
 * @details
 * Some Renesas USB-OTG PHYs latch into a stuck analog state after
 * power-on if the controller never enters host mode. The mitigation
 * is to briefly assert ``SYSCFG.DCFM`` + ``SYSCFG.DRPD`` (host + bus
 * pull-down), wait, then clear them again before completing the
 * device-mode bring-up. ``s_host_kick_done`` is incremented after the
 * kick sequence completes so a JLink session can confirm the path
 * was taken: ``mem32 &s_host_kick_done`` should read 1 after boot.
 *
 * @note Single-writer (worker thread); read-only elsewhere.
 * @since 0.1.0
 */
volatile uint32_t s_host_kick_done = 0U;

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
  volatile uint32_t loop_iter;
  volatile uint32_t loop_cdc_null;
  volatile uint32_t loop_pre_read;
  volatile uint32_t loop_post_read;
  volatile uint32_t loop_read_ok;
  volatile uint32_t loop_read_zero;
  volatile uint32_t loop_pre_write;
  volatile uint32_t loop_post_write;
} demo_diag_t;

/**
 * @var s_demo_diag
 * @brief Externally-readable counters for demo loop progress.
 * @note Increment-only; never cleared at runtime.
 * @since 0.1.0
 */
volatile demo_diag_t s_demo_diag = {};

/**
 * @var s_boot_probe
 * @brief Temporary bisect probe for USBHS bring-up HardFault.
 *
 * @details
 * Stepped through every major call in demo_worker so a JLink session
 * can read the last value reached and localise which call faulted.
 * Values: 1=thread entry, 2=pre _ux_system_initialize, 3=pre
 * _ux_device_stack_initialize, 4=pre _ux_device_stack_class_register,
 * 5=pre ra_board_usbhs_device_init, 7=post that, 8=pre
 * ux_dcd_ra_usb_initialize, 9=post, 10=pre ra_usb_device_attach,
 * 11=post, 12=entering echo while(1). Remove once the offending call
 * is identified.
 *
 * @note File-scope, single-writer (worker thread).
 * @since 0.1.0
 */
static volatile uint32_t s_boot_probe = 0U;

/**
 * @var s_syscfg_in_echo_loop
 * @brief SYSCFG snapshot taken on the FIRST iteration of the echo loop.
 *
 * @details Bisect probe (HUM Ch 37.2.1 SYSCFG p 2060) for the "USBE
 * clears between phy bring-up and echo loop" regression. Captured
 * exactly once -- before the loop body has had a chance to do any
 * USBX work -- so a JLink session can compare it against
 * ::s_syscfg_after_attach to localise whether anything between
 * ra_usb_device_attach and the echo-loop entry clears USBE.
 *
 * @note File-scope, single-writer (worker thread).
 * @since 0.1.0
 */
volatile uint16_t s_syscfg_in_echo_loop = 0U;

/**
 * @var s_lpsts_in_echo_loop
 * @brief LPSTS snapshot on first echo-loop iteration.
 * @details Companion to ::s_syscfg_in_echo_loop; HUM Ch 37.2.43 LPSTS
 * p 2111. Expected SUSPENDM=1 (0x4000).
 * @since 0.1.0
 */
volatile uint16_t s_lpsts_in_echo_loop = 0U;

/**
 * @var s_psar_state
 * @brief Snapshot of R_PSCU->PSARB on entry to the echo loop.
 *
 * @details
 * PSCU base = 0x40204000, PSARB offset 0x04 (HUM Ch 51.8.1
 * "PSARB : Peripheral Security Attribution Register B" p 3284).
 * Bit 12 (PSARB12) is the USBHS Type1 secure/non-secure bit; bit 11
 * (PSARB11) is the USBFS0 equivalent. 0 = Secure, 1 = Non-secure.
 * Reset value = 0x00000000 -- both modules default to Secure, which
 * is what this no-op-TrustZone build wants. We capture it once at
 * the echo-loop entry so a JLink session can verify nothing else
 * (e.g. ThreadX init or the USBX class layer) flipped PSARB12 to NS
 * out from under us.
 *
 * @note Single-writer (worker thread); read via JLink only.
 * @since 0.1.0
 */
volatile uint32_t s_psar_state = 0U;

/**
 * @var s_ppar_state
 * @brief Snapshot of R_PSCU->PPARB on entry to the echo loop.
 *
 * @details
 * PSCU base = 0x40204000, PPARB offset 0x1C (HUM Ch 51.8.6
 * "PPARB : Peripheral Privilege Attribution Register B" p 3292).
 * Bit 12 (PPARB12) is the USBHS Type1 privileged/unprivileged bit:
 * 0 = Privileged-only access, 1 = Unprivileged access permitted.
 * Reset value = 0xFFFFFFFF -- USBHS defaults to "unprivileged access
 * permitted", which means both privileged-secure and unprivileged-
 * secure register accesses are allowed by the TrustZone Filter.
 *
 * @note Single-writer (worker thread); read via JLink only.
 * @since 0.1.0
 */
volatile uint32_t s_ppar_state = 0U;

/**
 * @enum boot_probe_step_t
 * @brief Bisect-probe step values for s_boot_probe.
 */
typedef enum : uint32_t {
  k_boot_probe_thread_entry          = 1U,  /**< worker entry.                 */
  k_boot_probe_pre_sys_init          = 2U,  /**< before _ux_system_initialize. */
  k_boot_probe_pre_dev_stack_init    = 3U,  /**< before _ux_device_stack_init. */
  k_boot_probe_pre_class_register    = 4U,  /**< before _ux_device_stack_class_register. */
  k_boot_probe_pre_board_usbhs_init  = 5U,  /**< before ra_board_usbhs_device_init. */
  k_boot_probe_post_board_usbhs_init = 7U,  /**< after ra_board_usbhs_device_init. */
  k_boot_probe_pre_ux_dcd_init       = 8U,  /**< before ux_dcd_ra_usb_initialize. */
  k_boot_probe_post_ux_dcd_init      = 9U,  /**< after ux_dcd_ra_usb_initialize. */
  k_boot_probe_pre_dev_attach        = 10U, /**< before ra_usb_device_attach.  */
  k_boot_probe_post_dev_attach       = 11U, /**< after ra_usb_device_attach.   */
  k_boot_probe_enter_echo_loop       = 12U, /**< entering echo while(1).       */
} boot_probe_step_t;

/**
 * @var s_cdc_active_sem
 * @brief Posted by demo_cdc_activate; demo thread blocks on it instead
 *        of polling s_cdc_acm with tx_thread_sleep.
 * @note Single-producer (USBX class thread), single-consumer (demo).
 * @since 0.1.0
 */
static TX_SEMAPHORE s_cdc_active_sem;

/* -------------------------------------------------------------------------- */
/* USB descriptors (DEVICE + CONFIG + IAD + CDC interfaces + endpoints)       */
/* -------------------------------------------------------------------------- */

/* Same CDC-ACM composite descriptor set as the FS demo: 64-byte bulk
 * MPS keeps EP1/EP2 packets identical between the FS and HS variants
 * (HS allows up to 512 bytes per bulk packet but 64 is legal and
 * minimises descriptor churn during this first-pass HS bring-up). */
static UCHAR s_device_framework_fs[] = {
  /* Device descriptor (USB 2.0 sec 9.6.1) -- 18 bytes. */
  0x12U,
  0x01U,
  0x00U,
  0x02U,
  0xEFU,
  0x02U,
  0x01U,
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
  /* Configuration descriptor (75 bytes total). */
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
  /* CDC header functional descriptor. bcdCDC = 0x0120 (CDC 1.20). */
  0x05U,
  0x24U,
  0x00U,
  0x20U,
  0x01U,
  /* Call-management functional descriptor. */
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
  /* idx 2: "EK-RA8D2 HS CDC Echo" (20 ASCII bytes). */
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
  'H',
  'S',
  ' ',
  'C',
  'D',
  'C',
  ' ',
  'E',
  'c',
  'h',
  'o',
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
  '2',
};

/**
 * @var s_language_id_framework
 * @brief USBX language-id table -- US English.
 * @since 0.1.0
 */
static UCHAR s_language_id_framework[] = {0x09U, 0x04U};

/* Forward declarations -------------------------------------------------- */

static VOID intenb0_watchdog_entry(ULONG arg);

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
  if (_ux_system_slave != UX_NULL) {
    _ux_system_slave->ux_system_slave_device.ux_slave_device_state =
      (unsigned long)UX_DEVICE_CONFIGURED;
  }
  (void)tx_semaphore_put(&s_cdc_active_sem);
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

  s_boot_probe = (uint32_t)k_boot_probe_thread_entry;
  /* Bring USBX up. */
  s_boot_probe = (uint32_t)k_boot_probe_pre_sys_init;
  if (_ux_system_initialize(s_usbx_pool, k_demo_usbx_pool_bytes, UX_NULL, 0) != UX_SUCCESS) {
    return;
  }
  s_boot_probe = (uint32_t)k_boot_probe_pre_dev_stack_init;
  /* Pass our composite framework as both the FS and HS slot would --
   * the bridge announces UX_HIGH_SPEED_DEVICE and USBX accepts the
   * single framework as long as bcdUSB == 0x0200. */
  /* Pass the same descriptor framework for both speed slots: bcdUSB
   * = 0x0200, EP1/EP2 are bulk, and the bridge announces
   * UX_HIGH_SPEED_DEVICE. USBX picks the matching slot when SET_CONFIG
   * lands; passing only FS would leave the HS slot empty and the
   * chapter-9 dispatcher cannot answer GET_DESCRIPTOR(DEVICE) on HS. */
  if (_ux_device_stack_initialize(s_device_framework_fs,
                                  sizeof(s_device_framework_fs),
                                  s_device_framework_fs,
                                  sizeof(s_device_framework_fs),
                                  s_string_framework,
                                  sizeof(s_string_framework),
                                  s_language_id_framework,
                                  sizeof(s_language_id_framework),
                                  UX_NULL) != UX_SUCCESS) {
    return;
  }

  /* Register CDC-ACM class against the configuration. */
  UX_SLAVE_CLASS_CDC_ACM_PARAMETER cdc_params = {
    .ux_slave_class_cdc_acm_instance_activate   = demo_cdc_activate,
    .ux_slave_class_cdc_acm_instance_deactivate = demo_cdc_deactivate,
    .ux_slave_class_cdc_acm_parameter_change    = UX_NULL,
  };
  s_boot_probe = (uint32_t)k_boot_probe_pre_class_register;
  if (_ux_device_stack_class_register((UCHAR*)"ux_slave_class_cdc_acm",
                                      _ux_device_class_cdc_acm_entry,
                                      1, /* configuration #  */
                                      0, /* interface #      */
                                      &cdc_params) != UX_SUCCESS) {
    return;
  }

  /* USBHS controller bring-up is now performed exactly once, inside
   * ux_dcd_ra_usb_initialize below (which calls ra_usb_device_init
   * itself). The PHY clock was armed in main() via
   * ra_cgc_usbhs_pll_enable; PD07 role-select is owned by demo_pins_init.
   * MSTPB12 ungate happens inside ra_usb_device_init via ra_mstp_enable.
   *
   * Previously this site called ra_board_usbhs_device_init() which also
   * invoked ra_usb_device_init(hs), and ux_dcd_ra_usb_initialize then
   * invoked ra_usb_device_init(hs) AGAIN. The duplicate PHY bring-up
   * (HSE -> PLL CLKSEL bisect -> common init) re-cleared INTENB0 and
   * left the controller in a state where chirp completed but the host
   * never issued SETUP -- exactly the failure signature observed on
   * J7. The FS demo never had this duplicate call, which is consistent
   * with FS enumerating fine on the same hardware. */
  s_boot_probe = (uint32_t)k_boot_probe_post_board_usbhs_init;

  /* PHY analog "host-mode kick" + chip-level SYSCFG reset (DISABLED).
   *
   * Originally added when investigating the "host completes chirp but
   * never sends SETUP" symptom. We left the block in place but
   * compile-time disabled it via ``#if 0`` once the more likely root
   * cause -- duplicate ra_usb_device_init() calls -- was identified
   * and removed above. The kick wrote SYSCFG=0 and then re-asserted
   * SCKE/HSE/USBE which clobbered every register the (now-removed)
   * ra_board_usbhs_device_init had just programmed; running it after
   * ux_dcd_ra_usb_initialize would clobber that init too, and running
   * it BEFORE that init is redundant because ra_usb_device_init drives
   * SYSCFG itself.
   *
   * Sequence (HUM Ch 37.2.1 SYSCFG p 2060):
   *   1. Snapshot DPRPU/HSE/USBE/SCKE bits we want to preserve.
   *   2. Set DCFM=1, DRPD=1, clear DPRPU; keep USBE/SCKE/HSE intact.
   *   3. Sleep 2 ticks (~20 ms) so the analog block settles in host
   *      mode and the bus pull-downs are sampled.
   *   4. Chip-level reset: write SYSCFG=0 to drop USBE+everything,
   *      then re-assert SCKE, HSE, USBE in the original device-mode
   *      configuration with DPRPU still cleared (attach happens later
   *      via ra_usb_device_attach which sets DPRPU).
   *   5. Sleep 1 tick (~10 ms) to let the PHY relock.
   */
  /* Host-mode kick disabled -- see comment above. The variable is kept
   * so JLink scripts that read it still link; it is now always 0. */
  s_host_kick_done = 0U;

  /* Plug our DCD bridge into the device stack and turn the bus on. */
  s_boot_probe = (uint32_t)k_boot_probe_pre_ux_dcd_init;
  if (ux_dcd_ra_usb_initialize(k_ra_usb_speed_hs) != k_ra_ok) {
    return;
  }
  s_boot_probe = (uint32_t)k_boot_probe_post_ux_dcd_init;
  s_boot_probe = (uint32_t)k_boot_probe_pre_dev_attach;
  if (ra_usb_device_attach(k_ra_usb_speed_hs, true) != k_ra_ok) {
    return;
  }
  s_boot_probe = (uint32_t)k_boot_probe_post_dev_attach;

  /* Spawn the INTENB0 re-arm watchdog. USB Bus Reset on the RA8D2 HS
   * controller auto-clears INTENB0 to 0 (verified live via JLink: a
   * manual write of INTENB0 = 0xFF00 immediately resumed ISR firing
   * after a host bus reset). The driver's ``busreset_rearm`` is
   * supposed to do this from the DVST(=Default) ISR path, but the ISR
   * cannot run while INTENB0 is zero. This polled watchdog breaks the
   * chicken-and-egg by re-writing INTENB0 outside the ISR. */
  (void)tx_thread_create(&s_intenb0_watchdog_thread,
                         "usbhs_intenb0_wdog",
                         intenb0_watchdog_entry,
                         0UL,
                         s_intenb0_watchdog_stack,
                         (ULONG)sizeof(s_intenb0_watchdog_stack),
                         15U, /* lower priority than echo worker  */
                         15U,
                         TX_NO_TIME_SLICE,
                         TX_AUTO_START);

  /* Echo loop. */
  s_boot_probe = (uint32_t)k_boot_probe_enter_echo_loop;
  /* Bisect probes captured ONCE on entry to the echo loop. HUM
   * Ch 37.2.1 SYSCFG p 2060, HUM Ch 37.2.43 LPSTS p 2111, HUM
   * Ch 51.8.1 PSARB p 3284, HUM Ch 51.8.6 PPARB p 3292. */
  s_syscfg_in_echo_loop                  = ra_usb_hs()->SYSCFG;
  s_lpsts_in_echo_loop                   = *ra_usbhs_lpsts();
  volatile const uint32_t* const psarb_p = (volatile const uint32_t*)0x40204004UL;
  volatile const uint32_t* const pparb_p = (volatile const uint32_t*)0x4020401CUL;
  s_psar_state                           = *psarb_p;
  s_ppar_state                           = *pparb_p;
  UCHAR buf[k_demo_echo_buf_bytes];
  ULONG n = 0UL;
  while (1) {
    s_demo_diag.loop_iter++;
    if (s_cdc_acm == UX_NULL) {
      s_demo_diag.loop_cdc_null++;
      (void)tx_semaphore_get(&s_cdc_active_sem, TX_WAIT_FOREVER);
      continue;
    }
    if (_ux_system_slave != UX_NULL) {
      _ux_system_slave->ux_system_slave_device.ux_slave_device_state =
        (unsigned long)UX_DEVICE_CONFIGURED;
    }
    s_demo_diag.loop_pre_read++;
    UINT read_status = _ux_device_class_cdc_acm_read(s_cdc_acm, buf, sizeof(buf), &n);
    s_demo_diag.loop_post_read++;
    if (read_status != UX_SUCCESS) {
      tx_thread_sleep(k_demo_idle_ticks);
      continue;
    }
    s_demo_diag.loop_read_ok++;
    if (n == 0UL) {
      s_demo_diag.loop_read_zero++;
      continue;
    }
    s_demo_diag.loop_pre_write++;
    if (_ux_device_class_cdc_acm_write(s_cdc_acm, buf, n, &n) != UX_SUCCESS) {
      continue;
    }
    s_demo_diag.loop_post_write++;
    for (ULONG i = 0UL; i < n; i++) {
      (void)ra_board_led_toggle(k_ra_board_led1);
    }
  }
}

/* -------------------------------------------------------------------------- */
/* INTENB0 re-arm watchdog                                                    */
/* -------------------------------------------------------------------------- */

/**
 * @brief Watchdog entry: re-asserts USBHS INTENB0 = 0xFF00 if cleared.
 *
 * @param[in] arg Unused (ThreadX entry signature).
 *
 * @pre USBHS MSTP ungated and SYSCFG.USBE=1 (otherwise reads return 0).
 * @post Loops forever; never returns.
 *
 * @note Single-instance; reads/writes a single 16-bit MMIO register
 *       which is naturally atomic on Cortex-M.
 * @since 0.1.0
 */
static VOID intenb0_watchdog_entry(ULONG arg)
{
  (void)arg;
  volatile r_usb_regs_t* reg = ra_usb_hs();
  s_intenb0_watchdog_started = 1U;
  while (1) {
    if (reg->INTENB0 != (uint16_t)k_ra_int0_full_mask) {
      reg->INTENB0 = (uint16_t)k_ra_int0_full_mask;
      s_intenb0_rearm_count++;
    }
    tx_thread_sleep(1UL);
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
  (void)tx_semaphore_create(&s_cdc_active_sem, "cdc_active", 0U);
  (void)tx_thread_create(&s_demo_thread,
                         "usb_cdc_echo_hs",
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
 * @brief Route the USBHS_VBUS sense pin (P4_08) to the USBHS controller.
 *
 * @return Error from ra_pfs_route_peripheral, or k_ra_ok.
 * @retval k_ra_ok P4_08 is in USBHS peripheral mode (PSEL = 0x14).
 *
 * @pre IOPORT module is reachable.
 * @pre Single-threaded init context.
 * @post On success P4_08 PFS PSEL = 0x14, PMR = 1.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] static ra_err_t demo_pins_init(void)
{
  /* The HS PHY data lines (USBHSDP / USBHSDM / USBHSRREF) are
   * dedicated package balls on the BGA and bypass the PFS PSEL path.
   * VBUS / VBUSEN / OVRCUR are sourced by the on-board USB-PD
   * controller. So only P4_08 needs PFS routing for the controller. */
  ra_err_t err = ra_pfs_route_peripheral(k_demo_pin_hs_vbus, k_ra_psel_usb_hs, "usb_cdc_hs.vbus");
  if (err != k_ra_ok) {
    return err;
  }
  /* PD07 (J7 USB-HS role select, UM 6.2 p 34): drive LOW for Device
   * mode. Mirrors the FS demo's P5_00 / VBUSEN GPIO drive in shape:
   * the role line is owned by the application early so the analog
   * block sees a stable strap before the controller is brought up.
   * Previously this happened inside ra_board_usbhs_device_init from
   * the worker thread, but that caller also invoked ra_usb_device_init
   * which is then re-invoked by ux_dcd_ra_usb_initialize -- the
   * duplicate PHY bring-up is what kept INTENB0 from sticking and is
   * the most plausible reason the host enumerates FS but never sends
   * SETUP on HS. Owning PD07 here lets the worker drop the redundant
   * board init call entirely. */
  return ra_gpio_output_init(k_demo_pin_pd07_role, k_ra_level_low);
}

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmain"
/**
 * @brief Application entry. Brings up CGC + USB-HS pin + LED1 + ThreadX.
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

  if (ra_cgc_init() != k_ra_ok) {
    demo_panic_halt();
  }

  /* Bring up the USBHS PHY clock (USB60CKCR / USB60CLK = PLL2P / 4 =
   * 60 MHz) BEFORE any caller releases MSTPB12 (USBHS) -- mirrors the
   * FS demo's ra_cgc_usbfs_clock_enable() pattern. The bridge's
   * ra_usb_device_init invocation later releases MSTPB12 and immediately
   * starts the PHY-PLL CLKSEL bisect; without this clock arm step the
   * PHY block has no reference and PLLLOCK never asserts.
   *
   * Previously this lived inside ra_board_usbhs_device_init() called
   * from the worker, but that wrapper also called ra_usb_device_init
   * which is then re-invoked by ux_dcd_ra_usb_initialize -- the
   * duplicate PHY bring-up clobbered INTENB0 and is the most plausible
   * explanation for the "ISR fires but host never sends SETUP" symptom
   * on HS. With the clock armed here the worker can drop the wrapper
   * call entirely, making the HS demo's worker-side init sequence a
   * structural mirror of the working FS demo. */
  if (ra_cgc_usbhs_pll_enable() != k_ra_ok) {
    demo_panic_halt();
  }

  if (ra_cgc_get_clock_hz(k_ra_clock_id_cpuclk0, &cpuclk0_hz) != k_ra_ok) {
    demo_panic_halt();
  }
  if (ra_time_init(cpuclk0_hz) != k_ra_ok) {
    demo_panic_halt();
  }
  if (ra_board_led_init(k_ra_board_led1) != k_ra_ok) {
    demo_panic_halt();
  }
  if (demo_pins_init() != k_ra_ok) {
    demo_panic_halt();
  }
  ra_log_init();
  ra_log_info(s_demo_tag, "tz_secure_only_usb_hs boot, CGC OK, USBHS pin routed");

  ra_isr_globals_enable();

#ifndef RA_SIMULATOR_MODE
  /* tx_kernel_enter is __noreturn -- it never comes back. */
  tx_kernel_enter();
#endif

  demo_panic_halt();
  return 0;
}
#pragma GCC diagnostic pop
