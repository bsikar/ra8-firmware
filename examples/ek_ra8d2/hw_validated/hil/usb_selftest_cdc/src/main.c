/**
 * @file examples/ek_ra8d2/hw_validated/hil/usb_selftest_cdc/src/main.c
 * @brief USB self-loop: HS host writes bytes to a CDC-ACM device, reads the echo
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * The CDC-ACM echo self-loop -- it exercises a bidirectional bulk
 * round-trip through a real device class (host bulk-OUT -> device
 * receives -> device echoes -> host bulk-IN). The two USB ports are
 * cabled to EACH OTHER and one firmware image runs both USB stacks:
 *
 *  - USBFS (J11) = DEVICE: a ThreadX + USBX CDC-ACM class. A worker
 *    loops `_ux_device_class_cdc_acm_read` -> `_ux_device_class_cdc_acm_write`,
 *    echoing every byte the host sends straight back on the bulk-IN
 *    endpoint. IRQ-driven through the `port/usbx/ux_dcd_ra8_usb` bridge.
 *    (Worker-thread echo, NOT the DCD ISR auto-echo -- the worker path
 *    rides the normal device bulk-OUT receive that the WRITE(10) driver
 *    fix repaired.)
 *  - USBHS (J7) = HOST: a self-contained polled CDC host built on the
 *    first-party `ra8_usb_host_*` primitives. It enumerates the device
 *    (bus reset -> GET_DESCRIPTOR -> SET_ADDRESS -> SET_CONFIGURATION),
 *    opens the CDC data interface's bulk pipes (EP2 OUT / EP1 IN), then
 *    runs several rounds: bulk-OUT a deterministic pattern, bulk-IN the
 *    echo, and byte-check it -- proving the bidirectional bulk path
 *    round-trips intact, end to end on chip.
 *
 * Each round ships a sub-MPS (60-byte) payload so the echo returns as a
 * single short packet (no MPS-exact ZLP ambiguity on the host bulk-IN).
 * No serial terminal is involved; raw bulk transfers only.
 *
 * The link runs at 12 Mbps (FS device ceiling; HS host serves an FS
 * downstream device).
 *
 * Verdicts stream over SCI8 (J-Link OB CDC console, 115200) and are
 * mirrored in J-Link-readable probes (``s_dbg_*``).
 *
 * ## Pinout
 *
 * FS device: P4_07 VBUS sense, P5_00 VBUSEN GPIO LOW (device role),
 * P8_14 D+, P8_15 D- (PSEL usb_fs). HS host: SW4-8 to Host via the U15
 * expander, PD07 HIGH (U18 supplies J7 VBUS), P4_08 USBHS_VBUS
 * (PSEL usb_hs). Console: PD_02/PD_03 SCI8 (PSEL sci_async).
 *
 * @author Brighton Sikarskie
 * @date 2026-06-13
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
#include "ra8_port_constants.h"
#include "ra8_port_utils.h"
#include "ra8_time.h"
#include "ra8_usb.h"
#include "ra8_usb_compose.h"
#include "ra8_usb_desc.h"
#include "usb_selftest_cdc_steps.h"

#ifndef RA8_OFF_TARGET
#include "tx_api.h"
#include "ux_api.h"
#include "ux_dcd_ra8_usb.h"
#include "ux_device_class_cdc_acm.h"
#include "ux_device_stack.h"

/* Strong SysTick override: route the tick into BOTH the ra8_time millisecond
 * counter (for ra8_delay_ms and the polled host stack's timeouts) AND
 * ThreadX's timer; the 1 ms pulse also recovers the DCD's storm-guard mask. */

extern void _tx_timer_interrupt(void);

/**
 * @var s_tx_kernel_up
 * @brief Set in ::tx_application_define; gates ThreadX tick delivery.
 * @details main() starts SysTick before tx_kernel_enter and the setup
 *          window is long (U15 expander I2C blocks for ms), so the tick
 *          fires pre-kernel; feeding _tx_timer_interrupt into ThreadX's
 *          zeroed timer state bus-faults. Gate it until the kernel runs.
 * @since 0.1.0
 */
