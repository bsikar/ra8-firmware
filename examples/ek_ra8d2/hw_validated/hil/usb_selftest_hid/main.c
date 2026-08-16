/**
 * @file examples/ek_ra8d2/hw_validated/hil/usb_selftest_hid/main.c
 * @brief USB self-loop: HS host reads + verifies a HID device's interrupt reports
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * The HID interrupt-IN self-loop -- it exercises the device->host
 * interrupt report path through a real HID device class. The two USB
 * ports are cabled to EACH OTHER and one firmware image runs both USB
 * stacks:
 *
 *  - USBFS (J11) = DEVICE: a ThreadX + USBX HID class with a vendor
 *    8-byte input report. A worker continuously queues reports with
 *    `_ux_device_class_hid_event_set` on the interrupt-IN endpoint; each
 *    report is { rolling seq, fixed 7-byte pattern }. IRQ-driven through
 *    the `port/usbx/ux_dcd_ra8_usb` bridge.
 *  - USBHS (J7) = HOST: a self-contained polled host built on the
 *    first-party `ra8_usb_host_*` primitives. It enumerates the device
 *    (bus reset -> GET_DESCRIPTOR -> SET_ADDRESS -> SET_CONFIGURATION),
 *    opens the HID interrupt-IN endpoint (EP1 IN) as a receive pipe, then
 *    polls several reports, byte-checking the fixed pattern in each --
 *    proving the device interrupt-IN report path delivers intact, end to
 *    end on chip.
 *
 * Each report is 8 bytes (well under the 64-byte endpoint MPS), so it
 * arrives as a single short packet. The seq byte advances per report so
 * the host can also see the reports are fresh, not one stale buffer.
 * No OS HID driver is involved; raw interrupt-IN reads only.
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
#include "usb_selftest_hid_steps.h"

#ifndef RA8_OFF_TARGET
#include "tx_api.h"
#include "ux_api.h"
#include "ux_dcd_ra8_usb.h"
#include "ux_device_class_hid.h"
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

/** @brief USBFS VBUS sense pin (P4_07, PSEL = 0x13). */
static const ra8_port_pin_t k_hid_pin_fs_vbus = (ra8_port_pin_t)k_ra8_board_usbfs_pin_vbus;

/** @brief USBFS VBUSEN (P5_00) -- GPIO LOW for the device role. */
static const ra8_port_pin_t k_hid_pin_fs_vbusen = (ra8_port_pin_t)k_ra8_board_usbfs_pin_vbusen;

/** @brief USBFS D+ (P8_14). */
static const ra8_port_pin_t k_hid_pin_fs_dp = (ra8_port_pin_t)k_ra8_board_usbfs_pin_dp;

/** @brief USBFS D- (P8_15). */
static const ra8_port_pin_t k_hid_pin_fs_dm = (ra8_port_pin_t)k_ra8_board_usbfs_pin_dm;

/** @brief USBHS_VBUS sense pin (P4_08, PSEL = 0x14). */
static const ra8_port_pin_t k_hid_pin_hs_vbus = (ra8_port_pin_t)k_ra8_board_usbhs_pin_vbus;

/** @brief J7 host-power switch (PD07): HIGH = U18 supplies VBUS (UM 6.2). */
static const ra8_port_pin_t k_hid_pin_hs_pwr = (ra8_port_pin_t)k_ra8_board_usbhs_pin_pwr;

/* -------------------------------------------------------------------------- */
/* Tunables */
/* -------------------------------------------------------------------------- */

/*
 * The compile-time setting enums shared across the three translation units --
 * ::hid_config_t, ::hid_hex_t, ::hid_geom_t -- live in
 * `usb_selftest_hid_steps.h`. The host-only enums ::hid_phase_t,
 * ::hid_usb_req_t, and ::hid_enum_tune_t are private to the host cluster in
 * `src/usb_selftest_hid_host.c`; the console-only ::hid_mask_t is private to
 * `src/usb_selftest_hid_console.c`. Only the device-progress enum below stays
 * in this unit.
 */

/**
 * @enum hid_dev_step_t
 * @brief J-Link probe values marking device-worker bring-up progress.
 */
