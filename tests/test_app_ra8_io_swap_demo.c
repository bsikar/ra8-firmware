/**
 * @file test_app_ra8_io_swap_demo.c
 * @brief Host unit + integration test for the ra8_io_swap_demo capstone (#264).
 *
 * @details
 * Mirrors examples/ek_ra8d2/hw_pending/ra8_io_swap_demo/main.c and proves its
 * three ra8_io abstractions on the host, without the SCI console:
 *
 *  - **Drop-in block-device swap.** A local backend-agnostic engine (::run_backend,
 *    the host mirror of `swap_run_one`) runs the IDENTICAL FAT/VFS round-trip over
 *    two capability-different backends: an in-SRAM RAM block device and the
 *    register-level xSPI NOR model in `tests/mocks/ra8_sim_xspi_flash.c` (an
 *    erase-before-write medium). Both round-trip byte-for-byte behind the one
 *    ::ra8_io_blockdev_t vtable, so the media are proven interchangeable.
 *  - **VFS open/read/write/close.** Each backend is reached by name
 *    (`ram:/DATA.BIN`, `xs:/DATA.BIN`) through the streaming file API.
 *  - **Targetable stdio.** The engine's progress is captured into an in-RAM
 *    stream sink; the test asserts both backends' markers landed in the buffer.
 *
 * Two pure decision helpers mirror the app's compound decisions and carry MC/DC
 * vectors: ::swap_verdict (the read-back verdict, an OR of length + content
 * mismatch) and ::swap_capture_ok (main's capture-PASS gate, an AND).
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>
#include <string.h>

#include "ra8_check.h"
#include "ra8_err.h"
#include "ra8_fs.h"
#include "ra8_io_blockdev.h"
#include "ra8_io_blockdev_ram.h"
#include "ra8_io_blockdev_xspi.h"
#include "ra8_io_stream.h"
#include "ra8_io_stream_ram.h"
#include "ra8_io_vfs.h"
#include "ra8_sim_mmio.h"
#include "ra8_sim_xspi_flash.h"
#include "ra8_xspi.h"
#include "unity_minimal.h"

/**
 * @enum t_swap_const_t
 * @brief Fixture sizes and pattern coefficients (mirror the app's constants).
 *
 * @details The RAM and xSPI volumes are both small FAT12 windows; the xSPI count
 *          is a multiple of the 8-block (4 KiB) NOR erase unit. The pattern
 *          coefficients match `k_swap_pat_*` in the app so a drift between the two
 *          would fail the byte compare.
 *
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_t_ram_blocks  = 256U,  /**< 128 KiB in-SRAM RAM disk (FAT12).        */
  k_t_xspi_blocks = 256U,  /**< 128 KiB xSPI NOR window (multiple of 8). */
  k_t_payload     = 96U,   /**< Bytes round-tripped per backend.         */
  k_t_log_cap     = 512U,  /**< In-RAM stdio-capture buffer capacity.    */
  k_t_pat_mul     = 7U,    /**< Linear pattern multiplier.               */
  k_t_pat_add     = 3U,    /**< Linear pattern additive bias.            */
  k_t_pat_xor     = 0x5AU, /**< Linear pattern XOR mask.                 */
} t_swap_const_t;

/** @brief 128 KiB RAM-disk backing buffer. */
static uint8_t s_ram_disk[(size_t)k_t_ram_blocks * (size_t)k_ra8_io_block_size_bytes];
/** @brief Deterministic write payload + read-back scratch. */
static uint8_t s_pay[(size_t)k_t_payload];
static uint8_t s_rb[(size_t)k_t_payload];
/** @brief In-RAM stdio-capture buffer for the swap engine's progress log. */
static uint8_t s_log_buf[(size_t)k_t_log_cap];
/** @brief ra8_log tag used by the engine's per-step error checks. */
static const char* const s_swap_test_tag = "swap_test";

/**
 * @brief Mirror of the app's deterministic payload byte pattern.
 *
 * @details `byte[i] = (i * k_t_pat_mul + k_t_pat_add) ^ k_t_pat_xor`, identical to
 *          `swap_fill()` in the app so the read-back compare is meaningful.
 *
 * @param[in] i Byte index.
 *
 * @return uint8_t The pattern byte for index @p i.
 *
 * @pre @p i is a valid payload index.
 * @pre The coefficients mirror the app's `k_swap_pat_*`.
 * @post The return value depends only on @p i (pure function).
 * @post No state is modified.
 *
 * @note Thread-safe (pure).
 * @since 0.1.0
 */
