/**
 * @file test_ra8_io_vfs.c
 * @brief Unit tests for the ra8_io VFS mount table + path router (issue #158).
 *
 * @details
 * Sets up a FAT16 volume on a RAM block device, registers it under a name, and
 * exercises name-based open / stat / unlink / rename / listdir / mkdir / rmdir,
 * mount-table mechanics (duplicate, full, unmount isolation), and path-parse
 * rejection.
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
#include "ra8_io_blockdev_ram.h"
#include "ra8_io_fsfmt.h"
#include "ra8_io_vfs.h"
#include "ra8_io_vfs_internal.h"
#include "unity_minimal.h"

/**
 * @enum io_vfs_fixture_t
 * @brief The byte-level helpers.
 */
typedef enum : uint8_t {
  k_byte_mask = 0xFFU, /**< Truncates a generated or shifted value back into a byte. */
} io_vfs_fixture_t;

/**
 * @enum t_vfs_const_t
 * @brief Fixture sizes.
 */
typedef enum : uint32_t {
  k_t_disk_blocks = 16384, /**< 8 MiB -- comfortably FAT16. */
  k_t_payload     = 100,   /**< Bytes in the seeded file.   */
} t_vfs_const_t;

/**
 * @enum t_vfs_mock_const_t
 * @brief Foreign-format fixture geometry, cursor requirements, and bounds.
 */
typedef enum : uint32_t {
  k_t_mock_blocks       = 8U,    /**< Tiny foreign volume block count.         */
  k_t_mock_magic        = 0xC3U, /**< Foreign volume marker in block zero.     */
  k_t_mock_name_max     = 31U,   /**< Foreign UTF-8 name-byte limit.           */
  k_t_mock_cursor_bytes = 32U,   /**< Cursor workspace the mock demands.       */
  k_t_mock_cursor_align = 8U,    /**< Cursor workspace alignment demanded.     */
  k_t_mock_cursor_max   = 2U,    /**< Concurrent cursors the mock allows.      */
  k_t_mock_small_ws     = 16U,   /**< Cursor workspace under the requirement.  */
  k_t_mock_ws_words     = 8U,    /**< 64-bit words backing a cursor workspace. */
  k_t_bad_mode          = 0xEEU, /**< Value outside the open-mode vocabulary.  */
  k_t_misalign_offset   = 1U,    /**< Byte offset that breaks cursor align.    */
  k_t_long_name_len     = 15U,   /**< Longest mount name that still fits.      */
  k_t_marker_index      = 0U,    /**< Block byte carrying the volume marker.   */
  k_t_foreign_words     = 8U,    /**< 64-bit words behind a foreign facade.    */
} t_vfs_mock_const_t;

/** @brief The longest mount name the fixed name buffer can still terminate. */
static const char* const k_t_long_name = "longmountname12";

/** @brief A qualified path whose mount-name field is one byte too long. */
static const char* const k_t_overlong_path = "sixteencharname0:/X";

/**
 * @struct t_mock_mount_t
 * @brief Foreign-format mount state with injectable dispatch results.
 * @invariant `mounted` is true exactly between a successful mount and its
 *            matching unmount.
 * @see internal_mount_mock
 */
typedef struct {
  bool      mounted;         /**< true between mount and unmount.        */
  uint32_t  unmounts;        /**< Completed unmount dispatches.          */
  ra8_err_t unmount_status;  /**< Result the unmount dispatch returns.   */
  ra8_err_t dir_open_status; /**< Result the cursor open dispatch gives. */
} t_mock_mount_t;

/**
 * @struct t_mock_stream_t
 * @brief Foreign-format stream state shared by every open facade.
 * @invariant `closes` never exceeds `opens`.
 * @see internal_mock_open
 */
typedef struct {
  uint64_t offset; /**< Byte offset the seek/tell pair records. */
  uint32_t opens;  /**< Open dispatches the format received.    */
  uint32_t closes; /**< Streams closed through the mock format. */
  uint32_t syncs;  /**< Sync dispatches served.                 */
  uint32_t writes; /**< Write dispatches the format received.   */
} t_mock_stream_t;

/** @brief 8 MiB backing buffer. */
static uint8_t s_disk[(size_t)k_t_disk_blocks * (size_t)k_ra8_io_block_size_bytes];
/** @brief Foreign fixed-layout volume storage. */
static uint8_t s_mock_disk[(size_t)k_t_mock_blocks * (size_t)k_ra8_io_block_size_bytes];
/** @brief The single foreign mount context. */
static t_mock_mount_t s_mock;
/** @brief The single foreign stream context. */
static t_mock_stream_t s_mock_stream;
/** @brief Storage for a facade pointer the VFS file table never handed out. */
static uint64_t                    s_foreign_storage[k_t_foreign_words];
static ra8_io_blockdev_ram_state_t s_bstate;
static ra8_io_blockdev_t           s_bd;
static ra8_fs_backend_t            s_be;
static ra8_fs_mount_t*             s_mnt;

/**
 * @brief Build a fresh FAT16 volume with one seeded file and return its mount.
 *
 * @details Reclaims the preceding fixture mount, reformats the fixed RAM disk,
 *          mounts it through the block-device bridge, and seeds HELLO.BIN
 *          with a deterministic ascending-byte payload.
 *
 * @return The mount produced by ::ra8_fs_mount.
 * @retval nullptr Formatting or mounting did not produce a mount.
 * @retval non-NULL The fresh volume is mounted and contains the seeded file.
 *
 * @pre The file-scope disk and block-device fixtures are exclusively owned by
 *      this single-threaded test executable.
 * @pre ::s_disk holds ::k_t_disk_blocks complete logical blocks.
 * @post Any preceding ::s_mnt mount is released before the disk is reformatted.
 * @post ::s_mnt contains the current mount result; on success HELLO.BIN
 *       contains exactly ::k_t_payload deterministic bytes.
 *
 * @note This fixture deliberately ignores intermediate status values so each
 *       caller can assert the observable VFS contract under test.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_fs_mount_t* internal_setup_volume(void)
{
  if (s_mnt != nullptr) {
    (void)ra8_fs_unmount(s_mnt); /* ra8_fs has only 2 mount slots -- free the prior one */
    s_mnt = nullptr;
  }
  (void)ra8_io_blockdev_ram_init(&s_bd, &s_bstate, s_disk, k_t_disk_blocks, false);
  (void)ra8_io_blockdev_as_fs_backend(&s_bd, &s_be);
  ra8_fs_format_opts_t opts = {};
  opts.type                 = k_ra8_fs_type_fat16;
  opts.label                = "VFS";
  (void)ra8_fs_format(&s_be, &opts);
  s_mnt = nullptr;
  (void)ra8_fs_mount(&s_be, &s_mnt);
  uint8_t data[(size_t)k_t_payload];
  for (uint32_t i = 0; i < (uint32_t)k_t_payload; ++i) {
    data[i] = (uint8_t)((i + 1U) & k_byte_mask);
  }
  (void)ra8_fs_write_file(s_mnt, "HELLO.BIN", data, k_t_payload);
  return s_mnt;
}

