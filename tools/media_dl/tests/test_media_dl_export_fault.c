/**
 * @file test_media_dl_export_fault.c
 * @brief Portable exporter transaction, fault, and partial-publication tests.
 * @details Exercises exporter publication and rollback through injected
 *          storage failures while preserving destination-state assertions.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "fw_if_fs_backend.h"
#include "mdl_export.h"
#include "mdl_export_io_internal.h"
#include "mdl_test_storage.h"
#include "mdl_verify.h"
#include "ra8_attributes.h"
#include "support/mdl_state_fs_fault.h"
#include "tiny_jpeg_fixture.h"
#include "unity_minimal.h"

/** @brief Fixed capacities for fault qualification fixtures. */
typedef enum : uint32_t {
  k_fault_export_arena_bytes = 16U * 1024U * 1024U, /**< Export/verify arena. */
  k_fault_storage_bytes      = 8192U,               /**< Backend workspace.   */
  k_fault_path_bytes         = 512U,                /**< Test path storage.   */
} mdl_export_fault_limit_t;

/** @brief Maximally aligned portable backend workspace. */
typedef struct {
  alignas(max_align_t) uint8_t bytes[k_fault_storage_bytes]; /**< Backend-private state. */
} internal_fault_workspace_t;

/** @brief Transaction-only deterministic fail-on-Nth-begin wrapper. */
typedef struct {
  const fw_fs_t* inner;           /**< Wrapped complete filesystem. */
  fw_fs_t        fs;              /**< Published wrapper binding.   */
  uint32_t       fail_begin_call; /**< One-based failing call.      */
  uint32_t       begin_calls;     /**< Observed begin calls.        */
  uint32_t       commit_calls;    /**< Successful-path commits.     */
  uint32_t       abort_calls;     /**< Explicit stage aborts.       */
} internal_nth_transaction_t;

static uint8_t                    s_fault_arena[k_fault_export_arena_bytes];
static uint8_t                    s_second_arena[k_fault_export_arena_bytes];
static internal_fault_workspace_t s_fault_file_workspace;
static internal_fault_workspace_t s_fault_transaction_workspace;
static internal_fault_workspace_t s_nth_file_workspace;
static internal_fault_workspace_t s_nth_transaction_workspace;
static uint8_t                    s_fault_io[k_mdl_storage_io_bytes];
static uint8_t                    s_nth_io[k_mdl_storage_io_bytes];
static mdl_state_fault_fs_t       s_fault_fs;
static mdl_storage_t              s_fault_storage;
static mdl_storage_t              s_nth_storage;

/**
 * @brief Write one complete test byte range through the host composition edge
 * @details Creates a deterministic host fixture in one call and asserts every
 *          byte and close operation rather than returning partial setup state.
 * @param[in] path Host fixture path.
 * @param[in] bytes Fixture bytes.
 * @param[in] length Fixture extent.
 * @pre Inputs are valid and the parent directory exists.
 * @pre @p bytes remains readable for @p length bytes.
 * @post Normal return leaves one complete file or asserts.
 * @post No host stream remains open.
 * @note Test-only host helper.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_write_bytes(const char* path, const uint8_t* bytes, size_t length)
{
  const int descriptor = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
  TEST_ASSERT(descriptor >= 0);
  size_t offset = 0U;
  while (offset < length) {
    const ssize_t written = write(descriptor, &bytes[offset], length - offset);
    if (written > 0) {
      offset += (size_t)written;
    } else if ((written < 0) && (errno == EINTR)) {
      continue;
    } else {
      break;
    }
  }
  TEST_ASSERT(offset == length);
  TEST_ASSERT(close(descriptor) == 0);
}

/**
 * @brief Assert one fixture file has exact expected bytes
 * @details Reads the bounded fixture, proves exact EOF immediately afterward,
 *          closes it, and compares the complete expected span.
 * @param[in] path Host fixture path.
 * @param[in] bytes Expected bytes.
 * @param[in] length Expected extent.
 * @pre Inputs are valid and the file exists.
 * @pre @p length fits the fixed assertion buffer.
 * @post Normal return proves size and content equality.
 * @post No host stream remains open.
 * @note Test-only host helper.
 * @since 0.1.0
 */
