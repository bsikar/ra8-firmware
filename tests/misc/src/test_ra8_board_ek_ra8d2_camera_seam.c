/**
 * @file test_ra8_board_ek_ra8d2_camera_seam.c
 * @brief Host vectors for the EK-RA8D2 camera bus published as a house seam.
 *
 * @details
 * `ra8_board_camera_i2c_ops` is the board's answer to four copies of one SCCB
 * adapter: rather than every camera app packing its own 16-bit register
 * pointer, the board publishes RIIC1 through ::ra8_i2c_bus_ops_t and a sensor
 * binder consumes it. These vectors pin the two things a caller depends on,
 * namely that a null destination is refused and that a successful call leaves
 * no callback empty, because a half-filled seam would fail later inside the
 * sensor rather than here.
 *
 * The transfer semantics themselves belong to the RIIC backend and are
 * covered by `test_ra8_io_i2c_bus.c`. What this file adds on top of the
 * publication checks is the equivalence vector: the seam path, driven all
 * the way from `ra8_board_camera_i2c_ops` through `ra8_ov5640_bind_i2c`,
 * must put exactly the bytes on RIIC1 that the board's own SCCB helper
 * put there. Three merged slices asserted that in prose; this proves it
 * against the fake register window, so the helpers can be retired without
 * taking the only byte-level evidence with them.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>

#include "ra8_attributes.h"
#include "ra8_board_ek_ra8d2.h"
#include "ra8_err.h"
#include "ra8_fake_mmap.h"
#include "ra8_fake_mmio.h"
#include "ra8_i2c.h"
#include "ra8_i2c_bus_ops.h"
#include "ra8_i2c_regs.h"
#include "ra8_mstp.h"
#include "ra8_ov5640.h"
#include "ra8_pin_validator.h"
#include "unity_minimal.h"

/**
 * @brief Bus parameters and the transaction the equivalence vector replays.
 *
 * @details
 * The register and value are the ones `test_ra8_ceu_config.c` already uses
 * for the board SCCB helper, deliberately: the whole point is that both
 * paths emit the same bytes, so the vectors have to ask for the same
 * transaction. PCLKB matches the other board cases, so the bit-rate
 * divider lands identically.
 *
 * @invariant k_seam_reg == ((k_seam_reg_hi << 8) | k_seam_reg_lo).
 * @invariant k_seam_addr fits seven bits.
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_seam_pclkb_hz  = 50000000U, /**< 50 MHz PCLKB, as the board cases use. */
  k_seam_addr      = 0x3CU,     /**< Primary OV5640 SCCB address.          */
  k_seam_reg_hi    = 0x30U,     /**< High byte of the addressed register.  */
  k_seam_reg_lo    = 0x0AU,     /**< Low byte of the addressed register.   */
  k_seam_value     = 0x5AU,     /**< Value written to that register.       */
  k_seam_rx_byte   = 0xC3U,     /**< Byte staged in ICDRR for the read.    */
  k_seam_trace_cap = 8U,        /**< Capacity of the SCCB byte trace.      */
  k_seam_addr_wr   = 0x78U,     /**< k_seam_addr << 1, the write address.  */
  k_seam_trace_min = 4U,        /**< Address plus both pointer bytes.      */
} seam_wire_param_t;

/** @brief Register the equivalence vector addresses. */
typedef enum : uint16_t {
  k_seam_reg = 0x300AU, /**< OV5640 chip-id high byte. */
} seam_wire_register_t;

/** @brief Distinct consecutive ICDRT values observed on the camera bus. */
static uint8_t s_seam_trace[k_seam_trace_cap];

/** @brief Number of entries recorded in ::s_seam_trace. */
static uint8_t s_seam_trace_len;

/**
 * @brief Record each new byte the driver stages in the SCCB transmit register.
 * @details Runs inline on the driver's bounded status poll, before the byte
 *          for that poll is written, so equal consecutive samples fold away
 *          and what is left is the transmitted sequence.
 * @pre RIIC1 registers are mapped.
 * @pre The trace was cleared for the case being run.
 * @post A byte differing from the previous sample is appended, within capacity.
 * @post No register is modified.
 * @note Test-only and not thread-safe; the suite is single-threaded.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_seam_trace_hook(void)
{
  volatile const r_i2c_regs_t* reg = ra8_i2c_regs((uint8_t)k_ra8_board_camera_i2c_channel);
  if (reg == nullptr) {
    return;
  }
  if (s_seam_trace_len >= (uint8_t)k_seam_trace_cap) {
    return;
  }
  /* HUM Ch 39.2.17 "ICDRT : I2C Bus Transmit Data Register" p 2393 */
  const uint8_t byte = reg->ICDRT;
  if ((s_seam_trace_len != 0U) && (s_seam_trace[s_seam_trace_len - 1U] == byte)) {
    return;
  }
  s_seam_trace[s_seam_trace_len] = byte;
  s_seam_trace_len += 1U;
}

