/**
 * @file test_app_threadx_fs_demo.c
 * @brief Integration test: threadx_fs_demo's FAT mount + precondition flow
 *
 * @details
 * The production app at
 * examples/ek_ra8d2/hw_validated/hil/threadx_fs_demo/src/main.c stands up
 * ThreadX + LevelX + ra8_fs on the OSPI flash and runs a FAT file-ops
 * roundtrip. Neither ThreadX nor LevelX are linked into the host test build
 * (RA8_OFF_TARGET), so this test exercises the same ra8_fs surface the app
 * drives: the FAT adapter and its block-device backend seam.
 *
 * Modeled flow:
 *   1. Bring up CGC + SysTick (the demo's pre-kernel boot).
 *   2. Mount a FAT volume via ra8_fs_mount with an in-RAM backend (the
 *      same call shape the app makes over the LevelX backend).
 *   3. Validate ra8_fs_mount precondition guards (NULL-arg rejection,
 *      missing function pointer rejection).
 * (The full LevelX-backed roundtrip -- format, mount, write, list, read,
 * unlink over a real LevelX instance on a RAM NOR fake -- lives in
 * tests/misc/src/test_lx_fs_backend.c.)
 *
 * Exercised modules:
 *   - ra8_cgc, ra8_time   (pre-kernel boot)
 *   - ra8_fs             (the first-party FAT adapter)
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>
#include <string.h>

#include "ra8_cgc.h"
#include "ra8_err.h"
#include "ra8_fake_mmap.h"
#include "ra8_fs.h"
#include "ra8_pin_validator.h"
#include "ra8_system_regs.h"
#include "ra8_time.h"
#include "unity_minimal.h"

/**
 * @enum app_threadx_fs_demo_fixture_t
 * @brief All-bits-set register values, so a write that clears the wrong field leaves evidence.
 */
typedef enum : uint8_t {
  k_sys_oscsf_all_ready =
    0xFFU, /**< Every oscillator-stabilisation flag set, so bring-up sees all sources ready. */
} app_threadx_fs_demo_fixture_t;

/** @brief Per-test enums. */
typedef enum : uint32_t {
  k_test_fs_demo_block_count = 128U, /**< Tiny mock device, 128 sectors. */
  k_test_fs_demo_block_size  = 512U, /**< FAT bytes-per-sector.          */
} test_fs_demo_const_t;

/**
 * @brief Per-test fixture reset.
 */
static void reset_world(void)
{
  ra8_fake_mmap_reset();
  ra8_pin_validator_reset();
  /* Pre-seed OSCSF stabilisation bits so ra8_cgc_init() spin loops
   * complete on the first iteration in RA8_OFF_TARGET. */
  *ra8_sys_oscsf() = (uint8_t)k_sys_oscsf_all_ready;
}

/* -------------------------------------------------------------------------
 * Mock block-device backend (returns garbage data so mount validation
 * triggers k_ra8_err_validation_failed -- we want to exercise the call
 * shape, not a full FAT image).
 * ------------------------------------------------------------------------- */

static ra8_err_t mock_read_block(void* ctx, uint64_t lba, uint32_t count, uint8_t* buf)
{
  (void)ctx;
  (void)lba;
  if (buf == nullptr) {
    return k_ra8_err_null_ptr;
  }
  (void)memset(buf, 0, (size_t)count * (size_t)k_test_fs_demo_block_size);
  return k_ra8_ok;
}

static ra8_err_t mock_write_block(void* ctx, uint64_t lba, uint32_t count, const uint8_t* buf)
{
  (void)ctx;
  (void)lba;
  (void)count;
  (void)buf;
  return k_ra8_ok;
}

static ra8_err_t mock_get_capacity(void* ctx, uint64_t* block_count, uint32_t* block_size)
{
  (void)ctx;
  if ((block_count == nullptr) || (block_size == nullptr)) {
    return k_ra8_err_null_ptr;
  }
  *block_count = (uint32_t)k_test_fs_demo_block_count;
  *block_size  = (uint32_t)k_test_fs_demo_block_size;
  return k_ra8_ok;
}

/**
 * @brief Build a fully-populated mock backend.
 */
