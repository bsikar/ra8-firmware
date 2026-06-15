/**
 * @file examples/ek_ra8d2/hw_validated/hil/tz_nsc_cgc_usb/ns_usb.c
 * @brief Non-Secure image: full ThreadX + USBX CDC self-loop inside TrustZone NS (#96).
 *
 * @par Tag
 * [Ring 6 / APP] {World: NS}
 *
 * @details
 * This is the literal title of issue #60 -- "ThreadX + USBX inside the NS
 * image" -- realised on the RAM-resident two-project NS build, taken all the way
 * to a self-validating loop. The Secure side (``trustzone_init.c``), before
 * BLXNS, routes both ports' pins, enables the USBHS PLL, sets the U15 expander to
 * host mode, and marks USBFS + USBHS Non-secure (PSARB bits 11 + 12). From
 * genuine Non-secure state this file runs TWO time-sliced ThreadX workers:
 *
 *   - ::ns_usb_worker -- the USBFS (J11) CDC-ACM DEVICE. Brings USBX + the
 *     ``ux_dcd_ra_usb`` bridge up (``RA_USB_POLLED_ONLY`` -- no USB NVIC line),
 *     attaches (D+ pull-up), then spins ``ra_usb_dispatch`` to service chapter-9
 *     + bulk auto-echo (OUT pipe 2 -> IN pipe 1).
 *   - ::ns_host_worker -- the USBHS (J7) polled HOST. Enumerates the looped
 *     device (GET_DESCRIPTOR / SET_ADDRESS / SET_CONFIGURATION), opens the bulk
 *     pipes, then bulk round-trips a deterministic pattern and byte-checks the
 *     echo forever, advancing ::g_tz_usb_host_rounds_ok (the HIL gate).
 *
 * Both workers share one ThreadX priority with a 1-tick time-slice: the host's
 * ~10 ms polling windows dwarf the slice, so the polled device dispatch is
 * serviced inside them without any USB interrupt. The controllers are reached
 * through the IDAU bit[28]=1 Non-secure aliases (USBFS 0x5025_0000,
 * USBHS 0x5035_0000), injected by ``RA_PERIPH_NS_ALIAS``.
 *
 * All RTOS objects (pool, stacks, TCB) live in NS ``.bss`` (the NS-image linker
 * routes ordinary ``.bss`` into the SRAM2 NS alias, and ``ns_reset_handler``
 * zeros the whole NS BSS span before ``tx_kernel_enter``).
 *
 * The looped device identifies as ``1209:000a``; an external host (macOS) on
 * J11 would also enumerate it as ``/dev/cu.usbmodem*``.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>
#include <string.h>

#include "ra_usb.h"
#include "tx_api.h"
#include "ux_api.h"
#include "ux_dcd_ra_usb.h"
#include "ux_device_class_cdc_acm.h"
#include "ux_device_stack.h"
#include "ux_system.h"

/* =============================================================================
 * Bench milestone counters (NS .bss; J-Link reads these)
 * =============================================================================
 *
 * ``g_tz_nsc_cgc_usb_match`` is defined in ns_main.c (the HIL gate symbol). The
 * USB worker advances it once per polled-dispatch iteration, so a continuing
 * advance proves the NS-resident USBX worker is alive and dispatching.
 */

/** @brief HIL gate symbol -- defined in ns_main.c, advanced by the USB worker. */
extern volatile uint32_t g_tz_nsc_cgc_usb_match;

/**
 * @var g_tz_usb_state
 * @brief USBX bring-up progress breadcrumb (localises a stall under J-Link).
 * @details 0 = before stack-up; 1 = USBX stack up; 2 = CDC class registered;
 *          3 = DCD bridge installed; 4 = device attached (D+ pull-up on);
 *          5 = in the polled-dispatch loop.
 * @note Read externally by J-Link only.
 * @since 0.1.0
 */
volatile uint32_t g_tz_usb_state;

/**
 * @var g_tz_usb_configured
 * @brief Advances each time the CDC-ACM activate callback fires (enumeration
 *        reached SET_CONFIGURATION -- the host configured the device).
 * @note Read externally by J-Link only.
 * @since 0.1.0
 */
volatile uint32_t g_tz_usb_configured;

/**
 * @var g_tz_usb_dispatch_count
 * @brief Polled-dispatch iteration counter (USB worker liveness).
 * @note Read externally by J-Link only.
 * @since 0.1.0
 */
volatile uint32_t g_tz_usb_dispatch_count;

/**
 * @var g_tz_usb_intsts_or
 * @brief Bitwise OR of every INTSTS0 snapshot the worker has seen.
 * @details Non-zero proves USB bus activity (a host is driving SOF / reset /
 *          SETUP), i.e. the FS PHY + clock + NS attribution are all live.
 * @note Read externally by J-Link only.
 * @since 0.1.0
 */
volatile uint32_t g_tz_usb_intsts_or;

/* =============================================================================
 * Non-Secure ra_delay_ms (ThreadX-backed; replaces ra_time.c in the NS link)
 * =============================================================================
 */

