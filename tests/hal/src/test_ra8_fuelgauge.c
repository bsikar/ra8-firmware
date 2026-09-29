/**
 * @file test_ra8_fuelgauge.c
 * @brief Unit tests for the MAX17048 fuel-gauge driver.
 *
 * @details
 * Drives ``ra8_fuelgauge`` against a file-local fake ``ra8_i2c_bus_ops_t``
 * seam holding a small MAX17048 register model, so the decoding, the
 * probe contract and the error propagation are exercised with no MMIO and
 * no board. The fake answers the part's real access shape: a one-byte
 * register pointer written, then two bytes read back MSB first.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stddef.h>
#include <stdint.h>

#include "ra8_attributes.h"
#include "ra8_err.h"
#include "ra8_fuelgauge.h"
#include "ra8_fuelgauge_max17048_regs.h"
#include "ra8_i2c_bus_ops.h"
#include "unity_minimal.h"

/**
 * @enum fuelgauge_fixture_t
 * @brief The recognizable values moved through the code under test.
 */
typedef enum : uint16_t {
  k_fg_version_live  = 0x0012U, /**< A plausible live VERSION reading.     */
  k_fg_version_dead  = 0x0000U, /**< Segment stuck low.                    */
  k_fg_version_float = 0xFFFFU, /**< Segment stuck high.                   */
  k_fg_soc_57pct     = 0x3980U, /**< SOC 0x39 = 57%, plus a 0.5% fraction. */
  k_fg_vcell_3v7     = 0xBC00U, /**< 48128 LSB * 5/64 = 3760 mV.           */
  k_fg_vcell_3v7_mv  = 3760U,   /**< Expected millivolts for the above.    */
  k_fg_crate_charge  = 0x0064U, /**< +100: charging.                       */
  k_fg_crate_dischg  = 0xFF9CU, /**< -100 two's complement: discharging.   */
  k_fg_crate_zero    = 0x0000U, /**< Exactly zero: counted as charging.    */
} fuelgauge_fixture_t;

/**
 * @enum fuelgauge_fixture_byte_t
 * @brief Byte-width fixture constants.
 */
typedef enum : uint8_t {
  k_fg_soc_expected_pct = 0x39U, /**< 57%, the high byte of k_fg_soc_57pct. */
  k_fg_addr_alt_7b      = 0x2AU, /**< A non-default address, to prove it is
                                      the address the driver actually uses. */
} fuelgauge_fixture_byte_t;

/**
 * @struct fake_bus_t
 * @brief File-local MAX17048 model behind the injected I2C seam.
 */
typedef struct {
  uint16_t  version;     /**< VERSION register contents.                */
  uint16_t  vcell;       /**< VCELL register contents.                  */
  uint16_t  soc;         /**< SOC register contents.                    */
  uint16_t  crate;       /**< CRATE register contents.                  */
  ra8_err_t fail_with;   /**< Non-ok to make every transfer fail.       */
  uint8_t   last_addr;   /**< Address seen by the most recent transfer. */
  uint8_t   last_reg;    /**< Register pointer seen most recently.      */
  uint32_t  transfers;   /**< Count of transfers performed.             */
} fake_bus_t;

/** @brief The single fake bus instance. */
static fake_bus_t s_bus;

