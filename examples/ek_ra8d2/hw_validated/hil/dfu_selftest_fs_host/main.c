/**
 * @file examples/ek_ra8d2/hw_validated/hil/dfu_selftest_fs_host/main.c
 * @brief USB self-loop config B: FS host DFU-downloads firmware to an HS DFU device, uploads + verifies
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * Rehearses a firmware-update-over-USB path entirely on-chip, with the host and
 * device roles SWAPPED versus config A (``dfu_selftest_hs_host``). The two USB
 * ports are cabled to EACH OTHER and one image runs both stacks:
 *
 *  - USBHS (J7) = DEVICE: a ThreadX + USBX DFU class in DFU mode
 *    (bInterfaceProtocol 0x02, so it enumerates straight into dfuIDLE -- no
 *    runtime/detach round-trip). Its write callback programs each downloaded
 *    block straight into the inactive MRAM slot (libs/ra8_dfu); its read callback
 *    serves the slot body back on UPLOAD. DFU runs entirely over EP0 control
 *    transfers (no data endpoints).
 *  - USBFS (J11) = HOST: a self-contained polled host on the first-party
 *    ``ra8_usb_host_*`` primitives. It enumerates the device, DFU_DNLOADs a
 *    deterministic multi-block image (with DFU_GETSTATUS polling), DFU_ABORTs
 *    back to dfuIDLE, then DFU_UPLOADs it back and byte-checks it -- proving the
 *    MRAM program/read-back path round-trips intact.
 *
 * The download exercises the host control-OUT data stage of
 * ``ra8_usb_host_control_xfer`` (DFU_DNLOAD carries the firmware block in the
 * SETUP data stage host -> device).
 *
 * Verdicts stream over SCI8 (J-Link OB CDC console, 115200) and are mirrored in
 * J-Link-readable probes (``s_dbg_*``).
 *
 * ## Pinout
 *
 * HS device: P4_08 USBHS_VBUS sense (PSEL usb_hs), PD07 driven LOW so J7's role
 * is Device and U18 does not back-feed VBUS; D+/D- are dedicated PHY balls. FS
 * host: P4_07 VBUS sense, P5_00 VBUSEN peripheral-routed so the USBFS controller
 * sources J11 VBUS, P8_14 D+, P8_15 D- (PSEL usb_fs). Console: PD_02/PD_03 SCI8
 * (PSEL sci_async).
 *
 * @author Brighton Sikarskie
 * @date 2026-06-15
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>
#include <string.h>

#include "ra8_board_ek_ra8d2.h"
#include "ra8_cgc.h"
#include "ra8_dfu.h"
#include "ra8_dfu_device.h"
#include "ra8_dfu_host.h"
#include "ra8_err.h"
#include "ra8_gpio_constants.h"
#include "ra8_isr.h"
#include "ra8_port_constants.h"
#include "ra8_port_utils.h"
#include "ra8_time.h"
#include "ra8_usb.h"

#ifndef RA8_SIMULATOR_MODE
#include "tx_api.h"
#include "ux_api.h"
#include "ux_dcd_ra8_usb.h"
#include "ux_device_class_dfu.h"
#include "ux_device_stack.h"

extern void _tx_timer_interrupt(void); /**< @brief ThreadX 1 ms tick worker. */

/**
 * @var s_tx_kernel_up
 * @brief Set in ::tx_application_define; gates ThreadX tick delivery so a
 *        pre-kernel SysTick (the U15 expander I2C blocks for ms during setup)
 *        cannot feed ThreadX's zeroed timer state.
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
static const ra8_port_pin_t k_dfu_pin_fs_vbus = (ra8_port_pin_t)k_ra8_board_usbfs_pin_vbus;

/** @brief USBFS VBUSEN (P5_00) -- peripheral-routed; the FS host sources J11 VBUS. */
static const ra8_port_pin_t k_dfu_pin_fs_vbusen = (ra8_port_pin_t)k_ra8_board_usbfs_pin_vbusen;

/** @brief USBFS D+ (P8_14). */
static const ra8_port_pin_t k_dfu_pin_fs_dp = (ra8_port_pin_t)k_ra8_board_usbfs_pin_dp;

/** @brief USBFS D- (P8_15). */
static const ra8_port_pin_t k_dfu_pin_fs_dm = (ra8_port_pin_t)k_ra8_board_usbfs_pin_dm;

/** @brief USBHS_VBUS sense pin (P4_08, PSEL = 0x14). */
static const ra8_port_pin_t k_dfu_pin_hs_vbus = (ra8_port_pin_t)k_ra8_board_usbhs_pin_vbus;

/** @brief J7 role strap (PD07): LOW = Device, so U18 does not back-feed VBUS. */
static const ra8_port_pin_t k_dfu_pin_hs_role = (ra8_port_pin_t)k_ra8_board_usbhs_pin_pwr;