/**
 * @brief Non-Secure ``ra_delay_ms`` -- sleep ``ms`` ThreadX ticks.
 * @details ra_usb calls ``ra_delay_ms(1)`` once during device bring-up. The NS
 *          image must NOT link ra_time.c: its ra_time_init reprograms the SysTick
 *          that ThreadX owns, and its delay else-branch waits on a tick counter
 *          the ThreadX SysTick handler never advances. ThreadX's 1 ms tick makes
 *          one sleep tick == 1 ms here. Called only from thread context.
 * @param[in] ms Milliseconds to block (0 is rounded up to one tick).
 * @return void.
 * @pre Called from ThreadX thread context (the USB worker), not an ISR.
 * @pre The ThreadX scheduler is running (1 ms tick live).
 * @post The caller blocked for at least ``ms`` ticks.
 * @post No SysTick reconfiguration occurs.
 * @note Not callable from interrupt context.
 * @since 0.1.0
 */
void ra_delay_ms(uint32_t ms)
{
  (void)tx_thread_sleep(ms == 0U ? 1UL : (ULONG)ms);
}

/**
 * @brief Non-Secure ``ra_time_ms`` -- monotonic millisecond clock from ThreadX.
 * @details The polled host ladder (``cdc_enum_hunt``) uses ``ra_time_ms`` for
 *          its attach timeout. ThreadX's tick is 1 ms here, so ``tx_time_get``
 *          (ticks since boot) is already a millisecond count. Replaces ra_time.c
 *          (dropped from the NS link -- see ::ra_delay_ms).
 * @return Milliseconds since the ThreadX scheduler started.
 * @retval 0 Immediately after the kernel starts.
 * @pre The ThreadX scheduler is running.
 * @pre Called from thread context.
 * @post No state changes (pure read of the kernel tick).
 * @post The return value is monotonic between wraps (~49 days).
 * @note Thread-safe (single-word kernel read).
 * @since 0.1.0
 */
uint32_t ra_time_ms(void)
{
  return (uint32_t)tx_time_get();
}

/* =============================================================================
 * Host-side bench milestone counters (NS .bss; J-Link reads these)
 * =============================================================================
 */

/**
 * @var g_tz_usb_host_phase
 * @brief HS-host ladder phase: 0 boot, 1 host-init, 2 enumerating, 3 echoing,
 *        4 first full pass done.
 * @note Read externally by J-Link only.
 * @since 0.1.0
 */
volatile uint32_t g_tz_usb_host_phase;

/**
 * @var g_tz_usb_host_pid
 * @brief idProduct the HS host read from the looped FS device (expect 0x000A).
 * @note Read externally by J-Link only.
 * @since 0.1.0
 */
volatile uint32_t g_tz_usb_host_pid;

/**
 * @var g_tz_usb_host_rounds_ok
 * @brief Bulk echo rounds the HS host has verified byte-equal (advances
 *        forever once the loop is healthy -- the HIL gate probes this).
 * @note Read externally by J-Link only.
 * @since 0.1.0
 */
volatile uint32_t g_tz_usb_host_rounds_ok;

/**
 * @var g_tz_usb_host_err
 * @brief First non-OK ``ra_err_t`` from the host ladder (0 = none yet).
 * @note Read externally by J-Link only.
 * @since 0.1.0
 */
volatile uint32_t g_tz_usb_host_err;

/* =============================================================================
 * Worker thread + USBX pool storage (NS .bss)
 * =============================================================================
 */

/** @brief USBX worker thread + pool tunables. */
typedef enum : uint32_t {
  k_ns_usb_thread_stack  = 8192U,  /**< Worker thread stack (bytes).        */
  k_ns_usb_pool_bytes    = 16384U, /**< USBX memory pool (bytes).           */
  k_ns_usb_thread_prio   = 8U,     /**< Worker priority + preempt threshold.*/
  k_ns_usb_echo_out_pipe = 2U,     /**< CDC bulk-OUT -> pipe 2 (auto-echo). */
  k_ns_usb_echo_in_pipe  = 1U,     /**< CDC bulk-IN  -> pipe 1 (auto-echo). */
} ns_usb_cfg_t;

/**
 * @var s_ns_usb_thread
 * @brief ThreadX TCB for the NS USBX worker.
 * @note Single-writer (ThreadX in the NS image).
 * @since 0.1.0
 */
static TX_THREAD s_ns_usb_thread;

/**
 * @var s_ns_usb_stack
 * @brief Stack backing storage for ::s_ns_usb_thread.
 * @since 0.1.0
 */
static UCHAR s_ns_usb_stack[k_ns_usb_thread_stack];

/**
 * @var s_ns_usbx_pool
 * @brief USBX memory pool (USBX manages it as a ``tx_byte_pool``).
 * @since 0.1.0
 */
static UCHAR s_ns_usbx_pool[k_ns_usb_pool_bytes];

/**
 * @var s_ns_usb_thread_name
 * @brief Worker thread name (writable NS data; ThreadX name_ptr is CHAR*).
 * @since 0.1.0
 */