static volatile bool s_tx_kernel_up = false;

void SysTick_Handler(void);
void SysTick_Handler(void)
{
  ra8_time_on_tick();
  if (s_tx_kernel_up) {
    _tx_timer_interrupt();
    ux_dcd_ra8_usb_irq_reenable();
  }
}
#endif

/* -------------------------------------------------------------------------- */
/* Pinout (FSP-aligned, EK-RA8D2 v1 User's Manual) */
/* -------------------------------------------------------------------------- */

/** @brief USBHS_VBUS sense pin (P4_08, PSEL = 0x14). */
static const ra8_port_pin_t k_cdc_pin_hs_vbus = (ra8_port_pin_t)k_ra8_board_usbhs_pin_vbus;

/** @brief J7 host-power switch (PD07): HIGH = U18 supplies VBUS (UM 6.2). */
static const ra8_port_pin_t k_cdc_pin_hs_pwr = (ra8_port_pin_t)k_ra8_board_usbhs_pin_pwr;

/* -------------------------------------------------------------------------- */
/* Tunables */
/* -------------------------------------------------------------------------- */

/* The shared sizing/geometry enums (::cdc_config_t, ::cdc_geom_t) live in the
 * companion `usb_selftest_cdc_steps.h`; the host-only formatter, mask, and
 * phase enums live in `usb_selftest_cdc_steps.c`. Only the device-side step
 * enum is private to this unit. */

/**
 * @enum cdc_dev_step_t
 * @brief J-Link probe values marking device-worker bring-up progress.
 */
typedef enum : uint32_t {
  k_cdc_dev_step_stack  = 1U, /**< USBX system + device stack up. */
  k_cdc_dev_step_class  = 2U, /**< CDC-ACM class registered.      */
  k_cdc_dev_step_dcd    = 3U, /**< DCD bridge initialized.        */
  k_cdc_dev_step_attach = 4U, /**< Device attached (DPRPU).       */
  k_cdc_dev_step_echo   = 5U, /**< Echo loop running.             */
} cdc_dev_step_t;

#ifndef RA8_OFF_TARGET

/* -------------------------------------------------------------------------- */
/* ThreadX workers + USBX pool storage */
/* -------------------------------------------------------------------------- */

/**
 * @var s_device_thread
 * @brief ThreadX TCB for the USBX device-side worker thread.
 * @note Single-writer (worker only).
 * @since 0.1.0
 */
static TX_THREAD s_device_thread;

/**
 * @var s_device_stack
 * @brief Stack backing storage for ::s_device_thread.
 * @since 0.1.0
 */
static UCHAR s_device_stack[k_cdc_thread_stack];

/**
 * @var s_host_thread
 * @brief ThreadX TCB for the host-side worker thread.
 * @note Single-writer (worker only).
 * @since 0.1.0
 */
static TX_THREAD s_host_thread;

/**
 * @var s_host_stack
 * @brief Stack backing storage for ::s_host_thread.
 * @since 0.1.0
 */
static UCHAR s_host_stack[k_cdc_host_stack];

/**
 * @var s_usbx_pool
 * @brief USBX memory pool (USBX uses ``tx_byte_pool`` internally).
 * @since 0.1.0
 */
static UCHAR s_usbx_pool[k_cdc_usbx_pool_bytes];

/**
 * @var s_cdc_acm
 * @brief Active CDC-ACM class instance, captured by the activate callback.
 * @note Written by the USBX class thread; read by the device echo worker.
 * @since 0.1.0
 */
static UX_SLAVE_CLASS_CDC_ACM* s_cdc_acm = UX_NULL;

/**
 * @var s_cdc_active_sem
 * @brief Posted by the activate callback so the echo worker blocks on it
 *        instead of polling ``s_cdc_acm`` with tx_thread_sleep (which has
 *        been observed never returning on this silicon under load).
 * @note Single-producer (class thread), single-consumer (echo worker).
 * @since 0.1.0
 */
static TX_SEMAPHORE s_cdc_active_sem;

