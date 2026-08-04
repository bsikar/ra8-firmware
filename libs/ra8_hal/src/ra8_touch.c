/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file ra8_touch.c
 * @brief Multi-touch input driver -- GoodIX GT911 backend (implementation)
 *
 * @par Tag
 * [Ring 3 / HAL] {World: NS}
 *
 * @details
 * GT911-specific implementation of ``ra8_touch.h``. Speaks to the IC
 * through the injected I2C bus seam (``ra8_i2c_bus_ops_t``, bound by the
 * app -- IIC_B in I2C-compat mode today, RIIC on a future board
 * revision) using the standard "write 16-bit register pointer, RESTART,
 * read N bytes" pattern. The driver carries no per-frame state -- all
 * touch decoding happens against caller-provided buffers.
 */

#include "ra8_touch.h"

#include <stddef.h>
#include <stdint.h>

#include "ra8_attributes.h"
#include "ra8_check.h"
#include "ra8_err.h"
#include "ra8_i2c_bus_ops.h"
#include "ra8_icu.h"
#include "ra8_log.h"
#include "ra8_touch_gt911_regs.h"

/** @brief Log tag. */
static const char* s_tag = "TOUCH";

/**
 * @enum ra8_touch_internal_t
 * @brief Implementation-only constants (no magic numbers).
 */
typedef enum : uint32_t {
  k_ra8_touch_product_id_byte0 = (uint32_t)'9', /**< First char of "911".    */
  k_ra8_touch_irq_pin_count    = 32U,           /**< Total ICU IRQ channels. */
  k_ra8_touch_byte_mask        = 0xFFU,         /**< 8-bit byte mask.        */
  k_ra8_touch_byte_shift       = 8U,            /**< Bits per byte.          */
} ra8_touch_internal_t;

/**
 * @struct ra8_touch_state_t
 * @brief Driver-private state slot.
 */
typedef struct {
  ra8_touch_event_fn_t cb;         /**< Registered callback or NULL.          */
  void*                ctx;        /**< Callback context.                     */
  ra8_i2c_bus_ops_t    bus;        /**< Injected I2C transfer seam.           */
  uint8_t              target_7b;  /**< 7-bit GT911 address.                  */
  uint8_t              max_points; /**< Cap on touches reported.              */
  uint8_t              irq_pin;    /**< IRQ pin or k_ra8_touch_irq_pin_unset. */
  bool                 opened;     /**< True between open() and close().      */
} ra8_touch_state_t;

/**
 * @var s_state
 * @brief Single driver-private state slot.
 *
 * @details
 * One touch IC is supported at a time. Holding state in file scope
 * avoids handing the caller a large opaque struct.
 */
static ra8_touch_state_t s_state;

/* ===========================================================================
 * Helpers -- decoding + I2C transport
 * ===========================================================================
 */

/**
 * @brief Pack a 16-bit GT911 register pointer into MSB-first wire order.
 *
 * @param[in]  reg     Register pointer.
 * @param[out] out_buf Two-byte destination buffer.
 *
 * @pre ``out_buf`` is non-NULL and has at least 2 bytes of room.
 * @post ``out_buf[0]`` holds the high byte, ``out_buf[1]`` the low byte.
 *
 * @details See implementation.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
RA8_INTERNAL
static inline void priv_pack_reg(uint16_t reg, uint8_t* out_buf)
{
  out_buf[0] = (uint8_t)(((uint32_t)reg >> k_ra8_touch_byte_shift) & k_ra8_touch_byte_mask);
  out_buf[1] = (uint8_t)((uint32_t)reg & k_ra8_touch_byte_mask);
}

/**
 * @brief Read ``len`` bytes from the GT911 starting at ``reg``.
 *
 * @details
 * Issues a write-then-RESTART-then-read transfer:
 *   * write 2 bytes (the 16-bit register pointer, MSB first),
 *   * read ``len`` bytes back into ``buf``.
 *
 * @param[in]  reg Starting register address.
 * @param[out] buf Destination buffer.
 * @param[in]  len Byte count.
 *
 * @return ``bus.transfer`` return code.
 *
 * @retval k_ra8_ok Operation succeeded.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t priv_gt911_read(uint16_t reg, uint8_t* buf, uint32_t len)
{
  uint8_t reg_bytes[k_ra8_touch_gt911_reg_ptr_bytes];
  priv_pack_reg(reg, reg_bytes);
  return s_state.bus.transfer(s_state.bus.ctx,
                              s_state.target_7b,
                              reg_bytes,
                              (uint32_t)k_ra8_touch_gt911_reg_ptr_bytes,
                              buf,
                              len);
}

/**
 * @brief Write a single byte ``value`` into 16-bit register ``reg``.
 *
 * @param[in] reg   Register address.
 * @param[in] value Byte to write.
 *
 * @return ``bus.write`` return code.
 *
 * @details See implementation.
 * @retval k_ra8_ok Operation succeeded.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t priv_gt911_write_byte(uint16_t reg, uint8_t value)
{
  enum : uint8_t {
    k_payload_len = k_ra8_touch_gt911_reg_ptr_bytes + 1U, /**< Payload length. */
  };
  uint8_t payload[k_payload_len];
  priv_pack_reg(reg, payload);
  payload[k_ra8_touch_gt911_reg_ptr_bytes] = value;
  return s_state.bus.write(s_state.bus.ctx,
                           s_state.target_7b,
                           payload,
                           (uint32_t)k_payload_len,
                           /*send_stop=*/true);
}

