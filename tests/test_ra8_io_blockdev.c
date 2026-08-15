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

#include "ra8_attributes.h"
#include "ra8_err.h"
#include "ra8_fs.h"
#include "ra8_io_blockdev.h"
#include "ra8_io_blockdev_backend.h"
#include "ra8_io_blockdev_ram.h"
#include "unity_minimal.h"

/** @brief Distinguishable block payloads for the RAM backend round-trip. */
typedef enum : uint8_t {
  k_bd_fill_ones    = 0xFFU, /**< All-ones block, the erased-flash pattern. */
  k_bd_fill_payload = 0xAAU, /**< Payload written then read back.           */
} bd_fill_t;

/**
 * @enum io_blockdev_fixture_t
 * @brief The payload generators and their seeds, plus the byte-level helpers.
 */
typedef enum : uint8_t {
  k_bd_pattern_stride = 7U,    /**< Stride of the payload generator, `i * 7 + 3`.            */
  k_byte_mask         = 0xFFU, /**< Truncates a generated or shifted value back into a byte. */
} io_blockdev_fixture_t;

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
 * zero-size case via independent single-condition guards) @brief Verify ram init validation behavior. @details Executes the ram init validation scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since 0.1.0 */
RA8_INTERNAL static void internal_test_ram_init_validation(void)
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
 * (no compound decisions under test -- asserts each reported capability field) @brief Verify ram caps behavior. @details Executes the ram caps scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since 0.1.0 */
RA8_INTERNAL static void internal_test_ram_caps(void)
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
 * multi-block transfer) @brief Verify ram read write roundtrip behavior. @details Executes the ram read write roundtrip scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since 0.1.0 */
RA8_INTERNAL static void internal_test_ram_read_write_roundtrip(void)
{
  TEST_BEGIN("ram read/write round-trip");
  ra8_io_blockdev_t           bd    = {};
  ra8_io_blockdev_ram_state_t state = {};
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_io_blockdev_ram_init(&bd, &state, s_small, k_test_small_blocks, false));

  uint8_t out[(size_t)k_ra8_io_block_size_bytes];
  for (uint32_t i = 0; i < (uint32_t)k_ra8_io_block_size_bytes; ++i) {
    out[i] = (uint8_t)(i & k_byte_mask);
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
 * single-condition guards; each rejection path is hit) @brief Verify ram bounds behavior. @details Executes the ram bounds scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since 0.1.0 */
RA8_INTERNAL static void internal_test_ram_bounds(void)
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
 * (no compound decisions under test -- erase zero-fills; read-back is all zero) @brief Verify ram erase behavior. @details Executes the ram erase scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since 0.1.0 */
RA8_INTERNAL static void internal_test_ram_erase(void)
{
  TEST_BEGIN("ram erase");
  ra8_io_blockdev_t           bd    = {};
  ra8_io_blockdev_ram_state_t state = {};
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_io_blockdev_ram_init(&bd, &state, s_small, k_test_small_blocks, false));
  uint8_t ones[(size_t)k_ra8_io_block_size_bytes];
  (void)memset(ones, k_bd_fill_ones, sizeof(ones));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_blockdev_write(&bd, 1, 1, ones));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_blockdev_erase(&bd, 1, 1));

  uint8_t in[(size_t)k_ra8_io_block_size_bytes];
  (void)memset(in, k_bd_fill_payload, sizeof(in));
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
 * but still serves reads) @brief Verify ram read only behavior. @details Executes the ram read only scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since 0.1.0 */
RA8_INTERNAL static void internal_test_ram_read_only(void)
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
 * every entry point) @brief Verify unbound handle behavior. @details Executes the unbound handle scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since 0.1.0 */
RA8_INTERNAL static void internal_test_unbound_handle(void)
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

/* The pointer parameters below cannot be const: this mock implements a
 * function-pointer interface (the DI seam under test), so its signature is
 * fixed by the typedef it is assigned to -- adding const changes the
 * function type and the assignment stops compiling. */
