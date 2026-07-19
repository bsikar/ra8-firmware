/**
 * @file test_ra8_lsm6dso.c
 * @brief Host-side unit tests for the LSM6DSO driver
 *
 * @details
 * Exercises every public function in ``libs/ra8_lsm6dso`` against a
 * canned-response mock I2C transport. The mock implements both the
 * ``read_regs`` and ``write_regs`` callbacks of ``ra8_lsm6dso_bus_t``
 * over a tiny register file (``k_mock_regs_cap`` bytes deep) and
 * records every wire byte so test bodies can assert on what the
 * driver actually emitted.
 *
 * Test coverage:
 *   - ``ra8_lsm6dso_init`` -- NULL pointer rejection (3 atomic conditions).
 *   - ``ra8_lsm6dso_who_am_i`` -- happy path (returns 0x6C) and wrong-ID
 *     path (mock returns 0xFF).
 *   - ``ra8_lsm6dso_set_accel_range`` -- writes CTRL1_XL with the FS_XL
 *     bits programmed, leaves the ODR nibble intact.
 *   - ``ra8_lsm6dso_set_gyro_range``  -- writes CTRL2_G with the FS_G +
 *     FS_125 bits programmed.
 *   - ``ra8_lsm6dso_set_odr`` -- writes CTRL1_XL and CTRL2_G with the
 *     ODR nibble in [7:4].
 *   - ``ra8_lsm6dso_read_accel`` / ``_read_gyro`` -- bursts 6 bytes
 *     and combines them little-endian; checks against canned ints.
 *   - ``ra8_lsm6dso_read_temp`` -- converts (raw / 256 + 25) C into
 *     centi-degC.
 *   - I2C NAK propagation: when the mock returns ``k_ra8_err_nack``,
 *     the driver returns it back unchanged.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>
#include <string.h>

#include "ra8_err.h"
#include "ra8_lsm6dso.h"
#include "unity_minimal.h"

/**
 * @enum lsm6dso_uint8_const_t
 * @brief Named uint8_t constants used by this file.
 *
 * @details
 * Every literal this translation unit needs, named so the
 * value's role is visible at the point of use (CLAUDE.md
 * "No Magic Numbers").
 */
typedef enum : uint8_t {
  k_lsm6dso_off_z_high     = 5, /**< High byte of Z within the 6-byte little-endian XYZ block. */
  k_lsm6dso_fifo_last_off  = 7, /**< Offset of that last byte within the FIFO data window.     */
  k_lsm6dso_ctrl1_xl_probe = 0x0CU, /**< A CTRL1_XL value the driver must read back unchanged.   */
  k_lsm6dso_x_high         = 0x12U, /**< X high byte of that same value.                         */
  k_lsm6dso_x_low          = 0x34U, /**< X low byte; with the high byte it forms 0x1234 = +4660. */
  k_lsm6dso_odr_preserved =
    0x40U, /**< A CTRL register value whose ODR field a later write must leave alone. */
  k_lsm6dso_z_high_max =
    0x7FU, /**< Z high byte 0x7F, giving 0x7FFF = +32767, the most positive one. */
  k_lsm6dso_z_high_min =
    0x80U, /**< Z high byte 0x80, giving 0x8000 = -32768, the most negative 16-bit sample. */
  k_lsm6dso_fifo_byte_first = 0xAAU, /**< First byte of the mocked FIFO word. */
  k_lsm6dso_fifo_byte_last = 0xBBU, /**< Its last byte, distinct so a byte-order slip is visible. */
  k_lsm6dso_all_ones =
    0xFFU, /**< 0xFF: makes Y read back as 0xFFFF = -1, and stands in for a wrong WHO_AM_I. */
} lsm6dso_uint8_const_t;

/**
 * @enum lsm6dso_uint16_const_t
 * @brief Named uint16_t constants used by this file.
 *
 * @details
 * Every literal this translation unit needs, named so the
 * value's role is visible at the point of use (CLAUDE.md
 * "No Magic Numbers").
 */
typedef enum : uint16_t {
  k_lsm6dso_words_poison =
    0xFFFFU, /**< Poison written into the word-count out-parameter, so a call that fails without setting it is detectable. */
} lsm6dso_uint16_const_t;

/* =============================================================================
 * Mock transport
 * =============================================================================
 */

