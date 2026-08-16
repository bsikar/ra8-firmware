/**
 * @file test_ra8_mdl_storage_vfs.c
 * @brief Real FAT/VFS qualification for downloader transactional storage
 *
 * @details Exercises the production adapter over a RAM block device, real
 * `ra8_fs`, and named VFS routing. No mock filesystem or POSIX call sits in the
 * success path. A narrow backend wrapper injects media-removal, capacity, and
 * rename-write faults without changing the production filesystem.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>
#include <string.h>

#include "ra8_attributes.h"
#include "ra8_fs.h"
#include "ra8_io_blockdev.h"
#include "ra8_io_blockdev_ram.h"
#include "ra8_io_vfs.h"
#include "ra8_log.h"
#include "ra8_mdl_storage_vfs.h"
#include "unity_minimal.h"

/** @brief Fixture capacities and payload constants. */
typedef enum : uint32_t {
  k_disk_blocks        = 16384U, /**< 8 MiB FAT16 RAM disk.             */
  k_payload_len        = 300U,   /**< Crosses several transfer writes.  */
  k_compare_chunk_len  = 64U,    /**< Bounded validation read chunk.    */
  k_payload_multiplier = 29U,    /**< Deterministic payload multiplier. */
  k_payload_offset     = 7U,     /**< Deterministic payload offset.     */
  k_byte_mask          = 0xFFU,  /**< Low-byte payload mask.            */
  k_digest_tag         = 0xA5U,  /**< Test validator digest marker.     */
  k_written_sentinel   = 99U,    /**< Detect failure-path output reset. */
} test_limits_t;

/** @brief Backend fault controller wrapped around the real RAM bridge. */
typedef struct {
  ra8_fs_backend_t inner;       /**< Real blockdev-to-fs bridge.   */
  ra8_err_t        write_error; /**< Injected write failure or OK. */
  bool             removed;     /**< All access fails while true.  */
} fault_backend_t;

/** @brief Artifact-validator fixture. */
typedef struct {
  const uint8_t* expected;     /**< Expected staged bytes.    */
  uint32_t       expected_len; /**< Expected length.          */
  uint32_t       calls;        /**< Invocation count.         */
  bool           fail;         /**< Force validation failure. */
} validator_t;

static uint8_t                     s_disk[(size_t)k_disk_blocks * k_ra8_io_block_size_bytes];
static ra8_io_blockdev_ram_state_t s_ram_state;
static ra8_io_blockdev_t           s_blockdev;
static ra8_fs_backend_t            s_raw_backend;
static fault_backend_t             s_fault;
static ra8_fs_backend_t            s_backend;
static ra8_fs_mount_t*             s_mount;

/**
 * @brief Discard expected negative-path logs without host ITM MMIO.
 * @details Provides a host-safe sink for deliberately rejected storage operations.
 * @param[in] ctx Unused sink context.
 * @param[in] byte Emitted diagnostic byte to discard.
 * @pre Installed only by this single-threaded test. @pre Arguments may contain any values.
 * @post No fixture state changes. @post The byte is discarded.
 * @note This avoids fixed-address target logging. @since 0.1.0
 */
RA8_INTERNAL static void internal_discard_log_byte(void* ctx, uint8_t byte)
{
  (void)ctx;
  (void)byte;
}