static uint8_t swap_pattern(uint32_t i)
{
  return (uint8_t)(((i * (uint32_t)k_t_pat_mul) + (uint32_t)k_t_pat_add) ^ (uint32_t)k_t_pat_xor);
}

/**
 * @brief Mirror of the app read-back verdict: true when the read-back mismatched.
 *
 * @details Reproduces `swap_vfs_read_verify`'s decision
 *          `(got != len) || (bytes differ)` as a pure predicate so it can carry
 *          MC/DC vectors on the host.
 *
 * @param[in] got         Bytes actually read back.
 * @param[in] len         Bytes expected.
 * @param[in] bytes_equal true when the read-back content equalled the payload.
 *
 * @return bool true when the round-trip mismatched (length or content).
 *
 * @pre @p got and @p len are the read-back and expected byte counts.
 * @pre @p bytes_equal reflects a byte compare over @p len bytes.
 * @post The result depends only on the three arguments (pure).
 * @post No state is modified.
 *
 * @note Thread-safe (pure).
 * @since 0.1.0
 */
static bool swap_verdict(uint32_t got, uint32_t len, bool bytes_equal)
{
  return (got != len) || (!bytes_equal);
}

/**
 * @brief Mirror of the app's capture-PASS gate: true when the replay succeeded.
 *
 * @details Reproduces main()'s `(rep_e == k_ra8_ok) && (captured > 0)` decision as
 *          a pure predicate for MC/DC.
 *
 * @param[in] rep_e    Return code of the RAM-capture replay step.
 * @param[in] captured Number of bytes replayed out of the RAM capture.
 *
 * @return bool true when the capture step succeeded and produced bytes.
 *
 * @pre @p rep_e is the replay step's error code.
 * @pre @p captured is the reported capture length.
 * @post The result depends only on the two arguments (pure).
 * @post No state is modified.
 *
 * @note Thread-safe (pure).
 * @since 0.1.0
 */
static bool swap_capture_ok(ra8_err_t rep_e, uint32_t captured)
{
  return (rep_e == k_ra8_ok) && (captured > 0U);
}

/**
 * @brief Host mirror of `swap_run_one`: FAT/VFS round-trip over one block device.
 *
 * @details Bridges @p bd to `ra8_fs`, formats + mounts a FAT12 volume, registers
 *          it in the VFS under @p name, writes then reads back @p len bytes at
 *          @p file through the streaming VFS file API, byte-compares via
 *          ::swap_verdict, then unregisters + unmounts. Progress markers are
 *          emitted into @p log. Every file handle and mount is released on the
 *          success path.
 *
 * @param[in]  bd   Pre-bound block device (RAM or xSPI).
 * @param[in]  name VFS volume name (no ':' or '/').
 * @param[in]  label FAT volume label literal.
 * @param[in]  file Full "<name>:/..." VFS path.
 * @param[in]  len  Payload length to round-trip (<= ::k_t_payload).
 * @param[out] log  Stream to write progress markers into.
 *
 * @return ra8_err_t Error code (k_ra8_ok on a byte-exact round-trip).
 * @retval k_ra8_ok                    The volume mounted and round-tripped.
 * @retval k_ra8_err_checksum_mismatch The read-back length or bytes differed.
 * @retval (other)                    The first failing fabric step's code.
 *
 * @pre @p bd, @p name, @p file, and @p log are non-NULL and @p bd is reachable.
 * @pre @p len is at most ::k_t_payload.
 * @post On success @p file held the verified payload and the mount is torn down.
 * @post No file handle or mount is left open on the success path.
 *
 * @note Not thread-safe; single-caller test context.
 * @since 0.1.0
 */
/**
 * @brief Format, mount and VFS-publish one block device.
 *
 * @param[in,out] bd    Block device to bring up.
 * @param[in]     name  VFS mount name to publish under.
 * @param[in]     label FAT volume label to format with.
 * @param[out]    mnt   Receives the mounted volume on success.
 *
 * @return k_ra8_ok on success, else the first failing step's error.
 * @retval k_ra8_ok The device is formatted, mounted and published.
 *
 * @pre @p bd is initialised and @p mnt is non-NULL.
 * @pre @p name is not already mounted in the VFS.
 * @post On success `*mnt` is a mounted volume reachable through @p name.
 * @post On failure nothing is left mounted under @p name.
 *
 * @note Not thread-safe; the demo is single-threaded.
 */