/**
 * @brief Answer one MAX17048 register read from ::s_bus.
 *
 * @param[in]  ctx    Points at ::s_bus.
 * @param[in]  addr   7-bit address the driver selected.
 * @param[in]  wr     Register pointer bytes.
 * @param[in]  wr_len Number of pointer bytes (1 for this part).
 * @param[out] rd     Destination for the 16-bit register value.
 * @param[in]  rd_len Number of bytes to read (2 for this part).
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok Register answered.
 *
 * @pre ``ctx`` is non-NULL.
 * @post ::s_bus records the address and register pointer seen.
 * @note File-local helper; single-threaded test harness.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t fake_transfer(void*          ctx,
                               uint8_t        addr,
                               const uint8_t* wr,
                               uint32_t       wr_len,
                               uint8_t*       rd,
                               uint32_t       rd_len)
{
  fake_bus_t* bus = (fake_bus_t*)ctx;
  bus->transfers++;
  bus->last_addr = addr;

  if (bus->fail_with != k_ra8_ok) {
    return bus->fail_with;
  }
  if ((wr == NULL) || (wr_len != (uint32_t)k_ra8_fuelgauge_max17048_reg_ptr_bytes) ||
      (rd == NULL) || (rd_len != (uint32_t)k_ra8_fuelgauge_max17048_reg_bytes)) {
    return k_ra8_err_invalid_arg;
  }

  bus->last_reg  = wr[0];
  uint16_t value = 0U;
  switch (wr[0]) {
    case (uint8_t)k_ra8_fuelgauge_max17048_reg_version:
      value = bus->version;
      break;
    case (uint8_t)k_ra8_fuelgauge_max17048_reg_vcell:
      value = bus->vcell;
      break;
    case (uint8_t)k_ra8_fuelgauge_max17048_reg_soc:
      value = bus->soc;
      break;
    case (uint8_t)k_ra8_fuelgauge_max17048_reg_crate:
      value = bus->crate;
      break;
    default:
      return k_ra8_err_not_supported;
  }

  rd[0] = (uint8_t)(value >> 8U);
  rd[1] = (uint8_t)(value & 0xFFU);
  return k_ra8_ok;
}

/**
 * @brief Reset ::s_bus to a live gauge reporting 57%, 3760 mV, charging.
 *
 * @pre None.
 * @post ::s_bus holds the default fixture.
 * @note File-local helper; single-threaded test harness.
 * @since 0.1.0
 */
RA8_INTERNAL
static void internal_prep(void)
{
  const fake_bus_t fresh = {
    .version = (uint16_t)k_fg_version_live,
    .vcell   = (uint16_t)k_fg_vcell_3v7,
    .soc     = (uint16_t)k_fg_soc_57pct,
    .crate   = (uint16_t)k_fg_crate_charge,
  };
  s_bus = fresh;
}

/**
 * @brief Build a configuration bound to the fake bus.
 *
 * @param[in] addr_7b Address to bind.
 *
 * @return A configuration descriptor pointing at ::s_bus.
 *
 * @pre ::internal_prep has run.
 * @post No state is changed.
 * @note File-local helper; single-threaded test harness.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_fuelgauge_cfg_t internal_cfg(uint8_t addr_7b)
{
  const ra8_fuelgauge_cfg_t cfg = {
    .bus       = {.write = NULL, .read = NULL, .transfer = fake_transfer, .ctx = &s_bus},
    .target_7b = addr_7b,
  };
  return cfg;
}

/**
 * @test internal_test_open_probes_version
 * @brief Prove open reads VERSION at the configured address.
 *
 * @pre ::internal_prep has run.
 * @post The handle is open.
 * @note File-local test case; single-threaded harness.
 * @since 0.1.0
 */
RA8_INTERNAL
static void internal_test_open_probes_version(void)
{
  TEST_BEGIN("ra8_fuelgauge_open: probes VERSION at the bound address");
  internal_prep();
  ra8_fuelgauge_t           fg  = {};
  const ra8_fuelgauge_cfg_t cfg = internal_cfg((uint8_t)k_fg_addr_alt_7b);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fuelgauge_open(&fg, &cfg));
  TEST_ASSERT_EQ((uint8_t)k_ra8_fuelgauge_max17048_reg_version, s_bus.last_reg);
  TEST_ASSERT_EQ((uint8_t)k_fg_addr_alt_7b, s_bus.last_addr);
  TEST_ASSERT_EQ(1U, s_bus.transfers);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fuelgauge_close(&fg));
  TEST_END("ra8_fuelgauge_open: probes VERSION at the bound address");
}

/**
 * @test internal_test_open_bad_inputs
 * @brief Prove open rejects NULL arguments and an unfilled seam.
 *
 * @pre ::internal_prep has run.
 * @post No handle is opened.
 * @note File-local test case; single-threaded harness.
 * @since 0.1.0
 */
RA8_INTERNAL
static void internal_test_open_bad_inputs(void)
{
  TEST_BEGIN("ra8_fuelgauge_open: bad inputs rejected");
  internal_prep();
  ra8_fuelgauge_t           fg  = {};
  const ra8_fuelgauge_cfg_t cfg = internal_cfg((uint8_t)k_ra8_fuelgauge_default_addr_7b);
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_fuelgauge_open(nullptr, &cfg));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_fuelgauge_open(&fg, nullptr));

  ra8_fuelgauge_cfg_t unfilled = cfg;
  unfilled.bus.transfer        = NULL;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_fuelgauge_open(&fg, &unfilled));
  TEST_ASSERT_EQ(0U, s_bus.transfers);
  TEST_END("ra8_fuelgauge_open: bad inputs rejected");
}