/**
 * @brief Restore every hosted service the camera bus touches.
 * @details Zeroes the register window, disarms the fault seam and the trace
 *          hook, frees pin claims and reinitialises the module-stop model, so
 *          one case cannot inherit a claim or a latched status flag.
 * @pre No board operation is in flight.
 * @post No pin is claimed and the byte trace is empty.
 * @note Test-only and not thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_seam_prep(void)
{
  ra8_fake_mmap_reset();
  ra8_fake_mmio_reset();
  ra8_pin_validator_reset();
  (void)ra8_mstp_init();
  s_seam_trace_len = 0U;
}

/**
 * @brief Bring RIIC1 up and stage the flags every SCCB byte waits on.
 * @details `ra8_i2c_init` leaves ICSR2 alone and the per-transfer status
 *          clear preserves TDRE, TEND and RDRF, so staging them once lets a
 *          whole address-plus-data sequence run without a bounded-wait expiry.
 * @pre The register window was reset for this case.
 * @post RIIC1 is enabled and ICSR2 reports transmit-empty, transmit-end and
 *       receive-full.
 * @note Test-only and not thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_seam_bus_up(void)
{
  const ra8_i2c_cfg_t cfg = {
    .bus_hz   = (uint32_t)k_ra8_i2c_speed_standard,
    .pclkb_hz = (uint32_t)k_seam_pclkb_hz,
  };
  TEST_ASSERT_EQ(k_ra8_ok, ra8_i2c_init((uint8_t)k_ra8_board_camera_i2c_channel, &cfg));
  volatile r_i2c_regs_t* reg = ra8_i2c_regs((uint8_t)k_ra8_board_camera_i2c_channel);
  /* HUM Ch 39.2.10 "ICSR2 : I2C Bus Status Register 2" p 2384 */
  reg->ICSR2 = (uint8_t)((uint8_t)k_ra8_i2c_msk_icsr2_tdre | (uint8_t)k_ra8_i2c_msk_icsr2_tend |
                         (uint8_t)k_ra8_i2c_msk_icsr2_rdrf);
}

