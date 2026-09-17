/**
 * @file board_periph_i2c_devices.c
 * @brief LSM6DSO IMU + MAX17048 fuel-gauge device models on the I2C bus
 *
 * @details
 * The two flat-register-file bus devices (the ST LSM6DSO 6-DoF IMU at 0x6B
 * and the MAX17048-class fuel gauge at 0x36, backed by the CLI-settable
 * battery state) -- moved verbatim out of board_periph_i2c.c. They register
 * themselves on the modelled bus through the module-internal registry.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <string.h>

#include "board_periph.h"
#include "board_periph_i2c_internal.h"

/**
 * @brief ST LSM6DSO 6-DoF IMU constants (libs/ra8_lsm6dso, DS12140 Rev 4).
 *
 * @details
 * The MikroE 6DOF IMU 12 Click ties SA0 high, so the part answers at 7-bit
 * 0x6B. Unlike the GT911, the LSM6DSO uses an 8-bit register pointer that
 * auto-increments across a burst read, so the model is a flat register file:
 * the driver writes the start register, then reads N consecutive bytes
 * (WHO_AM_I, then 6-byte gyro / accel bursts). WHO_AM_I (0x0F) must read back
 * 0x6C or ra8_lsm6dso_init() rejects the part.
 */
typedef enum : uint32_t {
  k_lsm6dso_addr_7b      = 0x6BU,   /**< SA0-high 7-bit I2C address.            */
  k_lsm6dso_reg_who_am_i = 0x0FU,   /**< WHO_AM_I register.                     */
  k_lsm6dso_who_am_i_val = 0x6CU,   /**< Expected WHO_AM_I value.               */
  k_lsm6dso_reg_outx_l_g = 0x22U,   /**< First gyro output byte (OUTX_L_G).     */
  k_lsm6dso_reg_outz_l_a = 0x2CU,   /**< Accel Z low byte (OUTZ_L_A).           */
  k_lsm6dso_reg_file     = 0x100U,  /**< Flat register-file size (bytes).       */
  k_lsm6dso_seed_accel_z = 0x4000U, /**< Synthetic accel Z (~+1 g at +-2 g FS). */
  k_lsm6dso_seed_gyro_x  = 0x0100U, /**< Synthetic gyro X (small steady rate).  */
} lsm6dso_const_t;

/**
 * @brief Maxim MAX17048 fuel-gauge constants (battery state-of-charge over I2C).
 *
 * @details
 * The carrier's MAX17048-class fuel gauge answers at 7-bit 0x36 with a flat
 * register file of big-endian 16-bit values. The firmware writes a register
 * pointer then reads two bytes: VCELL (cell voltage, 78.125 uV/LSB), SOC
 * (state-of-charge, high byte = integer percent), VERSION, and CRATE (charge
 * rate, signed -- positive while charging). The emulator drives SOC + the
 * CRATE sign from the user-settable battery state (``--battery`` / ``--charge``).
 */
typedef enum : uint32_t {
  k_max17048_addr_7b      = 0x36U,   /**< MAX17048 7-bit I2C address.             */
  k_max17048_reg_vcell    = 0x02U,   /**< VCELL: cell voltage (78.125 uV/LSB).    */
  k_max17048_reg_soc      = 0x04U,   /**< SOC: state-of-charge (1/256 %/LSB).     */
  k_max17048_reg_version  = 0x08U,   /**< VERSION register.                       */
  k_max17048_reg_crate    = 0x16U,   /**< CRATE: charge rate (signed, 0.208%/hr). */
  k_max17048_reg_file     = 0x100U,  /**< Flat register-file size (bytes).        */
  k_battery_soc_default   = 72U,     /**< Default state-of-charge percent.        */
  k_battery_soc_max       = 100U,    /**< SOC clamp ceiling.                      */
  k_battery_vcell_default = 0xBE00U, /**< ~3.80 V at 78.125 uV/LSB.               */
  k_battery_version_val   = 0x0012U, /**< Reported VERSION.                       */
  k_battery_crate_mag     = 0x0018U, /**< |CRATE| ~5 %/hr (sign = charging).      */
  k_battery_byte_shift    = 8U,      /**< High-byte shift for a 16-bit register.  */
  k_battery_byte_mask     = 0xFFU,   /**< Low-byte mask.                          */
} max17048_const_t;

