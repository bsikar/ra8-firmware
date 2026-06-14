/**
 * @file examples/ek_ra8d2/hw_pending/usb_selftest_hid/main.c
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
 *    the `port/usbx/ux_dcd_ra_usb` bridge.
 *  - USBHS (J7) = HOST: a self-contained polled host built on the
 *    first-party `ra_usb_host_*` primitives. It enumerates the device
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

#include "ra_board_ek_ra8d2.h"
#include "ra_cgc.h"
#include "ra_err.h"
#include "ra_gpio_constants.h"
#include "ra_isr.h"
#include "ra_port_constants.h"
#include "ra_port_utils.h"
#include "ra_sci.h"
#include "ra_time.h"
#include "ra_usb.h"

#ifndef RA_SIMULATOR_MODE
#include "tx_api.h"
#include "ux_api.h"
#include "ux_dcd_ra_usb.h"
#include "ux_device_class_hid.h"
#include "ux_device_stack.h"

/* Strong SysTick override: route the tick into BOTH the ra_time millisecond
 * counter (for ra_delay_ms and the polled host stack's timeouts) AND
 * ThreadX's timer; the 1 ms pulse also recovers the DCD's storm-guard mask. */
extern void ra_time_on_tick(void);
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
  ra_time_on_tick();
  if (s_tx_kernel_up) {
    _tx_timer_interrupt();
    ux_dcd_ra_usb_irq_reenable();
  }
}
#endif

/* -------------------------------------------------------------------------- */
/* Pinout (FSP-aligned, EK-RA8D2 v1 User's Manual)                            */
/* -------------------------------------------------------------------------- */

/** @brief USBFS VBUS sense pin (P4_07, PSEL = 0x13). */
static const ra_port_pin_t k_hid_pin_fs_vbus =
  (ra_port_pin_t)(((uint16_t)k_ra_port_4 << 8) | (uint16_t)k_ra_pin_7);

/** @brief USBFS VBUSEN (P5_00) -- GPIO LOW for the device role. */
static const ra_port_pin_t k_hid_pin_fs_vbusen =
  (ra_port_pin_t)(((uint16_t)k_ra_port_5 << 8) | (uint16_t)k_ra_pin_0);

/** @brief USBFS D+ (P8_14). */
static const ra_port_pin_t k_hid_pin_fs_dp =
  (ra_port_pin_t)(((uint16_t)k_ra_port_8 << 8) | (uint16_t)k_ra_pin_14);

/** @brief USBFS D- (P8_15). */
static const ra_port_pin_t k_hid_pin_fs_dm =
  (ra_port_pin_t)(((uint16_t)k_ra_port_8 << 8) | (uint16_t)k_ra_pin_15);

/** @brief USBHS_VBUS sense pin (P4_08, PSEL = 0x14). */
static const ra_port_pin_t k_hid_pin_hs_vbus =
  (ra_port_pin_t)(((uint16_t)k_ra_port_4 << 8) | (uint16_t)k_ra_pin_8);

/** @brief J7 host-power switch (PD07): HIGH = U18 supplies VBUS (UM 6.2). */
static const ra_port_pin_t k_hid_pin_hs_pwr =
  (ra_port_pin_t)(((uint16_t)k_ra_port_13 << 8) | (uint16_t)k_ra_pin_7);

/** @brief J-Link OB CDC TX pin (PD_02 -- SCI8 TX). */
static const ra_port_pin_t k_hid_pin_sci_tx =
  (ra_port_pin_t)(((uint16_t)k_ra_port_13 << 8) | (uint16_t)k_ra_pin_2);

/** @brief J-Link OB CDC RX pin (PD_03 -- SCI8 RX). */
static const ra_port_pin_t k_hid_pin_sci_rx =
  (ra_port_pin_t)(((uint16_t)k_ra_port_13 << 8) | (uint16_t)k_ra_pin_3);

/* -------------------------------------------------------------------------- */
/* Tunables                                                                   */
/* -------------------------------------------------------------------------- */

/**
 * @enum hid_config_t
 * @brief Compile-time settings: threads, pool, console, cadence.
 */
typedef enum : uint32_t {
  k_hid_thread_stack    = 4096U,   /**< Device worker stack (bytes).      */
  k_hid_host_stack      = 8192U,   /**< Host worker stack (bytes).        */
  k_hid_usbx_pool_bytes = 32768U,  /**< USBX memory pool (bytes).         */
  k_hid_idle_ticks      = 50U,     /**< Parked-loop back-off (ticks).     */
  k_hid_boot_wait_ticks = 500U,    /**< Host start delay (1 ms ticks).    */
  k_hid_retry_ticks     = 3000U,   /**< Pause between ladder retries.     */
  k_hid_baud            = 115200U, /**< J-Link OB CDC log baud.           */
  k_hid_sci_channel     = 8U,      /**< SCI8 -> J-Link OB CDC bridge.     */
  k_hid_print_cap       = 160U,    /**< Bound for console-string scans.   */
  k_hid_dev_priority    = 8U,      /**< Device bring-up worker priority.  */
  k_hid_host_priority   = 24U,     /**< Host worker priority (below USBX). */
} hid_config_t;

/**
 * @enum hid_hex_t
 * @brief Hex/decimal text-formatter sizing constants.
 */
