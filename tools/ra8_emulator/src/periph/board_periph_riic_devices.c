/**
 * @file board_periph_riic_devices.c
 * @brief RIIC bus registry, PI4IOE5V6408 expander, and OV5640 sensor models
 * @details Implements the fixed-capacity device side of the RIIC model. The
 * controller resolves each address through this registry, while the device
 * callbacks retain their own bounded register state and transfer cursors.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include "board_periph_riic_devices_internal.h"
#include "emu_host_io_internal.h"

/** @brief Registry and PI4IOE5V6408 geometry. */
typedef enum : uint32_t {
  k_riic_dev_max     = 4U,    /**< Device-registry capacity.                 */
  k_pi4ioe_addr_7b   = 0x43U, /**< EK-RA8D2 U15 default 7-bit address.       */
  k_pi4ioe_reg_max   = 0x10U, /**< Register-file size modelled (0x00..0x0F). */
  k_pi4ioe_reg_devid = 0x01U, /**< Device-id register.                       */
  k_pi4ioe_devid_val = 0xA0U, /**< Device-id reset default.                  */
} riic_device_const_t;

/**
 * @brief One PI4IOE5V6408 I/O expander on the modelled I2C bus.
 * @details Holds the auto-incrementing register pointer and the bounded
 * register-file shadow written and read through the RIIC byte callbacks.
 */
typedef struct {
  uint8_t  reg_ptr;                /**< Active auto-incrementing register pointer. */
  bool     ptr_set;                /**< First byte of this transfer set the ptr.   */
  uint8_t  file[k_pi4ioe_reg_max]; /**< Register-file shadow.                      */
  uint32_t writes;                 /**< Register writes the controller landed.     */
} pi4ioe_state_t;

/** @brief OV5640 SCCB sensor constants used by camera_capture. */
typedef enum : uint16_t {
  k_ov5640_addr_7b     = 0x3CU,   /**< SCCB 7-bit address.                     */
  k_ov5640_reg_id_hi   = 0x300AU, /**< Chip-ID high-byte register.             */
  k_ov5640_reg_id_lo   = 0x300BU, /**< Chip-ID low-byte register.              */
  k_ov5640_reg_format  = 0x4300U, /**< DVP output pixel-format register.       */
  k_ov5640_reg_isp_mux = 0x501FU, /**< ISP output format-mux register.         */
  k_ov5640_reg_test    = 0x503DU, /**< ISP test-pattern register.              */
  k_ov5640_id_hi       = 0x56U,   /**< Chip-ID high byte.                      */
  k_ov5640_id_lo       = 0x40U,   /**< Chip-ID low byte.                       */
  k_ov5640_ptr_bytes   = 2U,      /**< Big-endian register-pointer byte count. */
} ov5640_const_t;

/**
 * @brief One OV5640 camera sensor on the SCCB bus.
 * @details Captures a 16-bit big-endian register pointer, serves the fixed
 * identity bytes, and latches the three configuration registers verified by
 * the firmware after sensor setup.
 */
typedef struct {
  uint16_t reg_ptr;     /**< Active 16-bit register pointer.       */
  uint8_t  ptr_bytes;   /**< Pointer bytes captured this transfer. */
  uint8_t  reg_format;  /**< Latched 0x4300 DVP output format.     */
  uint8_t  reg_isp_mux; /**< Latched 0x501F ISP output mux.        */
  uint8_t  reg_test;    /**< Latched 0x503D test-pattern control.  */
  uint32_t writes;      /**< Config register writes accepted.      */
  uint32_t id_reads;    /**< Chip-ID bytes served for reporting.   */
} ov5640_state_t;

/** @brief Fixed registry and board-device states. */
static riic_device_t  s_riic_dev[k_riic_dev_max];
static pi4ioe_state_t s_pi4ioe;
static ov5640_state_t s_ov5640;

