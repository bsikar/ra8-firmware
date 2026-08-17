/**
 * @file test_ra8_mdl_state.c
 * @brief Identical downloader-state vectors over POSIX and real RAM/FAT/VFS.
 * @details Qualifies journal publication, faults, recovery, and remounts on both backends.
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "fw_if_fs_posix.h"
#include "fw_if_fs_ra8_vfs.h"
#include "mdl_state.h"
#include "mdl_state_fs_fault.h"
#include "mdl_storage.h"
#include "ra8_attributes.h"
#include "ra8_fs.h"
#include "ra8_io_blockdev.h"
#include "ra8_io_blockdev_ram.h"
#include "ra8_io_vfs.h"
#include "unity_minimal.h"

/** @brief Fixed disk, workspace, path, and envelope bounds. */
typedef enum : uint32_t {
  k_state_disk_blocks     = 4096U, /**< RAM block-device capacity.      */
  k_state_work_bytes      = 8192U, /**< Generic backend workspace size. */
  k_state_io_bytes        = 512U,  /**< State scratch I/O capacity.     */
  k_state_file_bytes      = 4096U, /**< Fixture file capacity.          */
  k_state_header_bytes    = 40U,   /**< Canonical journal header size.  */
  k_state_header_crc_span = 36U,   /**< Authenticated header byte span. */
} mdl_state_vfs_limit_t;

/** @brief Maximally aligned generic backend workspace. */
typedef union {
  max_align_t alignment;                 /**< Forces maximum host alignment. */
  uint8_t     bytes[k_state_work_bytes]; /**< Backend workspace storage.     */
} mdl_state_test_workspace_t;

static uint8_t s_disk[(size_t)k_state_disk_blocks * (size_t)k_ra8_io_block_size_bytes];
static ra8_io_blockdev_ram_state_t s_ram_state;
static ra8_io_blockdev_t           s_blockdev;
static ra8_fs_backend_t            s_backend;
static ra8_fs_mount_t*             s_mount;
static mdl_state_t                 s_state;
static mdl_state_t                 s_loaded;
static mdl_state_test_workspace_t  s_file_work;
static mdl_state_test_workspace_t  s_transaction_work;
static uint8_t                     s_io[k_state_io_bytes];

/**
 * @brief Bind state storage through the delegating fault facade.
 * @details Initializes the facade and fixed-workspace downloader storage.
 * @param[in] inner Backend qualified by the current vector.
 * @param[in,out] fault Fault-injection facade to initialize.
 * @param[out] storage Downloader storage binding to initialize.
 * @pre All pointers are non-null.
 * @pre Workspace arrays are not in active use.
 * @post The facade delegates to @p inner.
 * @post @p storage owns no dynamic memory.
 * @note Unity assertions terminate the vector on initialization failure.
 * @since v1.0.0
 */
RA8_INTERNAL static void
internal_bind_storage(const fw_fs_t* inner, mdl_state_fault_fs_t* fault, mdl_storage_t* storage)
{
  TEST_ASSERT_EQ(k_ra8_ok, mdl_state_fault_fs_init(fault, inner));
  TEST_ASSERT_EQ(k_ra8_ok,
                 mdl_storage_init(storage,
                                  &fault->fs,
                                  s_file_work.bytes,
                                  sizeof(s_file_work.bytes),
                                  s_transaction_work.bytes,
                                  sizeof(s_transaction_work.bytes),
                                  s_io,
                                  sizeof(s_io)));
}

/**
 * @brief Seed one small but semantically rich current state.
 * @details Builds a complete one-chapter fixture in the shared state object.
 * @param[in] title Series title copied into the fixture.
 * @param[in] number Exact chapter number to persist.
 * @pre @p title is non-null and bounded by the state schema.
 * @pre @p number is a finite binary64 value.
 * @post The shared state contains one complete chapter and page.
 * @post Rich series and page metadata are populated.
 * @note Unity assertions enforce every bounded insertion.
 * @since v1.0.0
 */