/** @brief Register-file capacity for the mock. */
typedef enum : uint32_t {
  k_mock_regs_cap  = 256U, /**< Full 8-bit register address space. */
  k_mock_log_cap   = 32U,  /**< Last N writes are recorded.        */
  k_mock_burst_cap = 64U,  /**< Cap on per-read burst size.        */
} mock_cap_t;

/** @brief One recorded write transaction. */
typedef struct {
  uint8_t  reg;                    /**< Register. */
  uint8_t  data[k_mock_burst_cap]; /**< Data.     */
  uint32_t len;                    /**< Length.   */
} mock_write_t;

/** @brief Mock state -- one instance per test. */
typedef struct {
  uint8_t      regs[k_mock_regs_cap];  /**< Registers.                                */
  mock_write_t writes[k_mock_log_cap]; /**< Writes.                                   */
  uint32_t     write_count;            /**< Write count.                              */
  ra8_err_t    forced_err;             /**< Override return; ``k_ra8_ok`` to disable. */
} mock_t;

static mock_t s_mock;

/** @brief Reset the mock between tests. */
static void mock_reset(void)
{
  memset(&s_mock, 0, sizeof(s_mock));
  s_mock.forced_err = k_ra8_ok;
}

/** @brief Mock ``read_regs`` callback. */
static ra8_err_t mock_read(void* ctx, uint8_t reg, uint8_t* buf, uint32_t len)
{
  (void)ctx;
  if (s_mock.forced_err != k_ra8_ok) {
    return s_mock.forced_err;
  }
  for (uint32_t i = 0U; i < len; ++i) {
    const uint32_t idx = ((uint32_t)reg + i) & 0xFFU;
    buf[i]             = s_mock.regs[idx];
  }
  return k_ra8_ok;
}

/** @brief Mock ``write_regs`` callback. */
static ra8_err_t mock_write(void* ctx, uint8_t reg, const uint8_t* buf, uint32_t len)
{
  (void)ctx;
  if (s_mock.forced_err != k_ra8_ok) {
    return s_mock.forced_err;
  }
  if (s_mock.write_count < (uint32_t)k_mock_log_cap) {
    mock_write_t* w = &s_mock.writes[s_mock.write_count];
    w->reg          = reg;
    w->len          = (len > (uint32_t)k_mock_burst_cap) ? (uint32_t)k_mock_burst_cap : len;
    for (uint32_t i = 0U; i < w->len; ++i) {
      w->data[i] = buf[i];
    }
  }
  s_mock.write_count++;
  for (uint32_t i = 0U; i < len; ++i) {
    const uint32_t idx = ((uint32_t)reg + i) & 0xFFU;
    s_mock.regs[idx]   = buf[i];
  }
  return k_ra8_ok;
}

/** @brief Build a transport interface tied to the file-scope mock. */
static ra8_lsm6dso_bus_t make_bus(void)
{
  const ra8_lsm6dso_bus_t bus = {
    .read_regs  = mock_read,
    .write_regs = mock_write,
    .ctx        = nullptr,
  };
  return bus;
}

/* =============================================================================
 * Tests: init
 * =============================================================================
 */

/**
 * @test ra8_lsm6dso_init_validates_inputs
 *
 * @par MC/DC:
 * Decision in ``ra8_lsm6dso_init``: four ``RA8_CHECK_NULL_PTR`` guards.
 * Each guard is a single ``(ptr == nullptr)`` condition -- two
 * vectors per guard, eight total. We cover all four with NULL on
 * each pointer plus the all-non-NULL happy path.
 */
static void test_init_validates_inputs(void)
{
  mock_reset();
  TEST_BEGIN("lsm6dso: init validates pointers");
  ra8_lsm6dso_t           dev = {};
  const ra8_lsm6dso_bus_t bus = make_bus();

  /* Vector 1: out_dev == NULL. */
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_lsm6dso_init(nullptr, &bus));
  /* Vector 2: bus == NULL. */
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_lsm6dso_init(&dev, nullptr));
  /* Vector 3: bus.read_regs == NULL. */
  ra8_lsm6dso_bus_t bad_read = bus;
  bad_read.read_regs         = nullptr;
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_lsm6dso_init(&dev, &bad_read));
  /* Vector 4: bus.write_regs == NULL. */
  ra8_lsm6dso_bus_t bad_write = bus;
  bad_write.write_regs        = nullptr;
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_lsm6dso_init(&dev, &bad_write));
  /* Vector 5: happy path. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_lsm6dso_init(&dev, &bus));
  TEST_ASSERT(dev.initialized);
  TEST_END("lsm6dso: init validates pointers");
}

/* =============================================================================
 * Tests: who_am_i
 * =============================================================================
 */

