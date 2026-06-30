/**
 * @file test_ra_sdhi_width.c
 * @brief Unit tests for the SDHI bus-width setter + ACMD6 negotiation
 *
 * @details
 * Exercises ::ra_sdhi_set_bus_width (host-side SD_OPTION.WIDTH /
 * WIDTH8 programming) and ::ra_sdhi_set_bus_width_4bit (the CMD55 +
 * ACMD6 card-side negotiation). The two compound boolean decisions
 * introduced by the width feature -- the three-way width validation
 * and the two-condition ACMD6 acknowledgement check -- carry MC/DC
 * vector blocks below.
 *
 * The simulator backs every SDHI register with plain RAM, so the
 * ACMD6 tests pre-seed SD_RSP10 with a synthetic R1 card-status word
 * and use a periodic SIGALRM to keep SD_INFO1.RSPEND asserted while
 * the polled command path spins.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <signal.h>
#include <sys/time.h>

#include "ra8d2_sdhi_regs.h"
#include "ra_err.h"
#include "ra_mstp.h"
#include "ra_sdhi.h"
#include "ra_sim_mmap.h"
#include "unity_minimal.h"

/**
 * @enum ra_sdhi_width_test_inst_t
 * @brief Instance indices used by the width tests.
 *
 * @details
 * Two valid instances plus one deliberately out-of-range index used to
 * drive the null-pointer rejection path.
 */
typedef enum : uint8_t {
  k_test_inst_0   = 0U, /**< First valid SDHI instance.   */
  k_test_inst_1   = 1U, /**< Second valid SDHI instance.  */
  k_test_inst_bad = 9U, /**< Out-of-range -> null_ptr.    */
} ra_sdhi_width_test_inst_t;

/**
 * @enum ra_sdhi_width_test_const_t
 * @brief Synthetic R1 card-status responses for the ACMD6 vectors.
 *
 * @details
 * - ``ack`` sets APP_CMD with no error bits -> card accepts ACMD6.
 * - ``no_appcmd`` clears APP_CMD -> CMD55 was not honoured.
 * - ``error`` sets APP_CMD but also an R1 error bit -> ACMD6 rejected.
 * - ``bad_width`` is an invalid lane count for ::ra_sdhi_set_bus_width.
 */
typedef enum : uint32_t {
  k_test_r1_ack       = (uint32_t)k_ra_sdhi_r1_app_cmd_mask,
  k_test_r1_no_appcmd = 0x00000000UL,
  k_test_r1_error =
    (uint32_t)k_ra_sdhi_r1_app_cmd_mask | (uint32_t)k_ra_sdhi_r1_illegal_command,
  k_test_bad_width = 2U,
  k_test_rca       = 0xB368UL,
} ra_sdhi_width_test_const_t;

static uint8_t s_alarm_inst;

static void width_periodic_handler(int sig)
{
  (void)sig;
  /* Keep SD_INFO1.RSPEND asserted so each polled command send in the
   * negotiation finds it set even after the driver clears it inline. */
  volatile r_sdhi_regs_t* reg = ra_sdhi(s_alarm_inst);
  if (reg != nullptr) {
    reg->SD_INFO1 = reg->SD_INFO1 | 1UL;
  }
}

static void arm_rspend_periodic(uint8_t inst)
{
  s_alarm_inst = inst;
  struct sigaction sa;
  sa.sa_handler = width_periodic_handler;
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = SA_RESTART;
  (void)sigaction(SIGALRM, &sa, nullptr);
  struct itimerval timer;
  timer.it_value.tv_sec     = 0;
  timer.it_value.tv_usec    = 50;
  timer.it_interval.tv_sec  = 0;
  timer.it_interval.tv_usec = 50;
  (void)setitimer(ITIMER_REAL, &timer, nullptr);
}

static void disarm_alarm(void)
{
  struct itimerval timer = {};
  (void)setitimer(ITIMER_REAL, &timer, nullptr);
  struct sigaction sa;
  sa.sa_handler = SIG_DFL;
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = 0;
  (void)sigaction(SIGALRM, &sa, nullptr);
}

static void prep(void)
{
  ra_sim_mmap_reset();
  (void)ra_mstp_init();
}

/**
 * @brief Seed the simulated R1 response and prime RSPEND for a negotiate run.
 *
 * @details
 * Both CMD55 and ACMD6 read SD_RSP10 in the simulator, so a single
 * seed feeds both responses. SD_INFO1.RSPEND is set up front for the
 * first command; the periodic alarm re-asserts it for the second.
 */