RA8_INTERNAL static void
internal_assert_bytes(const char* path, const uint8_t* bytes, size_t length)
{
  uint8_t actual[32] = {};
  TEST_ASSERT(length <= sizeof(actual));
  const int descriptor = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
  TEST_ASSERT(descriptor >= 0);
  size_t offset = 0U;
  while (offset < length) {
    const ssize_t got = read(descriptor, &actual[offset], length - offset);
    if (got > 0) {
      offset += (size_t)got;
    } else if ((got < 0) && (errno == EINTR)) {
      continue;
    } else {
      break;
    }
  }
  uint8_t tail = 0U;
  TEST_ASSERT(read(descriptor, &tail, 1U) == 0);
  TEST_ASSERT(close(descriptor) == 0);
  TEST_ASSERT(offset == length);
  TEST_ASSERT(memcmp(actual, bytes, length) == 0);
}

/**
 * @brief Assert no private POSIX transaction stage remains in a directory
 * @details Enumerates composition-edge entries and rejects every reserved
 *          transaction-stage name before closing the directory cursor.
 * @param[in] path Host directory path.
 * @pre @p path names a readable directory.
 * @pre No concurrent fixture cleanup mutates the directory.
 * @post Normal return proves no `TXxxxxxx.TMP` entry remains.
 * @post The host directory cursor is closed.
 * @note Test-only host helper.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_assert_no_stage(const char* path)
{
  internal_fault_workspace_t directory_workspace;
  fw_fs_dir_t                directory = {};
  TEST_ASSERT(fw_fs_dir_open(&mdl_test_storage_get()->fs->names,
                             path,
                             &directory,
                             directory_workspace.bytes,
                             sizeof(directory_workspace.bytes)) == k_ra8_ok);
  bool                 present = false;
  fw_fs_dirent_value_t entry   = {};
  TEST_ASSERT(fw_fs_dir_next(&directory, &entry, &present) == k_ra8_ok);
  while (present) {
    const size_t length = entry.name_bytes;
    const bool   stage  = (length == 12U) && (entry.name[0] == 'T') && (entry.name[1] == 'X') &&
                          (strcmp(&entry.name[length - 4U], ".TMP") == 0);
    TEST_ASSERT(!stage);
    TEST_ASSERT(fw_fs_dir_next(&directory, &entry, &present) == k_ra8_ok);
  }
  TEST_ASSERT(fw_fs_dir_close(&directory) == k_ra8_ok);
}

/**
 * @brief Export one chapter through a selected storage binding and arena
 * @details Initializes a fresh workspace over the exact caller bytes and
 *          delegates once to the public bounded exporter contract.
 * @param[in,out] storage Portable binding.
 * @param[in] format Export format.
 * @param[in] directory Chapter directory.
 * @param[in] output Output path or JOF directory.
 * @param[out] arena Caller-owned export arena.
 * @param[in] arena_bytes Arena capacity.
 * @return Export status.
 * @retval k_ra8_ok The selected export completed.
 * @retval k_ra8_err_invalid_size The caller arena or another bound was exceeded.
 * @pre Inputs remain stable and @p storage is exclusively owned.
 * @pre @p arena is writable for @p arena_bytes bytes.
 * @post No arena ownership is transferred.
 * @post The public exporter status is returned unchanged.
 * @note Test-only adapter over the public exporter contract.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_export(mdl_storage_t*   storage,
                                              ra8_mdl_format_t format,
                                              const char*      directory,
                                              const char*      output,
                                              uint8_t*         arena,
                                              size_t           arena_bytes)
{
  mdl_export_workspace_t workspace;
  mdl_export_workspace_init(&workspace, arena, arena_bytes);
  return mdl_export_chapter_ws(storage, format, directory, output, &workspace);
}

/**
 * @brief Delegate a transaction begin except at one configured call
 * @details Counts begin attempts, returns the deterministic injected error on
 *          the configured one-based call, and delegates every other call exactly.
 * @param[in,out] ctx Wrapper controller.
 * @param[out] state Caller transaction workspace.
 * @param[in] state_bytes Workspace extent.
 * @param[in] destination Canonical destination.
 * @param[in] policy Requested publication policy.
 * @return Injected or wrapped begin status.
 * @retval k_ra8_err_hw_error This is the configured failing call.
 * @retval k_ra8_ok The wrapped transaction opened a stage.
 * @pre All inputs satisfy the wrapped transaction contract.
 * @pre The wrapper and delegated filesystem remain live.
 * @post The configured call fails before creating a stage.
 * @post Every other call preserves the delegated result and state.
 * @note Test-only deterministic fault seam.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_nth_begin(void*                      ctx,
                                                 void*                      state,
                                                 uint32_t                   state_bytes,
                                                 const char*                destination,
                                                 fw_fs_transaction_policy_t policy)
{
  internal_nth_transaction_t* nth = (internal_nth_transaction_t*)ctx;
  ++nth->begin_calls;
  if (nth->begin_calls == nth->fail_begin_call) {
    return k_ra8_err_hw_error;
  }
  return nth->inner->transactions.iface->begin(nth->inner->transactions.ctx,
                                               state,
                                               state_bytes,
                                               destination,
                                               policy);
}

/**
 * @brief Delegate transaction writes for the Nth-begin wrapper.
 * @details Forwards the original state, bytes, count, and progress pointer
 *          unchanged to the wrapped transaction port.
 * @param[in,out] ctx Live Nth-begin wrapper.
 * @param[in,out] state Wrapped transaction state.
 * @param[in] source Readable bytes.
 * @param[in] length Requested byte count.
 * @param[out] out_written Delegated progress count.
 * @return Wrapped write status.
 * @retval k_ra8_ok The wrapped transaction accepted bounded progress.
 * @retval k_ra8_fail The wrapped transaction rejected the write.
 * @pre Pointers satisfy the wrapped write contract.
 * @pre The wrapped transaction is active.
 * @post The delegated status and progress are returned unchanged.
 * @post The wrapper's begin/commit/abort counters remain unchanged.
 * @note Test-only transparent delegation seam.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_nth_write(void*          ctx,
                                                 void*          state,
                                                 const uint8_t* source,
                                                 uint32_t       length,
                                                 uint32_t*      out_written)
{
  internal_nth_transaction_t* nth = (internal_nth_transaction_t*)ctx;
  return nth->inner->transactions.iface->write(nth->inner->transactions.ctx,
                                               state,
                                               source,
                                               length,
                                               out_written);
}

/**
 * @brief Delegate transaction seeks for the Nth-begin wrapper.
 * @details Forwards the requested absolute stage offset without modifying the
 *          wrapper's configured failure or counters.
 * @param[in,out] ctx Live Nth-begin wrapper.
 * @param[in,out] state Wrapped transaction state.
 * @param[in] offset Absolute staged-file offset.
 * @return Wrapped seek status.
 * @retval k_ra8_ok The wrapped stage cursor moved.
 * @retval k_ra8_fail The wrapped transaction rejected the seek.
 * @pre The wrapper and transaction state are valid.
 * @pre @p offset satisfies the wrapped transaction's no-hole contract.
 * @post The delegated status is returned unchanged.
 * @post Wrapper counters remain unchanged.
 * @note Test-only transparent delegation seam.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_nth_seek(void* ctx, void* state, uint64_t offset)
{
  internal_nth_transaction_t* nth = (internal_nth_transaction_t*)ctx;
  return nth->inner->transactions.iface->seek(nth->inner->transactions.ctx, state, offset);
}

/**
 * @brief Delegate staged validation for the Nth-begin wrapper.
 * @details Preserves the borrowed-open-file validator callback and opaque
 *          context exactly while forwarding to the wrapped transaction.
 * @param[in,out] ctx Live Nth-begin wrapper.
 * @param[in,out] state Wrapped transaction state.
 * @param[in] validator Canonical borrowed-stage verifier.
 * @param[in,out] validator_ctx Caller-owned verifier context.
 * @return Wrapped validation status.
 * @retval k_ra8_ok The wrapped validator accepted the stage.
 * @retval k_ra8_err_validation_failed The stage was rejected.
 * @pre The wrapped transaction is active and seekable.
 * @pre @p validator and @p validator_ctx satisfy the wrapped contract.
 * @post The transaction retains ownership of the staged file.
 * @post Wrapper counters remain unchanged.
 * @note Test-only transparent delegation seam.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t
internal_nth_validate(void* ctx, void* state, fw_fs_validate_fn_t validator, void* validator_ctx)
{
  internal_nth_transaction_t* nth = (internal_nth_transaction_t*)ctx;
  return nth->inner->transactions.iface->validate(nth->inner->transactions.ctx,
                                                  state,
                                                  validator,
                                                  validator_ctx);
}

/**
 * @brief Delegate commit and record the attempted publication.
 * @details Increments the observed commit count exactly once before forwarding
 *          the wrapped commit and publication-truth output.
 * @param[in,out] ctx Live Nth-begin wrapper.
 * @param[in,out] state Wrapped transaction state.
 * @param[out] out_published Wrapped publication truth.
 * @return Wrapped commit status.
 * @retval k_ra8_ok The wrapped transaction published successfully.
 * @retval k_ra8_fail The wrapped commit reported an error.
 * @pre The wrapper and active transaction state are valid.
 * @pre @p out_published is writable.
 * @post The commit counter advances exactly once.
 * @post Status and publication truth are returned unchanged.
 * @note Test-only observation seam.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_nth_commit(void* ctx, void* state, bool* out_published)
{
  internal_nth_transaction_t* nth = (internal_nth_transaction_t*)ctx;
  ++nth->commit_calls;
  return nth->inner->transactions.iface->commit(nth->inner->transactions.ctx, state, out_published);
}

/**
 * @brief Delegate abort and record stage cleanup.
 * @details Increments the observed abort count exactly once before forwarding
 *          cleanup to the wrapped transaction.
 * @param[in,out] ctx Live Nth-begin wrapper.
 * @param[in,out] state Wrapped transaction state.
 * @return Wrapped abort status.
 * @retval k_ra8_ok The wrapped stage was removed.
 * @retval k_ra8_fail Wrapped cleanup failed.
 * @pre The wrapper and active transaction state are valid.
 * @pre No callback continues borrowing the staged file.
 * @post The abort counter advances exactly once.
 * @post The wrapped cleanup status is returned unchanged.
 * @note Test-only observation seam.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_nth_abort(void* ctx, void* state)
{
  internal_nth_transaction_t* nth = (internal_nth_transaction_t*)ctx;
  ++nth->abort_calls;
  return nth->inner->transactions.iface->abort(nth->inner->transactions.ctx, state);
}

static const fw_fs_transaction_iface_t s_nth_transactions = {.begin    = internal_nth_begin,
                                                             .write    = internal_nth_write,
                                                             .seek     = internal_nth_seek,
                                                             .validate = internal_nth_validate,
                                                             .commit   = internal_nth_commit,
                                                             .abort    = internal_nth_abort};

/**
 * @brief Bind the transaction-only Nth-begin wrapper
 * @details Copies the complete filesystem binding, then replaces only the
 *          transaction interface and context with the deterministic wrapper.
 * @param[out] nth Wrapper state.
 * @param[in] inner Complete filesystem to delegate.
 * @param[in] fail_call One-based begin call to reject.
 * @pre Pointers are non-null and @p inner remains live.
 * @pre @p nth is writable and does not alias @p inner.
 * @post All non-transaction ports retain the original binding exactly.
 * @post Counters start at zero and the requested failing call is retained.
 * @note Test-only and allocation-free.
 * @since 0.1.0
 */