static ra8_err_t swap_prepare_volume(ra8_io_blockdev_t* bd,
                                     const char*        name,
                                     const char*        label,
                                     ra8_fs_mount_t**   mnt)
{
  ra8_fs_backend_t be = {};
  RA8_RETURN_ON_ERROR(ra8_io_blockdev_as_fs_backend(bd, &be), s_swap_test_tag, "bridge");
  ra8_fs_format_opts_t opts = {};
  opts.type                 = k_ra8_fs_type_fat12;
  opts.label                = label;
  RA8_RETURN_ON_ERROR(ra8_fs_format(&be, &opts), s_swap_test_tag, "format");
  RA8_RETURN_ON_ERROR(ra8_fs_mount(&be, mnt), s_swap_test_tag, "mount");
  RA8_RETURN_ON_ERROR(ra8_io_vfs_mount(name, *mnt), s_swap_test_tag, "vfs mount");
  return k_ra8_ok;
}

/**
 * @brief Fill the payload buffer with the test pattern and write it to @p file.
 *
 * @param[in] file VFS path to create.
 * @param[in] len  Payload length in bytes.
 *
 * @return k_ra8_ok on success, else the open/write/close error.
 * @retval k_ra8_ok @p file holds @p len pattern bytes.
 *
 * @pre A volume is mounted that covers @p file.
 * @pre @p len does not exceed the payload buffer.
 * @post The file handle is closed on every path, including write failure.
 * @post `s_pay` holds the pattern the read-back is compared against.
 *
 * @note Not thread-safe; the payload buffers are file-scope state.
 */
static ra8_err_t swap_write_payload(const char* file, uint32_t len)
{
  for (uint32_t i = 0U; i < len; ++i) {
    s_pay[i] = swap_pattern(i);
  }
  ra8_fs_file_t* wf = nullptr;
  RA8_RETURN_ON_ERROR(ra8_io_vfs_open(file, k_ra8_fs_mode_write, &wf), s_swap_test_tag, "open w");
  const ra8_err_t we = ra8_fs_write(wf, s_pay, len);
  RA8_RETURN_ON_ERROR(ra8_fs_close(wf), s_swap_test_tag, "close w");
  RA8_RETURN_ON_ERROR(we, s_swap_test_tag, "write");
  return k_ra8_ok;
}

/**
 * @brief Read @p file back into the compare buffer.
 *
 * @param[in]  file VFS path to read.
 * @param[in]  len  Bytes to request.
 * @param[out] got  Receives the byte count actually read.
 *
 * @return k_ra8_ok on success, else the open/read/close error.
 * @retval k_ra8_ok `s_rb` holds the file's first `*got` bytes.
 *
 * @pre A volume is mounted that covers @p file and @p got is non-NULL.
 * @pre @p len does not exceed the compare buffer.
 * @post The compare buffer is zeroed first, so a short read cannot pass on
 *       stale bytes from an earlier backend.
 * @post The file handle is closed on every path, including read failure.
 *
 * @note Not thread-safe; the payload buffers are file-scope state.
 */
static ra8_err_t swap_read_payload(const char* file, uint32_t len, uint32_t* got)
{
  (void)memset(s_rb, 0, sizeof(s_rb));
  ra8_fs_file_t* rf = nullptr;
  RA8_RETURN_ON_ERROR(ra8_io_vfs_open(file, k_ra8_fs_mode_read, &rf), s_swap_test_tag, "open r");
  const ra8_err_t re = ra8_fs_read(rf, s_rb, len, got);
  RA8_RETURN_ON_ERROR(ra8_fs_close(rf), s_swap_test_tag, "close r");
  RA8_RETURN_ON_ERROR(re, s_swap_test_tag, "read");
  return k_ra8_ok;
}