typedef enum : uint8_t {
  k_hid_hex_chars_u16   = 4U,  /**< 16-bit value -> "ABCD".         */
  k_hid_hex_chars_u32   = 8U,  /**< 32-bit value -> "ABCDEF01".     */
  k_hid_dec_chars_u32   = 10U, /**< Max digits for a 32-bit count.  */
  k_hid_nibble_bits     = 4U,  /**< Bits per hex nibble.            */
  k_hid_hex_digit_split = 10U, /**< Threshold between '0-9'/'A-F'.  */
} hid_hex_t;

/**
 * @enum hid_mask_t
 * @brief Bit-mask constants used by the text formatters.
 */
typedef enum : uint32_t {
  k_hid_nibble_mask = 0xFU, /**< 4-bit nibble mask.           */
  k_hid_dec_radix   = 10U,  /**< Base for decimal conversion. */
} hid_mask_t;

/**
 * @enum hid_geom_t
 * @brief HID report + interrupt-pipe + pattern constants.
 */
typedef enum : uint32_t {
  k_hid_mps         = 64U,         /**< Interrupt endpoint wMaxPacketSize. */
  k_hid_report_len  = 8U,          /**< Vendor input report width (bytes). */
  k_hid_rounds      = 8U,          /**< Reports the host reads + checks.   */
  k_hid_read_buf    = 64U,         /**< One-MPS receive buffer (bytes).    */
  k_hid_dev_addr    = 1U,          /**< Address the host assigns.          */
  k_hid_ep_in_num   = 1U,          /**< Device interrupt-IN endpoint num.  */
  k_hid_pipe_in     = 1U,          /**< Host pipe for the device IN.       */
  k_hid_seq_idx     = 0U,          /**< Report byte 0: rolling seq.        */
  k_hid_body_idx    = 1U,          /**< Report body starts at byte 1.      */
  k_hid_no_mismatch = 0xFFFFFFFFU, /**< Probe: no mismatch.                */
  k_hid_pat_idx_mul = 7U,          /**< Per-index pattern multiplier.      */
  k_hid_pat_bias    = 0x5AU,       /**< Pattern constant bias.             */
  k_hid_byte_mask   = 0xFFU,       /**< Byte mask.                         */
} hid_geom_t;

/**
 * @enum hid_phase_t
 * @brief J-Link probe values marking host-ladder progress.
 */
typedef enum : uint32_t {
  k_hid_phase_boot   = 0U, /**< Host thread not started.    */
  k_hid_phase_init   = 1U, /**< Host controller init.       */
  k_hid_phase_enum   = 2U, /**< Enumerating.                */
  k_hid_phase_verify = 3U, /**< Reading + checking reports. */
  k_hid_phase_pass   = 4U, /**< All reports verified.       */
} hid_phase_t;

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

#ifndef RA_SIMULATOR_MODE

/* -------------------------------------------------------------------------- */
/* ThreadX workers + USBX pool storage                                        */
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
/* J-Link probes                                                              */
/* -------------------------------------------------------------------------- */

/** @brief Host-ladder phase marker (::hid_phase_t). */
static volatile uint32_t s_dbg_phase;
/** @brief Reports the host confirmed pattern-equal (expect ::k_hid_rounds). */
static volatile uint32_t s_dbg_rounds_ok;
/** @brief Device-reported product id captured at enumeration. */
static volatile uint32_t s_dbg_pid;
/** @brief First mismatching report round, or ::k_hid_no_mismatch. */
static volatile uint32_t s_dbg_mismatch = (uint32_t)k_hid_no_mismatch;
/** @brief Completed full passes (sticky success counter). */
static volatile uint32_t s_dbg_pass_count;
/** @brief Device-side report-queue successes (one hid_event_set each). */
static volatile uint32_t s_dbg_dev_sent;
/** @brief Seq byte of the most recent report the host read back. */
static volatile uint32_t s_dbg_last_seq;
/** @brief Device worker progress: 1 stack, 2 class, 3 dcd, 4 attach, 5 send. */
static volatile uint32_t s_dbg_dev_step;
/** @brief Device worker first failing return code (0 = none). */
static volatile uint32_t s_dbg_dev_err;

/* -------------------------------------------------------------------------- */
/* HID Report Descriptor (vendor-defined, one 8-byte input report)            */
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
  0x09U, 0x01U,        /*   Usage (0x01)                     */
  0x15U, 0x00U,        /*   Logical Minimum (0)              */
  0x26U, 0xFFU, 0x00U, /*   Logical Maximum (255)            */
  0x75U, 0x08U,        /*   Report Size (8 bits)             */
  0x95U, 0x08U,        /*   Report Count (8)                 */
  0x81U, 0x02U,        /*   Input (Data,Var,Abs)             */
  0xC0U,               /* End Collection                     */
};

/* -------------------------------------------------------------------------- */
/* USB descriptors (HID: one interface, one interrupt-IN endpoint)            */
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
  0x18U, /* PID = 0x0018 (pid.codes test).    */
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
/* Shared HID report pattern                                                  */
/* -------------------------------------------------------------------------- */

/**
 * @brief Fill the fixed body of a HID report (bytes 1..len-1).
 *
 * @details Body byte i = ``(i*7 + 0x5A) & 0xFF``, independent of the report
 * sequence. Byte 0 (the seq) is left untouched -- the device stamps it with
 * a rolling counter and the host ignores it for the pattern check. Both the
 * device (to build) and the host (to verify) compute the same body.
 *
 * @param[out] out Report buffer.
 * @param[in]  len Report width in bytes (>= 1).
 *
 * @pre @p out has @p len writable bytes.
 * @pre @p len is at most ::k_hid_read_buf.
 * @post @p out[1..len-1] hold the fixed pattern bytes.
 * @post @p out[0] is unchanged.
 *
 * @note Pure function (apart from the caller's buffer).
 * @since 0.1.0
 */