/**
 * @brief Decode one 8-byte GT911 point record.
 *
 * @param[in]  raw Source buffer (8 bytes, GT911 wire format).
 * @param[out] out Decoded point.
 *
 * @details See implementation.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
RA8_INTERNAL
static void priv_decode_one(const uint8_t* raw, ra8_touch_point_t* out)
{
  out->track_id = raw[k_ra8_touch_gt911_point_off_track];
  out->x = (uint16_t)((uint32_t)raw[k_ra8_touch_gt911_point_off_x_lsb] |
                      ((uint32_t)raw[k_ra8_touch_gt911_point_off_x_msb] << k_ra8_touch_byte_shift));
  out->y = (uint16_t)((uint32_t)raw[k_ra8_touch_gt911_point_off_y_lsb] |
                      ((uint32_t)raw[k_ra8_touch_gt911_point_off_y_msb] << k_ra8_touch_byte_shift));
  /* Pressure is the low byte of the 16-bit "size" field; the high byte
   * is rarely populated and is dropped here so the public type stays
   * one byte. */
  out->pressure = raw[k_ra8_touch_gt911_point_off_size_lsb];
}

/**
 * @brief Decode ``n`` GT911 point records from a flat raw buffer.
 *
 * @param[in]  raw       Source buffer holding ``n * 8`` bytes.
 * @param[in]  n_points  Number of point records present in ``raw``.
 * @param[out] out       Destination buffer.
 * @param[in]  max_count Capacity of ``out``.
 * @param[out] got_count Entries actually written.
 *
 * @details See implementation.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
RA8_INTERNAL
static void priv_decode_block(const uint8_t*     raw,
                              uint8_t            n_points,
                              ra8_touch_point_t* out,
                              uint8_t            max_count,
                              uint8_t*           got_count)
{
  uint8_t emit = n_points;
  if (emit > max_count) {
    emit = max_count;
  }
  if (emit > k_ra8_touch_gt911_max_points) {
    emit = k_ra8_touch_gt911_max_points;
  }
  for (uint8_t i = 0U; i < emit; i++) {
    const size_t off = (size_t)i * (size_t)k_ra8_touch_gt911_point_bytes;
    priv_decode_one(&raw[off], &out[i]);
  }
  *got_count = emit;
}

/* ===========================================================================
 * Lifecycle
 * ===========================================================================
 */

/**
 * @brief Validate a config descriptor.
 *
 * @param[in] cfg Non-NULL configuration descriptor.
 * @return ``k_ra8_ok`` if valid, otherwise ``k_ra8_err_invalid_arg``.
 *
 * @details See implementation.
 * @retval k_ra8_ok Operation succeeded.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t priv_validate_cfg(const ra8_touch_cfg_t* cfg)
{
  if ((cfg->bus.write == nullptr) || (cfg->bus.transfer == nullptr)) {
    return k_ra8_err_invalid_arg;
  }
  if ((cfg->target_7b != k_ra8_touch_gt911_addr_low) &&
      (cfg->target_7b != k_ra8_touch_gt911_addr_high)) {
    return k_ra8_err_invalid_arg;
  }
  return k_ra8_ok;
}

/**
 * @brief Optional ICU IRQ-pin programming for the GT911 INT line.
 *
 * @param[in] irq_pin IRQ pin index, or ::k_ra8_touch_irq_pin_unset.
 * @return ``k_ra8_ok`` if the pin was either out of range (skipped) or
 *         programmed for falling-edge detection.
 *
 * @details See implementation.
 * @retval k_ra8_ok Operation succeeded.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t priv_attach_irq_pin(uint8_t irq_pin)
{
  if ((uint32_t)irq_pin >= k_ra8_touch_irq_pin_count) {
    /* Either the sentinel value or any other "no pin" marker. */
    return k_ra8_ok;
  }
  const ra8_icu_irq_cfg_t cfg = {
    .sense      = k_ra8_icu_irqmd_falling,
    .filter_div = k_ra8_icu_fclksel_pclkb,
    .filter_en  = false,
  };
  return ra8_icu_configure_irq_pin(irq_pin, &cfg);
}