/**
 * @brief One ST LSM6DSO IMU device: a flat auto-incrementing register file.
 *
 * @details
 * @c reg_ptr is set by the first byte of each write phase (the start register)
 * and advances one per byte for both writes (config) and reads (burst). A read
 * served past the file end returns 0. @c ptr_set is cleared at STOP so the next
 * transaction's first write byte is taken as a fresh register pointer.
 */
typedef struct {
  uint8_t  regs[k_lsm6dso_reg_file]; /**< Flat register file.                */
  uint16_t reg_ptr;                  /**< Active register pointer.           */
  bool     ptr_set;                  /**< Start register captured this xfer. */
  uint32_t reads;                    /**< Register reads answered (report).  */
} lsm6dso_state_t;

/**
 * @brief User-settable battery state surfaced by the MAX17048 fuel gauge.
 *
 * @details Set from the CLI (``--battery <pct>`` / ``--charge``) before the run
 * and read back by the status overlay; the fuel-gauge register file is laid
 * from this whenever it changes.
 */
typedef struct {
  uint8_t soc_pct;  /**< State-of-charge percent (0..100).  */
  bool    charging; /**< Charger attached (CRATE positive). */
} battery_state_t;

/**
 * @brief One MAX17048 fuel gauge: a flat big-endian register file + read pointer.
 *
 * @details Same access shape as the LSM6DSO model -- the first write byte sets
 * @c reg_ptr, subsequent bytes/reads auto-increment. ::internal_fuelgauge_seed lays the
 * VCELL / SOC / VERSION / CRATE words from ::s_battery.
 */
typedef struct {
  uint8_t  regs[k_max17048_reg_file]; /**< Flat register file (big-endian).   */
  uint16_t reg_ptr;                   /**< Active register pointer.           */
  bool     ptr_set;                   /**< Start register captured this xfer. */
  uint32_t reads;                     /**< Register reads answered (report).  */
} max17048_state_t;

static lsm6dso_state_t s_lsm6dso;

/** @brief Battery state + the fuel-gauge device that exposes it over I2C. */
static battery_state_t  s_battery = {.soc_pct = (uint8_t)k_battery_soc_default, .charging = false};
static max17048_state_t s_fuelgauge;