RA8_INTERNAL static void
internal_nth_bind(internal_nth_transaction_t* nth, const fw_fs_t* inner, uint32_t fail_call)
{
  *nth = (internal_nth_transaction_t){.inner = inner, .fail_begin_call = fail_call, .fs = *inner};
  nth->fs.transactions.iface = &s_nth_transactions;
  nth->fs.transactions.ctx   = nth;
}

/**
 * @brief Build one storage binding around a test filesystem
 * @details Supplies distinct aligned file, transaction, and stream regions to
 *          the production storage initializer and asserts any contract failure.
 * @param[out] storage Downloader storage object.
 * @param[in] fs Complete filesystem binding.
 * @param[out] file_workspace File workspace.
 * @param[out] transaction_workspace Transaction workspace.
 * @param[out] io Streaming scratch.
 * @pre All storage regions are distinct and correctly aligned.
 * @pre @p fs remains live for the binding's complete test lifetime.
 * @post Normal return leaves a usable exclusive binding.
 * @post No workspace ownership transfers from the caller.
 * @note Test helper; failures assert immediately.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_bind_storage(mdl_storage_t*              storage,
                                               const fw_fs_t*              fs,
                                               internal_fault_workspace_t* file_workspace,
                                               internal_fault_workspace_t* transaction_workspace,
                                               uint8_t*                    io)
{
  TEST_ASSERT_EQ(k_ra8_ok,
                 mdl_storage_init(storage,
                                  fs,
                                  file_workspace->bytes,
                                  sizeof(file_workspace->bytes),
                                  transaction_workspace->bytes,
                                  sizeof(transaction_workspace->bytes),
                                  io,
                                  k_mdl_storage_io_bytes));
}

/**
 * @brief Prove short I/O and independent exporter bindings are safe.
 * @details Drives all single-container formats through one-byte source and
 *          transaction progress, then holds two active transactions backed by
 *          distinct storage/workspace objects at the same time.
 * @pre The portable POSIX storage and fault wrapper are initialized.
 * @pre Both caller-owned exporter arenas are writable and independent.
 * @post All four container exports complete and their stages are removed.
 * @post Both simultaneous transactions abort cleanly without a leaked stage.
 * @note Test-only; host fixture cleanup is asserted by the final directory
 * scan.
 * @test Short reads/writes and two independent transaction bindings are safe.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_short_io_and_two_bindings(void)
{
  TEST_BEGIN("export short I/O and two bindings");
  const char* directory = "/tmp/mdl_export_short";
  (void)mkdir(directory, 0755);
  const uint8_t page[] = {'i', 'm', 'a', 'g', 'e'};
  internal_write_bytes("/tmp/mdl_export_short/001.jpg", page, sizeof(page));
  s_fault_fs.flags =
    (uint32_t)k_mdl_state_fault_short_read | (uint32_t)k_mdl_state_fault_short_write;
  const struct {
    ra8_mdl_format_t format; /**< Selected bounded container writer. */
    const char*      path;   /**< Destination fixture path.          */
  } formats[] = {{k_ra8_mdl_format_cbz, "/tmp/mdl_export_short/a.cbz"},
                 {k_ra8_mdl_format_cbt, "/tmp/mdl_export_short/a.cbt"},
                 {k_ra8_mdl_format_cbt_gz, "/tmp/mdl_export_short/a.cbt.gz"},
                 {k_ra8_mdl_format_epub, "/tmp/mdl_export_short/a.epub"}};
  for (size_t i = 0U; i < (sizeof(formats) / sizeof(formats[0])); ++i) {
    TEST_ASSERT_EQ(k_ra8_ok,
                   internal_export(&s_fault_storage,
                                   formats[i].format,
                                   directory,
                                   formats[i].path,
                                   s_fault_arena,
                                   sizeof(s_fault_arena)));
    (void)unlink(formats[i].path);
  }
  s_fault_fs.flags           = 0U;
  mdl_export_output_t first  = {};
  mdl_export_output_t second = {};
  TEST_ASSERT_EQ(k_ra8_ok,
                 priv_mdl_export_output_begin(&first,
                                              &s_fault_storage,
                                              "/tmp/mdl_export_short/one.cbz",
                                              k_ra8_mdl_format_cbz));
  TEST_ASSERT_EQ(k_ra8_ok,
                 priv_mdl_export_output_begin(&second,
                                              &s_nth_storage,
                                              "/tmp/mdl_export_short/two.cbz",
                                              k_ra8_mdl_format_cbz));
  TEST_ASSERT_EQ(k_ra8_ok, priv_mdl_export_output_abort(&first));
  TEST_ASSERT_EQ(k_ra8_ok, priv_mdl_export_output_abort(&second));
  internal_assert_no_stage(directory);
  (void)unlink("/tmp/mdl_export_short/001.jpg");
  (void)rmdir(directory);
  TEST_END("export short I/O and two bindings");
}

