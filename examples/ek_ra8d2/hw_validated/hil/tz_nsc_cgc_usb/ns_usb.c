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
 *     ``ux_dcd_ra8_usb`` bridge up (``RA8_USB_POLLED_ONLY`` -- no USB NVIC line),
 *     attaches (D+ pull-up), then spins ``ra8_usb_dispatch`` to service chapter-9
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
 * USBHS 0x5035_0000), injected by ``RA8_PERIPH_NS_ALIAS``.
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

#include "ns_usb_internal.h"
#include "ra8_usb.h"
#include "tx_api.h"
#include "ux_api.h"
#include "ux_dcd_ra8_usb.h"
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
 * @enum tz_usb_state_t
 * @brief USBX bring-up progress breadcrumbs written to ::g_tz_usb_state.
 * @details Each value means "execution got this far". On any bring-up error
 *          the variable freezes at the failing step, so a single J-Link read
 *          localises the stall without a debugger session.
 * @invariant Values ascend in bring-up order, starting from the implicit 0
 *            (nothing attempted yet) that zero-initialised .bss provides.
 * @see g_tz_usb_state
 */
typedef enum : uint32_t {
  k_tz_usb_state_stack_up   = 1U, /**< USBX stack initialised.           */
  k_tz_usb_state_cdc_ready  = 2U, /**< CDC-ACM class registered.         */
  k_tz_usb_state_dcd_ready  = 3U, /**< DCD bridge installed.             */
  k_tz_usb_state_attached   = 4U, /**< Device attached (D+ pull-up on).  */
  k_tz_usb_state_dispatched = 5U, /**< Entered the polled-dispatch loop. */
} tz_usb_state_t;

/**
 * @var g_tz_usb_state
 * @brief USBX bring-up progress breadcrumb (localises a stall under J-Link).
 * @details Takes the ::tz_usb_state_t values; 0 means stack-up was never
 *          reached.
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
 * Non-Secure ra8_delay_ms (ThreadX-backed; replaces ra8_time.c in the NS link)
 * =============================================================================
 */

/**
 * @brief Non-Secure ``ra8_delay_ms`` -- sleep ``ms`` ThreadX ticks.
 * @details ra8_usb calls ``ra8_delay_ms(1)`` once during device bring-up. The NS
 *          image must NOT link ra8_time.c: its ra8_time_init reprograms the SysTick
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
void ra8_delay_ms(uint32_t ms)
{
  (void)tx_thread_sleep(ms == 0U ? 1UL : (ULONG)ms);
}

/**
 * @brief Non-Secure ``ra8_time_ms`` -- monotonic millisecond clock from ThreadX.
 * @details The polled host ladder (``cdc_enum_hunt``) uses ``ra8_time_ms`` for
 *          its attach timeout. ThreadX's tick is 1 ms here, so ``tx_time_get``
 *          (ticks since boot) is already a millisecond count. Replaces ra8_time.c
 *          (dropped from the NS link -- see ::ra8_delay_ms).
 * @return Milliseconds since the ThreadX scheduler started.
 * @retval 0 Immediately after the kernel starts.
 * @pre The ThreadX scheduler is running.
 * @pre Called from thread context.
 * @post No state changes (pure read of the kernel tick).
 * @post The return value is monotonic between wraps (~49 days).
 * @note Thread-safe (single-word kernel read).
 * @since 0.1.0
 */
uint32_t ra8_time_ms(void)
{
  return (uint32_t)tx_time_get();
}

/* =============================================================================
 * Worker thread + USBX pool storage (NS .bss)
 * =============================================================================
 */

/** @brief USBX worker thread + pool tunables. */
typedef enum : uint32_t {
  k_ns_usb_thread_stack  = 8192U,  /**< Worker thread stack (bytes).         */
  k_ns_usb_pool_bytes    = 16384U, /**< USBX memory pool (bytes).            */
  k_ns_usb_thread_prio   = 8U,     /**< Worker priority + preempt threshold. */
  k_ns_usb_echo_out_pipe = 2U,     /**< CDC bulk-OUT -> pipe 2 (auto-echo).  */
  k_ns_usb_echo_in_pipe  = 1U,     /**< CDC bulk-IN  -> pipe 1 (auto-echo).  */
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
 *      (here, from inside the worker's ra8_usb_dispatch call).
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
   * bottom-half, which the polled ra8_usb_dispatch reaches. */
  ux_dcd_ra8_usb_auto_echo_enable((uint8_t)k_ns_usb_echo_out_pipe, (uint8_t)k_ns_usb_echo_in_pipe);
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
  /* (UCHAR*)(uintptr_t): USBX takes a non-const class name; the uintptr_t hop
   * launders the string-literal const without tripping -Wcast-qual. */
  return _ux_device_stack_class_register((UCHAR*)(uintptr_t)"ux_slave_class_cdc_acm",
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
 * @post On clean bring-up the worker loops in ra8_usb_dispatch, advancing
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
  g_tz_usb_state = (uint32_t)k_tz_usb_state_stack_up;

  if (ns_cdc_class_register() != UX_SUCCESS) {
    return;
  }
  g_tz_usb_state = (uint32_t)k_tz_usb_state_cdc_ready;

  if (ux_dcd_ra8_usb_initialize(k_ra8_usb_speed_fs) != k_ra8_ok) {
    return;
  }
  g_tz_usb_state = (uint32_t)k_tz_usb_state_dcd_ready;

  if (ra8_usb_device_attach(k_ra8_usb_speed_fs, true) != k_ra8_ok) {
    return;
  }
  g_tz_usb_state = (uint32_t)k_tz_usb_state_attached;

  /* Polled controller service: drives bus reset, chapter-9 SETUP, and the
   * bulk auto-echo (all inside ra8_usb_dispatch -> internal_event_cb ->
   * ux_dcd_ra8_usb_irq). Time-sliced against ::ns_host_worker at the same
   * priority, so this yields each tick and the host's transfers land inside the
   * dispatch service window. */
  g_tz_usb_state = (uint32_t)k_tz_usb_state_dispatched;
  while (1) {
    ra8_usb_dispatch(k_ra8_usb_speed_fs);
    g_tz_usb_intsts_or |= (uint32_t)ra8_usb_intsts0_snapshot(k_ra8_usb_speed_fs);
    g_tz_usb_dispatch_count += 1U;
    g_tz_nsc_cgc_usb_match += 1U;
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
