/**
 * @file ra8_dfu_device.c
 * @brief USBX DFU device class wired to the ra8_dfu MRAM program path.
 *
 * @par Tag
 * [Ring 4 / Service] {World: S}
 *
 * @details
 * Registers the vendored USBX DFU class and backs its callbacks with real
 * MRAM. `dfu_write` only stages a 64-byte block + marks it pending (ISR-safe);
 * the caller's device-worker thread drains it via ::ra8_dfu_device_worker_step,
 * which prepares the slot on the first block, programs each block, and commits
 * the header on end-of-download. `dfu_get_status` reports dfuDNBUSY until the
 * worker catches up; `dfu_read` serves DFU_UPLOAD straight out of the target
 * slot's MRAM body. The whole TU is firmware-only (the host test build defines
 * `RA8_OFF_TARGET` and has no USBX).
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra8_dfu_device.h"

/* Firmware + USBX only. The host test build defines RA8_OFF_TARGET (no USBX),
 * and a firmware build that does not pull in USBX -- e.g. dfu_copy_to_run, which
 * only needs the ra8_dfu core (boot/program/launch) -- has no ux_api.h on the
 * include path. In both cases this whole TU is empty; the only consumers of the
 * DFU device API are apps that `USES usbx`. */
#if !defined(RA8_OFF_TARGET) && __has_include("ux_api.h")

#include <string.h>

#include "ra8_attributes.h"
#include "ra8_dfu.h"
#include "ra8_usb.h"
#include "ux_api.h"
#include "ux_dcd_ra8_usb.h"
#include "ux_device_class_dfu.h"
#include "ux_device_stack.h"

/** @brief DFU transfer geometry + class-registration constants. */
typedef enum : uint32_t {
  k_ra8_dfu_dev_block_bytes = 64U,         /**< wTransferSize: bytes per block. */
  k_ra8_dfu_dev_page_mask   = 0x0000001FU, /**< 32-byte page round-up mask.     */
} ra8_dfu_dev_geom_t;

/** @brief USBX class-register interface index / config arguments. */
typedef enum : uint8_t {
  k_ra8_dfu_dev_reg_interface = 1U, /**< Interface number passed to register. */
  k_ra8_dfu_dev_reg_config    = 0U, /**< Configuration number.                */
} ra8_dfu_dev_reg_t;

/** @brief MRAM erased-state fill byte used to pad a short final DNLOAD block. */
typedef enum : uint8_t {
  k_ra8_dfu_dev_erased_byte = 0xFFU, /**< Erased-state byte for an MRAM page. */
} ra8_dfu_dev_fill_t;

/**
 * @struct ra8_dfu_dev_ctx_t
 * @brief Single-instance device state shared between the DFU callbacks (ISR)
 *        and the program worker (thread).
 */
typedef struct {
  ra8_dfu_slot_t     target;                           /**< Slot to program / serve.  */
  ra8_usb_speed_t    speed;                            /**< Bound controller.         */
  volatile bool      prepared;                         /**< Slot opened at start.     */
  volatile bool      manifest;                         /**< End-of-download seen.     */
  volatile bool      committed;                        /**< Header committed.         */
  volatile uint32_t  img_len;                          /**< Total bytes accepted.     */
  volatile uint32_t  writes;                           /**< Programmed-block counter. */
  volatile ra8_err_t prog_err;                         /**< Latched program error.    */
  uint8_t            stage[k_ra8_dfu_dev_block_bytes]; /**< One-block staging buffer. */
} ra8_dfu_dev_ctx_t;

/** @brief The single DFU device instance state. */
static ra8_dfu_dev_ctx_t s_dev = {
  .target = k_ra8_dfu_slot_b,
};

void ra8_dfu_device_set_target(ra8_dfu_slot_t target_slot)
{
  if ((target_slot == k_ra8_dfu_slot_a) || (target_slot == k_ra8_dfu_slot_b)) {
    s_dev.target = target_slot;
  }
}

/**
 * @brief DFU activate callback -- nothing to do (state lives at file scope).
 *
 * @details Called by the USBX device stack when the DFU class instance is
 * activated (host enumerates the DFU interface). All persistent state is held
 * in the file-scope ::ra8_dfu_dev_ctx_t singleton initialised before this
 * callback fires, so no per-activation setup is required.
 *
 * @param[in] dfu USBX DFU class instance pointer (unused; state is file-scope).
 *
 * @return Nothing (VOID callback; no meaningful return value).
 * @retval None This is a VOID callback; the return value is not meaningful.
 *
 * @pre The DFU class has been registered via ::internal_class_register.
 * @pre The USBX device stack is running and has completed enumeration.
 * @post No state change; the file-scope context is unmodified.
 * @post The USBX stack may proceed to service DFU requests immediately.
 *
 * @note Called from the USBX device stack thread context; not ISR-safe.
 * @since 0.1.0
 */