typedef enum : uint32_t {
  k_hid_dev_step_stack  = 1U, /**< USBX system + device stack up. */
  k_hid_dev_step_class  = 2U, /**< HID class registered.          */
  k_hid_dev_step_dcd    = 3U, /**< DCD bridge initialized.        */
  k_hid_dev_step_attach = 4U, /**< Device attached (DPRPU).       */
  k_hid_dev_step_send   = 5U, /**< Report-send loop running.      */
} hid_dev_step_t;

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
static UCHAR s_device_stack[k_hid_thread_stack];

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
static UCHAR s_host_stack[k_hid_host_stack];

/**
 * @var s_usbx_pool
 * @brief USBX memory pool (USBX uses ``tx_byte_pool`` internally).
 * @since 0.1.0
 */
static UCHAR s_usbx_pool[k_hid_usbx_pool_bytes];

/**
 * @var s_hid_class
 * @brief Active HID class instance, captured by the activate callback.
 * @note Written by the USBX class thread; read by the device send worker.
 * @since 0.1.0
 */
static UX_SLAVE_CLASS_HID* s_hid_class = UX_NULL;

/**
 * @var s_hid_active_sem
 * @brief Posted by the activate callback so the send worker blocks on it
 *        instead of polling ``s_hid_class`` with tx_thread_sleep (which has
 *        been observed never returning on this silicon under load).
 * @note Single-producer (class thread), single-consumer (send worker).
 * @since 0.1.0
 */
static TX_SEMAPHORE s_hid_active_sem;

/* -------------------------------------------------------------------------- */
/* J-Link probes (device side; host-side probes live in the host cluster) */
/* -------------------------------------------------------------------------- */

/** @brief Device-side report-queue successes (one hid_event_set each). */
static volatile uint32_t s_dbg_dev_sent;
/** @brief Device worker progress: 1 stack, 2 class, 3 dcd, 4 attach, 5 send. */
static volatile uint32_t s_dbg_dev_step;
/** @brief Device worker first failing return code (0 = none). */
static volatile uint32_t s_dbg_dev_err;

/* -------------------------------------------------------------------------- */
/* HID Report Descriptor (vendor-defined, one 8-byte input report) */
/* -------------------------------------------------------------------------- */

/* A minimal vendor-defined collection with a single 8-byte input report
 * (Report Size 8 bits x Report Count 8). 21 bytes total. The host does not
 * parse this -- it only needs the device to expose an interrupt-IN report
 * of a known width -- but a real HID device must publish a report
 * descriptor, so the enumeration is genuinely HID-class. */
static UCHAR s_report_descriptor[] = {
  0x06U, 0x00U, 0xFFU, /* Usage Page (Vendor Defined 0xFF00) */
  0x09U, 0x01U,        /* Usage (0x01)                       */
  0xA1U, 0x01U,        /* Collection (Application)           */
  0x09U, 0x01U,        /* Usage (0x01)                       */
  0x15U, 0x00U,        /* Logical Minimum (0)                */
  0x26U, 0xFFU, 0x00U, /* Logical Maximum (255)              */
  0x75U, 0x08U,        /* Report Size (8 bits)               */
  0x95U, 0x08U,        /* Report Count (8)                   */
  0x81U, 0x02U,        /* Input (Data,Var,Abs)               */
  0xC0U,               /* End Collection                     */
};

/* -------------------------------------------------------------------------- */
/* USB descriptors (HID: one interface, one interrupt-IN endpoint) */
/* -------------------------------------------------------------------------- */