/**
 * @par MC/DC:
 * (no compound decisions under test -- mount rejects NULL, bad names, duplicates,
 * and a full table via independent single-condition guards) @brief Verify mount table behavior. @details Executes the mount table scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since 0.1.0 */
RA8_INTERNAL static void internal_test_mount_table(void)
{
  TEST_BEGIN("vfs mount table");
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_vfs_init());
  ra8_fs_mount_t* m = internal_setup_volume();
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_io_vfs_mount(nullptr, m));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_io_vfs_mount("sd", nullptr));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_io_vfs_mount("a:b", m)); /* reserved ':' */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_io_vfs_mount("", m));    /* empty        */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_vfs_mount("sd", m));
  TEST_ASSERT_EQ(k_ra8_err_exists, ra8_io_vfs_mount("sd", m)); /* duplicate */
  /* fill remaining slots, then overflow */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_vfs_mount("a", m));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_vfs_mount("b", m));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_vfs_mount("c", m));
  TEST_ASSERT_EQ(k_ra8_err_no_mem, ra8_io_vfs_mount("d", m));
  TEST_END("vfs mount table");
}

/**
 * @par MC/DC:
 * (no compound decisions under test -- open + read-back over a named mount, and
 * a path with no name: prefix is rejected) @brief Verify open read behavior. @details Executes the open read scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since 0.1.0 */
RA8_INTERNAL static void internal_test_open_read(void)
{
  TEST_BEGIN("vfs open/read");
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_vfs_init());
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_vfs_mount("sd", internal_setup_volume()));

  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_io_vfs_open("sd:/HELLO.BIN", k_ra8_fs_mode_read, nullptr));
  ra8_fs_file_t*  f = nullptr;
  const ra8_err_t e = ra8_io_vfs_open("noprefix", k_ra8_fs_mode_read, &f);
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, e);
  TEST_ASSERT_EQ(k_ra8_err_not_found, ra8_io_vfs_open("nope:/X", k_ra8_fs_mode_read, &f));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_vfs_open("sd:/HELLO.BIN", k_ra8_fs_mode_read, &f));
  uint8_t  got[(size_t)k_t_payload] = {};
  uint32_t got_len                  = 0;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_read(f, got, k_t_payload, &got_len));
  TEST_ASSERT_EQ(k_t_payload, got_len);
  TEST_ASSERT_EQ(1, got[0]);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_close(f));
  TEST_END("vfs open/read");
}

/**
 * @par MC/DC:
 * (no compound decisions under test -- stat reports presence/size, a missing
 * file yields exists==false with ok, and a DIRECTORY reports as one: #609, where
 * the open-based implementation returned every folder as a zero-byte file with a
 * hardcoded `archive` attribute) @brief Verify stat behavior. @details Executes the stat scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since 0.1.0 */
RA8_INTERNAL static void internal_test_stat(void)
{
  TEST_BEGIN("vfs stat");
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_vfs_init());
  ra8_fs_mount_t* m = internal_setup_volume();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_vfs_mount("sd", m));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mkdir(m, "/BOOKS"));

  ra8_io_vfs_stat_t st = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_vfs_stat("sd:/HELLO.BIN", &st));
  TEST_ASSERT(st.exists);
  TEST_ASSERT_EQ(k_t_payload, st.size_bytes);
  TEST_ASSERT(!st.is_directory);
  TEST_ASSERT_EQ(0U, (st.attr & (uint8_t)k_ra8_fs_attr_directory));
  TEST_ASSERT(st.created.valid);
  TEST_ASSERT(st.modified.valid);
  TEST_ASSERT(st.accessed.valid);
  TEST_ASSERT_EQ(1980U, st.created.value.year);

  /* The regression this test exists for: a folder must not look like a file. */
  ra8_io_vfs_stat_t dir = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_vfs_stat("sd:/BOOKS", &dir));
  TEST_ASSERT(dir.exists);
  TEST_ASSERT(dir.is_directory);
  TEST_ASSERT_EQ(0U, dir.size_bytes);
  TEST_ASSERT_EQ(k_ra8_fs_attr_directory, (dir.attr & (uint8_t)k_ra8_fs_attr_directory));

  /* The mount root is a directory too. */
  ra8_io_vfs_stat_t root = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_vfs_stat("sd:/", &root));
  TEST_ASSERT(root.exists);
  TEST_ASSERT(root.is_directory);
  TEST_ASSERT(!root.created.valid);
  TEST_ASSERT(!root.modified.valid);
  TEST_ASSERT(!root.accessed.valid);

  ra8_io_vfs_stat_t miss = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_vfs_stat("sd:/GONE.BIN", &miss));
  TEST_ASSERT(!miss.exists);
  TEST_END("vfs stat");
}

/**
 * @par MC/DC:
 * (no compound decisions under test -- rename within a mount succeeds; a
 * cross-mount rename is rejected; unlink removes the file) @brief Verify rename unlink behavior. @details Executes the rename unlink scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since 0.1.0 */
RA8_INTERNAL static void internal_test_rename_unlink(void)
{
  TEST_BEGIN("vfs rename/unlink");
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_vfs_init());
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_vfs_mount("sd", internal_setup_volume()));

  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_io_vfs_rename("sd:/HELLO.BIN", "ram:/BYE.BIN"));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_vfs_rename("sd:/HELLO.BIN", "sd:/BYE.BIN"));
  ra8_io_vfs_stat_t st = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_vfs_stat("sd:/BYE.BIN", &st));
  TEST_ASSERT(st.exists);

  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_vfs_unlink("sd:/BYE.BIN"));
  ra8_io_vfs_stat_t gone = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_vfs_stat("sd:/BYE.BIN", &gone));
  TEST_ASSERT(!gone.exists);
  TEST_END("vfs rename/unlink");
}

/** @brief listdir callback: count entries. @details Exercises the count cb path with bounded caller-owned fixture state and verifies its documented result. @param[in] name Directory-entry name reported by the callback. @param[in] attr Filesystem attributes reported for the directory entry. @param[in] size Entry or storage size in bytes. @param[in,out] ctx Injected callback context whose ownership remains with the test. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since 0.1.0 */
RA8_INTERNAL static void internal_count_cb(const char* name, uint8_t attr, uint64_t size, void* ctx)
{
  (void)name;
  (void)attr;
  (void)size;
  uint32_t* n = (uint32_t*)ctx;
  (*n)++;
}

/**
 * @par MC/DC:
 * (no compound decisions under test -- listdir visits the seeded file; mkdir
 * creates a real subdirectory that then lists and rejects a duplicate; unmount
 * makes a name stop resolving) @brief Verify listdir mkdir unmount behavior. @details Executes the listdir mkdir unmount scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since 0.1.0 */