RA8_INTERNAL static VOID internal_dfu_activate(VOID* dfu)
{
  (void)dfu;
}

/**
 * @brief DFU deactivate callback -- retain captured image for inspection.
 *
 * @details Called by the USBX device stack when the DFU class instance is
 * deactivated (USB disconnect or reset). The captured image state in the
 * file-scope ::ra8_dfu_dev_ctx_t singleton is intentionally preserved so the
 * caller (device-worker thread) can inspect the outcome after the USB link
 * drops. No teardown is performed here.
 *
 * @param[in] dfu USBX DFU class instance pointer (unused; state is file-scope).
 *
 * @return Nothing (VOID callback; no meaningful return value).
 * @retval None This is a VOID callback; the return value is not meaningful.
 *
 * @pre The DFU class was previously activated via ::internal_dfu_activate.
 * @pre The USBX device stack is handling a disconnect or bus reset.
 * @post No state change; the file-scope context image data is retained.
 * @post Subsequent calls to ::ra8_dfu_device_manifested remain valid.
 *
 * @note Called from the USBX device stack thread context; not ISR-safe.
 * @since 0.1.0
 */
RA8_INTERNAL static VOID internal_dfu_deactivate(VOID* dfu)
{
  (void)dfu;
}

/**
 * @brief DFU write callback -- program one DNLOAD block straight into MRAM.
 *
 * @details Length 0 marks end-of-download (the worker commits the header on its
 * next step). Otherwise the block is copied into the staging buffer (padded up
 * to a 32-byte page with the erased value) and programmed to the target slot
 * synchronously, before the callback returns. This is deliberate: the
 * vendored USBX DFU class has no handler for the dfuDNBUSY state, so a
 * MEDIA_STATUS_BUSY reply would wedge the state machine on the next
 * DFU_GETSTATUS. Programming here keeps the device in the OK path
 * (DNLOAD_SYNC -> DNLOAD_IDLE) the host can actually poll through. The program
 * loop is SRAM-resident and masks IRQs internally, so it is safe to run from
 * the control-request context even though this TU lives in MRAM. The slot was
 * opened once by ::ra8_dfu_device_start.
 *
 * @param[in]  dfu          USBX DFU class instance pointer (unused).
 * @param[in]  block_number DFU block sequence number; multiplied by the block
 *                          size to compute the MRAM write offset.
 * @param[in]  data         Host-supplied payload for this block (non-NULL when
 *                          length is non-zero).
 * @param[in]  length       Byte count of the payload; 0 signals end-of-download.
 * @param[out] media_status Set to UX_SLAVE_CLASS_DFU_MEDIA_STATUS_OK on success
 *                          or UX_SLAVE_CLASS_DFU_MEDIA_STATUS_ERROR on fault.
 *
 * @return UX_SUCCESS always; errors are conveyed via media_status.
 * @retval UX_SUCCESS Block programmed (or end-of-download latched) successfully.
 *
 * @pre ::ra8_dfu_device_start has prepared the target slot before this callback
 *      fires (s_dev.prepared is true).
 * @pre `data` points to at least `length` valid bytes when `length` is non-zero.
 * @post On non-zero length, the block is written to MRAM at the computed offset.
 * @post On zero length, s_dev.manifest is set to signal end-of-download.
 *
 * @note Invoked from the USBX control-request context; not thread-safe with
 *       concurrent calls to itself, but the USBX stack serializes them.
 * @since 0.1.0
 */