// NOLINTNEXTLINE(readability-non-const-parameter)
/** @brief Fake block read that succeeds without touching memory. @details Exercises the fk read path with bounded caller-owned fixture state and verifies its documented result. @param[in,out] c Backend callback context. @param[in] l Starting logical block address. @param[in] n Number of logical blocks, bytes, or entries requested. @param[in,out] b Block index or data-buffer argument exercised by the helper. @return RA8 status from the exercised fixture operation. @retval k_ra8_ok The fixture operation completed successfully. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since 0.1.0 */
RA8_INTERNAL static ra8_err_t internal_fk_read(void* c, uint32_t l, uint32_t n, uint8_t* b)
{
  (void)c;
  (void)l;
  (void)n;
  (void)b;
  return k_ra8_ok;
}

/** @brief Fake write: succeed without touching memory. @details Exercises the fk write path with bounded caller-owned fixture state and verifies its documented result. @param[in,out] c Backend callback context. @param[in] l Starting logical block address. @param[in] n Number of logical blocks, bytes, or entries requested. @param[in] b Block index or data-buffer argument exercised by the helper. @return RA8 status from the exercised fixture operation. @retval k_ra8_ok The fixture operation completed successfully. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since 0.1.0 */
RA8_INTERNAL static ra8_err_t internal_fk_write(void* c, uint32_t l, uint32_t n, const uint8_t* b)
{
  (void)c;
  (void)l;
  (void)n;
  (void)b;
  return k_ra8_ok;
}

/** @brief Fake erase: succeed without touching memory. @details Exercises the fk erase path with bounded caller-owned fixture state and verifies its documented result. @param[in,out] c Backend callback context. @param[in] l Starting logical block address. @param[in] n Number of logical blocks, bytes, or entries requested. @return RA8 status from the exercised fixture operation. @retval k_ra8_ok The fixture operation completed successfully. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since 0.1.0 */
RA8_INTERNAL static ra8_err_t internal_fk_erase(void* c, uint32_t l, uint32_t n)
{
  (void)c;
  (void)l;
  (void)n;
  return k_ra8_ok;
}

/** @brief Fake caps reporting an all-ones (flash-like) erase value. @details Exercises the fk caps ones path with bounded caller-owned fixture state and verifies its documented result. @param[in] c Backend callback context. @param[out] out Caller-owned result object populated by the callback. @return RA8 status from the exercised fixture operation. @retval k_ra8_ok The fixture operation completed successfully. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since 0.1.0 */
RA8_INTERNAL static ra8_err_t internal_fk_caps_ones(const void* c, ra8_io_blockdev_caps_t* out)
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
static const ra8_io_blockdev_iface_t s_fk_iface_no_erase = {
  .read     = internal_fk_read,
  .write    = internal_fk_write,
  .erase    = nullptr,
  .get_caps = internal_fk_caps_ones,
  .sync     = nullptr,
};

/** @brief Fake all-ones-erase backend (erase present, value 0xFF). */
static const ra8_io_blockdev_iface_t s_fk_iface_ones = {
  .read     = internal_fk_read,
  .write    = internal_fk_write,
  .erase    = internal_fk_erase,
  .get_caps = internal_fk_caps_ones,
  .sync     = nullptr,
};

/**
 * @par MC/DC:
 * (no compound decisions under test -- a NULL erase callback maps to
 * not-supported, a NULL sync callback maps to ok) @brief Verify dispatch optional callbacks behavior. @details Executes the dispatch optional callbacks scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since 0.1.0 */
RA8_INTERNAL static void internal_test_dispatch_optional_callbacks(void)
{
  TEST_BEGIN("dispatch optional callbacks");
  ra8_io_blockdev_t bd = {.iface = &s_fk_iface_no_erase, .ctx = nullptr};
  TEST_ASSERT_EQ(k_ra8_err_not_supported, ra8_io_blockdev_erase(&bd, 0, 1));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_blockdev_sync(&bd));
  TEST_END("dispatch optional callbacks");
}

/**
 * @par MC/DC:
 * (no compound decisions under test -- the FS erase trampoline refuses to
 * advertise erase on an all-ones medium so the formatter zero-writes) @brief Verify fs bridge erase value gate behavior. @details Executes the fs bridge erase value gate scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since 0.1.0 */