RA8_INTERNAL static void internal_test_listdir_mkdir_unmount(void)
{
  TEST_BEGIN("vfs listdir/mkdir/unmount");
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_vfs_init());
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_vfs_mount("sd", internal_setup_volume()));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_vfs_mount("ospi", s_mnt));

  uint32_t n = 0;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_vfs_listdir("sd:/", internal_count_cb, &n));
  TEST_ASSERT(n >= 1U);

  /* mkdir now creates a real subdirectory (the #158 ra8_fs mkdir work); a fresh
   * empty subdir lists with zero visible entries ("." / ".." are hidden), and a
   * duplicate create is rejected. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_vfs_mkdir("sd:/SUB"));
  TEST_ASSERT_EQ(k_ra8_err_exists, ra8_io_vfs_mkdir("sd:/SUB"));
  uint32_t m = 0;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_vfs_listdir("sd:/SUB", internal_count_cb, &m));
  TEST_ASSERT_EQ(0U, m);

  /* unmount isolation: dropping "sd" leaves "ospi" working */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_vfs_unmount("sd"));
  ra8_fs_file_t* f = nullptr;
  TEST_ASSERT_EQ(k_ra8_err_not_found, ra8_io_vfs_open("sd:/HELLO.BIN", k_ra8_fs_mode_read, &f));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_vfs_open("ospi:/HELLO.BIN", k_ra8_fs_mode_read, &f));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_close(f));
  TEST_ASSERT_EQ(k_ra8_err_not_found, ra8_io_vfs_unmount("sd"));
  TEST_END("vfs listdir/mkdir/unmount");
}

/**
 * @par MC/DC:
 * (no compound decisions under test -- rmdir delegates 1:1, so each case maps
 * to exactly one already-covered ra8_fs_rmdir outcome: NULL path, a path with
 * no `name:` prefix, an unknown mount name, a directory that still holds a
 * file, and the removal that succeeds once it is emptied) @brief Verify rmdir behavior. @details Executes the rmdir scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since 0.1.0 */
RA8_INTERNAL static void internal_test_rmdir(void)
{
  TEST_BEGIN("vfs rmdir");
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_vfs_init());
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_vfs_mount("sd", internal_setup_volume()));

  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_io_vfs_rmdir(nullptr));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_io_vfs_rmdir("noprefix"));
  TEST_ASSERT_EQ(k_ra8_err_not_found, ra8_io_vfs_rmdir("nope:/SUB"));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_vfs_mkdir("sd:/SUB"));
  ra8_fs_file_t* f = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_vfs_open("sd:/SUB/IN.BIN", k_ra8_fs_mode_write, &f));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_close(f));
  TEST_ASSERT_EQ(k_ra8_err_not_empty, ra8_io_vfs_rmdir("sd:/SUB"));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_vfs_unlink("sd:/SUB/IN.BIN"));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_vfs_rmdir("sd:/SUB"));
  TEST_ASSERT_EQ(k_ra8_err_not_found, ra8_io_vfs_rmdir("sd:/SUB"));
  TEST_END("vfs rmdir");
}

/** @brief Claim only the tiny volume carrying the foreign marker byte. @details Reads block zero through the backend and compares the marker, so the native probes decline the same volume. @param[in] backend Block-device backend under inspection. @return Whether the volume belongs to the foreign fixture format. @retval true Block zero carries the fixture marker. @retval false The backend is absent, unreadable, or not the fixture volume. @pre @p backend is either NULL or a bound block backend. @pre No mount is in flight on @p backend. @post No backend state is modified. @post The decision depends only on the volume's first block. @note Test-local; single-threaded host fixture. @since 0.1.0 */
RA8_INTERNAL static bool internal_mock_probe(const ra8_fs_backend_t* backend)
{
  if (backend == nullptr) {
    return false;
  }
  if (backend->read_block == nullptr) {
    return false;
  }
  uint8_t block[(size_t)k_ra8_io_block_size_bytes];
  if (backend->read_block(backend->ctx, 0U, 1U, block) != k_ra8_ok) {
    return false;
  }
  return block[k_t_marker_index] == (uint8_t)k_t_mock_magic;
}

/** @brief Bind the single static foreign mount context. @details Re-probes the volume and refuses a second concurrent mount. @param[in] backend Block-device backend to bind. @param[out] out_mount Receives the static mount context. @return Foreign mount status. @retval k_ra8_ok The static context is now bound. @retval k_ra8_err_not_found The volume is not the fixture volume. @retval k_ra8_err_busy The static context is already bound. @pre @p out_mount is non-NULL and writable. @pre The fixture reset the mount state before the first mount. @post On success the context reports itself mounted. @post On failure no context is published. @note Test-local; single-threaded host fixture. @since 0.1.0 */
RA8_INTERNAL static ra8_err_t internal_mock_mount(const ra8_fs_backend_t* backend, void** out_mount)
{
  if (!internal_mock_probe(backend)) {
    return k_ra8_err_not_found;
  }
  if (s_mock.mounted) {
    return k_ra8_err_busy;
  }
  s_mock.mounted = true;
  *out_mount     = &s_mock;
  return k_ra8_ok;
}

/** @brief Release the static foreign mount context. @details Counts the dispatch and returns the injected result so the VFS's own error propagation can be observed. @param[in,out] mount_ctx Context published by the fixture mount. @return The injected unmount status. @retval k_ra8_ok The default injection reports a clean release. @retval k_ra8_err_invalid_arg @p mount_ctx is not the static context. @pre @p mount_ctx was published by this format. @pre The fixture chose the injected result before the call. @post The context is no longer mounted. @post The dispatch counter advanced by exactly one. @note Test-local; single-threaded host fixture. @since 0.1.0 */
RA8_INTERNAL static ra8_err_t internal_mock_unmount(void* mount_ctx)
{
  if (mount_ctx != &s_mock) {
    return k_ra8_err_invalid_arg;
  }
  s_mock.mounted = false;
  s_mock.unmounts++;
  return s_mock.unmount_status;
}

/** @brief Open one read-only stream over the static foreign context. @details Every path names the same empty stream, so several facades can be open at once and the VFS file table can be filled exactly. @param[in,out] mount_ctx Mounted foreign context. @param[in] path Volume-relative path (ignored by the fixture). @param[in] mode Requested stream mode. @param[out] out_file Receives the static stream context. @return Foreign open status. @retval k_ra8_ok A stream context was published. @retval k_ra8_err_invalid_state @p mount_ctx is not the static context. @retval k_ra8_err_not_supported A writing mode reached the format. @pre @p out_file is non-NULL and writable. @pre @p path is a NUL-terminated volume-relative path. @post The dispatch counter advanced by one on every call, accepted or not. @post A writing mode never publishes a stream. @note Test-local; single-threaded host fixture. @since 0.1.0 */
RA8_INTERNAL static ra8_err_t
internal_mock_open(void* mount_ctx, const char* path, ra8_fs_mode_t mode, void** out_file)
{
  (void)path;
  s_mock_stream.opens++; /* counted first: a rejected open must never reach here */
  if (mount_ctx != &s_mock) {
    return k_ra8_err_invalid_state;
  }
  if (mode != k_ra8_fs_mode_read) {
    return k_ra8_err_not_supported;
  }
  *out_file = &s_mock_stream;
  return k_ra8_ok;
}

