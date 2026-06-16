/**
 * @file examples/ek_ra8d2/hw_validated/hil/usb_selftest_dfu/main.c
 * @brief USB self-loop: HS host DFU-downloads firmware to an FS DFU device, uploads + verifies
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * Rehearses a firmware-update-over-USB path entirely on-chip. The two USB ports
 * are cabled to EACH OTHER and one image runs both stacks:
 *
 *  - USBFS (J11) = DEVICE: a ThreadX + USBX DFU class in DFU mode
 *    (bInterfaceProtocol 0x02, so it enumerates straight into dfuIDLE -- no
 *    runtime/detach round-trip). Its write callback captures each downloaded
 *    block into a RAM "firmware" image; its read callback serves the image back
 *    on UPLOAD. DFU runs entirely over EP0 control transfers (no data
 *    endpoints).
 *  - USBHS (J7) = HOST: a self-contained polled host on the first-party
 *    ``ra_usb_host_*`` primitives. It enumerates the device, DFU_DNLOADs a
 *    deterministic multi-block image (with DFU_GETSTATUS polling + the
 *    zero-length manifest block), then DFU_UPLOADs it back and byte-checks it --
 *    proving the control-OUT firmware path round-trips intact.
 *
 * The download exercises the host control-OUT data stage added to
 * ``ra_usb_host_control_xfer`` for this app (DFU_DNLOAD carries the firmware
 * block in the SETUP data stage host -> device).
 *
 * Verdicts stream over SCI8 (J-Link OB CDC console, 115200) and are mirrored in
 * J-Link-readable probes (``s_dbg_*``).
 *
 * ## Pinout
 *
 * FS device: P4_07 VBUS sense, P5_00 VBUSEN GPIO LOW (device role), P8_14 D+,
 * P8_15 D- (PSEL usb_fs). HS host: SW4-8 to Host via the U15 expander, PD07
 * HIGH (U18 supplies J7 VBUS), P4_08 USBHS_VBUS (PSEL usb_hs). Console: PD_02/
 * PD_03 SCI8 (PSEL sci_async).
 *
 * @author Brighton Sikarskie
 * @date 2026-06-15
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
#include "ux_device_class_dfu.h"
#include "ux_device_stack.h"

extern void ra_time_on_tick(void);
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
static const ra_port_pin_t k_dfu_pin_fs_vbus =
  (ra_port_pin_t)(((uint16_t)k_ra_port_4 << 8) | (uint16_t)k_ra_pin_7);

/** @brief USBFS VBUSEN (P5_00) -- GPIO LOW for the device role. */
static const ra_port_pin_t k_dfu_pin_fs_vbusen =
  (ra_port_pin_t)(((uint16_t)k_ra_port_5 << 8) | (uint16_t)k_ra_pin_0);

/** @brief USBFS D+ (P8_14). */
static const ra_port_pin_t k_dfu_pin_fs_dp =
  (ra_port_pin_t)(((uint16_t)k_ra_port_8 << 8) | (uint16_t)k_ra_pin_14);

/** @brief USBFS D- (P8_15). */
static const ra_port_pin_t k_dfu_pin_fs_dm =
  (ra_port_pin_t)(((uint16_t)k_ra_port_8 << 8) | (uint16_t)k_ra_pin_15);

/** @brief USBHS_VBUS sense pin (P4_08, PSEL = 0x14). */
static const ra_port_pin_t k_dfu_pin_hs_vbus =
  (ra_port_pin_t)(((uint16_t)k_ra_port_4 << 8) | (uint16_t)k_ra_pin_8);

/** @brief J7 host-power switch (PD07): HIGH = U18 supplies VBUS. */
static const ra_port_pin_t k_dfu_pin_hs_pwr =
  (ra_port_pin_t)(((uint16_t)k_ra_port_13 << 8) | (uint16_t)k_ra_pin_7);

/** @brief J-Link OB CDC TX pin (PD_02 -- SCI8 TX). */
static const ra_port_pin_t k_dfu_pin_sci_tx =
  (ra_port_pin_t)(((uint16_t)k_ra_port_13 << 8) | (uint16_t)k_ra_pin_2);

/** @brief J-Link OB CDC RX pin (PD_03 -- SCI8 RX). */
static const ra_port_pin_t k_dfu_pin_sci_rx =
  (ra_port_pin_t)(((uint16_t)k_ra_port_13 << 8) | (uint16_t)k_ra_pin_3);

/* -------------------------------------------------------------------------- */
/* Tunables                                                                   */
/* -------------------------------------------------------------------------- */

/**
 * @enum dfu_config_t
 * @brief Compile-time settings: threads, pool, console, cadence.
 */
typedef enum : uint32_t {
  k_dfu_thread_stack    = 4096U,   /**< Device worker stack (bytes).      */
  k_dfu_host_stack      = 8192U,   /**< Host worker stack (bytes).        */
  k_dfu_usbx_pool_bytes = 32768U,  /**< USBX memory pool (bytes).         */
  k_dfu_idle_ticks      = 50U,     /**< Parked-loop back-off (ticks).     */
  k_dfu_boot_wait_ticks = 500U,    /**< Host start delay (1 ms ticks).    */
  k_dfu_retry_ticks     = 3000U,   /**< Pause between ladder retries.     */
  k_dfu_baud            = 115200U, /**< J-Link OB CDC log baud.           */
  k_dfu_sci_channel     = 8U,      /**< SCI8 -> J-Link OB CDC bridge.     */
  k_dfu_print_cap       = 160U,    /**< Bound for console-string scans.   */
  k_dfu_dev_priority    = 8U,      /**< Device bring-up worker priority.  */
  k_dfu_host_priority   = 24U,     /**< Host worker priority (below USBX). */
} dfu_config_t;

/**
 * @enum dfu_hex_t
 * @brief Hex/decimal text-formatter sizing constants.
 */