/* HID config: one HID interface (class 0x03, no boot subclass), one
 * interrupt-IN endpoint EP1 IN (64-byte MPS), no OUT endpoint. Total config
 * blob = 9 (config) + 9 (interface) + 9 (HID class) + 7 (EP IN) = 34 = 0x22.
 * Layout per USB 2.0 sec 9.6 + HID 1.11 sec 6.2.1. PID 0x0018 marks the HID
 * self-test identity. */
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
  0x18U, /* PID = 0x0018 (pid.codes test). */
  0x00U,
  0x00U,
  0x01U,
  0x01U,
  0x02U,
  0x03U,
  0x01U,
  /* Configuration descriptor (34 bytes total = 0x22). */
  0x09U,
  0x02U,
  0x22U,
  0x00U,
  0x01U,
  0x01U,
  0x00U,
  0x80U,
  0x32U,
  /* Interface descriptor -- HID, no boot subclass, no protocol. */
  0x09U,
  0x04U,
  0x00U,
  0x00U,
  0x01U,
  0x03U,
  0x00U,
  0x00U,
  0x00U,
  /* HID class descriptor (HID 1.11 sec 6.2.1) -- 9 bytes. bcdHID 0x0111,
     one report descriptor of sizeof(s_report_descriptor) = 21 (0x15). */
  0x09U,
  0x21U,
  0x11U,
  0x01U,
  0x00U,
  0x01U,
  0x22U,
  0x15U,
  0x00U,
  /* Interrupt-IN endpoint (EP1 IN, 64-byte MPS, 1 ms poll). */
  0x07U,
  0x05U,
  0x81U,
  0x03U,
  0x40U,
  0x00U,
  0x01U,
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
  /* idx 2: "RA8D2 HID TEST". */
  0x09U,
  0x04U,
  0x02U,
  0x0EU,
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
  'T',
  'E',
  'S',
  'T',
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
  '1',
  '3',
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
/* Shared HID report pattern */
/* -------------------------------------------------------------------------- */

/** @brief Implementation of `hid_fill_report_body()` -- fixed `i*7 + 0x5A` body. */
void hid_fill_report_body(uint8_t* out, uint32_t len)
{
  for (uint32_t i = (uint32_t)k_hid_body_idx; i < len; i++) {
    const uint32_t v = (i * (uint32_t)k_hid_pat_idx_mul) + (uint32_t)k_hid_pat_bias;
    out[i]           = (uint8_t)(v & (uint32_t)k_hid_byte_mask);
  }
}

/* -------------------------------------------------------------------------- */
/* HID activate / deactivate callbacks */
/* -------------------------------------------------------------------------- */

/**
 * @brief HID activate callback. Captures the live class instance.
 *
 * @details Pins ``ux_slave_device_state`` at CONFIGURED (works around a
 * residual DVSQ-poll race on this silicon that can demote it back to
 * ATTACHED after SET_CONFIGURATION), then posts the semaphore the send
 * worker blocks on so it begins queuing reports.
 *
 * @param[in] hid_instance Pointer to ``UX_SLAVE_CLASS_HID``.
 *
 * @pre Called from the USBX class thread.
 * @pre SET_CONFIGURATION has just configured the device.
 * @post ``s_hid_class`` points at the live class.
 * @post ::s_hid_active_sem is posted so the send worker runs.
 *
 * @note USBX serializes this with the deactivate callback.
 * @since 0.1.0
 */
static VOID hid_activate(VOID* hid_instance)
{
  s_hid_class = (UX_SLAVE_CLASS_HID*)hid_instance;
  if (_ux_system_slave != UX_NULL) {
    _ux_system_slave->ux_system_slave_device.ux_slave_device_state =
      (unsigned long)UX_DEVICE_CONFIGURED;
  }
  (void)tx_semaphore_put(&s_hid_active_sem);
}

/**
 * @brief HID deactivate callback. Drops the live class pointer.
 *
 * @param[in] hid_instance Unused.
 *
 * @pre Called from the USBX class thread.
 * @pre The HID class is being torn down.
 * @post ``s_hid_class`` is ``UX_NULL``.
 * @post The send worker blocks until the next activate.
 *
 * @note USBX serializes this with the activate callback.
 * @since 0.1.0
 */
static VOID hid_deactivate(VOID* hid_instance)
{
  (void)hid_instance;
  s_hid_class = UX_NULL;
}

/* -------------------------------------------------------------------------- */
/* Device side: USBX HID interrupt-IN reports */
/* -------------------------------------------------------------------------- */