RA8_INTERNAL static UINT
internal_dfu_write(VOID* dfu, ULONG block_number, UCHAR* data, ULONG length, ULONG* media_status)
{
  (void)dfu;
  if (length == 0UL) {
    s_dev.manifest = true;
  } else {
    uint32_t len = (uint32_t)length;
    if (len > (uint32_t)k_ra8_dfu_dev_block_bytes) {
      len = (uint32_t)k_ra8_dfu_dev_block_bytes;
    }
    (void)memcpy(s_dev.stage, data, (size_t)len);
    /* Pad the tail of a short final block to a 32-byte page with the erased
     * value so the 32-byte program granularity is satisfied. */
    const uint32_t plen =
      (uint32_t)((len + (uint32_t)k_ra8_dfu_dev_page_mask) & ~(uint32_t)k_ra8_dfu_dev_page_mask);
    for (uint32_t i = len; i < plen; i++) {
      s_dev.stage[i] = (uint8_t)k_ra8_dfu_dev_erased_byte;
    }
    const uint32_t  off = (uint32_t)block_number * (uint32_t)k_ra8_dfu_dev_block_bytes;
    const ra8_err_t we  = ra8_dfu_program_image(s_dev.target, off, s_dev.stage, plen);
    if (we != k_ra8_ok) {
      s_dev.prog_err = we;
    } else {
      s_dev.writes++;
      if ((off + plen) > s_dev.img_len) {
        s_dev.img_len = off + plen;
      }
    }
  }
  *media_status = (s_dev.prog_err == k_ra8_ok) ? (ULONG)UX_SLAVE_CLASS_DFU_MEDIA_STATUS_OK
                                               : (ULONG)UX_SLAVE_CLASS_DFU_MEDIA_STATUS_ERROR;
  return UX_SUCCESS;
}

/**
 * @brief DFU read callback -- serve DFU_UPLOAD from the target slot's MRAM body.
 *
 * @details Handles a DFU_UPLOAD request by copying bytes from the target slot's
 * MRAM body directly into the host-supplied buffer. The byte offset is
 * computed from block_number multiplied by the block size. If the offset is
 * at or beyond the image end, actual_length is set to zero to terminate the
 * upload. A partial final block is clipped to the remaining image length.
 *
 * @param[in]  dfu          USBX DFU class instance pointer (unused).
 * @param[in]  block_number DFU block sequence number; multiplied by the block
 *                          size to compute the MRAM read offset.
 * @param[out] data         Destination buffer; receives the MRAM payload bytes.
 * @param[in]  length       Maximum bytes the host can accept in this transfer.
 * @param[out] actual_length Set to the number of bytes copied; 0 when past
 *                           the end of the image.
 *
 * @return UX_SUCCESS always; zero actual_length signals end-of-upload.
 * @retval UX_SUCCESS Bytes copied to data (or actual_length set to 0 at EOF).
 *
 * @pre ::ra8_dfu_device_start has initialised the target slot and s_dev.img_len
 *      reflects the number of programmed bytes.
 * @pre `data` points to a buffer of at least `length` bytes.
 * @post actual_length contains the number of bytes placed in data.
 * @post No MRAM state is modified; this is a read-only path.
 *
 * @note Invoked from the USBX control-request context; not ISR-safe.
 * @since 0.1.0
 */
RA8_INTERNAL static UINT
internal_dfu_read(VOID* dfu, ULONG block_number, UCHAR* data, ULONG length, ULONG* actual_length)
{
  (void)dfu;
  const uint32_t off = (uint32_t)block_number * (uint32_t)k_ra8_dfu_dev_block_bytes;
  if (off >= s_dev.img_len) {
    *actual_length = 0UL;
    return UX_SUCCESS;
  }
  uint32_t remain = s_dev.img_len - off;
  if (remain > (uint32_t)length) {
    remain = (uint32_t)length;
  }
  const uintptr_t src = ra8_dfu_slot_base(s_dev.target) + (uintptr_t)off;
  (void)memcpy(data, (const void*)src, (size_t)remain);
  *actual_length = (ULONG)remain;
  return UX_SUCCESS;
}

/**
 * @brief DFU get-status callback -- OK normally, ERROR on a program fault.
 *
 * @details Never reports MEDIA_STATUS_BUSY: ::internal_dfu_write programs each
 * block synchronously, so by the time the host polls DFU_GETSTATUS the write has
 * already landed. BUSY would drive the class into the unhandled dfuDNBUSY state
 * and stall the next GET_STATUS. Instead this callback reads the latched
 * s_dev.prog_err and maps it to one of the two USBX media-status codes.
 *
 * @param[in]  dfu          USBX DFU class instance pointer (unused).
 * @param[out] media_status Set to UX_SLAVE_CLASS_DFU_MEDIA_STATUS_OK when no
 *                          program error has been latched, or
 *                          UX_SLAVE_CLASS_DFU_MEDIA_STATUS_ERROR otherwise.
 *
 * @return UX_SUCCESS always.
 * @retval UX_SUCCESS Status written to media_status; no internal error path.
 *
 * @pre The DFU class is active and ::internal_dfu_write has been invoked at
 *      least once (s_dev.prog_err is valid).
 * @pre `media_status` is a non-NULL pointer supplied by the USBX stack.
 * @post media_status reflects the current s_dev.prog_err latch value.
 * @post s_dev.prog_err is not modified by this call.
 *
 * @note Invoked from the USBX control-request context; not ISR-safe.
 * @since 0.1.0
 */