/**
 * @brief Prove every transaction fault preserves the promised publication
 * state.
 * @details Injects begin, write, validation, pre-commit, seek, close,
 * directory, stat, and abort faults before checking the prior destination and
 *          absence of leaked stages; the post-publication fault is checked
 * separately.
 * @pre The fault filesystem wraps a live portable POSIX filesystem.
 * @pre A sentinel destination and readable source page exist.
 * @post Every pre-publication fault preserves the sentinel byte-for-byte.
 * @post A post-publication error reports failure while exposing a valid
 * archive.
 * @note Test-only; each vector resets the injected fault mask explicitly.
 * @test Every publication phase preserves an existing artifact on failure.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_transaction_fault_preservation(void)
{
  TEST_BEGIN("export transaction fault preservation");
  const char*   directory = "/tmp/mdl_export_fault";
  const char*   output    = "/tmp/mdl_export_fault/book.cbz";
  const uint8_t page[]    = {'p', 'a', 'g', 'e'};
  const uint8_t old[]     = {'o', 'l', 'd'};
  (void)mkdir(directory, 0755);
  internal_write_bytes("/tmp/mdl_export_fault/001.jpg", page, sizeof(page));
  const uint32_t faults[] = {(uint32_t)k_mdl_state_fault_begin,
                             (uint32_t)k_mdl_state_fault_transaction_write,
                             (uint32_t)k_mdl_state_fault_validate,
                             (uint32_t)k_mdl_state_fault_commit_before,
                             (uint32_t)k_mdl_state_fault_stream_seek,
                             (uint32_t)k_mdl_state_fault_close,
                             (uint32_t)k_mdl_state_fault_dir_open,
                             (uint32_t)k_mdl_state_fault_dir_next,
                             (uint32_t)k_mdl_state_fault_stat,
                             (uint32_t)k_mdl_state_fault_transaction_write |
                               (uint32_t)k_mdl_state_fault_abort};
  for (size_t i = 0U; i < (sizeof(faults) / sizeof(faults[0])); ++i) {
    internal_write_bytes(output, old, sizeof(old));
    s_fault_fs.flags = faults[i];
    TEST_ASSERT(internal_export(&s_fault_storage,
                                k_ra8_mdl_format_cbz,
                                directory,
                                output,
                                s_fault_arena,
                                sizeof(s_fault_arena)) != k_ra8_ok);
    internal_assert_bytes(output, old, sizeof(old));
    internal_assert_no_stage(directory);
  }
  s_fault_fs.flags = (uint32_t)k_mdl_state_fault_commit_after;
  TEST_ASSERT(internal_export(&s_fault_storage,
                              k_ra8_mdl_format_cbz,
                              directory,
                              output,
                              s_fault_arena,
                              sizeof(s_fault_arena)) == k_ra8_err_hw_timeout);
  s_fault_fs.flags              = 0U;
  mdl_verify_report_t    report = {};
  mdl_export_workspace_t workspace;
  mdl_export_workspace_init(&workspace, s_fault_arena, sizeof(s_fault_arena));
  TEST_ASSERT_EQ(
    k_ra8_ok,
    mdl_verify_file(mdl_test_storage_get(), k_ra8_mdl_format_cbz, output, &workspace, &report));
  internal_assert_no_stage(directory);
  (void)unlink("/tmp/mdl_export_fault/001.jpg");
  (void)unlink(output);
  (void)rmdir(directory);
  TEST_END("export transaction fault preservation");
}

/**
 * @brief Prove a random-offset ZIP backfill seek fault cannot publish.
 * @details Opens a real exporter transaction, performs a sequential write, then
 *          injects the seek failure used by the miniz random-access callback.
 * @pre The fault storage is initialized and the sentinel destination exists.
 * @pre The active transaction has accepted an initial bounded write.
 * @post The callback reports zero progress and the explicit abort succeeds.
 * @post The original destination is byte-identical and no stage remains.
 * @note Test-only; this isolates the seek seam from archive construction.
 * @test Transaction backfill seek failures remain explicit and unpublished.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_backfill_seek_fault(void)
{
  TEST_BEGIN("export backfill seek fault");
  const char*   directory = "/tmp/mdl_export_seek";
  const char*   output    = "/tmp/mdl_export_seek/book.cbz";
  const uint8_t old[]     = {'o', 'l', 'd'};
  const uint8_t data[]    = {'a', 'b', 'c', 'd'};
  (void)mkdir(directory, 0755);
  internal_write_bytes(output, old, sizeof(old));
  mdl_export_output_t stage = {};
  TEST_ASSERT_EQ(
    k_ra8_ok,
    priv_mdl_export_output_begin(&stage, &s_fault_storage, output, k_ra8_mdl_format_cbz));
  TEST_ASSERT_EQ(k_ra8_ok, priv_mdl_export_output_write(&stage, data, sizeof(data)));
  s_fault_fs.flags = (uint32_t)k_mdl_state_fault_seek;
  TEST_ASSERT(priv_mdl_export_zip_write(&stage, 0U, data, sizeof(data)) == 0U);
  TEST_ASSERT_EQ(k_ra8_ok, priv_mdl_export_output_abort(&stage));
  s_fault_fs.flags = 0U;
  internal_assert_bytes(output, old, sizeof(old));
  internal_assert_no_stage(directory);
  (void)unlink(output);
  (void)rmdir(directory);
  TEST_END("export backfill seek fault");
}

/**
 * @brief Prove symlink inputs and destinations are rejected without mutation.
 * @details Exercises both a page symlink and a destination symlink through the
 *          normal chapter exporter rather than bypassing the storage contract.
 * @pre The composition-edge fixture directory and symlink targets exist.
 * @pre The fault storage is initialized without an active injected fault.
 * @post Both exports fail with invalid-argument status before publication.
 * @post The real destination remains byte-identical and no stage is leaked.
 * @note Test-only; direct POSIX calls create and remove the composition
 * fixtures.
 * @test Symlink/nonregular inputs and outputs are rejected without mutation.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_symlink_rejection(void)
{
  TEST_BEGIN("export symlink rejection");
  const char*   directory = "/tmp/mdl_export_link";
  const uint8_t page[]    = {'p'};
  const uint8_t old[]     = {'o'};
  (void)mkdir(directory, 0755);
  internal_write_bytes("/tmp/mdl_export_link/real.jpg", page, sizeof(page));
  internal_write_bytes("/tmp/mdl_export_link/out.cbz", old, sizeof(old));
  TEST_ASSERT(symlink("real.jpg", "/tmp/mdl_export_link/link.jpg") == 0);
  TEST_ASSERT(internal_export(&s_fault_storage,
                              k_ra8_mdl_format_cbz,
                              directory,
                              "/tmp/mdl_export_link/out.cbz",
                              s_fault_arena,
                              sizeof(s_fault_arena)) == k_ra8_err_invalid_arg);
  internal_assert_bytes("/tmp/mdl_export_link/out.cbz", old, sizeof(old));
  (void)unlink("/tmp/mdl_export_link/link.jpg");
  TEST_ASSERT(symlink("out.cbz", "/tmp/mdl_export_link/output-link.cbz") == 0);
  TEST_ASSERT(internal_export(&s_fault_storage,
                              k_ra8_mdl_format_cbz,
                              directory,
                              "/tmp/mdl_export_link/output-link.cbz",
                              s_fault_arena,
                              sizeof(s_fault_arena)) == k_ra8_err_invalid_arg);
  internal_assert_bytes("/tmp/mdl_export_link/out.cbz", old, sizeof(old));
  internal_assert_no_stage(directory);
  (void)unlink("/tmp/mdl_export_link/output-link.cbz");
  (void)unlink("/tmp/mdl_export_link/real.jpg");
  (void)unlink("/tmp/mdl_export_link/out.cbz");
  (void)rmdir(directory);
  TEST_END("export symlink rejection");
}

/**
 * @brief Prove JOF page-wise publication reports an exact page-two failure.
 * @details A fail-on-second-begin transaction wrapper allows page one to be
 *          produced, independently verified, and committed before page two
 * fails.
 * @pre Three valid source pages and sentinel page-two/page-three outputs exist.
 * @pre The wrapper delegates every operation except its configured second
 * begin.
 * @post Page one is verifier-valid and exactly one commit is recorded.
 * @post Page two and page three remain byte-identical and no stage is leaked.
 * @note Test-only; this documents the deliberate non-atomic multi-page policy.
 * @test A JOF fault at page two reports partial publication exactly.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_jof_page_two_partial_failure(void)
{
  TEST_BEGIN("JOF page-two partial publication");
  const char*   directory    = "/tmp/mdl_export_jof_fault";
  const uint8_t page_two[]   = {'t', 'w', 'o'};
  const uint8_t page_three[] = {'t', 'h', 'r', 'e', 'e'};
  (void)mkdir(directory, 0755);
  internal_write_bytes("/tmp/mdl_export_jof_fault/001.jpg", s_tiny_jpeg, s_tiny_jpeg_len);
  internal_write_bytes("/tmp/mdl_export_jof_fault/002.jpg", s_tiny_jpeg, s_tiny_jpeg_len);
  internal_write_bytes("/tmp/mdl_export_jof_fault/003.jpg", s_tiny_jpeg, s_tiny_jpeg_len);
  internal_write_bytes("/tmp/mdl_export_jof_fault/002.jof", page_two, sizeof(page_two));
  internal_write_bytes("/tmp/mdl_export_jof_fault/003.jof", page_three, sizeof(page_three));
  internal_nth_transaction_t nth;
  internal_nth_bind(&nth, mdl_test_storage_get()->fs, 2U);
  internal_bind_storage(&s_nth_storage,
                        &nth.fs,
                        &s_nth_file_workspace,
                        &s_nth_transaction_workspace,
                        s_nth_io);
  TEST_ASSERT_EQ(k_ra8_err_hw_error,
                 internal_export(&s_nth_storage,
                                 k_ra8_mdl_format_jof,
                                 directory,
                                 directory,
                                 s_second_arena,
                                 sizeof(s_second_arena)));
  TEST_ASSERT_EQ(2U, nth.begin_calls);
  TEST_ASSERT_EQ(1U, nth.commit_calls);
  TEST_ASSERT_EQ(0U, nth.abort_calls);
  mdl_verify_report_t    report = {};
  mdl_export_workspace_t workspace;
  mdl_export_workspace_init(&workspace, s_fault_arena, sizeof(s_fault_arena));
  TEST_ASSERT_EQ(k_ra8_ok,
                 mdl_verify_file(mdl_test_storage_get(),
                                 k_ra8_mdl_format_jof,
                                 "/tmp/mdl_export_jof_fault/001.jof",
                                 &workspace,
                                 &report));
  internal_assert_bytes("/tmp/mdl_export_jof_fault/002.jof", page_two, sizeof(page_two));
  internal_assert_bytes("/tmp/mdl_export_jof_fault/003.jof", page_three, sizeof(page_three));
  internal_assert_no_stage(directory);
  for (uint32_t i = 1U; i <= 3U; ++i) {
    char jpg[k_fault_path_bytes];
    char jof[k_fault_path_bytes];
    (void)__builtin_snprintf(jpg, sizeof(jpg), "%s/%03u.jpg", directory, i);
    (void)__builtin_snprintf(jof, sizeof(jof), "%s/%03u.jof", directory, i);
    (void)unlink(jpg);
    (void)unlink(jof);
  }
  (void)rmdir(directory);
  TEST_END("JOF page-two partial publication");
}

int main(void)
{
  TEST_ASSERT_EQ(k_ra8_ok, mdl_test_storage_init());
  TEST_ASSERT_EQ(k_ra8_ok, mdl_state_fault_fs_init(&s_fault_fs, mdl_test_storage_get()->fs));
  internal_bind_storage(&s_fault_storage,
                        &s_fault_fs.fs,
                        &s_fault_file_workspace,
                        &s_fault_transaction_workspace,
                        s_fault_io);
  internal_nth_transaction_t no_fault;
  internal_nth_bind(&no_fault, mdl_test_storage_get()->fs, UINT32_MAX);
  internal_bind_storage(&s_nth_storage,
                        &no_fault.fs,
                        &s_nth_file_workspace,
                        &s_nth_transaction_workspace,
                        s_nth_io);
  internal_test_short_io_and_two_bindings();
  internal_test_transaction_fault_preservation();
  internal_test_backfill_seek_fault();
  internal_test_symlink_rejection();
  internal_test_jof_page_two_partial_failure();
  TEST_ASSERT_EQ(k_ra8_ok, mdl_test_storage_deinit());
  return 0;
}