/* -------------------------------------------------------------------------- */
/* Tunables */
/* -------------------------------------------------------------------------- */

/**
 * @enum dfu_config_t
 * @brief Compile-time settings: threads, pool, console, cadence.
 */
typedef enum : uint32_t {
  k_dfu_thread_stack    = 4096U,   /**< Device worker stack (bytes).       */
  k_dfu_host_stack      = 8192U,   /**< Host worker stack (bytes).         */
  k_dfu_usbx_pool_bytes = 32768U,  /**< USBX memory pool (bytes).          */
  k_dfu_idle_ticks      = 50U,     /**< Parked-loop back-off (ticks).      */
  k_dfu_boot_wait_ticks = 500U,    /**< Host start delay (1 ms ticks).     */
  k_dfu_retry_ticks     = 3000U,   /**< Pause between ladder retries.      */
  k_dfu_baud            = 115200U, /**< J-Link OB CDC log baud.            */
  k_dfu_print_cap       = 160U,    /**< Bound for console-string scans.    */
  k_dfu_dev_priority    = 8U,      /**< Device bring-up worker priority.   */
  k_dfu_host_priority   = 24U,     /**< Host worker priority (below USBX). */
} dfu_config_t;

/**
 * @enum dfu_hex_t
 * @brief Hex/decimal text-formatter sizing constants.
 */
typedef enum : uint8_t {
  k_dfu_hex_chars_u16   = 4U,  /**< 16-bit value -> "ABCD".        */
  k_dfu_hex_chars_u32   = 8U,  /**< 32-bit value -> "ABCDEF01".    */
  k_dfu_dec_chars_u32   = 10U, /**< Max digits for a 32-bit count. */
  k_dfu_nibble_bits     = 4U,  /**< Bits per hex nibble.           */
  k_dfu_hex_digit_split = 10U, /**< Threshold between '0-9'/'A-F'. */
} dfu_hex_t;

/**
 * @enum dfu_mask_t
 * @brief Bit-mask constants used by the text formatters.
 */
typedef enum : uint32_t {
  k_dfu_nibble_mask = 0xFU, /**< 4-bit nibble mask.           */
  k_dfu_dec_radix   = 10U,  /**< Base for decimal conversion. */
} dfu_mask_t;

/**
 * @enum dfu_geom_t
 * @brief Firmware-image geometry + pattern constants.
 */
typedef enum : uint32_t {
  k_dfu_xfer_size   = 64U,         /**< wTransferSize: bytes per DFU block. */
  k_dfu_blocks      = 8U,          /**< Blocks in the rehearsal image.      */
  k_dfu_image_bytes = 512U,        /**< k_dfu_blocks * k_dfu_xfer_size.     */
  k_dfu_no_mismatch = 0xFFFFFFFFU, /**< Probe: no mismatch.                 */
  k_dfu_pat_blk_mul = 131U,        /**< Per-block pattern multiplier.       */
  k_dfu_pat_idx_mul = 7U,          /**< Per-index pattern multiplier.       */
  k_dfu_pat_bias    = 0xA5U,       /**< Pattern constant bias.              */
  k_dfu_byte_mask   = 0xFFU,       /**< Byte mask.                          */
  k_dfu_dev_addr    = 1U,          /**< Address the host assigns.           */
  k_dfu_intf        = 0U,          /**< DFU interface number.               */
  k_dfu_config_val  = 1U,          /**< bConfigurationValue.                */
} dfu_geom_t;

/**
 * @enum dfu_phase_t
 * @brief J-Link probe values marking host-ladder progress.
 */
typedef enum : uint32_t {
  k_dfu_phase_boot     = 0U, /**< Host thread not started.   */
  k_dfu_phase_init     = 1U, /**< Host controller init.      */
  k_dfu_phase_enum     = 2U, /**< Enumerating.               */
  k_dfu_phase_download = 3U, /**< Running DFU_DNLOAD.        */
  k_dfu_phase_upload   = 4U, /**< Running DFU_UPLOAD.        */
  k_dfu_phase_pass     = 5U, /**< Image verified byte-equal. */
} dfu_phase_t;

#ifndef RA8_SIMULATOR_MODE

/* -------------------------------------------------------------------------- */
/* ThreadX workers + USBX pool storage */
/* -------------------------------------------------------------------------- */

/** @brief ThreadX TCB for the USBX device-side worker thread. */
static TX_THREAD s_device_thread;
/** @brief Stack backing storage for ::s_device_thread. */
static UCHAR s_device_stack[k_dfu_thread_stack];
/** @brief ThreadX TCB for the host-side worker thread. */
static TX_THREAD s_host_thread;
/** @brief Stack backing storage for ::s_host_thread. */
static UCHAR s_host_stack[k_dfu_host_stack];
/** @brief USBX memory pool (USBX uses ``tx_byte_pool`` internally). */
static UCHAR s_usbx_pool[k_dfu_usbx_pool_bytes];