/**
 * @brief Bind a sensor through the published seam, as a camera app does.
 * @details The exact three-call sequence every converted app now runs, so
 *          the vector exercises the shipped path rather than a shortcut.
 * @param[out] dev Sensor instance to initialise.
 * @param[out] ctx Binding state; must out-live @p dev.
 * @pre RIIC1 is up.
 * @post On return the sensor is bound and nothing has reached the wire.
 * @note Test-only and not thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_seam_bind(ra8_ov5640_t* dev, ra8_ov5640_i2c_ctx_t* ctx)
{
  ra8_i2c_bus_ops_t ops = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_board_camera_i2c_ops(&ops));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ov5640_bind_i2c(dev, ctx, &ops, ra8_board_camera_delay_ms));
}

/**
 * @brief The publisher refuses a null destination.
 * @details Its only refusal case, and the one a caller can trip.
 * @pre None.
 * @post No bus handle was bound.
 * @note Test-only.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_seam_rejects_null(void)
{
  TEST_BEGIN("board.camera: i2c_ops rejects a null destination");
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_board_camera_i2c_ops(nullptr));
  TEST_END("board.camera: i2c_ops rejects a null destination");
}

/**
 * @brief A successful publication leaves no callback empty.
 * @details A seam missing `transfer` binds fine and then fails on the first
 *          register read, so the completeness check belongs here.
 * @pre None.
 * @post The board camera bus handle is bound.
 * @note Test-only.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_seam_is_complete(void)
{
  TEST_BEGIN("board.camera: i2c_ops publishes a complete seam");
  ra8_i2c_bus_ops_t ops = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_board_camera_i2c_ops(&ops));
  TEST_ASSERT_NOT_NULL(ops.write);
  TEST_ASSERT_NOT_NULL(ops.read);
  TEST_ASSERT_NOT_NULL(ops.transfer);
  TEST_ASSERT_NOT_NULL(ops.ctx);
  TEST_END("board.camera: i2c_ops publishes a complete seam");
}

/**
 * @brief Republishing is idempotent and keeps one board-owned handle.
 * @details Two apps in one image may each ask for the seam; both must get the
 *          same context rather than a second bus.
 * @pre None.
 * @post The board camera bus handle is bound.
 * @note Test-only.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_seam_is_stable(void)
{
  TEST_BEGIN("board.camera: i2c_ops republishes the same bus");
  ra8_i2c_bus_ops_t first  = {};
  ra8_i2c_bus_ops_t second = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_board_camera_i2c_ops(&first));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_board_camera_i2c_ops(&second));
  TEST_ASSERT_EQ(first.ctx, second.ctx);
  TEST_ASSERT_EQ(first.transfer, second.transfer);
  TEST_END("board.camera: i2c_ops republishes the same bus");
}

/**
 * @brief A register write through the seam emits the board helper's bytes.
 * @details The address byte, then the register pointer high byte, then its
 *          low byte, with the value left in the transmit register. These are
 *          the same assertions `test_ra8_ceu_config.c` makes against
 *          `ra8_board_camera_sccb_write_reg`, which is the point: the two
 *          paths are interchangeable on the wire.
 * @pre RIIC1 is up with its transmit flags staged.
 * @post The traced bytes match the SCCB write encoding.
 * @note Test-only and not thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_seam_write_matches_sccb(void)
{
  TEST_BEGIN("board.camera: a seam write emits the SCCB byte sequence");
  internal_seam_prep();
  internal_seam_bus_up();

  ra8_ov5640_t         dev = {};
  ra8_ov5640_i2c_ctx_t ctx = {};
  internal_seam_bind(&dev, &ctx);

  ra8_fake_mmio_set_poll_hook(internal_seam_trace_hook);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ov5640_write_reg(&dev, (uint16_t)k_seam_reg, (uint8_t)k_seam_value));
  ra8_fake_mmio_set_poll_hook(nullptr);

  volatile const r_i2c_regs_t* reg = ra8_i2c_regs((uint8_t)k_ra8_board_camera_i2c_channel);
  TEST_ASSERT(s_seam_trace_len >= (uint8_t)k_seam_trace_min);
  TEST_ASSERT_EQ(k_seam_addr_wr, s_seam_trace[1]);
  TEST_ASSERT_EQ(k_seam_reg_hi, s_seam_trace[2]);
  TEST_ASSERT_EQ(k_seam_reg_lo, s_seam_trace[3]);
  /* HUM Ch 39.2.17 "ICDRT : I2C Bus Transmit Data Register" p 2393 */
  TEST_ASSERT_EQ(k_seam_value, reg->ICDRT);
  TEST_END("board.camera: a seam write emits the SCCB byte sequence");
}

/**
 * @brief A register read through the seam repeats the same pointer bytes.
 * @details The read is one write-RESTART-read, so the pointer goes out
 *          exactly as it does for a write and the staged byte comes back.
 * @pre RIIC1 is up with its transmit and receive flags staged.
 * @post The traced pointer matches and the received byte reaches the caller.
 * @note Test-only and not thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_seam_read_matches_sccb(void)
{
  TEST_BEGIN("board.camera: a seam read repeats the SCCB pointer bytes");
  internal_seam_prep();
  internal_seam_bus_up();

  ra8_ov5640_t         dev = {};
  ra8_ov5640_i2c_ctx_t ctx = {};
  internal_seam_bind(&dev, &ctx);

  volatile r_i2c_regs_t* rx = ra8_i2c_regs((uint8_t)k_ra8_board_camera_i2c_channel);
  /* HUM Ch 39.2.18 "ICDRR : I2C Bus Receive Data Register" p 2393 */
  rx->ICDRR = (uint8_t)k_seam_rx_byte;

  uint8_t value = 0U;
  ra8_fake_mmio_set_poll_hook(internal_seam_trace_hook);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ov5640_read_reg(&dev, (uint16_t)k_seam_reg, &value));
  ra8_fake_mmio_set_poll_hook(nullptr);

  TEST_ASSERT_EQ(k_seam_rx_byte, value);
  TEST_ASSERT(s_seam_trace_len >= (uint8_t)k_seam_trace_min);
  TEST_ASSERT_EQ(k_seam_addr_wr, s_seam_trace[1]);
  TEST_ASSERT_EQ(k_seam_reg_hi, s_seam_trace[2]);
  TEST_ASSERT_EQ(k_seam_reg_lo, s_seam_trace[3]);
  TEST_END("board.camera: a seam read repeats the SCCB pointer bytes");
}

int main(void)
{
  internal_test_seam_rejects_null();
  internal_test_seam_is_complete();
  internal_test_seam_is_stable();
  internal_test_seam_write_matches_sccb();
  internal_test_seam_read_matches_sccb();
  return 0;
}