RA8_INTERNAL static void internal_seed_state(const char* title, double number)
{
  mdl_state_init(&s_state);
  mdl_state_set_series(&s_state, "https://s/series", title, "site", "s", "/site.cfg");
  TEST_ASSERT(mdl_state_set_series_metadata(&s_state,
                                            "summary",
                                            "writer",
                                            "artist",
                                            "https://s/cover.webp",
                                            "cover.webp",
                                            "en-US",
                                            k_mdl_state_read_rtl));
  mdl_chapter_rec_t* chapter =
    mdl_state_add_chapter_numbered(&s_state, "chapter-1", "https://s/chapter-1", number, true);
  TEST_ASSERT_NOT_NULL(chapter);
  chapter->page_count = 1U;
  chapter->pages_done = 1U;
  chapter->complete   = true;
  TEST_ASSERT(
    mdl_state_add_page(&s_state, 0x11U, 0x22U, "chapter-1/page.webp", "etag", "date", 1234, 200U));
}

/**
 * @brief Save the current fixture and assert publication semantics.
 * @details Persists the shared fixture and requires a visible new generation.
 * @param[in,out] storage Initialized storage binding.
 * @param[in] path Logical base path for the journal pair.
 * @pre @p storage and @p path are non-null.
 * @pre The shared fixture passes state validation.
 * @post A journal generation is visible at @p path or its alternate.
 * @post The published result has been asserted true.
 * @note Any save error terminates the active Unity vector.
 * @since v1.0.0
 */
RA8_INTERNAL static void internal_save_ok(mdl_storage_t* storage, const char* path)
{
  bool published = false;
  TEST_ASSERT_EQ(k_ra8_ok, mdl_state_save(storage, path, &s_state, &published));
  TEST_ASSERT(published);
}

/**
 * @brief Report whether two binary64 values have identical object bytes.
 * @details The persisted format round-trips the exact bit pattern, so this is
 *          a representation comparison and not a numeric one -- that is what
 *          keeps -0.0 distinct from +0.0. The bytes are read into an unsigned
 *          integer and compared there, because `double` has no unique object
 *          representation and comparing two of them byte-wise says nothing
 *          about which of the two properties is being asserted.
 * @param[in] left First value.
 * @param[in] right Second value.
 * @return Whether both values have the same 64-bit representation.
 * @retval true The representations are identical.
 * @retval false At least one representation bit differs.
 * @pre `double` is IEEE-754 binary64 on this host.
 * @pre Both arguments are ordinary readable values.
 * @post Neither argument is modified.
 * @post The result depends only on the two representations.
 * @note Test-only helper with no production ABI.
 * @since 0.1.0
 */
RA8_INTERNAL static bool internal_same_binary64(double left, double right)
{
  static_assert(sizeof(double) == sizeof(uint64_t), "binary64 host required");
  uint64_t left_bits  = 0U;
  uint64_t right_bits = 0U;
  (void)memcpy(&left_bits, &left, sizeof(left_bits));
  (void)memcpy(&right_bits, &right, sizeof(right_bits));
  return left_bits == right_bits;
}

/**
 * @brief Load one state and assert its identity plus exact chapter bits.
 * @details Verifies title, one-page shape, and bit-exact chapter identity.
 * @param[in,out] storage Initialized storage binding.
 * @param[in] path Logical journal base path.
 * @param[in] title Expected series title.
 * @param[in] number Expected exact chapter number.
 * @pre All pointer parameters are non-null.
 * @pre @p number is represented by the supported binary64 contract.
 * @post The shared loaded state holds the selected valid generation.
 * @post All expected identity assertions have passed.
 * @note Comparison uses object bytes to preserve signed zero.
 * @since v1.0.0
 */
RA8_INTERNAL static void
internal_assert_loaded(mdl_storage_t* storage, const char* path, const char* title, double number)
{
  TEST_ASSERT_EQ(k_ra8_ok, mdl_state_load(storage, path, &s_loaded));
  TEST_ASSERT(strcmp(s_loaded.series_title, title) == 0);
  const mdl_chapter_rec_t* chapter = mdl_state_find_chapter(&s_loaded, "chapter-1");
  TEST_ASSERT_NOT_NULL(chapter);
  TEST_ASSERT(internal_same_binary64(chapter->number, number));
  TEST_ASSERT_EQ(1U, s_loaded.page_rec_count);
}

/**
 * @brief Write an exact portable fixture without using stdio persistence.
 * @details Streams all bytes through the injected file contract and closes it.
 * @param[in,out] storage Initialized storage with file workspace.
 * @param[in] path Destination path.
 * @param[in] bytes Fixture bytes to write.
 * @param[in] len Exact fixture byte count.
 * @pre All pointer parameters are non-null.
 * @pre @p bytes addresses at least @p len readable bytes.
 * @post Exactly @p len bytes were accepted.
 * @post The file was successfully closed.
 * @note Short writes are retried to exercise stream semantics.
 * @since v1.0.0
 */