/**
 * @brief Delegate a read unless media removal is injected.
 * @details Preserves the real RAM backend while injecting one deterministic availability fault.
 * @param[in,out] ctx Bound ::fault_backend_t.
 * @param[in] lba First block to read.
 * @param[in] count Number of blocks.
 * @param[out] buf Writable block extent.
 * @return Backend or injected status.
 * @retval k_ra8_err_hw_error Media is marked removed.
 * @retval other Status from the real backend.
 * @pre Context and buffer extents are valid. @pre The inner read callback is bound.
 * @post Success initializes the requested blocks. @post Injected removal does not touch @p buf.
 * @note Used only by this deterministic fixture. @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t
internal_fault_read(void* ctx, uint64_t lba, uint32_t count, uint8_t* buf)
{
  fault_backend_t* const fault = ctx;
  if (fault->removed) {
    return k_ra8_err_hw_error;
  }
  return fault->inner.read_block(fault->inner.ctx, lba, count, buf);
}

/**
 * @brief Delegate a write unless a capacity/removal fault is injected.
 * @details Selects removal, configured write failure, or the real block write in that order.
 * @param[in,out] ctx Bound ::fault_backend_t.
 * @param[in] lba First block to write.
 * @param[in] count Number of blocks.
 * @param[in] buf Readable block extent.
 * @return Backend or injected status.
 * @retval k_ra8_ok The real write completed.
 * @retval other Removal, configured, or backend failure.
 * @pre Context and input extents are valid. @pre The inner write callback is bound.
 * @post Success updates exactly the requested blocks. @post Injected failure bypasses the backend.
 * @note Drives write and rename-publication faults. @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t
internal_fault_write(void* ctx, uint64_t lba, uint32_t count, const uint8_t* buf)
{
  fault_backend_t* const fault = ctx;
  if (fault->removed) {
    return k_ra8_err_hw_error;
  }
  if (fault->write_error != k_ra8_ok) {
    return fault->write_error;
  }
  return fault->inner.write_block(fault->inner.ctx, lba, count, buf);
}

/**
 * @brief Delegate capacity unless media removal is injected.
 * @details Makes capacity discovery participate in the same removable-media fault model.
 * @param[in,out] ctx Bound ::fault_backend_t.
 * @param[out] blocks Available block count.
 * @param[out] block_size Logical block size.
 * @return Backend or injected status.
 * @retval k_ra8_err_hw_error Media is marked removed.
 * @retval other Status from the real backend.
 * @pre Required pointers are valid. @pre The inner capacity callback is bound.
 * @post Success initializes both outputs. @post Removal preserves both outputs.
 * @note No capacity value is fabricated. @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t
internal_fault_capacity(void* ctx, uint64_t* blocks, uint32_t* block_size)
{
  fault_backend_t* const fault = ctx;
  if (fault->removed) {
    return k_ra8_err_hw_error;
  }
  return fault->inner.get_capacity(fault->inner.ctx, blocks, block_size);
}

/**
 * @brief Delegate erase unless a capacity/removal fault is injected.
 * @details Applies the same write-fault policy to erase operations used by FAT updates.
 * @param[in,out] ctx Bound ::fault_backend_t.
 * @param[in] lba First block to erase.
 * @param[in] count Number of blocks.
 * @return Backend or injected status.
 * @retval k_ra8_ok The real erase completed.
 * @retval other Removal, configured, or backend failure.
 * @pre Context is valid. @pre The inner erase callback is bound.
 * @post Success erases the requested range. @post Injected failure bypasses the backend.
 * @note Shares the write-error control deliberately. @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_fault_erase(void* ctx, uint64_t lba, uint64_t count)
{
  fault_backend_t* const fault = ctx;
  if (fault->removed) {
    return k_ra8_err_hw_error;
  }
  if (fault->write_error != k_ra8_ok) {
    return fault->write_error;
  }
  return fault->inner.erase_blocks(fault->inner.ctx, lba, count);
}

/**
 * @brief Format and mount a fresh real FAT16 volume behind the fault wrapper.
 * @details Reinitializes RAM block storage, formats FAT, and registers the `ram` VFS mount.
 * @pre Shared fixture objects are not in concurrent use. @pre Any previous mount is valid or NULL.
 * @post `s_mount` names a fresh mounted filesystem. @post All fault controls are clear.
 * @note Assertions terminate setup on any layer failure. @since 0.1.0
 */
RA8_INTERNAL static void internal_setup_volume(void)
{
  if (s_mount != nullptr) {
    TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(s_mount));
    s_mount = nullptr;
  }
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_vfs_init());
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_io_blockdev_ram_init(&s_blockdev, &s_ram_state, s_disk, k_disk_blocks, false));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_blockdev_as_fs_backend(&s_blockdev, &s_raw_backend));
  ra8_fs_format_opts_t opts = {};
  opts.type                 = k_ra8_fs_type_fat16;
  opts.label                = "MDLVFS";
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_format(&s_raw_backend, &opts));
  s_fault   = (fault_backend_t){.inner = s_raw_backend};
  s_backend = (ra8_fs_backend_t){.read_block   = internal_fault_read,
                                 .write_block  = internal_fault_write,
                                 .get_capacity = internal_fault_capacity,
                                 .erase_blocks = internal_fault_erase,
                                 .ctx          = &s_fault};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &s_mount));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_vfs_mount("ram", s_mount));
}