static CHAR s_ns_usb_thread_name[] = "ns_usb_worker";

/**
 * @var s_ns_cdc_acm
 * @brief Active CDC-ACM class instance captured by the activate callback.
 * @note Read by worker; written by the USBX class path (same worker thread).
 * @since 0.1.0
 */
static UX_SLAVE_CLASS_CDC_ACM* s_ns_cdc_acm = UX_NULL;

/* =============================================================================
 * USB descriptors (DEVICE + CONFIG + IAD + CDC interfaces + endpoints)
 * =============================================================================
 *
 * Verbatim from the validated threadx_usbx_cdc_demo: VID/PID 1209:000a (pid.codes
 * test range), one CDC-ACM communications interface + one CDC data interface,
 * EP3 IN (interrupt) for notifications, EP2 OUT / EP1 IN (bulk, 64-byte MPS).
 * Layout per CDC 1.20 sec 5 + USB 2.0 sec 9.6; bcdUSB 0x0200 (macOS rejects IAD
 * composite devices that advertise USB 1.1).
 */
static UCHAR s_ns_device_framework_fs[] = {
  /* Device descriptor (18 bytes). idVendor 0x1209, idProduct 0x000A. */
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
  /* Configuration descriptor (wTotalLength 0x4B = 75). */
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
  /* CDC header functional descriptor (bcdCDC 0x0120). */
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
 * @var s_ns_string_framework
 * @brief USBX string descriptor table (vendor / product / serial).
 * @since 0.1.0
 */
static UCHAR s_ns_string_framework[] = {
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
  /* idx 3: serial "00000001". */
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

/** @brief USBX LANGID 0x0409 (English-US), little-endian byte pair. */
typedef enum : uint8_t {
  k_ns_usb_langid_lo = 0x09U, /**< LANGID 0x0409 low byte.  */
  k_ns_usb_langid_hi = 0x04U, /**< LANGID 0x0409 high byte. */
} ns_usb_langid_t;

static UCHAR s_ns_language_id_framework[] = {k_ns_usb_langid_lo, k_ns_usb_langid_hi};

/* =============================================================================
 * CDC-ACM activate / deactivate callbacks
 * =============================================================================
 */

/**
 * @brief CDC-ACM activate callback -- capture the class + enable bulk auto-echo.
 * @param[in] cdc_instance Pointer to ``UX_SLAVE_CLASS_CDC_ACM``.
 * @return void.
 * @pre Invoked by the USBX device stack during SET_CONFIGURATION processing
 *      (here, from inside the worker's ra_usb_dispatch call).
 * @pre ``_ux_system_slave`` is non-NULL.
 * @post ::s_ns_cdc_acm points at the live class; device state pinned CONFIGURED.
 * @post ISR-side OUT->IN auto-echo is armed; ::g_tz_usb_configured advanced.
 * @note Runs in the polled-dispatch worker context.
 * @since 0.1.0
 */
static VOID ns_cdc_activate(VOID* cdc_instance)
{
  s_ns_cdc_acm = (UX_SLAVE_CLASS_CDC_ACM*)cdc_instance;
  if (_ux_system_slave != UX_NULL) {
    _ux_system_slave->ux_system_slave_device.ux_slave_device_state =
      (unsigned long)UX_DEVICE_CONFIGURED;
  }
  /* Mirror CDC bulk OUT (pipe 2) back on IN (pipe 1) inside the bridge
   * bottom-half, which the polled ra_usb_dispatch reaches. */
  ux_dcd_ra_usb_auto_echo_enable((uint8_t)k_ns_usb_echo_out_pipe, (uint8_t)k_ns_usb_echo_in_pipe);
  g_tz_usb_configured += 1U;
}

/**
 * @brief CDC-ACM deactivate callback -- drop the live class pointer.
 * @param[in] cdc_instance Unused.
 * @return void.
 * @pre Invoked by the USBX device stack on de-configuration / detach.
 * @pre ::s_ns_cdc_acm may or may not be set.
 * @post ::s_ns_cdc_acm is ``UX_NULL``.
 * @post No further echo until the next activate.
 * @note Runs in the polled-dispatch worker context.
 * @since 0.1.0
 */
static VOID ns_cdc_deactivate(VOID* cdc_instance)
{
  (void)cdc_instance;
  s_ns_cdc_acm = UX_NULL;
}

/* =============================================================================
 * USBX bring-up helpers
 * =============================================================================
 */

/**
 * @brief Bring the USBX system + device stack up on the FS framework.
 * @return UINT ``UX_SUCCESS`` on success, propagated USBX error otherwise.
 * @retval UX_SUCCESS Stack initialised; class registrations accepted.
 * @pre The USBX pool ::s_ns_usbx_pool is zeroed NS RAM.
 * @pre Called from the worker thread (USBX needs ThreadX services).
 * @post On success the device stack accepts class registration.
 * @post On failure USBX state is undefined; caller bails.
 * @note Single call; not idempotent.
 * @since 0.1.0
 */
static UINT ns_usbx_stack_up(void)
{
  if (_ux_system_initialize(s_ns_usbx_pool, (ULONG)k_ns_usb_pool_bytes, UX_NULL, 0) != UX_SUCCESS) {
    return UX_ERROR;
  }
  return _ux_device_stack_initialize((UCHAR*)UX_NULL,
                                     0,
                                     s_ns_device_framework_fs,
                                     sizeof(s_ns_device_framework_fs),
                                     s_ns_string_framework,
                                     sizeof(s_ns_string_framework),
                                     s_ns_language_id_framework,
                                     sizeof(s_ns_language_id_framework),
                                     UX_NULL);
}

/**
 * @brief Register the CDC-ACM class against the device-stack configuration.
 * @return UINT ``UX_SUCCESS`` on success, propagated USBX error otherwise.
 * @retval UX_SUCCESS Class registered against configuration 1, interface 0.
 * @pre ::ns_usbx_stack_up has succeeded.
 * @pre ::ns_cdc_activate / ::ns_cdc_deactivate are defined.
 * @post The CDC-ACM class is bound; activate posts ::g_tz_usb_configured.
 * @post Read/write endpoints become available after activation.
 * @note Not re-entrant.
 * @since 0.1.0
 */
static UINT ns_cdc_class_register(void)
{
  UX_SLAVE_CLASS_CDC_ACM_PARAMETER cdc_params = {
    .ux_slave_class_cdc_acm_instance_activate   = ns_cdc_activate,
    .ux_slave_class_cdc_acm_instance_deactivate = ns_cdc_deactivate,
    .ux_slave_class_cdc_acm_parameter_change    = UX_NULL,
  };
  return _ux_device_stack_class_register((UCHAR*)"ux_slave_class_cdc_acm",
                                         _ux_device_class_cdc_acm_entry,
                                         1,
                                         0,
                                         &cdc_params);
}

/* =============================================================================
 * USBX worker thread
 * =============================================================================
 */

/**
 * @brief NS USBX worker -- bring CDC up then poll the controller forever.
 * @param[in] arg Unused (ThreadX entry signature).
 * @return Never returns.
 * @pre ::ns_usb_application_define auto-started this thread.
 * @pre CPU is in NS thread mode; USB clock + PSARB delegation + pins are live.
 * @post On clean bring-up the worker loops in ra_usb_dispatch, advancing
 *       ::g_tz_nsc_cgc_usb_match (the HIL gate) every iteration.
 * @post On any bring-up error ::g_tz_usb_state freezes at the failing step and
 *       the thread returns (worker exits; gate stops -> failure visible).
 * @note Single-instance worker; not designed for re-entry.
 * @since 0.1.0
 */
static VOID ns_usb_worker(ULONG arg)
{
  (void)arg;

  if (ns_usbx_stack_up() != UX_SUCCESS) {
    return;
  }
  g_tz_usb_state = 1U;

  if (ns_cdc_class_register() != UX_SUCCESS) {
    return;
  }
  g_tz_usb_state = 2U;

  if (ux_dcd_ra_usb_initialize(k_ra_usb_speed_fs) != k_ra_ok) {
    return;
  }
  g_tz_usb_state = 3U;

  if (ra_usb_device_attach(k_ra_usb_speed_fs, true) != k_ra_ok) {
    return;
  }
  g_tz_usb_state = 4U;

  /* Polled controller service: drives bus reset, chapter-9 SETUP, and the
   * bulk auto-echo (all inside ra_usb_dispatch -> internal_event_cb ->
   * ux_dcd_ra_usb_irq). Time-sliced against ::ns_host_worker at the same
   * priority, so this yields each tick and the host's transfers land inside the
   * dispatch service window. */
  g_tz_usb_state = 5U;
  while (1) {
    ra_usb_dispatch(k_ra_usb_speed_fs);
    g_tz_usb_intsts_or |= (uint32_t)ra_usb_intsts0_snapshot(k_ra_usb_speed_fs);
    g_tz_usb_dispatch_count += 1U;
    g_tz_nsc_cgc_usb_match += 1U;
  }
}

/* =============================================================================
 * HS host: polled CDC enumerate + bulk echo over the self-loop
 * =============================================================================
 *
 * USBHS (J7) is wired host-side by the Secure boot (PSARB12, host-mode expander,
 * J7 VBUS, UTMI PLL). This worker enumerates the FS CDC device over the loop
 * cable using the first-party ra_usb_host_* polled primitives -- no IRQ -- and
 * then bulk round-trips a deterministic pattern through the device's auto-echo.
 * It runs at the SAME ThreadX priority as the device dispatch worker with
 * time-slicing, so each thread yields the CPU each tick; the device's chapter-9
 * + auto-echo (serviced in the device dispatch worker) lands well inside the
 * host primitives' ~10 ms polling windows. Ported from usb_selftest_cdc, with
 * the SCI console dropped (J-Link probes report the verdict instead).
 */

/** @brief Host worker stack (bytes). */
typedef enum : uint32_t {
  k_ns_host_stack_bytes = 8192U, /**< HS host worker stack.                */
  k_ns_time_slice       = 1U,    /**< ThreadX time-slice (ticks) per worker. */
} ns_host_cfg_t;

/**
 * @var s_ns_host_thread
 * @brief ThreadX TCB for the HS host worker.
 * @note Single-writer (ThreadX in the NS image).
 * @since 0.1.0
 */
static TX_THREAD s_ns_host_thread;

/**
 * @var s_ns_host_stack
 * @brief Stack backing storage for ::s_ns_host_thread.
 * @since 0.1.0
 */
static UCHAR s_ns_host_stack[k_ns_host_stack_bytes];

/**
 * @var s_ns_host_thread_name
 * @brief HS host worker thread name (writable NS data; ThreadX name is CHAR*).
 * @since 0.1.0
 */
static CHAR s_ns_host_thread_name[] = "ns_usb_host";

/** @brief Chapter-9 standard request / descriptor constants for the host. */
typedef enum : uint16_t {
  k_ns_bm_std_dev_in   = 0x80U, /**< bmRequestType: Std | Device | In.  */
  k_ns_bm_std_dev_out  = 0x00U, /**< bmRequestType: Std | Device | Out. */
  k_ns_breq_get_desc   = 0x06U, /**< GET_DESCRIPTOR.                    */
  k_ns_breq_set_addr   = 0x05U, /**< SET_ADDRESS.                      */
  k_ns_breq_set_config = 0x09U, /**< SET_CONFIGURATION.               */
  k_ns_desc_device     = 0x01U, /**< DEVICE descriptor type.          */
  k_ns_dev_desc_len    = 18U,   /**< DEVICE descriptor length.        */
  k_ns_off_dev_pid     = 10U,   /**< idProduct LSB byte offset.       */
  k_ns_byte_bits       = 8U,    /**< Bits per byte.                   */
  k_ns_config_value    = 1U,    /**< bConfigurationValue to select.   */
} ns_host_req_t;

/** @brief Host enumeration timing / retry / geometry tunables. */
typedef enum : uint32_t {
  k_ns_vbus_settle_ms = 200U,      /**< VBUS settle before probing.        */
  k_ns_attach_to_ms   = 2000U,     /**< Wait for the D+ pull-up.           */
  k_ns_debounce_ms    = 500U,      /**< Post-attach debounce (>=100 ms).   */
  k_ns_reset_hold_ms  = 50U,       /**< USB bus-reset hold (>=10 ms).      */
  k_ns_recovery_ms    = 20U,       /**< Post-reset recovery (TRSTRCY).     */
  k_ns_addr_settle_ms = 5U,        /**< Post-SET_ADDRESS recovery.         */
  k_ns_enum_tries     = 8U,        /**< Reset+probe attempts.              */
  k_ns_attach_spin    = 50000000U, /**< Attach spin cap (frozen-tick guard). */
  k_ns_dev_addr       = 1U,        /**< Address the host assigns.          */
  k_ns_mps            = 64U,       /**< Bulk endpoint wMaxPacketSize (FS). */
  k_ns_payload        = 60U,       /**< Bytes per echo round (sub-MPS).    */
  k_ns_echo_buf       = 64U,       /**< One-MPS host scratch buffer.       */
  k_ns_ep_in_num      = 1U,        /**< Device bulk-IN endpoint number.    */
  k_ns_ep_out_num     = 2U,        /**< Device bulk-OUT endpoint number.   */
  k_ns_host_pipe_in   = 1U,        /**< Host pipe for the device bulk-IN.  */
  k_ns_host_pipe_out  = 2U,        /**< Host pipe for the device bulk-OUT. */
  k_ns_pat_round_mul  = 97U,       /**< Per-round pattern multiplier.      */
  k_ns_pat_idx_mul    = 7U,        /**< Per-index pattern multiplier.      */
  k_ns_pat_bias       = 0x5AU,     /**< Pattern constant bias.             */
  k_ns_byte_mask      = 0xFFU,     /**< Byte mask.                         */
  k_ns_boot_wait_tk   = 500U,      /**< Host start delay (ticks).          */
  k_ns_retry_tk       = 1000U,     /**< Pause between failed enum passes.  */
} ns_host_tune_t;

/**
 * @brief Fill an echo payload with this round's deterministic bytes.
 * @details Byte i = ``(round*97 + i*7 + 0x5A) & 0xFF`` -- distinct per round so
 *          the host proves it read back what it sent for that round.
 * @param[in]  round The echo round index.
 * @param[out] out   Destination buffer.
 * @param[in]  len   Bytes to fill.
 * @return void.
 * @pre @p out has @p len writable bytes.
 * @pre @p len is at most ::k_ns_payload.
 * @post @p out[0..len-1] hold the round's pattern bytes.
 * @post No global state changes.
 * @note Pure function.
 * @since 0.1.0
 */
static void ns_host_pattern_fill(uint32_t round, uint8_t* out, uint32_t len)
{
  for (uint32_t i = 0U; i < len; i++) {
    const uint32_t v = (round * (uint32_t)k_ns_pat_round_mul) + (i * (uint32_t)k_ns_pat_idx_mul) +
                       (uint32_t)k_ns_pat_bias;
    out[i]           = (uint8_t)(v & (uint32_t)k_ns_byte_mask);
  }
}

/**
 * @brief GET_DESCRIPTOR(DEVICE) over the polled host control engine.
 * @param[out] desc Receives the 18-byte device descriptor.
 * @return Read outcome.
 * @retval k_ra_ok           All 18 bytes arrived.
 * @retval k_ra_err_hw_error A short descriptor came back.
 * @pre The bus is reset and the DCP targets the device's current address.
 * @pre @p desc holds at least ::k_ns_dev_desc_len bytes.
 * @post @p desc carries the device descriptor on success.
 * @post No global state changes.
 * @note Blocking (polled control transfer).
 * @since 0.1.0
 */
[[nodiscard]] static ra_err_t ns_host_get_dev_desc(uint8_t* desc)
{
  const ra_usb_setup_t setup = {
    .bm_request_type = (uint8_t)k_ns_bm_std_dev_in,
    .b_request       = (uint8_t)k_ns_breq_get_desc,
    .w_value         = (uint16_t)((uint16_t)k_ns_desc_device << (uint16_t)k_ns_byte_bits),
    .w_index         = 0U,
    .w_length        = (uint16_t)k_ns_dev_desc_len,
  };
  uint16_t       rx = 0U;
  const ra_err_t err =
    ra_usb_host_control_xfer(k_ra_usb_speed_hs, &setup, desc, (uint16_t)k_ns_dev_desc_len, &rx);
  if (err != k_ra_ok) {
    return err;
  }
  return (rx == (uint16_t)k_ns_dev_desc_len) ? k_ra_ok : k_ra_err_hw_error;
}

/**
 * @brief Wait for attach, then bus-reset + read the device descriptor.
 * @param[out] desc Receives the winning 18-byte device descriptor.
 * @return Hunt outcome.
 * @retval k_ra_ok             The device answered at address 0.
 * @retval k_ra_err_hw_timeout Nothing attached / nothing answered.
 * @pre ::ra_usb_host_init ran (host up, J7 VBUS supplied).
 * @pre The ThreadX 1 ms tick is live (ms delays / timeout).
 * @post On success the DCP targets address 0 with UACT on.
 * @post On failure the bus is left in the last attempt's state.
 * @note Blocking; worst case a few seconds.
 * @since 0.1.0
 */
[[nodiscard]] static ra_err_t ns_host_enum_hunt(uint8_t* desc)
{
  ra_delay_ms((uint32_t)k_ns_vbus_settle_ms);
  const uint32_t t0 = ra_time_ms();
  for (uint32_t spin = 0U; spin < (uint32_t)k_ns_attach_spin; spin++) {
    if (ra_usb_host_line_state(k_ra_usb_speed_hs) != 0U) {
      break;
    }
    if ((ra_time_ms() - t0) > (uint32_t)k_ns_attach_to_ms) {
      break;
    }
  }
  ra_delay_ms((uint32_t)k_ns_debounce_ms);
  ra_err_t err = k_ra_err_hw_timeout;
  for (uint8_t attempt = 0U; attempt < (uint8_t)k_ns_enum_tries; attempt++) {
    (void)ra_usb_host_bus_reset(k_ra_usb_speed_hs, true);
    ra_delay_ms((uint32_t)k_ns_reset_hold_ms);
    (void)ra_usb_host_bus_reset(k_ra_usb_speed_hs, false);
    (void)ra_usb_host_set_uact(k_ra_usb_speed_hs, true);
    ra_delay_ms((uint32_t)k_ns_recovery_ms);
    (void)ra_usb_host_set_target(k_ra_usb_speed_hs, 0U);
    err = ns_host_get_dev_desc(desc);
    if (err == k_ra_ok) {
      return k_ra_ok;
    }
  }
  return err;
}

/**
 * @brief SET_ADDRESS to ::k_ns_dev_addr, then retarget the DCP.
 * @return First failing step's error, or k_ra_ok.
 * @retval k_ra_ok The DCP now targets the operating address.
 * @pre ::ns_host_enum_hunt succeeded (device answering at address 0).
 * @pre The bus is active (UACT on).
 * @post Later transfers carry tokens to ::k_ns_dev_addr.
 * @post The set-address recovery delay has elapsed.
 * @note Blocking (one control transfer + settle).
 * @since 0.1.0
 */
[[nodiscard]] static ra_err_t ns_host_set_address(void)
{
  const ra_usb_setup_t setup = {
    .bm_request_type = (uint8_t)k_ns_bm_std_dev_out,
    .b_request       = (uint8_t)k_ns_breq_set_addr,
    .w_value         = (uint16_t)k_ns_dev_addr,
    .w_index         = 0U,
    .w_length        = 0U,
  };
  const ra_err_t err = ra_usb_host_control_xfer(k_ra_usb_speed_hs, &setup, nullptr, 0U, nullptr);
  if (err != k_ra_ok) {
    return err;
  }
  ra_delay_ms((uint32_t)k_ns_addr_settle_ms);
  return ra_usb_host_set_target(k_ra_usb_speed_hs, (uint8_t)k_ns_dev_addr);
}

/**
 * @brief SET_CONFIGURATION(::k_ns_config_value) on the addressed device.
 * @return Control-transfer outcome.
 * @retval k_ra_ok The device entered the Configured state.
 * @pre ::ns_host_set_address succeeded.
 * @pre The DCP targets ::k_ns_dev_addr.
 * @post On success the device's endpoints are usable.
 * @post No global state changes.
 * @note Blocking (one control transfer).
 * @since 0.1.0
 */
[[nodiscard]] static ra_err_t ns_host_set_config(void)
{
  const ra_usb_setup_t setup = {
    .bm_request_type = (uint8_t)k_ns_bm_std_dev_out,
    .b_request       = (uint8_t)k_ns_breq_set_config,
    .w_value         = (uint16_t)k_ns_config_value,
    .w_index         = 0U,
    .w_length        = 0U,
  };
  return ra_usb_host_control_xfer(k_ra_usb_speed_hs, &setup, nullptr, 0U, nullptr);
}

/**
 * @brief Open the host bulk pipes for the device's CDC data endpoints.
 * @return First failing pipe-setup error, or k_ra_ok.
 * @retval k_ra_ok Both bulk pipes configured.
 * @pre ::ns_host_set_config succeeded.
 * @pre The pipes are not currently armed.
 * @post Pipe OUT -> device EP2 OUT, pipe IN -> device EP1 IN, both at MPS.
 * @post The pipes target ::k_ns_dev_addr.
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] static ra_err_t ns_host_open_pipes(void)
{
  const ra_err_t err = ra_usb_host_pipe_setup(k_ra_usb_speed_hs,
                                              (uint8_t)k_ns_host_pipe_out,
                                              (uint8_t)k_ns_dev_addr,
                                              (uint8_t)k_ns_ep_out_num,
                                              false,
                                              (uint16_t)k_ns_mps);
  if (err != k_ra_ok) {
    return err;
  }
  return ra_usb_host_pipe_setup(k_ra_usb_speed_hs,
                                (uint8_t)k_ns_host_pipe_in,
                                (uint8_t)k_ns_dev_addr,
                                (uint8_t)k_ns_ep_in_num,
                                true,
                                (uint16_t)k_ns_mps);
}

/**
 * @brief Full enumeration ladder: hunt, SET_ADDRESS, SET_CONFIG, open pipes.
 * @param[out] out_pid Receives the device idProduct on success.
 * @return First failing step's error, or k_ra_ok.
 * @retval k_ra_ok Device enumerated; bulk pipes open.
 * @pre ::ra_usb_host_init succeeded on this pass.
 * @pre @p out_pid is non-NULL.
 * @post @p out_pid holds the device idProduct on success.
 * @post On failure the bus is left mid-ladder for the caller to deinit.
 * @note Blocking; runs on the host worker thread.
 * @since 0.1.0
 */
[[nodiscard]] static ra_err_t ns_host_enumerate(uint32_t* out_pid)
{
  uint8_t  desc[k_ns_dev_desc_len] = {};
  ra_err_t err                     = ns_host_enum_hunt(desc);
  if (err != k_ra_ok) {
    return err;
  }
  *out_pid = (uint32_t)desc[k_ns_off_dev_pid] |
             ((uint32_t)desc[(uint32_t)k_ns_off_dev_pid + 1U] << (uint32_t)k_ns_byte_bits);
  err      = ns_host_set_address();
  if (err != k_ra_ok) {
    return err;
  }
  err = ns_host_set_config();
  if (err != k_ra_ok) {
    return err;
  }
  return ns_host_open_pipes();
}

/**
 * @brief One echo round: bulk-OUT a pattern, bulk-IN the echo, compare.
 * @param[in] round The echo round index (pattern key).
 * @return ra_err_t verdict.
 * @retval k_ra_ok               The echo matched the sent payload.
 * @retval k_ra_err_invalid_size The echo length differed.
 * @retval k_ra_err_invalid_state The echo bytes differed.
 * @pre The bulk pipes were opened by ::ns_host_open_pipes.
 * @pre The device dispatch worker is auto-echoing.
 * @post Nothing is retained between rounds.
 * @post On a mismatch the caller records ::g_tz_usb_host_err.
 * @note Blocking; one bulk-OUT then one bulk-IN over the self-loop.
 * @since 0.1.0
 */
[[nodiscard]] static ra_err_t ns_host_echo_round(uint32_t round)
{
  static uint8_t s_tx[k_ns_payload]  = {};
  static uint8_t s_rx[k_ns_echo_buf] = {};
  ns_host_pattern_fill(round, s_tx, (uint32_t)k_ns_payload);
  ra_err_t err = ra_usb_host_bulk_out(k_ra_usb_speed_hs,
                                      (uint8_t)k_ns_host_pipe_out,
                                      s_tx,
                                      (uint16_t)k_ns_payload);
  if (err != k_ra_ok) {
    return err;
  }
  uint16_t rx = 0U;
  err         = ra_usb_host_bulk_in(k_ra_usb_speed_hs,
                                    (uint8_t)k_ns_host_pipe_in,
                                    s_rx,
                                    (uint16_t)k_ns_echo_buf,
                                    &rx);
  if (err != k_ra_ok) {
    return err;
  }
  if (rx != (uint16_t)k_ns_payload) {
    return k_ra_err_invalid_size;
  }
  if (memcmp(s_rx, s_tx, (size_t)k_ns_payload) != 0) {
    return k_ra_err_invalid_state;
  }
  return k_ra_ok;
}

/**
 * @brief HS host worker -- enumerate the looped FS device, then echo forever.
 * @param[in] arg Unused (ThreadX entry signature).
 * @return Never returns.
 * @pre ::tx_application_define auto-started this thread.
 * @pre The Secure boot delegated USBHS (PSARB12), set host mode + J7 VBUS, and
 *      enabled the UTMI PLL; the device worker is bringing the FS device up.
 * @post On success ::g_tz_usb_host_rounds_ok advances forever (HIL gate).
 * @post Each failed enumeration pass retries after a pause; the first error code
 *       is latched in ::g_tz_usb_host_err.
 * @note Polled host (no IRQ); shares a time-sliced priority with the device.
 * @since 0.1.0
 */
static VOID ns_host_worker(ULONG arg)
{
  (void)arg;
  (void)tx_thread_sleep((ULONG)k_ns_boot_wait_tk);

  uint32_t pid = 0U;
  while (1) {
    g_tz_usb_host_phase = 1U;
    ra_err_t err        = ra_usb_host_init(k_ra_usb_speed_hs);
    if (err != k_ra_ok) {
      g_tz_usb_host_err = (uint32_t)err;
      (void)tx_thread_sleep((ULONG)k_ns_retry_tk);
      continue;
    }
    g_tz_usb_host_phase = 2U;
    err                 = ns_host_enumerate(&pid);
    if (err != k_ra_ok) {
      g_tz_usb_host_err = (uint32_t)err;
      (void)ra_usb_host_deinit(k_ra_usb_speed_hs);
      (void)tx_thread_sleep((ULONG)k_ns_retry_tk);
      continue;
    }
    g_tz_usb_host_pid   = pid;
    g_tz_usb_host_phase = 3U;
    break;
  }

  /* Enumerated. Run bulk echo rounds forever; rounds_ok advancing is the
   * end-to-end self-loop proof (host bulk-OUT -> device auto-echo -> host
   * bulk-IN, byte-checked). */
  g_tz_usb_host_phase = 4U;
  uint32_t round      = 0U;
  while (1) {
    const ra_err_t err = ns_host_echo_round(round);
    if (err == k_ra_ok) {
      g_tz_usb_host_rounds_ok += 1U;
    } else if (g_tz_usb_host_err == 0U) {
      g_tz_usb_host_err = (uint32_t)err;
    }
    round += 1U;
  }
}

/**
 * @brief ThreadX application-define callback -- spawn the device + host workers.
 * @param[in] first_unused_memory ThreadX free-RAM base (unused; static stacks).
 * @return void.
 * @pre Called by ``tx_kernel_enter`` after kernel init (from ns_reset_handler).
 * @pre CPU is in NS state.
 * @post Two auto-started workers exist at ::k_ns_usb_thread_prio with a
 *       ::k_ns_time_slice time-slice, so they round-robin each tick.
 * @post The scheduler runs the FS device dispatch + the HS host ladder.
 * @note Single-threaded init context. This is the ONE tx_application_define for
 *       the NS image (ns_main.c no longer defines one).
 * @since 0.1.0
 */
void tx_application_define(void* first_unused_memory)
{
  (void)first_unused_memory;
  (void)tx_thread_create(&s_ns_usb_thread,
                         s_ns_usb_thread_name,
                         ns_usb_worker,
                         0UL,
                         s_ns_usb_stack,
                         (ULONG)k_ns_usb_thread_stack,
                         (UINT)k_ns_usb_thread_prio,
                         (UINT)k_ns_usb_thread_prio,
                         (ULONG)k_ns_time_slice,
                         TX_AUTO_START);
  (void)tx_thread_create(&s_ns_host_thread,
                         s_ns_host_thread_name,
                         ns_host_worker,
                         0UL,
                         s_ns_host_stack,
                         (ULONG)k_ns_host_stack_bytes,
                         (UINT)k_ns_usb_thread_prio,
                         (UINT)k_ns_usb_thread_prio,
                         (ULONG)k_ns_time_slice,
                         TX_AUTO_START);
}