/**
 * @var s_dfu_image
 * @brief Device-side RAM "firmware" image: the DFU write callback stores each
 *        downloaded block here; the read callback serves it back on upload.
 * @note Written by the USBX DFU class thread; read by the same on upload.
 * @since 0.1.0
 */
static UCHAR s_dfu_image[k_dfu_image_bytes];

/**
 * @var s_dfu_image_len
 * @brief Highest byte offset the device has captured (download high-water mark).
 * @since 0.1.0
 */
static volatile uint32_t s_dfu_image_len;

/* -------------------------------------------------------------------------- */
/* J-Link probes */
/* -------------------------------------------------------------------------- */

/** @brief Host-ladder phase marker (::dfu_phase_t). */
static volatile uint32_t s_dbg_phase;
/** @brief Device-reported product id captured at enumeration. */
static volatile uint32_t s_dbg_pid;
/** @brief Blocks the host confirmed byte-equal on upload (expect k_dfu_blocks). */
static volatile uint32_t s_dbg_blocks_ok;
/** @brief First mismatching block, or ::k_dfu_no_mismatch. */
static volatile uint32_t s_dbg_mismatch = (uint32_t)k_dfu_no_mismatch;
/** @brief Completed full passes (sticky success counter). */
static volatile uint32_t s_dbg_pass_count;
/** @brief Device-side download block-write count. */
static volatile uint32_t s_dbg_dev_writes;
/** @brief Device worker progress: 1 stack, 2 class, 3 dcd, 4 attach. */
static volatile uint32_t s_dbg_dev_step;
/** @brief Host-ladder first failing return code (0 = none). */
static volatile uint32_t s_dbg_host_err;

/* -------------------------------------------------------------------------- */
/* Shared per-block pattern */
/* -------------------------------------------------------------------------- */

/**
 * @brief Fill a firmware block with this block's deterministic bytes.
 * @details Byte i = ``(block*131 + i*7 + 0xA5) & 0xFF`` -- distinct per block so
 *          the upload check proves the bytes read back are the ones downloaded.
 * @param[in]  block The block index (0..::k_dfu_blocks-1).
 * @param[out] out   Destination buffer.
 * @param[in]  len   Bytes to fill.
 * @return void.
 * @pre @p out has @p len writable bytes; @p len <= ::k_dfu_xfer_size.
 * @pre @p block < ::k_dfu_blocks.
 * @post @p out[0..len-1] hold the block's pattern bytes.
 * @post No global state changes.
 * @note Pure function.
 * @since 0.1.0
 */
static void dfu_pattern_fill(uint32_t block, uint8_t* out, uint32_t len)
{
  for (uint32_t i = 0U; i < len; i++) {
    const uint32_t v = (block * (uint32_t)k_dfu_pat_blk_mul) + (i * (uint32_t)k_dfu_pat_idx_mul) +
                       (uint32_t)k_dfu_pat_bias;
    out[i]           = (uint8_t)(v & (uint32_t)k_dfu_byte_mask);
  }
}

/* -------------------------------------------------------------------------- */
/* USB descriptors (DFU mode: single DFU interface, EP0 only) */
/* -------------------------------------------------------------------------- */

/* DFU-mode framework: device (PID 0x0019) + one config with a single DFU
 * interface (class 0xFE / subclass 0x01 / protocol 0x02 = DFU mode, so USBX
 * enumerates straight into dfuIDLE) + the DFU functional descriptor
 * (CAN_DNLOAD | CAN_UPLOAD | MANIFESTATION_TOLERANT, wTransferSize 64). No data
 * endpoints -- DFU runs over EP0. Layout per DFU 1.1 + USB 2.0 sec 9.6. */
static UCHAR s_device_framework[] = {
  /* Device descriptor (18 bytes). idVendor 0x1209, idProduct 0x0019. */
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
  0x19U,
  0x00U,
  0x00U,
  0x01U,
  0x01U,
  0x02U,
  0x03U,
  0x01U,
  /* Configuration descriptor (wTotalLength 0x1B = 27). */
  0x09U,
  0x02U,
  0x1BU,
  0x00U,
  0x01U,
  0x01U,
  0x00U,
  0x80U,
  0x32U,
  /* DFU interface (class 0xFE, subclass 0x01, protocol 0x02 = DFU mode). */
  0x09U,
  0x04U,
  0x00U,
  0x00U,
  0x00U,
  0xFEU,
  0x01U,
  0x02U,
  0x00U,
  /* DFU functional descriptor. bmAttributes 0x07, wTransferSize 64,
     bcdDFUVersion 0x0110. */
  0x09U,
  0x21U,
  0x07U,
  0xFFU,
  0x00U,
  0x40U,
  0x00U,
  0x10U,
  0x01U,
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
  /* idx 2: "RA8D2 DFU". */
  0x09U,
  0x04U,
  0x02U,
  0x09U,
  'R',
  'A',
  '8',
  'D',
  '2',
  ' ',
  'D',
  'F',
  'U',
  /* idx 3: serial "00000019". */
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
  '9',
};