/** @brief Accept bytes so the streaming-write capability gate can be proven. @details The format offers a working write operation while declaring no streaming-write support, so a dispatched write is observable evidence that the VFS capability gate did not fire. @param[in,out] file_ctx Stream context published by the fixture open. @param[in] buf Source bytes (discarded by the fixture). @param[in] bytes Byte count the caller offered. @return Foreign write status. @retval k_ra8_ok The offered bytes were accepted. @retval k_ra8_err_invalid_arg @p file_ctx is not the static stream. @pre @p buf covers @p bytes readable bytes. @pre The stream is open. @post The write counter advanced by exactly one. @post No stream byte is retained. @note Test-local; single-threaded host fixture. @since 0.1.0 */
RA8_INTERNAL static ra8_err_t internal_mock_write(void* file_ctx, const void* buf, uint32_t bytes)
{
  (void)buf;
  (void)bytes;
  if (file_ctx != &s_mock_stream) {
    return k_ra8_err_invalid_arg;
  }
  s_mock_stream.writes++;
  return k_ra8_ok;
}

/** @brief Close one foreign stream and count the dispatch. @details Verifies the context identity before releasing the shared stream. @param[in,out] file_ctx Stream context published by the fixture open. @return Foreign close status. @retval k_ra8_ok The stream was released. @retval k_ra8_err_invalid_arg @p file_ctx is not the static stream. @pre @p file_ctx was published by this format. @pre The stream has not already been released by this facade. @post The close counter advanced by exactly one. @post No mount state is modified. @note Test-local; single-threaded host fixture. @since 0.1.0 */
RA8_INTERNAL static ra8_err_t internal_mock_close(void* file_ctx)
{
  if (file_ctx != &s_mock_stream) {
    return k_ra8_err_invalid_arg;
  }
  s_mock_stream.closes++;
  return k_ra8_ok;
}

/** @brief Serve the foreign stream, which is permanently at end-of-file. @details Reports a clean zero-byte read so a reader terminates immediately. @param[in,out] file_ctx Stream context published by the fixture open. @param[out] buf Caller destination (never written by an EOF read). @param[in] bytes Maximum bytes the caller accepts. @param[out] out_read Receives the delivered byte count. @return Foreign read status. @retval k_ra8_ok Clean end-of-file was reported. @retval k_ra8_err_invalid_arg @p file_ctx is not the static stream. @pre @p out_read is non-NULL and writable. @pre @p buf covers @p bytes writable bytes. @post @p out_read reports zero delivered bytes. @post No caller byte is modified. @note Test-local; single-threaded host fixture. @since 0.1.0 */
RA8_INTERNAL static ra8_err_t
internal_mock_read(void* file_ctx, void* buf, uint32_t bytes, uint32_t* out_read)
{
  (void)buf;
  (void)bytes;
  if (file_ctx != &s_mock_stream) {
    return k_ra8_err_invalid_arg;
  }
  *out_read = 0U;
  return k_ra8_ok;
}

/** @brief Record a requested stream offset. @details Stores the offset the matching tell dispatch reports back. @param[in,out] file_ctx Stream context published by the fixture open. @param[in] offset_bytes Requested absolute offset. @return Foreign seek status. @retval k_ra8_ok The offset was recorded. @retval k_ra8_err_invalid_arg @p file_ctx is not the static stream. @pre @p file_ctx was published by this format. @pre The stream is open. @post The recorded offset equals @p offset_bytes on success. @post No mount state is modified. @note Test-local; single-threaded host fixture. @since 0.1.0 */
RA8_INTERNAL static ra8_err_t internal_mock_seek(void* file_ctx, uint64_t offset_bytes)
{
  if (file_ctx != &s_mock_stream) {
    return k_ra8_err_invalid_arg;
  }
  s_mock_stream.offset = offset_bytes;
  return k_ra8_ok;
}

/** @brief Report the recorded stream offset. @details Publishes the value stored by the matching seek dispatch. @param[in] file_ctx Stream context published by the fixture open. @param[out] out_offset Receives the recorded offset. @return Foreign tell status. @retval k_ra8_ok The offset was published. @retval k_ra8_err_invalid_arg @p file_ctx is not the static stream. @pre @p out_offset is non-NULL and writable. @pre The stream is open. @post On success @p out_offset holds the last recorded offset. @post No fixture state is modified. @note Test-local; single-threaded host fixture. @since 0.1.0 */
RA8_INTERNAL static ra8_err_t internal_mock_tell(const void* file_ctx, uint64_t* out_offset)
{
  if (file_ctx != &s_mock_stream) {
    return k_ra8_err_invalid_arg;
  }
  *out_offset = s_mock_stream.offset;
  return k_ra8_ok;
}

/** @brief Report the foreign stream size, which is always empty. @details The fixture stores no bytes, so every stream reports zero length. @param[in] file_ctx Stream context published by the fixture open. @param[out] out_bytes Receives the stream size. @return Foreign size status. @retval k_ra8_ok The size was published. @retval k_ra8_err_invalid_arg @p file_ctx is not the static stream. @pre @p out_bytes is non-NULL and writable. @pre The stream is open. @post @p out_bytes reports the empty stream on success. @post No fixture state is modified. @note Test-local; single-threaded host fixture. @since 0.1.0 */
RA8_INTERNAL static ra8_err_t internal_mock_size(const void* file_ctx, uint64_t* out_bytes)
{
  if (file_ctx != &s_mock_stream) {
    return k_ra8_err_invalid_arg;
  }
  *out_bytes = 0U;
  return k_ra8_ok;
}

/** @brief Count one explicit software sync of the foreign stream. @details Proves the VFS dispatched the verb instead of refusing it. @param[in,out] file_ctx Stream context published by the fixture open. @return Foreign sync status. @retval k_ra8_ok The sync dispatch completed. @retval k_ra8_err_invalid_arg @p file_ctx is not the static stream. @pre @p file_ctx was published by this format. @pre The format advertises sync support. @post The sync counter advanced by exactly one. @post No stream bytes are modified. @note Test-local; single-threaded host fixture. @since 0.1.0 */
RA8_INTERNAL static ra8_err_t internal_mock_sync(void* file_ctx)
{
  if (file_ctx != &s_mock_stream) {
    return k_ra8_err_invalid_arg;
  }
  s_mock_stream.syncs++;
  return k_ra8_ok;
}

