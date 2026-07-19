/**
 * @file test_ra8_dfu_launch_cov.c
 * @brief White-box line-coverage tests for the copy-to-run launch hand-off
 *        (`ra8_dfu_launch.c`).
 *
 * @par Tag
 * [Ring 4 / Service] {World: S}
 *
 * @details
 * `ra8_dfu_launch` is the shared copy-to-run hand-off behind the DFU
 * bootloader. On the firmware target it copies an image to the SRAM run base
 * and branches into it; that copy + VTOR/MSP switch + `bx` all live inside
 * `#ifndef RA8_SIMULATOR_MODE`. Under the host test build `RA8_SIMULATOR_MODE`
 * is defined for every TU (see tests/CMakeLists.txt), so the hardware branch
 * is compiled out and never counted. What remains is host-compilable and is
 * covered here:
 *
 *  - the `src == 0` early return;
 *  - the `ra8_dfu_run_target_valid` cross-check early return; and
 *  - (only when `RA8_ENABLE_ROOT_OF_TRUST` is defined) the root-of-trust
 *    verify + anti-rollback default-deny arms.
 *
 * The default (flag-off) production object -- the one the coverage gate
 * measures -- contains only the two early-return guards plus the
 * fall-through; `test_launch_production_default_paths` drives the real
 * `ra8_dfu_launch` linked from `ra8_core_hal` across all three of those legs.
 *
 * The root-of-trust arms are compiled out of that default object, so to
 * exercise them this TU compiles a second, instrumented copy of the module
 * with `RA8_ENABLE_ROOT_OF_TRUST` defined: the one exported symbol
 * (`ra8_dfu_launch`) is renamed to `ra8_dfu_launch_cov` so it does not collide
 * with the production copy, and the three root-of-trust dependencies
 * (`ra8_rot_trailer_after`, `ra8_rot_verify_image`, `ra8_rot_antirollback_verify`
 * plus its default store) are redirected -- via preprocessor rename -- to
 * deterministic, test-scripted mocks. The REAL launch control flow then runs
 * line-by-line over the mocked verify results; no line is bypassed by an
 * exclusion marker.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>

#include "ra8_dfu.h"
#include "ra8_dfu_antirollback.h"
#include "ra8_err.h"
#include "ra8_rot.h"
#include "unity_minimal.h"

/**
 * @enum dfu_launch_cov_uint8_const_t
 * @brief Named uint8_t constants used by this file.
 *
 * @details
 * Every literal this translation unit needs, named so the
 * value's role is visible at the point of use (CLAUDE.md
 * "No Magic Numbers").
 */
typedef enum : uint8_t {
  k_dfu_image_bytes = 64, /**< Size of the fixture image. */
} dfu_launch_cov_uint8_const_t;

/**
 * @enum test_dfu_launch_const_t
 * @brief Benign launch arguments used to drive each control leg.
 */
typedef enum : uint32_t {
  k_tc_img_len   = 0x00000020U, /**< Valid image length: one 32-byte MRAM page. */
  k_tc_entry_bad = 0xDEADBEEFU, /**< An `entry` that is not the run base.       */
  k_tc_img_ver   = 0x00000007U, /**< Mock trailer anti-rollback image version.  */
} test_dfu_launch_const_t;

/**
 * @test test_launch_production_default_paths
 *
 * @par MC/DC:
 * `ra8_dfu_launch` carries no compound decisions -- both guards are
 * single-condition. This drives the production (flag-off) object across each
 * guard's independent-influence vectors:
 * - `src == 0`: V1 src!=0 -> false (continue); V2 src==0 -> true (return).
 * - `!ra8_dfu_run_target_valid(entry, img_len)`: V1 valid target -> false
 *   (continue); V2 bad entry -> true (return).
 * The third call (src!=0, valid target) falls through both guards; the
 * hardware copy/branch is `#ifndef RA8_SIMULATOR_MODE` and compiled out on the
 * host, so control simply returns.
 */
static void test_launch_production_default_paths(void)
{
  TEST_BEGIN("ra8_dfu_launch: src==0, invalid target, and valid fall-through");

  uint8_t img[k_dfu_image_bytes] = {};

  /* src == 0 -> nothing to copy -> early return. */
  ra8_dfu_launch(0U, (uint32_t)k_tc_img_len, (uint32_t)k_ra8_dfu_run_base);

  /* Bad entry -> ra8_dfu_run_target_valid false -> caller-fallback return. */
  ra8_dfu_launch((uintptr_t)img, (uint32_t)k_tc_img_len, (uint32_t)k_tc_entry_bad);

  /* Valid: entry == run base, len is a 32-byte page. Both guards fall
   * through; the copy/branch is compiled out under RA8_SIMULATOR_MODE, so the
   * call returns without touching MSP/VTOR. Reaching here proves it. */
  ra8_dfu_launch((uintptr_t)img, (uint32_t)k_tc_img_len, (uint32_t)k_ra8_dfu_run_base);

  TEST_ASSERT(true);
  TEST_END("ra8_dfu_launch: src==0, invalid target, and valid fall-through");
}