/**
 * @test internal_test_open_dead_segment
 * @brief Prove the probe rejects an all-zero and an all-ones reading.
 *
 * @pre ::internal_prep has run.
 * @post No handle is opened.
 * @note File-local test case; single-threaded harness.
 * @since 0.1.0
 */
RA8_INTERNAL
static void internal_test_open_dead_segment(void)
{
  TEST_BEGIN("ra8_fuelgauge_open: dead segment rejected");
  internal_prep();
  ra8_fuelgauge_t           fg  = {};
  const ra8_fuelgauge_cfg_t cfg = internal_cfg((uint8_t)k_ra8_fuelgauge_default_addr_7b);

  s_bus.version = (uint16_t)k_fg_version_dead;
  TEST_ASSERT_EQ(k_ra8_err_hw_not_ready, ra8_fuelgauge_open(&fg, &cfg));

  s_bus.version = (uint16_t)k_fg_version_float;
  TEST_ASSERT_EQ(k_ra8_err_hw_not_ready, ra8_fuelgauge_open(&fg, &cfg));

  /* A rejected open leaves the handle closed, so read stays refused. */
  ra8_fuelgauge_state_t st = {};
  TEST_ASSERT_EQ(k_ra8_err_not_initialized, ra8_fuelgauge_read(&fg, &st));
  TEST_END("ra8_fuelgauge_open: dead segment rejected");
}

/**
 * @test internal_test_open_propagates_transport_error
 * @brief Prove a transport failure during the probe is forwarded verbatim.
 *
 * @pre ::internal_prep has run.
 * @post No handle is opened.
 * @note File-local test case; single-threaded harness.
 * @since 0.1.0
 */
RA8_INTERNAL
static void internal_test_open_propagates_transport_error(void)
{
  TEST_BEGIN("ra8_fuelgauge_open: transport error forwarded");
  internal_prep();
  s_bus.fail_with = k_ra8_err_nack;
  ra8_fuelgauge_t           fg  = {};
  const ra8_fuelgauge_cfg_t cfg = internal_cfg((uint8_t)k_ra8_fuelgauge_default_addr_7b);
  TEST_ASSERT_EQ(k_ra8_err_nack, ra8_fuelgauge_open(&fg, &cfg));
  TEST_END("ra8_fuelgauge_open: transport error forwarded");
}

/**
 * @test internal_test_read_decodes_sample
 * @brief Prove read decodes SOC, VCELL and CRATE from the wire.
 *
 * @pre ::internal_prep has run.
 * @post The handle is closed again.
 * @note File-local test case; single-threaded harness.
 * @since 0.1.0
 */
RA8_INTERNAL
static void internal_test_read_decodes_sample(void)
{
  TEST_BEGIN("ra8_fuelgauge_read: decodes SOC, VCELL and CRATE");
  internal_prep();
  ra8_fuelgauge_t           fg  = {};
  const ra8_fuelgauge_cfg_t cfg = internal_cfg((uint8_t)k_ra8_fuelgauge_default_addr_7b);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fuelgauge_open(&fg, &cfg));

  ra8_fuelgauge_state_t st = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fuelgauge_read(&fg, &st));
  TEST_ASSERT_EQ((uint8_t)k_fg_soc_expected_pct, st.soc_pct);
  TEST_ASSERT_EQ((uint16_t)k_fg_vcell_3v7_mv, st.vcell_mv);
  TEST_ASSERT_EQ((int16_t)k_fg_crate_charge, st.crate_raw);
  TEST_ASSERT(st.charging);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fuelgauge_close(&fg));
  TEST_END("ra8_fuelgauge_read: decodes SOC, VCELL and CRATE");
}

/**
 * @test internal_test_read_charge_sign
 * @brief Prove the charging flag is the CRATE sign both apps tested by hand.
 *
 * @pre ::internal_prep has run.
 * @post The handle is closed again.
 * @note File-local test case; single-threaded harness.
 * @since 0.1.0
 */
