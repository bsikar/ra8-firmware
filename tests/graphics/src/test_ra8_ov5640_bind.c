/**
 * @file test_ra8_ov5640_bind.c
 * @brief Host vectors for the OV5640 binder over the house I2C seam.
 *
 * @details
 * Covers `ra8_ov5640_bind_i2c` only: its refusal surface, and the two wire
 * shapes it is responsible for. The sensor's own register logic is covered
 * by `test_ra8_ov5640.c`, which drives the transport interface directly; the
 * question here is narrower, namely whether the adapter that used to live in
 * each consuming app frames a read as one write-RESTART-read of the two-byte
 * register pointer and a write as one `[reg_hi][reg_lo][value]` with STOP.
 *
 * The fixture fills all three seam callbacks, including the plain `read` the
 * binder must never reach for, so that "the binder used transfer" is an
 * assertion and not an assumption.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>
#include <string.h>

#include "ra8_attributes.h"
#include "ra8_err.h"
#include "ra8_i2c_bus_ops.h"
#include "ra8_ov5640.h"
#include "unity_minimal.h"

/** @brief Fixture capacities and the registers these vectors touch. */
typedef enum : uint32_t {
  k_bind_register_count  = 65536U,  /**< Complete 16-bit register space.     */
  k_bind_frame_capacity  = 4U,      /**< Bytes recorded per seam frame.      */
  k_bind_high_shift      = 8U,      /**< Register-pointer high-byte shift.   */
  k_bind_probe_reg       = 0x300AU, /**< Chip-ID high byte, used as a probe. */
  k_bind_probe_reg_hi    = 0x30U,   /**< Probe register pointer, high byte.  */
  k_bind_probe_reg_lo    = 0x0AU,   /**< Probe register pointer, low byte.   */
  k_bind_chip_id_lo_reg  = 0x300BU, /**< Chip-ID low byte.                   */
  k_bind_chip_id_hi_byte = 0x56U,   /**< Expected chip-ID high byte.         */
  k_bind_chip_id_lo_byte = 0x40U,   /**< Expected chip-ID low byte.          */
  k_bind_write_value     = 0x37U,   /**< Value written by the write vector.  */
} ra8_ov5640_bind_test_const_t;

/** @brief Wire-visible record of one house-seam transaction. */
typedef struct {
  uint8_t  frame[k_bind_frame_capacity]; /**< Bytes handed to the seam.   */
  uint32_t wr_len;                       /**< Write-half byte count.      */
  uint32_t rd_len;                       /**< Read-half byte count.       */
  uint8_t  address;                      /**< 7-bit address addressed.    */
  bool     send_stop;                    /**< STOP flag on a plain write. */
} ov5640_bind_record_t;

/** @brief Fixture backing the house-seam binder vectors. */
typedef struct {
  uint8_t              regs[k_bind_register_count]; /**< Simulated register file. */
  ov5640_bind_record_t last_write;                  /**< Most recent write.       */
  ov5640_bind_record_t last_xfer;                   /**< Most recent transfer.    */
  uint32_t             write_count;                 /**< Framed writes seen.      */
  uint32_t             xfer_count;                  /**< Transfers seen.          */
  uint32_t             plain_read_count;            /**< Plain reads seen.        */
  uint32_t             delay_count;                 /**< Delay callbacks seen.    */
} ov5640_bind_mock_t;

static ov5640_bind_mock_t s_mock;

/**
 * @brief Decode the big-endian register pointer the binder staged.
 * @details Mirrors the packing in `ra8_ov5640_bind.c` so a framing change
 *          fails the vectors instead of quietly moving both sides together.
 * @param[in] bytes Two-byte pointer, high byte first.
 * @return The 16-bit register address.
 * @pre `bytes` holds two readable bytes.
 * @post No fixture state changes.
 * @note Test-only helper.
 * @since 0.1.0
 */
RA8_INTERNAL static uint16_t internal_decode_reg(const uint8_t* bytes)
{
  return (uint16_t)(((uint16_t)bytes[0] << (uint16_t)k_bind_high_shift) | (uint16_t)bytes[1]);
}

