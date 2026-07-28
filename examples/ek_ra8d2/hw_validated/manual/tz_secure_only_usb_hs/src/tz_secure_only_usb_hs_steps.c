/**
 * @file
 * examples/ek_ra8d2/hw_validated/manual/tz_secure_only_usb_hs/src/tz_secure_only_usb_hs_steps.c
 * @brief ThreadX + USBX worker machinery for the secure-only USB-HS echo app.
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * Sibling translation unit for
 * ``examples/ek_ra8d2/hw_validated/manual/tz_secure_only_usb_hs/main.c``.
 * Holds the CDC-ACM activate/deactivate callbacks, the USBX bring-up step
 * routines, the INTENB0 re-arm watchdog and the ThreadX
 * ``tx_application_define`` kernel hook. These were moved here verbatim
 * from ``main.c`` (a pure, behaviour-preserving code move) so that every
 * translation unit stays under the 1000-line ``check_file_size.py`` cap.
 * The four USBX descriptor byte arrays consumed by ``demo_worker_usbx_init``
 * live in the companion ``tz_secure_only_usb_hs_descriptors.c``.
 *
 * @author Brighton Sikarskie
 * @date 2026-05-03
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include "tz_secure_only_usb_hs_steps.h"

#include <stdint.h>
#include <string.h>

#include "ra8_board_ek_ra8d2.h"
#include "ra8_err.h"
#include "ra8_isr.h"
#include "ra8_usb.h"
#include "ra8_usb_regs.h"

/** @brief USBHS security/privilege attribution register addresses. */
typedef enum : uintptr_t {
  k_usbhs_psarb_addr = 0x40204004UL, /**< Peripheral Security Attribution B.  */
  k_usbhs_pparb_addr = 0x4020401CUL, /**< Peripheral Privilege Attribution B. */
} usbhs_sec_reg_addr_t;

typedef enum : uint32_t {
  k_demo_thread_stack    = 8192U,  /**< Worker thread stack (bytes).        */
  k_demo_usbx_pool_bytes = 16384U, /**< USBX memory pool (bytes).           */
  k_demo_echo_buf_bytes  = 512U,   /**< HS bulk MPS.                        */
  k_demo_idle_ticks      = 1U,     /**< Idle back-off when no class active. */
} demo_config_t;

#ifndef RA8_SIMULATOR_MODE
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
 * @note Read by worker; written by USBX class thread (ISR context).
 *       Must be volatile so the worker's tight polling loop re-reads
 *       memory each iteration and sees the ISR-side write.
 * @since 0.1.0
 */
/* Note the placement of `volatile`: we want the POINTER itself to be
 * volatile so the worker re-reads it from memory each loop iteration.
 * `volatile UX_SLAVE_CLASS_CDC_ACM*` would only make the pointee
 * volatile and the compiler is free to cache the NULL pointer value
 * in a register -- which is exactly the bug that masked the activate
 * write for the entire echo loop. */
static UX_SLAVE_CLASS_CDC_ACM* volatile s_cdc_acm = UX_NULL;