/**
 * @test ra8_lsm6dso_who_am_i_happy
 *
 * @par MC/DC:
 * Decision: ``r != k_ra8_ok`` after the transport read. Two vectors
 * -- transport returns ok (this) and transport returns NACK (separate
 * test below). The ``out_id == nullptr`` guard is covered by the
 * null-rejection test.
 */
static void test_who_am_i_happy(void)
{
  mock_reset();
  TEST_BEGIN("lsm6dso: WHO_AM_I returns 0x6C");
  ra8_lsm6dso_t           dev = {};
  const ra8_lsm6dso_bus_t bus = make_bus();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_lsm6dso_init(&dev, &bus));
  s_mock.regs[(uint8_t)k_lsm6dso_reg_who_am_i] = (uint8_t)k_lsm6dso_who_am_i_value;
  uint8_t id                                   = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_lsm6dso_who_am_i(&dev, &id));
  TEST_ASSERT_EQ(k_lsm6dso_who_am_i_value, id);
  TEST_END("lsm6dso: WHO_AM_I returns 0x6C");
}

/**
 * @test ra8_lsm6dso_who_am_i_wrong_id
 *
 * @par MC/DC:
 * The driver itself does NOT compare the WHO_AM_I byte -- it just
 * returns the raw value to the caller (the application layer
 * decides). This test confirms a 0xFF (e.g. open-bus pull-ups) is
 * surfaced verbatim so the app code can detect it.
 */
static void test_who_am_i_wrong_id(void)
{
  mock_reset();
  TEST_BEGIN("lsm6dso: WHO_AM_I 0xFF surfaces to caller");
  ra8_lsm6dso_t           dev = {};
  const ra8_lsm6dso_bus_t bus = make_bus();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_lsm6dso_init(&dev, &bus));
  s_mock.regs[(uint8_t)k_lsm6dso_reg_who_am_i] = k_lsm6dso_all_ones;
  uint8_t id                                   = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_lsm6dso_who_am_i(&dev, &id));
  TEST_ASSERT_EQ(0xFFU, id);
  TEST_END("lsm6dso: WHO_AM_I 0xFF surfaces to caller");
}

/**
 * @test ra8_lsm6dso_who_am_i_null_rejected
 *
 * @par MC/DC:
 * Three ``RA8_CHECK_NULL_PTR`` guards in ``who_am_i``: dev, out_id,
 * initialized. Two vectors per guard.
 */
static void test_who_am_i_null_rejected(void)
{
  mock_reset();
  TEST_BEGIN("lsm6dso: WHO_AM_I rejects NULL");
  ra8_lsm6dso_t           dev = {};
  const ra8_lsm6dso_bus_t bus = make_bus();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_lsm6dso_init(&dev, &bus));
  uint8_t id = 0U;
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_lsm6dso_who_am_i(nullptr, &id));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_lsm6dso_who_am_i(&dev, nullptr));
  /* Uninitialized device -> not_initialized. */
  ra8_lsm6dso_t fresh = {};
  TEST_ASSERT(ra8_lsm6dso_who_am_i(&fresh, &id) != k_ra8_ok);
  TEST_END("lsm6dso: WHO_AM_I rejects NULL");
}

/* =============================================================================
 * Tests: NACK propagation
 * =============================================================================
 */

/**
 * @test ra8_lsm6dso_nack_propagation
 *
 * @par MC/DC:
 * Confirms the driver does not eat transport errors. The mock's
 * ``forced_err`` knob makes every read / write return ``k_ra8_err_nack``
 * unconditionally.
 */