static void seed_response(uint8_t inst, uint32_t r1)
{
  volatile r_sdhi_regs_t* reg = ra_sdhi(inst);
  reg->SD_RSP10               = r1;
  reg->SD_INFO1               = 1UL;
}

/**
 * @test set_bus_width validates the lane count.
 *
 * @par MC/DC:
 * Decision: `if (width != 1 && width != 4 && width != 8)` (3 conditions
 * c1=`width!=1`, c2=`width!=4`, c3=`width!=8`; AND-chain).
 * - Vector 1: width=1 -> c1=F                  -> false (accept, 1-bit)
 * - Vector 2: width=4 -> c1=T,c2=F             -> false (accept, 4-bit)
 * - Vector 3: width=8 -> c1=T,c2=T,c3=F        -> false (accept, 8-bit)
 * - Vector 4: width=2 -> c1=T,c2=T,c3=T        -> true  (reject)
 * Vectors 1+4 prove c1 independence, 2+4 prove c2, 3+4 prove c3.
 * N+1 = 4 vectors for the 3-condition decision: minimal MC/DC.
 */
static void test_set_bus_width_validation(void)
{
  TEST_BEGIN("sdhi set_bus_width: width validation MC/DC");
  prep();
  TEST_ASSERT_EQ(k_ra_ok, ra_sdhi_init((uint8_t)k_test_inst_0));
  volatile r_sdhi_regs_t* reg = ra_sdhi((uint8_t)k_test_inst_0);

  /* Vector 1: 1-bit accepted -> WIDTH set, WIDTH8 clear. */
  TEST_ASSERT_EQ(k_ra_ok, ra_sdhi_set_bus_width((uint8_t)k_test_inst_0, k_ra_sdhi_bus_width_1bit));
  TEST_ASSERT_EQ((uint32_t)k_ra_sdhi_option_width_bit,
                 (reg->SD_OPTION & (uint32_t)k_ra_sdhi_option_width_mask));

  /* Vector 2: 4-bit accepted -> WIDTH clear, WIDTH8 clear. */
  TEST_ASSERT_EQ(k_ra_ok, ra_sdhi_set_bus_width((uint8_t)k_test_inst_0, k_ra_sdhi_bus_width_4bit));
  TEST_ASSERT_EQ(0U, (reg->SD_OPTION & (uint32_t)k_ra_sdhi_option_width_mask));

  /* Vector 3: 8-bit accepted -> WIDTH clear, WIDTH8 set. */
  TEST_ASSERT_EQ(k_ra_ok, ra_sdhi_set_bus_width((uint8_t)k_test_inst_0, k_ra_sdhi_bus_width_8bit));
  TEST_ASSERT_EQ((uint32_t)k_ra_sdhi_option_width8_bit,
                 (reg->SD_OPTION & (uint32_t)k_ra_sdhi_option_width_mask));

  /* Vector 4: invalid width rejected; SD_OPTION left from vector 3. */
  const uint32_t before = reg->SD_OPTION;
  TEST_ASSERT_EQ(k_ra_err_invalid_arg,
                 ra_sdhi_set_bus_width((uint8_t)k_test_inst_0,
                                       (ra_sdhi_bus_width_t)k_test_bad_width));
  TEST_ASSERT_EQ(before, reg->SD_OPTION);
  TEST_END("sdhi set_bus_width: width validation MC/DC");
}

/**
 * @test set_bus_width preserves the SD_OPTION timeout fields.
 *
 * @par MC/DC:
 * (no compound decision under test here -- this case verifies the
 * read-modify-write keeps TOP / CTOP / reserved bit 14 intact across
 * width changes; the validation decision is covered above)
 */
