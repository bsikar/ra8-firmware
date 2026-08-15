/**
 * @file test_ra8_ov5640.c
 * @brief Host unit tests for the transport-independent OV5640 driver.
 *
 * @details Exercises initialization, raw SCCB forwarding, dual-address chip
 * probing, VGA UYVY/JPEG programming, raw JPEG quantization scale handling,
 * and transport/readback failures against an in-memory 16-bit register file.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>
#include <string.h>

#include "ra8_attributes.h"
#include "ra8_err.h"
#include "ra8_ov5640.h"
#include "unity_minimal.h"

/** @brief OV5640 registers observed by these black-box tests. */
typedef enum : uint16_t {
  k_test_reg_system_reset00     = 0x3000U, /**< MCU reset-control register.           */
  k_test_reg_system_reset02     = 0x3002U, /**< JPEG reset-control register.          */
  k_test_reg_clock_enable02     = 0x3006U, /**< JPEG clock-control register.          */
  k_test_reg_sw_reset           = 0x3008U, /**< Software reset and standby register.  */
  k_test_reg_chip_id_hi         = 0x300AU, /**< Chip identifier high byte.            */
  k_test_reg_chip_id_lo         = 0x300BU, /**< Chip identifier low byte.             */
  k_test_reg_pll_bit_mode       = 0x3034U, /**< PLL bit-mode register.                */
  k_test_reg_pll_sys_div        = 0x3035U, /**< PLL system-divider register.          */
  k_test_reg_pll_multiplier     = 0x3036U, /**< PLL multiplier register.              */
  k_test_reg_pll_pre_div        = 0x3037U, /**< PLL pre-divider register.             */
  k_test_reg_pll_bypass         = 0x3039U, /**< PLL bypass register.                  */
  k_test_reg_clock_select       = 0x3103U, /**< System-clock source register.         */
  k_test_reg_clock_root         = 0x3108U, /**< Clock-root divider register.          */
  k_test_reg_analog_ctrl0c      = 0x370CU, /**< VGA analog timing control.            */
  k_test_reg_timing_y_start_hi  = 0x3802U, /**< Sensor crop Y start high byte.        */
  k_test_reg_timing_y_start_lo  = 0x3803U, /**< Sensor crop Y start low byte.         */
  k_test_reg_timing_y_end_hi    = 0x3806U, /**< Sensor crop Y end high byte.          */
  k_test_reg_timing_y_end_lo    = 0x3807U, /**< Sensor crop Y end low byte.           */
  k_test_reg_timing_hts_hi      = 0x380CU, /**< Horizontal total high byte.           */
  k_test_reg_timing_hts_lo      = 0x380DU, /**< Horizontal total low byte.            */
  k_test_reg_timing_vts_hi      = 0x380EU, /**< Vertical total high byte.             */
  k_test_reg_timing_vts_lo      = 0x380FU, /**< Vertical total low byte.              */
  k_test_reg_timing_y_offset_hi = 0x3812U, /**< Output Y offset high byte.            */
  k_test_reg_timing_y_offset_lo = 0x3813U, /**< Output Y offset low byte.             */
  k_test_reg_timing_tc_reg20    = 0x3820U, /**< Vertical timing and binning register. */
  k_test_reg_timing_tc_reg21    = 0x3821U, /**< Timing and JPEG-enable register.      */
  k_test_reg_pclk_divider       = 0x3824U, /**< Pixel-clock divider register.         */
  k_test_reg_aec_max_hi         = 0x3A02U, /**< AEC maximum exposure high byte.       */
  k_test_reg_aec_max_lo         = 0x3A03U, /**< AEC maximum exposure low byte.        */
  k_test_reg_aec_b50_hi         = 0x3A08U, /**< 50 Hz banding step high byte.         */
  k_test_reg_aec_b50_lo         = 0x3A09U, /**< 50 Hz banding step low byte.          */
  k_test_reg_aec_b60_hi         = 0x3A0AU, /**< 60 Hz banding step high byte.         */
  k_test_reg_aec_b60_lo         = 0x3A0BU, /**< 60 Hz banding step low byte.          */
  k_test_reg_aec_b50_max        = 0x3A0DU, /**< 50 Hz maximum band count.             */
  k_test_reg_aec_b60_max        = 0x3A0EU, /**< 60 Hz maximum band count.             */
  k_test_reg_aec_vts_hi         = 0x3A14U, /**< AEC VTS high byte.                    */
  k_test_reg_aec_vts_lo         = 0x3A15U, /**< AEC VTS low byte.                     */
  k_test_reg_format             = 0x4300U, /**< Output-format register.               */
  k_test_reg_jpeg_ctrl00        = 0x4400U, /**< JPEG input-format control.            */
  k_test_reg_jpeg_ctrl01        = 0x4401U, /**< JPEG pacing control.                  */
  k_test_reg_jpeg_ctrl04        = 0x4404U, /**< JPEG header-output control.           */
  k_test_reg_jpeg_quality       = 0x4407U, /**< JPEG quantization-scale register.     */
  k_test_reg_jpeg_length_hi     = 0x4414U, /**< JPEG length high byte.                */
  k_test_reg_jpeg_length_mid    = 0x4415U, /**< JPEG length middle byte.              */
  k_test_reg_jpeg_length_lo     = 0x4416U, /**< JPEG length low byte.                 */
  k_test_reg_jfifo_overflow     = 0x4417U, /**< JPEG FIFO overflow status.            */
  k_test_reg_jpeg_timing14      = 0x4514U, /**< JPEG binning/orientation timing.      */
  k_test_reg_jpeg_timing20      = 0x4520U, /**< JPEG sampling timing.                 */
  k_test_reg_vfifo_ctrl00       = 0x4600U, /**< VFIFO output control.                 */
  k_test_reg_compression_w_hi   = 0x4602U, /**< Compression width high byte.          */
  k_test_reg_compression_w_lo   = 0x4603U, /**< Compression width low byte.           */
  k_test_reg_compression_h_hi   = 0x4604U, /**< Compression height high byte.         */
  k_test_reg_compression_h_lo   = 0x4605U, /**< Compression height low byte.          */
  k_test_reg_vfifo_ctrl0b       = 0x460BU, /**< VFIFO control register 0B.            */
  k_test_reg_vfifo_ctrl0c       = 0x460CU, /**< VFIFO control register 0C.            */
  k_test_reg_jpeg_mode          = 0x4713U, /**< DVP JPEG-mode register.               */
  k_test_reg_jpeg_ctrl1c        = 0x471CU, /**< JPEG DVP-control register.            */
  k_test_reg_href_minimum       = 0x471FU, /**< JPEG HREF minimum blanking.           */
  k_test_reg_polarity_ctrl00    = 0x4740U, /**< DVP sync-polarity control.            */
  k_test_reg_isp_ctrl01         = 0x5001U, /**< ISP feature-enable register.          */
  k_test_reg_isp_mux            = 0x501FU, /**< ISP format-multiplexer register.      */
  k_test_reg_test_pattern       = 0x503DU, /**< Test-pattern register.                */
} ov5640_test_register_t;