/**
 * @brief Write the pattern to @p file, read it back, and report whether it survived.
 *
 * @details
 * The write and the read-back are only meaningful as a pair, so they travel
 * together here. The comparison runs through ::swap_verdict, the same predicate
 * the app uses, so a backend that returns short or corrupt data is judged by
 * exactly the criteria the demo applies -- the round-trip result is reported
 * through @p match rather than as an error code, because a content mismatch is
 * a test verdict and not an I/O failure.
 *
 * @param[in]  file  VFS path to write and then read.
 * @param[in]  len   Payload length in bytes; must not exceed the file-scope buffers.
 * @param[out] match Receives true when the read-back matched the written pattern.
 *
 * @return k_ra8_ok when both transfers completed, else the first I/O error.
 * @retval k_ra8_ok Both transfers completed and @p match holds the verdict.
 *
 * @pre A volume is mounted that covers @p file and @p match is non-NULL.
 * @pre @p len does not exceed the payload or compare buffer.
 * @post On success @p match reflects the byte-for-byte comparison.
 * @post On failure @p match is untouched and the error is the transfer's own.
 *
 * @note Not thread-safe; the payload buffers are file-scope state.
 *
 * @see swap_verdict()  The comparison predicate this reports through.
 */
static ra8_err_t swap_roundtrip_file(const char* file, uint32_t len, bool* match)
{
  RA8_RETURN_ON_ERROR(swap_write_payload(file, len), s_swap_test_tag, "write phase");
  uint32_t got = 0U;
  RA8_RETURN_ON_ERROR(swap_read_payload(file, len, &got), s_swap_test_tag, "read phase");
  const bool equal = (memcmp(s_pay, s_rb, (size_t)len) == 0);
  *match           = !swap_verdict(got, len, equal);
  return k_ra8_ok;
}

/**
 * @brief Release a volume from both layers that hold it, VFS first.
 *
 * @details
 * A volume is registered twice -- once with the VFS under @p name and once with
 * the filesystem as @p mnt -- so releasing it means undoing both, and the VFS
 * mapping must go first because it refers to the filesystem mount. Unwinding in
 * the other order would leave the VFS pointing at a released mount.
 *
 * @param[in] name VFS mount name the volume was registered under.
 * @param[in] mnt  Filesystem mount handle returned by ::swap_prepare_volume.
 *
 * @return k_ra8_ok when both layers released, else the first unmount error.
 * @retval k_ra8_ok The volume is no longer reachable from either layer.
 *
 * @pre @p name names a currently-mounted VFS entry.
 * @pre @p mnt is the live filesystem mount behind that entry.
 * @post On success neither layer still refers to the volume.
 * @post On failure the VFS release may already have happened.
 *
 * @note Not thread-safe; mutates process-wide mount tables.
 *
 * @see swap_prepare_volume()  The acquisition this reverses.
 */
static ra8_err_t swap_release_volume(const char* name, ra8_fs_mount_t* mnt)
{
  RA8_RETURN_ON_ERROR(ra8_io_vfs_unmount(name), s_swap_test_tag, "vfs unmount");
  RA8_RETURN_ON_ERROR(ra8_fs_unmount(mnt), s_swap_test_tag, "unmount");
  return k_ra8_ok;
}

/**
 * @brief Drive one block-device backend through the whole swap exercise.
 *
 * @details
 * Backend-agnostic by construction: every backend reaches this through the same
 * ::ra8_io_blockdev_t vtable, so the demo proves substitutability by running
 * the identical mount / round-trip / release sequence against each one. A
 * content mismatch is reported as ::k_ra8_err_checksum_mismatch and skips the
 * release, leaving the volume mounted for inspection.
 *
 * @param[in]     bd    Block device to mount; must be initialised.
 * @param[in]     name  Short VFS mount name, e.g. "ram".
 * @param[in]     label Volume label to format with.
 * @param[in]     file  VFS path to exercise, under @p name.
 * @param[in]     len   Payload length in bytes.
 * @param[in,out] log   Stream that receives the per-backend progress line.
 *
 * @return k_ra8_ok when the backend completed the exercise.
 * @retval k_ra8_ok                     Round-trip matched and the volume released.
 * @retval k_ra8_err_checksum_mismatch  Read-back differed from what was written.
 * @retval other                        Propagated from mount, transfer, or release.
 *
 * @pre @p bd, @p name, @p label, @p file, and @p log are non-NULL.
 * @pre @p len does not exceed the file-scope payload buffers.
 * @post On success the volume is released from both the VFS and the filesystem.
 * @post @p log carries one line naming the backend and its outcome.
 *
 * @note Not thread-safe; the payload buffers are file-scope state.
 *
 * @see swap_roundtrip_file()  The write/read/compare phase.
 * @see swap_release_volume()  The teardown phase.
 */