/** @brief Report the foreign namespace, which holds only an empty root. @details Describes the root as a directory and refuses every other path. @param[in,out] mount_ctx Mounted foreign context. @param[in] path Volume-relative path. @param[out] out Receives the metadata snapshot. @return Foreign stat status. @retval k_ra8_ok The root directory was described. @retval k_ra8_err_not_found Any path other than the root. @retval k_ra8_err_invalid_state @p mount_ctx is not the static context. @pre @p out is non-NULL and writable. @pre @p path is NUL-terminated. @post On success @p out describes a directory of zero bytes. @post No fixture state is modified. @note Test-local; single-threaded host fixture. @since 0.1.0 */
RA8_INTERNAL static ra8_err_t
internal_mock_stat(void* mount_ctx, const char* path, ra8_fs_stat_t* out)
{
  if (mount_ctx != &s_mock) {
    return k_ra8_err_invalid_state;
  }
  *out = (ra8_fs_stat_t){};
  if (strcmp(path, "/") != 0) {
    return k_ra8_err_not_found;
  }
  out->attr         = (uint8_t)k_ra8_fs_attr_directory;
  out->is_directory = true;
  return k_ra8_ok;
}

/** @brief Enumerate the foreign root, which contains no entries. @details Accepts only the root path and never invokes the callback. @param[in,out] mount_ctx Mounted foreign context. @param[in] path Volume-relative directory path. @param[in] cb Per-entry callback (never invoked for an empty root). @param[in,out] cb_ctx Cookie forwarded to @p cb. @return Foreign enumeration status. @retval k_ra8_ok The empty root was enumerated. @retval k_ra8_err_not_found Any path other than the root. @retval k_ra8_err_invalid_state @p mount_ctx is not the static context. @pre @p cb is non-NULL. @pre @p path is NUL-terminated. @post The callback is never invoked. @post No fixture state is modified. @note Test-local; single-threaded host fixture. @since 0.1.0 */
RA8_INTERNAL static ra8_err_t
internal_mock_listdir(void* mount_ctx, const char* path, ra8_fs_listdir_cb_t cb, void* cb_ctx)
{
  (void)cb;
  (void)cb_ctx;
  if (mount_ctx != &s_mock) {
    return k_ra8_err_invalid_state;
  }
  if (strcmp(path, "/") != 0) {
    return k_ra8_err_not_found;
  }
  return k_ra8_ok;
}

/** @brief Open a foreign directory cursor in the caller's workspace. @details Re-checks the advertised workspace extent, then returns the injected result so the VFS's propagation of a format-level cursor failure is observable. @param[in,out] mount_ctx Mounted foreign context. @param[in] path Volume-relative directory path. @param[out] directory_state Caller-owned cursor workspace. @param[in] state_bytes Accessible workspace extent. @return The injected cursor-open status. @retval k_ra8_ok The default injection reports an open cursor. @retval k_ra8_err_invalid_size @p state_bytes is under the advertised requirement, which the VFS extent guard should already have refused. @retval k_ra8_err_invalid_state @p mount_ctx is not the static context. @pre @p directory_state covers @p state_bytes writable bytes. @pre The fixture chose the injected result before the call. @post The workspace is cleared before any success is reported. @post No fixture counter is advanced. @note Test-local; single-threaded host fixture. @since 0.1.0 */
RA8_INTERNAL static ra8_err_t internal_mock_dir_open(void*       mount_ctx,
                                                     const char* path,
                                                     void*       directory_state,
                                                     uint32_t    state_bytes)
{
  (void)path;
  if (mount_ctx != &s_mock) {
    return k_ra8_err_invalid_state;
  }
  if (state_bytes < (uint32_t)k_t_mock_cursor_bytes) {
    return k_ra8_err_invalid_size; /* the VFS extent guard should have fired */
  }
  (void)memset(directory_state, 0, (size_t)k_t_mock_cursor_bytes);
  return s_mock.dir_open_status;
}

/** @brief Report clean end-of-directory for the empty foreign root. @details Publishes no entry, which is the cursor's terminating answer. @param[in,out] directory_state Open cursor workspace. @param[out] out Receives one entry when the cursor has any. @param[out] out_entry Receives whether @p out was populated. @return Foreign cursor status. @retval k_ra8_ok Clean end-of-directory was reported. @retval k_ra8_err_null_ptr The cursor workspace is absent. @pre @p out and @p out_entry are non-NULL and writable. @pre The cursor was opened by the fixture cursor open. @post @p out_entry reports that no entry was produced. @post No fixture state is modified. @note Test-local; single-threaded host fixture. @since 0.1.0 */
RA8_INTERNAL static ra8_err_t
internal_mock_dir_next(void* directory_state, ra8_fs_dirent_t* out, bool* out_entry)
{
  if (directory_state == nullptr) {
    return k_ra8_err_null_ptr;
  }
  *out       = (ra8_fs_dirent_t){};
  *out_entry = false;
  return k_ra8_ok;
}

/** @brief Consume one foreign directory cursor. @details Clears the caller workspace so a stale cursor cannot be reused. @param[in,out] directory_state Open cursor workspace. @return Foreign cursor status. @retval k_ra8_ok The cursor workspace was consumed. @retval k_ra8_err_null_ptr The cursor workspace is absent. @pre The cursor was opened by the fixture cursor open. @pre The caller no longer reads the workspace as a cursor. @post The workspace is cleared on success. @post No fixture state is modified. @note Test-local; single-threaded host fixture. @since 0.1.0 */
RA8_INTERNAL static ra8_err_t internal_mock_dir_close(void* directory_state)
{
  if (directory_state == nullptr) {
    return k_ra8_err_null_ptr;
  }
  (void)memset(directory_state, 0, (size_t)k_t_mock_cursor_bytes);
  return k_ra8_ok;
}

/** @brief Read-side ops plus a cursor and a sync; every mutation is absent. */
static const ra8_io_fsfmt_ops_t s_mock_ops = {
  .probe      = internal_mock_probe,
  .mount      = internal_mock_mount,
  .unmount    = internal_mock_unmount,
  .open       = internal_mock_open,
  .close      = internal_mock_close,
  .read       = internal_mock_read,
  .write      = internal_mock_write,
  .seek       = internal_mock_seek,
  .tell       = internal_mock_tell,
  .size       = internal_mock_size,
  .sync       = internal_mock_sync,
  .stat       = internal_mock_stat,
  .listdir    = internal_mock_listdir,
  .dir_open   = internal_mock_dir_open,
  .dir_next   = internal_mock_dir_next,
  .dir_close  = internal_mock_dir_close,
  .unlink     = nullptr,
  .rename     = nullptr,
  .mkdir      = nullptr,
  .rmdir      = nullptr,
  .free_space = nullptr,
};

/**
 * @brief A writable format that still lacks every namespace mutation.
 * @details `read_only` is false, so each capability-gated verb must be refused
 *          by its own capability flag rather than by the blanket read-only
 *          rejection an entirely read-only format would take.
 */