static void hid_fill_report_body(uint8_t* out, uint32_t len)
{
  for (uint32_t i = (uint32_t)k_hid_body_idx; i < len; i++) {
    const uint32_t v = (i * (uint32_t)k_hid_pat_idx_mul) + (uint32_t)k_hid_pat_bias;
    out[i]           = (uint8_t)(v & (uint32_t)k_hid_byte_mask);
  }
}

/* -------------------------------------------------------------------------- */
/* HID activate / deactivate callbacks                                        */
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
/* Console helpers (SCI8 -> J-Link OB CDC)                                    */
/* -------------------------------------------------------------------------- */

/**
 * @brief Format one nibble (0..15) into an uppercase hex character.
 *
 * @details Standard '0'-'9' then 'A'-'F' mapping.
 *
 * @param[in] nibble 4-bit value.
 *
 * @return ASCII '0'..'9' or 'A'..'F'.
 * @retval '0' For a zero nibble.
 *
 * @pre Caller has masked the value to 4 bits.
 * @pre None beyond the mask contract.
 * @post Returned byte is printable hex.
 * @post No state changes.
 *
 * @note Pure function.
 * @since 0.1.0
 */
static uint8_t hid_nibble_to_hex(uint32_t nibble)
{
  if (nibble < k_hid_hex_digit_split) {
    return (uint8_t)((uint8_t)'0' + (uint8_t)nibble);
  }
  return (uint8_t)((uint8_t)'A' + (uint8_t)nibble - (uint8_t)k_hid_hex_digit_split);
}

/**
 * @brief Bounded ASCII string length (cap ::k_hid_print_cap).
 *
 * @details Linear scan with a hard upper bound.
 *
 * @param[in] text NUL-terminated string.
 *
 * @return Number of bytes before the NUL, capped.
 * @retval 0 For an empty string.
 *
 * @pre @p text is non-NULL.
 * @pre @p text points to readable storage of at least the length.
 * @post No state changes.
 * @post Return value never exceeds ::k_hid_print_cap.
 *
 * @note Bounded scan.
 * @since 0.1.0
 */
static uint32_t hid_str_len(const char* text)
{
  uint32_t len = 0U;
  while (len < (uint32_t)k_hid_print_cap) {
    if (text[len] == '\0') {
      break;
    }
    len++;
  }
  return len;
}

/**
 * @brief Push a literal block over SCI8 polled.
 *
 * @details Thin wrapper fixing the console channel.
 *
 * @param[in] data Buffer to send.
 * @param[in] len  Byte count.
 *
 * @return ra_err_t passthrough from `ra_sci_write_polling`.
 * @retval k_ra_ok All bytes queued.
 *
 * @pre @p data is non-NULL; SCI8 init already ran.
 * @pre @p len excludes any NUL terminator.
 * @post Bytes are in the SCI8 TX FIFO.
 * @post No other state changes.
 *
 * @note Blocking polled TX.
 * @since 0.1.0
 */
[[nodiscard]] static ra_err_t hid_sci_write(const uint8_t* data, uint32_t len)
{
  return ra_sci_write_polling((uint8_t)k_hid_sci_channel, data, len);
}

/**
 * @brief Print a NUL-terminated ASCII string over the console.
 *
 * @details Length-bounded by ::hid_str_len.
 *
 * @param[in] text String to print (CR/LF included by the caller).
 *
 * @return ra_err_t propagated from the SCI helper.
 * @retval k_ra_ok All bytes queued.
 *
 * @pre SCI8 init already ran; @p text is non-NULL.
 * @pre @p text is NUL-terminated within ::k_hid_print_cap bytes.
 * @post The string bytes are in the SCI8 TX FIFO.
 * @post No other state changes.
 *
 * @note Blocking polled TX.
 * @since 0.1.0
 */
[[nodiscard]] static ra_err_t hid_print(const char* text)
{
  return hid_sci_write((const uint8_t*)text, hid_str_len(text));
}

/**
 * @brief Print a uint32_t as ASCII decimal.
 *
 * @details Digit-reversal into a bounded scratch buffer.
 *
 * @param[in] value Value to print.
 *
 * @return ra_err_t propagated from the SCI helper.
 * @retval k_ra_ok All bytes queued.
 *
 * @pre SCI8 init already ran.
 * @pre None beyond console readiness.
 * @post One ASCII decimal token is in the SCI8 TX FIFO.
 * @post No other state changes.
 *
 * @note Blocking polled TX.
 * @since 0.1.0
 */
[[nodiscard]] static ra_err_t hid_print_dec(uint32_t value)
{
  uint8_t  scratch[k_hid_dec_chars_u32] = {};
  uint8_t  out[k_hid_dec_chars_u32]     = {};
  uint8_t  count                        = 0U;
  uint32_t v                            = value;
  if (v == 0U) {
    out[0] = (uint8_t)'0';
    return hid_sci_write(out, 1U);
  }
  while (v != 0U) {
    if (count >= (uint8_t)k_hid_dec_chars_u32) {
      break;
    }
    scratch[count] = (uint8_t)((uint8_t)'0' + (uint8_t)(v % k_hid_dec_radix));
    v              = v / k_hid_dec_radix;
    count++;
  }
  for (uint8_t i = 0U; i < count; i++) {
    out[i] = scratch[count - 1U - i];
  }
  return hid_sci_write(out, (uint32_t)count);
}

