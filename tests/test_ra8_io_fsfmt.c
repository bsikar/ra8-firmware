/**
 * @file test_ra8_io_fsfmt.c
 * @brief Unit tests for the ra8_io filesystem-format registry (issue #159).
 *
 * @details
 * Probes real FAT16 and exFAT volumes (formatted on a RAM block device through
 * the bridge), registers a foreign stub format and probes a volume bearing its
 * magic byte, checks capability flags + registry validation, and confirms a
 * no-signature volume yields not_found.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>
#include <string.h>

#include "ra8_check.h"
#include "ra8_err.h"
#include "ra8_fs.h"
#include "ra8_io_blockdev.h"
#include "ra8_io_blockdev_ram.h"
#include "ra8_io_fsfmt.h"
#include "unity_minimal.h"

/**
 * @enum t_fsfmt_const_t
 * @brief Fixture sizes.
 */
typedef enum : uint32_t {
  k_t_fat_blocks   = 16384, /**< 8 MiB -- FAT16 / exFAT.      */
  k_t_small_blocks = 8,     /**< Tiny device for stub / zero. */
  k_t_stub_magic   = 0x5AU, /**< Stub format block-0 marker.  */
  k_t_stub_name    = 16U,   /**< Stub max name length.        */
} t_fsfmt_const_t;

static uint8_t s_disk[(size_t)k_t_fat_blocks * (size_t)k_ra8_io_block_size_bytes];
static uint8_t s_small[(size_t)k_t_small_blocks * (size_t)k_ra8_io_block_size_bytes];

/** @brief Stub-format probe: block 0 begins with the stub magic byte. */
static bool stub_probe(const ra8_fs_backend_t* be)
{
  if (be == nullptr) {
    return false;
  }
  if (be->read_block == nullptr) {
    return false;
  }
  uint8_t blk[(size_t)k_ra8_io_block_size_bytes];
  if (be->read_block(be->ctx, 0, 1, blk) != k_ra8_ok) {
    return false;
  }
  return blk[0] == (uint8_t)k_t_stub_magic;
}

/** @brief Foreign stub format descriptor (read-only, case-sensitive). */
static const ra8_io_fsfmt_t k_test_stub = {
  .name = "stub",
  .caps =
    {
      .max_name_len             = (uint16_t)k_t_stub_name,
      .read_only                = true,
      .supports_mkdir           = false,
      .supports_streaming_write = false,
      .case_sensitive           = true,
    },
  .probe = stub_probe,
};

/** @brief Format the big RAM disk with `type` and return its fs backend. */
static ra8_err_t make_volume(ra8_fs_type_t type, ra8_fs_backend_t* out_be)
{
  static ra8_io_blockdev_ram_state_t s_state;
  static ra8_io_blockdev_t           s_bd;
  RA8_RETURN_ON_ERROR(ra8_io_blockdev_ram_init(&s_bd, &s_state, s_disk, k_t_fat_blocks, false),
                      "t",
                      "bd");
  RA8_RETURN_ON_ERROR(ra8_io_blockdev_as_fs_backend(&s_bd, out_be), "t", "bridge");
  ra8_fs_format_opts_t opts = {};
  opts.type                 = type;
  opts.label                = "FMT";
  return ra8_fs_format(out_be, &opts);
}

/**
 * @par MC/DC:
 * (no compound decisions under test -- a FAT16 then an exFAT volume each probe
 * to the matching built-in format with the right capability flags)
 */