/**
 * @brief Bring USBX system + device stack up with the HID framework.
 *
 * @details One-shot USBX pool + device-stack init (FS-only framework).
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
static UINT hid_usbx_stack_up(void)
{
  if (_ux_system_initialize(s_usbx_pool, k_hid_usbx_pool_bytes, UX_NULL, 0) != UX_SUCCESS) {
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
 * @brief HID GET_REPORT control-pipe callback: hand back a neutral report.
 *
 * @details The host in this self-loop only reads the interrupt-IN pipe and
 * never issues GET_REPORT, but USBX requires a get callback. Fill the same
 * fixed body with a zero seq so any control GET_REPORT is well-formed.
 *
 * @param[in,out] hid       USBX HID class instance (unused).
 * @param[in,out] hid_event Pre-allocated event slot to fill.
 *
 * @return Always ``UX_SUCCESS``.
 * @retval UX_SUCCESS The event buffer holds a neutral report.
 *
 * @pre @p hid_event is non-NULL (USBX guarantee).
 * @pre The HID class is live.
 * @post The event buffer holds a ::k_hid_report_len-byte report.
 * @post No global state changes.
 *
 * @note Called from USBX's control-pipe thread.
 * @since 0.1.0
 */
static UINT hid_get_callback(UX_SLAVE_CLASS_HID* hid, UX_SLAVE_CLASS_HID_EVENT* hid_event)
{
  (void)hid;
  hid_event->ux_device_class_hid_event_length      = (ULONG)k_hid_report_len;
  hid_event->ux_device_class_hid_event_report_id   = 0UL;
  hid_event->ux_device_class_hid_event_report_type = (ULONG)UX_DEVICE_CLASS_HID_REPORT_TYPE_INPUT;
  hid_fill_report_body(hid_event->ux_device_class_hid_event_buffer, (uint32_t)k_hid_report_len);
  hid_event->ux_device_class_hid_event_buffer[k_hid_seq_idx] = 0U;
  return UX_SUCCESS;
}

/**
 * @brief Register the HID class against configuration 1, interface 0.
 *
 * @details Binds ::hid_activate / ::hid_deactivate and publishes the vendor
 * report descriptor so the host's enumeration sees a real HID interface.
 *
 * @return UINT UX_SUCCESS on success, propagated USBX error otherwise.
 * @retval UX_SUCCESS Class registered.
 *
 * @pre ::hid_usbx_stack_up has succeeded.
 * @pre ::s_report_descriptor is at file scope and valid.
 * @post The HID class is bound; the activate callback will fire on
 *       SET_CONFIGURATION.
 * @post No other class is registered.
 *
 * @note Not re-entrant.
 * @since 0.1.0
 */
static UINT hid_class_register(void)
{
  UX_SLAVE_CLASS_HID_PARAMETER hid_params = {
    .ux_slave_class_hid_instance_activate         = hid_activate,
    .ux_slave_class_hid_instance_deactivate       = hid_deactivate,
    .ux_device_class_hid_parameter_report_address = s_report_descriptor,
    .ux_device_class_hid_parameter_report_id      = 0UL,
    .ux_device_class_hid_parameter_report_length  = (ULONG)sizeof(s_report_descriptor),
    .ux_device_class_hid_parameter_callback       = UX_NULL,
    .ux_device_class_hid_parameter_get_callback   = hid_get_callback,
  };
  return _ux_device_stack_class_register((UCHAR*)"ux_slave_class_hid",
                                         _ux_device_class_hid_entry,
                                         1,
                                         0,
                                         &hid_params);
}

/**
 * @brief One device send iteration: queue a fresh input report.
 *
 * @details Pins CONFIGURED (DVSQ-poll race guard), builds a report
 * { seq, fixed body }, queues it with `_ux_device_class_hid_event_set` on
 * the interrupt-IN endpoint, and yields one tick. The yield lets the
 * lower-priority host thread drain the queue; when the queue is full the
 * event_set fails harmlessly and the seq does not advance.
 *
 * @param[in,out] seq Rolling report sequence; advanced on a queued report.
 *
 * @pre ``s_hid_class`` is non-NULL (class activated).
 * @pre @p seq is non-NULL.
 * @post On a queued report ::s_dbg_dev_sent advanced and @p seq incremented.
 * @post The worker yielded one tick.
 *
 * @note Runs on the device worker thread.
 * @since 0.1.0
 */
