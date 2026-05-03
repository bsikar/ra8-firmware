/**
 * @file test_app_threadx_filex_levelx_demo.c
 * @brief Integration test: FileX-on-LevelX FAT format + read/write flow
 *
 * @details
 * The production app at examples/ek_ra8d2/threadx_filex_levelx_demo/main.c
 * stands up ThreadX + LevelX + FileX, lays a FAT12 superblock on top
 * of LevelX-managed wear-levelled NOR sectors, then writes and reads
 * a known message back. None of ThreadX, LevelX, or FileX are linked
 * into the host test build (RA_SIMULATOR_MODE), so this test exercises
 * the same call surface the FileX-to-LevelX adapter ultimately drives:
 *   - ra_cgc / ra_time pre-kernel boot.
 *   - The SCI panic-flush helper used by demo_panic_halt().
 *   - ra_fs (the FileX equivalent surface) precondition guards on a
 *     fresh / partially-initialised backend.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>
#include <string.h>

#include "ra8d2_system_regs.h"
#include "ra_cgc.h"
#include "ra_err.h"
#include "ra_fs.h"
#include "ra_pin_validator.h"
#include "ra_sci.h"
#include "ra_sim_mmap.h"
#include "ra_time.h"
#include "unity_minimal.h"

/** @brief Per-test enums. */
typedef enum : uint32_t {
  k_test_fxlx_block_count = 256U, /**< Mock backend size in sectors.        */
  k_test_fxlx_block_size  = 512U, /**< FAT bytes-per-sector.                 */
  k_test_fxlx_sci_chan    = 8U,   /**< SCI channel routed to J-Link OB CDC. */
} test_fxlx_const_t;

/* -------------------------------------------------------------------------
 * Mock block-device backend (returns zeroes -> mount fails BPB validation,
 * which is exactly what the production demo path triggers before its own
 * fx_media_format).
 * ------------------------------------------------------------------------- */

static ra_err_t mock_read_block(void* ctx, uint32_t lba, uint32_t count, uint8_t* buf)
{
  (void)ctx;
  (void)lba;
  if (buf == NULL) {
    return k_ra_err_null_ptr;
  }
  (void)memset(buf, 0, (size_t)count * (size_t)k_test_fxlx_block_size);
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
  *block_count = (uint32_t)k_test_fxlx_block_count;
  *block_size  = (uint32_t)k_test_fxlx_block_size;
  return k_ra_ok;
}

static ra_fs_backend_t make_mock_backend(void)
{
  ra_fs_backend_t b = {};
  b.read_block      = mock_read_block;
  b.write_block     = mock_write_block;
  b.get_capacity    = mock_get_capacity;
  b.ctx             = NULL;
  return b;
}

static void reset_world(void)
{
  ra_sim_mmap_reset();
  ra_pin_validator_reset();
  /* Pre-seed OSCSF stabilisation bits so ra_cgc_init() spin loops
   * complete on the first iteration in RA_SIMULATOR_MODE. */
  *ra_sys_oscsf() = (uint8_t)0xFFU;
}

/* -------------------------------------------------------------------------
 * Golden path: pre-kernel + adapter call shape
 * ------------------------------------------------------------------------- */

/**
 * @brief Pre-kernel boot replay: CGC -> CPUCLK0 + PCLKA readback.
 *
 * @par MC/DC: not applicable -- sequential init/teardown, no compound
 * boolean decisions in path.
 */
static void test_fxlx_pre_kernel_bringup(void)
{
  reset_world();
  TEST_BEGIN("fxlx_demo: pre-kernel CGC + clock readback");
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_cgc_init());
  uint32_t cpuclk0_hz = 0U;
  uint32_t pclka_hz   = 0U;
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_cgc_get_clock_hz(k_ra_clock_id_cpuclk0, &cpuclk0_hz));
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_cgc_get_clock_hz(k_ra_clock_id_pclka, &pclka_hz));
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_time_init(cpuclk0_hz));
  TEST_END("fxlx_demo: pre-kernel CGC + clock readback");
}

/**
 * @brief Mount call rejects NULL backend (FileX bind precondition equivalent).
 *
 * @par MC/DC:
 * Compound decision under test: ``backend == NULL || out_handle == NULL``
 * inside ra_fs_mount. Two atomic conditions x N+1 = 3 vectors. This case
 * covers backend==NULL.
 */