/**
 * @brief Perform lsm6dso seed16 for the board periph I2C devices model.
 * @details Perform lsm6dso seed16 for the board periph i2c devices model; this step is contained within the board periph I2C devices model and uses bounded caller or module-owned storage.
 * @param[in,out] s Module state instance processed by the operation.
 * @param[in] reg Register index or value selected by the operation.
 * @param[in] val Register or payload value processed by the operation.
 * @pre Arguments satisfy the ranges documented for lsm6dso seed16. @pre The call executes on the emulator's single owning thread.
 * @post State changes remain confined to the board periph I2C devices model and documented output objects. @post Ownership of caller-supplied storage is unchanged.
 * @note The operation is synchronous and does not transfer heap ownership.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_lsm6dso_seed16(lsm6dso_state_t* s, uint8_t reg, uint16_t val)
{
  s->regs[reg]                 = (uint8_t)((uint32_t)val & (uint32_t)k_i3c_dev_byte_mask);
  s->regs[(uint8_t)(reg + 1U)] = (uint8_t)((uint32_t)val >> 8U);
}

/**
 * @brief Reset the register file: WHO_AM_I + synthetic accel/gyro samples.
 * @details Reset the register file: who_am_i + synthetic accel/gyro samples; this step is contained within the board periph I2C devices model and uses bounded caller or module-owned storage.
 * @param[in,out] s Module state instance processed by the operation.
 * @pre Arguments satisfy the ranges documented for lsm6dso reset regs. @pre The call executes on the emulator's single owning thread.
 * @post State changes remain confined to the board periph I2C devices model and documented output objects. @post Ownership of caller-supplied storage is unchanged.
 * @note The operation is synchronous and does not transfer heap ownership.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_lsm6dso_reset_regs(lsm6dso_state_t* s)
{
  for (uint32_t i = 0U; i < (uint32_t)k_lsm6dso_reg_file; i++) {
    s->regs[i] = 0U;
  }
  s->regs[(uint8_t)k_lsm6dso_reg_who_am_i] = (uint8_t)k_lsm6dso_who_am_i_val;
  internal_lsm6dso_seed16(s, (uint8_t)k_lsm6dso_reg_outz_l_a, (uint16_t)k_lsm6dso_seed_accel_z);
  internal_lsm6dso_seed16(s, (uint8_t)k_lsm6dso_reg_outx_l_g, (uint16_t)k_lsm6dso_seed_gyro_x);
  s->reg_ptr = 0U;
  s->ptr_set = false;
  s->reads   = 0U;
}

/**
 * @brief Controller -> LSM6DSO: start register first, then config payload.
 * @details Controller -> lsm6dso: start register first, then config payload; this step is contained within the board periph I2C devices model and uses bounded caller or module-owned storage.
 * @param[in,out] ctx Opaque callback context identifying module-owned device state.
 * @param[in] byte One data byte received from or sent to the emulated interface.
 * @pre Arguments satisfy the ranges documented for lsm6dso write. @pre The call executes on the emulator's single owning thread.
 * @post State changes remain confined to the board periph I2C devices model and documented output objects. @post Ownership of caller-supplied storage is unchanged.
 * @note The operation is synchronous and does not transfer heap ownership.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_lsm6dso_write(void* ctx, uint8_t byte)
{
  lsm6dso_state_t* s = (lsm6dso_state_t*)ctx;
  if (!s->ptr_set) {
    s->reg_ptr = byte;
    s->ptr_set = true;
    return;
  }
  if (s->reg_ptr < (uint16_t)k_lsm6dso_reg_file) {
    s->regs[s->reg_ptr] = byte; /* CTRL config writes land harmlessly. */
  }
  s->reg_ptr = (uint16_t)(s->reg_ptr + 1U);
}

/**
 * @brief LSM6DSO -> controller: burst from the pointer, auto-incrementing.
 * @details Lsm6dso -> controller: burst from the pointer, auto-incrementing; this step is contained within the board periph I2C devices model and uses bounded caller or module-owned storage.
 * @param[in,out] ctx Opaque callback context identifying module-owned device state.
 * @param[in,out] buf Bounded byte buffer read or updated by the operation.
 * @param[in] max Capacity of the destination or operation in elements.
 * @return The lsm6dso read result produced by the board periph I2C devices model.
 * @retval value The operation-specific lsm6dso read value.
 * @pre Arguments satisfy the ranges documented for lsm6dso read. @pre The call executes on the emulator's single owning thread.
 * @post State changes remain confined to the board periph I2C devices model and documented output objects. @post Ownership of caller-supplied storage is unchanged.
 * @note The operation is synchronous and does not transfer heap ownership.
 * @since 0.1.0
 */
RA8_INTERNAL static uint32_t internal_lsm6dso_read(void* ctx, uint8_t* buf, uint32_t max)
{
  lsm6dso_state_t* s = (lsm6dso_state_t*)ctx;
  for (uint32_t i = 0U; i < max; i++) {
    buf[i]     = (s->reg_ptr < (uint16_t)k_lsm6dso_reg_file) ? s->regs[s->reg_ptr] : 0U;
    s->reg_ptr = (uint16_t)(s->reg_ptr + 1U);
  }
  s->reads++;
  return max;
}