/**
 * @brief Register one device in the first free bus slot.
 * @details Copies the supplied callback entry into fixed registry storage and
 * leaves the registry unchanged when every slot is occupied.
 * @param[in] device Fully initialized device callback entry.
 * @pre @p device is marked present and carries callbacks valid for the run.
 * @pre The call executes on the emulator's single owning thread.
 * @post At most one previously free registry slot is occupied.
 * @post Device callback context ownership remains with its defining model.
 * @note Registry overflow deliberately drops the extra device.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_riic_device_register(riic_device_t device)
{
  for (uint32_t i = 0U; i < (uint32_t)k_riic_dev_max; i++) {
    if (!s_riic_dev[i].present) {
      s_riic_dev[i] = device;
      return;
    }
  }
}

/**
 * @brief Accept one controller byte for the PI4IOE5V6408.
 * @details Treats the first byte as the register pointer and each following
 * byte as register data, advancing the pointer after every payload byte.
 * @param[in,out] ctx Expander state registered with the bus callback.
 * @param[in] byte Register-pointer or payload byte from the controller.
 * @pre @p ctx identifies the module-owned ::pi4ioe_state_t instance.
 * @pre The callback runs within an active RIIC transfer.
 * @post The pointer latch or one register shadow byte is updated.
 * @post No storage outside the expander state is modified.
 * @note The register pointer wraps within the modelled 0x10-byte file.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_pi4ioe_write(void* ctx, uint8_t byte)
{
  pi4ioe_state_t* p = (pi4ioe_state_t*)ctx;
  if (!p->ptr_set) {
    p->reg_ptr = (uint8_t)(byte % (uint8_t)k_pi4ioe_reg_max);
    p->ptr_set = true;
    return;
  }
  p->file[p->reg_ptr] = byte;
  p->reg_ptr          = (uint8_t)((p->reg_ptr + 1U) % (uint8_t)k_pi4ioe_reg_max);
  p->writes++;
}

/**
 * @brief Serve PI4IOE5V6408 register bytes to the controller.
 * @details Copies at most the register-file capacity beginning at the active
 * auto-incrementing pointer, wrapping each addressed register independently.
 * @param[in,out] ctx Expander state registered with the bus callback.
 * @param[out] buf Destination receiving consecutive register bytes.
 * @param[in] max Capacity of @p buf in bytes.
 * @return Number of bytes written to @p buf.
 * @retval 0 @p max is zero; otherwise the smaller of @p max and the register
 * file capacity.
 * @pre @p ctx identifies the module-owned ::pi4ioe_state_t instance.
 * @pre @p buf provides at least @p max writable bytes when @p max is nonzero.
 * @post Exactly the returned number of destination bytes is initialized.
 * @post Expander register contents remain unchanged.
 * @note A later STOP clears only the pointer-capture latch.
 * @since 0.1.0
 */
RA8_INTERNAL static uint32_t internal_pi4ioe_read(void* ctx, uint8_t* buf, uint32_t max)
{
  pi4ioe_state_t* p = (pi4ioe_state_t*)ctx;
  if (max == 0U) {
    return 0U;
  }
  const uint32_t n = (max < (uint32_t)k_pi4ioe_reg_max) ? max : (uint32_t)k_pi4ioe_reg_max;
  for (uint32_t i = 0U; i < n; i++) {
    const uint8_t reg = (uint8_t)((p->reg_ptr + i) % (uint8_t)k_pi4ioe_reg_max);
    buf[i]            = p->file[reg];
  }
  return n;
}

/**
 * @brief End a PI4IOE5V6408 transfer.
 * @details Clears the pointer-capture latch so the first byte of the next
 * transfer selects a new register.
 * @param[in,out] ctx Expander state registered with the bus callback.
 * @pre @p ctx identifies the module-owned ::pi4ioe_state_t instance.
 * @pre The callback follows a completed or aborted RIIC transfer.
 * @post The next controller byte is interpreted as a register pointer.
 * @post Register contents and counters remain unchanged.
 * @note The active pointer value itself is retained until overwritten.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_pi4ioe_stop(void* ctx)
{
  ((pi4ioe_state_t*)ctx)->ptr_set = false;
}

/**
 * @brief Accept pointer and configuration bytes for the OV5640.
 * @details Captures the first two bytes as a big-endian register pointer and
 * latches payload writes only for the verifier-visible configuration subset.
 * @param[in,out] ctx Sensor state registered with the SCCB callback.
 * @param[in] byte Register-pointer or configuration byte from the controller.
 * @pre @p ctx identifies the module-owned ::ov5640_state_t instance.
 * @pre The callback runs within an active RIIC transfer.
 * @post Pointer capture or one supported configuration register is updated.
 * @post Unsupported register writes affect only the activity counter.
 * @note The model intentionally does not emulate analog camera behavior.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_ov5640_write(void* ctx, uint8_t byte)
{
  ov5640_state_t* sensor = (ov5640_state_t*)ctx;
  if (sensor->ptr_bytes < (uint8_t)k_ov5640_ptr_bytes) {
    if (sensor->ptr_bytes == 0U) {
      sensor->reg_ptr = 0U;
    }
    sensor->reg_ptr = (uint16_t)(((uint16_t)(sensor->reg_ptr << 8U)) | (uint16_t)byte);
    sensor->ptr_bytes++;
    return;
  }
  if (sensor->reg_ptr == (uint16_t)k_ov5640_reg_format) {
    sensor->reg_format = byte;
  } else if (sensor->reg_ptr == (uint16_t)k_ov5640_reg_isp_mux) {
    sensor->reg_isp_mux = byte;
  } else if (sensor->reg_ptr == (uint16_t)k_ov5640_reg_test) {
    sensor->reg_test = byte;
  }
  sensor->writes++;
}

/**
 * @brief Serve one verifier-visible OV5640 register byte.
 * @details Returns the fixed 0x5640 identity, a latched configuration byte,
 * or zero for registers outside the deliberately modelled subset.
 * @param[in,out] ctx Sensor state registered with the SCCB callback.
 * @param[out] buf Destination receiving the selected register byte.
 * @param[in] max Capacity of @p buf in bytes.
 * @return Number of bytes written to @p buf.
 * @retval 0 @p max is zero; otherwise one byte is returned.
 * @pre @p ctx identifies the module-owned ::ov5640_state_t instance.
 * @pre @p buf provides at least one writable byte when @p max is nonzero.
 * @post The destination contains the selected register value on success.
 * @post Identity-read accounting advances only for 0x300A or 0x300B.
 * @note Configuration reads reflect the last accepted write.
 * @since 0.1.0
 */