/**
 * @brief Read and compare a complete VFS file.
 * @details Streams bounded chunks and preserves the first read, length, content, or close error.
 * @param[in] path Canonical VFS path.
 * @param[in] expected Expected bytes.
 * @param[in] expected_len Exact expected length.
 * @return Comparison or storage status.
 * @retval k_ra8_ok All bytes and close matched.
 * @retval other Open, read, length, content, or close failure.
 * @pre Inputs are valid for their extents. @pre The mounted backend contains the fixture.
 * @post Every expected byte was examined on success. @post The file is closed before return.
 * @note Reads use a fixed 64-byte stack buffer. @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t
internal_compare_file(const char* path, const uint8_t* expected, uint32_t expected_len)
{
  ra8_fs_file_t* file = nullptr;
  ra8_err_t      err  = ra8_io_vfs_open(path, k_ra8_fs_mode_read, &file);
  if (err != k_ra8_ok) {
    return err;
  }
  uint8_t  buf[k_compare_chunk_len] = {};
  uint32_t offset                   = 0U;
  while (offset < expected_len) {
    uint32_t want = expected_len - offset;
    if (want > sizeof(buf)) {
      want = sizeof(buf);
    }
    uint32_t got = 0U;
    err          = ra8_fs_read(file, buf, want, &got);
    if (err != k_ra8_ok) {
      break;
    }
    if (got != want) {
      err = k_ra8_err_invalid_size;
      break;
    }
    if (memcmp(buf, &expected[offset], want) != 0) {
      err = k_ra8_err_validation_failed;
      break;
    }
    offset += got;
  }
  const ra8_err_t close_err = ra8_fs_close(file);
  return (err == k_ra8_ok) ? close_err : err;
}

/**
 * @brief Validator that reads the closed private object through the real VFS.
 * @details Checks configured failure, exact length, digest tag, and staged bytes in order.
 * @param[in,out] ctx Bound ::validator_t.
 * @param[in] staging_path Closed private VFS path.
 * @param[in] total_bytes Observed staged length.
 * @param[in] sha256 Transfer digest.
 * @return Validation status.
 * @retval k_ra8_ok Every configured invariant matched.
 * @retval other Forced, size, digest, or byte-comparison failure.
 * @pre Required pointers and digest extent are valid. @pre Staging path remains readable.
 * @post Invocation count is incremented. @post No namespace object is modified.
 * @note The digest marker isolates callback plumbing from SHA implementation. @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t
internal_validate_artifact(void*         ctx,
                           const char*   staging_path,
                           uint64_t      total_bytes,
                           const uint8_t sha256[k_ra8_mdl_sha256_bytes])
{
  validator_t* const validator = ctx;
  validator->calls++;
  if (validator->fail) {
    return k_ra8_err_validation_failed;
  }
  if (total_bytes != validator->expected_len) {
    return k_ra8_err_invalid_size;
  }
  if (sha256[0] != k_digest_tag) {
    return k_ra8_err_checksum_mismatch;
  }
  return internal_compare_file(staging_path, validator->expected, validator->expected_len);
}

/**
 * @brief Bind an adapter with one test-reserved staging leaf.
 * @details Configures optional staged-file validation over the fixed private leaf.
 * @param[out] storage Adapter state.
 * @param[out] iface Published storage interface.
 * @param[in,out] validator Optional validator fixture.
 * @pre Required outputs are valid. @pre The VFS mount is active.
 * @post Both outputs are initialized. @post No transaction is active.
 * @note A NULL validator intentionally disables validation. @since 0.1.0
 */
RA8_INTERNAL static void internal_bind_adapter(ra8_mdl_storage_vfs_t*   storage,
                                               ra8_mdl_storage_iface_t* iface,
                                               validator_t*             validator)
{
  const ra8_mdl_storage_vfs_config_t config = {
    .stage_leaf   = "MDLSTAGE.TMP",
    .validate     = validator == nullptr ? nullptr : internal_validate_artifact,
    .validate_ctx = validator,
  };
  TEST_ASSERT_EQ(k_ra8_ok, ra8_mdl_storage_vfs_init(storage, &config, iface));
}

