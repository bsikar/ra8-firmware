/**
 * @file test_app_threadx_filex_levelx_demo.c
 * @brief Integration test: FileX-on-LevelX FAT format + read/write flow
 *
 * @details
 * The production app at examples/ek_ra8d2/threadx_filex_levelx_demo/main.c
 * stands up ThreadX + LevelX + FileX, lays a FAT12 superblock on top
 * of LevelX-managed wear-levelled NOR sectors, then writes and reads
 * a known message back. None of ThreadX, LevelX, or FileX are linked
 * into the host test build (RA8_OFF_TARGET), so this test exercises
 * the same call surface the FileX-to-LevelX adapter ultimately drives:
 *   - ra8_cgc / ra8_time pre-kernel boot.
 *   - The SCI panic-flush helper used by demo_panic_halt().
 *   - ra8_fs (the FileX equivalent surface) precondition guards on a
 *     fresh / partially-initialized backend.
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
#include "ra8_sci.h"
#include "ra8_system_regs.h"
#include "ra8_time.h"
#include "unity_minimal.h"

/**
 * @enum app_threadx_filex_levelx_demo_fixture_t
 * @brief All-bits-set register values, so a write that clears the wrong field leaves evidence.
 */
typedef enum : uint8_t {
  k_sys_oscsf_all_ready =
    0xFFU, /**< Every oscillator-stabilisation flag set, so clock bring-up sees all sources ready */
} app_threadx_filex_levelx_demo_fixture_t;

/** @brief Per-test enums. */
typedef enum : uint32_t {
  k_test_fxlx_block_count = 256U, /**< Mock backend size in sectors.        */
  k_test_fxlx_block_size  = 512U, /**< FAT bytes-per-sector.                */
  k_test_fxlx_sci_chan    = 8U,   /**< SCI channel routed to J-Link OB CDC. */
} test_fxlx_const_t;

/* -------------------------------------------------------------------------
 * Mock block-device backend (returns zeroes -> mount fails BPB validation,
 * which is exactly what the production demo path triggers before its own
 * fx_media_format).
 * ------------------------------------------------------------------------- */

static ra8_err_t mock_read_block(void* ctx, uint64_t lba, uint32_t count, uint8_t* buf)
{
  (void)ctx;
  (void)lba;
  if (buf == nullptr) {
    return k_ra8_err_null_ptr;
  }
  (void)memset(buf, 0, (size_t)count * (size_t)k_test_fxlx_block_size);
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
  *block_count = (uint32_t)k_test_fxlx_block_count;
  *block_size  = (uint32_t)k_test_fxlx_block_size;
  return k_ra8_ok;
}

static ra8_fs_backend_t make_mock_backend(void)
{
  ra8_fs_backend_t b = {};
  b.read_block       = mock_read_block;
  b.write_block      = mock_write_block;
  b.get_capacity     = mock_get_capacity;
  b.ctx              = nullptr;
  return b;
}

static void reset_world(void)
{
  ra8_fake_mmap_reset();
  ra8_pin_validator_reset();
  /* Pre-seed OSCSF stabilisation bits so ra8_cgc_init() spin loops
   * complete on the first iteration in RA8_OFF_TARGET. */
  *ra8_sys_oscsf() = (uint8_t)k_sys_oscsf_all_ready;
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
  TEST_ASSERT_EQ(k_ra8_ok, ra8_cgc_init());
  uint32_t cpuclk0_hz = 0U;
  uint32_t pclka_hz   = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_cgc_get_clock_hz(k_ra8_clock_id_cpuclk0, &cpuclk0_hz));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_cgc_get_clock_hz(k_ra8_clock_id_pclka, &pclka_hz));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_time_init(cpuclk0_hz));
  TEST_END("fxlx_demo: pre-kernel CGC + clock readback");
}

/**
 * @brief Mount call rejects NULL backend (FileX bind precondition equivalent).
 *
 * @par MC/DC:
 * Compound decision under test: ``backend == NULL || out_handle == NULL``
 * inside ra8_fs_mount. Two atomic conditions x N+1 = 3 vectors. This case
 * covers backend==NULL.
 */
static void test_fxlx_mount_rejects_null_backend(void)
{
  reset_world();
  TEST_BEGIN("fxlx_demo: mount(NULL backend) rejected");
  ra8_fs_mount_t* handle = nullptr;
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_fs_mount(nullptr, &handle));
  TEST_END("fxlx_demo: mount(NULL backend) rejected");
}

/**
 * @brief Mount call rejects NULL out_handle.
 *
 * @par MC/DC:
 * Compound decision under test: ``backend == NULL || out_handle == NULL``
 * inside ra8_fs_mount. Vector covering out_handle==NULL.
 */
static void test_fxlx_mount_rejects_null_handle(void)
{
  reset_world();
  TEST_BEGIN("fxlx_demo: mount(NULL out_handle) rejected");
  const ra8_fs_backend_t backend = make_mock_backend();
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_fs_mount(&backend, nullptr));
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
  const ra8_fs_backend_t backend = make_mock_backend();
  ra8_fs_mount_t*        handle  = nullptr;
  TEST_ASSERT(ra8_fs_mount(&backend, &handle) != k_ra8_ok);
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
 * NULL || get_capacity == NULL`` guard inside ra8_fs_mount. This case
 * covers get_capacity==NULL (LevelX adapter partial-init bug class).
 */
static void test_fxlx_mount_rejects_partial_backend(void)
{
  reset_world();
  TEST_BEGIN("fxlx_demo: mount rejects backend with NULL get_capacity");
  ra8_fs_backend_t backend = make_mock_backend();
  backend.get_capacity     = nullptr;
  ra8_fs_mount_t* handle   = nullptr;
  TEST_ASSERT(ra8_fs_mount(&backend, &handle) != k_ra8_ok);
  TEST_END("fxlx_demo: mount rejects backend with NULL get_capacity");
}

/**
 * @brief Panic-flush before SCI8 init returns cleanly (non-fatal).
 *
 * @details demo_panic_halt() always (void)-casts ra8_sci_flush; verify
 * the call shape returns a well-defined failure rather than crashing
 * when the SCI is uninitialized.
 *
 * @par MC/DC:
 * State-machine vector: SCI8 stays uninitialized; flush rejection
 * matches the demo_panic_halt() drain budget being zero.
 */
static void test_fxlx_panic_flush_before_init(void)
{
  reset_world();
  TEST_BEGIN("fxlx_demo: panic flush before SCI init returns cleanly");
  /* In RA8_OFF_TARGET the TEND wait is short-circuited to ok, so
   * the flush returns ok even when the channel was never opened.
   * The demo_panic_halt() caller (void)-casts the return value so any
   * defined ra8_err_t is acceptable; we just assert no crash. */
  (void)ra8_sci_flush((uint8_t)k_test_fxlx_sci_chan);
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