/* USBX LANGID descriptor 0x0409 (English-US), little-endian byte pair. */
typedef enum : uint8_t {
  k_usb_langid_en_us_lo = 0x09U, /**< LANGID 0x0409 low byte.  */
  k_usb_langid_en_us_hi = 0x04U, /**< LANGID 0x0409 high byte. */
} usb_langid_byte_t;

/** @brief USBX language-id table -- US English. */
static UCHAR s_language_id_framework[] = {k_usb_langid_en_us_lo, k_usb_langid_en_us_hi};

/* -------------------------------------------------------------------------- */
/* Device side: USBX DFU class callbacks */
/* -------------------------------------------------------------------------- */

/**
 * @brief Device-side worker: bring the DFU device up, then park.
 * @param[in] arg ThreadX entry argument (unused).
 * @return Never returns.
 * @pre tx_application_define created this thread.
 * @pre USB-HS pins + PHY PLL are up (main did both).
 * @post The HS device is attached in DFU mode; the DFU class services EP0.
 * @post On any bring-up failure the thread exits (s_dbg_dev_step frozen).
 * @note The DFU class runs its own thread; this worker only brings it up.
 * @since 0.1.0
 */
static VOID dfu_device_worker(ULONG arg)
{
  (void)arg;

  /* Config B device half: the HS controller runs the DFU device class wired
   * to real MRAM (libs/ra8_dfu). DFU_DNLOAD programs the inactive Slot B. */
  ra8_dfu_device_set_target(k_ra8_dfu_slot_b);
  const ra8_err_t e = ra8_dfu_device_start(k_ra8_usb_speed_hs,
                                           s_usbx_pool,
                                           (uint32_t)sizeof(s_usbx_pool),
                                           s_device_framework,
                                           (uint32_t)sizeof(s_device_framework),
                                           s_string_framework,
                                           (uint32_t)sizeof(s_string_framework),
                                           s_language_id_framework,
                                           (uint32_t)sizeof(s_language_id_framework));
  if (e != k_ra8_ok) {
    s_dbg_host_err = (uint32_t)e;
    return;
  }
  s_dbg_dev_step = 4U;
  /* Each DFU_DNLOAD block is programmed synchronously inside the DFU write
   * callback; this worker only finalizes the image header once the host
   * signals end-of-download (the self-test uses DFU_ABORT, so that path is
   * the bootloader's, not this twin's). */
  while (1) {
    (void)ra8_dfu_device_worker_step();
    tx_thread_sleep(k_dfu_idle_ticks);
  }
}

/* -------------------------------------------------------------------------- */
/* Console helpers (SCI8 -> J-Link OB CDC) */
/* -------------------------------------------------------------------------- */

/**
 * @brief Format one nibble (0..15) into an uppercase hex character.
 * @param[in] nibble 4-bit value.
 * @return ASCII '0'..'9' or 'A'..'F'.
 * @retval '0' For a zero nibble.
 * @pre Caller has masked the value to 4 bits.
 * @pre None beyond the mask contract.
 * @post Returned byte is printable hex.
 * @post No state changes.
 * @note Pure function.
 * @since 0.1.0
 */
static uint8_t dfu_nibble_to_hex(uint32_t nibble)
{
  if (nibble < k_dfu_hex_digit_split) {
    return (uint8_t)((uint8_t)'0' + (uint8_t)nibble);
  }
  return (uint8_t)((uint8_t)'A' + (uint8_t)nibble - (uint8_t)k_dfu_hex_digit_split);
}

/**
 * @brief Bounded ASCII string length (cap ::k_dfu_print_cap).
 * @param[in] text NUL-terminated string.
 * @return Number of bytes before the NUL, capped.
 * @retval 0 For an empty string.
 * @pre @p text is non-NULL with readable storage.
 * @pre @p text fits the cap.
 * @post No state changes.
 * @post Return value never exceeds ::k_dfu_print_cap.
 * @note Bounded scan.
 * @since 0.1.0
 */
static uint32_t dfu_str_len(const char* text)
{
  uint32_t len = 0U;
  while (len < (uint32_t)k_dfu_print_cap) {
    if (text[len] == '\0') {
      break;
    }
    len++;
  }
  return len;
}

