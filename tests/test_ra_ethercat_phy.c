/**
 * @file test_ra_ethercat_phy.c
 * @brief Unit tests for ra_ethercat_phy.c
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>
#include <string.h>

#include "ra_err.h"
#include "ra_ethercat_phy.h"
#include "ra_sim_mmap.h"
#include "unity_minimal.h"

typedef enum : uint8_t {
  k_test_phy_addr  = 1U,
  k_test_reg_count = 32U,
  k_test_addr_high = 31U,
  k_test_port_high = 3U,
} test_ecphy_const_t;

typedef struct {
  uint16_t regs[k_test_reg_count];
  uint8_t  fail_next_write;
} test_ec_io_t;

static test_ec_io_t s_io = {};

static ra_err_t bus_read(void* ctx, uint8_t phy, uint8_t reg, uint16_t* out)
{
  (void)phy;
  test_ec_io_t* st = (test_ec_io_t*)ctx;
  *out             = st->regs[reg];
  return k_ra_ok;
}

static ra_err_t bus_write(void* ctx, uint8_t phy, uint8_t reg, uint16_t data)
{
  (void)phy;
  test_ec_io_t* st = (test_ec_io_t*)ctx;
  if (st->fail_next_write != 0U) {
    st->fail_next_write = 0U;
    return k_ra_err_hw_error;
  }
  st->regs[reg] = data;
  return k_ra_ok;
}

static void prep(void)
{
  ra_sim_mmap_reset();
  (void)ra_ethercat_phy_close();
  (void)memset(&s_io, 0, sizeof(s_io));
  /* BMSR pre-staged with link-up so open() resolves immediately. */
  s_io.regs[1] = 0x0004U;
}

static ra_ethercat_phy_cfg_t make_cfg(void)
{
  ra_ethercat_phy_cfg_t cfg = {};
  cfg.io.read               = bus_read;
  cfg.io.write              = bus_write;
  cfg.io.ctx                = &s_io;
  cfg.phy_address           = (uint8_t)k_test_phy_addr;
  cfg.port                  = 0U;
  cfg.station_id            = 0x1001U;
  cfg.link_poll_max         = 4U;
  return cfg;
}

static void test_open_null(void)
{
  TEST_BEGIN("open rejects NULL cfg + callbacks");
  prep();
  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr, (int32_t)ra_ethercat_phy_open(nullptr));
  ra_ethercat_phy_cfg_t cfg = make_cfg();
  cfg.io.read               = nullptr;
  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr, (int32_t)ra_ethercat_phy_open(&cfg));
  cfg.io.read  = bus_read;
  cfg.io.write = nullptr;
  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr, (int32_t)ra_ethercat_phy_open(&cfg));
  TEST_END("open rejects NULL cfg + callbacks");
}

static void test_open_bad_args(void)
{
  TEST_BEGIN("open rejects bad PHY addr / port");
  prep();
  ra_ethercat_phy_cfg_t cfg = make_cfg();
  cfg.phy_address           = (uint8_t)(k_test_addr_high + 1U);
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg, (int32_t)ra_ethercat_phy_open(&cfg));
  cfg      = make_cfg();
  cfg.port = (uint8_t)(k_test_port_high + 1U);
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg, (int32_t)ra_ethercat_phy_open(&cfg));
  TEST_END("open rejects bad PHY addr / port");
}

static void test_lifecycle(void)
{
  TEST_BEGIN("open / close lifecycle");
  prep();
  const ra_ethercat_phy_cfg_t cfg = make_cfg();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_ethercat_phy_open(&cfg));
  /* BMCR forced to 0x2100. */
  TEST_ASSERT_EQ((int32_t)0x2100, (int32_t)s_io.regs[0]);
  TEST_ASSERT_EQ((int32_t)k_ra_err_exists, (int32_t)ra_ethercat_phy_open(&cfg));
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_ethercat_phy_close());
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_state, (int32_t)ra_ethercat_phy_close());
  TEST_END("open / close lifecycle");
}