static ra8_fs_backend_t make_mock_backend(void)
{
  ra8_fs_backend_t b = {};
  b.read_block       = mock_read_block;
  b.write_block      = mock_write_block;
  b.get_capacity     = mock_get_capacity;
  b.ctx              = nullptr;
  return b;
}

/**
 * @brief Pre-kernel boot replay: CGC + SysTick at CPUCLK0.
 *
 * @par MC/DC: not applicable -- sequential cgc_init -> readback ->
 * time_init wiring with no compound boolean decisions.
 */
static void test_fs_demo_pre_kernel_bringup(void)
{
  reset_world();
  TEST_BEGIN("fs_demo: pre-kernel CGC + SysTick");
  TEST_ASSERT_EQ(k_ra8_ok, ra8_cgc_init());
  uint32_t hz = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_cgc_get_clock_hz(k_ra8_clock_id_cpuclk0, &hz));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_time_init(hz));
  TEST_END("fs_demo: pre-kernel CGC + SysTick");
}

/**
 * @brief Mount call rejects NULL backend (backend precondition equivalent).
 *
 * @par MC/DC:
 * Compound decision under test: ``backend == NULL || out_handle == NULL``
 * inside ra8_fs_mount. Two atomic conditions x N+1 = 3 vectors. This case
 * covers backend==NULL (both reject paths join in early-return).
 */
static void test_fs_demo_mount_rejects_null_backend(void)
{
  reset_world();
  TEST_BEGIN("fs_demo: mount(NULL backend) rejected");
  ra8_fs_mount_t* handle = nullptr;
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_fs_mount(nullptr, &handle));
  TEST_END("fs_demo: mount(NULL backend) rejected");
}

/**
 * @brief Mount call rejects NULL out_handle.
 *
 * @par MC/DC:
 * Compound decision under test: ``backend == NULL || out_handle == NULL``
 * inside ra8_fs_mount. Vector covering out_handle==NULL.
 */
static void test_fs_demo_mount_rejects_null_handle(void)
{
  reset_world();
  TEST_BEGIN("fs_demo: mount(NULL out_handle) rejected");
  const ra8_fs_backend_t backend = make_mock_backend();
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_fs_mount(&backend, nullptr));
  TEST_END("fs_demo: mount(NULL out_handle) rejected");
}

/**
 * @brief Backend with a missing function pointer is rejected.
 *
 * @par MC/DC:
 * Decision vector under test: ``read_block == NULL || write_block == NULL
 * || get_capacity == NULL`` guard inside ra8_fs_mount. This case covers
 * read_block==NULL (the most common partial-init bug).
 */
static void test_fs_demo_mount_rejects_partial_backend(void)
{
  reset_world();
  TEST_BEGIN("fs_demo: mount rejects partial backend");
  ra8_fs_backend_t backend = make_mock_backend();
  backend.read_block       = nullptr;
  ra8_fs_mount_t* handle   = nullptr;
  TEST_ASSERT(ra8_fs_mount(&backend, &handle) != k_ra8_ok);
  TEST_END("fs_demo: mount rejects partial backend");
}

/**
 * @brief Mount with a fully-zeroed image fails at BPB validation.
 *
 * @par MC/DC: not applicable -- this exercises a single sequential
 * mount call that flows into the BPB-signature check; the test is
 * a fault-injection of "device is empty".
 */
static void test_fs_demo_mount_rejects_unformatted(void)
{
  reset_world();
  TEST_BEGIN("fs_demo: mount of unformatted device fails cleanly");
  const ra8_fs_backend_t backend = make_mock_backend();
  ra8_fs_mount_t*        handle  = nullptr;
  /* Mock backend returns all-zero sectors -> BPB signature 0x55AA absent. */
  ra8_err_t err = ra8_fs_mount(&backend, &handle);
  TEST_ASSERT(err != k_ra8_ok);
  TEST_END("fs_demo: mount of unformatted device fails cleanly");
}

int main(void)
{
  test_fs_demo_pre_kernel_bringup();
  test_fs_demo_mount_rejects_null_backend();
  test_fs_demo_mount_rejects_null_handle();
  test_fs_demo_mount_rejects_partial_backend();
  test_fs_demo_mount_rejects_unformatted();
  return 0;
}