/**
 * @brief Push a literal block over the BSP UART console (SCI8) polled.
 * @param[in] data Buffer to send.
 * @param[in] len  Byte count.
 * @return ra8_err_t passthrough from `ra8_board_uart_console_write`.
 * @retval k_ra8_ok All bytes queued.
 * @pre @p data is non-NULL; the BSP console init already ran.
 * @pre @p len excludes any NUL terminator.
 * @post Bytes are in the SCI8 TX FIFO.
 * @post No other state changes.
 * @note Blocking polled TX.
 * @since 0.1.0
 */
[[nodiscard]] static ra8_err_t dfu_sci_write(const uint8_t* data, uint32_t len)
{
  return ra8_board_uart_console_write(data, (size_t)len);
}

/**
 * @brief Print a NUL-terminated ASCII string over the console.
 * @param[in] text String to print (CR/LF included by the caller).
 * @return ra8_err_t propagated from the SCI helper.
 * @retval k_ra8_ok All bytes queued.
 * @pre SCI8 init already ran; @p text is non-NULL.
 * @pre @p text is NUL-terminated within ::k_dfu_print_cap bytes.
 * @post The string bytes are in the SCI8 TX FIFO.
 * @post No other state changes.
 * @note Blocking polled TX.
 * @since 0.1.0
 */
[[nodiscard]] static ra8_err_t dfu_print(const char* text)
{
  return dfu_sci_write((const uint8_t*)text, dfu_str_len(text));
}

/**
 * @brief Print a value as fixed-width uppercase hex.
 * @param[in] value  Value to print.
 * @param[in] digits Hex digit count (4 for u16, 8 for u32).
 * @return ra8_err_t propagated from the SCI helper.
 * @retval k_ra8_ok All bytes queued.
 * @pre SCI8 init already ran.
 * @pre @p digits is at most ::k_dfu_hex_chars_u32.
 * @post One fixed-width hex token is in the SCI8 TX FIFO.
 * @post No other state changes.
 * @note Blocking polled TX.
 * @since 0.1.0
 */
[[nodiscard]] static ra8_err_t dfu_print_hex(uint32_t value, uint8_t digits)
{
  uint8_t out[k_dfu_hex_chars_u32] = {};
  uint8_t width                    = digits;
  if (width > (uint8_t)k_dfu_hex_chars_u32) {
    width = (uint8_t)k_dfu_hex_chars_u32;
  }
  for (uint8_t i = 0U; i < width; i++) {
    const uint8_t shift = (uint8_t)((width - 1U - i) * k_dfu_nibble_bits);
    out[i]              = dfu_nibble_to_hex((value >> shift) & k_dfu_nibble_mask);
  }
  return dfu_sci_write(out, (uint32_t)width);
}

/**
 * @brief Print a uint32_t as ASCII decimal.
 * @param[in] value Value to print.
 * @return ra8_err_t propagated from the SCI helper.
 * @retval k_ra8_ok All bytes queued.
 * @pre SCI8 init already ran.
 * @pre None beyond console readiness.
 * @post One ASCII decimal token is in the SCI8 TX FIFO.
 * @post No other state changes.
 * @note Blocking polled TX.
 * @since 0.1.0
 */
[[nodiscard]] static ra8_err_t dfu_print_dec(uint32_t value)
{
  uint8_t  scratch[k_dfu_dec_chars_u32] = {};
  uint8_t  out[k_dfu_dec_chars_u32]     = {};
  uint8_t  count                        = 0U;
  uint32_t v                            = value;
  if (v == 0U) {
    out[0] = (uint8_t)'0';
    return dfu_sci_write(out, 1U);
  }
  while (v != 0U) {
    if (count >= (uint8_t)k_dfu_dec_chars_u32) {
      break;
    }
    scratch[count] = (uint8_t)((uint8_t)'0' + (uint8_t)(v % k_dfu_dec_radix));
    v              = v / k_dfu_dec_radix;
    count++;
  }
  for (uint8_t i = 0U; i < count; i++) {
    out[i] = scratch[count - 1U - i];
  }
  return dfu_sci_write(out, (uint32_t)count);
}

/**
 * @brief Print "FAIL <what> err=0xNNNNNNNN" on its own line.
 * @param[in] what Short description of the failed step.
 * @param[in] err  Error code returned by the step.
 * @return ra8_err_t propagated from the SCI helpers.
 * @retval k_ra8_ok The diagnostic line is queued.
 * @pre SCI8 init already ran; @p what is NUL-terminated within the cap.
 * @pre None beyond console readiness.
 * @post One diagnostic line is in the SCI8 TX FIFO.
 * @post No other state changes.
 * @note Blocking polled TX.
 * @since 0.1.0
 */