RA8_INTERNAL static UINT internal_dfu_get_status(VOID* dfu, ULONG* media_status)
{
  (void)dfu;
  *media_status = (s_dev.prog_err == k_ra8_ok) ? (ULONG)UX_SLAVE_CLASS_DFU_MEDIA_STATUS_OK
                                               : (ULONG)UX_SLAVE_CLASS_DFU_MEDIA_STATUS_ERROR;
  return UX_SUCCESS;
}

/**
 * @brief DFU notify callback -- latch end-of-download for the worker commit.
 *
 * @details Called by the USBX DFU class when a noteworthy event occurs. The
 * only event handled here is UX_SLAVE_CLASS_DFU_NOTIFICATION_END_DOWNLOAD:
 * when that notification arrives, s_dev.manifest is set to true so the
 * device-worker thread (::ra8_dfu_device_worker_step) knows to commit the
 * image header on its next invocation. All other notification codes are
 * silently ignored.
 *
 * @param[in] dfu          USBX DFU class instance pointer (unused).
 * @param[in] notification USBX DFU notification code; only
 *                         UX_SLAVE_CLASS_DFU_NOTIFICATION_END_DOWNLOAD is acted on.
 *
 * @return UX_SUCCESS always.
 * @retval UX_SUCCESS Notification processed (or ignored); no failure path.
 *
 * @pre The DFU class is active and the USBX stack is issuing notifications.
 * @pre s_dev is initialised (::ra8_dfu_device_start has run).
 * @post If notification is END_DOWNLOAD, s_dev.manifest is set to true.
 * @post All other notification codes leave s_dev unmodified.
 *
 * @note Invoked from the USBX device stack thread context; not ISR-safe.
 * @since 0.1.0
 */
RA8_INTERNAL static UINT internal_dfu_notify(VOID* dfu, ULONG notification)
{
  (void)dfu;
  if (notification == (ULONG)UX_SLAVE_CLASS_DFU_NOTIFICATION_END_DOWNLOAD) {
    s_dev.manifest = true;
  }
  return UX_SUCCESS;
}

/**
 * @brief Register the USBX DFU class with the MRAM-backed callbacks.
 *
 * @details Populates a UX_SLAVE_CLASS_DFU_PARAMETER structure that wires the
 * MRAM-backed callbacks (::internal_dfu_read, ::internal_dfu_write,
 * ::internal_dfu_get_status, ::internal_dfu_notify) and the lifecycle hooks
 * (::internal_dfu_activate, ::internal_dfu_deactivate) into the USBX DFU
 * class, then calls _ux_device_stack_class_register. The capabilities field
 * advertises both CAN_DOWNLOAD and CAN_UPLOAD. will_detach is cleared because
 * the device stays in DFU mode for the entire session.
 *
 * @param[in] framework     USB descriptor framework buffer (device + config +
 *                          DFU interface descriptor).
 * @param[in] framework_len Byte length of the descriptor framework.
 *
 * @return USBX status code from _ux_device_stack_class_register.
 * @retval UX_SUCCESS        Class registered; DFU callbacks are live.
 * @retval UX_ERROR          USBX internal registration failure.
 *
 * @pre _ux_system_initialize and _ux_device_stack_initialize have both
 *      returned UX_SUCCESS before this function is called.
 * @pre `framework` is non-NULL and `framework_len` describes a valid DFU
 *      descriptor set recognised by the USBX stack.
 * @post On UX_SUCCESS the DFU class entry function is registered at interface
 *       k_ra8_dfu_dev_reg_interface of configuration k_ra8_dfu_dev_reg_config.
 * @post On failure no partial state is cleaned up; the caller must handle it.
 *
 * @note Not thread-safe; call once during device init before attaching D+.
 * @since 0.1.0
 */