/* JLink-readable counters: how many times activate / deactivate ran. */
volatile uint32_t s_cdc_activate_count        = 0U;
volatile uint32_t s_cdc_activate_post_put     = 0U;
volatile uint32_t s_cdc_deactivate_count      = 0U;
volatile uint32_t s_pendsv_observed_run_count = 0U;

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
 * @var s_boot_probe
 * @brief Temporary bisect probe for USBHS bring-up HardFault.
 *
 * @details
 * Stepped through every major call in demo_worker so a JLink session
 * can read the last value reached and localise which call faulted.
 * Values: 1=thread entry, 2=pre _ux_system_initialize, 3=pre
 * _ux_device_stack_initialize, 4=pre _ux_device_stack_class_register,
 * 5=pre ra8_board_usbhs_device_init, 7=post that, 8=pre
 * ux_dcd_ra8_usb_initialize, 9=post, 10=pre ra8_usb_device_attach,
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
 * ra8_usb_device_attach and the echo-loop entry clears USBE.
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
  k_boot_probe_thread_entry          = 1U,  /**< worker entry.                           */
  k_boot_probe_pre_sys_init          = 2U,  /**< before _ux_system_initialize.           */
  k_boot_probe_pre_dev_stack_init    = 3U,  /**< before _ux_device_stack_init.           */
  k_boot_probe_pre_class_register    = 4U,  /**< before _ux_device_stack_class_register. */
  k_boot_probe_pre_board_usbhs_init  = 5U,  /**< before ra8_board_usbhs_device_init.     */
  k_boot_probe_post_board_usbhs_init = 7U,  /**< after ra8_board_usbhs_device_init.      */
  k_boot_probe_pre_ux_dcd_init       = 8U,  /**< before ux_dcd_ra8_usb_initialize.       */
  k_boot_probe_post_ux_dcd_init      = 9U,  /**< after ux_dcd_ra8_usb_initialize.        */
  k_boot_probe_pre_dev_attach        = 10U, /**< before ra8_usb_device_attach.           */
  k_boot_probe_post_dev_attach       = 11U, /**< after ra8_usb_device_attach.            */
  k_boot_probe_enter_echo_loop       = 12U, /**< entering echo while(1).                 */
} boot_probe_step_t;

/**
 * @var s_cdc_active_sem
 * @brief Posted by demo_cdc_activate; demo thread blocks on it instead
 *        of polling s_cdc_acm with tx_thread_sleep.
 * @note Single-producer (USBX class thread), single-consumer (demo).
 * @since 0.1.0
 */
static TX_SEMAPHORE s_cdc_active_sem;

/* Forward declarations -------------------------------------------------- */

static VOID intenb0_watchdog_entry(ULONG arg);

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
  if (_ux_system_slave != UX_NULL) {
    _ux_system_slave->ux_system_slave_device.ux_slave_device_state =
      (unsigned long)UX_DEVICE_CONFIGURED;
  }
  s_cdc_activate_count++;
  /* Enable bridge-side auto-echo for the CDC bulk pair (EP2 OUT -> EP1 IN).
   * The worker thread DOES get scheduled now (the IRQ-mask + SysTick re-
   * enable in ux_dcd_ra8_usb.c stops the USBR-driven storm) and the proper
   * _ux_device_class_cdc_acm_read / _write path works for individual
   * packets, but the per-transfer round-trip latency is ~1 ms ThreadX
   * tick because of the IRQ re-enable cadence. Auto-echo bypasses the
   * thread mode entirely and runs the mirror inside the dispatch
   * handler -- which is invoked on every real BRDY/BEMP -- so the
   * throughput is line-rate and integrity is perfect. */
  ux_dcd_ra8_usb_auto_echo_enable(2U, 1U);
  (void)tx_semaphore_put(&s_cdc_active_sem);
  s_cdc_activate_post_put++;
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
  s_cdc_deactivate_count++;
  s_cdc_acm = UX_NULL;
}

/* -------------------------------------------------------------------------- */
/* Worker thread: bring USBX up + echo loop */
/* -------------------------------------------------------------------------- */

/**
 * @brief Bring up USBX system + device stack with HS+FS frameworks.
 *
 * @details
 * Initializes the USBX memory pool, then registers both HS and FS device
 * frameworks. With the PHY CLKSEL=24 fix landed (see
 * ra8_usb.c::internal_usbhs_phy_bringup), the HS chirp completes correctly
 * and the host expects HS-conformant descriptors with 512-byte bulk MPS.
 * USBX picks the framework matching the negotiated bus speed
 * (RHST=011 -> HS framework, RHST=010 -> FS framework).
 *
 * @return true on success, false on any USBX failure.
 * @retval true Both system and device stacks initialized.
 * @retval false ``_ux_system_initialize`` or ``_ux_device_stack_initialize`` failed.
 *
 * @pre USBX pool storage exists and is sized k_demo_usbx_pool_bytes.
 * @post On success, the USBX stack is ready for class registration.
 *
 * @note Not thread-safe; called only from ``demo_worker``.
 * @since 0.1.0
 */