/**
 * @brief Print a value as fixed-width uppercase hex.
 *
 * @details Width is clamped to 8 hex digits.
 *
 * @param[in] value  Value to print.
 * @param[in] digits Hex digit count (4 for u16, 8 for u32).
 *
 * @return ra_err_t propagated from the SCI helper.
 * @retval k_ra_ok All bytes queued.
 *
 * @pre SCI8 init already ran.
 * @pre @p digits is at most ::k_hid_hex_chars_u32.
 * @post One fixed-width hex token is in the SCI8 TX FIFO.
 * @post No other state changes.
 *
 * @note Blocking polled TX.
 * @since 0.1.0
 */
[[nodiscard]] static ra_err_t hid_print_hex(uint32_t value, uint8_t digits)
{
  uint8_t out[k_hid_hex_chars_u32] = {};
  uint8_t width                    = digits;
  if (width > (uint8_t)k_hid_hex_chars_u32) {
    width = (uint8_t)k_hid_hex_chars_u32;
  }
  for (uint8_t i = 0U; i < width; i++) {
    const uint8_t shift = (uint8_t)((width - 1U - i) * k_hid_nibble_bits);
    out[i]              = hid_nibble_to_hex((value >> shift) & k_hid_nibble_mask);
  }
  return hid_sci_write(out, (uint32_t)width);
}

/**
 * @brief Print "FAIL <what> err=0xNNNNNNNN" on its own line.
 *
 * @details One-line diagnostic; first failing chunk's code returned.
 *
 * @param[in] what Short description of the failed step.
 * @param[in] err  Error code returned by the step.
 *
 * @return ra_err_t propagated from the SCI helpers.
 * @retval k_ra_ok The diagnostic line is queued.
 *
 * @pre SCI8 init already ran.
 * @pre @p what is NUL-terminated within the print cap.
 * @post One diagnostic line is in the SCI8 TX FIFO.
 * @post No other state changes.
 *
 * @note Blocking polled TX.
 * @since 0.1.0
 */
[[nodiscard]] static ra_err_t hid_print_fail(const char* what, ra_err_t err)
{
  ra_err_t e = hid_print("ra8d2 hid: FAIL ");
  if (e != k_ra_ok) {
    return e;
  }
  e = hid_print(what);
  if (e != k_ra_ok) {
    return e;
  }
  e = hid_print(" err=0x");
  if (e != k_ra_ok) {
    return e;
  }
  e = hid_print_hex((uint32_t)err, (uint8_t)k_hid_hex_chars_u32);
  if (e != k_ra_ok) {
    return e;
  }
  return hid_print("\r\n");
}