/* -------------------------------------------------------------------------- */
/* J-Link probes (device side) */
/* -------------------------------------------------------------------------- */

/* The host-side ladder probes (``s_dbg_phase``, ``s_dbg_rounds_ok``,
 * ``s_dbg_pid``, ``s_dbg_mismatch``, ``s_dbg_pass_count``) live in the
 * companion `usb_selftest_cdc_steps.c` alongside their only writers. */

/** @brief Device-side echo iterations (one read+write each). */
static volatile uint32_t s_dbg_dev_echo_calls;
/** @brief Bytes the device echoed on the most recent round. */
static volatile uint32_t s_dbg_dev_last_len;
/** @brief Device worker progress: 1 stack, 2 class, 3 dcd, 4 attach, 5 echo. */
static volatile uint32_t s_dbg_dev_step;
/** @brief Device worker first failing return code (0 = none). */
static volatile uint32_t s_dbg_dev_err;

/* -------------------------------------------------------------------------- */
/* USB descriptors (CDC-ACM: comm + data interface via an IAD) */
/* -------------------------------------------------------------------------- */

/* The three frameworks below are synthesised at start-up from the config
 * structs by libs/ra8_usb_pal (#766) rather than typed out as raw byte
 * arrays. The layout is byte-identical to the proven usb_cdc_echo device,
 * retagged PID 0x0017 for the CDC self-test identity: one CDC ACM
 * communications interface + one CDC data interface joined by an IAD, with
 * EP3 IN (interrupt) for notifications and EP2 OUT / EP1 IN (bulk, 64-byte
 * MPS) for the data pipes, per CDC 1.20 sec 5 + USB 2.0 sec 9.6. The host
 * half of this self-loop drives only the bulk pipes. The wTotalLength a
 * human used to count by hand is now derived from what the builder actually
 * emitted.

/**
 * @enum cdc_usb_identity_t
 * @brief The device identity this self-test publishes.
 *
 * @details ::k_cdc_usb_max_power_ma is the real milliamp draw, not the
 * halved bMaxPower encoding; the builder halves it. Bus-powered is the
 * default, because advertising self-powered alongside a 100 mA draw is the
 * contradiction an earlier revision of this family of apps shipped.
 */
typedef enum : uint16_t {
  k_cdc_usb_vid          = 0x1209U, /**< idVendor, pid.codes test range. */
  k_cdc_usb_pid          = 0x0017U, /**< idProduct, CDC self-test tag.   */
  k_cdc_usb_bcd_device   = 0x0100U, /**< bcdDevice, release 1.00.        */
  k_cdc_usb_max_power_ma = 100U,    /**< Bus draw in mA.                 */
} cdc_usb_identity_t;

/**
 * @enum cdc_usb_endpoint_t
 * @brief The CDC-ACM endpoint layout, addresses as they appear on the wire.
 *
 * @details An IN endpoint carries bit 7 set, so EP1 IN is 0x81 and EP3 IN is
 * 0x83, while EP2 OUT is 0x02. That is how the byte array this block replaces
 * wrote them, which keeps the two diffable.
 */
typedef enum : uint16_t {
  k_cdc_usb_notify_ep       = 0x83U, /**< Interrupt-IN, notifications.  */
  k_cdc_usb_notify_bytes    = 8U,    /**< Interrupt-IN max packet size. */
  k_cdc_usb_notify_interval = 255U,  /**< bInterval, 255 ms poll.       */
  k_cdc_usb_out_ep          = 0x02U, /**< Bulk-OUT data pipe.           */
  k_cdc_usb_in_ep           = 0x81U, /**< Bulk-IN data pipe.            */
  k_cdc_usb_data_bytes      = 64U,   /**< Bulk max packet size, FS.     */
  k_cdc_usb_functions       = 1U,    /**< Functions in the config.      */
} cdc_usb_endpoint_t;

/**
 * @var k_cdc_usb_device
 * @brief Device identity handed to the framework builders.
 * @note The three strings are string-literal storage with static duration;
 *       the builders copy them and retain no pointer.
 * @since 0.1.0
 */