RA8_INTERNAL static UINT internal_class_register(unsigned char* framework, uint32_t framework_len)
{
  static UCHAR s_class_name[] = "ux_slave_class_dfu";

  UX_SLAVE_CLASS_DFU_PARAMETER p = {
    .ux_slave_class_dfu_parameter_will_detach = 0UL,
    .ux_slave_class_dfu_parameter_capabilities =
      (ULONG)(UX_SLAVE_CLASS_DFU_CAPABILITY_CAN_DOWNLOAD |
              UX_SLAVE_CLASS_DFU_CAPABILITY_CAN_UPLOAD),
    .ux_slave_class_dfu_parameter_instance_activate   = internal_dfu_activate,
    .ux_slave_class_dfu_parameter_instance_deactivate = internal_dfu_deactivate,
    .ux_slave_class_dfu_parameter_read                = internal_dfu_read,
    .ux_slave_class_dfu_parameter_write               = internal_dfu_write,
    .ux_slave_class_dfu_parameter_get_status          = internal_dfu_get_status,
    .ux_slave_class_dfu_parameter_notify              = internal_dfu_notify,
    .ux_slave_class_dfu_parameter_framework           = (UCHAR*)framework,
    .ux_slave_class_dfu_parameter_framework_length    = (ULONG)framework_len,
  };
  return _ux_device_stack_class_register(s_class_name,
                                         _ux_device_class_dfu_entry,
                                         (ULONG)k_ra8_dfu_dev_reg_interface,
                                         (ULONG)k_ra8_dfu_dev_reg_config,
                                         &p);
}

ra8_err_t ra8_dfu_device_start(ra8_usb_speed_t speed,
                               void*           usbx_pool,
                               uint32_t        pool_bytes,
                               unsigned char*  framework,
                               uint32_t        framework_len,
                               unsigned char*  strings,
                               uint32_t        strings_len,
                               unsigned char*  langids,
                               uint32_t        langids_len)
{
  if ((usbx_pool == nullptr) || (framework == nullptr) || (strings == nullptr) ||
      (langids == nullptr)) {
    return k_ra8_err_null_ptr;
  }
  if (_ux_system_initialize(usbx_pool, (ULONG)pool_bytes, UX_NULL, 0) != UX_SUCCESS) {
    return k_ra8_err_invalid_state;
  }
  if (_ux_device_stack_initialize((UCHAR*)UX_NULL,
                                  0,
                                  (UCHAR*)framework,
                                  (ULONG)framework_len,
                                  (UCHAR*)strings,
                                  (ULONG)strings_len,
                                  (UCHAR*)langids,
                                  (ULONG)langids_len,
                                  UX_NULL) != UX_SUCCESS) {
    return k_ra8_err_invalid_state;
  }
  if (internal_class_register(framework, framework_len) != UX_SUCCESS) {
    return k_ra8_err_invalid_state;
  }
  s_dev.speed       = speed;
  ra8_err_t dcd_err = ux_dcd_ra8_usb_initialize(speed);
  if (dcd_err != k_ra8_ok) {
    return dcd_err;
  }
  /* Open the target slot once, here in thread context, so the synchronous
   * per-block program path in internal_dfu_write never runs the controller
   * bring-up (which logs over UART) from the USB control-request context. */
  const ra8_err_t pe = ra8_dfu_program_prepare(s_dev.target);
  if (pe != k_ra8_ok) {
    return pe;
  }
  s_dev.prepared = true;
  return ra8_usb_device_attach(speed, true);
}

ra8_err_t ra8_dfu_device_worker_step(void)
{
  /* Per-block programming now happens synchronously in internal_dfu_write, so
   * the worker only finalizes the image header once the host signals
   * end-of-download (zero-length DFU_DNLOAD). The self-test twins use DFU_ABORT
   * instead of a manifest, so they never reach this branch -- only the
   * bootloader's real download path does. */
  if (s_dev.manifest && !s_dev.committed && s_dev.prepared && (s_dev.prog_err == k_ra8_ok)) {
    uint32_t other_seq = 0U;
    (void)ra8_dfu_slot_seq(ra8_dfu_other_slot(s_dev.target), &other_seq);
    const ra8_err_t ce = ra8_dfu_program_commit(s_dev.target, s_dev.img_len, other_seq + 1U);
    if (ce != k_ra8_ok) {
      s_dev.prog_err = ce;
    }
    s_dev.committed = true;
    return ce;
  }
  return s_dev.prog_err;
}

uint32_t ra8_dfu_device_image_len(void)
{
  return s_dev.img_len;
}

uint32_t ra8_dfu_device_block_writes(void)
{
  return s_dev.writes;
}

bool ra8_dfu_device_manifested(void)
{
  return s_dev.manifest;
}

ra8_err_t ra8_dfu_device_last_error(void)
{
  return s_dev.prog_err;
}

bool ra8_dfu_device_committed(void)
{
  return s_dev.committed;
}

#endif /* !RA8_OFF_TARGET */