/**
 * @brief STOP / transfer end: re-arm pointer capture for the next transfer.
 * @details Stop / transfer end: re-arm pointer capture for the next transfer; this step is contained within the board periph I2C devices model and uses bounded caller or module-owned storage.
 * @param[in,out] ctx Opaque callback context identifying module-owned device state.
 * @pre Arguments satisfy the ranges documented for lsm6dso stop. @pre The call executes on the emulator's single owning thread.
 * @post State changes remain confined to the board periph I2C devices model and documented output objects. @post Ownership of caller-supplied storage is unchanged.
 * @note The operation is synchronous and does not transfer heap ownership.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_lsm6dso_stop(void* ctx)
{
  lsm6dso_state_t* s = (lsm6dso_state_t*)ctx;
  s->ptr_set         = false;
}

/**
 * @brief Write a big-endian 16-bit value at register @p reg of @p s.
 * @details Write a big-endian 16-bit value at register @p reg of @p s; this step is contained within the board periph I2C devices model and uses bounded caller or module-owned storage.
 * @param[in,out] s Module state instance processed by the operation.
 * @param[in] reg Register index or value selected by the operation.
 * @param[in] val Register or payload value processed by the operation.
 * @pre Arguments satisfy the ranges documented for fuelgauge put16. @pre The call executes on the emulator's single owning thread.
 * @post State changes remain confined to the board periph I2C devices model and documented output objects. @post Ownership of caller-supplied storage is unchanged.
 * @note The operation is synchronous and does not transfer heap ownership.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_fuelgauge_put16(max17048_state_t* s, uint8_t reg, uint16_t val)
{
  s->regs[reg]                 = (uint8_t)(val >> (uint16_t)k_battery_byte_shift);
  s->regs[(uint8_t)(reg + 1U)] = (uint8_t)(val & (uint16_t)k_battery_byte_mask);
}

/**
 * @brief Lay the MAX17048 register file from the current ::s_battery state.
 * @details Lay the max17048 register file from the current ::s_battery state; this step is contained within the board periph I2C devices model and uses bounded caller or module-owned storage.
 * @param[in,out] s Module state instance processed by the operation.
 * @pre Arguments satisfy the ranges documented for fuelgauge seed. @pre The call executes on the emulator's single owning thread.
 * @post State changes remain confined to the board periph I2C devices model and documented output objects. @post Ownership of caller-supplied storage is unchanged.
 * @note The operation is synchronous and does not transfer heap ownership.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_fuelgauge_seed(max17048_state_t* s)
{
  for (uint32_t i = 0U; i < (uint32_t)k_max17048_reg_file; i++) {
    s->regs[i] = 0U;
  }
  internal_fuelgauge_put16(s, (uint8_t)k_max17048_reg_vcell, (uint16_t)k_battery_vcell_default);
  /* SOC: high byte is the integer percent, low byte the 1/256 fraction (0). */
  internal_fuelgauge_put16(
    s,
    (uint8_t)k_max17048_reg_soc,
    (uint16_t)((uint16_t)s_battery.soc_pct << (uint16_t)k_battery_byte_shift));
  internal_fuelgauge_put16(s, (uint8_t)k_max17048_reg_version, (uint16_t)k_battery_version_val);
  /* CRATE is signed: positive while charging, negative while discharging. */
  const int32_t crate =
    s_battery.charging ? (int32_t)k_battery_crate_mag : -(int32_t)k_battery_crate_mag;
  internal_fuelgauge_put16(s, (uint8_t)k_max17048_reg_crate, (uint16_t)(int16_t)crate);
}

/**
 * @brief MAX17048 write: first byte is the register pointer, then it advances.
 * @details Max17048 write: first byte is the register pointer, then it advances; this step is contained within the board periph I2C devices model and uses bounded caller or module-owned storage.
 * @param[in,out] ctx Opaque callback context identifying module-owned device state.
 * @param[in] byte One data byte received from or sent to the emulated interface.
 * @pre Arguments satisfy the ranges documented for fuelgauge write. @pre The call executes on the emulator's single owning thread.
 * @post State changes remain confined to the board periph I2C devices model and documented output objects. @post Ownership of caller-supplied storage is unchanged.
 * @note The operation is synchronous and does not transfer heap ownership.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_fuelgauge_write(void* ctx, uint8_t byte)
{
  max17048_state_t* s = (max17048_state_t*)ctx;
  if (!s->ptr_set) {
    s->reg_ptr = byte;
    s->ptr_set = true;
    return;
  }
  if (s->reg_ptr < (uint16_t)k_max17048_reg_file) {
    s->regs[s->reg_ptr] = byte;
  }
  s->reg_ptr = (uint16_t)(s->reg_ptr + 1U);
}

/**
 * @brief MAX17048 read: serve consecutive register bytes (auto-increment).
 * @details Max17048 read: serve consecutive register bytes (auto-increment); this step is contained within the board periph I2C devices model and uses bounded caller or module-owned storage.
 * @param[in,out] ctx Opaque callback context identifying module-owned device state.
 * @param[in,out] buf Bounded byte buffer read or updated by the operation.
 * @param[in] max Capacity of the destination or operation in elements.
 * @return The fuelgauge read result produced by the board periph I2C devices model.
 * @retval value The operation-specific fuelgauge read value.
 * @pre Arguments satisfy the ranges documented for fuelgauge read. @pre The call executes on the emulator's single owning thread.
 * @post State changes remain confined to the board periph I2C devices model and documented output objects. @post Ownership of caller-supplied storage is unchanged.
 * @note The operation is synchronous and does not transfer heap ownership.
 * @since 0.1.0
 */