static const ra8_usb_desc_device_t k_cdc_usb_device = {
  .vid           = (uint16_t)k_cdc_usb_vid,
  .pid           = (uint16_t)k_cdc_usb_pid,
  .bcd_device    = (uint16_t)k_cdc_usb_bcd_device,
  .manufacturer  = "Brighton Sikarskie",
  .product       = "RA8D2 CDC ECHO",
  .serial        = "00000013",
  .langid        = (uint16_t)k_ra8_usb_desc_langid_en_us,
  .max_power_ma  = (uint16_t)k_cdc_usb_max_power_ma,
  .self_powered  = false,
  .remote_wakeup = false,
};

/**
 * @var k_cdc_usb_cdc
 * @brief CDC-ACM endpoint layout handed to the framework builder.
 * @since 0.1.0
 */
static const ra8_usb_desc_cdc_acm_t k_cdc_usb_cdc = {
  .notify_ep          = (uint8_t)k_cdc_usb_notify_ep,
  .notify_bytes       = (uint16_t)k_cdc_usb_notify_bytes,
  .notify_interval_ms = (uint8_t)k_cdc_usb_notify_interval,
  .out_ep             = (uint8_t)k_cdc_usb_out_ep,
  .in_ep              = (uint8_t)k_cdc_usb_in_ep,
  .data_bytes         = (uint16_t)k_cdc_usb_data_bytes,
};

/**
 * @var s_device_framework_fs
 * @brief Synthesised device framework: device descriptor + configuration.
 * @note Written once by ::cdc_usb_build_frameworks, then read-only.
 * @since 0.1.0
 */
static uint8_t s_device_framework_fs[k_ra8_usb_desc_framework_bytes_max];

/**
 * @var s_string_framework
 * @brief Synthesised string framework: manufacturer, product, serial.
 * @note Written once by ::cdc_usb_build_frameworks, then read-only.
 * @since 0.1.0
 */
static uint8_t s_string_framework[k_ra8_usb_desc_strings_bytes_max];

/**
 * @var s_language_id_framework
 * @brief Synthesised language-id framework -- US English.
 * @note Written once by ::cdc_usb_build_frameworks, then read-only.
 * @since 0.1.0
 */
static uint8_t s_language_id_framework[k_ra8_usb_desc_langid_bytes];

/**
 * @var s_device_framework_len
 * @brief Bytes ::cdc_usb_build_frameworks wrote to ::s_device_framework_fs.
 * @since 0.1.0
 */
static uint32_t s_device_framework_len = 0U;

/**
 * @var s_string_framework_len
 * @brief Bytes ::cdc_usb_build_frameworks wrote to ::s_string_framework.
 * @since 0.1.0
 */
static uint32_t s_string_framework_len = 0U;

/**
 * @var s_language_id_framework_len
 * @brief Bytes written to ::s_language_id_framework.
 * @since 0.1.0
 */
static uint32_t s_language_id_framework_len = 0U;

/**
 * @brief Synthesise the three USB frameworks this self-test enumerates with.
 *
 * @details Replaces the three hand-typed byte arrays this app used to carry,
 * and now the three encoder calls that replaced them: ::ra8_usb_device_compose
 * writes all three frameworks from one identity plus one class entry. Nothing
 * here touches a controller: a synthesised framework is bytes, not an attached
 * device.
 *
 * @return ra8_err_t Result of the compose.
 * @retval k_ra8_ok               All three frameworks written.
 * @retval k_ra8_err_invalid_size A destination buffer is too small.
 * @retval k_ra8_err_invalid_arg  An endpoint address or packet size is wrong.
 *
 * @pre Called from thread context before ``_ux_device_stack_initialize``.
 * @post On success the three buffers hold the frameworks and the three
 *       length variables count them.
 * @post On failure the lengths of the encodes that did not run stay 0.
 *
 * @note Single-call; the builders are pure, so a repeat call is harmless.
 * @since 0.1.0
 */