RA8_INTERNAL static void
internal_write_fixture(mdl_storage_t* storage, const char* path, const uint8_t* bytes, uint32_t len)
{
  fw_fs_file_t file = {};
  TEST_ASSERT_EQ(k_ra8_ok,
                 fw_fs_open(&storage->fs->streams,
                            path,
                            k_fw_fs_open_write_truncate,
                            &file,
                            storage->file_workspace,
                            storage->file_workspace_bytes));
  uint32_t offset = 0U;
  while (offset < len) {
    uint32_t count = 0U;
    TEST_ASSERT_EQ(k_ra8_ok, fw_fs_write(&file, bytes + offset, len - offset, &count));
    TEST_ASSERT(count > 0U);
    offset += count;
  }
  TEST_ASSERT_EQ(k_ra8_ok, fw_fs_close(&file));
}

/**
 * @brief Read one complete bounded portable fixture.
 * @details Reads the exact reported file size through the injected stream.
 * @param[in,out] storage Initialized storage with file workspace.
 * @param[in] path Source path.
 * @param[out] bytes Destination byte array.
 * @param[in] cap Destination capacity in bytes.
 * @return Number of fixture bytes read.
 * @retval 0 The fixture is empty.
 * @pre All pointer parameters are non-null.
 * @pre @p bytes has capacity @p cap.
 * @post The returned count does not exceed @p cap.
 * @post The source stream was successfully closed.
 * @note Oversized or failed reads terminate the Unity vector.
 * @since v1.0.0
 */
RA8_INTERNAL static uint32_t
internal_read_fixture(mdl_storage_t* storage, const char* path, uint8_t* bytes, uint32_t cap)
{
  fw_fs_file_t file = {};
  TEST_ASSERT_EQ(k_ra8_ok,
                 fw_fs_open(&storage->fs->streams,
                            path,
                            k_fw_fs_open_read,
                            &file,
                            storage->file_workspace,
                            storage->file_workspace_bytes));
  uint64_t size = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, fw_fs_file_size(&file, &size));
  TEST_ASSERT(size <= cap);
  uint32_t offset = 0U;
  while (offset < (uint32_t)size) {
    uint32_t count = 0U;
    TEST_ASSERT_EQ(k_ra8_ok, fw_fs_read(&file, bytes + offset, (uint32_t)size - offset, &count));
    TEST_ASSERT(count > 0U);
    offset += count;
  }
  TEST_ASSERT_EQ(k_ra8_ok, fw_fs_close(&file));
  return offset;
}

/**
 * @brief Update an unfinalized reflected CRC-32 state.
 * @details Applies the journal header polynomial without final complement.
 * @param[in] state Initial CRC state.
 * @param[in] bytes Input byte sequence.
 * @param[in] length Number of bytes to consume.
 * @return Updated unfinalized CRC state.
 * @retval UINT32_MAX CRC of an empty sequence from the standard seed.
 * @pre @p bytes is non-null when @p length is nonzero.
 * @pre @p bytes addresses at least @p length readable bytes.
 * @post Every input byte has been processed exactly once.
 * @post The input buffer is unchanged.
 * @note This independent test helper authenticates a modified header.
 * @since v1.0.0
 */
RA8_INTERNAL static uint32_t
internal_crc32_update(uint32_t state, const uint8_t* bytes, uint32_t length)
{
  for (uint32_t i = 0U; i < length; ++i) {
    state ^= bytes[i];
    for (uint8_t bit = 0U; bit < 8U; ++bit) {
      const uint32_t mask = (uint32_t)(0U - (state & 1U));
      state               = (state >> 1U) ^ (0xEDB88320UL & mask);
    }
  }
  return state;
}

/**
 * @brief Encode one canonical big-endian 32-bit test value.
 * @details Stores the four octets from most to least significant.
 * @param[out] out Four-byte destination.
 * @param[in] value Value to encode.
 * @pre @p out is non-null.
 * @pre @p out addresses at least four writable bytes.
 * @post The destination contains the canonical big-endian value.
 * @post No bytes outside the four-byte destination are changed.
 * @note Used only to construct an authenticated exhaustion fixture.
 * @since v1.0.0
 */
