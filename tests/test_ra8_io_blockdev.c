/**
 * @file test_ra8_io_blockdev.c
 * @brief Unit tests for the ra8_io block-device fabric (Phase 1, issue #156).
 *
 * @details
 * Exercises the RAM backend, the dispatcher's optional-callback handling
 * (NULL erase / NULL sync), the capability query, and the end-to-end
 * `ra8_fs_backend_t` bridge by formatting + mounting a real FAT volume on a
 * RAM block device and round-tripping a file.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>
#include <string.h>

#include "ra8_err.h"
#include "ra8_fs.h"
#include "ra8_io_blockdev.h"
#include "ra8_io_blockdev_internal.h"
#include "ra8_io_blockdev_ram.h"
#include "unity_minimal.h"

/**
 * @enum io_blockdev_uint8_const_t
 * @brief Named uint8_t constants used by this file.
 *
 * @details
 * Every literal this translation unit needs, named so the
 * value's role is visible at the point of use (CLAUDE.md
 * "No Magic Numbers").
 */
typedef enum : uint8_t {
  k_io_blockdev_i_7  = 7U,
  k_io_blockdev_i_ff = 0xFFU,
} io_blockdev_uint8_const_t;

/**
 * @enum test_const_t
 * @brief Sizes used by the fixtures.
 */
typedef enum : uint32_t {
  k_test_disk_blocks  = 16384, /**< 8 MiB ramdisk -- comfortably FAT16.        */
  k_test_small_blocks = 8,     /**< Tiny device for bounds / fake-iface tests. */
  k_test_payload_len  = 100,   /**< Bytes written through the FAT bridge.      */
} test_const_t;

/** @brief 8 MiB backing buffer for the FAT bridge fixture. */
static uint8_t s_disk[(size_t)k_test_disk_blocks * (size_t)k_ra8_io_block_size_bytes];

/** @brief One-block scratch for read/write round-trips. */
static uint8_t s_small[(size_t)k_test_small_blocks * (size_t)k_ra8_io_block_size_bytes];

/* =============================================================================
 * RAM backend
 * =============================================================================
 */

/**
 * @par MC/DC:
 * (no compound decisions under test -- init rejects each NULL argument and the
 * zero-size case via independent single-condition guards)
 */
static void test_ram_init_validation(void)
{
  TEST_BEGIN("ram init validation");
  ra8_io_blockdev_t           bd    = {};
  ra8_io_blockdev_ram_state_t state = {};
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_io_blockdev_ram_init(nullptr, &state, s_small, k_test_small_blocks, false));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_io_blockdev_ram_init(&bd, nullptr, s_small, k_test_small_blocks, false));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_io_blockdev_ram_init(&bd, &state, nullptr, k_test_small_blocks, false));
  TEST_ASSERT_EQ(k_ra8_err_invalid_size, ra8_io_blockdev_ram_init(&bd, &state, s_small, 0, false));
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_io_blockdev_ram_init(&bd, &state, s_small, k_test_small_blocks, false));
  TEST_END("ram init validation");
}

/**
 * @par MC/DC:
 * (no compound decisions under test -- asserts each reported capability field)
 */
static void test_ram_caps(void)
{
  TEST_BEGIN("ram caps");
  ra8_io_blockdev_t           bd    = {};
  ra8_io_blockdev_ram_state_t state = {};
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_io_blockdev_ram_init(&bd, &state, s_small, k_test_small_blocks, false));
  ra8_io_blockdev_caps_t caps = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_blockdev_get_caps(&bd, &caps));
  TEST_ASSERT_EQ(k_test_small_blocks, caps.block_count);
  TEST_ASSERT_EQ(k_ra8_io_block_size_bytes, caps.logical_block_bytes);
  TEST_ASSERT_EQ(1, caps.erase_unit_blocks);
  TEST_ASSERT_EQ(k_ra8_io_erase_value_zero, caps.erase_value);
  TEST_ASSERT(!caps.must_erase_before_write);
  TEST_ASSERT(!caps.read_only);
  TEST_END("ram caps");
}