/** @brief Fixture constants and expected official JPEG settings. */
typedef enum : uint32_t {
  k_test_register_count        = 65536U,     /**< Complete 16-bit register space.     */
  k_test_write_log_capacity    = 384U,       /**< Maximum captured SCCB writes.       */
  k_test_delay_log_capacity    = 5U,         /**< Maximum captured delays.            */
  k_test_min_scene_write_count = 250U,       /**< Minimum validated VGA table size.   */
  k_test_no_failure            = UINT32_MAX, /**< Sentinel disabling write failure.   */
  k_test_reset_guard_ms        = 100U,       /**< Expected reset guard time.          */
  k_test_mcu_reset_delay_ms    = 10U,        /**< Expected MCU-reset hold time.       */
  k_test_config_delay_ms       = 500U,       /**< Expected configuration settle time. */
  k_test_jpeg_tail_write_count = 7U,         /**< Ordered JPEG transition tail rows.  */
  k_test_jpeg_length           = 0x012345U,  /**< Synthetic 24-bit JPEG length.       */
  k_test_compression_width     = 640U,       /**< Synthetic compression width.        */
  k_test_compression_height    = 480U,       /**< Synthetic compression height.       */
} ov5640_test_capacity_t;

/** @brief Register values and fixture bytes used by the OV5640 tests. */
typedef enum : uint8_t {
  k_test_chip_id_hi          = 0x56U, /**< Expected chip identifier high byte.    */
  k_test_chip_id_lo          = 0x40U, /**< Expected chip identifier low byte.     */
  k_test_sw_reset_hold       = 0x82U, /**< Reset-held software-control value.     */
  k_test_sw_standby          = 0x42U, /**< Standby software-control value.        */
  k_test_sw_reset_wake       = 0x02U, /**< Running software-control value.        */
  k_test_mcu_reset_hold      = 0x20U, /**< Internal-MCU reset-held value.         */
  k_test_format_yuyv         = 0x30U, /**< Expected packed YUV format value.      */
  k_test_jpeg_mode_2         = 0x02U, /**< Compressed-frame DVP mode.             */
  k_test_jpeg_ctrl00         = 0x81U, /**< Expected JPEG input/FIFO control.      */
  k_test_jpeg_ctrl01         = 0x01U, /**< Expected JPEG FIFO pacing control.     */
  k_test_jpeg_ctrl04         = 0x24U, /**< Expected JPEG header/output control.   */
  k_test_jpeg_timing14       = 0xAAU, /**< Expected VGA JPEG binning timing.      */
  k_test_jpeg_timing20       = 0x0BU, /**< Expected VGA JPEG sampling timing.     */
  k_test_timing_tc_reg20     = 0x41U, /**< Qualified VGA vertical timing.         */
  k_test_jpeg_y_start_hi     = 0x00U, /**< Expected crop Y start high byte.       */
  k_test_jpeg_y_start_lo     = 0x00U, /**< Expected crop Y start low byte.        */
  k_test_jpeg_y_end_hi       = 0x07U, /**< Expected crop Y end high byte.         */
  k_test_jpeg_y_end_lo       = 0x9FU, /**< Expected crop Y end low byte.          */
  k_test_jpeg_hts_hi         = 0x08U, /**< Expected horizontal total high byte.   */
  k_test_jpeg_hts_lo         = 0x0CU, /**< Expected horizontal total low byte.    */
  k_test_jpeg_vts_hi         = 0x03U, /**< Expected vertical total high byte.     */
  k_test_jpeg_vts_lo         = 0xD8U, /**< Expected vertical total low byte.      */
  k_test_jpeg_y_offset_hi    = 0x00U, /**< Expected output Y offset high byte.    */
  k_test_jpeg_y_offset_lo    = 0x08U, /**< Expected output Y offset low byte.     */
  k_test_analog_ctrl0c       = 0x02U, /**< Expected VGA analog timing value.      */
  k_test_aec_vga_hi          = 0x03U, /**< VGA frame/AEC total high byte.         */
  k_test_aec_vga_lo          = 0xD8U, /**< VGA frame/AEC total low byte.          */
  k_test_aec_b50_hi          = 0x01U, /**< VGA 50 Hz banding high byte.           */
  k_test_aec_b50_lo          = 0x27U, /**< VGA 50 Hz banding low byte.            */
  k_test_aec_b60_hi          = 0x00U, /**< VGA 60 Hz banding high byte.           */
  k_test_aec_b60_lo          = 0xF6U, /**< VGA 60 Hz banding low byte.            */
  k_test_aec_b50_max         = 0x04U, /**< VGA 50 Hz maximum band count.          */
  k_test_aec_b60_max         = 0x03U, /**< VGA 60 Hz maximum band count.          */
  k_test_isp_scale_mask      = 0x20U, /**< ISP scaling-enable mask.               */
  k_test_jpeg_enable_mask    = 0x20U, /**< JPEG timing-enable bit.                */
  k_test_jpeg_reset_mask     = 0x1CU, /**< JPEG reset-control bits.               */
  k_test_jpeg_clocks_enabled = 0xEBU, /**< Base clocks plus JPEG clock bits.      */
  k_test_jpeg_ctrl1c         = 0x50U, /**< Expected JPEG DVP control.             */
  k_test_dvp_polarity_raw    = 0x20U, /**< Board-qualified raw DVP polarity.      */
  k_test_dvp_jpeg_polarity   = 0x21U, /**< JPEG-mode VSYNC/HREF polarity.         */
  k_test_pll_bit_mode        = 0x18U, /**< Qualified DVP PLL bit mode.            */
  k_test_pll_sys_div         = 0x21U, /**< Qualified DVP system divider.          */
  k_test_pll_multiplier      = 0x46U, /**< Qualified DVP PLL multiplier.          */
  k_test_pll_pre_div         = 0x13U, /**< Qualified DVP PLL pre-divider.         */
  k_test_pll_bypass          = 0x00U, /**< Expected PLL bypass state.             */
  k_test_clock_root          = 0x01U, /**< Qualified DVP clock roots.             */
  k_test_clock_select        = 0x02U, /**< Qualified DVP PLL selection.           */
  k_test_jpeg_pclk_divider   = 0x01U, /**< Qualified DVP pixel-clock divider.     */
  k_test_jpeg_vfifo_ctrl0b   = 0x35U, /**< Expected VFIFO control 0B value.       */
  k_test_jpeg_vfifo_ctrl0c   = 0x22U, /**< Expected VFIFO control 0C value.       */
  k_test_jpeg_width_hi       = 0x02U, /**< Expected VGA JPEG width high byte.     */
  k_test_jpeg_width_lo       = 0x80U, /**< Expected VGA JPEG width low byte.      */
  k_test_jpeg_height_hi      = 0x01U, /**< Expected VGA JPEG height high byte.    */
  k_test_jpeg_height_lo      = 0xE0U, /**< Expected VGA JPEG height low byte.     */
  k_test_upper_bits_fixture  = 0xC0U, /**< Preserved upper-register fixture bits. */
  k_test_quant_scale_fixture = 0x15U, /**< Valid quantization-scale fixture.      */
  k_test_quant_scale_invalid = 0x40U, /**< First invalid quantization scale.      */
  k_test_wrong_id            = 0xAAU, /**< Non-OV5640 identifier fixture.         */
  k_test_read_poison         = 0xA5U, /**< Corrupted readback fixture.            */
  k_test_stream_settle_ms    = 5U,    /**< Expected stream-start delay.           */
} ov5640_test_value_t;