/**
 * @brief Produce a deterministic nontrivial payload.
 * @details Fills the complete transfer buffer with a reproducible nonconstant byte sequence.
 * @param[out] payload Writable fixed-size payload.
 * @pre The complete extent is writable. @pre No concurrent fixture writer exists.
 * @post Every payload byte is initialized. @post Repeated calls produce identical bytes.
 * @note Arithmetic is reduced to the low byte deliberately. @since 0.1.0
 */
RA8_INTERNAL static void internal_fill_payload(uint8_t payload[k_payload_len])
{
  for (uint32_t i = 0U; i < k_payload_len; ++i) {
    payload[i] = (uint8_t)(((i * k_payload_multiplier) + k_payload_offset) & k_byte_mask);
  }
}

/**
 * @brief Stat helper asserting path absence.
 * @details Requires the portable stat itself to succeed while reporting no node.
 * @param[in] path Canonical VFS path.
 * @pre @p path is non-NULL. @pre The VFS mount is active.
 * @post Stat completed successfully. @post The path is absent.
 * @note Assertion failure terminates the test. @since 0.1.0
 */
RA8_INTERNAL static void internal_assert_absent(const char* path)
{
  ra8_io_vfs_stat_t stat = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_vfs_stat(path, &stat));
  TEST_ASSERT(!stat.exists);
}

/**
 * @brief Initialisation rejects bad staging ownership without modifying VFS.
 * @details Covers null bindings, invalid leaf forms, and unterminated names before any I/O.
 *
 * @par MC/DC:
 * For `storage == nullptr || config == nullptr || out_iface == nullptr`, valid
 * bindings elsewhere provide `(F,F,F) -> false`; this test provides `(T,-,-)`,
 * `(F,T,-)`, and `(F,F,T)`, each true. Holding the other conditions false
 * proves each pointer independently controls the decision; N=3, N+1=4.
 * @pre The VFS fixture is not required. @pre Local output objects are writable.
 * @post Every invalid configuration returns its exact status. @post No transaction begins.
 * @note This vector intentionally exercises only initialization policy. @since 0.1.0
 */
RA8_INTERNAL static void internal_test_init_guards(void)
{
  TEST_BEGIN("media VFS storage init guards");
  ra8_mdl_storage_vfs_t        storage = {};
  ra8_mdl_storage_iface_t      iface   = {};
  ra8_mdl_storage_vfs_config_t config  = {.stage_leaf = "MDLSTAGE.TMP"};
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_mdl_storage_vfs_init(nullptr, &config, &iface));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_mdl_storage_vfs_init(&storage, nullptr, &iface));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_mdl_storage_vfs_init(&storage, &config, nullptr));
  config.stage_leaf = nullptr;
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_mdl_storage_vfs_init(&storage, &config, &iface));
  config.stage_leaf = ".";
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_mdl_storage_vfs_init(&storage, &config, &iface));
  config.stage_leaf = "../BAD";
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_mdl_storage_vfs_init(&storage, &config, &iface));
  char unterminated[k_ra8_mdl_storage_vfs_stage_leaf_capacity];
  (void)memset(unterminated, 'A', sizeof(unterminated));
  config.stage_leaf = unterminated;
  TEST_ASSERT_EQ(k_ra8_err_invalid_size, ra8_mdl_storage_vfs_init(&storage, &config, &iface));
  TEST_END("media VFS storage init guards");
}

/**
 * @brief Real multi-chunk transaction is byte-identical and idempotent.
 * @details Writes a payload in two pieces, independently validates it, and publishes once.
 *
 * @par MC/DC:
 * At validation, `(close/stat err == k_ra8_ok) && (validate != nullptr)` is
 * `(T,T) -> true` here. `internal_test_stale_stage_recovery` supplies `(T,F) -> false`,
 * proving the callback-presence condition independently controls dispatch; the
 * earlier wrong-total call exits before this compound decision and is not
 * claimed as an MC/DC vector.
 * @pre Shared RAM/FAT/VFS fixtures are available. @pre The destination path is absent.
 * @post Final bytes equal the generated payload. @post Stage cleanup and repeated lifecycle calls succeed.
 * @note The callback observes the closed private stage before rename. @since 0.1.0
 */