/**
 * @par MC/DC:
 * (no compound decisions under test -- write then read-back equality + a
 * multi-block transfer)
 */
static void test_ram_read_write_roundtrip(void)
{
  TEST_BEGIN("ram read/write round-trip");
  ra8_io_blockdev_t           bd    = {};
  ra8_io_blockdev_ram_state_t state = {};
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_io_blockdev_ram_init(&bd, &state, s_small, k_test_small_blocks, false));

  uint8_t out[(size_t)k_ra8_io_block_size_bytes];
  for (uint32_t i = 0; i < (uint32_t)k_ra8_io_block_size_bytes; ++i) {
    out[i] = (uint8_t)(i & k_io_blockdev_i_ff);
  }
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_blockdev_write(&bd, 2, 1, out));

  uint8_t in[(size_t)k_ra8_io_block_size_bytes];
  (void)memset(in, 0, sizeof(in));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_blockdev_read(&bd, 2, 1, in));
  TEST_ASSERT(memcmp(in, out, sizeof(in)) == 0);
  TEST_END("ram read/write round-trip");
}

/**
 * @par MC/DC:
 * (no compound decisions under test -- the bounds helper is two independent
 * single-condition guards; each rejection path is hit)
 */
static void test_ram_bounds(void)
{
  TEST_BEGIN("ram bounds");
  ra8_io_blockdev_t           bd    = {};
  ra8_io_blockdev_ram_state_t state = {};
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_io_blockdev_ram_init(&bd, &state, s_small, k_test_small_blocks, false));
  uint8_t buf[(size_t)k_ra8_io_block_size_bytes] = {};
  /* count exceeds capacity */
  TEST_ASSERT_EQ(k_ra8_err_out_of_range,
                 ra8_io_blockdev_read(&bd, 0, k_test_small_blocks + 1U, buf));
  /* lba+count walks off the end */
  TEST_ASSERT_EQ(k_ra8_err_out_of_range, ra8_io_blockdev_write(&bd, k_test_small_blocks, 1, buf));
  TEST_END("ram bounds");
}

/**
 * @par MC/DC:
 * (no compound decisions under test -- erase zero-fills; read-back is all zero)
 */
static void test_ram_erase(void)
{
  TEST_BEGIN("ram erase");
  ra8_io_blockdev_t           bd    = {};
  ra8_io_blockdev_ram_state_t state = {};
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_io_blockdev_ram_init(&bd, &state, s_small, k_test_small_blocks, false));
  uint8_t ones[(size_t)k_ra8_io_block_size_bytes];
  (void)memset(ones, 0xFF, sizeof(ones));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_blockdev_write(&bd, 1, 1, ones));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_blockdev_erase(&bd, 1, 1));

  uint8_t in[(size_t)k_ra8_io_block_size_bytes];
  (void)memset(in, 0xAA, sizeof(in));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_blockdev_read(&bd, 1, 1, in));
  uint32_t nonzero = 0;
  for (uint32_t i = 0; i < (uint32_t)k_ra8_io_block_size_bytes; ++i) {
    if (in[i] != 0U) {
      nonzero++;
    }
  }
  TEST_ASSERT_EQ(0, nonzero);
  TEST_END("ram erase");
}

/**
 * @par MC/DC:
 * (no compound decisions under test -- a read-only device rejects write/erase
 * but still serves reads)
 */