static void hid_send_iter(uint32_t* seq)
{
  if (_ux_system_slave != UX_NULL) {
    _ux_system_slave->ux_system_slave_device.ux_slave_device_state =
      (unsigned long)UX_DEVICE_CONFIGURED;
  }
  UX_SLAVE_CLASS_HID_EVENT ev;
  (void)memset(&ev, 0, sizeof(ev));
  ev.ux_device_class_hid_event_length      = (ULONG)k_hid_report_len;
  ev.ux_device_class_hid_event_report_id   = 0UL;
  ev.ux_device_class_hid_event_report_type = (ULONG)UX_DEVICE_CLASS_HID_REPORT_TYPE_INPUT;
  hid_fill_report_body(ev.ux_device_class_hid_event_buffer, (uint32_t)k_hid_report_len);
  ev.ux_device_class_hid_event_buffer[k_hid_seq_idx] = (UCHAR)(*seq & (uint32_t)k_hid_byte_mask);
  if (_ux_device_class_hid_event_set(s_hid_class, &ev) == UX_SUCCESS) {
    (*seq)++;
    s_dbg_dev_sent++;
    (void)ra8_board_led_toggle(k_ra8_board_led1);
  }
  tx_thread_sleep(1U);
}

/**
 * @brief Device-side worker: bring the HID device up, then send reports.
 *
 * @details USBX system + device stack + HID class + DCD bridge on the USBFS
 * controller, then DPRPU attach. Blocks on ::s_hid_active_sem until the host
 * configures the device, then loops ::hid_send_iter to keep the interrupt-IN
 * report queue fed.
 *
 * @param[in] arg ThreadX entry argument (unused).
 *
 * @pre tx_application_define created this thread.
 * @pre USB-FS pins + 48 MHz clock are up (main did both).
 * @post The FS device is attached and streaming input reports.
 * @post On any bring-up failure the thread exits.
 *
 * @note Runs once; loops forever on success.
 * @since 0.1.0
 */