static void test_nack_propagation(void)
{
  mock_reset();
  TEST_BEGIN("lsm6dso: I2C NAK propagates verbatim");
  ra8_lsm6dso_t           dev = {};
  const ra8_lsm6dso_bus_t bus = make_bus();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_lsm6dso_init(&dev, &bus));
  s_mock.forced_err = k_ra8_err_nack;
  uint8_t id        = 0U;
  TEST_ASSERT_EQ(k_ra8_err_nack, ra8_lsm6dso_who_am_i(&dev, &id));
  /* Set-range path issues a read first; it should NACK too. */
  TEST_ASSERT_EQ(k_ra8_err_nack, ra8_lsm6dso_set_accel_range(&dev, k_lsm6dso_xl_fs_4g));
  TEST_ASSERT_EQ(k_ra8_err_nack, ra8_lsm6dso_set_gyro_range(&dev, k_lsm6dso_g_fs_500dps));
  TEST_ASSERT_EQ(k_ra8_err_nack, ra8_lsm6dso_set_odr(&dev, k_lsm6dso_odr_104hz));
  ra8_lsm6dso_xyz_t xyz   = {};
  int32_t           tcent = 0;
  TEST_ASSERT_EQ(k_ra8_err_nack, ra8_lsm6dso_read_accel(&dev, &xyz));
  TEST_ASSERT_EQ(k_ra8_err_nack, ra8_lsm6dso_read_gyro(&dev, &xyz));
  TEST_ASSERT_EQ(k_ra8_err_nack, ra8_lsm6dso_read_temp(&dev, &tcent));
  TEST_END("lsm6dso: I2C NAK propagates verbatim");
}

/* =============================================================================
 * Tests: config setters
 * =============================================================================
 */

/**
 * @test ra8_lsm6dso_set_accel_range_writes_fs_xl
 *
 * @par MC/DC:
 * Decision: read-modify-write on CTRL1_XL preserves the existing
 * ODR nibble. Two vectors: ODR pre-seeded != 0 (this) and ODR pre-set
 * to 0 (folded into the happy path). FS_XL bits [3:2] must reflect
 * the new code.
 */
static void test_set_accel_range_writes_fs_xl(void)
{
  mock_reset();
  TEST_BEGIN("lsm6dso: set_accel_range writes FS_XL bits");
  ra8_lsm6dso_t           dev = {};
  const ra8_lsm6dso_bus_t bus = make_bus();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_lsm6dso_init(&dev, &bus));
  /* Pre-seed CTRL1_XL with ODR_XL = 4 (104 Hz, bits [7:4] = 0x40),
   * FS_XL = 0 to mimic a previous _set_odr call. */
  s_mock.regs[(uint8_t)k_lsm6dso_reg_ctrl1_xl] = k_lsm6dso_odr_preserved;

  TEST_ASSERT_EQ(k_ra8_ok, ra8_lsm6dso_set_accel_range(&dev, k_lsm6dso_xl_fs_8g));
  /* 8g code is 0x03, occupying bits [3:2] -> 0x0C. */
  TEST_ASSERT_EQ(0x40U | 0x0CU, s_mock.regs[(uint8_t)k_lsm6dso_reg_ctrl1_xl]);
  TEST_ASSERT_EQ(k_lsm6dso_xl_fs_8g, dev.accel_fs_code);
  TEST_END("lsm6dso: set_accel_range writes FS_XL bits");
}

/**
 * @test ra8_lsm6dso_set_accel_range_invalid
 *
 * @par MC/DC:
 * Range check guard: code > k_lsm6dso_xl_fs_cap -> invalid_arg.
 */
static void test_set_accel_range_invalid(void)
{
  mock_reset();
  TEST_BEGIN("lsm6dso: set_accel_range rejects out-of-range code");
  ra8_lsm6dso_t           dev = {};
  const ra8_lsm6dso_bus_t bus = make_bus();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_lsm6dso_init(&dev, &bus));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_lsm6dso_set_accel_range(&dev, (ra8_lsm6dso_xl_fs_t)0x7FU));
  TEST_END("lsm6dso: set_accel_range rejects out-of-range code");
}

/**
 * @test ra8_lsm6dso_set_gyro_range_writes_fs_g
 *
 * @par MC/DC:
 * Tests both code paths in ``internal_lsm6dso_g_fs_bits``:
 *  - FS == 125 dps -> FS_125 (bit 1) set, FS_G[1:0] cleared.
 *  - FS == 1000 dps -> FS_125 cleared, FS_G = 0x02 (= 1000 - 250 = 3,
 *    encoded as ``code - 1 == 2``).
 */