RA8_INTERNAL static void internal_test_multichunk_commit(void)
{
  TEST_BEGIN("media VFS storage real multi-chunk commit");
  internal_setup_volume();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_vfs_mkdir("ram:/BOOKS"));
  uint8_t payload[k_payload_len];
  internal_fill_payload(payload);
  validator_t             validator = {.expected = payload, .expected_len = k_payload_len};
  ra8_mdl_storage_vfs_t   storage   = {};
  ra8_mdl_storage_iface_t iface     = {};
  internal_bind_adapter(&storage, &iface, &validator);
  TEST_ASSERT_EQ(k_ra8_ok, iface.begin(iface.ctx, "ram:/BOOKS/BOOK.RBK"));
  uint16_t written = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, iface.write(iface.ctx, payload, 73U, &written));
  TEST_ASSERT_EQ(73U, written);
  TEST_ASSERT_EQ(k_ra8_ok,
                 iface.write(iface.ctx, &payload[73], (uint16_t)(k_payload_len - 73U), &written));
  TEST_ASSERT_EQ(k_payload_len - 73U, written);
  uint8_t digest[k_ra8_mdl_sha256_bytes] = {[0] = k_digest_tag};
  TEST_ASSERT_EQ(k_ra8_err_invalid_size, iface.validate(iface.ctx, k_payload_len - 1U, digest));
  TEST_ASSERT_EQ(k_ra8_ok, iface.validate(iface.ctx, k_payload_len, digest));
  TEST_ASSERT_EQ(1U, validator.calls);
  TEST_ASSERT_EQ(k_ra8_ok, iface.commit(iface.ctx));
  TEST_ASSERT_EQ(k_ra8_ok, iface.commit(iface.ctx));
  TEST_ASSERT_EQ(k_ra8_ok, iface.abort(iface.ctx));
  TEST_ASSERT_EQ(k_ra8_ok, internal_compare_file("ram:/BOOKS/BOOK.RBK", payload, k_payload_len));
  internal_assert_absent("ram:/BOOKS/MDLSTAGE.TMP");
  TEST_END("media VFS storage real multi-chunk commit");
}

/**
 * @brief Failed validation publishes nothing and repeated abort is harmless.
 * @details Forces the borrowed-stage validator to reject a complete transaction before commit.
 *
 * @par MC/DC:
 * The validation-dispatch decision is `(err == k_ra8_ok, validate != nullptr)`
 * = `(T,T) -> true`; the callback then returns failure, making the following
 * N=1 `err == k_ra8_ok` state-publication decision false. The successful
 * callback in `internal_test_multichunk_commit` supplies its true vector.
 * @pre Shared storage fixtures are available. @pre The final and staging paths are absent.
 * @post Neither final nor stage is visible. @post Repeated abort remains successful.
 * @note Reported failure comes from the validator, not storage I/O. @since 0.1.0
 */
RA8_INTERNAL static void internal_test_validation_failure_abort(void)
{
  TEST_BEGIN("media VFS storage validation failure and abort");
  internal_setup_volume();
  const uint8_t payload[] = {1U, 2U, 3U, 4U};
  validator_t   validator = {.expected = payload, .expected_len = sizeof(payload), .fail = true};
  ra8_mdl_storage_vfs_t   storage = {};
  ra8_mdl_storage_iface_t iface   = {};
  internal_bind_adapter(&storage, &iface, &validator);
  TEST_ASSERT_EQ(k_ra8_ok, iface.begin(iface.ctx, "ram:/BAD.RBK"));
  uint16_t written = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, iface.write(iface.ctx, payload, sizeof(payload), &written));
  uint8_t digest[k_ra8_mdl_sha256_bytes] = {[0] = k_digest_tag};
  TEST_ASSERT_EQ(k_ra8_err_validation_failed, iface.validate(iface.ctx, sizeof(payload), digest));
  internal_assert_absent("ram:/BAD.RBK");
  TEST_ASSERT_EQ(k_ra8_ok, iface.abort(iface.ctx));
  TEST_ASSERT_EQ(k_ra8_ok, iface.abort(iface.ctx));
  internal_assert_absent("ram:/BAD.RBK");
  internal_assert_absent("ram:/MDLSTAGE.TMP");
  TEST_END("media VFS storage validation failure and abort");
}

