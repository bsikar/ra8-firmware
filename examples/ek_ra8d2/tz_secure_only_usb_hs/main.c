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
 * @var s_probe_thread
 * @brief Diagnostic thread: 1 Hz USBREQ snapshot ring (no INTSTS0 W0C).
 *
 * @details
 * Companion to the "disable polled DCD dispatch worker" diagnostic at
 * HEAD 447693253. Once per second this thread invokes
 * ``ra_usb_read_setup_unconditional`` and stores the resulting
 * ``bm_request_type:b_request`` packed value in the
 * ::s_usbreq_observed ring. ``ra_usb_read_setup_unconditional`` only
 * reads USBREQ / USBVAL / USBINDX / USBLENG (HUM Ch 37.2.21..24
 * p 2087..2090) and W0C-clears INTSTS0.VALID once at the very end --
 * but the captured-SETUP latch survives that clear, so this is the
 * lightest-touch read available from the HAL. We deliberately do NOT
 * touch INTSTS1, BCHG, BEMP, DVSTCTR0, FRMNUM, etc.
 *
 * @note Single-writer (this thread).
 * @since 0.1.0
 */
static TX_THREAD s_probe_thread;

/**
 * @var s_probe_stack
 * @brief Stack backing storage for ::s_probe_thread.
 * @since 0.1.0
 */
static UCHAR s_probe_stack[k_demo_thread_stack];

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
 * @enum probe_ring_t
 * @brief Sizing constants for ::s_usbreq_observed ring buffer.
 */
typedef enum : uint32_t {
  k_probe_ring_depth = 8U, /**< Most-recent N samples kept. */
} probe_ring_t;

/**
 * @var s_usbreq_observed
 * @brief 1 Hz ring of (bm_request_type | (b_request << 8)) snapshots.
 *
 * @details
 * Bisect probe for the "polled DCD dispatch worker disturbs SIE" test
 * at HEAD 447693253. With the polled worker disabled the chip should
 * auto-handle Reset -> chirp -> Default state, and the host's SETUP
 * should land in the captured-SETUP latch (USBREQ / USBVAL / USBINDX /
 * USBLENG, HUM Ch 37.2.21..24 p 2087..2090). We sample once per
 * second from a separate ThreadX thread; if any entry shows 0x0680
 * (GET_DESCRIPTOR DEVICE: bmRequestType=0x80, bRequest=0x06) we have
 * isolated the regression to the polled worker. If every entry stays
 * 0x0000 the issue is upstream of the worker.
 *
 * @note Single-writer (probe thread); read via JLink only.
 * @since 0.1.0
 */
volatile uint16_t s_usbreq_observed[k_probe_ring_depth] = {};

/**
 * @var s_usbreq_observed_idx
 * @brief Next write index into ::s_usbreq_observed (mod k_probe_ring_depth).
 * @note Single-writer (probe thread); read via JLink only.
 * @since 0.1.0
 */
volatile uint32_t s_usbreq_observed_idx = 0U;

/**
 * @var s_probe_iter
 * @brief Probe-thread iteration counter (one tick per second).
 * @note Single-writer (probe thread); read via JLink only.
 * @since 0.1.0
 */
volatile uint32_t s_probe_iter = 0U;

/**
 * @var s_probe_last_status
 * @brief Last ::ra_usb_read_setup_unconditional return value (raw).
 * @note Single-writer (probe thread); read via JLink only.
 * @since 0.1.0
 */