[[nodiscard]] static ra8_err_t dfu_print_fail(const char* what, ra8_err_t err)
{
  ra8_err_t e = dfu_print("ra8d2 dfu: FAIL ");
  if (e != k_ra8_ok) {
    return e;
  }
  e = dfu_print(what);
  if (e != k_ra8_ok) {
    return e;
  }
  e = dfu_print(" err=0x");
  if (e != k_ra8_ok) {
    return e;
  }
  e = dfu_print_hex((uint32_t)err, (uint8_t)k_dfu_hex_chars_u32);
  if (e != k_ra8_ok) {
    return e;
  }
  return dfu_print("\r\n");
}

/* -------------------------------------------------------------------------- */
/* Host side: enumerate, then DFU download + upload-verify */
/* -------------------------------------------------------------------------- */

/**
 * @enum dfu_req_t
 * @brief Chapter-9 + DFU class request / descriptor constants.
 */
typedef enum : uint16_t {
  k_dfu_bm_std_dev_in     = 0x80U, /**< bmRequestType: Std | Device | In.       */
  k_dfu_bm_std_dev_out    = 0x00U, /**< bmRequestType: Std | Device | Out.      */
  k_dfu_bm_class_if_out   = 0x21U, /**< bmRequestType: Class | Interface | Out. */
  k_dfu_bm_class_if_in    = 0xA1U, /**< bmRequestType: Class | Interface | In.  */
  k_dfu_breq_get_desc     = 0x06U, /**< GET_DESCRIPTOR.                         */
  k_dfu_breq_set_addr     = 0x05U, /**< SET_ADDRESS.                            */
  k_dfu_breq_set_config   = 0x09U, /**< SET_CONFIGURATION.                      */
  k_dfu_breq_dnload       = 0x01U, /**< DFU_DNLOAD.                             */
  k_dfu_breq_upload       = 0x02U, /**< DFU_UPLOAD.                             */
  k_dfu_breq_getstatus    = 0x03U, /**< DFU_GETSTATUS.                          */
  k_dfu_breq_abort        = 0x06U, /**< DFU_ABORT (-> dfuIDLE).                 */
  k_dfu_desc_device       = 0x01U, /**< DEVICE descriptor type.                 */
  k_dfu_dev_desc_len      = 18U,   /**< DEVICE descriptor length.               */
  k_dfu_off_dev_pid       = 10U,   /**< idProduct LSB byte offset.              */
  k_dfu_byte_bits         = 8U,    /**< Bits per byte.                          */
  k_dfu_getstatus_len     = 6U,    /**< DFU_GETSTATUS payload len.              */
  k_dfu_off_status_state  = 4U,    /**< bState offset in GETSTATUS.             */
  k_dfu_state_dnload_idle = 5U,    /**< dfuDNLOAD-IDLE.                         */
  k_dfu_state_idle        = 2U,    /**< dfuIDLE.                                */
} dfu_req_t;

/**
 * @enum dfu_enum_tune_t
 * @brief Timing / retry tunables for the polled enumeration + status polling.
 */
typedef enum : uint32_t {
  k_dfu_vbus_settle_ms = 200U,      /**< VBUS settle before probing.          */
  k_dfu_attach_to_ms   = 2000U,     /**< Wait for the D+ pull-up.             */
  k_dfu_debounce_ms    = 500U,      /**< Post-attach debounce (>=100 ms).     */
  k_dfu_reset_hold_ms  = 50U,       /**< USB bus-reset hold (>=10 ms).        */
  k_dfu_recovery_ms    = 20U,       /**< Post-reset recovery (TRSTRCY).       */
  k_dfu_addr_settle_ms = 5U,        /**< Post-SET_ADDRESS recovery.           */
  k_dfu_status_poll_ms = 2U,        /**< Pause between GETSTATUS polls.       */
  k_dfu_status_tries   = 50U,       /**< GETSTATUS polls before giving up.    */
  k_dfu_enum_tries     = 8U,        /**< Reset+probe attempts.                */
  k_dfu_attach_spin    = 50000000U, /**< Attach spin cap (frozen-tick guard). */
} dfu_enum_tune_t;

/**
 * @brief Run the full host pass: enumerate, download, upload-verify.
 * @return First failing step's error, or k_ra8_ok.
 * @retval k_ra8_ok The pass printed DFU PASS.
 * @pre Device-side DFU class is registered (other thread).
 * @pre The self-loop cable connects J7 to J11.
 * @post On success ::s_dbg_pass_count advanced and LED2 is on.
 * @post On failure the host controller is deinitialized for a clean retry.
 * @note Blocking; runs on the low-priority host thread.
 * @since 0.1.0
 */