/**
 * @brief Begin recovers its owned stale file but never removes a directory.
 * @details Leaves one validated private object, proves recovery, then substitutes a directory.
 *
 * @par MC/DC:
 * A successful stage check with no callback gives `(err == k_ra8_ok,
 * validate != nullptr) = (T,F) -> false`; paired with `(T,T) -> true` in
 * `internal_test_multichunk_commit`, callback presence independently controls dispatch.
 * The absent, stale-regular, and directory stages also drive both outcomes of
 * the separate N=1 existence and directory decisions.
 * @pre A fresh mounted volume can create the fixture directory. @pre No final object exists.
 * @post A stale regular stage is reclaimed. @post A same-name directory survives rejection.
 * @note Recovery is restricted to the adapter-owned leaf. @since 0.1.0
 */
RA8_INTERNAL static void internal_test_stale_stage_recovery(void)
{
  TEST_BEGIN("media VFS storage owned stale stage recovery");
  internal_setup_volume();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_vfs_mkdir("ram:/BOOKS"));
  ra8_mdl_storage_vfs_t   old_storage = {};
  ra8_mdl_storage_iface_t old_iface   = {};
  internal_bind_adapter(&old_storage, &old_iface, nullptr);
  TEST_ASSERT_EQ(k_ra8_ok, old_iface.begin(old_iface.ctx, "ram:/BOOKS/OLD.RBK"));
  const uint8_t stale[] = {9U, 8U, 7U};
  uint16_t      written = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, old_iface.write(old_iface.ctx, stale, sizeof(stale), &written));
  uint8_t digest[k_ra8_mdl_sha256_bytes] = {};
  TEST_ASSERT_EQ(k_ra8_ok, old_iface.validate(old_iface.ctx, sizeof(stale), digest));

  ra8_mdl_storage_vfs_t   new_storage = {};
  ra8_mdl_storage_iface_t new_iface   = {};
  internal_bind_adapter(&new_storage, &new_iface, nullptr);
  TEST_ASSERT_EQ(k_ra8_ok, new_iface.begin(new_iface.ctx, "ram:/BOOKS/NEW.RBK"));
  ra8_io_vfs_stat_t stat = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_vfs_stat(new_storage.staging_path, &stat));
  TEST_ASSERT(stat.exists);
  TEST_ASSERT_EQ(0U, stat.size_bytes);
  TEST_ASSERT_EQ(k_ra8_ok, new_iface.abort(new_iface.ctx));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_vfs_mkdir("ram:/BOOKS/MDLSTAGE.TMP"));
  ra8_mdl_storage_vfs_t   dir_storage = {};
  ra8_mdl_storage_iface_t dir_iface   = {};
  internal_bind_adapter(&dir_storage, &dir_iface, nullptr);
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, dir_iface.begin(dir_iface.ctx, "ram:/BOOKS/DIR.RBK"));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_vfs_stat("ram:/BOOKS/MDLSTAGE.TMP", &stat));
  TEST_ASSERT(stat.exists);
  TEST_ASSERT(stat.is_directory);
  TEST_END("media VFS storage owned stale stage recovery");
}

/**
 * @brief Canonical path checks reject files, dirs, traversal, and bad mounts.
 * @details Exercises existing destinations, missing parents, traversal, separators, and bounds.
 *
 * @par MC/DC:
 * For the component terminator decision `c == '/' || c == '\0'`, ordinary
 * component bytes give `(F,F) -> false`, the nested `PARENT/X.RBK` path gives
 * `(T,-) -> true`, and the final NUL gives `(F,T) -> true`. The two control
 * pairs prove independent influence; N=2, N+1=3.
 * @pre A fresh mounted volume is writable. @pre Sentinel objects are created before rejection.
 * @post Every unsafe path is rejected exactly. @post Existing bytes and directories remain intact.
 * @note No path is normalized implicitly by the adapter. @since 0.1.0
 */