static ra8_err_t cdc_usb_build_frameworks(void)
{
  const ra8_usb_class_t function = {
    .kind    = k_ra8_usb_class_cdc_acm,
    .cdc_acm = k_cdc_usb_cdc,
  };

  const ra8_usb_device_cfg_t cfg = {
    .desc        = &k_cdc_usb_device,
    .classes     = &function,
    .class_count = (uint8_t)k_cdc_usb_functions,
  };

  ra8_usb_device_frameworks_t fw = {
    .device      = s_device_framework_fs,
    .device_cap  = (uint32_t)sizeof(s_device_framework_fs),
    .strings     = s_string_framework,
    .strings_cap = (uint32_t)sizeof(s_string_framework),
    .langid      = s_language_id_framework,
    .langid_cap  = (uint32_t)sizeof(s_language_id_framework),
  };

  const ra8_err_t err = ra8_usb_device_compose(&cfg, &fw);
  if (err != k_ra8_ok) {
    return err;
  }

  s_device_framework_len      = fw.device_len;
  s_string_framework_len      = fw.strings_len;
  s_language_id_framework_len = fw.langid_len;

  return k_ra8_ok;
}

/* -------------------------------------------------------------------------- */
/* CDC-ACM activate / deactivate callbacks */
/* -------------------------------------------------------------------------- */

/**
 * @brief CDC-ACM activate callback. Captures the live class instance.
 *
 * @details Pins ``ux_slave_device_state`` at CONFIGURED (works around a
 * residual DVSQ-poll race on this silicon that can demote it back to
 * ATTACHED after SET_CONFIGURATION and break the cdc_acm_read gate), then
 * posts the semaphore the echo worker blocks on. The DCD ISR auto-echo is
 * deliberately NOT enabled -- the worker read/write path is the sole data
 * mover, so the two cannot race on the bulk-OUT pipe.
 *
 * @param[in] cdc_instance Pointer to ``UX_SLAVE_CLASS_CDC_ACM``.
 *
 * @pre Called from the USBX class thread.
 * @pre SET_CONFIGURATION has just configured the device.
 * @post ``s_cdc_acm`` points at the live class.
 * @post ::s_cdc_active_sem is posted so the echo worker runs.
 *
 * @note USBX serializes this with the deactivate callback.
 * @since 0.1.0
 */
static VOID cdc_activate(VOID* cdc_instance)
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
 * @pre The CDC-ACM class is being torn down.
 * @post ``s_cdc_acm`` is ``UX_NULL``.
 * @post The echo worker blocks until the next activate.
 *
 * @note USBX serializes this with the activate callback.
 * @since 0.1.0
 */
static VOID cdc_deactivate(VOID* cdc_instance)
{
  (void)cdc_instance;
  s_cdc_acm = UX_NULL;
}

/* -------------------------------------------------------------------------- */
/* Device side: USBX CDC-ACM echo */
/* -------------------------------------------------------------------------- */

/**
 * @brief Bring USBX system + device stack up with the CDC-ACM framework.
 *
 * @details Synthesises the three frameworks, then does a one-shot USBX
 * pool + device-stack init (FS-only framework).
 *
 * @return UINT UX_SUCCESS on success.
 * @retval UX_SUCCESS Stack ready.
 *
 * @pre File-scope pool reserved.
 * @pre Thread context.
 * @post Device stack accepts class registrations.
 * @post On failure USBX state is undefined.
 *
 * @note Single-call; not idempotent.
 * @since 0.1.0
 */
static UINT cdc_usbx_stack_up(void)
{
  if (cdc_usb_build_frameworks() != k_ra8_ok) {
    return UX_ERROR;
  }
  if (_ux_system_initialize(s_usbx_pool, k_cdc_usbx_pool_bytes, UX_NULL, 0) != UX_SUCCESS) {
    return UX_ERROR;
  }
  return _ux_device_stack_initialize((UCHAR*)UX_NULL,
                                     0,
                                     (UCHAR*)s_device_framework_fs,
                                     (ULONG)s_device_framework_len,
                                     (UCHAR*)s_string_framework,
                                     (ULONG)s_string_framework_len,
                                     (UCHAR*)s_language_id_framework,
                                     (ULONG)s_language_id_framework_len,
                                     UX_NULL);
}