static const ra8_io_fsfmt_t s_mock_format = {
  .name = "mockfs",
  .caps =
    {
      .directory_workspace_bytes = (uint32_t)k_t_mock_cursor_bytes,
      .max_name_len              = (uint16_t)k_t_mock_name_max,
      .max_open_directories      = (uint16_t)k_t_mock_cursor_max,
      .directory_workspace_align = (uint8_t)k_t_mock_cursor_align,
      .read_only                 = false,
      .supports_mkdir            = false,
      .supports_rmdir            = false,
      .supports_streaming_write  = false,
      .supports_timestamps       = false,
      .supports_free_space       = false,
      .supports_dir_cursor       = true,
      .supports_sync             = true,
      .atomic_rename             = false,
      .durable_sync              = false,
      .unicode_names             = false,
      .case_sensitive            = true,
    },
  .ops = &s_mock_ops,
};

/**
 * @brief Register the foreign format and mount its volume as "mock".
 * @details Resets the fixture state, seeds the marker block, then rebuilds the
 *          format registry and mount table so each vector starts from a known
 *          slate.
 * @param[out] state Caller-owned RAM block-device state.
 * @param[out] blockdev Block-device facade bound to the fixture volume.
 * @param[out] backend Backend descriptor handed to the probe and mount.
 * @pre The three outputs are exclusively owned by the calling vector.
 * @pre The fixture volume storage is not in use by another mount.
 * @post "mock" resolves through the foreign format with an owned context.
 * @post Every fixture counter and injected result is reset.
 * @note The stale-mount reset runs before the counters are cleared, so a
 *       previous vector's owned context cannot be counted against this one.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_mount_mock(ra8_io_blockdev_ram_state_t* state,
                                             ra8_io_blockdev_t*           blockdev,
                                             ra8_fs_backend_t*            backend)
{
  (void)ra8_io_vfs_init(); /* drop any mount an earlier vector left behind */
  s_mock        = (t_mock_mount_t){};
  s_mock_stream = (t_mock_stream_t){};
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_io_blockdev_ram_init(blockdev, state, s_mock_disk, k_t_mock_blocks, false));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_blockdev_erase(blockdev, 0U, k_t_mock_blocks));
  uint8_t block[(size_t)k_ra8_io_block_size_bytes] = {};
  block[k_t_marker_index]                          = (uint8_t)k_t_mock_magic;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_blockdev_write(blockdev, 0U, 1U, block));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_blockdev_as_fs_backend(blockdev, backend));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_fsfmt_init());
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_fsfmt_register(&s_mock_format));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_vfs_init());
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_vfs_mount_auto("mock", backend));
}

/**
 * @par MC/DC:
 * (no compound decisions under test -- the name validator rejects a path
 * separator, the fixed-name copy terminates the longest accepted name, and the
 * splitter rejects an over-long mount field, each at an independent
 * single-condition guard)
 * @brief Bound the mount-name and path-prefix parsers at their exact limits.
 * @details Mounts under the longest name the fixed buffer can terminate and
 *          proves it still resolves, then rejects a name holding a separator
 *          and a qualified path whose name field is one byte too long.
 * @pre A freshly formatted FAT volume carrying HELLO.BIN is available.
 * @pre The VFS mount table has at least two free slots.
 * @post The separator name returned k_ra8_err_invalid_arg.
 * @post The longest accepted name resolves and the over-long path is refused.
 * @note File-local helper; single-threaded host test.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_name_and_path_bounds(void)
{
  TEST_BEGIN("vfs name and path bounds");
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_vfs_init());
  ra8_fs_mount_t* m = internal_setup_volume();
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_io_vfs_mount("a/b", m)); /* reserved '/' */

  TEST_ASSERT_EQ(k_t_long_name_len, strlen(k_t_long_name));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_vfs_mount(k_t_long_name, m));
  ra8_io_vfs_stat_t st = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_vfs_stat("longmountname12:/HELLO.BIN", &st));
  TEST_ASSERT(st.exists);
  TEST_ASSERT_EQ(k_t_payload, st.size_bytes);

  ra8_io_vfs_stat_t overlong = {.exists = true};
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_io_vfs_stat(k_t_overlong_path, &overlong));
  TEST_ASSERT(!overlong.exists);
  TEST_END("vfs name and path bounds");
}