static void test_ram_read_only(void)
{
  TEST_BEGIN("ram read-only");
  ra8_io_blockdev_t           bd    = {};
  ra8_io_blockdev_ram_state_t state = {};
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_io_blockdev_ram_init(&bd, &state, s_small, k_test_small_blocks, true));
  uint8_t buf[(size_t)k_ra8_io_block_size_bytes] = {};
  TEST_ASSERT_EQ(k_ra8_err_not_supported, ra8_io_blockdev_write(&bd, 0, 1, buf));
  TEST_ASSERT_EQ(k_ra8_err_not_supported, ra8_io_blockdev_erase(&bd, 0, 1));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_blockdev_read(&bd, 0, 1, buf));
  ra8_io_blockdev_caps_t caps = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_blockdev_get_caps(&bd, &caps));
  TEST_ASSERT(caps.read_only);
  TEST_END("ram read-only");
}

/**
 * @par MC/DC:
 * (no compound decisions under test -- an unbound / NULL handle is rejected on
 * every entry point)
 */
static void test_unbound_handle(void)
{
  TEST_BEGIN("unbound handle");
  ra8_io_blockdev_t      bd                                     = {}; /* iface == nullptr */
  uint8_t                buf[(size_t)k_ra8_io_block_size_bytes] = {};
  ra8_io_blockdev_caps_t caps                                   = {};
  TEST_ASSERT_EQ(k_ra8_err_not_initialized, ra8_io_blockdev_read(&bd, 0, 1, buf));
  TEST_ASSERT_EQ(k_ra8_err_not_initialized, ra8_io_blockdev_write(&bd, 0, 1, buf));
  TEST_ASSERT_EQ(k_ra8_err_not_initialized, ra8_io_blockdev_get_caps(&bd, &caps));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_io_blockdev_read(nullptr, 0, 1, buf));
  TEST_END("unbound handle");
}

/* =============================================================================
 * Dispatcher optional-callback handling (fake ifaces)
 * =============================================================================
 */

/** @brief Fake read: succeed without touching memory. */
/* The pointer parameters below cannot be const: this mock implements a
 * function-pointer interface (the DI seam under test), so its signature is
 * fixed by the typedef it is assigned to -- adding const changes the
 * function type and the assignment stops compiling. */
// NOLINTNEXTLINE(readability-non-const-parameter)
static ra8_err_t fk_read(void* c, uint32_t l, uint32_t n, uint8_t* b)
{
  (void)c;
  (void)l;
  (void)n;
  (void)b;
  return k_ra8_ok;
}

/** @brief Fake write: succeed without touching memory. */
static ra8_err_t fk_write(void* c, uint32_t l, uint32_t n, const uint8_t* b)
{
  (void)c;
  (void)l;
  (void)n;
  (void)b;
  return k_ra8_ok;
}

/** @brief Fake erase: succeed without touching memory. */
static ra8_err_t fk_erase(void* c, uint32_t l, uint32_t n)
{
  (void)c;
  (void)l;
  (void)n;
  return k_ra8_ok;
}

/** @brief Fake caps reporting an all-ones (flash-like) erase value. */
static ra8_err_t fk_caps_ones(const void* c, ra8_io_blockdev_caps_t* out)
{
  (void)c;
  *out                     = (ra8_io_blockdev_caps_t){};
  out->block_count         = k_test_small_blocks;
  out->erase_unit_blocks   = 1;
  out->logical_block_bytes = (uint16_t)k_ra8_io_block_size_bytes;
  out->erase_value         = (uint8_t)k_ra8_io_erase_value_ones;
  return k_ra8_ok;
}

/** @brief Fake backend with NO erase / NO sync callback. */
static const ra8_io_blockdev_iface_t k_fk_iface_no_erase = {
  .read     = fk_read,
  .write    = fk_write,
  .erase    = nullptr,
  .get_caps = fk_caps_ones,
  .sync     = nullptr,
};

/** @brief Fake all-ones-erase backend (erase present, value 0xFF). */
static const ra8_io_blockdev_iface_t k_fk_iface_ones = {
  .read     = fk_read,
  .write    = fk_write,
  .erase    = fk_erase,
  .get_caps = fk_caps_ones,
  .sync     = nullptr,
};