volatile uint32_t s_probe_last_status = 0U;

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

  /* DIAGNOSTIC (HEAD 447693253 follow-up): skip _ux_system_initialize,
   * _ux_device_stack_initialize, _ux_device_stack_class_register, and
   * ux_dcd_ra_usb_initialize entirely. Hypothesis: the polled DCD
   * dispatch worker reads INTSTS0 every tick and W0C-acks bits the
   * SIE needs for proper enumeration. With USBX disabled the chip
   * should auto-handle USB Reset -> chirp -> Default state and the
   * host's SETUP, if it arrives, lands in USBREQ regs without our
   * worker disturbing the SIE. (void)-cast the activate / deactivate
   * callbacks and the descriptor / framework arrays so the unused-
   * function / unused-variable diagnostics stay quiet under -Werror. */
  (void)demo_cdc_activate;
  (void)demo_cdc_deactivate;
  (void)s_device_framework_fs;
  (void)s_string_framework;
  (void)s_language_id_framework;
  (void)s_usbx_pool;
  (void)s_cdc_acm;
  (void)s_cdc_active_sem;

  /* Bring the USBHS controller up: PHY 12 MHz reference + MSTP ungate
   * + SYSCFG.SCKE/USBE/HSE. ra_board_usbhs_device_init wraps all three
   * (libs/ra_board_ek_ra8d2/src/ra_board_ek_ra8d2.c::
   * ra_board_usbhs_device_init -> internal_usbhs_clock_and_mstp ->
   * ra_usb_device_init(k_ra_usb_speed_hs)). */
  s_boot_probe = (uint32_t)k_boot_probe_pre_board_usbhs_init;
  if (ra_board_usbhs_device_init() != k_ra_ok) {
    return;
  }
  s_boot_probe = (uint32_t)k_boot_probe_post_board_usbhs_init;

  /* Raise DPRPU + CNEN (D+ pull-up) so the host begins enumeration. */
  s_boot_probe = (uint32_t)k_boot_probe_pre_dev_attach;
  if (ra_usb_device_attach(k_ra_usb_speed_hs, true) != k_ra_ok) {
    return;
  }
  s_boot_probe = (uint32_t)k_boot_probe_post_dev_attach;

  /* Bisect probes captured ONCE after attach. HUM Ch 37.2.1 SYSCFG p
   * 2060, HUM Ch 37.2.43 LPSTS p 2111, HUM Ch 51.8.1 PSARB p 3284,
   * HUM Ch 51.8.6 PPARB p 3292. */
  s_syscfg_in_echo_loop                  = ra_usb_hs()->SYSCFG;
  s_lpsts_in_echo_loop                   = *ra_usbhs_lpsts();
  volatile const uint32_t* const psarb_p = (volatile const uint32_t*)0x40204004UL;
  volatile const uint32_t* const pparb_p = (volatile const uint32_t*)0x4020401CUL;
  s_psar_state                           = *psarb_p;
  s_ppar_state                           = *pparb_p;

  s_boot_probe = (uint32_t)k_boot_probe_enter_echo_loop;

  /* Just hold the chip in attached state -- no INTSTS0 polling, no
   * INTSTS1 polling, no W0C anywhere. Sleep 10 s at a time so this
   * thread effectively never wakes up. The probe thread (1 Hz) does
   * the only register reads. */
  while (1) {
    s_demo_diag.loop_iter++;
    tx_thread_sleep(1000U); /* 10 s @ 100 Hz tick. */
  }
}

/**
 * @brief Diagnostic probe thread: 1 Hz USBREQ snapshot ring.
 *
 * @param[in] arg Unused (ThreadX entry signature).
 *
 * @pre ::s_probe_thread was created by ::tx_application_define.
 * @pre ::ra_usb_device_init has run for HS (worker thread completed
 *      ::ra_board_usbhs_device_init before any wake-up of this
 *      thread is meaningful; if not, ::ra_usb_read_setup_unconditional
 *      simply returns the current USBREQ value, which will read 0
 *      until the worker has armed the controller).
 *
 * @post ::s_usbreq_observed[s_usbreq_observed_idx] receives one new
 *       sample per ~1 s.
 *
 * @note Single-instance probe thread; not re-entrant.
 * @since 0.1.0
 */
static VOID demo_probe_thread(ULONG arg)
{
  (void)arg;
  while (1) {
    tx_thread_sleep(100U); /* 1 s @ 100 Hz tick. */
    s_probe_iter++;
    ra_usb_setup_t setup = {};
    ra_err_t       rc    = ra_usb_read_setup_unconditional(k_ra_usb_speed_hs, &setup);
    s_probe_last_status  = (uint32_t)rc;
    uint16_t packed = (uint16_t)setup.bm_request_type | (uint16_t)((uint16_t)setup.b_request << 8U);
    uint32_t i      = s_usbreq_observed_idx % (uint32_t)k_probe_ring_depth;
    s_usbreq_observed[i]  = packed;
    s_usbreq_observed_idx = (s_usbreq_observed_idx + 1U) % (uint32_t)k_probe_ring_depth;
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
  (void)tx_thread_create(&s_probe_thread,
                         "usbhs_probe_1hz",
                         demo_probe_thread,
                         0UL,
                         s_probe_stack,
                         k_demo_thread_stack,
                         9U, /* priority (lower than worker)  */
                         9U, /* preempt threshold              */
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
   * controller. PD07 (the J7 host/device role-select strap, UM 6.2 p
   * 34) is driven explicitly low later inside
   * ra_board_usbhs_device_init -> internal_usbhs_role_select_device.
   * So only P4_08 needs routing here. */
  return ra_pfs_route_peripheral(k_demo_pin_hs_vbus, k_ra_psel_usb_hs, "usb_cdc_hs.vbus");
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

  /* Note: ra_cgc_usbhs_pll_enable() is invoked later from
   * ra_board_usbhs_device_init() inside the worker thread, so we do
   * NOT need to enable USBFS PLL2 here (this app does not use USBFS).
   * The HS PHY 12 MHz reference is derived from the main XTAL via
   * USB60CKCR, set by ra_cgc_usbhs_pll_enable. */

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