/**
 * @par MC/DC:
 * (no compound decisions under test -- the mode validator, each capability
 * flag, and the stream-facade validity check are independent single-condition
 * guards)
 * @brief Capability-gate every stream verb of a writable no-write format.
 * @details The foreign format is not read-only yet offers no streaming write,
 *          so the mode gate, the write verb, and the sync verb must each take
 *          their own decision; a released facade is then refused.
 * @pre The foreign format is registered and mounted as "mock".
 * @pre The VFS file table is empty.
 * @post An unknown mode returned k_ra8_err_invalid_arg.
 * @post Writing returned k_ra8_err_not_supported, sync dispatched exactly once,
 *       and the released facade returned k_ra8_err_invalid_state.
 * @note File-local helper; single-threaded host test.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_mock_stream_guards(void)
{
  TEST_BEGIN("vfs mock stream capability guards");
  ra8_io_blockdev_ram_state_t state    = {};
  ra8_io_blockdev_t           blockdev = {};
  ra8_fs_backend_t            backend  = {};
  internal_mount_mock(&state, &blockdev, &backend);

  ra8_io_vfs_file_t* file = nullptr;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_io_vfs_file_open("mock:/F", (ra8_fs_mode_t)k_t_bad_mode, &file));
  TEST_ASSERT_EQ(k_ra8_err_not_supported,
                 ra8_io_vfs_file_open("mock:/F", k_ra8_fs_mode_write, &file));
  TEST_ASSERT_EQ(0U, s_mock_stream.opens);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_vfs_file_open("mock:/F", k_ra8_fs_mode_read, &file));

  uint8_t byte = 0U;
  TEST_ASSERT_EQ(k_ra8_err_not_supported, ra8_io_vfs_file_write(file, &byte, 1U));
  TEST_ASSERT_EQ(0U, s_mock_stream.writes);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_vfs_file_sync(file));
  TEST_ASSERT_EQ(1U, s_mock_stream.syncs);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_vfs_file_close(file));

  uint32_t got = 0U;
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, ra8_io_vfs_file_read(file, &byte, 1U, &got));
  TEST_END("vfs mock stream capability guards");
}

/**
 * @brief Reject every stream verb driven through a facade the VFS never issued.
 * @details The pointer is compared against the fixed facade table before any
 *          dereference, so a foreign address is refused rather than followed.
 * @param[in,out] foreign Pointer that is not one of the pooled facades.
 * @pre @p foreign addresses caller storage the VFS never handed out.
 * @pre No pooled facade shares that address.
 * @post Every stream verb returned k_ra8_err_invalid_state.
 * @post The foreign storage is never read or written by the VFS.
 * @note Not thread-safe; single-threaded host-test helper.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_expect_foreign_facade_rejected(ra8_io_vfs_file_t* foreign)
{
  uint8_t  byte  = 0U;
  uint32_t got   = 0U;
  uint64_t value = 0U;
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, ra8_io_vfs_file_close(foreign));
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, ra8_io_vfs_file_read(foreign, &byte, 1U, &got));
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, ra8_io_vfs_file_write(foreign, &byte, 1U));
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, ra8_io_vfs_file_seek(foreign, 0U));
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, ra8_io_vfs_file_tell(foreign, &value));
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, ra8_io_vfs_file_size(foreign, &value));
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, ra8_io_vfs_file_sync(foreign));
}

/**
 * @par MC/DC:
 * (no compound decisions under test -- the facade-table membership check and
 * the fixed-table exhaustion returns are independent single-condition guards)
 * @brief Fill the fixed facade table exactly, then one past it.
 * @details Opens ::k_ra8_io_vfs_max_files streams, proves the next request is
 *          refused for want of a facade rather than by the format, and refuses
 *          a facade pointer the table never issued.
 * @pre The foreign format is registered and mounted as "mock".
 * @pre The VFS file table is empty.
 * @post The table accepted exactly its capacity and then returned
 *       k_ra8_err_no_mem without dispatching another format open.
 * @post Every issued facade was closed and a foreign facade was refused.
 * @note File-local helper; single-threaded host test.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_mock_file_table_limit(void)
{
  TEST_BEGIN("vfs facade table exhaustion");
  ra8_io_blockdev_ram_state_t state    = {};
  ra8_io_blockdev_t           blockdev = {};
  ra8_fs_backend_t            backend  = {};
  internal_mount_mock(&state, &blockdev, &backend);

  ra8_io_vfs_file_t* files[(uint32_t)k_ra8_io_vfs_max_files] = {};
  for (uint32_t i = 0U; i < (uint32_t)k_ra8_io_vfs_max_files; ++i) {
    TEST_ASSERT_EQ(k_ra8_ok, ra8_io_vfs_file_open("mock:/F", k_ra8_fs_mode_read, &files[i]));
  }
  TEST_ASSERT_EQ(k_ra8_io_vfs_max_files, s_mock_stream.opens);
  ra8_io_vfs_file_t* overflow = nullptr;
  TEST_ASSERT_EQ(k_ra8_err_no_mem, ra8_io_vfs_file_open("mock:/F", k_ra8_fs_mode_read, &overflow));
  TEST_ASSERT_NULL(overflow);
  TEST_ASSERT_EQ(k_ra8_io_vfs_max_files, s_mock_stream.opens);

  internal_expect_foreign_facade_rejected((ra8_io_vfs_file_t*)s_foreign_storage);

  for (uint32_t i = 0U; i < (uint32_t)k_ra8_io_vfs_max_files; ++i) {
    TEST_ASSERT_EQ(k_ra8_ok, ra8_io_vfs_file_close(files[i]));
  }
  TEST_ASSERT_EQ(k_ra8_io_vfs_max_files, s_mock_stream.closes);
  TEST_END("vfs facade table exhaustion");
}

/**
 * @par MC/DC:
 * (no compound decisions under test -- each namespace verb consults its own
 * capability flag or operation pointer at an independent single-condition
 * guard)
 * @brief Refuse every namespace mutation the foreign format cannot dispatch.
 * @details The format is writable, so each verb must be stopped by its own
 *          missing capability or absent operation; unmounted names are refused
 *          by the lookup instead.
 * @pre The foreign format is registered and mounted as "mock".
 * @pre No other mount is registered under the queried absent name.
 * @post Unlink, rename, mkdir and rmdir returned k_ra8_err_not_supported.
 * @post Capability, free-space and rename lookups of an absent mount returned
 *       k_ra8_err_not_found.
 * @note File-local helper; single-threaded host test.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_mock_namespace_guards(void)
{
  TEST_BEGIN("vfs mock namespace guards");
  ra8_io_blockdev_ram_state_t state    = {};
  ra8_io_blockdev_t           blockdev = {};
  ra8_fs_backend_t            backend  = {};
  internal_mount_mock(&state, &blockdev, &backend);

  TEST_ASSERT_EQ(k_ra8_err_not_supported, ra8_io_vfs_unlink("mock:/F"));
  TEST_ASSERT_EQ(k_ra8_err_not_supported, ra8_io_vfs_rename("mock:/A", "mock:/B"));
  TEST_ASSERT_EQ(k_ra8_err_not_supported, ra8_io_vfs_mkdir("mock:/D"));
  TEST_ASSERT_EQ(k_ra8_err_not_supported, ra8_io_vfs_rmdir("mock:/D"));

  ra8_io_fsfmt_caps_t caps = {.supports_dir_cursor = true};
  TEST_ASSERT_EQ(k_ra8_err_not_found, ra8_io_vfs_get_caps("absent", &caps));
  TEST_ASSERT(caps.supports_dir_cursor); /* an absent mount publishes nothing */
  ra8_fs_space_t space = {};
  TEST_ASSERT_EQ(k_ra8_err_not_found, ra8_io_vfs_free_space("absent", &space));
  TEST_ASSERT_EQ(k_ra8_err_not_found, ra8_io_vfs_rename("absent:/A", "absent:/B"));
  TEST_END("vfs mock namespace guards");
}