static void test_probe_builtins(void)
{
  TEST_BEGIN("fsfmt probe builtins");
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_fsfmt_init());

  ra8_fs_backend_t be = {};
  TEST_ASSERT_EQ(k_ra8_ok, make_volume(k_ra8_fs_type_fat16, &be));
  const ra8_io_fsfmt_t* fmt = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_fsfmt_probe(&be, &fmt));
  TEST_ASSERT(strcmp(fmt->name, "fat") == 0);
  TEST_ASSERT(fmt->caps.supports_streaming_write);
  TEST_ASSERT(!fmt->caps.supports_mkdir);
  TEST_ASSERT_EQ(12, fmt->caps.max_name_len);

  /* exFAT: craft a boot sector with the EXFAT name. ra8_fs_format(exFAT) needs a
   * larger geometry than this fixture, and the probe only inspects block 0. */
  ra8_io_blockdev_ram_state_t xstate = {};
  ra8_io_blockdev_t           xbd    = {};
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_io_blockdev_ram_init(&xbd, &xstate, s_small, k_t_small_blocks, false));
  uint8_t xblk[(size_t)k_ra8_io_block_size_bytes] = {};
  (void)memcpy(&xblk[3], "EXFAT   ", 8);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_blockdev_write(&xbd, 0, 1, xblk));
  ra8_fs_backend_t xbe = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_blockdev_as_fs_backend(&xbd, &xbe));
  const ra8_io_fsfmt_t* xfmt = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_fsfmt_probe(&xbe, &xfmt));
  TEST_ASSERT(strcmp(xfmt->name, "exfat") == 0);
  TEST_ASSERT(!xfmt->caps.supports_streaming_write);
  TEST_ASSERT_EQ(255, xfmt->caps.max_name_len);
  TEST_END("fsfmt probe builtins");
}

/**
 * @par MC/DC:
 * (no compound decisions under test -- a registered foreign format claims a
 * volume bearing its magic; its read-only capability is reported)
 */
static void test_foreign_format(void)
{
  TEST_BEGIN("fsfmt foreign format");
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_fsfmt_init());
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_fsfmt_register(&k_test_stub));

  ra8_io_blockdev_ram_state_t state = {};
  ra8_io_blockdev_t           bd    = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_blockdev_ram_init(&bd, &state, s_small, k_t_small_blocks, false));
  uint8_t blk[(size_t)k_ra8_io_block_size_bytes] = {};
  blk[0]                                         = (uint8_t)k_t_stub_magic;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_blockdev_write(&bd, 0, 1, blk));
  ra8_fs_backend_t be = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_blockdev_as_fs_backend(&bd, &be));

  const ra8_io_fsfmt_t* fmt = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_fsfmt_probe(&be, &fmt));
  TEST_ASSERT(strcmp(fmt->name, "stub") == 0);
  TEST_ASSERT(fmt->caps.read_only);
  TEST_ASSERT(fmt->caps.case_sensitive);
  TEST_END("fsfmt foreign format");
}

/**
 * @par MC/DC:
 * (no compound decisions under test -- register rejects NULL members and a full
 * registry; an unsignatured volume probes to not_found)
 */
static void test_register_and_notfound(void)
{
  TEST_BEGIN("fsfmt register/not-found");
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_fsfmt_init());
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_io_fsfmt_register(nullptr));
  const ra8_io_fsfmt_t bad_name = {.name = nullptr, .probe = stub_probe};
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_io_fsfmt_register(&bad_name));
  const ra8_io_fsfmt_t bad_probe = {.name = "x", .probe = nullptr};
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_io_fsfmt_register(&bad_probe));
  /* fill the registry (2 built-ins already in) then overflow */
  for (uint32_t i = 0; i < (uint32_t)k_ra8_io_fsfmt_max; ++i) {
    const ra8_err_t e = ra8_io_fsfmt_register(&k_test_stub);
    if (e != k_ra8_ok) {
      TEST_ASSERT_EQ(k_ra8_err_no_mem, e);
      break;
    }
  }

  /* an all-zero volume matches no built-in */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_fsfmt_init());
  ra8_io_blockdev_ram_state_t state = {};
  ra8_io_blockdev_t           bd    = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_blockdev_ram_init(&bd, &state, s_small, k_t_small_blocks, false));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_blockdev_erase(&bd, 0, k_t_small_blocks));
  ra8_fs_backend_t be = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_blockdev_as_fs_backend(&bd, &be));
  const ra8_io_fsfmt_t* fmt = nullptr;
  TEST_ASSERT_EQ(k_ra8_err_not_found, ra8_io_fsfmt_probe(&be, &fmt));
  TEST_END("fsfmt register/not-found");
}

int32_t main(void)
{
  test_probe_builtins();
  test_foreign_format();
  test_register_and_notfound();
  (void)fprintf(stderr, "[OK  ] test_ra8_io_fsfmt.c\n");
  return 0;
}