typedef enum : uint8_t {
  k_dfu_hex_chars_u16   = 4U,  /**< 16-bit value -> "ABCD".         */
  k_dfu_hex_chars_u32   = 8U,  /**< 32-bit value -> "ABCDEF01".     */
  k_dfu_dec_chars_u32   = 10U, /**< Max digits for a 32-bit count.  */
  k_dfu_nibble_bits     = 4U,  /**< Bits per hex nibble.            */
  k_dfu_hex_digit_split = 10U, /**< Threshold between '0-9'/'A-F'.  */
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

#ifndef RA_SIMULATOR_MODE

/* -------------------------------------------------------------------------- */
/* ThreadX workers + USBX pool storage                                        */
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
/* J-Link probes                                                              */
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
/* Shared per-block pattern                                                   */
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
/* USB descriptors (DFU mode: single DFU interface, EP0 only)                 */
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
/* Device side: USBX DFU class callbacks                                      */
/* -------------------------------------------------------------------------- */

/**
 * @brief DFU activate callback. The captured image lives at file scope.
 * @param[in] dfu Unused (single instance).
 * @return void.
 * @pre Called from the USBX class thread on SET_CONFIGURATION.
 * @pre The device entered DFU mode (descriptor protocol 0x02).
 * @post The device is ready to accept DFU_DNLOAD / DFU_UPLOAD.
 * @post No other state changes.
 * @note USBX serializes this with deactivate.
 * @since 0.1.0
 */
static VOID dfu_activate(VOID* dfu)
{
  (void)dfu;
}

/**
 * @brief DFU deactivate callback.
 * @param[in] dfu Unused.
 * @return void.
 * @pre Called from the USBX class thread on de-configuration.
 * @pre The DFU class is being torn down.
 * @post No state changes (the captured image is retained for inspection).
 * @post Safe to re-activate later.
 * @note USBX serializes this with activate.
 * @since 0.1.0
 */
static VOID dfu_deactivate(VOID* dfu)
{
  (void)dfu;
}

/**
 * @brief DFU write callback -- store one downloaded block into the RAM image.
 * @param[in]  dfu          Unused.
 * @param[in]  block_number DFU block sequence number (byte offset = n * 64).
 * @param[in]  data         Block payload from the control-OUT data stage.
 * @param[in]  length       Block length in bytes (0 ends the download).
 * @param[out] media_status Receives ::UX_SLAVE_CLASS_DFU_MEDIA_STATUS_OK.
 * @return UINT ``UX_SUCCESS`` on success.
 * @retval UX_SUCCESS Block stored (or zero-length end-of-download accepted).
 * @pre @p data holds @p length bytes; called from the USBX class thread.
 * @pre @p block_number * 64 + @p length fits ::s_dfu_image.
 * @post On a non-empty block the bytes are in ::s_dfu_image and the high-water
 *       mark advanced.
 * @post @p media_status is OK so the DFU state machine advances to DNLOAD_IDLE.
 * @note Single-writer (the USBX DFU thread).
 * @since 0.1.0
 */
static UINT dfu_write(VOID* dfu, ULONG block_number, UCHAR* data, ULONG length, ULONG* media_status)
{
  (void)dfu;
  const ULONG off = block_number * (ULONG)k_dfu_xfer_size;
  if ((length > 0UL) && (off + length <= (ULONG)sizeof(s_dfu_image))) {
    (void)memcpy(&s_dfu_image[off], data, (size_t)length);
    if ((uint32_t)(off + length) > s_dfu_image_len) {
      s_dfu_image_len = (uint32_t)(off + length);
    }
    s_dbg_dev_writes++;
  }
  *media_status = (ULONG)UX_SLAVE_CLASS_DFU_MEDIA_STATUS_OK;
  return UX_SUCCESS;
}

/**
 * @brief DFU read callback -- serve one block of the RAM image on UPLOAD.
 * @param[in]  dfu           Unused.
 * @param[in]  block_number  DFU block sequence number (byte offset = n * 64).
 * @param[out] data          Destination for the block payload.
 * @param[in]  length        Bytes the host can accept (wTransferSize).
 * @param[out] actual_length Receives the bytes returned (0 ends the upload).
 * @return UINT ``UX_SUCCESS``.
 * @retval UX_SUCCESS Block (or short/zero end-of-image) returned.
 * @pre @p data holds @p length bytes; called from the USBX class thread.
 * @pre ::s_dfu_image_len reflects the captured image.
 * @post @p actual_length holds min(remaining, @p length); a short block ends
 *       the upload.
 * @post No global state changes.
 * @note Single-reader (the USBX DFU thread).
 * @since 0.1.0
 */
static UINT dfu_read(VOID* dfu, ULONG block_number, UCHAR* data, ULONG length, ULONG* actual_length)
{
  (void)dfu;
  const ULONG off = block_number * (ULONG)k_dfu_xfer_size;
  if (off >= (ULONG)s_dfu_image_len) {
    *actual_length = 0UL;
    return UX_SUCCESS;
  }
  ULONG remain = (ULONG)s_dfu_image_len - off;
  if (remain > length) {
    remain = length;
  }
  (void)memcpy(data, &s_dfu_image[off], (size_t)remain);
  *actual_length = remain;
  return UX_SUCCESS;
}

/**
 * @brief DFU get-status callback -- report the media always ready.
 * @param[in]  dfu          Unused.
 * @param[out] media_status Receives ::UX_SLAVE_CLASS_DFU_MEDIA_STATUS_OK.
 * @return UINT ``UX_SUCCESS``.
 * @retval UX_SUCCESS Media OK.
 * @pre Called from the USBX class thread on DFU_GETSTATUS.
 * @pre The RAM image media is always writable.
 * @post @p media_status is OK.
 * @post No global state changes.
 * @note Single instance.
 * @since 0.1.0
 */
static UINT dfu_get_status(VOID* dfu, ULONG* media_status)
{
  (void)dfu;
  *media_status = (ULONG)UX_SLAVE_CLASS_DFU_MEDIA_STATUS_OK;
  return UX_SUCCESS;
}

/**
 * @brief DFU notify callback -- accept begin/end/manifest notifications.
 * @param[in] dfu          Unused.
 * @param[in] notification The DFU notification code.
 * @return UINT ``UX_SUCCESS``.
 * @retval UX_SUCCESS Notification accepted.
 * @pre Called from the USBX class thread at download/manifest boundaries.
 * @pre No long-running work is required (RAM image).
 * @post The state machine proceeds.
 * @post No global state changes.
 * @note Single instance.
 * @since 0.1.0
 */
static UINT dfu_notify(VOID* dfu, ULONG notification)
{
  (void)dfu;
  (void)notification;
  return UX_SUCCESS;
}

/**
 * @brief Bring USBX system + device stack up with the DFU framework.
 * @return UINT ``UX_SUCCESS`` on success.
 * @retval UX_SUCCESS Stack ready.
 * @pre File-scope pool reserved; thread context.
 * @pre The DFU framework is valid.
 * @post Device stack accepts class registrations.
 * @post On failure USBX state is undefined.
 * @note Single-call; not idempotent.
 * @since 0.1.0
 */
static UINT dfu_usbx_stack_up(void)
{
  if (_ux_system_initialize(s_usbx_pool, k_dfu_usbx_pool_bytes, UX_NULL, 0) != UX_SUCCESS) {
    return UX_ERROR;
  }
  return _ux_device_stack_initialize((UCHAR*)UX_NULL,
                                     0,
                                     s_device_framework,
                                     sizeof(s_device_framework),
                                     s_string_framework,
                                     sizeof(s_string_framework),
                                     s_language_id_framework,
                                     sizeof(s_language_id_framework),
                                     UX_NULL);
}

/**
 * @brief Register the DFU class against configuration 1, interface 0.
 * @return UINT ``UX_SUCCESS`` on success, propagated USBX error otherwise.
 * @retval UX_SUCCESS Class registered.
 * @pre ::dfu_usbx_stack_up has succeeded.
 * @pre The DFU callbacks are defined.
 * @post The DFU class is bound; activate fires on SET_CONFIGURATION.
 * @post No other class is registered.
 * @note Not re-entrant.
 * @since 0.1.0
 */
static UINT dfu_class_register(void)
{
  UX_SLAVE_CLASS_DFU_PARAMETER dfu_params = {
    .ux_slave_class_dfu_parameter_will_detach = 0UL,
    .ux_slave_class_dfu_parameter_capabilities =
      (ULONG)(UX_SLAVE_CLASS_DFU_CAPABILITY_CAN_DOWNLOAD |
              UX_SLAVE_CLASS_DFU_CAPABILITY_CAN_UPLOAD),
    .ux_slave_class_dfu_parameter_instance_activate   = dfu_activate,
    .ux_slave_class_dfu_parameter_instance_deactivate = dfu_deactivate,
    .ux_slave_class_dfu_parameter_read                = dfu_read,
    .ux_slave_class_dfu_parameter_write               = dfu_write,
    .ux_slave_class_dfu_parameter_get_status          = dfu_get_status,
    .ux_slave_class_dfu_parameter_notify              = dfu_notify,
    .ux_slave_class_dfu_parameter_framework           = s_device_framework,
    .ux_slave_class_dfu_parameter_framework_length    = (ULONG)sizeof(s_device_framework),
  };
  return _ux_device_stack_class_register((UCHAR*)"ux_slave_class_dfu",
                                         _ux_device_class_dfu_entry,
                                         1,
                                         0,
                                         &dfu_params);
}

/**
 * @brief Device-side worker: bring the DFU device up, then park.
 * @param[in] arg ThreadX entry argument (unused).
 * @return Never returns.
 * @pre tx_application_define created this thread.
 * @pre USB-FS pins + 48 MHz clock are up (main did both).
 * @post The FS device is attached in DFU mode; the DFU class services EP0.
 * @post On any bring-up failure the thread exits (s_dbg_dev_step frozen).
 * @note The DFU class runs its own thread; this worker only brings it up.
 * @since 0.1.0
 */
static VOID dfu_device_worker(ULONG arg)
{
  (void)arg;

  UINT ux = dfu_usbx_stack_up();
  if (ux != UX_SUCCESS) {
    s_dbg_host_err = (uint32_t)ux;
    return;
  }
  s_dbg_dev_step = 1U;
  ux             = dfu_class_register();
  if (ux != UX_SUCCESS) {
    s_dbg_host_err = (uint32_t)ux;
    return;
  }
  s_dbg_dev_step = 2U;
  ra_err_t e     = ux_dcd_ra_usb_initialize(k_ra_usb_speed_fs);
  if (e != k_ra_ok) {
    s_dbg_host_err = (uint32_t)e;
    return;
  }
  s_dbg_dev_step = 3U;
  e              = ra_usb_device_attach(k_ra_usb_speed_fs, true);
  if (e != k_ra_ok) {
    s_dbg_host_err = (uint32_t)e;
    return;
  }
  s_dbg_dev_step = 4U;
  while (1) {
    tx_thread_sleep(k_dfu_idle_ticks);
  }
}

/* -------------------------------------------------------------------------- */
/* Console helpers (SCI8 -> J-Link OB CDC)                                    */
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
 * @brief Push a literal block over SCI8 polled.
 * @param[in] data Buffer to send.
 * @param[in] len  Byte count.
 * @return ra_err_t passthrough from `ra_sci_write_polling`.
 * @retval k_ra_ok All bytes queued.
 * @pre @p data is non-NULL; SCI8 init already ran.
 * @pre @p len excludes any NUL terminator.
 * @post Bytes are in the SCI8 TX FIFO.
 * @post No other state changes.
 * @note Blocking polled TX.
 * @since 0.1.0
 */
[[nodiscard]] static ra_err_t dfu_sci_write(const uint8_t* data, uint32_t len)
{
  return ra_sci_write_polling((uint8_t)k_dfu_sci_channel, data, len);
}

/**
 * @brief Print a NUL-terminated ASCII string over the console.
 * @param[in] text String to print (CR/LF included by the caller).
 * @return ra_err_t propagated from the SCI helper.
 * @retval k_ra_ok All bytes queued.
 * @pre SCI8 init already ran; @p text is non-NULL.
 * @pre @p text is NUL-terminated within ::k_dfu_print_cap bytes.
 * @post The string bytes are in the SCI8 TX FIFO.
 * @post No other state changes.
 * @note Blocking polled TX.
 * @since 0.1.0
 */
[[nodiscard]] static ra_err_t dfu_print(const char* text)
{
  return dfu_sci_write((const uint8_t*)text, dfu_str_len(text));
}

/**
 * @brief Print a value as fixed-width uppercase hex.
 * @param[in] value  Value to print.
 * @param[in] digits Hex digit count (4 for u16, 8 for u32).
 * @return ra_err_t propagated from the SCI helper.
 * @retval k_ra_ok All bytes queued.
 * @pre SCI8 init already ran.
 * @pre @p digits is at most ::k_dfu_hex_chars_u32.
 * @post One fixed-width hex token is in the SCI8 TX FIFO.
 * @post No other state changes.
 * @note Blocking polled TX.
 * @since 0.1.0
 */
[[nodiscard]] static ra_err_t dfu_print_hex(uint32_t value, uint8_t digits)
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
 * @return ra_err_t propagated from the SCI helper.
 * @retval k_ra_ok All bytes queued.
 * @pre SCI8 init already ran.
 * @pre None beyond console readiness.
 * @post One ASCII decimal token is in the SCI8 TX FIFO.
 * @post No other state changes.
 * @note Blocking polled TX.
 * @since 0.1.0
 */
[[nodiscard]] static ra_err_t dfu_print_dec(uint32_t value)
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
 * @return ra_err_t propagated from the SCI helpers.
 * @retval k_ra_ok The diagnostic line is queued.
 * @pre SCI8 init already ran; @p what is NUL-terminated within the cap.
 * @pre None beyond console readiness.
 * @post One diagnostic line is in the SCI8 TX FIFO.
 * @post No other state changes.
 * @note Blocking polled TX.
 * @since 0.1.0
 */
[[nodiscard]] static ra_err_t dfu_print_fail(const char* what, ra_err_t err)
{
  ra_err_t e = dfu_print("ra8d2 dfu: FAIL ");
  if (e != k_ra_ok) {
    return e;
  }
  e = dfu_print(what);
  if (e != k_ra_ok) {
    return e;
  }
  e = dfu_print(" err=0x");
  if (e != k_ra_ok) {
    return e;
  }
  e = dfu_print_hex((uint32_t)err, (uint8_t)k_dfu_hex_chars_u32);
  if (e != k_ra_ok) {
    return e;
  }
  return dfu_print("\r\n");
}

/* -------------------------------------------------------------------------- */
/* Host side: enumerate, then DFU download + upload-verify                    */
/* -------------------------------------------------------------------------- */

/**
 * @enum dfu_req_t
 * @brief Chapter-9 + DFU class request / descriptor constants.
 */
typedef enum : uint16_t {
  k_dfu_bm_std_dev_in     = 0x80U, /**< bmRequestType: Std | Device | In.   */
  k_dfu_bm_std_dev_out    = 0x00U, /**< bmRequestType: Std | Device | Out.  */
  k_dfu_bm_class_if_out   = 0x21U, /**< bmRequestType: Class | Interface | Out. */
  k_dfu_bm_class_if_in    = 0xA1U, /**< bmRequestType: Class | Interface | In.  */
  k_dfu_breq_get_desc     = 0x06U, /**< GET_DESCRIPTOR.           */
  k_dfu_breq_set_addr     = 0x05U, /**< SET_ADDRESS.             */
  k_dfu_breq_set_config   = 0x09U, /**< SET_CONFIGURATION.       */
  k_dfu_breq_dnload       = 0x01U, /**< DFU_DNLOAD.              */
  k_dfu_breq_upload       = 0x02U, /**< DFU_UPLOAD.              */
  k_dfu_breq_getstatus    = 0x03U, /**< DFU_GETSTATUS.           */
  k_dfu_breq_abort        = 0x06U, /**< DFU_ABORT (-> dfuIDLE).  */
  k_dfu_desc_device       = 0x01U, /**< DEVICE descriptor type.  */
  k_dfu_dev_desc_len      = 18U,   /**< DEVICE descriptor length.*/
  k_dfu_off_dev_pid       = 10U,   /**< idProduct LSB byte offset.*/
  k_dfu_byte_bits         = 8U,    /**< Bits per byte.           */
  k_dfu_getstatus_len     = 6U,    /**< DFU_GETSTATUS payload len.*/
  k_dfu_off_status_state  = 4U,    /**< bState offset in GETSTATUS.*/
  k_dfu_state_dnload_idle = 5U,    /**< dfuDNLOAD-IDLE.          */
  k_dfu_state_idle        = 2U,    /**< dfuIDLE.                 */
} dfu_req_t;

/**
 * @enum dfu_enum_tune_t
 * @brief Timing / retry tunables for the polled enumeration + status polling.
 */
typedef enum : uint32_t {
  k_dfu_vbus_settle_ms = 200U,      /**< VBUS settle before probing.        */
  k_dfu_attach_to_ms   = 2000U,     /**< Wait for the D+ pull-up.           */
  k_dfu_debounce_ms    = 500U,      /**< Post-attach debounce (>=100 ms).   */
  k_dfu_reset_hold_ms  = 50U,       /**< USB bus-reset hold (>=10 ms).      */
  k_dfu_recovery_ms    = 20U,       /**< Post-reset recovery (TRSTRCY).     */
  k_dfu_addr_settle_ms = 5U,        /**< Post-SET_ADDRESS recovery.         */
  k_dfu_status_poll_ms = 2U,        /**< Pause between GETSTATUS polls.     */
  k_dfu_status_tries   = 50U,       /**< GETSTATUS polls before giving up.  */
  k_dfu_enum_tries     = 8U,        /**< Reset+probe attempts.              */
  k_dfu_attach_spin    = 50000000U, /**< Attach spin cap (frozen-tick guard). */
} dfu_enum_tune_t;

/**
 * @brief GET_DESCRIPTOR(DEVICE) over the polled control engine.
 * @param[out] desc Receives the 18-byte device descriptor.
 * @return Read outcome.
 * @retval k_ra_ok           All 18 bytes arrived.
 * @retval k_ra_err_hw_error A short descriptor came back.
 * @pre The bus is reset and the DCP targets the device's current address.
 * @pre @p desc holds at least ::k_dfu_dev_desc_len bytes.
 * @post @p desc carries the device descriptor on success.
 * @post No global state changes.
 * @note Blocking (polled control transfer).
 * @since 0.1.0
 */
[[nodiscard]] static ra_err_t dfu_ctrl_get_dev_desc(uint8_t* desc)
{
  const ra_usb_setup_t setup = {
    .bm_request_type = (uint8_t)k_dfu_bm_std_dev_in,
    .b_request       = (uint8_t)k_dfu_breq_get_desc,
    .w_value         = (uint16_t)((uint16_t)k_dfu_desc_device << (uint16_t)k_dfu_byte_bits),
    .w_index         = 0U,
    .w_length        = (uint16_t)k_dfu_dev_desc_len,
  };
  uint16_t       rx = 0U;
  const ra_err_t err =
    ra_usb_host_control_xfer(k_ra_usb_speed_hs, &setup, desc, (uint16_t)k_dfu_dev_desc_len, &rx);
  if (err != k_ra_ok) {
    return err;
  }
  return (rx == (uint16_t)k_dfu_dev_desc_len) ? k_ra_ok : k_ra_err_hw_error;
}

/**
 * @brief Wait for attach, then bus-reset + read the device descriptor.
 * @param[out] desc Receives the winning 18-byte device descriptor.
 * @return Hunt outcome.
 * @retval k_ra_ok             The device answered at address 0.
 * @retval k_ra_err_hw_timeout Nothing attached / nothing answered.
 * @pre ::ra_usb_host_init ran (host up, J7 VBUS supplied).
 * @pre ::ra_time_init has run (ms delays).
 * @post On success the DCP targets address 0 with UACT on.
 * @post On failure the bus is left in the last attempt's state.
 * @note Blocking; worst case a few seconds.
 * @since 0.1.0
 */
[[nodiscard]] static ra_err_t dfu_enum_hunt(uint8_t* desc)
{
  ra_delay_ms(k_dfu_vbus_settle_ms);
  const uint32_t t0 = ra_time_ms();
  for (uint32_t spin = 0U; spin < (uint32_t)k_dfu_attach_spin; spin++) {
    if (ra_usb_host_line_state(k_ra_usb_speed_hs) != 0U) {
      break;
    }
    if ((ra_time_ms() - t0) > (uint32_t)k_dfu_attach_to_ms) {
      break;
    }
  }
  ra_delay_ms(k_dfu_debounce_ms);
  ra_err_t err = k_ra_err_hw_timeout;
  for (uint8_t attempt = 0U; attempt < (uint8_t)k_dfu_enum_tries; attempt++) {
    (void)ra_usb_host_bus_reset(k_ra_usb_speed_hs, true);
    ra_delay_ms(k_dfu_reset_hold_ms);
    (void)ra_usb_host_bus_reset(k_ra_usb_speed_hs, false);
    (void)ra_usb_host_set_uact(k_ra_usb_speed_hs, true);
    ra_delay_ms(k_dfu_recovery_ms);
    (void)ra_usb_host_set_target(k_ra_usb_speed_hs, 0U);
    err = dfu_ctrl_get_dev_desc(desc);
    if (err == k_ra_ok) {
      return k_ra_ok;
    }
  }
  return err;
}

/**
 * @brief SET_ADDRESS to ::k_dfu_dev_addr, then retarget the DCP.
 * @return First failing step's error, or k_ra_ok.
 * @retval k_ra_ok The DCP now targets the operating address.
 * @pre ::dfu_enum_hunt succeeded (device answering at address 0).
 * @pre The bus is active (UACT on).
 * @post Later transfers carry tokens to ::k_dfu_dev_addr.
 * @post The set-address recovery delay has elapsed.
 * @note Blocking (one control transfer + settle).
 * @since 0.1.0
 */
[[nodiscard]] static ra_err_t dfu_enum_set_address(void)
{
  const ra_usb_setup_t setup = {
    .bm_request_type = (uint8_t)k_dfu_bm_std_dev_out,
    .b_request       = (uint8_t)k_dfu_breq_set_addr,
    .w_value         = (uint16_t)k_dfu_dev_addr,
    .w_index         = 0U,
    .w_length        = 0U,
  };
  const ra_err_t err = ra_usb_host_control_xfer(k_ra_usb_speed_hs, &setup, nullptr, 0U, nullptr);
  if (err != k_ra_ok) {
    return err;
  }
  ra_delay_ms(k_dfu_addr_settle_ms);
  return ra_usb_host_set_target(k_ra_usb_speed_hs, (uint8_t)k_dfu_dev_addr);
}

/**
 * @brief SET_CONFIGURATION(::k_dfu_config_val) on the addressed device.
 * @return Control-transfer outcome.
 * @retval k_ra_ok The device entered the Configured state (dfuIDLE).
 * @pre ::dfu_enum_set_address succeeded.
 * @pre The DCP targets ::k_dfu_dev_addr.
 * @post On success the DFU class is active in DFU mode.
 * @post No global state changes.
 * @note Blocking (one control transfer).
 * @since 0.1.0
 */
[[nodiscard]] static ra_err_t dfu_enum_set_config(void)
{
  const ra_usb_setup_t setup = {
    .bm_request_type = (uint8_t)k_dfu_bm_std_dev_out,
    .b_request       = (uint8_t)k_dfu_breq_set_config,
    .w_value         = (uint16_t)k_dfu_config_val,
    .w_index         = 0U,
    .w_length        = 0U,
  };
  return ra_usb_host_control_xfer(k_ra_usb_speed_hs, &setup, nullptr, 0U, nullptr);
}

/**
 * @brief DFU_GETSTATUS: read the 6-byte status and return the bState field.
 * @param[out] out_state Receives the DFU state machine byte.
 * @return Control-transfer outcome.
 * @retval k_ra_ok           Status read; @p out_state valid.
 * @retval k_ra_err_hw_error A short status payload came back.
 * @pre The device is configured in DFU mode.
 * @pre @p out_state is non-NULL.
 * @post @p out_state holds bState on success.
 * @post No global state changes.
 * @note Blocking (one control-IN transfer).
 * @since 0.1.0
 */
[[nodiscard]] static ra_err_t dfu_getstatus(uint8_t* out_state)
{
  uint8_t              status[k_dfu_getstatus_len] = {};
  const ra_usb_setup_t setup                       = {
    .bm_request_type = (uint8_t)k_dfu_bm_class_if_in,
    .b_request       = (uint8_t)k_dfu_breq_getstatus,
    .w_value         = 0U,
    .w_index         = (uint16_t)k_dfu_intf,
    .w_length        = (uint16_t)k_dfu_getstatus_len,
  };
  uint16_t       rx = 0U;
  const ra_err_t err =
    ra_usb_host_control_xfer(k_ra_usb_speed_hs, &setup, status, (uint16_t)k_dfu_getstatus_len, &rx);
  if (err != k_ra_ok) {
    return err;
  }
  if (rx != (uint16_t)k_dfu_getstatus_len) {
    return k_ra_err_hw_error;
  }
  *out_state = status[k_dfu_off_status_state];
  return k_ra_ok;
}

/**
 * @brief Poll DFU_GETSTATUS until the device reports @p want_state.
 * @param[in] want_state The DFU bState the caller is waiting for.
 * @return Poll outcome.
 * @retval k_ra_ok             The device reached @p want_state.
 * @retval k_ra_err_hw_timeout It did not within ::k_dfu_status_tries.
 * @pre The device is configured in DFU mode.
 * @pre A control transfer (DNLOAD) preceded this.
 * @post On success the state machine is at @p want_state.
 * @post On failure the last poll's state is whatever the device held.
 * @note Blocking; bounded poll with ms pacing.
 * @since 0.1.0
 */
[[nodiscard]] static ra_err_t dfu_wait_state(uint8_t want_state)
{
  for (uint32_t i = 0U; i < (uint32_t)k_dfu_status_tries; i++) {
    uint8_t        state = 0U;
    const ra_err_t err   = dfu_getstatus(&state);
    if (err != k_ra_ok) {
      return err;
    }
    if (state == want_state) {
      return k_ra_ok;
    }
    ra_delay_ms(k_dfu_status_poll_ms);
  }
  return k_ra_err_hw_timeout;
}

/**
 * @brief DFU_DNLOAD one block, then poll to dfuDNLOAD-IDLE.
 * @param[in] block The DFU block sequence number (wValue).
 * @param[in] data  Block payload (control-OUT data stage).
 * @param[in] len   Block length in bytes.
 * @return First failing step's error, or k_ra_ok.
 * @retval k_ra_ok The block was downloaded and the device is DNLOAD-IDLE.
 * @pre The device is configured in DFU mode (dfuIDLE / dfuDNLOAD-IDLE).
 * @pre @p data holds @p len bytes.
 * @post The device captured the block; the state machine is DNLOAD-IDLE.
 * @post No global state changes.
 * @note Blocking; uses the host control-OUT data stage.
 * @since 0.1.0
 */
[[nodiscard]] static ra_err_t dfu_dnload_block(uint16_t block, uint8_t* data, uint16_t len)
{
  const ra_usb_setup_t setup = {
    .bm_request_type = (uint8_t)k_dfu_bm_class_if_out,
    .b_request       = (uint8_t)k_dfu_breq_dnload,
    .w_value         = block,
    .w_index         = (uint16_t)k_dfu_intf,
    .w_length        = len,
  };
  const ra_err_t err = ra_usb_host_control_xfer(k_ra_usb_speed_hs, &setup, data, len, nullptr);
  if (err != k_ra_ok) {
    return err;
  }
  return dfu_wait_state((uint8_t)k_dfu_state_dnload_idle);
}

/**
 * @brief Download the whole rehearsal image, then DFU_ABORT back to dfuIDLE.
 * @return First failing step's error, or k_ra_ok.
 * @retval k_ra_ok All blocks downloaded; the device is back in dfuIDLE.
 * @pre The device is configured in DFU mode.
 * @pre The DFU functional descriptor advertises CAN_DOWNLOAD.
 * @post The device captured ::k_dfu_image_bytes; the state machine is dfuIDLE.
 * @post On failure the caller logs the offending step.
 * @note Blocking; runs on the host worker thread. The "real" DFU end-of-download
 *       (zero-length DFU_DNLOAD -> MANIFEST) is intentionally NOT used here: this
 *       USBX DFU class is not manifestation-tolerant, so after MANIFEST it parks
 *       in dfuMANIFEST-WAIT-RESET and only a USB bus reset returns it to a usable
 *       state -- which would tear down the in-place UPLOAD round-trip. DFU_ABORT
 *       returns dfuDNLOAD-IDLE -> dfuIDLE without a reset, so the same enumerated
 *       device can immediately be UPLOAD-verified.
 * @since 0.1.0
 */
[[nodiscard]] static ra_err_t dfu_download_all(void)
{
  for (uint16_t b = 0U; b < (uint16_t)k_dfu_blocks; b++) {
    uint8_t block[k_dfu_xfer_size] = {};
    dfu_pattern_fill((uint32_t)b, block, (uint32_t)k_dfu_xfer_size);
    const ra_err_t err = dfu_dnload_block(b, block, (uint16_t)k_dfu_xfer_size);
    if (err != k_ra_ok) {
      return err;
    }
  }
  /* DFU_ABORT closes the download (dfuDNLOAD-IDLE -> dfuIDLE) without the
   * manifest's wait-for-reset, so the UPLOAD phase runs on the same device. */
  const ra_usb_setup_t setup = {
    .bm_request_type = (uint8_t)k_dfu_bm_class_if_out,
    .b_request       = (uint8_t)k_dfu_breq_abort,
    .w_value         = 0U,
    .w_index         = (uint16_t)k_dfu_intf,
    .w_length        = 0U,
  };
  const ra_err_t err = ra_usb_host_control_xfer(k_ra_usb_speed_hs, &setup, nullptr, 0U, nullptr);
  if (err != k_ra_ok) {
    return err;
  }
  return dfu_wait_state((uint8_t)k_dfu_state_idle);
}

/**
 * @brief DFU_UPLOAD the image back block by block and byte-check each.
 * @return Verify outcome.
 * @retval k_ra_ok                All blocks matched their downloaded pattern.
 * @retval k_ra_err_invalid_size  A block returned the wrong length.
 * @retval k_ra_err_invalid_state A block's bytes differed.
 * @pre ::dfu_download_all succeeded; the device is in dfuIDLE.
 * @pre The DFU functional descriptor advertises CAN_UPLOAD.
 * @post ::s_dbg_blocks_ok counts verified blocks; ::s_dbg_mismatch records the
 *       first bad block on failure.
 * @post No device state retained between blocks.
 * @note Blocking; one control-IN per block.
 * @since 0.1.0
 */
[[nodiscard]] static ra_err_t dfu_upload_verify(void)
{
  s_dbg_blocks_ok = 0U;
  for (uint16_t b = 0U; b < (uint16_t)k_dfu_blocks; b++) {
    uint8_t              got[k_dfu_xfer_size] = {};
    const ra_usb_setup_t setup                = {
      .bm_request_type = (uint8_t)k_dfu_bm_class_if_in,
      .b_request       = (uint8_t)k_dfu_breq_upload,
      .w_value         = b,
      .w_index         = (uint16_t)k_dfu_intf,
      .w_length        = (uint16_t)k_dfu_xfer_size,
    };
    uint16_t       rx = 0U;
    const ra_err_t err =
      ra_usb_host_control_xfer(k_ra_usb_speed_hs, &setup, got, (uint16_t)k_dfu_xfer_size, &rx);
    if (err != k_ra_ok) {
      return err;
    }
    if (rx != (uint16_t)k_dfu_xfer_size) {
      s_dbg_mismatch = (uint32_t)b;
      return k_ra_err_invalid_size;
    }
    uint8_t want[k_dfu_xfer_size] = {};
    dfu_pattern_fill((uint32_t)b, want, (uint32_t)k_dfu_xfer_size);
    if (memcmp(got, want, (size_t)k_dfu_xfer_size) != 0) {
      s_dbg_mismatch = (uint32_t)b;
      return k_ra_err_invalid_state;
    }
    s_dbg_blocks_ok++;
  }
  return k_ra_ok;
}

/**
 * @brief Run the full host pass: enumerate, download, upload-verify.
 * @return First failing step's error, or k_ra_ok.
 * @retval k_ra_ok The pass printed DFU PASS.
 * @pre Device-side DFU class is registered (other thread).
 * @pre The self-loop cable connects J7 to J11.
 * @post On success ::s_dbg_pass_count advanced and LED2 is on.
 * @post On failure the host controller is deinitialized for a clean retry.
 * @note Blocking; runs on the low-priority host thread.
 * @since 0.1.0
 */
[[nodiscard]] static ra_err_t dfu_host_pass(void)
{
  s_dbg_phase  = (uint32_t)k_dfu_phase_init;
  ra_err_t err = dfu_print("ra8d2 dfu: host up on USB-HS, probing the loop...\r\n");
  if (err != k_ra_ok) {
    return err;
  }
  err = ra_usb_host_init(k_ra_usb_speed_hs);
  if (err != k_ra_ok) {
    (void)dfu_print_fail("host init", err);
    return err;
  }

  s_dbg_phase                      = (uint32_t)k_dfu_phase_enum;
  uint8_t desc[k_dfu_dev_desc_len] = {};
  err                              = dfu_enum_hunt(desc);
  if (err != k_ra_ok) {
    (void)dfu_print_fail("enumerate", err);
    (void)ra_usb_host_deinit(k_ra_usb_speed_hs);
    return err;
  }
  s_dbg_pid = (uint32_t)desc[k_dfu_off_dev_pid] |
              ((uint32_t)desc[(uint32_t)k_dfu_off_dev_pid + 1U] << (uint32_t)k_dfu_byte_bits);
  err       = dfu_enum_set_address();
  if (err != k_ra_ok) {
    (void)dfu_print_fail("set_address", err);
    (void)ra_usb_host_deinit(k_ra_usb_speed_hs);
    return err;
  }
  err = dfu_enum_set_config();
  if (err != k_ra_ok) {
    (void)dfu_print_fail("set_config", err);
    (void)ra_usb_host_deinit(k_ra_usb_speed_hs);
    return err;
  }
  err = dfu_print("ra8d2 dfu: enumerated pid=0x");
  if (err == k_ra_ok) {
    err = dfu_print_hex(s_dbg_pid, (uint8_t)k_dfu_hex_chars_u16);
  }
  if (err == k_ra_ok) {
    err = dfu_print("\r\n");
  }
  if (err != k_ra_ok) {
    return err;
  }

  s_dbg_phase = (uint32_t)k_dfu_phase_download;
  err         = dfu_download_all();
  if (err != k_ra_ok) {
    (void)dfu_print_fail("dnload", err);
    (void)ra_usb_host_deinit(k_ra_usb_speed_hs);
    return err;
  }

  s_dbg_phase = (uint32_t)k_dfu_phase_upload;
  err         = dfu_upload_verify();
  if (err != k_ra_ok) {
    (void)dfu_print_fail("upload verify", err);
    (void)ra_usb_host_deinit(k_ra_usb_speed_hs);
    return err;
  }

  s_dbg_phase = (uint32_t)k_dfu_phase_pass;
  s_dbg_pass_count++;
  err = dfu_print("ra8d2 dfu: ");
  if (err == k_ra_ok) {
    err = dfu_print_dec((uint32_t)k_dfu_blocks);
  }
  if (err == k_ra_ok) {
    err = dfu_print(" blocks downloaded + verified -- USB SELFTEST DFU PASS\r\n");
  }
  if (err != k_ra_ok) {
    return err;
  }
  (void)ra_board_led_on(k_ra_board_led2);
  return k_ra_ok;
}

/**
 * @brief Host-side worker: retry the full pass until it succeeds, then park.
 * @param[in] arg ThreadX entry argument (unused).
 * @return Never returns.
 * @pre tx_application_define created this thread.
 * @pre The HS host pins, expander switch, and PLL are up (main).
 * @post On success the pass counter and LED2 are latched.
 * @post Retries forever otherwise; each failure prints its step.
 * @note Blocking calls; ms timeouts via ra_time.
 * @since 0.1.0
 */
static VOID dfu_host_worker(ULONG arg)
{
  (void)arg;

  tx_thread_sleep(k_dfu_boot_wait_ticks);
  for (;;) {
    const ra_err_t err = dfu_host_pass();
    if (err == k_ra_ok) {
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
#endif /* !RA_SIMULATOR_MODE */

/* -------------------------------------------------------------------------- */
/* Startup                                                                    */
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
 * @brief Route both ports' pins: FS as device, HS as host.
 * @return void.
 * @pre IOPORT and the U15 expander are reachable.
 * @pre Called once from ::dfu_setup_or_halt.
 * @post FS pins carry the device role, HS pins the host role, PD07 HIGH.
 * @post Panic-halts on any routing failure.
 * @note Panic-halts on any routing failure.
 * @since 0.1.0
 */
static void dfu_route_usb_or_halt(void)
{
  if (ra_pfs_route_peripheral(k_dfu_pin_fs_vbus, k_ra_psel_usb_fs, "dfu.fs_vbus") != k_ra_ok) {
    dfu_panic_halt();
  }
  if (ra_gpio_output_init(k_dfu_pin_fs_vbusen, k_ra_level_low) != k_ra_ok) {
    dfu_panic_halt();
  }
  if (ra_pfs_route_peripheral(k_dfu_pin_fs_dp, k_ra_psel_usb_fs, "dfu.fs_dp") != k_ra_ok) {
    dfu_panic_halt();
  }
  if (ra_pfs_route_peripheral(k_dfu_pin_fs_dm, k_ra_psel_usb_fs, "dfu.fs_dm") != k_ra_ok) {
    dfu_panic_halt();
  }
  if (ra_board_io_expander_set_usbhs_host_mode() != k_ra_ok) {
    dfu_panic_halt();
  }
  if (ra_gpio_output_init(k_dfu_pin_hs_pwr, k_ra_level_high) != k_ra_ok) {
    dfu_panic_halt();
  }
  if (ra_pfs_route_peripheral(k_dfu_pin_hs_vbus, k_ra_psel_usb_hs, "dfu.hs_vbus") != k_ra_ok) {
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
  uint32_t pclka_hz   = 0U;
  if (ra_cgc_init() != k_ra_ok) {
    dfu_panic_halt();
  }
  if (ra_cgc_usbfs_clock_enable() != k_ra_ok) {
    dfu_panic_halt();
  }
  if (ra_cgc_usbhs_pll_enable() != k_ra_ok) {
    dfu_panic_halt();
  }
  if (ra_cgc_get_clock_hz(k_ra_clock_id_cpuclk0, &cpuclk0_hz) != k_ra_ok) {
    dfu_panic_halt();
  }
  if (ra_cgc_get_clock_hz(k_ra_clock_id_pclka, &pclka_hz) != k_ra_ok) {
    dfu_panic_halt();
  }
  if (ra_time_init(cpuclk0_hz) != k_ra_ok) {
    dfu_panic_halt();
  }
  if (ra_pfs_route_peripheral(k_dfu_pin_sci_tx, k_ra_psel_sci_async, "dfu.txd8") != k_ra_ok) {
    dfu_panic_halt();
  }
  if (ra_pfs_route_peripheral(k_dfu_pin_sci_rx, k_ra_psel_sci_async, "dfu.rxd8") != k_ra_ok) {
    dfu_panic_halt();
  }
  const ra_sci_cfg_t sci_cfg = {
    .baud      = k_dfu_baud,
    .data_bits = k_ra_sci_data_8,
    .parity    = k_ra_sci_parity_none,
    .stop_bits = k_ra_sci_stop_1,
    .pclk_hz   = pclka_hz,
  };
  if (ra_sci_init((uint8_t)k_dfu_sci_channel, &sci_cfg) != k_ra_ok) {
    dfu_panic_halt();
  }
  if (ra_board_led_init(k_ra_board_led1) != k_ra_ok) {
    dfu_panic_halt();
  }
  if (ra_board_led_init(k_ra_board_led2) != k_ra_ok) {
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

  ra_isr_globals_enable();

#ifndef RA_SIMULATOR_MODE
  tx_kernel_enter();
#endif

  dfu_panic_halt();
  return 0;
}
#pragma GCC diagnostic pop