/** @brief One captured SCCB write. */
typedef struct {
  uint8_t  address; /**< SCCB device address.     */
  uint16_t reg;     /**< Sensor register address. */
  uint8_t  value;   /**< Register value written.  */
} ov5640_mock_write_t;

/** @brief Complete mock transport state. */
typedef struct {
  uint8_t             regs[k_test_register_count];       /**< Simulated sensor register file.   */
  ov5640_mock_write_t writes[k_test_write_log_capacity]; /**< Ordered SCCB write log.           */
  uint32_t            delays[k_test_delay_log_capacity]; /**< Requested delay log.              */
  uint32_t            read_count;                        /**< Number of SCCB reads.             */
  uint32_t            write_count;                       /**< Number of SCCB writes.            */
  uint32_t            delay_count;                       /**< Number of requested delays.       */
  uint32_t            fail_write_at;                     /**< Write index forced to fail.       */
  ra8_err_t           forced_read_error;                 /**< Error returned by each read.      */
  bool                primary_present;                   /**< Primary address responds.         */
  bool                secondary_present;                 /**< Secondary address responds.       */
  bool                corrupt_read;                      /**< Selected readback is replaced.    */
  uint16_t            corrupt_reg;                       /**< Register selected for corruption. */
  uint8_t             corrupt_value;                     /**< Replacement corrupt value.        */
} ov5640_mock_t;

static ov5640_mock_t s_mock;

/**
 * @brief Reset the transport and plant a primary-address OV5640 ID.
 * @details Clears logs and faults before initializing the expected chip identifier.
 * @pre The global mock fixture is writable.
 * @pre No mock transport callback is executing concurrently.
 * @post Primary-address probing is enabled.
 * @post Write failure injection is disabled.
 * @note Tests may override individual fields after reset.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_mock_reset(void)
{
  memset(&s_mock, 0, sizeof(s_mock));
  s_mock.primary_present                       = true;
  s_mock.fail_write_at                         = (uint32_t)k_test_no_failure;
  s_mock.regs[(uint16_t)k_test_reg_chip_id_hi] = (uint8_t)k_test_chip_id_hi;
  s_mock.regs[(uint16_t)k_test_reg_chip_id_lo] = (uint8_t)k_test_chip_id_lo;
}

/**
 * @brief Report whether the addressed sensor is present.
 * @details Maps the two legal SCCB addresses to independent fixture flags.
 * @param[in] address SCCB address to inspect.
 * @return Presence state.
 * @retval true The selected legal address is enabled.
 * @retval false The address is absent or unsupported.
 * @pre The global mock fixture has been initialized.
 * @pre `address` is representable in one SCCB address byte.
 * @post Mock fixture state remains unchanged.
 * @post No read or write counter is incremented.
 * @note Unsupported addresses never respond.
 * @since 0.1.0
 */
RA8_INTERNAL static bool internal_mock_address_present(uint8_t address)
{
  if (address == (uint8_t)k_ra8_ov5640_addr_primary) {
    return s_mock.primary_present;
  }
  if (address == (uint8_t)k_ra8_ov5640_addr_secondary) {
    return s_mock.secondary_present;
  }
  return false;
}

