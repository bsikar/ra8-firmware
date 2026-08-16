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
 *    ``ra8_usb_host_*`` primitives. It enumerates the device, DFU_DNLOADs a
 *    deterministic multi-block image (with DFU_GETSTATUS polling + the
 *    zero-length manifest block), then DFU_UPLOADs it back and byte-checks it --
 *    proving the control-OUT firmware path round-trips intact.
 *
 * The download exercises the host control-OUT data stage added to
 * ``ra8_usb_host_control_xfer`` for this app (DFU_DNLOAD carries the firmware
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

#include "ra8_boot_entry.h"
#include "ra8_board_ek_ra8d2.h"
#include "ra8_cgc.h"
#include "ra8_err.h"
#include "ra8_gpio_constants.h"
#include "ra8_isr.h"
#include "ra8_port_constants.h"
#include "ra8_port_utils.h"
#include "ra8_time.h"
#include "ra8_usb.h"
#include "usb_selftest_dfu_steps.h"

#ifndef RA8_OFF_TARGET
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

/** @brief USBFS VBUSEN (P5_00) -- GPIO LOW for the device role. */
static const ra8_port_pin_t k_dfu_pin_fs_vbusen = (ra8_port_pin_t)k_ra8_board_usbfs_pin_vbusen;

/** @brief USBFS D+ (P8_14). */
static const ra8_port_pin_t k_dfu_pin_fs_dp = (ra8_port_pin_t)k_ra8_board_usbfs_pin_dp;

/** @brief USBFS D- (P8_15). */
static const ra8_port_pin_t k_dfu_pin_fs_dm = (ra8_port_pin_t)k_ra8_board_usbfs_pin_dm;

/** @brief USBHS_VBUS sense pin (P4_08, PSEL = 0x14). */
static const ra8_port_pin_t k_dfu_pin_hs_vbus = (ra8_port_pin_t)k_ra8_board_usbhs_pin_vbus;

/** @brief J7 host-power switch (PD07): HIGH = U18 supplies VBUS. */
static const ra8_port_pin_t k_dfu_pin_hs_pwr = (ra8_port_pin_t)k_ra8_board_usbhs_pin_pwr;

/* The J-Link OB CDC console (SCI8, PD_02 TXD / PD_03 RXD) bring-up -- pin
 * routing, baud, and SCI init -- is owned by ra8_board_uart_console_init(). */

/* Compile-time tunables, image geometry, text-formatter sizing, the host
 * progress-phase markers, and the Chapter-9 + DFU request constants all live in
 * "usb_selftest_dfu_steps.h" (shared with the host-ladder sibling). */

#ifndef RA8_OFF_TARGET

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
/* J-Link probes (device side) */
/* -------------------------------------------------------------------------- */

/* The host-ladder probes (s_dbg_phase / s_dbg_pid / s_dbg_blocks_ok /
 * s_dbg_mismatch / s_dbg_pass_count) live in the host-ladder sibling, the only
 * translation unit that reads or writes them. */

/** @brief Device-side download block-write count. */
static volatile uint32_t s_dbg_dev_writes;
/** @brief Device worker progress: 1 stack, 2 class, 3 dcd, 4 attach. */
static volatile uint32_t s_dbg_dev_step;
/** @brief Host-ladder first failing return code (0 = none). */
static volatile uint32_t s_dbg_host_err;

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
static UINT
dfu_write(VOID* dfu, ULONG block_number, const UCHAR* data, ULONG length, ULONG* media_status)
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
 * @typedef dfu_write_cb_t
 * @brief Signature of the USBX DFU download-write callback field.
 * @details
 * Mirrors ::UX_SLAVE_CLASS_DFU_PARAMETER::ux_slave_class_dfu_parameter_write,
 * whose vendored prototype takes a non-const ``UCHAR*`` payload. Our
 * ::dfu_write keeps the payload ``const`` (it never mutates the block), so the
 * assignment is funnelled through this typedef with a single explicit cast that
 * narrows only the const qualifier of the data pointer -- a layout-identical
 * function-pointer conversion.
 * @note Used solely at the ::dfu_class_register assignment site.
 * @since 0.1.0
 */
typedef UINT (
  *dfu_write_cb_t)(VOID* dfu, ULONG block_number, UCHAR* data, ULONG length, ULONG* media_status);

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
    .ux_slave_class_dfu_parameter_write               = (dfu_write_cb_t)dfu_write,
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
  ra8_err_t e    = ux_dcd_ra8_usb_initialize(k_ra8_usb_speed_fs);
  if (e != k_ra8_ok) {
    s_dbg_host_err = (uint32_t)e;
    return;
  }
  s_dbg_dev_step = 3U;
  e              = ra8_usb_device_attach(k_ra8_usb_speed_fs, true);
  if (e != k_ra8_ok) {
    s_dbg_host_err = (uint32_t)e;
    return;
  }
  s_dbg_dev_step = 4U;
  while (1) {
    tx_thread_sleep(k_dfu_idle_ticks);
  }
}

/**
 * @brief Host-side worker: retry the full pass until it succeeds, then park.
 * @param[in] arg ThreadX entry argument (unused).
 * @return Never returns.
 * @pre tx_application_define created this thread.
 * @pre The HS host pins, expander switch, and PLL are up (main).
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
#endif /* !RA8_OFF_TARGET */

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
  if (ra8_pfs_route_peripheral(k_dfu_pin_fs_vbus, k_ra8_psel_usb_fs, "dfu.fs_vbus") != k_ra8_ok) {
    dfu_panic_halt();
  }
  if (ra8_gpio_output_init(k_dfu_pin_fs_vbusen, k_ra8_level_low) != k_ra8_ok) {
    dfu_panic_halt();
  }
  if (ra8_pfs_route_peripheral(k_dfu_pin_fs_dp, k_ra8_psel_usb_fs, "dfu.fs_dp") != k_ra8_ok) {
    dfu_panic_halt();
  }
  if (ra8_pfs_route_peripheral(k_dfu_pin_fs_dm, k_ra8_psel_usb_fs, "dfu.fs_dm") != k_ra8_ok) {
    dfu_panic_halt();
  }
  if (ra8_board_io_expander_set_usbhs_host_mode() != k_ra8_ok) {
    dfu_panic_halt();
  }
  if (ra8_gpio_output_init(k_dfu_pin_hs_pwr, k_ra8_level_high) != k_ra8_ok) {
    dfu_panic_halt();
  }
  if (ra8_pfs_route_peripheral(k_dfu_pin_hs_vbus, k_ra8_psel_usb_hs, "dfu.hs_vbus") != k_ra8_ok) {
    dfu_panic_halt();
  }
}

/**
 * @brief Bring CGC + both USB clocks + SysTick + console + LEDs + pins up.
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

/**
 * @brief Application entry: bring the board up, then hand off to ThreadX.
 * @pre Reset_Handler copied .data and zeroed .bss.
 * @pre SystemInit set VTOR, FPU, priority grouping.
 * @post On clean entry the CPU stays in tx_kernel_enter forever.
 * @post On any HAL init failure the function halts in WFI.
 * @note Single entry point; not re-entrant.
 * @since 0.1.0
 */
void main(void)
{
  dfu_setup_or_halt();

  ra8_isr_globals_enable();

#ifndef RA8_OFF_TARGET
  tx_kernel_enter();
#endif

  dfu_panic_halt();
}