static VOID hid_device_worker(ULONG arg)
{
  (void)arg;

  UINT ux = hid_usbx_stack_up();
  if (ux != UX_SUCCESS) {
    s_dbg_dev_err = (uint32_t)ux;
    return;
  }
  s_dbg_dev_step = (uint32_t)k_hid_dev_step_stack;
  ux             = hid_class_register();
  if (ux != UX_SUCCESS) {
    s_dbg_dev_err = (uint32_t)ux;
    return;
  }
  s_dbg_dev_step = (uint32_t)k_hid_dev_step_class;
  ra8_err_t e    = ux_dcd_ra8_usb_initialize(k_ra8_usb_speed_fs);
  if (e != k_ra8_ok) {
    s_dbg_dev_err = (uint32_t)e;
    return;
  }
  s_dbg_dev_step = (uint32_t)k_hid_dev_step_dcd;
  e              = ra8_usb_device_attach(k_ra8_usb_speed_fs, true);
  if (e != k_ra8_ok) {
    s_dbg_dev_err = (uint32_t)e;
    return;
  }
  s_dbg_dev_step = (uint32_t)k_hid_dev_step_attach;

  uint32_t seq = 0U;
  while (1) {
    if (s_hid_class == UX_NULL) {
      (void)tx_semaphore_get(&s_hid_active_sem, TX_WAIT_FOREVER);
      continue;
    }
    s_dbg_dev_step = (uint32_t)k_hid_dev_step_send;
    hid_send_iter(&seq);
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
 * @post ::s_hid_active_sem exists and two auto-start workers are queued.
 * @post ``s_tx_kernel_up`` is true.
 *
 * @note Called once at boot; not thread-safe.
 * @since 0.1.0
 */
VOID tx_application_define(VOID* first_unused_memory)
{
  (void)first_unused_memory;
  s_tx_kernel_up = true;
  (void)tx_semaphore_create(&s_hid_active_sem, (CHAR*)"hid_active", 0U);
  (void)tx_thread_create(&s_device_thread,
                         "hid_device",
                         hid_device_worker,
                         0UL,
                         s_device_stack,
                         k_hid_thread_stack,
                         (UINT)k_hid_dev_priority,
                         (UINT)k_hid_dev_priority,
                         TX_NO_TIME_SLICE,
                         TX_AUTO_START);
  (void)tx_thread_create(&s_host_thread,
                         "hid_host",
                         hid_host_worker,
                         0UL,
                         s_host_stack,
                         k_hid_host_stack,
                         (UINT)k_hid_host_priority,
                         (UINT)k_hid_host_priority,
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
static void hid_panic_halt(void)
{
  while (1) {
    __asm__ volatile("wfi");
  }
}

/**
 * @brief Route both ports' pins: FS as device, HS as host.
 *
 * @details FS device: P4_07 VBUS sense, P5_00 VBUSEN GPIO LOW (else
 * peripheral routing forces host VBUSEN and blocks device enum),
 * P8_14/P8_15 data. HS host: SW4-8 to Host via the U15 expander, PD07
 * HIGH (U18 supplies J7), P4_08 VBUS sense.
 *
 * @pre IOPORT and the U15 expander are reachable.
 * @pre Called once from ::hid_setup_or_halt.
 * @post FS pins carry the device role, HS pins the host role.
 * @post PD07 is HIGH (J7 powered).
 *
 * @note Panic-halts on any routing failure.
 * @since 0.1.0
 */
static void hid_route_usb_or_halt(void)
{
  if (ra8_pfs_route_peripheral(k_hid_pin_fs_vbus, k_ra8_psel_usb_fs, "hid.fs_vbus") != k_ra8_ok) {
    hid_panic_halt();
  }
  if (ra8_gpio_output_init(k_hid_pin_fs_vbusen, k_ra8_level_low) != k_ra8_ok) {
    hid_panic_halt();
  }
  if (ra8_pfs_route_peripheral(k_hid_pin_fs_dp, k_ra8_psel_usb_fs, "hid.fs_dp") != k_ra8_ok) {
    hid_panic_halt();
  }
  if (ra8_pfs_route_peripheral(k_hid_pin_fs_dm, k_ra8_psel_usb_fs, "hid.fs_dm") != k_ra8_ok) {
    hid_panic_halt();
  }
  if (ra8_board_io_expander_set_usbhs_host_mode() != k_ra8_ok) {
    hid_panic_halt();
  }
  if (ra8_gpio_output_init(k_hid_pin_hs_pwr, k_ra8_level_high) != k_ra8_ok) {
    hid_panic_halt();
  }
  if (ra8_pfs_route_peripheral(k_hid_pin_hs_vbus, k_ra8_psel_usb_hs, "hid.hs_vbus") != k_ra8_ok) {
    hid_panic_halt();
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
static void hid_setup_or_halt(void)
{
  uint32_t cpuclk0_hz = 0U;
  if (ra8_cgc_init() != k_ra8_ok) {
    hid_panic_halt();
  }
  if (ra8_cgc_usbfs_clock_enable() != k_ra8_ok) {
    hid_panic_halt();
  }
  if (ra8_cgc_usbhs_pll_enable() != k_ra8_ok) {
    hid_panic_halt();
  }
  if (ra8_cgc_get_clock_hz(k_ra8_clock_id_cpuclk0, &cpuclk0_hz) != k_ra8_ok) {
    hid_panic_halt();
  }
  if (ra8_time_init(cpuclk0_hz) != k_ra8_ok) {
    hid_panic_halt();
  }
  if (ra8_board_uart_console_init((uint32_t)k_hid_baud) != k_ra8_ok) {
    hid_panic_halt();
  }
  if (ra8_board_led_init(k_ra8_board_led1) != k_ra8_ok) {
    hid_panic_halt();
  }
  if (ra8_board_led_init(k_ra8_board_led2) != k_ra8_ok) {
    hid_panic_halt();
  }
  hid_route_usb_or_halt();
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
  hid_setup_or_halt();

  ra8_isr_globals_enable();

#ifndef RA8_OFF_TARGET
  tx_kernel_enter();
#endif

  hid_panic_halt();
}
