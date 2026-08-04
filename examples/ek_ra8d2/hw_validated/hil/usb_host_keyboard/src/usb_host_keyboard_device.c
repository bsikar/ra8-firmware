/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file examples/ek_ra8d2/hw_validated/hil/usb_host_keyboard/src/usb_host_keyboard_device.c
 * @brief USBX HID boot-keyboard device worker for usb_host_keyboard
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * The USBFS (J11) device half carved out of `usb_host_keyboard/main.c` so each
 * translation unit stays under the 1000-line cap. This TU owns the fake
 * boot-keyboard: the HID report + USB descriptor frameworks, the HID class
 * activate / deactivate callbacks, the USBX system + device-stack bring-up, the
 * interrupt-IN report send loop, and the device worker thread (TCB + stack).
 * main.c's `tx_application_define` spawns the worker through
 * ::usb_host_keyboard_device_thread_create. The shared report-pattern builder,
 * the shared activation semaphore, and the shared enums live in
 * `usb_host_keyboard_steps.h`. Pure code move -- no logic change.
 *
 * @author Brighton Sikarskie
 * @date 2026-06-13
 * @since 0.1.0
 */

#include <stdint.h>
#include <string.h>

#include "ra8_board_ek_ra8d2.h"
#include "ra8_err.h"
#include "ra8_usb.h"
#include "usb_host_keyboard_steps.h"

#ifndef RA8_OFF_TARGET
#include "tx_api.h"
#include "ux_api.h"
#include "ux_dcd_ra8_usb.h"
#include "ux_device_class_hid.h"
#include "ux_device_stack.h"

/* -------------------------------------------------------------------------- */
/* Device-worker progress markers */
/* -------------------------------------------------------------------------- */

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

/* USBX LANGID descriptor 0x0409 (English-US), little-endian byte pair. */
typedef enum : uint8_t {
  k_usb_langid_en_us_lo = 0x09U, /**< LANGID 0x0409 low byte.  */
  k_usb_langid_en_us_hi = 0x04U, /**< LANGID 0x0409 high byte. */
} usb_langid_byte_t;

/* -------------------------------------------------------------------------- */
/* Typed keycodes the fake keyboard "types" */
/* -------------------------------------------------------------------------- */

/** @brief Keycodes the fake keyboard "types": R, A, 8, D, 2. */
static const uint8_t s_kbd_keys[k_hid_nkeys] = {
  (uint8_t)k_hid_kc_r,
  (uint8_t)k_hid_kc_a,
  (uint8_t)k_hid_kc_8,
  (uint8_t)k_hid_kc_d,
  (uint8_t)k_hid_kc_2,
};

/* -------------------------------------------------------------------------- */
/* ThreadX worker + USBX pool storage */
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

/* -------------------------------------------------------------------------- */
/* J-Link probes (device side) */
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

/* USB HID boot-keyboard report descriptor (HID 1.11 Appendix B.1), input-only
 * variant (no LED output report, so no OUT endpoint / SET_REPORT is needed):
 * an 8-byte input report [modifier][reserved][keycode x6]. 45 bytes (0x2D). The
 * host enumerates this as a real boot keyboard (interface subclass 1 /
 * protocol 1) and decodes the keycodes in bytes 2.. back to ASCII. */
static UCHAR s_report_descriptor[] = {
  0x05U, 0x01U, /* Usage Page (Generic Desktop)      */
  0x09U, 0x06U, /* Usage (Keyboard)                  */
  0xA1U, 0x01U, /* Collection (Application)          */
  0x05U, 0x07U, /* Usage Page (Keyboard/Keypad)      */
  0x19U, 0xE0U, /* Usage Minimum (Left Control)      */
  0x29U, 0xE7U, /* Usage Maximum (Right GUI)         */
  0x15U, 0x00U, /* Logical Minimum (0)               */
  0x25U, 0x01U, /* Logical Maximum (1)               */
  0x75U, 0x01U, /* Report Size (1)                   */
  0x95U, 0x08U, /* Report Count (8) -- modifier bits */
  0x81U, 0x02U, /* Input (Data,Var,Abs)              */
  0x95U, 0x01U, /* Report Count (1)                  */
  0x75U, 0x08U, /* Report Size (8) -- reserved byte  */
  0x81U, 0x01U, /* Input (Const)                     */
  0x95U, 0x06U, /* Report Count (6) -- key array     */
  0x75U, 0x08U, /* Report Size (8)                   */
  0x15U, 0x00U, /* Logical Minimum (0)               */
  0x25U, 0x65U, /* Logical Maximum (101)             */
  0x05U, 0x07U, /* Usage Page (Keyboard/Keypad)      */
  0x19U, 0x00U, /* Usage Minimum (0)                 */
  0x29U, 0x65U, /* Usage Maximum (101)               */
  0x81U, 0x00U, /* Input (Data,Array) -- 6 keycodes  */
  0xC0U,        /* End Collection                    */
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
  /* Interface descriptor -- HID, boot subclass (1), keyboard protocol (1). */
  0x09U,
  0x04U,
  0x00U,
  0x00U,
  0x01U,
  0x03U,
  0x01U, /* bInterfaceSubClass = 1 (Boot)     */
  0x01U, /* bInterfaceProtocol = 1 (Keyboard) */
  0x00U,
  /* HID class descriptor (HID 1.11 sec 6.2.1) -- 9 bytes. bcdHID 0x0111,
     one report descriptor of sizeof(s_report_descriptor) = 45 (0x2D). */
  0x09U,
  0x21U,
  0x11U,
  0x01U,
  0x00U,
  0x01U,
  0x22U,
  0x2DU, /* wDescriptorLength = 45 (boot-keyboard report descriptor) */
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

/**
 * @var s_language_id_framework
 * @brief USBX language-id table -- US English.
 * @since 0.1.0
 */
static UCHAR s_language_id_framework[] = {k_usb_langid_en_us_lo, k_usb_langid_en_us_hi};

/* -------------------------------------------------------------------------- */
/* Shared HID report pattern */
/* -------------------------------------------------------------------------- */

void hid_fill_report_body(uint8_t* out, uint32_t len)
{
  /* Boot-keyboard report: byte 1 = reserved, bytes 2.. = up to six keycodes
   * (the typed "RA8D2"), the remainder 0 (no key). Byte 0 (modifier) is set by
   * the caller. Deterministic, so the host computes the same body to verify. */
  for (uint32_t i = (uint32_t)k_hid_body_idx; i < len; i++) {
    out[i] = 0U;
  }
  for (uint32_t i = 0U; i < (uint32_t)k_hid_nkeys; i++) {
    const uint32_t idx = (uint32_t)k_hid_key0_idx + i;
    if (idx < len) {
      out[idx] = s_kbd_keys[i];
    }
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
 * @post ::s_usb_host_keyboard_hid_active_sem is posted so the send worker runs.
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
  (void)tx_semaphore_put(&s_usb_host_keyboard_hid_active_sem);
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

VOID hid_device_worker(ULONG arg)
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
      (void)tx_semaphore_get(&s_usb_host_keyboard_hid_active_sem, TX_WAIT_FOREVER);
      continue;
    }
    s_dbg_dev_step = (uint32_t)k_hid_dev_step_send;
    hid_send_iter(&seq);
  }
}

void usb_host_keyboard_device_thread_create(void)
{
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
}

#endif /* !RA8_OFF_TARGET */