static bool demo_worker_usbx_init(void)
{
  s_boot_probe = (uint32_t)k_boot_probe_pre_sys_init;
  if (_ux_system_initialize(s_usbx_pool, k_demo_usbx_pool_bytes, UX_NULL, 0) != UX_SUCCESS) {
    return false;
  }
  s_boot_probe = (uint32_t)k_boot_probe_pre_dev_stack_init;
  return _ux_device_stack_initialize(s_tz_secure_only_usb_hs_device_framework_hs,
                                     sizeof(s_tz_secure_only_usb_hs_device_framework_hs),
                                     s_tz_secure_only_usb_hs_device_framework_fs,
                                     sizeof(s_tz_secure_only_usb_hs_device_framework_fs),
                                     s_tz_secure_only_usb_hs_string_framework,
                                     sizeof(s_tz_secure_only_usb_hs_string_framework),
                                     s_tz_secure_only_usb_hs_language_id_framework,
                                     sizeof(s_tz_secure_only_usb_hs_language_id_framework),
                                     UX_NULL) == UX_SUCCESS;
}

/**
 * @brief Register the CDC-ACM class against configuration 1, interface 0.
 *
 * @details
 * Wires the activate/deactivate callbacks so ``s_cdc_acm`` tracks the
 * live class instance. Must be called after the device stack has been
 * initialized and before ``ux_dcd_ra8_usb_initialize``.
 *
 * @return true on success, false on USBX failure.
 * @retval true CDC-ACM class registered.
 * @retval false ``_ux_device_stack_class_register`` failed.
 *
 * @pre ``demo_worker_usbx_init`` returned true.
 * @post CDC-ACM class is bound to configuration 1, interface 0.
 *
 * @note Not thread-safe; called only from ``demo_worker``.
 * @since 0.1.0
 */
static bool demo_worker_register_cdc(void)
{
  UX_SLAVE_CLASS_CDC_ACM_PARAMETER cdc_params = {
    .ux_slave_class_cdc_acm_instance_activate   = demo_cdc_activate,
    .ux_slave_class_cdc_acm_instance_deactivate = demo_cdc_deactivate,
    .ux_slave_class_cdc_acm_parameter_change    = UX_NULL,
  };
  s_boot_probe = (uint32_t)k_boot_probe_pre_class_register;
  return _ux_device_stack_class_register((UCHAR*)"ux_slave_class_cdc_acm",
                                         _ux_device_class_cdc_acm_entry,
                                         1, /* configuration # */
                                         0, /* interface #     */
                                         &cdc_params) == UX_SUCCESS;
}

/**
 * @brief Plug the DCD bridge into the device stack and attach the bus.
 *
 * @details
 * USBHS controller bring-up is performed exactly once, inside
 * ``ux_dcd_ra8_usb_initialize`` (which itself calls ``ra8_usb_device_init``).
 * The PHY clock was armed in main() via ``ra8_cgc_usbhs_pll_enable``;
 * PD07 role-select is owned by ``demo_pins_init``. MSTPB12 ungate happens
 * inside ``ra8_usb_device_init`` via ``ra8_mstp_enable``.
 *
 * Previously this site called ``ra8_board_usbhs_device_init()`` which also
 * invoked ``ra8_usb_device_init(hs)`` and ``ux_dcd_ra8_usb_initialize`` then
 * invoked it AGAIN. The duplicate PHY bring-up cleared INTENB0 and left
 * the controller in a state where chirp completed but the host never
 * issued SETUP -- exactly the failure signature observed on J7.
 *
 * The "host-mode kick" / SYSCFG reset block previously sitting between
 * the two probes was compile-time disabled (#if 0) and has been removed;
 * ``s_host_kick_done`` is left at 0 so JLink scripts that read it still
 * link.
 *
 * @return true on success, false on any HAL failure.
 * @retval true DCD initialized and bus attached at HS.
 * @retval false ``ux_dcd_ra8_usb_initialize`` or ``ra8_usb_device_attach`` failed.
 *
 * @pre ``demo_worker_register_cdc`` returned true.
 * @post Bus is attached at HS speed; controller is enumerating.
 *
 * @note Not thread-safe; called only from ``demo_worker``.
 * @since 0.1.0
 */