/**
 * @brief Mock one-register SCCB read.
 * @details Applies injected transport errors, address presence, and optional corruption.
 * @param[in] ctx Unused transport context.
 * @param[in] address SCCB device address.
 * @param[in] reg Sensor register address.
 * @param[out] out_value Destination for the returned byte.
 * @return Repository error code.
 * @retval k_ra8_ok A fixture byte was returned.
 * @retval k_ra8_err_nack The addressed sensor is absent.
 * @pre `out_value` points to writable byte storage.
 * @pre The global mock fixture has been initialized.
 * @post The read counter is incremented once.
 * @post On success, `out_value` contains the selected fixture value.
 * @note A forced read error takes precedence over address presence.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t
internal_mock_read(void* ctx, uint8_t address, uint16_t reg, uint8_t* out_value)
{
  (void)ctx;
  s_mock.read_count++;
  if (s_mock.forced_read_error != k_ra8_ok) {
    return s_mock.forced_read_error;
  }
  if (!internal_mock_address_present(address)) {
    return k_ra8_err_nack;
  }
  *out_value =
    (s_mock.corrupt_read && (reg == s_mock.corrupt_reg)) ? s_mock.corrupt_value : s_mock.regs[reg];
  return k_ra8_ok;
}

/**
 * @brief Mock one-register SCCB write and record its wire-visible fields.
 * @details Enforces injected failure and address presence before updating the register file.
 * @param[in] ctx Unused transport context.
 * @param[in] address SCCB device address.
 * @param[in] reg Sensor register address.
 * @param[in] value Register value to write.
 * @return Repository error code.
 * @retval k_ra8_ok The write was recorded and applied.
 * @retval k_ra8_err_nack The injected index failed or the device is absent.
 * @pre The global mock fixture has been initialized.
 * @pre The write log capacity constant matches its backing array.
 * @post Successful writes update the addressed register.
 * @post Successful writes increment the write counter once.
 * @note Writes beyond log capacity still update registers and counters.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t
internal_mock_write(void* ctx, uint8_t address, uint16_t reg, uint8_t value)
{
  (void)ctx;
  if (s_mock.write_count == s_mock.fail_write_at) {
    return k_ra8_err_nack;
  }
  if (!internal_mock_address_present(address)) {
    return k_ra8_err_nack;
  }
  if (s_mock.write_count < (uint32_t)k_test_write_log_capacity) {
    s_mock.writes[s_mock.write_count] = (ov5640_mock_write_t){
      .address = address,
      .reg     = reg,
      .value   = value,
    };
  }
  s_mock.write_count++;
  s_mock.regs[reg] = value;
  return k_ra8_ok;
}

/**
 * @brief Record a requested millisecond delay.
 * @details Appends bounded delay history while always counting every request.
 * @param[in] ctx Unused transport context.
 * @param[in] milliseconds Requested delay duration.
 * @pre The global mock fixture has been initialized.
 * @pre The delay-log capacity matches its backing array.
 * @post The delay counter is incremented once.
 * @post In-capacity requests are retained in order.
 * @note No real time elapses in this mock.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_mock_delay(void* ctx, uint32_t milliseconds)
{
  (void)ctx;
  if (s_mock.delay_count < (uint32_t)k_test_delay_log_capacity) {
    s_mock.delays[s_mock.delay_count] = milliseconds;
  }
  s_mock.delay_count++;
}

/**
 * @brief Count successful mock writes to one sensor register.
 * @details Scans only the initialized prefix of the bounded write journal.
 * @param[in] reg Sensor register address to count.
 * @return Number of matching entries in the bounded write log.
 * @retval uint32_t The exact number of retained writes targeting @p reg.
 * @pre The mock write count does not exceed its log capacity.
 * @pre Every initialized journal entry contains a complete register address.
 * @post Mock state remains unchanged.
 * @post The result includes every and only the matching initialized entries.
 * @note Failed writes are not appended and therefore are not counted.
 * @since 0.1.0
 */
RA8_INTERNAL static uint32_t internal_mock_write_count_for_reg(uint16_t reg)
{
  uint32_t count = 0U;
  for (uint32_t write_index = 0U; write_index < s_mock.write_count; write_index += 1U) {
    if (s_mock.writes[write_index].reg == reg) {
      count += 1U;
    }
  }
  return count;
}