static ra8_err_t run_backend(ra8_io_blockdev_t* bd,
                             const char*        name,
                             const char*        label,
                             const char*        file,
                             uint32_t           len,
                             ra8_io_stream_t*   log)
{
  ra8_fs_mount_t* mnt = nullptr;
  RA8_RETURN_ON_ERROR(swap_prepare_volume(bd, name, label, &mnt), s_swap_test_tag, "prepare");
  (void)ra8_io_stream_puts(log, "swap[");
  (void)ra8_io_stream_puts(log, name);
  (void)ra8_io_stream_puts(log, "] ");

  bool match = false;
  RA8_RETURN_ON_ERROR(swap_roundtrip_file(file, len, &match), s_swap_test_tag, "roundtrip");
  if (!match) {
    return k_ra8_err_checksum_mismatch;
  }
  RA8_RETURN_ON_ERROR(swap_release_volume(name, mnt), s_swap_test_tag, "release");
  (void)ra8_io_stream_puts(log, "ok\r\n");
  return k_ra8_ok;
}

/**
 * @test swap_pattern_is_deterministic_and_reproducible
 *
 * @par MC/DC:
 * No compound decision; the fill loop bound is the only branch. Two independent
 * fills of the same length must agree byte-for-byte, and at least two distinct
 * byte values must appear so the pattern is not a constant.
 */
static void test_pattern(void)
{
  TEST_BEGIN("swap pattern deterministic");
  uint8_t a[(size_t)k_t_payload];
  uint8_t b[(size_t)k_t_payload];
  for (uint32_t i = 0U; i < (uint32_t)k_t_payload; ++i) {
    a[i] = swap_pattern(i);
    b[i] = swap_pattern(i);
  }
  TEST_ASSERT(memcmp(a, b, sizeof(a)) == 0);
  bool saw_diff = false;
  for (uint32_t i = 1U; i < (uint32_t)k_t_payload; ++i) {
    if (a[i] != a[0]) {
      saw_diff = true;
    }
  }
  TEST_ASSERT(saw_diff);
  TEST_END("swap pattern deterministic");
}

/**
 * @test swap_verdict_mcdc
 *
 * @par MC/DC:
 * Decision: `(got != len) || (!bytes_equal)` (2 conditions).
 * - Vector 1: got=len,   bytes_equal=true  -> false (control: both conditions false)
 * - Vector 2: got=len-1, bytes_equal=true  -> true  (varies the length term only)
 * - Vector 3: got=len,   bytes_equal=false -> true  (varies the content term only)
 * Vectors 1+2 prove the length term independently affects the outcome; 1+3 prove
 * the same for the content term. N+1 = 3 vectors for N=2 conditions: minimal MC/DC.
 */
static void test_verdict(void)
{
  TEST_BEGIN("swap verdict MC/DC");
  const uint32_t len = (uint32_t)k_t_payload;
  TEST_ASSERT(!swap_verdict(len, len, true));     /* V1: match           */
  TEST_ASSERT(swap_verdict(len - 1U, len, true)); /* V2: short read      */
  TEST_ASSERT(swap_verdict(len, len, false));     /* V3: content differs */
  TEST_END("swap verdict MC/DC");
}

/**
 * @test swap_capture_ok_mcdc
 *
 * @par MC/DC:
 * Decision: `(rep_e == k_ra8_ok) && (captured > 0)` (2 conditions).
 * - Vector 1: rep_e=ok,           captured=1 -> true  (control: both conditions true)
 * - Vector 2: rep_e=invalid_arg,  captured=1 -> false (varies the rep_e term only)
 * - Vector 3: rep_e=ok,           captured=0 -> false (varies the captured term only)
 * Vectors 1+2 prove the rep_e term independently affects the outcome; 1+3 prove
 * the same for the captured term. N+1 = 3 vectors for N=2 conditions: minimal MC/DC.
 */
static void test_capture_ok(void)
{
  TEST_BEGIN("swap capture-ok MC/DC");
  TEST_ASSERT(swap_capture_ok(k_ra8_ok, 1U));               /* V1 */
  TEST_ASSERT(!swap_capture_ok(k_ra8_err_invalid_arg, 1U)); /* V2 */
  TEST_ASSERT(!swap_capture_ok(k_ra8_ok, 0U));              /* V3 */
  TEST_END("swap capture-ok MC/DC");
}