static bool demo_worker_start_dcd(void)
{
  s_boot_probe     = (uint32_t)k_boot_probe_post_board_usbhs_init;
  s_host_kick_done = 0U;
  s_boot_probe     = (uint32_t)k_boot_probe_pre_ux_dcd_init;
  if (ux_dcd_ra8_usb_initialize(k_ra8_usb_speed_hs) != k_ra8_ok) {
    return false;
  }
  s_boot_probe = (uint32_t)k_boot_probe_post_ux_dcd_init;
  s_boot_probe = (uint32_t)k_boot_probe_pre_dev_attach;
  if (ra8_usb_device_attach(k_ra8_usb_speed_hs, true) != k_ra8_ok) {
    return false;
  }
  s_boot_probe = (uint32_t)k_boot_probe_post_dev_attach;
  return true;
}

/**
 * @brief Spawn the INTENB0 re-arm watchdog thread.
 *
 * @details
 * USB Bus Reset on the RA8D2 HS controller auto-clears INTENB0 to 0
 * (verified live via JLink: a manual write of INTENB0 = 0xFF00 immediately
 * resumed ISR firing after a host bus reset). The driver's
 * ``busreset_rearm`` is supposed to do this from the DVST(=Default) ISR
 * path, but the ISR cannot run while INTENB0 is zero. This polled
 * watchdog breaks the chicken-and-egg by re-writing INTENB0 outside the
 * ISR.
 *
 * @pre ``demo_worker_start_dcd`` returned true.
 * @post A ThreadX thread named ``usbhs_intenb0_wdog`` is auto-started.
 *
 * @note Not thread-safe; called only from ``demo_worker``.
 * @since 0.1.0
 */
static void demo_worker_spawn_intenb0_watchdog(void)
{
  (void)tx_thread_create(&s_intenb0_watchdog_thread,
                         "usbhs_intenb0_wdog",
                         intenb0_watchdog_entry,
                         0UL,
                         s_intenb0_watchdog_stack,
                         (ULONG)sizeof(s_intenb0_watchdog_stack),
                         15U, /* lower priority than echo worker */
                         15U,
                         TX_NO_TIME_SLICE,
                         TX_AUTO_START);
}

/**
 * @brief Capture bisect probes ONCE on entry to the echo loop.
 *
 * @details
 * Reads SYSCFG / LPSTS / PSARB / PPARB into JLink-visible globals so
 * post-mortem analysis can confirm the controller state at the moment
 * the echo loop starts spinning.
 *
 * HUM Ch 37.2.1 SYSCFG p 2060, HUM Ch 37.2.43 LPSTS p 2111,
 * HUM Ch 51.8.1 PSARB p 3284, HUM Ch 51.8.6 PPARB p 3292.
 *
 * @pre USBHS MSTP ungated; SYSCFG.USBE=1.
 * @post Four ``s_*_in_echo_loop`` / ``s_*_state`` globals are set.
 *
 * @note Not thread-safe; called only from ``demo_worker``.
 * @since 0.1.0
 */