[[nodiscard]] static ra8_err_t dfu_host_pass(void)
{
  s_dbg_phase   = (uint32_t)k_dfu_phase_init;
  ra8_err_t err = dfu_print("ra8d2 dfu: host up on USB-FS, probing the loop...\r\n");
  if (err != k_ra8_ok) {
    return err;
  }

  /* Build the deterministic test image the host will DFU_DNLOAD; the device
   * programs it into the inactive MRAM slot and we DFU_UPLOAD + byte-verify. */
  for (uint32_t b = 0U; b < (uint32_t)k_dfu_blocks; b++) {
    dfu_pattern_fill(b, &s_dfu_image[b * (uint32_t)k_dfu_xfer_size], (uint32_t)k_dfu_xfer_size);
  }

  s_dbg_phase               = (uint32_t)k_dfu_phase_download;
  ra8_dfu_host_result_t res = {};
  err       = ra8_dfu_host_run(k_ra8_usb_speed_fs, s_dfu_image, (uint32_t)k_dfu_image_bytes, &res);
  s_dbg_pid = res.pid;
  s_dbg_blocks_ok = res.blocks_ok;
  s_dbg_mismatch  = res.mismatch;
  if (err != k_ra8_ok) {
    (void)dfu_print_fail("dfu round-trip", err);
    return err;
  }

  err = dfu_print("ra8d2 dfu: enumerated pid=0x");
  if (err == k_ra8_ok) {
    err = dfu_print_hex(s_dbg_pid, (uint8_t)k_dfu_hex_chars_u16);
  }
  if (err == k_ra8_ok) {
    err = dfu_print("\r\n");
  }
  if (err != k_ra8_ok) {
    return err;
  }

  s_dbg_phase = (uint32_t)k_dfu_phase_pass;
  s_dbg_pass_count++;
  err = dfu_print("ra8d2 dfu: ");
  if (err == k_ra8_ok) {
    err = dfu_print_dec((uint32_t)k_dfu_blocks);
  }
  if (err == k_ra8_ok) {
    err =
      dfu_print(" blocks DFU-programmed to MRAM + verified -- USB SELFTEST DFU CONFIG B PASS\r\n");
  }
  if (err != k_ra8_ok) {
    return err;
  }
  (void)ra8_board_led_on(k_ra8_board_led2);
  return k_ra8_ok;
}

/**
 * @brief Host-side worker: retry the full pass until it succeeds, then park.
 * @param[in] arg ThreadX entry argument (unused).
 * @return Never returns.
 * @pre tx_application_define created this thread.
 * @pre The FS host pins and clock are up (main).
 * @post On success the pass counter and LED2 are latched.
 * @post Retries forever otherwise; each failure prints its step.
 * @note Blocking calls; ms timeouts via ra8_time.
 * @since 0.1.0
 */
static VOID dfu_host_worker(ULONG arg)
{
  (void)arg;

  tx_thread_sleep(k_dfu_boot_wait_ticks);
  for (;;) {
    const ra8_err_t err = dfu_host_pass();
    if (err == k_ra8_ok) {
      break;
    }
    s_dbg_host_err = (uint32_t)err;
    tx_thread_sleep(k_dfu_retry_ticks);
  }
  while (1) {
    tx_thread_sleep(k_dfu_idle_ticks);
  }
}

/**
 * @brief ThreadX application-define hook. Spawns both workers.
 * @param[in] first_unused_memory Sentinel (unused; static stacks).
 * @return void.
 * @pre Called from ``tx_kernel_enter`` after scheduler init.
 * @pre Static stacks are reserved at file scope.
 * @post Two auto-start workers are queued; ``s_tx_kernel_up`` is true.
 * @post The scheduler runs the device DFU bring-up + the host DFU ladder.
 * @note Called once at boot; not thread-safe.
 * @since 0.1.0
 */
VOID tx_application_define(VOID* first_unused_memory)
{
  (void)first_unused_memory;
  s_tx_kernel_up = true;
  (void)tx_thread_create(&s_device_thread,
                         "dfu_device",
                         dfu_device_worker,
                         0UL,
                         s_device_stack,
                         k_dfu_thread_stack,
                         (UINT)k_dfu_dev_priority,
                         (UINT)k_dfu_dev_priority,
                         TX_NO_TIME_SLICE,
                         TX_AUTO_START);
  (void)tx_thread_create(&s_host_thread,
                         "dfu_host",
                         dfu_host_worker,
                         0UL,
                         s_host_stack,
                         k_dfu_host_stack,
                         (UINT)k_dfu_host_priority,
                         (UINT)k_dfu_host_priority,
                         TX_NO_TIME_SLICE,
                         TX_AUTO_START);
}
#endif /* !RA8_SIMULATOR_MODE */

/* -------------------------------------------------------------------------- */
/* Startup */
/* -------------------------------------------------------------------------- */

/**
 * @brief Halt forever in WFI -- panic stop on init failure.
 * @return void.
 * @pre Called only after a fatal boot error.
 * @pre Interrupts may be in any state.
 * @post CPU is parked.
 * @post No further code runs.
 * @note Not reachable post-boot.
 * @since 0.1.0
 */