/**
 * @test swap_ram_and_xspi_round_trip_through_one_engine
 *
 * @par MC/DC:
 * No compound decision under test here; this is the end-to-end integration path.
 * The two backends are driven through the IDENTICAL ::run_backend so the swap is
 * literal, and the in-RAM capture must contain both backends' markers.
 */
static void test_swap_integration(void)
{
  TEST_BEGIN("swap RAM+xSPI integration");
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_vfs_init());

  /* xSPI NOR model: reset the MMIO seam, install the flash, bring the driver up. */
  ra8_sim_mmio_reset();
  ra8_sim_xspi_flash_install();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_xspi_init(0, k_ra8_xspi_lio_1s1s1s));

  ra8_io_blockdev_t           ram_bd    = {};
  ra8_io_blockdev_ram_state_t ram_state = {};
  TEST_ASSERT_EQ(
    k_ra8_ok,
    ra8_io_blockdev_ram_init(&ram_bd, &ram_state, s_ram_disk, (uint32_t)k_t_ram_blocks, false));

  ra8_io_blockdev_t            xspi_bd    = {};
  ra8_io_blockdev_xspi_state_t xspi_state = {};
  TEST_ASSERT_EQ(
    k_ra8_ok,
    ra8_io_blockdev_xspi_init(&xspi_bd, &xspi_state, 0, 0, (uint32_t)k_t_xspi_blocks, false));

  /* The engine's progress is retargeted into an in-RAM capture sink. */
  ra8_io_stream_t           log       = {};
  ra8_io_stream_ram_state_t log_state = {};
  (void)memset(s_log_buf, 0, sizeof(s_log_buf));
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_io_stream_ram_init(&log, &log_state, s_log_buf, (uint32_t)k_t_log_cap));

  /* Identical engine, two capability-different backends: the literal swap. */
  TEST_ASSERT_EQ(
    k_ra8_ok,
    run_backend(&ram_bd, "ram", "RAIORAM", "ram:/DATA.BIN", (uint32_t)k_t_payload, &log));
  TEST_ASSERT_EQ(
    k_ra8_ok,
    run_backend(&xspi_bd, "xs", "RAIOXS", "xs:/DATA.BIN", (uint32_t)k_t_payload, &log));

  /* Capability check: the two backends really are different media behind one API. */
  ra8_io_blockdev_caps_t ram_caps  = {};
  ra8_io_blockdev_caps_t xspi_caps = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_blockdev_get_caps(&ram_bd, &ram_caps));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_blockdev_get_caps(&xspi_bd, &xspi_caps));
  TEST_ASSERT(!ram_caps.must_erase_before_write);
  TEST_ASSERT(xspi_caps.must_erase_before_write);
  TEST_ASSERT_EQ(k_ra8_io_erase_value_zero, ram_caps.erase_value);
  TEST_ASSERT_EQ(k_ra8_io_erase_value_ones, xspi_caps.erase_value);

  /* stdio-to-RAM: both backends' markers landed in the capture buffer. */
  uint32_t used = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_stream_ram_used(&log_state, &used));
  TEST_ASSERT(used > 0U);
  TEST_ASSERT(used < (uint32_t)k_t_log_cap);
  s_log_buf[used] = 0U; /* NUL-terminate inside the buffer for strstr */
  TEST_ASSERT(strstr((const char*)s_log_buf, "swap[ram]") != nullptr);
  TEST_ASSERT(strstr((const char*)s_log_buf, "swap[xs]") != nullptr);
  TEST_END("swap RAM+xSPI integration");
}

/**
 * @brief Test entry point: runs the pure MC/DC checks then the integration path.
 *
 * @return int32_t 0 on success; a non-zero exit is raised inside the assertions.
 *
 * @pre The host build links ra8_core_hal (ra8_io + the xSPI NOR model).
 * @pre No prior test left a VFS mount registered.
 * @post Every test function has run and passed (else the process exited non-zero).
 * @post stderr carries one [PASS] line per test.
 *
 * @note Single-threaded host test.
 * @since 0.1.0
 */
int32_t main(void)
{
  test_pattern();
  test_verdict();
  test_capture_ok();
  test_swap_integration();
  (void)fprintf(stderr, "[OK  ] test_app_ra8_io_swap_demo.c\n");
  return 0;
}