RA8_INTERNAL static uint32_t internal_fuelgauge_read(void* ctx, uint8_t* buf, uint32_t max)
{
  max17048_state_t* s = (max17048_state_t*)ctx;
  for (uint32_t i = 0U; i < max; i++) {
    buf[i]     = (s->reg_ptr < (uint16_t)k_max17048_reg_file) ? s->regs[s->reg_ptr] : 0U;
    s->reg_ptr = (uint16_t)(s->reg_ptr + 1U);
  }
  s->reads++;
  return max;
}

/**
 * @brief MAX17048 STOP: re-arm the register-pointer capture.
 * @details Max17048 stop: re-arm the register-pointer capture; this step is contained within the board periph I2C devices model and uses bounded caller or module-owned storage.
 * @param[in,out] ctx Opaque callback context identifying module-owned device state.
 * @pre Arguments satisfy the ranges documented for fuelgauge stop. @pre The call executes on the emulator's single owning thread.
 * @post State changes remain confined to the board periph I2C devices model and documented output objects. @post Ownership of caller-supplied storage is unchanged.
 * @note The operation is synchronous and does not transfer heap ownership.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_fuelgauge_stop(void* ctx)
{
  max17048_state_t* s = (max17048_state_t*)ctx;
  s->ptr_set          = false;
}

void board_periph_battery_set(uint8_t soc_pct, bool charging)
{
  s_battery.soc_pct = (soc_pct > (uint8_t)k_battery_soc_max) ? (uint8_t)k_battery_soc_max : soc_pct;
  s_battery.charging = charging;
  internal_fuelgauge_seed(&s_fuelgauge);
}

void board_periph_battery_get(uint8_t* out_soc, bool* out_charging)
{
  if (out_soc != nullptr) {
    *out_soc = s_battery.soc_pct;
  }
  if (out_charging != nullptr) {
    *out_charging = s_battery.charging;
  }
}

/** @brief Implementation of `priv_board_i2c_imu_fuel_reset()` -- re-lay register files. */
void priv_board_i2c_imu_fuel_reset(void)
{
  internal_lsm6dso_reset_regs(&s_lsm6dso);
  s_fuelgauge = (max17048_state_t){};
  internal_fuelgauge_seed(&s_fuelgauge); /* lay the register file from s_battery (CLI-set) */
}

/** @brief Implementation of `priv_board_i2c_imu_fuel_register()` -- bus attach. */
void priv_board_i2c_imu_fuel_register(void)
{
  priv_i2c_device_register((uint8_t)k_lsm6dso_addr_7b,
                           internal_lsm6dso_write,
                           internal_lsm6dso_read,
                           internal_lsm6dso_stop,
                           &s_lsm6dso);
  /* MAX17048-class fuel gauge at 0x36: exposes the battery state-of-charge +
   * charge direction so a battery monitor reads a real % over I2C. */
  priv_i2c_device_register((uint8_t)k_max17048_addr_7b,
                           internal_fuelgauge_write,
                           internal_fuelgauge_read,
                           internal_fuelgauge_stop,
                           &s_fuelgauge);
}

/** @brief Implementation of `priv_board_i2c_imu_reads()` -- report telemetry. */
uint32_t priv_board_i2c_imu_reads(void)
{
  return s_lsm6dso.reads;
}