static void test_mdio(void)
{
  TEST_BEGIN("mdio read/write validation");
  prep();
  const ra_ethercat_phy_cfg_t cfg = make_cfg();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_ethercat_phy_open(&cfg));

  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg,
                 (int32_t)ra_ethercat_phy_mdio_write((uint8_t)k_test_reg_count, 0U));
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_ethercat_phy_mdio_write(7U, 0xCAFEU));

  uint16_t v = 0U;
  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr, (int32_t)ra_ethercat_phy_mdio_read(7U, nullptr));
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg,
                 (int32_t)ra_ethercat_phy_mdio_read((uint8_t)k_test_reg_count, &v));
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_ethercat_phy_mdio_read(7U, &v));
  TEST_ASSERT_EQ((int32_t)0xCAFE, (int32_t)v);
  TEST_END("mdio read/write validation");
}

static void test_station_and_counters(void)
{
  TEST_BEGIN("station_id_set + counters_add update status snapshot");
  prep();
  const ra_ethercat_phy_cfg_t cfg = make_cfg();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_ethercat_phy_open(&cfg));

  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_ethercat_phy_station_id_set(0xBEEFU));
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_ethercat_phy_counters_add(3U, 5U));

  ra_ethercat_phy_status_t st = {};
  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr, (int32_t)ra_ethercat_phy_status_get(nullptr));
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_ethercat_phy_status_get(&st));
  TEST_ASSERT_EQ((int32_t)0xBEEF, (int32_t)st.station_id);
  TEST_ASSERT_EQ((int32_t)3, (int32_t)st.rx_frames);
  TEST_ASSERT_EQ((int32_t)5, (int32_t)st.tx_frames);
  TEST_ASSERT_EQ((int32_t)k_ra_ethercat_phy_state_active, (int32_t)st.state);
  TEST_ASSERT_EQ((int32_t)1, (int32_t)st.link_up);
  TEST_END("station_id_set + counters_add update status snapshot");
}

static void test_link_down(void)
{
  TEST_BEGIN("open returns ok with link_up=0 if BMSR.LINK never sets");
  prep();
  s_io.regs[1]              = 0U; /* link down */
  ra_ethercat_phy_cfg_t cfg = make_cfg();
  cfg.link_poll_max         = 2U;
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_ethercat_phy_open(&cfg));

  ra_ethercat_phy_status_t st = {};
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_ethercat_phy_status_get(&st));
  TEST_ASSERT_EQ((int32_t)0, (int32_t)st.link_up);
  TEST_END("open returns ok with link_up=0 if BMSR.LINK never sets");
}

static void test_open_write_failure(void)
{
  TEST_BEGIN("open propagates write failure from BMCR force");
  prep();
  s_io.fail_next_write      = 1U;
  ra_ethercat_phy_cfg_t cfg = make_cfg();
  TEST_ASSERT_EQ((int32_t)k_ra_err_hw_error, (int32_t)ra_ethercat_phy_open(&cfg));
  TEST_END("open propagates write failure from BMCR force");
}

static void test_not_initialized(void)
{
  TEST_BEGIN("ops fail with not_initialized when closed");
  prep();
  uint16_t v = 0U;
  TEST_ASSERT_EQ((int32_t)k_ra_err_not_initialized, (int32_t)ra_ethercat_phy_mdio_read(0U, &v));
  TEST_ASSERT_EQ((int32_t)k_ra_err_not_initialized, (int32_t)ra_ethercat_phy_mdio_write(0U, 0U));
  TEST_ASSERT_EQ((int32_t)k_ra_err_not_initialized, (int32_t)ra_ethercat_phy_station_id_set(0U));
  TEST_ASSERT_EQ((int32_t)k_ra_err_not_initialized, (int32_t)ra_ethercat_phy_counters_add(0U, 0U));
  ra_ethercat_phy_status_t st = {};
  TEST_ASSERT_EQ((int32_t)k_ra_err_not_initialized, (int32_t)ra_ethercat_phy_status_get(&st));
  TEST_END("ops fail with not_initialized when closed");
}

int32_t main(void)
{
  test_open_null();
  test_open_bad_args();
  test_lifecycle();
  test_mdio();
  test_station_and_counters();
  test_link_down();
  test_open_write_failure();
  test_not_initialized();
  (void)fprintf(stderr, "[OK ] test_ra_ethercat_phy.c\n");
  return 0;
}