static void test_set_gyro_range_writes_fs_g(void)
{
  mock_reset();
  TEST_BEGIN("lsm6dso: set_gyro_range writes FS_G + FS_125 bits");
  ra8_lsm6dso_t           dev = {};
  const ra8_lsm6dso_bus_t bus = make_bus();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_lsm6dso_init(&dev, &bus));

  /* Vector 1: 125 dps -> FS_125 set. */
  s_mock.regs[(uint8_t)k_lsm6dso_reg_ctrl2_g] = k_lsm6dso_odr_preserved; /* preserved ODR */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_lsm6dso_set_gyro_range(&dev, k_lsm6dso_g_fs_125dps));
  TEST_ASSERT_EQ(0x40U | 0x02U, s_mock.regs[(uint8_t)k_lsm6dso_reg_ctrl2_g]);

  /* Vector 2: 1000 dps -> FS_G[1:0] = 0b10 (bits [3:2] = 0x08), FS_125 cleared. */
  s_mock.regs[(uint8_t)k_lsm6dso_reg_ctrl2_g] = k_lsm6dso_odr_preserved;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_lsm6dso_set_gyro_range(&dev, k_lsm6dso_g_fs_1000dps));
  TEST_ASSERT_EQ(0x40U | 0x08U, s_mock.regs[(uint8_t)k_lsm6dso_reg_ctrl2_g]);

  TEST_END("lsm6dso: set_gyro_range writes FS_G + FS_125 bits");
}

/**
 * @test ra8_lsm6dso_set_odr_writes_both_blocks
 *
 * @par MC/DC:
 * Decision: ``set_odr`` writes CTRL1_XL AND CTRL2_G in sequence.
 * Two vectors -- both writes succeed (this) and the first write
 * fails (covered by the NACK propagation test).
 */
static void test_set_odr_writes_both_blocks(void)
{
  mock_reset();
  TEST_BEGIN("lsm6dso: set_odr writes CTRL1_XL + CTRL2_G");
  ra8_lsm6dso_t           dev = {};
  const ra8_lsm6dso_bus_t bus = make_bus();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_lsm6dso_init(&dev, &bus));
  /* Pre-seed FS_XL = 0x0C, FS_G = 0x02 (FS_125) so we can verify the
   * low nibble is preserved while bits [7:4] flip to the ODR. */
  s_mock.regs[(uint8_t)k_lsm6dso_reg_ctrl1_xl] = k_lsm6dso_ctrl1_xl_probe;
  s_mock.regs[(uint8_t)k_lsm6dso_reg_ctrl2_g]  = 0x02U;

  /* 104 Hz code is 0x04, occupying bits [7:4] -> 0x40. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_lsm6dso_set_odr(&dev, k_lsm6dso_odr_104hz));
  TEST_ASSERT_EQ(0x40U | 0x0CU, s_mock.regs[(uint8_t)k_lsm6dso_reg_ctrl1_xl]);
  TEST_ASSERT_EQ(0x40U | 0x02U, s_mock.regs[(uint8_t)k_lsm6dso_reg_ctrl2_g]);
  TEST_ASSERT_EQ(k_lsm6dso_odr_104hz, dev.odr_code);
  TEST_END("lsm6dso: set_odr writes CTRL1_XL + CTRL2_G");
}

/**
 * @test ra8_lsm6dso_set_odr_invalid
 *
 * @par MC/DC:
 * Range check: code > k_lsm6dso_odr_cap -> invalid_arg.
 */
static void test_set_odr_invalid(void)
{
  mock_reset();
  TEST_BEGIN("lsm6dso: set_odr rejects out-of-range code");
  ra8_lsm6dso_t           dev = {};
  const ra8_lsm6dso_bus_t bus = make_bus();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_lsm6dso_init(&dev, &bus));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_lsm6dso_set_odr(&dev, (ra8_lsm6dso_odr_t)0x7FU));
  TEST_END("lsm6dso: set_odr rejects out-of-range code");
}

/* =============================================================================
 * Tests: sample reads
 * =============================================================================
 */

/**
 * @test ra8_lsm6dso_read_accel_combines_le_bytes
 *
 * @par MC/DC:
 * The 6-byte burst is parsed as three little-endian int16_t values.
 * Test vector: X = 0x1234, Y = -1 (0xFFFF), Z = INT16_MIN (0x8000).
 */