RA8_INTERNAL static void internal_put_be32(uint8_t* out, uint32_t value)
{
  out[0] = (uint8_t)(value >> 24U);
  out[1] = (uint8_t)(value >> 16U);
  out[2] = (uint8_t)(value >> 8U);
  out[3] = (uint8_t)value;
}

/**
 * @brief Set a journal sequence to UINT64_MAX and authenticate its header.
 * @details Rewrites a valid generation into the terminal sequence fixture.
 * @param[in,out] storage Initialized storage binding.
 * @param[in] path Existing generation path.
 * @pre @p storage and @p path are non-null.
 * @pre The source is a complete current journal generation.
 * @post The stored sequence is UINT64_MAX.
 * @post The modified header carries a matching CRC.
 * @note This deliberately mutates only the test fixture.
 * @since v1.0.0
 */
RA8_INTERNAL static void internal_force_sequence_exhaustion(mdl_storage_t* storage,
                                                            const char*    path)
{
  uint8_t  bytes[k_state_file_bytes];
  uint32_t length = internal_read_fixture(storage, path, bytes, sizeof(bytes));
  TEST_ASSERT(length > (uint32_t)k_state_header_bytes);
  memset(&bytes[16], 0xFF, 8U);
  const uint32_t crc = ~internal_crc32_update(UINT32_MAX, bytes, (uint32_t)k_state_header_crc_span);
  internal_put_be32(&bytes[36], crc);
  internal_write_fixture(storage, path, bytes, length);
}

/**
 * @brief Remove one state generation when it exists.
 * @details Queries the namespace before conditionally unlinking the path.
 * @param[in,out] storage Initialized storage binding.
 * @param[in] path Generation path to remove.
 * @pre @p storage and @p path are non-null.
 * @pre The namespace interface is initialized.
 * @post The path does not identify an existing file.
 * @post A missing input path remains missing.
 * @note Namespace errors terminate the Unity vector.
 * @since v1.0.0
 */
RA8_INTERNAL static void internal_remove_file_if_present(mdl_storage_t* storage, const char* path)
{
  fw_fs_stat_t stat = {};
  TEST_ASSERT_EQ(k_ra8_ok, fw_fs_stat(&storage->fs->names, path, &stat));
  if (stat.exists) {
    TEST_ASSERT_EQ(k_ra8_ok, fw_fs_unlink(&storage->fs->names, path));
  }
}

/**
 * @brief Prove clean round-trip and generation recovery behavior.
 * @details Exercises alternation, corruption, torn files, and alternate-only probe.
 * @param[in,out] storage Initialized backend storage.
 * @pre @p storage is non-null.
 * @pre Vector paths do not exist on entry.
 * @post The newest valid generation remains loadable.
 * @post Alternate-only state is recognized as tracked.
 * @note The same vector runs unchanged on both backend families.
 * @since v1.0.0
 */
RA8_INTERNAL static void internal_run_clean_vectors(mdl_storage_t* storage)
{
  TEST_ASSERT_EQ(k_ra8_ok, fw_fs_mkdir(&storage->fs->names, "/series"));
  bool exists = true;
  TEST_ASSERT_EQ(k_ra8_ok, mdl_state_probe(storage, "/series/.mdl_state", &exists));
  TEST_ASSERT(!exists);
  internal_seed_state("one", 1.5);
  internal_save_ok(storage, "/series/.mdl_state");
  internal_seed_state("two", -0.0);
  internal_save_ok(storage, "/series/.mdl_state");
  internal_assert_loaded(storage, "/series/.mdl_state", "two", -0.0);
  uint8_t  generation[k_state_file_bytes];
  uint32_t generation_bytes =
    internal_read_fixture(storage, "/series/.mdl_state.alt", generation, sizeof(generation));
  TEST_ASSERT(generation_bytes < sizeof(generation));
  generation[generation_bytes] = 0xA5U;
  internal_write_fixture(storage, "/series/.mdl_state.alt", generation, generation_bytes + 1U);
  internal_assert_loaded(storage, "/series/.mdl_state", "one", 1.5);
  internal_seed_state("two", -0.0);
  internal_save_ok(storage, "/series/.mdl_state");
  internal_write_fixture(storage, "/series/.mdl_state", (const uint8_t*)"torn", 4U);
  internal_assert_loaded(storage, "/series/.mdl_state", "two", -0.0);
  internal_seed_state("three", 108.5);
  internal_save_ok(storage, "/series/.mdl_state");
  internal_remove_file_if_present(storage, "/series/.mdl_state.alt");
  internal_assert_loaded(storage, "/series/.mdl_state", "three", 108.5);
  internal_seed_state("four", 4.25);
  internal_save_ok(storage, "/series/.mdl_state");
  internal_remove_file_if_present(storage, "/series/.mdl_state");
  TEST_ASSERT_EQ(k_ra8_ok, mdl_state_probe(storage, "/series/.mdl_state", &exists));
  TEST_ASSERT(exists);
  internal_assert_loaded(storage, "/series/.mdl_state", "four", 4.25);
}