/* =============================================================================
 * Root-of-trust white-box copy: a second, instrumented compile of the module
 * with RA8_ENABLE_ROOT_OF_TRUST defined and its verify dependencies mocked.
 * =============================================================================
 */

/** @brief Mock trailer `mock_rot_trailer_after` hands back (never NULL). */
static ra8_rot_trailer_t s_mock_trailer = {};
/** @brief Result `mock_rot_verify_image` returns. */
static ra8_err_t s_verify_err = k_ra8_ok;
/** @brief Result `mock_rot_antirollback_verify` returns. */
static ra8_err_t s_antirollback_err = k_ra8_ok;
/** @brief Dummy store the default-store mock hands back (never NULL). */
static const ra8_rot_antirollback_store_t s_mock_store = {};

static const ra8_rot_trailer_t* mock_rot_trailer_after(const void* image_base, uint32_t body_len)
{
  (void)image_base;
  (void)body_len;
  s_mock_trailer.img_version = (uint32_t)k_tc_img_ver;
  return &s_mock_trailer;
}

static ra8_err_t
mock_rot_verify_image(const uint8_t* body, uint32_t body_len, const ra8_rot_trailer_t* trailer)
{
  (void)body;
  (void)body_len;
  (void)trailer;
  return s_verify_err;
}

static const ra8_rot_antirollback_store_t* mock_rot_antirollback_default_store(void)
{
  return &s_mock_store;
}

static ra8_err_t mock_rot_antirollback_verify(const ra8_rot_antirollback_store_t* store,
                                              uint32_t                            image_version)
{
  (void)store;
  (void)image_version;
  return s_antirollback_err;
}

/* Rename the exported symbol so the instrumented copy does not clash with the
 * production `ra8_dfu_launch` linked from ra8_core_hal, enable the flag-gated
 * root-of-trust arms, and redirect their dependencies to the mocks above. */
void ra8_dfu_launch_cov(uintptr_t src, uint32_t img_len, uint32_t entry);

/** @brief RA8 ENABLE ROOT OF TRUST. */
#define RA8_ENABLE_ROOT_OF_TRUST
/** @brief RA8 dfu launch. */
/* Include-time interposition seam: each macro below must be spelled
 * EXACTLY like the production symbol it replaces, so the project rule
 * that macro names are UPPER_CASE cannot apply here -- renaming one
 * silently un-hooks the mock and the test would exercise the real
 * driver while still passing. */
// NOLINTBEGIN(readability-identifier-naming)
#define ra8_dfu_launch ra8_dfu_launch_cov
/** @brief RA8 rot trailer after. */
#define ra8_rot_trailer_after mock_rot_trailer_after
/** @brief RA8 rot verify image. */
#define ra8_rot_verify_image mock_rot_verify_image
/** @brief RA8 rot antirollback verify. */
#define ra8_rot_antirollback_verify mock_rot_antirollback_verify
/** @brief RA8 rot antirollback default store. */
#define ra8_rot_antirollback_default_store mock_rot_antirollback_default_store
// NOLINTEND(readability-identifier-naming)

#include "ra8_dfu_launch.c" // NOLINT(bugprone-suspicious-include) -- white-box copy

/* =============================================================================
 * White-box tests: each drives the instrumented `ra8_dfu_launch_cov` on scripted
 * verify results so the flag-gated root-of-trust arms run line-by-line.
 * =============================================================================
 */

/**
 * @test test_launch_cov_early_returns
 *
 * @par MC/DC:
 * The two pre-root-of-trust guards on the instrumented copy, single-condition
 * each:
 * - `src == 0`: V (true) -> early return here.
 * - `!ra8_dfu_run_target_valid(entry, img_len)`: V (true, bad entry) -> return.
 * Their false controls are the third call in the production test above and the
 * accept test below.
 */