static void test_set_bus_width_preserves_timeout(void)
{
  TEST_BEGIN("sdhi set_bus_width: RMW preserves timeout fields");
  prep();
  TEST_ASSERT_EQ(k_ra_ok, ra_sdhi_init((uint8_t)k_test_inst_1));
  volatile r_sdhi_regs_t* reg = ra_sdhi((uint8_t)k_test_inst_1);

  /* init leaves the genuine 1-bit default (0xC0E0). */
  TEST_ASSERT_EQ((uint32_t)k_ra_sdhi_option_bus_1bit, reg->SD_OPTION);

  TEST_ASSERT_EQ(k_ra_ok, ra_sdhi_set_bus_width((uint8_t)k_test_inst_1, k_ra_sdhi_bus_width_4bit));
  TEST_ASSERT_EQ((uint32_t)k_ra_sdhi_option_bus_4bit, reg->SD_OPTION);

  TEST_ASSERT_EQ(k_ra_ok, ra_sdhi_set_bus_width((uint8_t)k_test_inst_1, k_ra_sdhi_bus_width_8bit));
  TEST_ASSERT_EQ((uint32_t)k_ra_sdhi_option_bus_8bit, reg->SD_OPTION);

  TEST_ASSERT_EQ(k_ra_ok, ra_sdhi_set_bus_width((uint8_t)k_test_inst_1, k_ra_sdhi_bus_width_1bit));
  TEST_ASSERT_EQ((uint32_t)k_ra_sdhi_option_bus_1bit, reg->SD_OPTION);
  TEST_END("sdhi set_bus_width: RMW preserves timeout fields");
}

/**
 * @test set_bus_width rejects an out-of-range instance.
 *
 * @par MC/DC:
 * (no compound decision under test -- exercises the RA_CHECK_NULL_PTR
 * precondition on a bad instance index)
 */
static void test_set_bus_width_bad_instance(void)
{
  TEST_BEGIN("sdhi set_bus_width: bad instance");
  prep();
  TEST_ASSERT_EQ(k_ra_err_null_ptr,
                 ra_sdhi_set_bus_width((uint8_t)k_test_inst_bad, k_ra_sdhi_bus_width_4bit));
  TEST_END("sdhi set_bus_width: bad instance");
}

/**
 * @test ACMD6 negotiation widens to 4-bit when the card acknowledges.
 *
 * @par MC/DC:
 * Decision: `if (!app_cmd_ready || !acmd6_clean)` (2 conditions
 * a=`!app_cmd_ready`, b=`!acmd6_clean`; OR).
 * - Vector 1 (this test): app_cmd_ready=T, acmd6_clean=T -> a=F,b=F
 *   -> false (proceed: host widened to 4-bit, returns k_ra_ok).
 * Vectors 2/3 (the reject tests below) flip each condition. Vectors
 * 1+2 prove app_cmd_ready independence; 1+3 prove acmd6_clean. N+1 = 3.
 */
static void test_acmd6_ack_widens(void)
{
  TEST_BEGIN("sdhi acmd6: card ACK widens host to 4-bit");
  prep();
  TEST_ASSERT_EQ(k_ra_ok, ra_sdhi_init((uint8_t)k_test_inst_0));
  seed_response((uint8_t)k_test_inst_0, (uint32_t)k_test_r1_ack);
  arm_rspend_periodic((uint8_t)k_test_inst_0);

  const ra_err_t err = ra_sdhi_set_bus_width_4bit((uint8_t)k_test_inst_0, (uint16_t)k_test_rca);
  disarm_alarm();

  TEST_ASSERT_EQ(k_ra_ok, err);
  volatile r_sdhi_regs_t* reg = ra_sdhi((uint8_t)k_test_inst_0);
  /* Host SD_OPTION switched to the 4-bit encoding. */
  TEST_ASSERT_EQ((uint32_t)k_ra_sdhi_option_bus_4bit, reg->SD_OPTION);
  /* Final command issued was ACMD6 with the 4-bit argument. */
  TEST_ASSERT_EQ((uint32_t)k_ra_sdhi_cmd_set_bus_width, reg->SD_CMD);
  TEST_ASSERT_EQ((uint32_t)k_ra_sdhi_acmd6_arg_4bit, reg->SD_ARG);
  TEST_END("sdhi acmd6: card ACK widens host to 4-bit");
}

/**
 * @test ACMD6 negotiation rejects when CMD55 is not acknowledged.
 *
 * @par MC/DC:
 * Decision: `if (!app_cmd_ready || !acmd6_clean)` (OR, 2 conditions).
 * - Vector 2 (this test): app_cmd_ready=F, acmd6_clean=T -> a=T
 *   -> true (reject: not_supported, host stays 1-bit). Pairs with
 *   vector 1 to prove app_cmd_ready independently drives the outcome.
 */
