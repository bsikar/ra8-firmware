/**
 * @file test_ra_opamp.c
 * @brief Unit tests for ra_opamp.c (op-amp / AMP driver)
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>

#include "ra8d2_opamp_regs.h"
#include "ra_err.h"
#include "ra_opamp.h"
#include "ra_sim_mmap.h"
#include "unity_minimal.h"

static void prep(void)
{
  ra_sim_mmap_reset();
  (void)ra_opamp_close();
}

/* ---- Lifecycle ---- */

static void test_open_clears_registers(void)
{
  TEST_BEGIN("open zeros AMPC/AMPGAIN/AMPINSEL");
  prep();

  volatile r_opamp_regs_t* reg = ra_opamp();
  reg->AMPC                    = 0xFFU;
  reg->AMPGAIN                 = 0xFFFFU;
  reg->AMPINSEL                = 0xFFFFU;

  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_opamp_open());
  TEST_ASSERT_EQ(0, (int)reg->AMPC);
  TEST_ASSERT_EQ(0, (int)reg->AMPGAIN);
  TEST_ASSERT_EQ(0, (int)reg->AMPINSEL);
  TEST_END("open zeros AMPC/AMPGAIN/AMPINSEL");
}

static void test_close_without_open(void)
{
  TEST_BEGIN("close without open returns invalid_state");
  prep();
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_state, (int32_t)ra_opamp_close());
  TEST_END("close without open returns invalid_state");
}

/* ---- Pre-open guard on configure ---- */

static void test_configure_pre_open(void)
{
  TEST_BEGIN("configure rejects pre-open");
  prep();
  ra_opamp_cfg_t cfg = {
    .channel    = k_ra_opamp_ch0,
    .gain       = k_ra_opamp_gain_2x,
    .input_pin  = k_ra_opamp_input_pin_default,
    .output_pin = k_ra_opamp_output_pin_default,
  };
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_state, (int32_t)ra_opamp_configure(&cfg));
  TEST_END("configure rejects pre-open");
}

/* ---- Null arg ---- */

static void test_configure_null(void)
{
  TEST_BEGIN("configure rejects NULL cfg");
  prep();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_opamp_open());
  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr, (int32_t)ra_opamp_configure(nullptr));
  TEST_END("configure rejects NULL cfg");
}

/* ---- Range guards ---- */

static void test_configure_range_guards(void)
{
  TEST_BEGIN("configure rejects bogus channel/gain/input_pin/output_pin");
  prep();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_opamp_open());

  ra_opamp_cfg_t cfg = {
    .channel    = (ra_opamp_channel_t)9U,
    .gain       = k_ra_opamp_gain_unity,
    .input_pin  = k_ra_opamp_input_pin_default,
    .output_pin = k_ra_opamp_output_pin_default,
  };
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg, (int32_t)ra_opamp_configure(&cfg));

  cfg.channel = k_ra_opamp_ch0;
  cfg.gain    = (ra_opamp_gain_t)9U;
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg, (int32_t)ra_opamp_configure(&cfg));

  cfg.gain      = k_ra_opamp_gain_unity;
  cfg.input_pin = (ra_opamp_input_pin_t)9U;
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg, (int32_t)ra_opamp_configure(&cfg));

  cfg.input_pin  = k_ra_opamp_input_pin_default;
  cfg.output_pin = (ra_opamp_output_pin_t)9U;
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg, (int32_t)ra_opamp_configure(&cfg));

  TEST_END("configure rejects bogus channel/gain/input_pin/output_pin");
}

/* ---- configure() programmes the right nibbles ---- */

static void test_configure_writes_nibbles(void)
{
  TEST_BEGIN("configure writes per-channel nibbles in AMPGAIN/AMPINSEL");
  prep();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_opamp_open());

  ra_opamp_cfg_t cfg2 = {
    .channel    = k_ra_opamp_ch2,
    .gain       = k_ra_opamp_gain_4x,
    .input_pin  = k_ra_opamp_input_pin_alt1,
    .output_pin = k_ra_opamp_output_pin_default,
  };
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_opamp_configure(&cfg2));

  volatile r_opamp_regs_t* reg = ra_opamp();
  /* Channel 2 nibble starts at bit 8. gain_4x = 2 -> AMPGAIN = 0x0200. */
  TEST_ASSERT_EQ((int32_t)0x0200U, (int32_t)reg->AMPGAIN);
  /* input_pin_alt1 = 1 -> AMPINSEL = 0x0100. */
  TEST_ASSERT_EQ((int32_t)0x0100U, (int32_t)reg->AMPINSEL);
  /* AMPC bit 2 set. */
  TEST_ASSERT_EQ((int32_t)0x04U, (int32_t)reg->AMPC);

  /* Configure channel 0 too -- ensure ch2 nibbles survive. */
  ra_opamp_cfg_t cfg0 = {
    .channel    = k_ra_opamp_ch0,
    .gain       = k_ra_opamp_gain_8x,
    .input_pin  = k_ra_opamp_input_pin_alt2,
    .output_pin = k_ra_opamp_output_pin_default,
  };
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_opamp_configure(&cfg0));
  /* Channel 0 nibble at bit 0 (gain_8x=3), channel 2 nibble preserved. */
  TEST_ASSERT_EQ((int32_t)0x0203U, (int32_t)reg->AMPGAIN);
  TEST_ASSERT_EQ((int32_t)0x0102U, (int32_t)reg->AMPINSEL);
  TEST_ASSERT_EQ((int32_t)0x05U, (int32_t)reg->AMPC); /* bits 0 and 2. */
  TEST_END("configure writes per-channel nibbles in AMPGAIN/AMPINSEL");
}

int32_t main(void)
{
  test_open_clears_registers();
  test_close_without_open();
  test_configure_pre_open();
  test_configure_null();
  test_configure_range_guards();
  test_configure_writes_nibbles();
  (void)fprintf(stderr, "[OK ] test_ra_opamp.c\n");
  return 0;
}