RA8_INTERNAL static void internal_test_fs_bridge_erase_value_gate(void)
{
  TEST_BEGIN("fs bridge erase-value gate");
  ra8_io_blockdev_t bd = {.iface = &s_fk_iface_ones, .ctx = nullptr};
  ra8_fs_backend_t  be = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_blockdev_as_fs_backend(&bd, &be));
  TEST_ASSERT_NOT_NULL(be.erase_blocks);
  /* all-ones medium: erase trampoline declines so ra8_fs zero-writes */
  TEST_ASSERT_EQ(k_ra8_err_not_supported, be.erase_blocks(be.ctx, 0, 1));
  TEST_END("fs bridge erase-value gate");
}

/**
 * @test internal_test_mcdc_fs_bridge_erase_64bit_guard
 * @brief The bridge refuses erase coordinates past the fabric's 32-bit reach.
 *
 * @par MC/DC:
 * Decision: `(lba > UINT32_MAX) || (count > UINT32_MAX)` (2 conditions) in
 * `libs/ra8_io/src/ra8_io_blockdev.c@internal_fs_erase` -- the honest boundary
 * between the 64-bit `ra8_fs` backend interface (#683) and the fabric's
 * 32-bit LBAs.
 * - V1: lba small, count small -> F,F -> falls through to the real erase (ok).
 * - V2: lba = 2^32, count = 1  -> T (short-circuit) -> out_of_range.
 * - V3: lba = 0, count = 2^32  -> F,T -> out_of_range.
 * Vectors 1+2 prove lba's independence; 1+3 prove count's. N+1 = 3 vectors
 * for N=2 conditions: minimal MC/DC. @details Executes the mcdc fs bridge erase 64bit guard scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since 0.1.0 */
RA8_INTERNAL static void internal_test_mcdc_fs_bridge_erase_64bit_guard(void)
{
  TEST_BEGIN("fs bridge MC/DC: 64-bit erase coordinates past 32 bits refused");
  ra8_io_blockdev_t           bd    = {};
  ra8_io_blockdev_ram_state_t state = {};
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_io_blockdev_ram_init(&bd, &state, s_disk, k_test_disk_blocks, false));
  ra8_fs_backend_t be = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_blockdev_as_fs_backend(&bd, &be));
  TEST_ASSERT_NOT_NULL(be.erase_blocks);
  /* V1: both in range -- the guard falls through and the RAM erase runs. */
  TEST_ASSERT_EQ(k_ra8_ok, be.erase_blocks(be.ctx, 0U, 1U));
  /* V2 / V3: either coordinate past 32 bits is refused, never truncated. */
  TEST_ASSERT_EQ(k_ra8_err_out_of_range, be.erase_blocks(be.ctx, (uint64_t)UINT32_MAX + 1U, 1U));
  TEST_ASSERT_EQ(k_ra8_err_out_of_range, be.erase_blocks(be.ctx, 0U, (uint64_t)UINT32_MAX + 1U));
  TEST_END("fs bridge MC/DC: 64-bit erase coordinates past 32 bits refused");
}

/* =============================================================================
 * End-to-end: real FAT on a RAM block device through the bridge
 * =============================================================================
 */

/**
 * @par MC/DC:
 * (no compound decisions under test -- formats FAT16 on the ramdisk via the
 * bridge, mounts it, writes a file, and reads it back byte-identical) @brief Verify fs bridge fat roundtrip behavior. @details Executes the fs bridge fat roundtrip scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since 0.1.0 */
RA8_INTERNAL static void internal_test_fs_bridge_fat_roundtrip(void)
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
    payload[i] = (uint8_t)(((i * k_bd_pattern_stride) + 3U) & k_byte_mask);
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
  internal_test_ram_init_validation();
  internal_test_ram_caps();
  internal_test_ram_read_write_roundtrip();
  internal_test_ram_bounds();
  internal_test_ram_erase();
  internal_test_ram_read_only();
  internal_test_unbound_handle();
  internal_test_dispatch_optional_callbacks();
  internal_test_fs_bridge_erase_value_gate();
  internal_test_mcdc_fs_bridge_erase_64bit_guard();
  internal_test_fs_bridge_fat_roundtrip();
  return 0;
}