/**
 * @brief Clear the fixture between vectors.
 * @details Zeroes the register file, both records, and every counter.
 * @pre The fixture is exclusively owned by the current test vector.
 * @post Every counter reads zero.
 * @note Test-only and not reentrant because the fixture has file scope.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_mock_reset(void)
{
  (void)memset(&s_mock, 0, sizeof(s_mock));
}

/**
 * @brief Seam write half: record the staged frame and apply it.
 * @details Decodes the register pointer and stores the trailing payload byte.
 * @param[in] ctx Binder-owned cookie.
 * @param[in] addr 7-bit address.
 * @param[in] data Staged frame.
 * @param[in] len Frame length.
 * @param[in] send_stop STOP flag as the binder set it.
 * @return Repository error code.
 * @retval k_ra8_ok The frame was recorded.
 * @pre `data` holds `len` readable bytes.
 * @post The write counter is incremented once.
 * @note Test-only and not reentrant because the fixture has file scope.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t
internal_seam_write(void* ctx, uint8_t addr, const uint8_t* data, uint32_t len, bool send_stop)
{
  (void)ctx;
  s_mock.write_count++;
  s_mock.last_write =
    (ov5640_bind_record_t){.wr_len = len, .rd_len = 0U, .address = addr, .send_stop = send_stop};
  for (uint32_t i = 0U; (i < len) && (i < (uint32_t)k_bind_frame_capacity); ++i) {
    s_mock.last_write.frame[i] = data[i];
  }
  if (len == (uint32_t)k_ra8_ov5640_i2c_frame_bytes) {
    const uint16_t reg = internal_decode_reg(data);
    s_mock.regs[reg]   = data[k_ra8_ov5640_i2c_reg_bytes];
  }
  return k_ra8_ok;
}

/**
 * @brief Seam plain-read half: counted only, never used by the binder.
 * @details Present so the seam is fully filled and its absence is provable.
 * @param[in] ctx Binder-owned cookie.
 * @param[in] addr 7-bit address.
 * @param[out] data Destination buffer.
 * @param[in] len Byte count.
 * @return Repository error code.
 * @retval k_ra8_ok Zero bytes were produced.
 * @pre `data` holds `len` writable bytes.
 * @post The plain-read counter is incremented once.
 * @note Test-only and not reentrant because the fixture has file scope.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t
internal_seam_read(void* ctx, uint8_t addr, uint8_t* data, uint32_t len)
{
  (void)ctx;
  (void)addr;
  s_mock.plain_read_count++;
  for (uint32_t i = 0U; i < len; ++i) {
    data[i] = 0U;
  }
  return k_ra8_ok;
}

/**
 * @brief Seam transfer half: record the write-RESTART-read and answer it.
 * @details Decodes the staged register pointer and returns the fixture byte.
 * @param[in] ctx Binder-owned cookie.
 * @param[in] addr 7-bit address.
 * @param[in] wr Write-half bytes.
 * @param[in] wr_len Write-half length.
 * @param[out] rd Read-half destination.
 * @param[in] rd_len Read-half length.
 * @return Repository error code.
 * @retval k_ra8_ok The transfer was recorded and answered.
 * @pre `wr` and `rd` hold their stated byte counts.
 * @post The transfer counter is incremented once.
 * @note Test-only and not reentrant because the fixture has file scope.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_seam_transfer(void*          ctx,
                                                     uint8_t        addr,
                                                     const uint8_t* wr,
                                                     uint32_t       wr_len,
                                                     uint8_t*       rd,
                                                     uint32_t       rd_len)
{
  (void)ctx;
  s_mock.xfer_count++;
  s_mock.last_xfer =
    (ov5640_bind_record_t){.wr_len = wr_len, .rd_len = rd_len, .address = addr, .send_stop = true};
  for (uint32_t i = 0U; (i < wr_len) && (i < (uint32_t)k_bind_frame_capacity); ++i) {
    s_mock.last_xfer.frame[i] = wr[i];
  }
  uint16_t reg = 0U;
  if (wr_len == (uint32_t)k_ra8_ov5640_i2c_reg_bytes) {
    reg = internal_decode_reg(wr);
  }
  for (uint32_t i = 0U; i < rd_len; ++i) {
    rd[i] = s_mock.regs[reg];
  }
  return k_ra8_ok;
}

/**
 * @brief Count a delay request from the bound sensor.
 * @details The seam is transfer-only, so the delay stays a separate callback.
 * @param[in] ctx Binder-owned cookie.
 * @param[in] milliseconds Requested wait.
 * @pre The fixture is exclusively owned by the current test vector.
 * @post The delay counter is incremented once.
 * @note Test-only and not reentrant because the fixture has file scope.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_delay(void* ctx, uint32_t milliseconds)
{
  (void)ctx;
  (void)milliseconds;
  s_mock.delay_count++;
}

/**
 * @brief Build a fully-filled house seam over the fixture.
 * @details All three callbacks are filled so a vector can null one at a time.
 * @return Seam value referencing the file-scope fixture.
 * @pre The fixture is exclusively owned by the current test vector.
 * @post No transaction has occurred.
 * @note Test-only and not reentrant because the fixture has file scope.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_i2c_bus_ops_t internal_make_ops(void)
{
  return (ra8_i2c_bus_ops_t){.write    = internal_seam_write,
                             .read     = internal_seam_read,
                             .transfer = internal_seam_transfer,
                             .ctx      = &s_mock};
}

/**
 * @brief `ra8_ov5640_bind_i2c` rejects every malformed argument.
 * @details Walks the six NULL conditions, which are its whole refusal surface.
 * @pre The fixture is exclusively owned by the current test vector.
 * @post Nothing reached the seam.
 * @note Test-only and not reentrant because the fixture has file scope.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_bind_validates_inputs(void)
{
  internal_mock_reset();
  TEST_BEGIN("ov5640_bind: validates inputs");
  ra8_ov5640_t            dev = {};
  ra8_ov5640_i2c_ctx_t    ctx = {};
  const ra8_i2c_bus_ops_t ops = internal_make_ops();

  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_ov5640_bind_i2c(nullptr, &ctx, &ops, internal_delay));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_ov5640_bind_i2c(&dev, nullptr, &ops, internal_delay));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_ov5640_bind_i2c(&dev, &ctx, nullptr, internal_delay));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_ov5640_bind_i2c(&dev, &ctx, &ops, nullptr));

  ra8_i2c_bus_ops_t no_write = ops;
  no_write.write             = nullptr;
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_ov5640_bind_i2c(&dev, &ctx, &no_write, internal_delay));

  ra8_i2c_bus_ops_t no_xfer = ops;
  no_xfer.transfer          = nullptr;
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_ov5640_bind_i2c(&dev, &ctx, &no_xfer, internal_delay));

  TEST_ASSERT_EQ(0U, s_mock.write_count);
  TEST_ASSERT_EQ(0U, s_mock.xfer_count);
  TEST_END("ov5640_bind: validates inputs");
}

/**
 * @brief A bound read becomes one write-RESTART-read of the register pointer.
 * @details Plants a byte behind the seam and proves the binder addresses it
 *          with a two-byte big-endian pointer and a one-byte read half, on the
 *          sensor's own address, without touching the plain read callback.
 * @pre The fixture is exclusively owned by the current test vector.
 * @post Exactly one transfer reached the fixture.
 * @note Test-only and not reentrant because the fixture has file scope.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_bind_read_uses_transfer(void)
{
  internal_mock_reset();
  TEST_BEGIN("ov5640_bind: bound read is one write-RESTART-read");
  s_mock.regs[k_bind_probe_reg] = (uint8_t)k_bind_chip_id_hi_byte;

  ra8_ov5640_t            dev = {};
  ra8_ov5640_i2c_ctx_t    ctx = {};
  const ra8_i2c_bus_ops_t ops = internal_make_ops();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ov5640_bind_i2c(&dev, &ctx, &ops, internal_delay));

  uint8_t value = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ov5640_read_reg(&dev, (uint16_t)k_bind_probe_reg, &value));
  TEST_ASSERT_EQ(k_bind_chip_id_hi_byte, value);

  TEST_ASSERT_EQ(1U, s_mock.xfer_count);
  TEST_ASSERT_EQ(0U, s_mock.write_count);
  TEST_ASSERT_EQ(0U, s_mock.plain_read_count);
  TEST_ASSERT_EQ(k_ra8_ov5640_addr_primary, s_mock.last_xfer.address);
  TEST_ASSERT_EQ(k_ra8_ov5640_i2c_reg_bytes, s_mock.last_xfer.wr_len);
  TEST_ASSERT_EQ(1U, s_mock.last_xfer.rd_len);
  TEST_ASSERT_EQ(k_bind_probe_reg_hi, s_mock.last_xfer.frame[0]);
  TEST_ASSERT_EQ(k_bind_probe_reg_lo, s_mock.last_xfer.frame[1]);
  TEST_END("ov5640_bind: bound read is one write-RESTART-read");
}

/**
 * @brief A bound write becomes one framed `[reg_hi][reg_lo][value]` with STOP.
 * @details Proves the staged frame, its length, the STOP flag, and that the
 *          value lands where a following read finds it.
 * @pre The fixture is exclusively owned by the current test vector.
 * @post Exactly one framed write reached the fixture.
 * @note Test-only and not reentrant because the fixture has file scope.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_bind_write_frames_register(void)
{
  internal_mock_reset();
  TEST_BEGIN("ov5640_bind: bound write is one framed register write");
  ra8_ov5640_t            dev = {};
  ra8_ov5640_i2c_ctx_t    ctx = {};
  const ra8_i2c_bus_ops_t ops = internal_make_ops();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ov5640_bind_i2c(&dev, &ctx, &ops, internal_delay));

  TEST_ASSERT_EQ(
    k_ra8_ok,
    ra8_ov5640_write_reg(&dev, (uint16_t)k_bind_probe_reg, (uint8_t)k_bind_write_value));

  TEST_ASSERT_EQ(1U, s_mock.write_count);
  TEST_ASSERT_EQ(0U, s_mock.xfer_count);
  TEST_ASSERT_EQ(k_ra8_ov5640_i2c_frame_bytes, s_mock.last_write.wr_len);
  TEST_ASSERT_EQ(true, s_mock.last_write.send_stop);
  TEST_ASSERT_EQ(k_ra8_ov5640_addr_primary, s_mock.last_write.address);
  TEST_ASSERT_EQ(k_bind_probe_reg_hi, s_mock.last_write.frame[0]);
  TEST_ASSERT_EQ(k_bind_probe_reg_lo, s_mock.last_write.frame[1]);
  TEST_ASSERT_EQ(k_bind_write_value, s_mock.last_write.frame[2]);

  uint8_t value = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ov5640_read_reg(&dev, (uint16_t)k_bind_probe_reg, &value));
  TEST_ASSERT_EQ(k_bind_write_value, value);
  TEST_END("ov5640_bind: bound write is one framed register write");
}

/**
 * @brief The binder leaves the wire untouched and the sensor probe-ready.
 * @details Binding alone must issue nothing, and the probe that follows must
 *          reach the sensor through the single binding.
 * @pre The fixture is exclusively owned by the current test vector.
 * @post The sensor has been probed through the bound seam.
 * @note Test-only and not reentrant because the fixture has file scope.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_bind_is_quiet_then_probes(void)
{
  internal_mock_reset();
  TEST_BEGIN("ov5640_bind: bind is quiet and the sensor then probes");
  ra8_ov5640_t            dev = {};
  ra8_ov5640_i2c_ctx_t    ctx = {};
  const ra8_i2c_bus_ops_t ops = internal_make_ops();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ov5640_bind_i2c(&dev, &ctx, &ops, internal_delay));

  TEST_ASSERT_EQ(0U, s_mock.write_count);
  TEST_ASSERT_EQ(0U, s_mock.xfer_count);
  TEST_ASSERT_EQ(0U, s_mock.delay_count);

  s_mock.regs[k_bind_probe_reg]      = (uint8_t)k_bind_chip_id_hi_byte;
  s_mock.regs[k_bind_chip_id_lo_reg] = (uint8_t)k_bind_chip_id_lo_byte;
  uint16_t id                        = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ov5640_probe(&dev, &id));
  TEST_ASSERT_EQ(k_ra8_ov5640_chip_id, id);
  TEST_ASSERT_EQ(0U, s_mock.plain_read_count);
  TEST_END("ov5640_bind: bind is quiet and the sensor then probes");
}

/**
 * @brief Run every OV5640 house-seam binder vector.
 * @details Keeps the executable entry point to one dispatch call.
 * @pre Unity test accounting is initialized.
 * @post Every binder vector has executed once.
 * @note Test-only; the fixture has file scope so the vectors run in order.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_ov5640_bind(void)
{
  internal_test_bind_validates_inputs();
  internal_test_bind_read_uses_transfer();
  internal_test_bind_write_frames_register();
  internal_test_bind_is_quiet_then_probes();
}

int main(void)
{
  internal_test_ov5640_bind();
  return 0;
}