RA8_INTERNAL static void internal_test_path_and_existing_final_guards(void)
{
  TEST_BEGIN("media VFS storage path and existing-final guards");
  internal_setup_volume();
  const uint8_t original[] = {0x31U, 0x32U, 0x33U};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_write_file(s_mount, "/KEEP.RBK", original, sizeof(original)));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_vfs_mkdir("ram:/DIR.RBK"));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_write_file(s_mount, "/PARENT", original, sizeof(original)));
  ra8_mdl_storage_vfs_t   storage = {};
  ra8_mdl_storage_iface_t iface   = {};
  internal_bind_adapter(&storage, &iface, nullptr);
  TEST_ASSERT_EQ(k_ra8_err_exists, iface.begin(iface.ctx, "ram:/KEEP.RBK"));
  TEST_ASSERT_EQ(k_ra8_ok, internal_compare_file("ram:/KEEP.RBK", original, sizeof(original)));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, iface.begin(iface.ctx, "ram:/DIR.RBK"));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, iface.begin(iface.ctx, "ram:/PARENT/X.RBK"));
  TEST_ASSERT_EQ(k_ra8_err_not_found, iface.begin(iface.ctx, "ram:/MISSING/X.RBK"));
  TEST_ASSERT_EQ(k_ra8_err_not_found, iface.begin(iface.ctx, "sd:/X.RBK"));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, iface.begin(iface.ctx, "ram:/A/../X.RBK"));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, iface.begin(iface.ctx, "ram:/A//X.RBK"));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, iface.begin(iface.ctx, "ram:/"));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, iface.begin(iface.ctx, "ram:X.RBK"));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, iface.begin(iface.ctx, "ram:/A\\X.RBK"));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, iface.begin(iface.ctx, "ra\\m:/X.RBK"));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, iface.begin(iface.ctx, "ram:/MDLSTAGE.TMP"));
  char too_long[k_ra8_mdl_storage_vfs_path_capacity + 1U];
  (void)memset(too_long, 'A', sizeof(too_long));
  too_long[0]                                   = 'r';
  too_long[1]                                   = 'a';
  too_long[2]                                   = 'm';
  too_long[3]                                   = ':';
  too_long[4]                                   = '/';
  too_long[k_ra8_mdl_storage_vfs_path_capacity] = '\0';
  TEST_ASSERT_EQ(k_ra8_err_invalid_size, iface.begin(iface.ctx, too_long));
  TEST_END("media VFS storage path and existing-final guards");
}

/**
 * @brief Capacity/removal faults publish nothing and cleanup remains retryable.
 * @details Injects begin, write, and abort failures and then proves deterministic recovery.
 *
 * @par MC/DC:
 * This fault toggle does not supply a compound-decision independence pair;
 * its deliberate vector pair targets the N=1
 * post-write `err == k_ra8_ok` decision, the injected no-memory error gives
 * false and preserves `written=0`; the retry write gives true and records the
 * bytes. Removed/present media likewise supplies both outcomes of the backend
 * fault guards.
 * @pre The mounted volume starts fresh. @pre Fault controls are initially clear.
 * @post Failed writes publish zero bytes. @post Clearing each fault permits complete cleanup.
 * @note Output-count reset is checked with a nonzero sentinel. @since 0.1.0
 */
RA8_INTERNAL static void internal_test_write_and_removal_faults(void)
{
  TEST_BEGIN("media VFS storage capacity and removal faults");
  internal_setup_volume();
  const uint8_t           payload[] = {0xA1U, 0xB2U, 0xC3U, 0xD4U};
  ra8_mdl_storage_vfs_t   storage   = {};
  ra8_mdl_storage_iface_t iface     = {};
  internal_bind_adapter(&storage, &iface, nullptr);

  s_fault.removed = true;
  TEST_ASSERT_EQ(k_ra8_err_hw_error, iface.begin(iface.ctx, "ram:/REMOVED.RBK"));
  s_fault.removed = false;
  TEST_ASSERT_EQ(k_ra8_ok, iface.begin(iface.ctx, "ram:/FULL.RBK"));
  s_fault.write_error = k_ra8_err_no_mem;
  uint16_t written    = k_written_sentinel;
  TEST_ASSERT_EQ(k_ra8_err_no_mem, iface.write(iface.ctx, payload, sizeof(payload), &written));
  TEST_ASSERT_EQ(0U, written);
  s_fault.write_error = k_ra8_ok;
  TEST_ASSERT_EQ(k_ra8_ok, iface.abort(iface.ctx));
  internal_assert_absent("ram:/FULL.RBK");

  TEST_ASSERT_EQ(k_ra8_ok, iface.begin(iface.ctx, "ram:/RETRY.RBK"));
  TEST_ASSERT_EQ(k_ra8_ok, iface.write(iface.ctx, payload, sizeof(payload), &written));
  s_fault.removed = true;
  TEST_ASSERT_EQ(k_ra8_err_hw_error, iface.abort(iface.ctx));
  s_fault.removed = false;
  TEST_ASSERT_EQ(k_ra8_ok, iface.abort(iface.ctx));
  TEST_ASSERT_EQ(k_ra8_ok, iface.abort(iface.ctx));
  internal_assert_absent("ram:/RETRY.RBK");
  TEST_END("media VFS storage capacity and removal faults");
}