/**
 * @brief Stash the validated config into the state slot.
 *
 * @details See implementation.
 * @param[in] cfg See implementation.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
RA8_INTERNAL
static void priv_stash_state(const ra8_touch_cfg_t* cfg)
{
  s_state.bus        = cfg->bus;
  s_state.target_7b  = cfg->target_7b;
  s_state.irq_pin    = cfg->irq_pin;
  s_state.max_points = cfg->max_points;
  if ((s_state.max_points == 0U) || (s_state.max_points > k_ra8_touch_max_points)) {
    s_state.max_points = k_ra8_touch_max_points;
  }
}

/**
 * @brief Read the GT911 product id and verify the first byte is '9'.
 *
 * @details
 * Issues the 4-byte PRODUCT_ID read and rejects both a failed transfer
 * and a first byte other than ASCII '9' ("911" is the id string): either
 * means the bus is broken or the device behind the configured address is
 * not a GT911, so bring-up must not proceed. Both rejection legs compile
 * on every build; the host tests drive them through a scripted-RX
 * trampoline filled into the ``ra8_i2c_bus_ops_t`` seam.
 *
 * @return Result code.
 * @retval k_ra8_ok               Product id read back and byte 0 matched.
 * @retval k_ra8_err_hw_init_failed The read transfer failed, or the
 *                               product-id byte did not match.
 * @pre Module state is consistent.
 * @pre The injected I2C bus seam is bound.
 * @post Caller-visible state matches the documented contract.
 * @post No hardware state is mutated.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t priv_check_product_id(void)
{
  uint8_t         product[k_ra8_touch_gt911_id_bytes] = {};
  const ra8_err_t pid_err =
    priv_gt911_read(k_ra8_touch_gt911_reg_product, product, sizeof(product));
  if (pid_err != k_ra8_ok) {
    return k_ra8_err_hw_init_failed;
  }
  if ((uint32_t)product[0] != k_ra8_touch_product_id_byte0) {
    return k_ra8_err_hw_init_failed;
  }
  return k_ra8_ok;
}

/**
 * @brief Post-validate bring-up: product-id check, status ack, IRQ pin.
 *
 * @details
 * Runs against the injected bus seam already stashed in ``s_state``.
 * There is nothing to roll back on failure -- the bus peripheral is
 * app-owned, so the driver simply reports the error and leaves the
 * slot unopened.
 *
 * @param[in] cfg See implementation.
 * @return Result code.
 * @retval k_ra8_ok Operation succeeded.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t priv_open_finalise(const ra8_touch_cfg_t* cfg)
{
  const ra8_err_t pid_err = priv_check_product_id();
  if (pid_err != k_ra8_ok) {
    return pid_err;
  }
  (void)priv_gt911_write_byte(k_ra8_touch_gt911_reg_status, k_ra8_touch_gt911_cmd_clear_status);
  return priv_attach_irq_pin(cfg->irq_pin);
}

[[nodiscard]] ra8_err_t ra8_touch_open(const ra8_touch_cfg_t* cfg)
{
  RA8_CHECK_NULL_PTR(cfg, s_tag, "ra8_touch_open: cfg");
  if (s_state.opened) {
    return k_ra8_err_invalid_state;
  }
  const ra8_err_t cfg_err = priv_validate_cfg(cfg);
  RA8_RETURN_ON_ERROR(cfg_err, s_tag, "ra8_touch_open: cfg validation"); /* GCOVR_EXCL_BR_LINE */

  priv_stash_state(cfg);

  const ra8_err_t fin_err = priv_open_finalise(cfg);
  RA8_RETURN_ON_ERROR(fin_err, s_tag, "ra8_touch_open: finalise"); /* GCOVR_EXCL_BR_LINE */

  s_state.cb     = nullptr;
  s_state.ctx    = nullptr;
  s_state.opened = true;
  ra8_log_info(s_tag, "ra8_touch_open");
  return k_ra8_ok;
}

[[nodiscard]] ra8_err_t ra8_touch_close(void)
{
  if (!s_state.opened) {
    return k_ra8_err_not_initialized;
  }
  s_state.cb     = nullptr;
  s_state.ctx    = nullptr;
  s_state.opened = false;
  return k_ra8_ok;
}

/* ===========================================================================
 * Event handler attach / dispatch
 * ===========================================================================
 */

[[nodiscard]] ra8_err_t ra8_touch_attach_handler(ra8_touch_event_fn_t fn, void* ctx)
{
  if (!s_state.opened) {
    return k_ra8_err_not_initialized;
  }
  s_state.cb  = fn;
  s_state.ctx = ctx;
  return k_ra8_ok;
}

void ra8_touch_dispatch_irq(void)
{
  const ra8_touch_event_fn_t fn  = s_state.cb;
  void* const                ctx = s_state.ctx;
  if (fn != nullptr) {
    fn(ctx);
  }
}