static void test_acmd6_reject_no_appcmd(void)
{
  TEST_BEGIN("sdhi acmd6: missing APP_CMD echo stays 1-bit");
  prep();
  TEST_ASSERT_EQ(k_ra_ok, ra_sdhi_init((uint8_t)k_test_inst_1));
  seed_response((uint8_t)k_test_inst_1, (uint32_t)k_test_r1_no_appcmd);
  arm_rspend_periodic((uint8_t)k_test_inst_1);

  const ra_err_t err = ra_sdhi_set_bus_width_4bit((uint8_t)k_test_inst_1, (uint16_t)k_test_rca);
  disarm_alarm();

  TEST_ASSERT_EQ(k_ra_err_not_supported, err);
  /* Host bus width unchanged from the 1-bit default. */
  TEST_ASSERT_EQ((uint32_t)k_ra_sdhi_option_bus_1bit, ra_sdhi((uint8_t)k_test_inst_1)->SD_OPTION);
  TEST_END("sdhi acmd6: missing APP_CMD echo stays 1-bit");
}

/**
 * @test ACMD6 negotiation rejects when the R1 response carries an error.
 *
 * @par MC/DC:
 * Decision: `if (!app_cmd_ready || !acmd6_clean)` (OR, 2 conditions).
 * - Vector 3 (this test): app_cmd_ready=T, acmd6_clean=F -> b=T
 *   -> true (reject: not_supported, host stays 1-bit). Pairs with
 *   vector 1 to prove acmd6_clean independently drives the outcome.
 */
static void test_acmd6_reject_error(void)
{
  TEST_BEGIN("sdhi acmd6: R1 error bit stays 1-bit");
  prep();
  TEST_ASSERT_EQ(k_ra_ok, ra_sdhi_init((uint8_t)k_test_inst_0));
  seed_response((uint8_t)k_test_inst_0, (uint32_t)k_test_r1_error);
  arm_rspend_periodic((uint8_t)k_test_inst_0);

  const ra_err_t err = ra_sdhi_set_bus_width_4bit((uint8_t)k_test_inst_0, (uint16_t)k_test_rca);
  disarm_alarm();

  TEST_ASSERT_EQ(k_ra_err_not_supported, err);
  TEST_ASSERT_EQ((uint32_t)k_ra_sdhi_option_bus_1bit, ra_sdhi((uint8_t)k_test_inst_0)->SD_OPTION);
  TEST_END("sdhi acmd6: R1 error bit stays 1-bit");
}

/**
 * @test ACMD6 negotiation propagates a command-response timeout.
 *
 * @par MC/DC:
 * (no compound decision under test -- with no RSPEND alarm the first
 * command times out and RA_RETURN_ON_ERROR propagates k_ra_err_hw_timeout)
 */
static void test_acmd6_timeout(void)
{
  TEST_BEGIN("sdhi acmd6: CMD55 timeout propagates");
  prep();
  TEST_ASSERT_EQ(k_ra_ok, ra_sdhi_init((uint8_t)k_test_inst_0));
  /* No RSPEND ever asserted -> the polled send times out. */
  const ra_err_t err = ra_sdhi_set_bus_width_4bit((uint8_t)k_test_inst_0, (uint16_t)k_test_rca);
  TEST_ASSERT_EQ(k_ra_err_hw_timeout, err);
  TEST_END("sdhi acmd6: CMD55 timeout propagates");
}

/**
 * @test ACMD6 negotiation rejects an out-of-range instance.
 *
 * @par MC/DC:
 * (no compound decision under test -- exercises the RA_CHECK_NULL_PTR
 * precondition on a bad instance index)
 */
static void test_acmd6_bad_instance(void)
{
  TEST_BEGIN("sdhi acmd6: bad instance");
  prep();
  TEST_ASSERT_EQ(k_ra_err_null_ptr,
                 ra_sdhi_set_bus_width_4bit((uint8_t)k_test_inst_bad, (uint16_t)k_test_rca));
  TEST_END("sdhi acmd6: bad instance");
}

int32_t main(void)
{
  test_set_bus_width_validation();
  test_set_bus_width_preserves_timeout();
  test_set_bus_width_bad_instance();
  test_acmd6_ack_widens();
  test_acmd6_reject_no_appcmd();
  test_acmd6_reject_error();
  test_acmd6_timeout();
  test_acmd6_bad_instance();
  (void)fprintf(stderr, "[OK  ] test_ra_sdhi_width.c\n");
  return 0;
}