/**
 * @brief Rename faults and destination races preserve no-replace semantics.
 * @details Fails publication once, retries it, then creates a competing destination before commit.
 *
 * @par MC/DC:
 * Commit's publish chain uses separate N=1 decisions. The injected rename
 * error makes the post-rename `err == k_ra8_ok` decision false; retrying with
 * writes enabled makes it true and publishes. The destination-race vector
 * fails the earlier final-absence decision and never reaches rename.
 * @pre The mounted volume starts fresh. @pre The adapter owns its private stage.
 * @post Retry publishes the intended bytes. @post A race winner is preserved exactly.
 * @note Commit truth follows whether rename already published. @since 0.1.0
 */
RA8_INTERNAL static void internal_test_rename_fault_and_no_replace(void)
{
  TEST_BEGIN("media VFS storage rename fault and no-replace");
  internal_setup_volume();
  const uint8_t           payload[]                      = {0xA1U, 0xB2U, 0xC3U, 0xD4U};
  uint8_t                 digest[k_ra8_mdl_sha256_bytes] = {};
  ra8_mdl_storage_vfs_t   storage                        = {};
  ra8_mdl_storage_iface_t iface                          = {};
  internal_bind_adapter(&storage, &iface, nullptr);
  uint16_t written = 0U;

  TEST_ASSERT_EQ(k_ra8_ok, iface.begin(iface.ctx, "ram:/RENAME.RBK"));
  TEST_ASSERT_EQ(k_ra8_ok, iface.write(iface.ctx, payload, sizeof(payload), &written));
  TEST_ASSERT_EQ(k_ra8_ok, iface.validate(iface.ctx, sizeof(payload), digest));
  s_fault.write_error = k_ra8_err_hw_error;
  TEST_ASSERT_EQ(k_ra8_err_hw_error, iface.commit(iface.ctx));
  s_fault.write_error = k_ra8_ok;
  internal_assert_absent("ram:/RENAME.RBK");
  TEST_ASSERT_EQ(k_ra8_ok, iface.commit(iface.ctx));
  TEST_ASSERT_EQ(k_ra8_ok, internal_compare_file("ram:/RENAME.RBK", payload, sizeof(payload)));

  TEST_ASSERT_EQ(k_ra8_ok, iface.begin(iface.ctx, "ram:/RACE.RBK"));
  TEST_ASSERT_EQ(k_ra8_ok, iface.write(iface.ctx, payload, sizeof(payload), &written));
  TEST_ASSERT_EQ(k_ra8_ok, iface.validate(iface.ctx, sizeof(payload), digest));
  const uint8_t winner[] = {0x55U, 0x66U};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_write_file(s_mount, "/RACE.RBK", winner, sizeof(winner)));
  TEST_ASSERT_EQ(k_ra8_err_exists, iface.commit(iface.ctx));
  TEST_ASSERT_EQ(k_ra8_ok, internal_compare_file("ram:/RACE.RBK", winner, sizeof(winner)));
  TEST_ASSERT_EQ(k_ra8_ok, iface.abort(iface.ctx));
  TEST_ASSERT_EQ(k_ra8_ok, internal_compare_file("ram:/RACE.RBK", winner, sizeof(winner)));
  TEST_END("media VFS storage rename fault and no-replace");
}

int main(void)
{
  ra8_log_set_byte_sink(internal_discard_log_byte, nullptr);
  internal_test_init_guards();
  internal_test_multichunk_commit();
  internal_test_validation_failure_abort();
  internal_test_stale_stage_recovery();
  internal_test_path_and_existing_final_guards();
  internal_test_write_and_removal_faults();
  internal_test_rename_fault_and_no_replace();
  return 0;
}