/* ===========================================================================
 * Touch-point read
 * ===========================================================================
 */

/**
 * @brief Compute the actual number of touch records to drain.
 *
 * @details See implementation.
 * @param[in] status_byte See implementation.
 * @param[in] max_count See implementation.
 * @return Result code.
 * @retval k_ra8_ok Operation succeeded.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
RA8_INTERNAL
static uint8_t priv_clamp_emit(uint8_t status_byte, uint8_t max_count)
{
  uint8_t emit = status_byte & k_ra8_touch_gt911_status_count_mask;
  if (emit > max_count) {
    emit = max_count;
  }
  if (emit > s_state.max_points) {
    emit = s_state.max_points;
  }
  return emit;
}

/**
 * @brief Acknowledge the current frame; ignore I2C errors.
 *
 * @details See implementation.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
RA8_INTERNAL
static void priv_ack_frame(void)
{
  (void)priv_gt911_write_byte(k_ra8_touch_gt911_reg_status, k_ra8_touch_gt911_cmd_clear_status);
}

/**
 * @brief Inner read: assumes args validated and driver opened.
 *
 * @details See implementation.
 * @param[in] out_points See implementation.
 * @param[in] max_count See implementation.
 * @param[in] got_count See implementation.
 * @return Result code.
 * @retval k_ra8_ok Operation succeeded.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t
priv_read_inner(ra8_touch_point_t* out_points, uint8_t max_count, uint8_t* got_count)
{
  /* Step 1: read the status byte. */
  uint8_t         status     = 0U;
  const ra8_err_t status_err = priv_gt911_read(k_ra8_touch_gt911_reg_status, &status, 1U);
  if (status_err != k_ra8_ok) {
    *got_count = 0U;
    return k_ra8_err_hw_error;
  }

  /* Step 2: bit7 must be set, otherwise no frame is ready. */
  if ((status & k_ra8_touch_gt911_status_ready_mask) == 0U) {
    *got_count = 0U;
    return k_ra8_ok;
  }

  /* Step 3 + 4: clamp, pull the per-point block, decode. */
  const uint8_t emit = priv_clamp_emit(status, max_count);
  uint8_t raw[(size_t)k_ra8_touch_gt911_max_points * (size_t)k_ra8_touch_gt911_point_bytes] = {};
  if (emit > 0U) {
    const uint32_t  bytes   = (uint32_t)emit * (uint32_t)k_ra8_touch_gt911_point_bytes;
    const ra8_err_t blk_err = priv_gt911_read(k_ra8_touch_gt911_reg_point0, raw, bytes);
    if (blk_err != k_ra8_ok) {
      *got_count = 0U;
      priv_ack_frame();
      return k_ra8_err_hw_error;
    }
    priv_decode_block(raw, emit, out_points, max_count, got_count);
  } else {
    /* Frame-ready with zero contacts: a full-release frame. Nothing to
     * decode -- fall through to the ack so the IC latches the next one. */
    *got_count = 0U;
  }

  /* Step 5: ack the frame so the IC latches the next one. */
  priv_ack_frame();
  return k_ra8_ok;
}

[[nodiscard]] ra8_err_t
ra8_touch_read(ra8_touch_point_t* out_points, uint8_t max_count, uint8_t* got_count)
{
  RA8_CHECK_NULL_PTR(out_points, s_tag, "ra8_touch_read: out_points");
  RA8_CHECK_NULL_PTR(got_count, s_tag, "ra8_touch_read: got_count");
  if (max_count == 0U) {
    *got_count = 0U;
    return k_ra8_err_invalid_arg;
  }
  if (!s_state.opened) {
    *got_count = 0U;
    return k_ra8_err_not_initialized;
  }
  return priv_read_inner(out_points, max_count, got_count);
}

[[nodiscard]] ra8_err_t ra8_touch_calibrate(void)
{
  /* GT911 is factory-calibrated. Nothing to do. */
  return k_ra8_ok;
}

#ifdef UNIT_TEST
[[nodiscard]] ra8_err_t ra8_touch_test_decode(const uint8_t*     raw,
                                              uint8_t            n_points,
                                              ra8_touch_point_t* out_points,
                                              uint8_t            max_count,
                                              uint8_t*           got_count)
{
  RA8_CHECK_NULL_PTR(raw, s_tag, "test_decode: raw");
  RA8_CHECK_NULL_PTR(out_points, s_tag, "test_decode: out_points");
  RA8_CHECK_NULL_PTR(got_count, s_tag, "test_decode: got_count");
  if (max_count == 0U) {
    *got_count = 0U;
    return k_ra8_err_invalid_arg;
  }
  priv_decode_block(raw, n_points, out_points, max_count, got_count);
  return k_ra8_ok;
}
#endif /* UNIT_TEST */