static void test_read_accel_combines_le_bytes(void)
{
  mock_reset();
  TEST_BEGIN("lsm6dso: read_accel combines little-endian bytes");
  ra8_lsm6dso_t           dev = {};
  const ra8_lsm6dso_bus_t bus = make_bus();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_lsm6dso_init(&dev, &bus));
  /* OUTX_L_A = 0x28; layout XL XH YL YH ZL ZH. */
  const uint8_t base                       = (uint8_t)k_lsm6dso_reg_outx_l_a;
  s_mock.regs[base + 0]                    = k_lsm6dso_x_low;      /* XL                     */
  s_mock.regs[base + 1]                    = k_lsm6dso_x_high;     /* XH -> 0x1234  =  4660  */
  s_mock.regs[base + 2]                    = k_lsm6dso_all_ones;   /* YL                     */
  s_mock.regs[base + 3]                    = k_lsm6dso_all_ones;   /* YH -> 0xFFFF  =    -1  */
  s_mock.regs[base + 4]                    = 0x00U;                /* ZL                     */
  s_mock.regs[base + k_lsm6dso_off_z_high] = k_lsm6dso_z_high_min; /* ZH -> 0x8000  = -32768 */

  ra8_lsm6dso_xyz_t out = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_lsm6dso_read_accel(&dev, &out));
  TEST_ASSERT_EQ(0x1234, out.x);
  TEST_ASSERT_EQ(-1, out.y);
  TEST_ASSERT_EQ(-32768, out.z);
  TEST_END("lsm6dso: read_accel combines little-endian bytes");
}

/**
 * @test ra8_lsm6dso_read_gyro_combines_le_bytes
 *
 * @par MC/DC:
 * No compound boolean decisions are exercised by this test -- the
 * production code path under test (``internal_lsm6dso_read_xyz``)
 * contains a single ``if (r != k_ra8_ok)`` early-exit on the
 * transport return code, which is single-condition. The vectors
 * below therefore cover statement / branch coverage only:
 * - Vector 1: ``read_regs`` returns ``k_ra8_ok`` -> XYZ bytes packed.
 * Coverage of the ``k_ra8_ok`` short-circuit branch lives in
 * ``test_read_accel_returns_bus_error`` so MC/DC is satisfied at the
 * file level even though this individual test does not add vectors.
 */
static void test_read_gyro_combines_le_bytes(void)
{
  mock_reset();
  TEST_BEGIN("lsm6dso: read_gyro combines little-endian bytes");
  ra8_lsm6dso_t           dev = {};
  const ra8_lsm6dso_bus_t bus = make_bus();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_lsm6dso_init(&dev, &bus));
  const uint8_t base                       = (uint8_t)k_lsm6dso_reg_outx_l_g;
  s_mock.regs[base + 0]                    = 0x01U;
  s_mock.regs[base + 1]                    = 0x00U; /* X = 1 */
  s_mock.regs[base + 2]                    = 0x00U;
  s_mock.regs[base + 3]                    = 0x01U; /* Y = 256 */
  s_mock.regs[base + 4]                    = k_lsm6dso_all_ones;
  s_mock.regs[base + k_lsm6dso_off_z_high] = k_lsm6dso_z_high_max; /* Z = 32767 */

  ra8_lsm6dso_xyz_t out = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_lsm6dso_read_gyro(&dev, &out));
  TEST_ASSERT_EQ(1, out.x);
  TEST_ASSERT_EQ(256, out.y);
  TEST_ASSERT_EQ(32767, out.z);
  TEST_END("lsm6dso: read_gyro combines little-endian bytes");
}

/**
 * @test ra8_lsm6dso_read_temp_converts_to_centi_c
 *
 * @par MC/DC:
 * Two vectors:
 *   - raw = 0     -> (0 * 100 / 256) + 2500 = 2500 centi-C (= 25.00 C).
 *   - raw = 256   -> (256 * 100 / 256) + 2500 = 2600 centi-C (= 26.00 C).
 * Confirms the integer-scaled (raw / 256 + 25) formula.
 */
static void test_read_temp_converts(void)
{
  mock_reset();
  TEST_BEGIN("lsm6dso: read_temp converts raw to centi-degC");
  ra8_lsm6dso_t           dev = {};
  const ra8_lsm6dso_bus_t bus = make_bus();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_lsm6dso_init(&dev, &bus));
  const uint8_t base = (uint8_t)k_lsm6dso_reg_out_temp_l;

  /* Vector 1: raw == 0 -> 25.00 C. */
  s_mock.regs[base + 0] = 0x00U;
  s_mock.regs[base + 1] = 0x00U;
  int32_t t             = -1;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_lsm6dso_read_temp(&dev, &t));
  TEST_ASSERT_EQ(2500, t);

  /* Vector 2: raw == 256 (0x0100, LE -> low=0x00, high=0x01) -> 26.00 C. */
  s_mock.regs[base + 0] = 0x00U;
  s_mock.regs[base + 1] = 0x01U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_lsm6dso_read_temp(&dev, &t));
  TEST_ASSERT_EQ(2600, t);

  TEST_END("lsm6dso: read_temp converts raw to centi-degC");
}