static void dfu_panic_halt(void)
{
  while (1) {
    __asm__ volatile("wfi");
  }
}

/**
 * @brief Route both ports' pins: HS as device, FS as host (config B).
 * @return void.
 * @details HS device: P4_08 VBUS sense (PSEL usb_hs), PD07 driven LOW so J7's
 * role is Device and U18 does not back-feed VBUS into the FS host's cable; D+/D-
 * are dedicated PHY balls (no PFS routing). FS host: P4_07 VBUS sense, P5_00
 * VBUSEN peripheral-routed so the USBFS controller sources J11 VBUS, P8_14/P8_15
 * data -- all PSEL usb_fs.
 * @pre IOPORT is reachable.
 * @pre Called once from ::dfu_setup_or_halt.
 * @post HS pins carry the device role, FS pins the host role.
 * @post PD07 is LOW (J7 device, not self-powered).
 * @note Panic-halts on any routing failure.
 * @since 0.1.0
 */
static void dfu_route_usb_or_halt(void)
{
  /* HS port: device role. PD07 LOW so U18 does not back-feed VBUS. */
  if (ra8_pfs_route_peripheral(k_dfu_pin_hs_vbus, k_ra8_psel_usb_hs, "dfu.hs_vbus") != k_ra8_ok) {
    dfu_panic_halt();
  }
  if (ra8_gpio_output_init(k_dfu_pin_hs_role, k_ra8_level_low) != k_ra8_ok) {
    dfu_panic_halt();
  }
  /* FS port: host role. P5_00 VBUSEN peripheral-routed sources J11. */
  if (ra8_pfs_route_peripheral(k_dfu_pin_fs_vbus, k_ra8_psel_usb_fs, "dfu.fs_vbus") != k_ra8_ok) {
    dfu_panic_halt();
  }
  if (ra8_pfs_route_peripheral(k_dfu_pin_fs_vbusen, k_ra8_psel_usb_fs, "dfu.fs_vbusen") !=
      k_ra8_ok) {
    dfu_panic_halt();
  }
  if (ra8_pfs_route_peripheral(k_dfu_pin_fs_dp, k_ra8_psel_usb_fs, "dfu.fs_dp") != k_ra8_ok) {
    dfu_panic_halt();
  }
  if (ra8_pfs_route_peripheral(k_dfu_pin_fs_dm, k_ra8_psel_usb_fs, "dfu.fs_dm") != k_ra8_ok) {
    dfu_panic_halt();
  }
}

/**
 * @brief Bring CGC + both USB clocks + SysTick + SCI8 + LEDs + pins up.
 * @return void.
 * @pre Reset_Handler finished C runtime init.
 * @pre SystemInit has run.
 * @post Console works; both USB ports' pins and clocks are live.
 * @post Panic-halts on any failure.
 * @note Called once from main.
 * @since 0.1.0
 */
static void dfu_setup_or_halt(void)
{
  uint32_t cpuclk0_hz = 0U;
  if (ra8_cgc_init() != k_ra8_ok) {
    dfu_panic_halt();
  }
  if (ra8_cgc_usbfs_clock_enable() != k_ra8_ok) {
    dfu_panic_halt();
  }
  if (ra8_cgc_usbhs_pll_enable() != k_ra8_ok) {
    dfu_panic_halt();
  }
  if (ra8_cgc_get_clock_hz(k_ra8_clock_id_cpuclk0, &cpuclk0_hz) != k_ra8_ok) {
    dfu_panic_halt();
  }
  if (ra8_time_init(cpuclk0_hz) != k_ra8_ok) {
    dfu_panic_halt();
  }
  if (ra8_board_uart_console_init((uint32_t)k_dfu_baud) != k_ra8_ok) {
    dfu_panic_halt();
  }
  if (ra8_board_led_init(k_ra8_board_led1) != k_ra8_ok) {
    dfu_panic_halt();
  }
  if (ra8_board_led_init(k_ra8_board_led2) != k_ra8_ok) {
    dfu_panic_halt();
  }
  dfu_route_usb_or_halt();
}

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmain"
/**
 * @brief Application entry: bring the board up, then hand off to ThreadX.
 * @return Never returns (``tx_kernel_enter`` is __noreturn).
 * @pre Reset_Handler copied .data and zeroed .bss.
 * @pre SystemInit set VTOR, FPU, priority grouping.
 * @post On clean entry the CPU stays in tx_kernel_enter forever.
 * @post On any HAL init failure the function halts in WFI.
 * @note Single entry point; not re-entrant.
 * @since 0.1.0
 */
int32_t main(void)
{
  dfu_setup_or_halt();

  ra8_isr_globals_enable();

#ifndef RA8_SIMULATOR_MODE
  tx_kernel_enter();
#endif

  dfu_panic_halt();
  return 0;
}
#pragma GCC diagnostic pop
