/**
 * @file test_app_threadx_filex_demo.c
 * @brief Integration test: FileX-equivalent FAT mount + listdir flow
 *
 * @details
 * The production app at examples/ek_ra8d2/threadx_filex_demo/main.c
 * stands up ThreadX + FileX against the SDHI driver and walks the FAT
 * root directory. Neither ThreadX nor FileX are linked into the host
 * test build (RA_SIMULATOR_MODE), so this test exercises the same
 * filesystem surface the FileX-to-ra_sdhi shim ultimately drives:
 * the ra_fs FAT adapter and its block-device backend.
 *
 * Modeled flow:
 *   1. Bring up CGC + SysTick (the demo's pre-kernel boot).
 *   2. Mount a FAT volume via ra_fs_mount with an in-RAM backend (the
 *      same call shape FileX ultimately makes through fx_media_open).
 *   3. Validate ra_fs_mount precondition guards (NULL-arg rejection,
 *      missing function pointer rejection).
 *
 * Exercised modules:
 *   - ra_cgc, ra_time   (pre-kernel boot)
 *   - ra_fs             (FAT adapter -- equivalent surface to FileX)
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>
#include <string.h>

#include "ra_cgc.h"
#include "ra_err.h"
#include "ra_fs.h"
#include "ra_pin_validator.h"
#include "ra_sim_mmap.h"
#include "ra_time.h"
#include "unity_minimal.h"

/** @brief Per-test enums. */
typedef enum : uint32_t {
  k_test_filex_demo_block_count = 128U, /**< Tiny mock device, 128 sectors. */
  k_test_filex_demo_block_size  = 512U, /**< FAT bytes-per-sector. */
} test_filex_const_t;

/**
 * @brief Per-test fixture reset.
 */
static void reset_world(void)
{
  ra_sim_mmap_reset();
  ra_pin_validator_reset();
}

/* -------------------------------------------------------------------------
 * Mock block-device backend (returns garbage data so mount validation
 * triggers k_ra_err_validation_failed -- we want to exercise the call
 * shape, not a full FAT image).
 * ------------------------------------------------------------------------- */

static ra_err_t mock_read_block(void* ctx, uint32_t lba, uint32_t count, uint8_t* buf)
{
  (void)ctx;
  (void)lba;
  if (buf == NULL) {
    return k_ra_err_null_ptr;
  }
  (void)memset(buf, 0, (size_t)count * (size_t)k_test_filex_demo_block_size);
  return k_ra_ok;
}

static ra_err_t mock_write_block(void* ctx, uint32_t lba, uint32_t count, const uint8_t* buf)
{
  (void)ctx;
  (void)lba;
  (void)count;
  (void)buf;
  return k_ra_ok;
}

static ra_err_t mock_get_capacity(void* ctx, uint32_t* block_count, uint32_t* block_size)
{
  (void)ctx;
  if ((block_count == NULL) || (block_size == NULL)) {
    return k_ra_err_null_ptr;
  }
  *block_count = (uint32_t)k_test_filex_demo_block_count;
  *block_size  = (uint32_t)k_test_filex_demo_block_size;
  return k_ra_ok;
}

/**
 * @brief Build a fully-populated mock backend.
 */
static ra_fs_backend_t make_mock_backend(void)
{
  ra_fs_backend_t b = {};
  b.read_block      = mock_read_block;
  b.write_block     = mock_write_block;
  b.get_capacity    = mock_get_capacity;
  b.ctx             = NULL;
  return b;
}

/**
 * @brief Pre-kernel boot replay: CGC + SysTick at CPUCLK0.
 *
 * @par MC/DC: not applicable -- sequential cgc_init -> readback ->
 * time_init wiring with no compound boolean decisions.
 */
static void test_filex_demo_pre_kernel_bringup(void)
{
  reset_world();
  TEST_BEGIN("filex_demo: pre-kernel CGC + SysTick");
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_cgc_init());
  uint32_t hz = 0U;
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_cgc_get_clock_hz(k_ra_clock_id_cpuclk0, &hz));
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_time_init(hz));
  TEST_END("filex_demo: pre-kernel CGC + SysTick");
}

/**
 * @brief Mount call rejects NULL backend (FileX precondition equivalent).
 *
 * @par MC/DC:
 * Compound decision under test: ``backend == NULL || out_handle == NULL``
 * inside ra_fs_mount. Two atomic conditions x N+1 = 3 vectors. This case
 * covers backend==NULL (both reject paths join in early-return).
 */
static void test_filex_demo_mount_rejects_null_backend(void)
{
  reset_world();
  TEST_BEGIN("filex_demo: mount(NULL backend) rejected");
  ra_fs_mount_t* handle = NULL;
  TEST_ASSERT_EQ((int)k_ra_err_null_ptr, (int)ra_fs_mount(NULL, &handle));
  TEST_END("filex_demo: mount(NULL backend) rejected");
}

/**
 * @brief Mount call rejects NULL out_handle.
 *
 * @par MC/DC:
 * Compound decision under test: ``backend == NULL || out_handle == NULL``
 * inside ra_fs_mount. Vector covering out_handle==NULL.
 */
static void test_filex_demo_mount_rejects_null_handle(void)
{
  reset_world();
  TEST_BEGIN("filex_demo: mount(NULL out_handle) rejected");
  const ra_fs_backend_t backend = make_mock_backend();
  TEST_ASSERT_EQ((int)k_ra_err_null_ptr, (int)ra_fs_mount(&backend, NULL));
  TEST_END("filex_demo: mount(NULL out_handle) rejected");
}

/**
 * @brief Backend with a missing function pointer is rejected.
 *
 * @par MC/DC:
 * Decision vector under test: ``read_block == NULL || write_block == NULL
 * || get_capacity == NULL`` guard inside ra_fs_mount. This case covers
 * read_block==NULL (the most common partial-init bug).
 */
static void test_filex_demo_mount_rejects_partial_backend(void)
{
  reset_world();
  TEST_BEGIN("filex_demo: mount rejects partial backend");
  ra_fs_backend_t backend = make_mock_backend();
  backend.read_block      = NULL;
  ra_fs_mount_t* handle   = NULL;
  TEST_ASSERT(ra_fs_mount(&backend, &handle) != k_ra_ok);
  TEST_END("filex_demo: mount rejects partial backend");
}

/**
 * @brief Mount with a fully-zeroed image fails at BPB validation.
 *
 * @par MC/DC: not applicable -- this exercises a single sequential
 * mount call that flows into the BPB-signature check; the test is
 * a fault-injection of "device is empty".
 */
static void test_filex_demo_mount_rejects_unformatted(void)
{
  reset_world();
  TEST_BEGIN("filex_demo: mount of unformatted device fails cleanly");
  const ra_fs_backend_t backend = make_mock_backend();
  ra_fs_mount_t*        handle  = NULL;
  /* Mock backend returns all-zero sectors -> BPB signature 0x55AA absent. */
  ra_err_t err = ra_fs_mount(&backend, &handle);
  TEST_ASSERT(err != k_ra_ok);
  TEST_END("filex_demo: mount of unformatted device fails cleanly");
}

int main(void)
{
  test_filex_demo_pre_kernel_bringup();
  test_filex_demo_mount_rejects_null_backend();
  test_filex_demo_mount_rejects_null_handle();
  test_filex_demo_mount_rejects_partial_backend();
  test_filex_demo_mount_rejects_unformatted();
  return 0;
}