static void test_fxlx_mount_rejects_null_backend(void)
{
  reset_world();
  TEST_BEGIN("fxlx_demo: mount(NULL backend) rejected");
  ra_fs_mount_t* handle = NULL;
  TEST_ASSERT_EQ((int)k_ra_err_null_ptr, (int)ra_fs_mount(NULL, &handle));
  TEST_END("fxlx_demo: mount(NULL backend) rejected");
}

/**
 * @brief Mount call rejects NULL out_handle.
 *
 * @par MC/DC:
 * Compound decision under test: ``backend == NULL || out_handle == NULL``
 * inside ra_fs_mount. Vector covering out_handle==NULL.
 */
static void test_fxlx_mount_rejects_null_handle(void)
{
  reset_world();
  TEST_BEGIN("fxlx_demo: mount(NULL out_handle) rejected");
  const ra_fs_backend_t backend = make_mock_backend();
  TEST_ASSERT_EQ((int)k_ra_err_null_ptr, (int)ra_fs_mount(&backend, NULL));
  TEST_END("fxlx_demo: mount(NULL out_handle) rejected");
}

/**
 * @brief Mount of an unformatted (zeroed) device fails cleanly.
 *
 * @details Equivalent to the path the production demo deliberately
 * walks: a fresh LevelX partition has no BPB, so fx_media_open returns
 * an error -- the demo then falls through to fx_media_format. We model
 * the open-on-empty-device step here.
 *
 * @par MC/DC: not applicable -- exercises a single sequential mount
 * call that flows into the BPB-signature check; this is a fault
 * injection of "device is empty".
 */
static void test_fxlx_mount_rejects_unformatted(void)
{
  reset_world();
  TEST_BEGIN("fxlx_demo: mount of unformatted device fails");
  const ra_fs_backend_t backend = make_mock_backend();
  ra_fs_mount_t*        handle  = NULL;
  TEST_ASSERT(ra_fs_mount(&backend, &handle) != k_ra_ok);
  TEST_END("fxlx_demo: mount of unformatted device fails");
}

/* -------------------------------------------------------------------------
 * Edge / failure paths
 * ------------------------------------------------------------------------- */

/**
 * @brief Backend with a missing get_capacity is rejected at mount.
 *
 * @par MC/DC:
 * Decision vector under test: ``read_block == NULL || write_block ==
 * NULL || get_capacity == NULL`` guard inside ra_fs_mount. This case
 * covers get_capacity==NULL (LevelX adapter partial-init bug class).
 */
static void test_fxlx_mount_rejects_partial_backend(void)
{
  reset_world();
  TEST_BEGIN("fxlx_demo: mount rejects backend with NULL get_capacity");
  ra_fs_backend_t backend = make_mock_backend();
  backend.get_capacity    = NULL;
  ra_fs_mount_t* handle   = NULL;
  TEST_ASSERT(ra_fs_mount(&backend, &handle) != k_ra_ok);
  TEST_END("fxlx_demo: mount rejects backend with NULL get_capacity");
}

/**
 * @brief Panic-flush before SCI8 init returns cleanly (non-fatal).
 *
 * @details demo_panic_halt() always (void)-casts ra_sci_flush; verify
 * the call shape returns a well-defined failure rather than crashing
 * when the SCI is uninitialised.
 *
 * @par MC/DC:
 * State-machine vector: SCI8 stays uninitialised; flush rejection
 * matches the demo_panic_halt() drain budget being zero.
 */
static void test_fxlx_panic_flush_before_init(void)
{
  reset_world();
  TEST_BEGIN("fxlx_demo: panic flush before SCI init returns cleanly");
  TEST_ASSERT(ra_sci_flush((uint8_t)k_test_fxlx_sci_chan) != k_ra_ok);
  TEST_END("fxlx_demo: panic flush before SCI init returns cleanly");
}

int main(void)
{
  test_fxlx_pre_kernel_bringup();
  test_fxlx_mount_rejects_null_backend();
  test_fxlx_mount_rejects_null_handle();
  test_fxlx_mount_rejects_unformatted();
  test_fxlx_mount_rejects_partial_backend();
  test_fxlx_panic_flush_before_init();
  return 0;
}