RA8_INTERNAL static uint32_t internal_ov5640_read(void* ctx, uint8_t* buf, uint32_t max)
{
  ov5640_state_t* sensor = (ov5640_state_t*)ctx;
  if (max == 0U) {
    return 0U;
  }
  uint8_t value = 0U;
  if (sensor->reg_ptr == (uint16_t)k_ov5640_reg_id_hi) {
    value = (uint8_t)k_ov5640_id_hi;
    sensor->id_reads++;
  } else if (sensor->reg_ptr == (uint16_t)k_ov5640_reg_id_lo) {
    value = (uint8_t)k_ov5640_id_lo;
    sensor->id_reads++;
  } else if (sensor->reg_ptr == (uint16_t)k_ov5640_reg_format) {
    value = sensor->reg_format;
  } else if (sensor->reg_ptr == (uint16_t)k_ov5640_reg_isp_mux) {
    value = sensor->reg_isp_mux;
  } else if (sensor->reg_ptr == (uint16_t)k_ov5640_reg_test) {
    value = sensor->reg_test;
  }
  buf[0] = value;
  return 1U;
}

/**
 * @brief End an OV5640 SCCB transfer.
 * @details Clears the two-byte pointer-capture count so the next transfer
 * begins with a fresh big-endian register address.
 * @param[in,out] ctx Sensor state registered with the SCCB callback.
 * @pre @p ctx identifies the module-owned ::ov5640_state_t instance.
 * @pre The callback follows a completed or aborted RIIC transfer.
 * @post The next two controller bytes form a new register pointer.
 * @post Latched configuration values and counters remain unchanged.
 * @note The pointer value remains available across a repeated START.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_ov5640_stop(void* ctx)
{
  ((ov5640_state_t*)ctx)->ptr_bytes = 0U;
}

RA8_PRIV riic_device_t* priv_riic_device_find(uint8_t addr_7b)
{
  for (uint32_t i = 0U; i < (uint32_t)k_riic_dev_max; i++) {
    if (s_riic_dev[i].present && (s_riic_dev[i].addr_7b == addr_7b)) {
      return &s_riic_dev[i];
    }
  }
  return nullptr;
}

RA8_PRIV void priv_riic_devices_reset(void)
{
  for (uint32_t i = 0U; i < (uint32_t)k_riic_dev_max; i++) {
    s_riic_dev[i] = (riic_device_t){};
  }
  s_pi4ioe                          = (pi4ioe_state_t){};
  s_pi4ioe.file[k_pi4ioe_reg_devid] = (uint8_t)k_pi4ioe_devid_val;
  internal_riic_device_register((riic_device_t){.present = true,
                                                .addr_7b = (uint8_t)k_pi4ioe_addr_7b,
                                                .write   = internal_pi4ioe_write,
                                                .read    = internal_pi4ioe_read,
                                                .stop    = internal_pi4ioe_stop,
                                                .ctx     = &s_pi4ioe});

  s_ov5640 = (ov5640_state_t){};
  internal_riic_device_register((riic_device_t){.present = true,
                                                .addr_7b = (uint8_t)k_ov5640_addr_7b,
                                                .write   = internal_ov5640_write,
                                                .read    = internal_ov5640_read,
                                                .stop    = internal_ov5640_stop,
                                                .ctx     = &s_ov5640});
}

RA8_PRIV void priv_riic_devices_report(void)
{
  if (s_pi4ioe.writes > 0U) {
    (void)priv_emu_io_errf("  RIIC PI4IOE   : U15 expander 0x%02X acked %u register write(s)\n",
                           (unsigned)k_pi4ioe_addr_7b,
                           s_pi4ioe.writes);
  }
  if ((s_ov5640.id_reads > 0U) || (s_ov5640.writes > 0U)) {
    (void)priv_emu_io_errf(
      "  RIIC OV5640   : SCCB 0x%02X chip-id 0x5640 (%u id read(s), %u cfg write(s))\n",
      (unsigned)k_ov5640_addr_7b,
      s_ov5640.id_reads,
      s_ov5640.writes);
  }
}