/**
 * @brief Prove v1 migration and transactionally empty corrupt output.
 * @details Loads legacy text, republishes the current schema, and rejects
 *          malformed state safely.
 * @param[in,out] storage Initialized backend storage.
 * @pre @p storage is non-null.
 * @pre Migration vector paths do not exist on entry.
 * @post The legacy fixture is recoverable after current-schema publication.
 * @post A failed load leaves the output initialized and empty.
 * @note Legacy input is written through the same portable stream contract.
 * @since v1.0.0
 */
RA8_INTERNAL static void internal_run_migration_vectors(mdl_storage_t* storage)
{
  TEST_ASSERT_EQ(k_ra8_ok, fw_fs_mkdir(&storage->fs->names, "/legacy"));
  static const uint8_t legacy[] =
    "V\t1\nS\thttps://s/old\nC\tchapter-7\t7\t1\t1\t1\t42\thttps://s/chapter-7\n";
  internal_write_fixture(storage, "/legacy/.mdl_state", legacy, sizeof(legacy) - 1U);
  TEST_ASSERT_EQ(k_ra8_ok, mdl_state_load(storage, "/legacy/.mdl_state", &s_loaded));
  TEST_ASSERT(mdl_state_find_chapter(&s_loaded, "chapter-7")->number == 7.0);
  s_state = s_loaded;
  internal_save_ok(storage, "/legacy/.mdl_state");
  internal_remove_file_if_present(storage, "/legacy/.mdl_state");
  TEST_ASSERT_EQ(k_ra8_ok, mdl_state_load(storage, "/legacy/.mdl_state", &s_loaded));
  TEST_ASSERT_EQ(k_mdl_state_version, s_loaded.version);

  TEST_ASSERT_EQ(k_ra8_ok, fw_fs_mkdir(&storage->fs->names, "/bad"));
  internal_write_fixture(storage, "/bad/.mdl_state", (const uint8_t*)"broken", 6U);
  internal_seed_state("sentinel", 9.0);
  s_loaded = s_state;
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, mdl_state_load(storage, "/bad/.mdl_state", &s_loaded));
  TEST_ASSERT_EQ(0U, s_loaded.chapter_count);
  TEST_ASSERT(s_loaded.series_title[0] == '\0');
}

/**
 * @brief Prove canonical sequence exhaustion never wraps.
 * @details Attempts publication after authenticating a UINT64_MAX generation.
 * @param[in,out] storage Initialized backend storage.
 * @pre @p storage is non-null.
 * @pre The overflow vector path does not exist on entry.
 * @post Save reports invalid size without publication.
 * @post The terminal generation remains readable and unchanged.
 * @note Sequence rollover is rejected rather than ordered ambiguously.
 * @since v1.0.0
 */
RA8_INTERNAL static void internal_run_sequence_vector(mdl_storage_t* storage)
{
  TEST_ASSERT_EQ(k_ra8_ok, fw_fs_mkdir(&storage->fs->names, "/overflow"));
  internal_seed_state("max", 2.0);
  internal_save_ok(storage, "/overflow/.mdl_state");
  internal_force_sequence_exhaustion(storage, "/overflow/.mdl_state");
  bool published = true;
  TEST_ASSERT_EQ(k_ra8_err_invalid_size,
                 mdl_state_save(storage, "/overflow/.mdl_state", &s_state, &published));
  TEST_ASSERT(!published);
  internal_assert_loaded(storage, "/overflow/.mdl_state", "max", 2.0);
}