/**
 * @par MC/DC:
 * (no compound decisions under test -- a NULL erase callback maps to
 * not-supported, a NULL sync callback maps to ok)
 */
static void test_dispatch_optional_callbacks(void)
{
  TEST_BEGIN("dispatch optional callbacks");
  ra8_io_blockdev_t bd = {.iface = &k_fk_iface_no_erase, .ctx = nullptr};
  TEST_ASSERT_EQ(k_ra8_err_not_supported, ra8_io_blockdev_erase(&bd, 0, 1));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_blockdev_sync(&bd));
  TEST_END("dispatch optional callbacks");
}

/**
 * @par MC/DC:
 * (no compound decisions under test -- the FS erase trampoline refuses to
 * advertise erase on an all-ones medium so the formatter zero-writes)
 */
static void test_fs_bridge_erase_value_gate(void)
{
  TEST_BEGIN("fs bridge erase-value gate");
  ra8_io_blockdev_t bd = {.iface = &k_fk_iface_ones, .ctx = nullptr};
  ra8_fs_backend_t  be = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_blockdev_as_fs_backend(&bd, &be));
  TEST_ASSERT_NOT_NULL(be.erase_blocks);
  /* all-ones medium: erase trampoline declines so ra8_fs zero-writes */
  TEST_ASSERT_EQ(k_ra8_err_not_supported, be.erase_blocks(be.ctx, 0, 1));
  TEST_END("fs bridge erase-value gate");
}

/* =============================================================================
 * End-to-end: real FAT on a RAM block device through the bridge
 * =============================================================================
 */

/**
 * @par MC/DC:
 * (no compound decisions under test -- formats FAT16 on the ramdisk via the
 * bridge, mounts it, writes a file, and reads it back byte-identical)
 */
static void test_fs_bridge_fat_roundtrip(void)
{
  TEST_BEGIN("fs bridge FAT round-trip");
  ra8_io_blockdev_t           bd    = {};
  ra8_io_blockdev_ram_state_t state = {};
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_io_blockdev_ram_init(&bd, &state, s_disk, k_test_disk_blocks, false));
  ra8_fs_backend_t be = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_blockdev_as_fs_backend(&bd, &be));

  ra8_fs_format_opts_t opts = {};
  opts.type                 = k_ra8_fs_type_fat16;
  opts.label                = "RAIO";
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_format(&be, &opts));

  ra8_fs_mount_t* mnt = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&be, &mnt));
  TEST_ASSERT_EQ(k_ra8_fs_type_fat16, mnt->type);

  uint8_t payload[(size_t)k_test_payload_len];
  for (uint32_t i = 0; i < (uint32_t)k_test_payload_len; ++i) {
    payload[i] = (uint8_t)(((i * k_io_blockdev_i_7) + 3U) & k_io_blockdev_i_ff);
  }
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_write_file(mnt, "HELLO.BIN", payload, k_test_payload_len));

  ra8_fs_file_t* f = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_open(mnt, "HELLO.BIN", k_ra8_fs_mode_read, &f));
  uint8_t  got[(size_t)k_test_payload_len] = {};
  uint32_t got_len                         = 0;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_read(f, got, k_test_payload_len, &got_len));
  TEST_ASSERT_EQ(k_test_payload_len, got_len);
  TEST_ASSERT(memcmp(got, payload, sizeof(payload)) == 0);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_close(f));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(mnt));
  TEST_END("fs bridge FAT round-trip");
}

int32_t main(void)
{
  test_ram_init_validation();
  test_ram_caps();
  test_ram_read_write_roundtrip();
  test_ram_bounds();
  test_ram_erase();
  test_ram_read_only();
  test_unbound_handle();
  test_dispatch_optional_callbacks();
  test_fs_bridge_erase_value_gate();
  test_fs_bridge_fat_roundtrip();
  (void)fprintf(stderr, "[OK  ] test_ra8_io_blockdev.c\n");
  return 0;
}