/**
 * @par MC/DC:
 * (no compound decisions under test -- the cursor busy, capability, extent,
 * alignment and dispatch guards are independent single-condition checks)
 * @brief Prove every directory-cursor precondition of the VFS facade.
 * @details Drives an unresolvable path, an undersized workspace, a misaligned
 *          workspace, a format-level open failure, and finally a live cursor
 *          that refuses a second open until it is closed.
 * @pre The foreign format is registered and mounted as "mock".
 * @pre The cursor workspace is 64-bit aligned and larger than the requirement.
 * @post Each precondition returned its own documented status.
 * @post A format-level failure left the caller's cursor closed and the
 *       successful cursor was consumed before return.
 * @note File-local helper; single-threaded host test.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_mock_cursor_guards(void)
{
  TEST_BEGIN("vfs mock cursor guards");
  ra8_io_blockdev_ram_state_t state    = {};
  ra8_io_blockdev_t           blockdev = {};
  ra8_fs_backend_t            backend  = {};
  internal_mount_mock(&state, &blockdev, &backend);

  uint64_t         words[(uint32_t)k_t_mock_ws_words] = {};
  uint8_t* const   workspace                          = (uint8_t*)words;
  ra8_io_vfs_dir_t directory                          = {};
  uint32_t         bytes                              = 0U;
  uint8_t          align                              = 0U;
  uint16_t         max_open                           = 0U;
  TEST_ASSERT_EQ(k_ra8_err_not_found,
                 ra8_io_vfs_dir_requirements("absent:/", &bytes, &align, &max_open));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_vfs_dir_requirements("mock:/", &bytes, &align, &max_open));
  TEST_ASSERT_EQ(k_t_mock_cursor_bytes, bytes);
  TEST_ASSERT_EQ(k_t_mock_cursor_align, align);
  TEST_ASSERT_EQ(k_t_mock_cursor_max, max_open);

  TEST_ASSERT_EQ(k_ra8_err_not_found,
                 ra8_io_vfs_dir_open("absent:/", &directory, workspace, (uint32_t)sizeof(words)));
  TEST_ASSERT_EQ(k_ra8_err_no_mem,
                 ra8_io_vfs_dir_open("mock:/", &directory, workspace, k_t_mock_small_ws));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_io_vfs_dir_open("mock:/",
                                     &directory,
                                     &workspace[k_t_misalign_offset],
                                     (uint32_t)sizeof(words) - 1U));
  s_mock.dir_open_status = k_ra8_err_hw_timeout;
  TEST_ASSERT_EQ(k_ra8_err_hw_timeout,
                 ra8_io_vfs_dir_open("mock:/", &directory, workspace, (uint32_t)sizeof(words)));
  TEST_ASSERT(!directory.is_open);

  s_mock.dir_open_status = k_ra8_ok;
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_io_vfs_dir_open("mock:/", &directory, workspace, (uint32_t)sizeof(words)));
  ra8_fs_dirent_t entry   = {};
  bool            present = true;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_vfs_dir_next(&directory, &entry, &present));
  TEST_ASSERT(!present);
  TEST_ASSERT_EQ(k_ra8_err_busy,
                 ra8_io_vfs_dir_open("mock:/", &directory, workspace, (uint32_t)sizeof(words)));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_vfs_dir_close(&directory));
  TEST_END("vfs mock cursor guards");
}

/**
 * @par MC/DC:
 * (no compound decisions under test -- the probing mount rejects a bad name, a
 * duplicate name and an exhausted table at independent single-condition
 * guards, and the reset walks one owned slot)
 * @brief Bound the probing mount path and the owned-context reset.
 * @details Fills the mount table with legacy native registrations so the next
 *          probing mount has no slot, then proves that resetting the VFS
 *          unmounts the owned foreign context and reports its error verbatim.
 * @pre The foreign format is registered and mounted as "mock".
 * @pre A freshly formatted FAT volume is available for the filler names.
 * @post The invalid, duplicate and overflowing names returned their own codes.
 * @post The reset dispatched exactly one owned unmount and propagated its
 *       injected failure.
 * @note File-local helper; single-threaded host test.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_mock_mount_table_limits(void)
{
  TEST_BEGIN("vfs probing mount table limits");
  ra8_io_blockdev_ram_state_t state    = {};
  ra8_io_blockdev_t           blockdev = {};
  ra8_fs_backend_t            backend  = {};
  internal_mount_mock(&state, &blockdev, &backend);

  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_io_vfs_mount_auto("a/b", &backend));
  TEST_ASSERT_EQ(k_ra8_err_exists, ra8_io_vfs_mount_auto("mock", &backend));

  ra8_fs_mount_t* m = internal_setup_volume();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_vfs_mount("a", m));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_vfs_mount("b", m));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_vfs_mount("c", m));
  TEST_ASSERT_EQ(k_ra8_err_no_mem, ra8_io_vfs_mount_auto("d", &backend));

  s_mock.unmount_status = k_ra8_err_hw_timeout;
  TEST_ASSERT_EQ(k_ra8_err_hw_timeout, ra8_io_vfs_init());
  TEST_ASSERT_EQ(1U, s_mock.unmounts);
  TEST_ASSERT_EQ(k_ra8_err_not_found, ra8_io_vfs_unmount("mock"));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_vfs_init());
  TEST_ASSERT_EQ(1U, s_mock.unmounts);
  TEST_END("vfs probing mount table limits");
}

/** @brief Count of fixture-format unmount dispatches for the init_slot MC/DC test. */
static uint32_t s_init_slot_unmounts;

/** @brief Fixture unmount that only counts dispatches. @details Performs one bounded, deterministic operation for this host test. @param[in,out] mount_ctx Unused fixture context. @return Always k_ra8_ok. @retval k_ra8_ok The fixture never injects a failure. @pre None. @post s_init_slot_unmounts advances by exactly one. @note Test-local; single-threaded host fixture. @since 0.1.0 */
RA8_INTERNAL static ra8_err_t internal_init_slot_unmount(void* mount_ctx)
{
  (void)mount_ctx;
  s_init_slot_unmounts++;
  return k_ra8_ok;
}

/** @brief Minimal ops table exposing only unmount, for the init_slot fixture. */
static const ra8_io_fsfmt_ops_t s_init_slot_ops = {.unmount = internal_init_slot_unmount};

/** @brief Minimal registered format for the init_slot fixture. */
static const ra8_io_fsfmt_t s_init_slot_fmt = {.name = "init_slot_fixture",
                                               .ops  = &s_init_slot_ops};

/**
 * @brief Drive every MC/DC vector of the mount-slot reset decision directly.
 * @details Builds a ::vfs_slot_t by hand for each vector so `in_use` and
 * `owned` vary independently without needing three different mount paths to
 * reach every combination.
 * @par MC/DC:
 * Decision: `if (slot->in_use && slot->owned)`
 * (2 conditions, libs/ra8_io/src/ra8_io_vfs.c@internal_vfs_init_slot)
 * - Vector 1: in_use=true,  owned=true  -> true  -> unmount dispatched once.
 * - Vector 2: in_use=false, owned=true  -> false -> unmount not dispatched.
 * - Vector 3: in_use=true,  owned=false -> false -> unmount not dispatched.
 * Vectors 1+2 flip the outcome varying in_use only; vectors 1+3 flip it
 * varying owned only. N+1 = 3 vectors for N=2 conditions: minimal MC/DC.
 * @pre None; each vector builds its own slot.
 * @post Every vector's slot is zero-initialized on return.
 * @note Not thread-safe; single-threaded host test.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_vfs_init_slot_mcdc(void)
{
  TEST_BEGIN("vfs init_slot MC/DC");

  vfs_slot_t slot      = {.format = &s_init_slot_fmt, .in_use = true, .owned = true};
  s_init_slot_unmounts = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_vfs_init_slot_test(&slot));
  TEST_ASSERT_EQ(1U, s_init_slot_unmounts);
  TEST_ASSERT(!slot.in_use);

  slot                 = (vfs_slot_t){.format = &s_init_slot_fmt, .in_use = false, .owned = true};
  s_init_slot_unmounts = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_vfs_init_slot_test(&slot));
  TEST_ASSERT_EQ(0U, s_init_slot_unmounts);

  slot                 = (vfs_slot_t){.format = &s_init_slot_fmt, .in_use = true, .owned = false};
  s_init_slot_unmounts = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_vfs_init_slot_test(&slot));
  TEST_ASSERT_EQ(0U, s_init_slot_unmounts);

  TEST_END("vfs init_slot MC/DC");
}

int main(void)
{
  internal_test_mount_table();
  internal_test_open_read();
  internal_test_stat();
  internal_test_rename_unlink();
  internal_test_listdir_mkdir_unmount();
  internal_test_rmdir();
  internal_test_name_and_path_bounds();
  internal_test_mock_stream_guards();
  internal_test_mock_file_table_limit();
  internal_test_mock_namespace_guards();
  internal_test_mock_cursor_guards();
  internal_test_mock_mount_table_limits();
  internal_test_vfs_init_slot_mcdc();
  return 0;
}