/**
 * @brief Build a complete injected transport.
 * @details Binds all OV5640 bus callbacks to the global host fixture.
 * @return OV5640 bus descriptor.
 * @retval ra8_ov5640_bus_t Fully populated mock transport.
 * @pre Mock callback functions are linked into the test executable.
 * @pre The global fixture outlives any returned bus descriptor.
 * @post The global fixture remains unchanged.
 * @post The returned descriptor contains no owned resources.
 * @note The opaque context is intentionally null.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_ov5640_bus_t internal_make_bus(void)
{
  const ra8_ov5640_bus_t bus = {
    .read_reg  = internal_mock_read,
    .write_reg = internal_mock_write,
    .delay_ms  = internal_mock_delay,
    .ctx       = nullptr,
  };
  return bus;
}

/**
 * @brief Initialize one sensor instance against the mock.
 * @details Constructs a transport and asserts successful driver initialization.
 * @return Initialized OV5640 device state.
 * @retval ra8_ov5640_t Device bound to the mock callbacks.
 * @pre Unity test accounting is initialized.
 * @pre The global mock fixture has been reset for the calling test.
 * @post The returned device is marked initialized.
 * @post No SCCB transfer has occurred solely from construction.
 * @note Initialization failure is recorded as a test assertion.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_ov5640_t internal_make_device(void)
{
  ra8_ov5640_t           dev = {};
  const ra8_ov5640_bus_t bus = internal_make_bus();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ov5640_init(&dev, &bus));
  return dev;
}

/**
 * @brief Verify initialization dependency validation.
 * @details Rejects missing device, bus, and each required callback independently.
 * @par MC/DC:
 * The complete bus is the all-false rejection baseline. Each missing device,
 * bus, or callback vector independently makes one dependency guard true.
 * @pre Unity test accounting is initialized.
 * @pre The mock fixture can be reset.
 * @post Valid initialization selects the primary address.
 * @post Every missing dependency records a null-pointer error.
 * @note This test performs no SCCB transfer.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_init_validates_transport(void)
{
  TEST_BEGIN("ov5640: init validates transport");
  internal_mock_reset();
  ra8_ov5640_t           dev = {};
  const ra8_ov5640_bus_t bus = internal_make_bus();
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_ov5640_init(nullptr, &bus));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_ov5640_init(&dev, nullptr));
  ra8_ov5640_bus_t invalid = bus;
  invalid.read_reg         = nullptr;
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_ov5640_init(&dev, &invalid));
  invalid           = bus;
  invalid.write_reg = nullptr;
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_ov5640_init(&dev, &invalid));
  invalid          = bus;
  invalid.delay_ms = nullptr;
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_ov5640_init(&dev, &invalid));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ov5640_init(&dev, &bus));
  TEST_ASSERT(dev.initialized);
  TEST_ASSERT_EQ(k_ra8_ov5640_addr_primary, dev.address);
  TEST_END("ov5640: init validates transport");
}

/**
 * @brief Verify raw SCCB register access forwarding.
 * @details Checks register zero, wire-visible fields, null pointers, and fresh-device guards.
 * @par MC/DC:
 * Valid read/write calls provide the accepted baseline; null output and fresh
 * device vectors independently flip the pointer and initialization guards.
 * @pre Unity test accounting is initialized.
 * @pre The mock fixture can be reset.
 * @post A successful read returns the previously written value.
 * @post Invalid calls record their documented errors.
 * @note Register zero is deliberately treated as valid.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_raw_register_access(void)
{
  TEST_BEGIN("ov5640: raw register access forwards transport");
  internal_mock_reset();
  ra8_ov5640_t dev = internal_make_device();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ov5640_write_reg(&dev, 0U, (uint8_t)k_test_read_poison));
  uint8_t value = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ov5640_read_reg(&dev, 0U, &value));
  TEST_ASSERT_EQ(k_test_read_poison, value);
  TEST_ASSERT_EQ(k_ra8_ov5640_addr_primary, s_mock.writes[0].address);
  TEST_ASSERT_EQ(0U, s_mock.writes[0].reg);
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_ov5640_read_reg(nullptr, 0U, &value));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_ov5640_read_reg(&dev, 0U, nullptr));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_ov5640_write_reg(nullptr, 0U, 0U));
  ra8_ov5640_t fresh = {};
  TEST_ASSERT_EQ(k_ra8_err_not_initialized, ra8_ov5640_read_reg(&fresh, 0U, &value));
  TEST_ASSERT_EQ(k_ra8_err_not_initialized, ra8_ov5640_write_reg(&fresh, 0U, 0U));
  TEST_END("ov5640: raw register access forwards transport");
}

/**
 * @brief Verify probing at both legal strap addresses.
 * @details Enables each mock address in turn and checks chip ID and retained address.
 * @par MC/DC:
 * Primary-only and secondary-only vectors independently vary each address
 * response while retaining the same valid identifier.
 * @pre Unity test accounting is initialized.
 * @pre Both address flags are writable by the test.
 * @post Each responding sensor reports the expected chip identifier.
 * @post Device state retains the matching address.
 * @note Only one address is enabled for the secondary-address phase.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_probe_both_addresses(void)
{
  TEST_BEGIN("ov5640: probe checks both legal addresses");
  internal_mock_reset();
  ra8_ov5640_t dev = internal_make_device();
  uint16_t     id  = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ov5640_probe(&dev, &id));
  TEST_ASSERT_EQ(k_ra8_ov5640_chip_id, id);
  TEST_ASSERT_EQ(k_ra8_ov5640_addr_primary, dev.address);

  s_mock.primary_present   = false;
  s_mock.secondary_present = true;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ov5640_probe(&dev, &id));
  TEST_ASSERT_EQ(k_ra8_ov5640_chip_id, id);
  TEST_ASSERT_EQ(k_ra8_ov5640_addr_secondary, dev.address);
  TEST_END("ov5640: probe checks both legal addresses");
}

/**
 * @brief Verify probe failure and state restoration.
 * @details Covers absent devices, wrong IDs, null outputs, and uninitialized state.
 * @par MC/DC:
 * A responding sensor with the correct ID is the accepted baseline. Absence,
 * wrong ID, null output, and fresh state each flip one rejection condition.
 * @pre Unity test accounting is initialized.
 * @pre The mock identifier and presence flags are writable.
 * @post Not-found paths restore the primary address selection.
 * @post The output identifier is cleared when no sensor responds.
 * @note Transport errors are represented by address absence here.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_probe_not_found(void)
{
  TEST_BEGIN("ov5640: probe rejects missing and wrong ID");
  internal_mock_reset();
  ra8_ov5640_t dev       = internal_make_device();
  uint16_t     id        = UINT16_MAX;
  s_mock.primary_present = false;
  TEST_ASSERT_EQ(k_ra8_err_not_found, ra8_ov5640_probe(&dev, &id));
  TEST_ASSERT_EQ(0U, id);
  TEST_ASSERT_EQ(k_ra8_ov5640_addr_primary, dev.address);

  s_mock.primary_present                       = true;
  s_mock.regs[(uint16_t)k_test_reg_chip_id_hi] = (uint8_t)k_test_wrong_id;
  TEST_ASSERT_EQ(k_ra8_err_not_found, ra8_ov5640_probe(&dev, &id));
  TEST_ASSERT_EQ(k_ra8_ov5640_addr_primary, dev.address);
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_ov5640_probe(nullptr, &id));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_ov5640_probe(&dev, nullptr));
  ra8_ov5640_t fresh = {};
  TEST_ASSERT_EQ(k_ra8_err_not_initialized, ra8_ov5640_probe(&fresh, &id));
  TEST_END("ov5640: probe rejects missing and wrong ID");
}

/**
 * @brief Verify the validated VGA UYVY register scene.
 * @details Checks table size, reset sequence, output format, test pattern, and delays.
 * @par MC/DC: not applicable -- this case verifies the sequential successful
 * register and delay sequence; failure decisions are covered separately.
 * @pre Unity test accounting is initialized.
 * @pre The mock fixture can retain the complete write sequence.
 * @post Final registers describe VGA packed YUV output.
 * @post Reset and configuration delays are recorded in order.
 * @note Driver readback verification also runs through the mock.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_configure_uyvy(void)
{
  TEST_BEGIN("ov5640: configure VGA UYVY");
  internal_mock_reset();
  ra8_ov5640_t dev = internal_make_device();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ov5640_configure(&dev, k_ra8_ov5640_mode_vga_uyvy));
  TEST_ASSERT(s_mock.write_count > (uint32_t)k_test_min_scene_write_count);
  TEST_ASSERT_EQ(k_test_sw_reset_hold, s_mock.writes[0].value);
  TEST_ASSERT_EQ(k_test_reg_sw_reset, s_mock.writes[0].reg);
  TEST_ASSERT_EQ(k_test_sw_reset_wake, s_mock.regs[(uint16_t)k_test_reg_sw_reset]);
  TEST_ASSERT_EQ(k_test_mcu_reset_hold, s_mock.regs[(uint16_t)k_test_reg_system_reset00]);
  TEST_ASSERT_EQ(k_test_format_yuyv, s_mock.regs[(uint16_t)k_test_reg_format]);
  TEST_ASSERT_EQ(0U, s_mock.regs[(uint16_t)k_test_reg_isp_mux]);
  TEST_ASSERT_EQ(k_test_dvp_polarity_raw, s_mock.regs[(uint16_t)k_test_reg_polarity_ctrl00]);
  TEST_ASSERT_EQ(0U, s_mock.regs[(uint16_t)k_test_reg_test_pattern]);
  TEST_ASSERT_EQ(4U, s_mock.delay_count);
  TEST_ASSERT_EQ(k_test_reset_guard_ms, s_mock.delays[0]);
  TEST_ASSERT_EQ(k_test_reset_guard_ms, s_mock.delays[1]);
  TEST_ASSERT_EQ(k_test_mcu_reset_delay_ms, s_mock.delays[2]);
  TEST_ASSERT_EQ(k_test_config_delay_ms, s_mock.delays[3]);
  TEST_END("ov5640: configure VGA UYVY");
}

/**
 * @brief Assert final JPEG register state after configuration.
 * @details Checks reset, timing, JPEG, ISP, PLL, VFIFO, and clock fields.
 * @pre `s_mock.regs` contains a completed JPEG configuration scene.
 * @pre The mock register array covers the complete 16-bit address space.
 * @post Every required JPEG, timing, and clock field is checked.
 * @post The register fixture remains unchanged.
 * @note Assertion failure terminates the current test process.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_assert_jpeg_register_state(void)
{
  TEST_ASSERT_EQ(0U,
                 s_mock.regs[(uint16_t)k_test_reg_system_reset02] &
                   (uint8_t)k_test_jpeg_reset_mask);
  TEST_ASSERT_EQ(k_test_mcu_reset_hold, s_mock.regs[(uint16_t)k_test_reg_system_reset00]);
  TEST_ASSERT_EQ(k_test_jpeg_clocks_enabled, s_mock.regs[(uint16_t)k_test_reg_clock_enable02]);
  TEST_ASSERT_EQ(k_test_timing_tc_reg20, s_mock.regs[(uint16_t)k_test_reg_timing_tc_reg20]);
  TEST_ASSERT_EQ(k_test_jpeg_enable_mask,
                 s_mock.regs[(uint16_t)k_test_reg_timing_tc_reg21] &
                   (uint8_t)k_test_jpeg_enable_mask);
  TEST_ASSERT_EQ(k_ra8_ov5640_jpeg_quant_scale_default,
                 s_mock.regs[(uint16_t)k_test_reg_jpeg_quality]);
  TEST_ASSERT_EQ(k_test_jpeg_mode_2, s_mock.regs[(uint16_t)k_test_reg_jpeg_mode]);
  TEST_ASSERT_EQ(k_test_isp_scale_mask,
                 s_mock.regs[(uint16_t)k_test_reg_isp_ctrl01] & (uint8_t)k_test_isp_scale_mask);
  TEST_ASSERT_EQ(k_test_jpeg_ctrl1c, s_mock.regs[(uint16_t)k_test_reg_jpeg_ctrl1c]);
  TEST_ASSERT_EQ(k_test_dvp_jpeg_polarity, s_mock.regs[(uint16_t)k_test_reg_polarity_ctrl00]);
  TEST_ASSERT_EQ(k_test_pll_bypass, s_mock.regs[(uint16_t)k_test_reg_pll_bypass]);
  TEST_ASSERT_EQ(k_test_pll_bit_mode, s_mock.regs[(uint16_t)k_test_reg_pll_bit_mode]);
  TEST_ASSERT_EQ(k_test_pll_sys_div, s_mock.regs[(uint16_t)k_test_reg_pll_sys_div]);
  TEST_ASSERT_EQ(k_test_pll_multiplier, s_mock.regs[(uint16_t)k_test_reg_pll_multiplier]);
  TEST_ASSERT_EQ(k_test_pll_pre_div, s_mock.regs[(uint16_t)k_test_reg_pll_pre_div]);
  TEST_ASSERT_EQ(k_test_clock_root, s_mock.regs[(uint16_t)k_test_reg_clock_root]);
  TEST_ASSERT_EQ(k_test_jpeg_pclk_divider, s_mock.regs[(uint16_t)k_test_reg_pclk_divider]);
  TEST_ASSERT_EQ(k_test_jpeg_vfifo_ctrl0b, s_mock.regs[(uint16_t)k_test_reg_vfifo_ctrl0b]);
  TEST_ASSERT_EQ(k_test_jpeg_vfifo_ctrl0c, s_mock.regs[(uint16_t)k_test_reg_vfifo_ctrl0c]);
  TEST_ASSERT_EQ(k_test_clock_select, s_mock.regs[(uint16_t)k_test_reg_clock_select]);
}

/**
 * @brief Assert clock writes remain confined to the qualified base scene.
 * @details Counts each PLL, root, pixel, and source-select register write.
 * @pre The mock write log contains a completed JPEG configuration.
 * @pre Its retained entry count is within the journal capacity.
 * @post Every clock-register write count is checked.
 * @post The write journal remains unchanged.
 * @note The source selector is deliberately written twice by the qualified scene.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_assert_jpeg_clock_write_counts(void)
{
  TEST_ASSERT_EQ(1U, internal_mock_write_count_for_reg((uint16_t)k_test_reg_pll_bit_mode));
  TEST_ASSERT_EQ(1U, internal_mock_write_count_for_reg((uint16_t)k_test_reg_pll_sys_div));
  TEST_ASSERT_EQ(1U, internal_mock_write_count_for_reg((uint16_t)k_test_reg_pll_multiplier));
  TEST_ASSERT_EQ(1U, internal_mock_write_count_for_reg((uint16_t)k_test_reg_pll_pre_div));
  TEST_ASSERT_EQ(1U, internal_mock_write_count_for_reg((uint16_t)k_test_reg_clock_root));
  TEST_ASSERT_EQ(1U, internal_mock_write_count_for_reg((uint16_t)k_test_reg_pclk_divider));
  TEST_ASSERT_EQ(2U, internal_mock_write_count_for_reg((uint16_t)k_test_reg_clock_select));
}

/**
 * @brief Assert the ordered JPEG transition tail and stream lifecycle.
 * @details Verifies the seven final writes plus reset hold/wake ordering.
 * @pre The mock write log contains a completed JPEG configuration.
 * @pre The log contains at least the fixed transition-tail length.
 * @post Tail register ordering and the reset-to-wake transition are checked.
 * @post The write journal and register fixture remain unchanged.
 * @note The helper also pins the exact two-entry stream-state sequence.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_assert_jpeg_transition_order(void)
{
  const uint32_t tail = s_mock.write_count - (uint32_t)k_test_jpeg_tail_write_count;
  TEST_ASSERT_EQ(k_test_reg_format, s_mock.writes[tail].reg);
  TEST_ASSERT_EQ(k_test_reg_isp_mux, s_mock.writes[tail + 1U].reg);
  TEST_ASSERT_EQ(k_test_reg_polarity_ctrl00, s_mock.writes[tail + 2U].reg);
  TEST_ASSERT_EQ(k_test_reg_timing_tc_reg21, s_mock.writes[tail + 3U].reg);
  TEST_ASSERT_EQ(k_test_reg_system_reset02, s_mock.writes[tail + 4U].reg);
  TEST_ASSERT_EQ(k_test_reg_clock_enable02, s_mock.writes[tail + 5U].reg);
  TEST_ASSERT_EQ(k_test_reg_system_reset00, s_mock.writes[tail + 6U].reg);
  uint8_t  stream_states[2] = {};
  uint32_t stream_count     = 0U;
  for (uint32_t i = 0U; i < s_mock.write_count; i += 1U) {
    if (s_mock.writes[i].reg == (uint16_t)k_test_reg_sw_reset) {
      if (stream_count < (uint32_t)sizeof(stream_states)) {
        stream_states[stream_count] = s_mock.writes[i].value;
      }
      stream_count += 1U;
    }
  }
  TEST_ASSERT_EQ(2U, stream_count);
  TEST_ASSERT_EQ(k_test_sw_reset_hold, stream_states[0]);
  TEST_ASSERT_EQ(k_test_sw_reset_wake, stream_states[1]);
}

/**
 * @brief Verify the VGA JPEG switch sequence.
 * @details Checks reset, clocks, timing, quantization, mode, divider, and VFIFO values.
 * @par MC/DC: not applicable -- this case verifies the sequential successful
 * JPEG register sequence; rejected modes and transfers are covered separately.
 * @pre Unity test accounting is initialized.
 * @pre The mock fixture can retain the complete VGA base table.
 * @post JPEG blocks are released from reset and clocked.
 * @post Final JPEG controls match the selected repository sequence.
 * @note The test observes final register state after readback verification.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_configure_jpeg(void)
{
  TEST_BEGIN("ov5640: configure VGA JPEG");
  internal_mock_reset();
  ra8_ov5640_t dev = internal_make_device();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ov5640_configure(&dev, k_ra8_ov5640_mode_vga_jpeg));
  internal_assert_jpeg_register_state();
  internal_assert_jpeg_clock_write_counts();
  internal_assert_jpeg_transition_order();
  TEST_ASSERT_EQ(4U, s_mock.delay_count);
  TEST_ASSERT_EQ(k_test_reset_guard_ms, s_mock.delays[0]);
  TEST_ASSERT_EQ(k_test_reset_guard_ms, s_mock.delays[1]);
  TEST_ASSERT_EQ(k_test_mcu_reset_delay_ms, s_mock.delays[2]);
  TEST_ASSERT_EQ(k_test_config_delay_ms, s_mock.delays[3]);
  TEST_END("ov5640: configure VGA JPEG");
}

/**
 * @brief Verify configuration validation and failure propagation.
 * @details Covers null, fresh state, unsupported mode, write NACK, and corrupt readback.
 * @par MC/DC:
 * A valid initialized device and supported mode provide the accepted baseline.
 * Each null, state, mode, write, or readback vector flips one guard independently.
 * @pre Unity test accounting is initialized.
 * @pre Failure-injection fields are writable by the test.
 * @post Each injected failure records its expected error.
 * @post No failed configuration is reported as successful.
 * @note UYVY and JPEG readback corruption are tested separately.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_configure_failures(void)
{
  TEST_BEGIN("ov5640: configure propagates failures");
  internal_mock_reset();
  ra8_ov5640_t fresh = {};
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_ov5640_configure(nullptr, k_ra8_ov5640_mode_vga_uyvy));
  TEST_ASSERT_EQ(k_ra8_err_not_initialized,
                 ra8_ov5640_configure(&fresh, k_ra8_ov5640_mode_vga_uyvy));
  ra8_ov5640_t dev = internal_make_device();
  TEST_ASSERT_EQ(k_ra8_err_not_supported, ra8_ov5640_configure(&dev, (ra8_ov5640_mode_t)UINT8_MAX));
  s_mock.fail_write_at = 0U;
  TEST_ASSERT_EQ(k_ra8_err_nack, ra8_ov5640_configure(&dev, k_ra8_ov5640_mode_vga_uyvy));

  internal_mock_reset();
  dev                  = internal_make_device();
  s_mock.corrupt_read  = true;
  s_mock.corrupt_reg   = (uint16_t)k_test_reg_format;
  s_mock.corrupt_value = 0U;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_ov5640_configure(&dev, k_ra8_ov5640_mode_vga_uyvy));

  internal_mock_reset();
  dev                  = internal_make_device();
  s_mock.corrupt_read  = true;
  s_mock.corrupt_reg   = (uint16_t)k_test_reg_jpeg_mode;
  s_mock.corrupt_value = 0U;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_ov5640_configure(&dev, k_ra8_ov5640_mode_vga_jpeg));
  TEST_END("ov5640: configure propagates failures");
}

/**
 * @brief Verify raw JPEG quantization-scale updates.
 * @details Checks upper-bit preservation, the 0..63 bound, state guards, and read errors.
 * @par MC/DC:
 * An initialized device and in-range scale provide the accepted baseline;
 * range, state, and transport vectors independently flip rejection guards.
 * @pre Unity test accounting is initialized.
 * @pre The mock quality register is writable.
 * @post Valid scale updates preserve unrelated register bits.
 * @post Invalid scale and transport failures record expected errors.
 * @note Lower sensor scale values represent higher JPEG quality.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_jpeg_quantization_scale(void)
{
  TEST_BEGIN("ov5640: JPEG quantization scale");
  internal_mock_reset();
  ra8_ov5640_t dev                               = internal_make_device();
  s_mock.regs[(uint16_t)k_test_reg_jpeg_quality] = (uint8_t)k_test_upper_bits_fixture;
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_ov5640_set_jpeg_quantization_scale(&dev, (uint8_t)k_test_quant_scale_fixture));
  TEST_ASSERT_EQ(k_test_upper_bits_fixture | k_test_quant_scale_fixture,
                 s_mock.regs[(uint16_t)k_test_reg_jpeg_quality]);
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_ov5640_set_jpeg_quantization_scale(&dev, (uint8_t)k_test_quant_scale_invalid));
  TEST_ASSERT_EQ(
    k_ra8_err_null_ptr,
    ra8_ov5640_set_jpeg_quantization_scale(nullptr, (uint8_t)k_test_quant_scale_fixture));
  ra8_ov5640_t fresh = {};
  TEST_ASSERT_EQ(
    k_ra8_err_not_initialized,
    ra8_ov5640_set_jpeg_quantization_scale(&fresh, (uint8_t)k_test_quant_scale_fixture));
  s_mock.forced_read_error = k_ra8_err_nack;
  TEST_ASSERT_EQ(k_ra8_err_nack,
                 ra8_ov5640_set_jpeg_quantization_scale(&dev, (uint8_t)k_test_quant_scale_fixture));
  TEST_END("ov5640: JPEG quantization scale");
}

/**
 * @brief Verify JPEG status decoding and error handling.
 * @details Exercises 24-bit length assembly and all documented status bits.
 * @par MC/DC:
 * A readable initialized device is the accepted baseline; fresh-state and
 * transport-failure vectors independently vary the two rejection conditions.
 * @pre Unity test accounting is initialized.
 * @pre The mock register file is writable.
 * @post A successful snapshot matches the synthetic sensor state.
 * @post Invalid state and transport failures return their expected errors.
 * @note The caller stops streaming when a frame-coherent length is required.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_jpeg_status(void)
{
  TEST_BEGIN("ov5640: JPEG status");
  internal_mock_reset();
  ra8_ov5640_t dev                                   = internal_make_device();
  s_mock.regs[(uint16_t)k_test_reg_jpeg_length_hi]   = 0x01U;
  s_mock.regs[(uint16_t)k_test_reg_jpeg_length_mid]  = 0x23U;
  s_mock.regs[(uint16_t)k_test_reg_jpeg_length_lo]   = 0x45U;
  s_mock.regs[(uint16_t)k_test_reg_jfifo_overflow]   = 0x01U;
  s_mock.regs[(uint16_t)k_test_reg_jpeg_ctrl00]      = 0x81U;
  s_mock.regs[(uint16_t)k_test_reg_jpeg_ctrl01]      = 0x01U;
  s_mock.regs[(uint16_t)k_test_reg_jpeg_ctrl04]      = 0x24U;
  s_mock.regs[(uint16_t)k_test_reg_vfifo_ctrl00]     = 0x80U;
  s_mock.regs[(uint16_t)k_test_reg_compression_w_hi] = 0x02U;
  s_mock.regs[(uint16_t)k_test_reg_compression_w_lo] = 0x80U;
  s_mock.regs[(uint16_t)k_test_reg_compression_h_hi] = 0x01U;
  s_mock.regs[(uint16_t)k_test_reg_compression_h_lo] = 0xE0U;
  s_mock.regs[(uint16_t)k_test_reg_href_minimum]     = 0x40U;
  s_mock.regs[(uint16_t)k_test_reg_timing_tc_reg21]  = 0x21U;
  ra8_ov5640_jpeg_status_t status                    = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ov5640_jpeg_status_get(&dev, &status));
  TEST_ASSERT_EQ(k_test_jpeg_length, status.encoded_bytes);
  TEST_ASSERT(status.fifo_overflow);
  TEST_ASSERT(status.input_is_yuv422);
  TEST_ASSERT(status.header_output);
  TEST_ASSERT(status.compression_enabled);
  TEST_ASSERT_EQ(k_test_compression_width, status.compression_width);
  TEST_ASSERT_EQ(k_test_compression_height, status.compression_height);
  TEST_ASSERT_EQ(0x01U, status.jpeg_ctrl01);
  TEST_ASSERT_EQ(0x80U, status.vfifo_ctrl00);
  TEST_ASSERT_EQ(0x40U, status.href_minimum_blanking);
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_ov5640_jpeg_status_get(nullptr, &status));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_ov5640_jpeg_status_get(&dev, nullptr));
  ra8_ov5640_t fresh = {};
  TEST_ASSERT_EQ(k_ra8_err_not_initialized, ra8_ov5640_jpeg_status_get(&fresh, &status));
  s_mock.forced_read_error = k_ra8_err_nack;
  TEST_ASSERT_EQ(k_ra8_err_nack, ra8_ov5640_jpeg_status_get(&dev, &status));
  TEST_END("ov5640: JPEG status");
}

/**
 * @brief Verify sensor stream standby and wake control.
 * @details Checks control values, settle delays, null/fresh state, and write failure.
 * @par MC/DC:
 * Successful stop/start calls provide the accepted baseline. Null, fresh-state,
 * and write-failure vectors independently flip each rejection condition.
 * @pre Unity test accounting is initialized.
 * @pre The mock delay and write logs are available.
 * @post Successful transitions write their expected software-control values.
 * @post Each successful transition records one settle delay.
 * @note The mock does not model image timing after wake.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_stream_control(void)
{
  TEST_BEGIN("ov5640: stream control");
  internal_mock_reset();
  ra8_ov5640_t dev = internal_make_device();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ov5640_stream_set(&dev, false));
  TEST_ASSERT_EQ(k_test_sw_standby, s_mock.regs[(uint16_t)k_test_reg_sw_reset]);
  TEST_ASSERT_EQ(k_test_stream_settle_ms, s_mock.delays[0]);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ov5640_stream_set(&dev, true));
  TEST_ASSERT_EQ(k_test_sw_reset_wake, s_mock.regs[(uint16_t)k_test_reg_sw_reset]);
  TEST_ASSERT_EQ(k_test_stream_settle_ms, s_mock.delays[1]);
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_ov5640_stream_set(nullptr, true));
  ra8_ov5640_t fresh = {};
  TEST_ASSERT_EQ(k_ra8_err_not_initialized, ra8_ov5640_stream_set(&fresh, true));
  s_mock.fail_write_at = s_mock.write_count;
  TEST_ASSERT_EQ(k_ra8_err_nack, ra8_ov5640_stream_set(&dev, false));
  TEST_END("ov5640: stream control");
}

/**
 * @brief Run OV5640 probe and mode-configuration MC/DC vectors.
 * @details Executes every transport, probe, format, status, and stream group.
 * @par MC/DC:
 * Decisions: libs/ra8_ov5640/src/ra8_ov5640.c@ra8_ov5640_configure,
 * libs/ra8_ov5640/src/ra8_ov5640.c@ra8_ov5640_probe.
 * @pre Unity test accounting is initialized.
 * @pre The in-memory register and journal capacities match their enum bounds.
 * @post Every OV5640 transport and mode vector group has executed once.
 * @post Any failed assertion has terminated before this helper returns.
 * @note This single dispatcher keeps the executable entry point minimal.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_mcdc_ov5640_modes(void)
{
  internal_test_init_validates_transport();
  internal_test_raw_register_access();
  internal_test_probe_both_addresses();
  internal_test_probe_not_found();
  internal_test_configure_uyvy();
  internal_test_configure_jpeg();
  internal_test_configure_failures();
  internal_test_jpeg_quantization_scale();
  internal_test_jpeg_status();
  internal_test_stream_control();
}

int main(void)
{
  internal_test_mcdc_ov5640_modes();
  return 0;
}