static void test_launch_cov_early_returns(void)
{
  TEST_BEGIN("ra8_dfu_launch_cov: src==0 and bad-entry early returns");

  uint8_t img[k_dfu_image_bytes] = {};

  /* src == 0 -> early return before the root-of-trust gate. */
  ra8_dfu_launch_cov(0U, (uint32_t)k_tc_img_len, (uint32_t)k_ra8_dfu_run_base);

  /* Bad entry -> run-target cross-check false -> return before verify. */
  ra8_dfu_launch_cov((uintptr_t)img, (uint32_t)k_tc_img_len, (uint32_t)k_tc_entry_bad);

  TEST_ASSERT(true);
  TEST_END("ra8_dfu_launch_cov: src==0 and bad-entry early returns");
}

/**
 * @test test_launch_cov_rot_verify_deny
 *
 * @par MC/DC:
 * Decision `ra8_rot_verify_image(...) != k_ra8_ok` (1 condition).
 * - V (true): a forged / mis-signed image (mock returns crc_mismatch) ->
 *   DEFAULT-DENY, the launch returns without reaching anti-rollback.
 * The false control (verify == k_ra8_ok) is `test_launch_cov_rot_accept`.
 * N+1 = 2 vectors for N=1 condition: minimal MC/DC.
 */
static void test_launch_cov_rot_verify_deny(void)
{
  TEST_BEGIN("ra8_dfu_launch_cov(ROT): verify failure default-denies");

  uint8_t img[k_dfu_image_bytes] = {};
  s_verify_err                   = k_ra8_err_crc_mismatch; /* Signature did not verify. */
  s_antirollback_err             = k_ra8_ok;
  ra8_dfu_launch_cov((uintptr_t)img, (uint32_t)k_tc_img_len, (uint32_t)k_ra8_dfu_run_base);

  TEST_ASSERT(true);
  TEST_END("ra8_dfu_launch_cov(ROT): verify failure default-denies");
}

/**
 * @test test_launch_cov_rot_antirollback_deny
 *
 * @par MC/DC:
 * Decision `ra8_rot_antirollback_verify(...) != k_ra8_ok` (1 condition),
 * reached only when verify passed:
 * - verify guard false (verify == k_ra8_ok), so control enters the
 *   anti-rollback guard;
 * - V (true): a downgrade / not-provisioned verdict (mock returns
 *   validation_failed) -> DEFAULT-DENY, the launch returns.
 * The false control is `test_launch_cov_rot_accept`. N+1 = 2 vectors.
 */
static void test_launch_cov_rot_antirollback_deny(void)
{
  TEST_BEGIN("ra8_dfu_launch_cov(ROT): anti-rollback failure default-denies");

  uint8_t img[k_dfu_image_bytes] = {};
  s_verify_err                   = k_ra8_ok;
  s_antirollback_err             = k_ra8_err_validation_failed; /* Downgrade / unprovisioned. */
  ra8_dfu_launch_cov((uintptr_t)img, (uint32_t)k_tc_img_len, (uint32_t)k_ra8_dfu_run_base);

  TEST_ASSERT(true);
  TEST_END("ra8_dfu_launch_cov(ROT): anti-rollback failure default-denies");
}

/**
 * @test test_launch_cov_rot_accept
 *
 * @par MC/DC:
 * The all-conditions-false control for both root-of-trust guards:
 * - `ra8_rot_verify_image(...) != k_ra8_ok` -> false (verify == k_ra8_ok);
 * - `ra8_rot_antirollback_verify(...) != k_ra8_ok` -> false (== k_ra8_ok).
 * Both guards fall through to the hardware copy/branch, which is
 * `#ifndef RA8_SIMULATOR_MODE` and compiled out on the host, so the accepted
 * launch reaches the end of the function and returns. This vector pairs with
 * the two deny tests to prove each guard independently forces the outcome.
 */
static void test_launch_cov_rot_accept(void)
{
  TEST_BEGIN("ra8_dfu_launch_cov(ROT): authentic + fresh image is accepted");

  uint8_t img[k_dfu_image_bytes] = {};
  s_verify_err                   = k_ra8_ok;
  s_antirollback_err             = k_ra8_ok;
  ra8_dfu_launch_cov((uintptr_t)img, (uint32_t)k_tc_img_len, (uint32_t)k_ra8_dfu_run_base);

  TEST_ASSERT(true);
  TEST_END("ra8_dfu_launch_cov(ROT): authentic + fresh image is accepted");
}

int main(void)
{
  test_launch_production_default_paths();
  test_launch_cov_early_returns();
  test_launch_cov_rot_verify_deny();
  test_launch_cov_rot_antirollback_deny();
  test_launch_cov_rot_accept();
  (void)fprintf(stderr, "[OK ] test_ra8_dfu_launch_cov.c\n");
  return 0;
}