static void demo_worker_capture_echo_probes(void)
{
  s_syscfg_in_echo_loop                  = ra8_usb_hs()->SYSCFG;
  s_lpsts_in_echo_loop                   = *ra8_usbhs_lpsts();
  volatile const uint32_t* const psarb_p = (volatile const uint32_t*)k_usbhs_psarb_addr;
  volatile const uint32_t* const pparb_p = (volatile const uint32_t*)k_usbhs_pparb_addr;
  s_psar_state                           = *psarb_p;
  s_ppar_state                           = *pparb_p;
}

/**
 * @brief Run the CDC-ACM echo loop forever.
 *
 * @details
 * Read up to ``k_demo_echo_buf_bytes`` bytes from the host, echo them
 * straight back, and toggle LED1 once per byte echoed. ``s_demo_diag``
 * counters track each branch for JLink-side instrumentation.
 *
 * @pre ``demo_worker_start_dcd`` returned true.
 * @post Never returns.
 *
 * @note Single-instance; not designed for re-entry.
 * @since 0.1.0
 */
static void demo_worker_echo_loop(void)
{
  UCHAR buf[k_demo_echo_buf_bytes];
  ULONG n = 0UL;
  while (1) {
    s_demo_diag.loop_iter++;
    UX_SLAVE_CLASS_CDC_ACM* cdc_snap = (UX_SLAVE_CLASS_CDC_ACM*)s_cdc_acm;
    if (cdc_snap == UX_NULL) {
      s_demo_diag.loop_cdc_null++;
      /* Bounded-wait sleep instead of WAIT_FOREVER/relinquish. On this
       * silicon both tx_semaphore_get(TX_WAIT_FOREVER) and a tight
       * relinquish loop leave the worker un-schedulable once an
       * ISR-context activate runs. tx_thread_sleep with a small tick
       * count keeps the thread on a timer-resume path, which PendSV
       * does pick up here. The next iteration re-reads volatile
       * s_cdc_acm and proceeds. */
      tx_thread_sleep(k_demo_idle_ticks);
      continue;
    }
    if (_ux_system_slave != UX_NULL) {
      _ux_system_slave->ux_system_slave_device.ux_slave_device_state =
        (unsigned long)UX_DEVICE_CONFIGURED;
    }
    s_demo_diag.loop_pre_read++;
    UINT read_status = _ux_device_class_cdc_acm_read(cdc_snap, buf, sizeof(buf), &n);
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
    if (_ux_device_class_cdc_acm_write(cdc_snap, buf, n, &n) != UX_SUCCESS) {
      continue;
    }
    s_demo_diag.loop_post_write++;
    for (ULONG i = 0UL; i < n; i++) {
      (void)ra8_board_led_toggle(k_ra8_board_led1);
    }
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

  s_boot_probe = (uint32_t)k_boot_probe_thread_entry;
  if (!demo_worker_usbx_init()) {
    return;
  }
  if (!demo_worker_register_cdc()) {
    return;
  }
  if (!demo_worker_start_dcd()) {
    return;
  }
  demo_worker_spawn_intenb0_watchdog();

  s_boot_probe = (uint32_t)k_boot_probe_enter_echo_loop;
  demo_worker_capture_echo_probes();
  demo_worker_echo_loop();
}

/* -------------------------------------------------------------------------- */
/* INTENB0 re-arm watchdog */
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
  volatile r_usb_regs_t* reg = ra8_usb_hs();
  s_intenb0_watchdog_started = 1U;
  while (1) {
    if (reg->INTENB0 != (uint16_t)k_ra8_int0_full_mask) {
      reg->INTENB0 = (uint16_t)k_ra8_int0_full_mask;
      s_intenb0_rearm_count++;
    }
    tx_thread_sleep(1UL);
  }
}

/* -------------------------------------------------------------------------- */
/* ThreadX kernel entry: spawn the worker */
/* -------------------------------------------------------------------------- */

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
                         8U, /* priority          */
                         8U, /* preempt threshold */
                         TX_NO_TIME_SLICE,
                         TX_AUTO_START);
}
#endif /* !RA8_SIMULATOR_MODE */