/**
 * @brief Register the CDC-ACM class against configuration 1, interface 0.
 *
 * @details Binds ::cdc_activate / ::cdc_deactivate so the worker learns
 * when the host has configured the device. The comm + data interfaces are
 * described by the device framework (IAD composite).
 *
 * @return UINT UX_SUCCESS on success, propagated USBX error otherwise.
 * @retval UX_SUCCESS Class registered.
 *
 * @pre ::cdc_usbx_stack_up has succeeded.
 * @pre ::cdc_activate / ::cdc_deactivate are defined.
 * @post The CDC-ACM class is bound; the activate callback will fire on
 *       SET_CONFIGURATION.
 * @post No other class is registered.
 *
 * @note Not re-entrant.
 * @since 0.1.0
 */
static UINT cdc_class_register(void)
{
  UX_SLAVE_CLASS_CDC_ACM_PARAMETER cdc_params = {
    .ux_slave_class_cdc_acm_instance_activate   = cdc_activate,
    .ux_slave_class_cdc_acm_instance_deactivate = cdc_deactivate,
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
 * @brief One device echo iteration: read a bulk-OUT chunk, write it back.
 *
 * @details Pins CONFIGURED (DVSQ-poll race guard), then
 * `_ux_device_class_cdc_acm_read` (blocks for the host's bulk-OUT, returns
 * on the short packet) -> `_ux_device_class_cdc_acm_write` (stages the same
 * bytes on bulk-IN). This rides the normal device bulk-OUT receive path the
 * WRITE(10) driver fix repaired, NOT the DCD ISR auto-echo.
 *
 * @param[in,out] buf Scratch buffer for one chunk.
 * @param[in]     cap Capacity of @p buf in bytes.
 *
 * @pre ``s_cdc_acm`` is non-NULL (class activated).
 * @pre @p buf holds @p cap bytes.
 * @post On a non-empty read the bytes were echoed; counters advanced.
 * @post On a read error the worker backs off one idle tick.
 *
 * @note Runs on the device worker thread.
 * @since 0.1.0
 */
static void cdc_echo_iter(UCHAR* buf, ULONG cap)
{
  if (_ux_system_slave != UX_NULL) {
    _ux_system_slave->ux_system_slave_device.ux_slave_device_state =
      (unsigned long)UX_DEVICE_CONFIGURED;
  }
  ULONG n = 0UL;
  if (_ux_device_class_cdc_acm_read(s_cdc_acm, buf, cap, &n) != UX_SUCCESS) {
    tx_thread_sleep(1U);
    return;
  }
  if (n == 0UL) {
    return;
  }
  if (_ux_device_class_cdc_acm_write(s_cdc_acm, buf, n, &n) != UX_SUCCESS) {
    return;
  }
  s_dbg_dev_echo_calls++;
  s_dbg_dev_last_len = (uint32_t)n;
  (void)ra8_board_led_toggle(k_ra8_board_led1);
}

/**
 * @brief Device-side worker: bring the CDC-ACM device up, then echo forever.
 *
 * @details USBX system + device stack + CDC-ACM class + DCD bridge on the
 * USBFS controller, then DPRPU attach. Blocks on ::s_cdc_active_sem until
 * the host configures the device, then loops ::cdc_echo_iter.
 *
 * @param[in] arg ThreadX entry argument (unused).
 *
 * @pre tx_application_define created this thread.
 * @pre USB-FS pins + 48 MHz clock are up (main did both).
 * @post The FS device is attached and echoes bulk-OUT back on bulk-IN.
 * @post On any bring-up failure the thread exits.
 *
 * @note Runs once; loops forever on success.
 * @since 0.1.0
 */
static VOID cdc_device_worker(ULONG arg)
{
  (void)arg;

  UINT ux = cdc_usbx_stack_up();
  if (ux != UX_SUCCESS) {
    s_dbg_dev_err = (uint32_t)ux;
    return;
  }
  s_dbg_dev_step = (uint32_t)k_cdc_dev_step_stack;
  ux             = cdc_class_register();
  if (ux != UX_SUCCESS) {
    s_dbg_dev_err = (uint32_t)ux;
    return;
  }
  s_dbg_dev_step = (uint32_t)k_cdc_dev_step_class;
  ra8_err_t e    = ux_dcd_ra8_usb_initialize(k_ra8_usb_speed_fs);
  if (e != k_ra8_ok) {
    s_dbg_dev_err = (uint32_t)e;
    return;
  }
  s_dbg_dev_step = (uint32_t)k_cdc_dev_step_dcd;
  e              = ra8_usb_device_attach(k_ra8_usb_speed_fs, true);
  if (e != k_ra8_ok) {
    s_dbg_dev_err = (uint32_t)e;
    return;
  }
  s_dbg_dev_step = (uint32_t)k_cdc_dev_step_attach;

  static UCHAR s_echo_buf[k_cdc_echo_buf];
  while (1) {
    if (s_cdc_acm == UX_NULL) {
      (void)tx_semaphore_get(&s_cdc_active_sem, TX_WAIT_FOREVER);
      continue;
    }
    s_dbg_dev_step = (uint32_t)k_cdc_dev_step_echo;
    cdc_echo_iter(s_echo_buf, (ULONG)k_cdc_echo_buf);
  }
}

/**
 * @brief ThreadX application-define hook. Spawns both workers.
 *
 * @details Creates the activation semaphore, then the device worker at
 * priority 8 and the host worker at 24 (below the USBX class threads).
 * Sets ::s_tx_kernel_up so SysTick may feed ThreadX from here on.
 *
 * @param[in] first_unused_memory Sentinel (unused; static stacks).
 *
 * @pre Called from ``tx_kernel_enter`` after scheduler init.
 * @pre Static stacks are reserved at file scope.
 * @post ::s_cdc_active_sem exists and two auto-start workers are queued.
 * @post ``s_tx_kernel_up`` is true.
 *
 * @note Called once at boot; not thread-safe.
 * @since 0.1.0
 */
VOID tx_application_define(VOID* first_unused_memory)
{
  static CHAR s_semaphore_name[]     = "cdc_active";
  static CHAR s_device_thread_name[] = "cdc_device";
  static CHAR s_host_thread_name[]   = "cdc_host";

  (void)first_unused_memory;
  s_tx_kernel_up = true;
  (void)tx_semaphore_create(&s_cdc_active_sem, s_semaphore_name, 0U);
  (void)tx_thread_create(&s_device_thread,
                         s_device_thread_name,
                         cdc_device_worker,
                         0UL,
                         s_device_stack,
                         k_cdc_thread_stack,
                         (UINT)k_cdc_dev_priority,
                         (UINT)k_cdc_dev_priority,
                         TX_NO_TIME_SLICE,
                         TX_AUTO_START);
  (void)tx_thread_create(&s_host_thread,
                         s_host_thread_name,
                         cdc_host_worker,
                         0UL,
                         s_host_stack,
                         k_cdc_host_stack,
                         (UINT)k_cdc_host_priority,
                         (UINT)k_cdc_host_priority,
                         TX_NO_TIME_SLICE,
                         TX_AUTO_START);
}
#endif /* !RA8_OFF_TARGET */

/* -------------------------------------------------------------------------- */
/* Startup */
/* -------------------------------------------------------------------------- */

/**
 * @brief Halt forever in WFI -- panic stop on init failure.
 *
 * @details Last-resort stop; only a debugger or reset recovers.
 *
 * @pre Called only after a fatal boot error.
 * @pre Interrupts may be in any state.
 * @post CPU is parked.
 * @post No further code runs.
 *
 * @note Not reachable post-boot.
 * @since 0.1.0
 */
static void cdc_panic_halt(void)
{
  while (1) {
    __asm__ volatile("wfi");
  }
}

/**
 * @brief Open FS in the device role via the board facade, then arm HS as host.
 *
 * @details FS device: opened by ::ra8_board_usb_port_init, which owns the
 * pin identities and the VBUSEN strap. HS host: SW4-8 to Host via the U15 expander, PD07
 * HIGH (U18 supplies J7), P4_08 VBUS sense.
 *
 * @pre IOPORT and the U15 expander are reachable.
 * @pre Called once from ::cdc_setup_or_halt.
 * @post FS pins carry the device role, HS pins the host role.
 * @post PD07 is HIGH (J7 powered).
 *
 * @note Panic-halts on any routing failure.
 * @since 0.1.0
 */
static void cdc_route_usb_or_halt(void)
{
  /* One board call replaces the four-step FS choreography: it routes VBUS,
   * D+ and D- to the USBFS function and keeps VBUSEN a GPIO strapped LOW for
   * the device role. The pin identities are board facts, so they live in
   * libs/ra8_board_ek_ra8d2 rather than being re-declared here. */
  if (ra8_board_usb_port_init(k_ra8_board_usb_port_fs, k_ra8_board_usb_role_device) != k_ra8_ok) {
    cdc_panic_halt();
  }
  if (ra8_board_io_expander_set_usbhs_host_mode() != k_ra8_ok) {
    cdc_panic_halt();
  }
  if (ra8_gpio_output_init(k_cdc_pin_hs_pwr, k_ra8_level_high) != k_ra8_ok) {
    cdc_panic_halt();
  }
  if (ra8_pfs_route_peripheral(k_cdc_pin_hs_vbus, k_ra8_psel_usb_hs, "cdc.hs_vbus") != k_ra8_ok) {
    cdc_panic_halt();
  }
}

/**
 * @brief Bring CGC + both USB clocks + SysTick + SCI8 + LEDs + pins up.
 *
 * @details USBFS needs the 48 MHz PLL2 reference; USBHS needs its UTMI
 * PLL. SCI8 is the J-Link OB CDC console at 115200.
 *
 * @pre Reset_Handler finished C runtime init.
 * @pre SystemInit has run.
 * @post Console works; both USB ports' pins and clocks are live.
 * @post LED1/LED2 are initialized.
 *
 * @note Panic-halts on any failure; called once from main.
 * @since 0.1.0
 */
static void cdc_setup_or_halt(void)
{
  uint32_t cpuclk0_hz = 0U;
  const fw_clock_module_t core_module = {.kind = k_fw_clock_module_core, .index = 0U};
  if (ra8_cgc_init() != k_ra8_ok) {
    cdc_panic_halt();
  }
  if (ra8_cgc_usbfs_clock_enable() != k_ra8_ok) {
    cdc_panic_halt();
  }
  if (ra8_cgc_usbhs_pll_enable() != k_ra8_ok) {
    cdc_panic_halt();
  }
  if (fw_clock_rate_for(ra8_board_clock(), core_module, &cpuclk0_hz) != k_ra8_ok) {
    cdc_panic_halt();
  }
  if (ra8_time_init(cpuclk0_hz) != k_ra8_ok) {
    cdc_panic_halt();
  }
  if (ra8_board_uart_console_init((uint32_t)k_cdc_baud) != k_ra8_ok) {
    cdc_panic_halt();
  }
  if (ra8_board_led_init(k_ra8_board_led1) != k_ra8_ok) {
    cdc_panic_halt();
  }
  if (ra8_board_led_init(k_ra8_board_led2) != k_ra8_ok) {
    cdc_panic_halt();
  }
  cdc_route_usb_or_halt();
}

/**
 * @brief Application entry: bring the board up, then hand off to ThreadX.
 *
 * @details Both USB controllers' clocks and pins come up before the
 * kernel so the workers only deal with stack bring-up.
 *
 * @pre Reset_Handler copied .data and zeroed .bss.
 * @pre SystemInit set VTOR, FPU, priority grouping.
 * @post On clean entry the CPU stays in tx_kernel_enter forever.
 * @post On any HAL init failure the function halts in WFI.
 *
 * @note Single entry point; not re-entrant.
 * @since 0.1.0
 */
void main(void)
{
  cdc_setup_or_halt();

  ra8_isr_globals_enable();

#ifndef RA8_OFF_TARGET
  tx_kernel_enter();
#endif

  cdc_panic_halt();
}