/* -------------------------------------------------------------------------- */
/* Device side: USBX HID interrupt-IN reports                                 */
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
    (void)ra_board_led_toggle(k_ra_board_led1);
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
  ra_err_t e     = ux_dcd_ra_usb_initialize(k_ra_usb_speed_fs);
  if (e != k_ra_ok) {
    s_dbg_dev_err = (uint32_t)e;
    return;
  }
  s_dbg_dev_step = (uint32_t)k_hid_dev_step_dcd;
  e              = ra_usb_device_attach(k_ra_usb_speed_fs, true);
  if (e != k_ra_ok) {
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

/* -------------------------------------------------------------------------- */
/* Host side: self-contained polled enumerate + HID interrupt-IN read        */
/* -------------------------------------------------------------------------- */

/**
 * @enum hid_usb_req_t
 * @brief Standard chapter-9 request / descriptor constants for the host.
 */
typedef enum : uint16_t {
  k_hid_bm_std_dev_in   = 0x80U, /**< bmRequestType: Std | Device | In.  */
  k_hid_bm_std_dev_out  = 0x00U, /**< bmRequestType: Std | Device | Out. */
  k_hid_breq_get_desc   = 0x06U, /**< GET_DESCRIPTOR.                    */
  k_hid_breq_set_addr   = 0x05U, /**< SET_ADDRESS.                      */
  k_hid_breq_set_config = 0x09U, /**< SET_CONFIGURATION.               */
  k_hid_desc_device     = 0x01U, /**< DEVICE descriptor type.          */
  k_hid_dev_desc_len    = 18U,   /**< DEVICE descriptor length.        */
  k_hid_off_dev_pid     = 10U,   /**< idProduct LSB byte offset.       */
  k_hid_byte_bits       = 8U,    /**< Bits per byte.                   */
  k_hid_config_value    = 1U,    /**< bConfigurationValue to select.   */
} hid_usb_req_t;

/**
 * @enum hid_enum_tune_t
 * @brief Timing / retry tunables for the polled enumeration ladder.
 */
typedef enum : uint32_t {
  k_hid_vbus_settle_ms = 200U,      /**< VBUS settle before probing.        */
  k_hid_attach_to_ms   = 2000U,     /**< Wait for the D+ pull-up.           */
  k_hid_debounce_ms    = 500U,      /**< Post-attach debounce (>=100 ms).   */
  k_hid_reset_hold_ms  = 50U,       /**< USB bus-reset hold (>=10 ms).      */
  k_hid_recovery_ms    = 20U,       /**< Post-reset recovery (TRSTRCY).     */
  k_hid_addr_settle_ms = 5U,        /**< Post-SET_ADDRESS recovery.         */
  k_hid_enum_tries     = 8U,        /**< Reset+probe attempts.              */
  k_hid_attach_spin    = 50000000U, /**< Attach spin cap (frozen-tick guard). */
} hid_enum_tune_t;

/**
 * @brief GET_DESCRIPTOR(DEVICE) over the polled control engine.
 *
 * @param[out] desc Receives the 18-byte device descriptor.
 *
 * @return Read outcome.
 * @retval k_ra_ok           All 18 bytes arrived.
 * @retval k_ra_err_hw_error A short descriptor came back.
 *
 * @pre The bus is reset and the DCP targets the device's current address.
 * @pre @p desc holds at least ::k_hid_dev_desc_len bytes.
 * @post @p desc carries the device descriptor on success.
 * @post No global state changes.
 *
 * @note Blocking (polled control transfer).
 * @since 0.1.0
 */
[[nodiscard]] static ra_err_t hid_ctrl_get_dev_desc(uint8_t* desc)
{
  const ra_usb_setup_t setup = {
    .bm_request_type = (uint8_t)k_hid_bm_std_dev_in,
    .b_request       = (uint8_t)k_hid_breq_get_desc,
    .w_value         = (uint16_t)((uint16_t)k_hid_desc_device << (uint16_t)k_hid_byte_bits),
    .w_index         = 0U,
    .w_length        = (uint16_t)k_hid_dev_desc_len,
  };
  uint16_t       rx = 0U;
  const ra_err_t err =
    ra_usb_host_control_xfer(k_ra_usb_speed_hs, &setup, desc, (uint16_t)k_hid_dev_desc_len, &rx);
  if (err != k_ra_ok) {
    return err;
  }
  if (rx != (uint16_t)k_hid_dev_desc_len) {
    return k_ra_err_hw_error;
  }
  return k_ra_ok;
}

/**
 * @brief Wait for the device to attach, then bus-reset + read its descriptor.
 *
 * @details Waits for the D+ pull-up (LNST leaves SE0) plus the spec
 * debounce, then each attempt drives a full bus reset (returns the device
 * to address 0), re-enables SOF, targets address 0, and reads the device
 * descriptor. The first attempt that returns all 18 bytes wins.
 *
 * @param[out] desc Receives the winning 18-byte device descriptor.
 *
 * @return Hunt outcome.
 * @retval k_ra_ok             The device answered at address 0.
 * @retval k_ra_err_hw_timeout Nothing attached / nothing answered.
 *
 * @pre ::ra_usb_host_init ran (host mode up, VBUS supplied).
 * @pre ::ra_time_init has run (ms delays).
 * @post On success the DCP targets address 0 with UACT on.
 * @post On failure the bus is left in the last attempt's state.
 *
 * @note Blocking; worst case a few seconds.
 * @since 0.1.0
 */
[[nodiscard]] static ra_err_t hid_enum_hunt(uint8_t* desc)
{
  ra_delay_ms(k_hid_vbus_settle_ms);
  const uint32_t t0 = ra_time_ms();
  for (uint32_t spin = 0U; spin < (uint32_t)k_hid_attach_spin; spin++) {
    if (ra_usb_host_line_state(k_ra_usb_speed_hs) != 0U) {
      break;
    }
    if ((ra_time_ms() - t0) > (uint32_t)k_hid_attach_to_ms) {
      break;
    }
  }
  ra_delay_ms(k_hid_debounce_ms);
  ra_err_t err = k_ra_err_hw_timeout;
  for (uint8_t attempt = 0U; attempt < (uint8_t)k_hid_enum_tries; attempt++) {
    (void)ra_usb_host_bus_reset(k_ra_usb_speed_hs, true);
    ra_delay_ms(k_hid_reset_hold_ms);
    (void)ra_usb_host_bus_reset(k_ra_usb_speed_hs, false);
    (void)ra_usb_host_set_uact(k_ra_usb_speed_hs, true);
    ra_delay_ms(k_hid_recovery_ms);
    (void)ra_usb_host_set_target(k_ra_usb_speed_hs, 0U);
    err = hid_ctrl_get_dev_desc(desc);
    if (err == k_ra_ok) {
      return k_ra_ok;
    }
  }
  return err;
}

/**
 * @brief SET_ADDRESS to ::k_hid_dev_addr, then retarget the DCP.
 *
 * @return First failing step's error, or k_ra_ok.
 * @retval k_ra_ok The DCP now targets the operating address.
 *
 * @pre ::hid_enum_hunt succeeded (device answering at address 0).
 * @pre The bus is active (UACT on).
 * @post Later transfers carry tokens to ::k_hid_dev_addr.
 * @post The set-address recovery delay has elapsed.
 *
 * @note Blocking (one control transfer + settle).
 * @since 0.1.0
 */
[[nodiscard]] static ra_err_t hid_enum_set_address(void)
{
  const ra_usb_setup_t setup = {
    .bm_request_type = (uint8_t)k_hid_bm_std_dev_out,
    .b_request       = (uint8_t)k_hid_breq_set_addr,
    .w_value         = (uint16_t)k_hid_dev_addr,
    .w_index         = 0U,
    .w_length        = 0U,
  };
  ra_err_t err = ra_usb_host_control_xfer(k_ra_usb_speed_hs, &setup, nullptr, 0U, nullptr);
  if (err != k_ra_ok) {
    return err;
  }
  ra_delay_ms(k_hid_addr_settle_ms);
  return ra_usb_host_set_target(k_ra_usb_speed_hs, (uint8_t)k_hid_dev_addr);
}

/**
 * @brief SET_CONFIGURATION(::k_hid_config_value) on the addressed device.
 *
 * @return Control-transfer outcome.
 * @retval k_ra_ok The device entered the Configured state.
 *
 * @pre ::hid_enum_set_address succeeded.
 * @pre The DCP targets ::k_hid_dev_addr.
 * @post On success the device's endpoints are usable.
 * @post No global state changes.
 *
 * @note Blocking (one control transfer).
 * @since 0.1.0
 */
[[nodiscard]] static ra_err_t hid_enum_set_config(void)
{
  const ra_usb_setup_t setup = {
    .bm_request_type = (uint8_t)k_hid_bm_std_dev_out,
    .b_request       = (uint8_t)k_hid_breq_set_config,
    .w_value         = (uint16_t)k_hid_config_value,
    .w_index         = 0U,
    .w_length        = 0U,
  };
  return ra_usb_host_control_xfer(k_ra_usb_speed_hs, &setup, nullptr, 0U, nullptr);
}

/**
 * @brief Open the host receive pipe for the device's HID interrupt-IN endpoint.
 *
 * @details Pipe ::k_hid_pipe_in -> device EP1 IN (host receives), 64-byte
 * MPS. The endpoint is interrupt on the device; the host SIE drives it as a
 * receive pipe (issues IN tokens on demand), which is sufficient for the
 * polled report read. HID is input-only, so there is no OUT pipe.
 *
 * @return The pipe-setup error, or k_ra_ok.
 * @retval k_ra_ok The IN pipe is configured and parked NAK.
 *
 * @pre ::hid_enum_set_config succeeded.
 * @pre The pipe is not currently armed.
 * @post The pipe targets ::k_hid_dev_addr EP1 with DATA0 forced.
 * @post The pipe's status bits are cleared.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] static ra_err_t hid_open_pipes(void)
{
  return ra_usb_host_pipe_setup(k_ra_usb_speed_hs,
                                (uint8_t)k_hid_pipe_in,
                                (uint8_t)k_hid_dev_addr,
                                (uint8_t)k_hid_ep_in_num,
                                true,
                                (uint16_t)k_hid_mps);
}

/**
 * @brief Run the full enumeration ladder and open the bulk pipes.
 *
 * @details hunt -> SET_ADDRESS -> SET_CONFIGURATION -> open pipes, and
 * extract idProduct from the device descriptor. Each step prints its own
 * failure tag. The caller deinitializes the host controller on error.
 *
 * @param[out] out_pid Receives the device's idProduct on success.
 *
 * @return First failing step's error, or k_ra_ok.
 * @retval k_ra_ok Device enumerated; bulk pipes open.
 *
 * @pre ::ra_usb_host_init has succeeded on this pass.
 * @pre @p out_pid is non-NULL.
 * @post @p out_pid holds the device idProduct on success.
 * @post On failure the offending step printed its tag.
 *
 * @note Blocking; runs on the low-priority host thread.
 * @since 0.1.0
 */
[[nodiscard]] static ra_err_t hid_enumerate(uint32_t* out_pid)
{
  uint8_t  desc[k_hid_dev_desc_len] = {};
  ra_err_t err                      = hid_enum_hunt(desc);
  if (err != k_ra_ok) {
    (void)hid_print_fail("enumerate", err);
    return err;
  }
  *out_pid = (uint32_t)desc[k_hid_off_dev_pid] |
             ((uint32_t)desc[(uint32_t)k_hid_off_dev_pid + 1U] << (uint32_t)k_hid_byte_bits);
  err      = hid_enum_set_address();
  if (err != k_ra_ok) {
    (void)hid_print_fail("set_address", err);
    return err;
  }
  err = hid_enum_set_config();
  if (err != k_ra_ok) {
    (void)hid_print_fail("set_config", err);
    return err;
  }
  err = hid_open_pipes();
  if (err != k_ra_ok) {
    (void)hid_print_fail("open_pipes", err);
  }
  return err;
}

/**
 * @brief Print "enumerated pid=0xNNNN" for the looped device.
 *
 * @param[in] pid The device idProduct to report.
 *
 * @return ra_err_t propagated from the SCI helpers.
 * @retval k_ra_ok The line is queued.
 *
 * @pre ::hid_enumerate succeeded.
 * @pre SCI8 init already ran.
 * @post One ASCII line is in the SCI8 TX FIFO.
 * @post No other state changes.
 *
 * @note Blocking polled TX.
 * @since 0.1.0
 */
[[nodiscard]] static ra_err_t hid_print_enum(uint32_t pid)
{
  ra_err_t err = hid_print("ra8d2 hid: enumerated pid=0x");
  if (err != k_ra_ok) {
    return err;
  }
  err = hid_print_hex(pid, (uint8_t)k_hid_hex_chars_u16);
  if (err != k_ra_ok) {
    return err;
  }
  return hid_print("\r\n");
}

/**
 * @brief Print "N reports verified -- USB SELFTEST HID PASS".
 *
 * @return ra_err_t propagated from the SCI helpers.
 * @retval k_ra_ok The verdict line is queued.
 *
 * @pre All ::k_hid_rounds report rounds verified.
 * @pre SCI8 init already ran.
 * @post One ASCII verdict line is in the SCI8 TX FIFO.
 * @post No other state changes.
 *
 * @note Blocking polled TX.
 * @since 0.1.0
 */
[[nodiscard]] static ra_err_t hid_print_pass(void)
{
  ra_err_t err = hid_print("ra8d2 hid: ");
  if (err != k_ra_ok) {
    return err;
  }
  err = hid_print_dec((uint32_t)k_hid_rounds);
  if (err != k_ra_ok) {
    return err;
  }
  return hid_print(" reports verified -- USB SELFTEST HID PASS\r\n");
}

/**
 * @brief One report round: read an interrupt-IN report, check its body.
 *
 * @details Polls one report off the device's interrupt-IN endpoint, checks
 * the read length equals ::k_hid_report_len, and byte-checks the fixed body
 * (bytes 1..N-1) against ::hid_fill_report_body. Byte 0 (the seq) is
 * recorded in ::s_dbg_last_seq as a liveness witness. Each report is well
 * under the endpoint MPS, so it arrives as one short packet.
 *
 * @param[in] round The report round index (0..::k_hid_rounds-1).
 *
 * @return ra_err_t verdict.
 * @retval k_ra_ok               The report body matched.
 * @retval k_ra_err_invalid_size The report length differed.
 * @retval k_ra_err_invalid_state The report body bytes differed.
 *
 * @pre The interrupt-IN pipe was opened by ::hid_open_pipes.
 * @pre The device send worker is queuing reports.
 * @post ::s_dbg_mismatch records @p round on any mismatch.
 * @post ::s_dbg_last_seq holds the read report's seq on success.
 *
 * @note Blocking; one interrupt-IN read over the self-loop.
 * @since 0.1.0
 */
[[nodiscard]] static ra_err_t hid_read_round(uint32_t round)
{
  static uint8_t s_expect[k_hid_read_buf] = {};
  static uint8_t s_rx[k_hid_read_buf]     = {};
  hid_fill_report_body(s_expect, (uint32_t)k_hid_report_len);
  uint16_t       rx  = 0U;
  const ra_err_t err = ra_usb_host_bulk_in(k_ra_usb_speed_hs,
                                           (uint8_t)k_hid_pipe_in,
                                           s_rx,
                                           (uint16_t)k_hid_read_buf,
                                           &rx);
  if (err != k_ra_ok) {
    (void)hid_print_fail("report read", err);
    return err;
  }
  if (rx != (uint16_t)k_hid_report_len) {
    s_dbg_mismatch = round;
    (void)hid_print_fail("report length", k_ra_err_invalid_size);
    return k_ra_err_invalid_size;
  }
  const size_t body = (size_t)((uint32_t)k_hid_report_len - (uint32_t)k_hid_body_idx);
  if (memcmp(&s_rx[k_hid_body_idx], &s_expect[k_hid_body_idx], body) != 0) {
    s_dbg_mismatch = round;
    (void)hid_print_fail("report pattern mismatch", k_ra_err_invalid_state);
    return k_ra_err_invalid_state;
  }
  s_dbg_last_seq = (uint32_t)s_rx[k_hid_seq_idx];
  return k_ra_ok;
}

/**
 * @brief One full host-side pass: enumerate, then read + check reports.
 *
 * @details Phases mirror ::hid_phase_t. On any failure the host controller
 * is deinitialized so the next retry starts from a clean attach.
 *
 * @return First failing step's error, or k_ra_ok.
 * @retval k_ra_ok The pass printed HID PASS.
 *
 * @pre Device-side HID class is registered and sending (other thread).
 * @pre The self-loop cable connects J7 to J11.
 * @post On success ::s_dbg_pass_count advanced and LED2 is on.
 * @post On failure the host controller is deinitialized.
 *
 * @note Blocking; runs on the low-priority host thread.
 * @since 0.1.0
 */
[[nodiscard]] static ra_err_t hid_host_pass(void)
{
  s_dbg_phase  = (uint32_t)k_hid_phase_init;
  ra_err_t err = hid_print("ra8d2 hid: host up on USB-HS, probing the loop...\r\n");
  if (err != k_ra_ok) {
    return err;
  }
  err = ra_usb_host_init(k_ra_usb_speed_hs);
  if (err != k_ra_ok) {
    (void)hid_print_fail("host init", err);
    return err;
  }

  s_dbg_phase  = (uint32_t)k_hid_phase_enum;
  uint32_t pid = 0U;
  err          = hid_enumerate(&pid);
  if (err != k_ra_ok) {
    (void)ra_usb_host_deinit(k_ra_usb_speed_hs);
    return err;
  }
  s_dbg_pid = pid;
  err       = hid_print_enum(pid);
  if (err != k_ra_ok) {
    return err;
  }

  s_dbg_phase     = (uint32_t)k_hid_phase_verify;
  s_dbg_rounds_ok = 0U;
  for (uint32_t r = 0U; r < (uint32_t)k_hid_rounds; r++) {
    err = hid_read_round(r);
    if (err != k_ra_ok) {
      (void)ra_usb_host_deinit(k_ra_usb_speed_hs);
      return err;
    }
    s_dbg_rounds_ok++;
  }

  s_dbg_phase = (uint32_t)k_hid_phase_pass;
  s_dbg_pass_count++;
  err = hid_print_pass();
  if (err != k_ra_ok) {
    return err;
  }
  (void)ra_board_led_on(k_ra_board_led2);
  return k_ra_ok;
}

/**
 * @brief Host-side worker: retry the full pass until it succeeds.
 *
 * @details Waits for the device side to attach, then loops ::hid_host_pass
 * with a retry pause until every report round verifies; afterwards parks so
 * the verdict stays on the wire.
 *
 * @param[in] arg ThreadX entry argument (unused).
 *
 * @pre tx_application_define created this thread.
 * @pre The HS host pins, expander switch, and PLL are up (main).
 * @post On success the pass counter and LED2 are latched.
 * @post Retries forever otherwise; each failure prints its step.
 *
 * @note Blocking calls; ms timeouts via ra_time.
 * @since 0.1.0
 */
static VOID hid_host_worker(ULONG arg)
{
  (void)arg;

  tx_thread_sleep(k_hid_boot_wait_ticks);
  for (;;) {
    const ra_err_t err = hid_host_pass();
    if (err == k_ra_ok) {
      break;
    }
    tx_thread_sleep(k_hid_retry_ticks);
  }
  while (1) {
    tx_thread_sleep(k_hid_idle_ticks);
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
#endif /* !RA_SIMULATOR_MODE */

/* -------------------------------------------------------------------------- */
/* Startup                                                                    */
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
  if (ra_pfs_route_peripheral(k_hid_pin_fs_vbus, k_ra_psel_usb_fs, "hid.fs_vbus") != k_ra_ok) {
    hid_panic_halt();
  }
  if (ra_gpio_output_init(k_hid_pin_fs_vbusen, k_ra_level_low) != k_ra_ok) {
    hid_panic_halt();
  }
  if (ra_pfs_route_peripheral(k_hid_pin_fs_dp, k_ra_psel_usb_fs, "hid.fs_dp") != k_ra_ok) {
    hid_panic_halt();
  }
  if (ra_pfs_route_peripheral(k_hid_pin_fs_dm, k_ra_psel_usb_fs, "hid.fs_dm") != k_ra_ok) {
    hid_panic_halt();
  }
  if (ra_board_io_expander_set_usbhs_host_mode() != k_ra_ok) {
    hid_panic_halt();
  }
  if (ra_gpio_output_init(k_hid_pin_hs_pwr, k_ra_level_high) != k_ra_ok) {
    hid_panic_halt();
  }
  if (ra_pfs_route_peripheral(k_hid_pin_hs_vbus, k_ra_psel_usb_hs, "hid.hs_vbus") != k_ra_ok) {
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
  uint32_t pclka_hz   = 0U;
  if (ra_cgc_init() != k_ra_ok) {
    hid_panic_halt();
  }
  if (ra_cgc_usbfs_clock_enable() != k_ra_ok) {
    hid_panic_halt();
  }
  if (ra_cgc_usbhs_pll_enable() != k_ra_ok) {
    hid_panic_halt();
  }
  if (ra_cgc_get_clock_hz(k_ra_clock_id_cpuclk0, &cpuclk0_hz) != k_ra_ok) {
    hid_panic_halt();
  }
  if (ra_cgc_get_clock_hz(k_ra_clock_id_pclka, &pclka_hz) != k_ra_ok) {
    hid_panic_halt();
  }
  if (ra_time_init(cpuclk0_hz) != k_ra_ok) {
    hid_panic_halt();
  }
  if (ra_pfs_route_peripheral(k_hid_pin_sci_tx, k_ra_psel_sci_async, "hid.txd8") != k_ra_ok) {
    hid_panic_halt();
  }
  if (ra_pfs_route_peripheral(k_hid_pin_sci_rx, k_ra_psel_sci_async, "hid.rxd8") != k_ra_ok) {
    hid_panic_halt();
  }
  const ra_sci_cfg_t sci_cfg = {
    .baud      = k_hid_baud,
    .data_bits = k_ra_sci_data_8,
    .parity    = k_ra_sci_parity_none,
    .stop_bits = k_ra_sci_stop_1,
    .pclk_hz   = pclka_hz,
  };
  if (ra_sci_init((uint8_t)k_hid_sci_channel, &sci_cfg) != k_ra_ok) {
    hid_panic_halt();
  }
  if (ra_board_led_init(k_ra_board_led1) != k_ra_ok) {
    hid_panic_halt();
  }
  if (ra_board_led_init(k_ra_board_led2) != k_ra_ok) {
    hid_panic_halt();
  }
  hid_route_usb_or_halt();
}

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmain"
/**
 * @brief Application entry: bring the board up, then hand off to ThreadX.
 *
 * @details Both USB controllers' clocks and pins come up before the
 * kernel so the workers only deal with stack bring-up.
 *
 * @return Never returns (``tx_kernel_enter`` is __noreturn).
 *
 * @pre Reset_Handler copied .data and zeroed .bss.
 * @pre SystemInit set VTOR, FPU, priority grouping.
 * @post On clean entry the CPU stays in tx_kernel_enter forever.
 * @post On any HAL init failure the function halts in WFI.
 *
 * @note Single entry point; not re-entrant.
 * @since 0.1.0
 */
int32_t main(void)
{
  hid_setup_or_halt();

  ra_isr_globals_enable();

#ifndef RA_SIMULATOR_MODE
  tx_kernel_enter();
#endif

  hid_panic_halt();
  return 0;
}
#pragma GCC diagnostic pop