RA8_INTERNAL
static void internal_test_read_charge_sign(void)
{
  TEST_BEGIN("ra8_fuelgauge_read: charging is the CRATE sign");
  internal_prep();
  ra8_fuelgauge_t           fg  = {};
  const ra8_fuelgauge_cfg_t cfg = internal_cfg((uint8_t)k_ra8_fuelgauge_default_addr_7b);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fuelgauge_open(&fg, &cfg));

  ra8_fuelgauge_state_t st = {};
  s_bus.crate              = (uint16_t)k_fg_crate_dischg;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fuelgauge_read(&fg, &st));
  TEST_ASSERT(!st.charging);
  TEST_ASSERT(st.crate_raw < 0);

  /* The in-tree readers test (crate_high & 0x80) == 0, so exactly zero
   * counts as charging; keep that behaviour bit-for-bit. */
  s_bus.crate = (uint16_t)k_fg_crate_zero;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fuelgauge_read(&fg, &st));
  TEST_ASSERT(st.charging);
  TEST_ASSERT_EQ(0, st.crate_raw);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fuelgauge_close(&fg));
  TEST_END("ra8_fuelgauge_read: charging is the CRATE sign");
}

/**
 * @test internal_test_read_bad_inputs
 * @brief Prove read rejects NULLs, a closed handle, and forwards failures.
 *
 * @pre ::internal_prep has run.
 * @post The handle is closed again.
 * @note File-local test case; single-threaded harness.
 * @since 0.1.0
 */
RA8_INTERNAL
static void internal_test_read_bad_inputs(void)
{
  TEST_BEGIN("ra8_fuelgauge_read: bad inputs and failures");
  internal_prep();
  ra8_fuelgauge_t           fg  = {};
  const ra8_fuelgauge_cfg_t cfg = internal_cfg((uint8_t)k_ra8_fuelgauge_default_addr_7b);
  ra8_fuelgauge_state_t     st  = {};

  TEST_ASSERT_EQ(k_ra8_err_not_initialized, ra8_fuelgauge_read(&fg, &st));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fuelgauge_open(&fg, &cfg));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_fuelgauge_read(nullptr, &st));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_fuelgauge_read(&fg, nullptr));

  /* A transport failure leaves the caller's sample untouched. */
  st.soc_pct      = (uint8_t)k_fg_soc_expected_pct;
  s_bus.fail_with = k_ra8_err_hw_timeout;
  TEST_ASSERT_EQ(k_ra8_err_hw_timeout, ra8_fuelgauge_read(&fg, &st));
  TEST_ASSERT_EQ((uint8_t)k_fg_soc_expected_pct, st.soc_pct);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fuelgauge_close(&fg));
  TEST_END("ra8_fuelgauge_read: bad inputs and failures");
}

/**
 * @test internal_test_close_contract
 * @brief Prove close clears the handle and is not idempotent by accident.
 *
 * @pre ::internal_prep has run.
 * @post The handle is closed.
 * @note File-local test case; single-threaded harness.
 * @since 0.1.0
 */
RA8_INTERNAL
static void internal_test_close_contract(void)
{
  TEST_BEGIN("ra8_fuelgauge_close: clears the handle");
  internal_prep();
  ra8_fuelgauge_t           fg  = {};
  const ra8_fuelgauge_cfg_t cfg = internal_cfg((uint8_t)k_ra8_fuelgauge_default_addr_7b);

  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_fuelgauge_close(nullptr));
  TEST_ASSERT_EQ(k_ra8_err_not_initialized, ra8_fuelgauge_close(&fg));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fuelgauge_open(&fg, &cfg));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fuelgauge_close(&fg));
  TEST_ASSERT_EQ(k_ra8_err_not_initialized, ra8_fuelgauge_close(&fg));

  ra8_fuelgauge_state_t st = {};
  TEST_ASSERT_EQ(k_ra8_err_not_initialized, ra8_fuelgauge_read(&fg, &st));
  TEST_END("ra8_fuelgauge_close: clears the handle");
}

/** @brief Every test case in this translation unit. */
static void (*const s_test_roster[])(void) = {
  internal_test_open_probes_version,
  internal_test_open_bad_inputs,
  internal_test_open_dead_segment,
  internal_test_open_propagates_transport_error,
  internal_test_read_decodes_sample,
  internal_test_read_charge_sign,
  internal_test_read_bad_inputs,
  internal_test_close_contract,
};

/**
 * @brief Run every test case in this translation unit.
 *
 * @return 0 when all cases pass (Unity aborts the process on failure).
 *
 * @pre None.
 * @post Every registered case has executed.
 * @note Thread safety: single-threaded test harness.
 * @since 0.1.0
 */
int main(void)
{
  for (size_t i = 0U; i < (sizeof s_test_roster / sizeof s_test_roster[0]); ++i) {
    s_test_roster[i]();
  }
  return 0;
}