/**
 * @brief Assert an unpublished injected save leaves baseline state readable.
 * @details Arms one fault, checks its exact error, then reloads the baseline.
 * @param[in,out] fault Fault facade to arm and clear.
 * @param[in,out] storage Storage bound through @p fault.
 * @param[in] flags Fault flags to inject.
 * @param[in] expected Expected save error.
 * @pre @p fault and @p storage are non-null.
 * @pre A baseline generation exists at the fault-vector path.
 * @post Publication was reported false.
 * @post The baseline remains readable after clearing the fault.
 * @note Combined write and abort faults verify cleanup precedence.
 * @since v1.0.0
 */
RA8_INTERNAL static void internal_expect_unpublished(mdl_state_fault_fs_t* fault,
                                                     mdl_storage_t*        storage,
                                                     uint32_t              flags,
                                                     ra8_err_t             expected)
{
  internal_seed_state("attempt", 6.5);
  fault->flags   = flags;
  bool published = true;
  TEST_ASSERT_EQ(expected, mdl_state_save(storage, "/fault/.mdl_state", &s_state, &published));
  TEST_ASSERT(!published);
  fault->flags = 0U;
  internal_assert_loaded(storage, "/fault/.mdl_state", "baseline", 5.5);
}

/**
 * @brief Exercise every state-visible injected transaction failure.
 * @details Checks begin, write, seek, validation, unlink, commit, and abort paths.
 * @param[in,out] fault Fault facade to arm and clear.
 * @param[in,out] storage Storage bound through @p fault.
 * @pre @p fault and @p storage are non-null.
 * @pre The fault vector path does not exist on entry.
 * @post Unpublished failures preserve the baseline generation.
 * @post Post-publication commit failure reports publication truthfully.
 * @note Error and cleanup precedence are asserted explicitly.
 * @since v1.0.0
 */
RA8_INTERNAL static void internal_run_fault_vectors(mdl_state_fault_fs_t* fault,
                                                    mdl_storage_t*        storage)
{
  TEST_ASSERT_EQ(k_ra8_ok, fw_fs_mkdir(&storage->fs->names, "/fault"));
  internal_seed_state("baseline", 5.5);
  internal_save_ok(storage, "/fault/.mdl_state");
  internal_expect_unpublished(fault, storage, k_mdl_state_fault_begin, k_ra8_err_hw_error);
  internal_expect_unpublished(fault,
                              storage,
                              k_mdl_state_fault_transaction_write,
                              k_ra8_err_hw_error);
  internal_expect_unpublished(fault,
                              storage,
                              (uint32_t)k_mdl_state_fault_transaction_write |
                                (uint32_t)k_mdl_state_fault_abort,
                              k_ra8_err_cancelled);
  internal_expect_unpublished(fault, storage, k_mdl_state_fault_seek, k_ra8_err_hw_error);
  internal_expect_unpublished(fault,
                              storage,
                              k_mdl_state_fault_validate,
                              k_ra8_err_validation_failed);
  internal_expect_unpublished(fault,
                              storage,
                              k_mdl_state_fault_commit_before,
                              k_ra8_err_hw_timeout);
  internal_seed_state("baseline", 5.5);
  internal_save_ok(storage, "/fault/.mdl_state");
  internal_expect_unpublished(fault, storage, k_mdl_state_fault_unlink, k_ra8_err_hw_error);

  internal_seed_state("published-error", 6.75);
  fault->flags   = (uint32_t)k_mdl_state_fault_commit_after;
  bool published = false;
  TEST_ASSERT_EQ(k_ra8_err_hw_timeout,
                 mdl_state_save(storage, "/fault/.mdl_state", &s_state, &published));
  TEST_ASSERT(published);
  fault->flags = 0U;
  internal_assert_loaded(storage, "/fault/.mdl_state", "published-error", 6.75);
}

/**
 * @brief Exercise short I/O plus read and close failures.
 * @details Verifies retry behavior and transactional output reset on load errors.
 * @param[in,out] fault Fault facade to arm and clear.
 * @param[in,out] storage Storage bound through @p fault.
 * @pre @p fault and @p storage are non-null.
 * @pre The fault vector has a valid baseline generation.
 * @post Short reads and writes preserve exact state identity.
 * @post Failed loads leave the caller output initialized and empty.
 * @note Fault flags are cleared before returning.
 * @since v1.0.0
 */