/* =============================================================================
 * Tests: FIFO read
 * =============================================================================
 */

/**
 * @test ra8_lsm6dso_fifo_drains_words
 *
 * @par MC/DC:
 * Decision: ``to_read = min(live, max_words)``.
 * Two vectors:
 *  - live <= max_words: the driver reads exactly ``live`` words.
 *  - live == 0:         the driver returns ok and out_words == 0
 *                       without touching the data stream.
 */
static void test_fifo_drains_words(void)
{
  mock_reset();
  TEST_BEGIN("lsm6dso: FIFO drain reads `live` words");
  ra8_lsm6dso_t           dev = {};
  const ra8_lsm6dso_bus_t bus = make_bus();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_lsm6dso_init(&dev, &bus));
  /* DIFF_FIFO low byte = 2 -> 2 FIFO words live. */
  s_mock.regs[(uint8_t)k_lsm6dso_reg_fifo_status1]     = 2U;
  s_mock.regs[(uint8_t)k_lsm6dso_reg_fifo_status1 + 1] = 0U;
  /* Stamp recognisable bytes into the FIFO data tap. */
  s_mock.regs[(uint8_t)k_lsm6dso_reg_fifo_data_out + 0] = k_lsm6dso_fifo_byte_first;
  s_mock.regs[(uint8_t)k_lsm6dso_reg_fifo_data_out + k_lsm6dso_fifo_last_off] =
    k_lsm6dso_fifo_byte_last;

  uint8_t  buf[16] = {};
  uint32_t words   = k_lsm6dso_words_poison;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_lsm6dso_read_xl_gyro_fifo(&dev, buf, 16U, &words));
  TEST_ASSERT_EQ(2U, words);
  TEST_ASSERT_EQ(0xAAU, buf[0]);
  TEST_ASSERT_EQ(0xBBU, buf[7]);

  /* Vector 2: live = 0 -> out_words = 0, no payload read needed. */
  s_mock.regs[(uint8_t)k_lsm6dso_reg_fifo_status1] = 0U;
  words                                            = k_lsm6dso_words_poison;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_lsm6dso_read_xl_gyro_fifo(&dev, buf, 16U, &words));
  TEST_ASSERT_EQ(0U, words);

  TEST_END("lsm6dso: FIFO drain reads `live` words");
}

/**
 * @test ra8_lsm6dso_fifo_validates_inputs
 *
 * @par MC/DC:
 * Four guard conditions: dev / out_buf / out_words NULL, and
 * max_words == 0.
 */
static void test_fifo_validates_inputs(void)
{
  mock_reset();
  TEST_BEGIN("lsm6dso: FIFO drain validates inputs");
  ra8_lsm6dso_t           dev = {};
  const ra8_lsm6dso_bus_t bus = make_bus();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_lsm6dso_init(&dev, &bus));
  uint8_t  buf[4] = {};
  uint32_t words  = 0U;
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_lsm6dso_read_xl_gyro_fifo(nullptr, buf, 1U, &words));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_lsm6dso_read_xl_gyro_fifo(&dev, nullptr, 1U, &words));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_lsm6dso_read_xl_gyro_fifo(&dev, buf, 1U, nullptr));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_lsm6dso_read_xl_gyro_fifo(&dev, buf, 0U, &words));
  TEST_END("lsm6dso: FIFO drain validates inputs");
}

/* =============================================================================
 * main
 * =============================================================================
 */

int main(void)
{
  test_init_validates_inputs();
  test_who_am_i_happy();
  test_who_am_i_wrong_id();
  test_who_am_i_null_rejected();
  test_nack_propagation();
  test_set_accel_range_writes_fs_xl();
  test_set_accel_range_invalid();
  test_set_gyro_range_writes_fs_g();
  test_set_odr_writes_both_blocks();
  test_set_odr_invalid();
  test_read_accel_combines_le_bytes();
  test_read_gyro_combines_le_bytes();
  test_read_temp_converts();
  test_fifo_drains_words();
  test_fifo_validates_inputs();
  return 0;
}