RA8_INTERNAL static void internal_run_stream_fault_vectors(mdl_state_fault_fs_t* fault,
                                                           mdl_storage_t*        storage)
{
  internal_seed_state("short-write", 7.25);
  fault->flags = (uint32_t)k_mdl_state_fault_short_write;
  internal_save_ok(storage, "/fault/.mdl_state");
  fault->flags = (uint32_t)k_mdl_state_fault_short_read;
  internal_assert_loaded(storage, "/fault/.mdl_state", "short-write", 7.25);
  fault->flags = (uint32_t)k_mdl_state_fault_read;
  TEST_ASSERT_EQ(k_ra8_err_hw_error, mdl_state_load(storage, "/fault/.mdl_state", &s_loaded));
  TEST_ASSERT_EQ(0U, s_loaded.chapter_count);
  fault->flags = (uint32_t)k_mdl_state_fault_close;
  TEST_ASSERT_EQ(k_ra8_err_hw_error, mdl_state_load(storage, "/fault/.mdl_state", &s_loaded));
  TEST_ASSERT_EQ(0U, s_loaded.chapter_count);
  fault->flags = 0U;
}

/**
 * @brief Seed a dedicated remount-recovery generation.
 * @details Creates and publishes state retained across backend teardown.
 * @param[in,out] storage Initialized backend storage.
 * @pre @p storage is non-null.
 * @pre The remount vector path does not exist on entry.
 * @post A valid remount generation is published.
 * @post The fixture title and chapter identity are deterministic.
 * @note The owning backend is remounted by its outer vector.
 * @since v1.0.0
 */
RA8_INTERNAL static void internal_seed_remount(mdl_storage_t* storage)
{
  TEST_ASSERT_EQ(k_ra8_ok, fw_fs_mkdir(&storage->fs->names, "/remount"));
  internal_seed_state("remounted", 8.5);
  internal_save_ok(storage, "/remount/.mdl_state");
}

/**
 * @brief Remove all vector objects through the portable namespace facade.
 * @details Deletes both generations and then each vector directory.
 * @param[in,out] storage Initialized backend storage.
 * @pre @p storage is non-null.
 * @pre No vector file is held open.
 * @post All known state generations are absent.
 * @post All known vector directories are absent.
 * @note Cleanup itself is qualified through portable namespace operations.
 * @since v1.0.0
 */
RA8_INTERNAL static void internal_cleanup_vectors(mdl_storage_t* storage)
{
  static const char* const directories[] =
    {"/series", "/legacy", "/bad", "/overflow", "/fault", "/remount"};
  for (size_t i = 0U; i < (sizeof(directories) / sizeof(directories[0])); ++i) {
    char base[64];
    char alternate[72];
    (void)snprintf(base, sizeof(base), "%s/.mdl_state", directories[i]);
    (void)snprintf(alternate, sizeof(alternate), "%s.alt", base);
    internal_remove_file_if_present(storage, base);
    internal_remove_file_if_present(storage, alternate);
    TEST_ASSERT_EQ(k_ra8_ok, fw_fs_rmdir(&storage->fs->names, directories[i]));
  }
}

/**
 * @brief Run all identical vectors against one live real backend.
 * @details Binds the fault facade and executes clean, migration, and fault cases.
 * @param[in] label Unity test label.
 * @param[in] inner Live filesystem backend.
 * @param[in,out] fault Fault facade workspace.
 * @param[out] storage Downloader storage workspace.
 * @pre All pointer parameters are non-null.
 * @pre @p inner is mounted and writable.
 * @post Every common state vector has passed.
 * @post A remount fixture remains for the backend-specific phase.
 * @note Backend-specific setup and teardown remain outside this function.
 * @since v1.0.0
 */
RA8_INTERNAL static void internal_run_backend_vectors(const char*           label,
                                                      const fw_fs_t*        inner,
                                                      mdl_state_fault_fs_t* fault,
                                                      mdl_storage_t*        storage)
{
  TEST_BEGIN(label);
  internal_bind_storage(inner, fault, storage);
  internal_run_clean_vectors(storage);
  internal_run_migration_vectors(storage);
  internal_run_sequence_vector(storage);
  internal_run_fault_vectors(fault, storage);
  internal_run_stream_fault_vectors(fault, storage);
  internal_seed_remount(storage);
  TEST_END(label);
}

/**
 * @brief Qualify and remount the secure POSIX adapter.
 * @details Runs common vectors inside a temporary descriptor-relative root.
 * @pre The POSIX adapter is available on the host.
 * @pre The process may create a temporary directory under /tmp.
 * @post Remount recovery and cleanup have passed.
 * @post The adapter and temporary directory are removed.
 * @note This is the host reference for identical backend behavior.
 * @since v1.0.0
 */
RA8_INTERNAL static void internal_test_posix_state(void)
{
  char root[] = "/tmp/mdl_state_port_XXXXXX";
  TEST_ASSERT_NOT_NULL(mkdtemp(root));
  fw_fs_t                 fs    = {};
  fw_fs_posix_state_t     posix = {.root_fd = -1};
  const fw_fs_posix_cfg_t cfg   = {.root_path = root, .removable_media = false};
  TEST_ASSERT_EQ(k_ra8_ok, fw_fs_posix_init(&fs, &posix, &cfg));
  mdl_state_fault_fs_t fault   = {};
  mdl_storage_t        storage = {};
  internal_run_backend_vectors("media state POSIX vectors", &fs, &fault, &storage);
  TEST_ASSERT_EQ(k_ra8_ok, fw_fs_posix_deinit(&posix));
  TEST_ASSERT_EQ(k_ra8_ok, fw_fs_posix_init(&fs, &posix, &cfg));
  internal_bind_storage(&fs, &fault, &storage);
  internal_assert_loaded(&storage, "/remount/.mdl_state", "remounted", 8.5);
  internal_cleanup_vectors(&storage);
  TEST_ASSERT_EQ(k_ra8_ok, fw_fs_posix_deinit(&posix));
  TEST_ASSERT_EQ(0, rmdir(root));
}

/**
 * @brief Mount the real RAM block-device FAT volume through VFS.
 * @details Attaches the preformatted backend and initializes the filesystem port.
 * @param[out] fs Portable filesystem facade to initialize.
 * @param[out] state VFS adapter state to initialize.
 * @pre @p fs and @p state are non-null.
 * @pre The shared block backend is formatted and unmounted.
 * @post The volume is mounted under the ram name.
 * @post @p fs delegates to the mounted RA8 VFS instance.
 * @note Formatting occurs once in the outer backend vector.
 * @since v1.0.0
 */
RA8_INTERNAL static void internal_mount_vfs(fw_fs_t* fs, fw_fs_ra8_vfs_state_t* state)
{
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &s_mount));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_vfs_init());
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_vfs_mount("ram", s_mount));
  const fw_fs_ra8_vfs_cfg_t cfg = {.mount_name = "ram", .mount = s_mount, .removable_media = false};
  TEST_ASSERT_EQ(k_ra8_ok, fw_fs_ra8_vfs_init(fs, state, &cfg));
}

/**
 * @brief Qualify and remount the real RAM blockdev to FAT12 to VFS adapter.
 * @details Formats real library layers, runs common vectors, and remounts them.
 * @pre The shared RAM disk and backend objects are idle.
 * @pre No VFS volume is mounted under the ram name.
 * @post Remount recovery and cleanup have passed.
 * @post The VFS volume and filesystem mount are detached.
 * @note This executes the same state calls as the POSIX vector.
 * @since v1.0.0
 */
RA8_INTERNAL static void internal_test_vfs_state(void)
{
  TEST_ASSERT_EQ(
    k_ra8_ok,
    ra8_io_blockdev_ram_init(&s_blockdev, &s_ram_state, s_disk, k_state_disk_blocks, false));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_blockdev_as_fs_backend(&s_blockdev, &s_backend));
  const ra8_fs_format_opts_t format = {.type = k_ra8_fs_type_fat12, .label = "MDLSTATE"};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_format(&s_backend, &format));
  fw_fs_t               fs  = {};
  fw_fs_ra8_vfs_state_t vfs = {};
  internal_mount_vfs(&fs, &vfs);
  mdl_state_fault_fs_t fault   = {};
  mdl_storage_t        storage = {};
  internal_run_backend_vectors("media state RAM/FAT/VFS vectors", &fs, &fault, &storage);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_vfs_unmount("ram"));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(s_mount));
  s_mount = nullptr;
  internal_mount_vfs(&fs, &vfs);
  internal_bind_storage(&fs, &fault, &storage);
  internal_assert_loaded(&storage, "/remount/.mdl_state", "remounted", 8.5);
  internal_cleanup_vectors(&storage);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_vfs_unmount("ram"));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(s_mount));
  s_mount = nullptr;
}

int main(void)
{
  internal_test_posix_state();
  internal_test_vfs_state();
  return 0;
}
