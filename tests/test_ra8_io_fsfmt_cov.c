/**
 * @file test_ra8_io_fsfmt_cov.c
 * @brief Validation and capability-consistency vectors for filesystem formats.
 *
 * @details
 * Builds fixed fake backend operation tables to cover required callbacks,
 * mutually dependent capability flags, duplicate registration, capacity
 * limits, and lookup behavior. The vectors verify that inconsistent filesystem
 * implementations are rejected before they enter the global format registry.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>

#include "ra8_attributes.h"
#include "ra8_err.h"
#include "ra8_fs.h"
#include "ra8_io_fsfmt.h"
#include "ra8_log.h"
#include "unity_minimal.h"

/** @brief Provide the file-local cov probe test helper. @details Exercises the cov probe path with bounded caller-owned fixture state and verifies its documented result. @param[in] backend Backend descriptor or operation table under test. @return Whether the named fixture condition holds. @retval true The named fixture condition holds. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since 0.1.0 */
RA8_INTERNAL static bool internal_cov_probe(const ra8_fs_backend_t* backend)
{
  return backend->ctx != nullptr;
}

/** @brief Provide the file-local cov mount test helper. @details Exercises the cov mount path with bounded caller-owned fixture state and verifies its documented result. @param[in] backend Backend descriptor or operation table under test. @param[out] out_mount Receives the opened mount context. @return RA8 status from the exercised fixture operation. @retval k_ra8_ok The fixture operation completed successfully. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since 0.1.0 */
RA8_INTERNAL static ra8_err_t internal_cov_mount(const ra8_fs_backend_t* backend, void** out_mount)
{
  *out_mount = backend->ctx;
  return k_ra8_ok;
}

/** @brief Provide the file-local cov unmount test helper. @details Exercises the cov unmount path with bounded caller-owned fixture state and verifies its documented result. @param[in,out] mount_ctx Injected filesystem-mount context retained by the fixture. @return RA8 status from the exercised fixture operation. @retval k_ra8_ok The fixture operation completed successfully. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since 0.1.0 */
RA8_INTERNAL static ra8_err_t internal_cov_unmount(void* mount_ctx)
{
  (void)mount_ctx;
  return k_ra8_ok;
}

/** @brief Provide the file-local cov open test helper. @details Exercises the cov open path with bounded caller-owned fixture state and verifies its documented result. @param[in,out] mount_ctx Injected filesystem-mount context retained by the fixture. @param[in] path Normalized path supplied to the fake filesystem backend. @param[in] mode Open or seek mode exercised by the fixture. @param[out] out_file Receives the opened file context. @return RA8 status from the exercised fixture operation. @retval k_ra8_ok The fixture operation completed successfully. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since 0.1.0 */
RA8_INTERNAL static ra8_err_t
internal_cov_open(void* mount_ctx, const char* path, ra8_fs_mode_t mode, void** out_file)
{
  (void)path;
  (void)mode;
  *out_file = mount_ctx;
  return k_ra8_ok;
}

/** @brief Provide the file-local cov close test helper. @details Exercises the cov close path with bounded caller-owned fixture state and verifies its documented result. @param[in,out] file_ctx Injected open-file context retained by the fixture. @return RA8 status from the exercised fixture operation. @retval k_ra8_ok The fixture operation completed successfully. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since 0.1.0 */
RA8_INTERNAL static ra8_err_t internal_cov_close(void* file_ctx)
{
  (void)file_ctx;
  return k_ra8_ok;
}

/** @brief Provide the file-local cov read test helper. @details Exercises the cov read path with bounded caller-owned fixture state and verifies its documented result. @param[in,out] file_ctx Injected open-file context retained by the fixture. @param[out] buf Caller-owned byte buffer read or written by the backend. @param[in] bytes Readable byte sequence length or storage size. @param[out] out_read Receives the number of bytes actually read. @return RA8 status from the exercised fixture operation. @retval k_ra8_ok The fixture operation completed successfully. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since 0.1.0 */
RA8_INTERNAL static ra8_err_t
internal_cov_read(void* file_ctx, void* buf, uint32_t bytes, uint32_t* out_read)
{
  (void)file_ctx;
  (void)buf;
  (void)bytes;
  *out_read = 0U;
  return k_ra8_ok;
}

/** @brief Provide the file-local cov write test helper. @details Exercises the cov write path with bounded caller-owned fixture state and verifies its documented result. @param[in,out] file_ctx Injected open-file context retained by the fixture. @param[in] buf Caller-owned byte buffer read or written by the backend. @param[in] bytes Readable byte sequence length or storage size. @return RA8 status from the exercised fixture operation. @retval k_ra8_ok The fixture operation completed successfully. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since 0.1.0 */
RA8_INTERNAL static ra8_err_t internal_cov_write(void* file_ctx, const void* buf, uint32_t bytes)
{
  (void)file_ctx;
  (void)buf;
  (void)bytes;
  return k_ra8_ok;
}

/** @brief Provide the file-local cov seek test helper. @details Exercises the cov seek path with bounded caller-owned fixture state and verifies its documented result. @param[in,out] file_ctx Injected open-file context retained by the fixture. @param[in] offset Stream-relative offset under test. @return RA8 status from the exercised fixture operation. @retval k_ra8_ok The fixture operation completed successfully. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since 0.1.0 */
RA8_INTERNAL static ra8_err_t internal_cov_seek(void* file_ctx, uint64_t offset)
{
  (void)file_ctx;
  (void)offset;
  return k_ra8_ok;
}

/** @brief Provide the file-local cov tell test helper. @details Exercises the cov tell path with bounded caller-owned fixture state and verifies its documented result. @param[in] file_ctx Injected open-file context retained by the fixture. @param[out] out_offset Receives the resulting stream offset. @return RA8 status from the exercised fixture operation. @retval k_ra8_ok The fixture operation completed successfully. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since 0.1.0 */
RA8_INTERNAL static ra8_err_t internal_cov_tell(const void* file_ctx, uint64_t* out_offset)
{
  (void)file_ctx;
  *out_offset = 0U;
  return k_ra8_ok;
}

/** @brief Provide the file-local cov size test helper. @details Exercises the cov size path with bounded caller-owned fixture state and verifies its documented result. @param[in] file_ctx Injected open-file context retained by the fixture. @param[out] out_size Receives the resulting object size in bytes. @return RA8 status from the exercised fixture operation. @retval k_ra8_ok The fixture operation completed successfully. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since 0.1.0 */
RA8_INTERNAL static ra8_err_t internal_cov_size(const void* file_ctx, uint64_t* out_size)
{
  (void)file_ctx;
  *out_size = 0U;
  return k_ra8_ok;
}

/** @brief Provide the file-local cov sync test helper. @details Exercises the cov sync path with bounded caller-owned fixture state and verifies its documented result. @param[in,out] file_ctx Injected open-file context retained by the fixture. @return RA8 status from the exercised fixture operation. @retval k_ra8_ok The fixture operation completed successfully. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since 0.1.0 */
RA8_INTERNAL static ra8_err_t internal_cov_sync(void* file_ctx)
{
  (void)file_ctx;
  return k_ra8_ok;
}

/** @brief Provide the file-local cov stat test helper. @details Exercises the cov stat path with bounded caller-owned fixture state and verifies its documented result. @param[in,out] mount_ctx Injected filesystem-mount context retained by the fixture. @param[in] path Normalized path supplied to the fake filesystem backend. @param[out] out Caller-owned result object populated by the callback. @return RA8 status from the exercised fixture operation. @retval k_ra8_ok The fixture operation completed successfully. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since 0.1.0 */
RA8_INTERNAL static ra8_err_t
internal_cov_stat(void* mount_ctx, const char* path, ra8_fs_stat_t* out)
{
  (void)mount_ctx;
  (void)path;
  *out = (ra8_fs_stat_t){};
  return k_ra8_ok;
}

/** @brief Provide the file-local cov listdir test helper. @details Exercises the cov listdir path with bounded caller-owned fixture state and verifies its documented result. @param[in,out] mount_ctx Injected filesystem-mount context retained by the fixture. @param[in] path Normalized path supplied to the fake filesystem backend. @param[in] cb Callback invoked by the enumeration fixture. @param[in,out] cb_ctx Injected callback context passed through unchanged. @return RA8 status from the exercised fixture operation. @retval k_ra8_ok The fixture operation completed successfully. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since 0.1.0 */
RA8_INTERNAL static ra8_err_t
internal_cov_listdir(void* mount_ctx, const char* path, ra8_fs_listdir_cb_t cb, void* cb_ctx)
{
  (void)mount_ctx;
  (void)path;
  (void)cb;
  (void)cb_ctx;
  return k_ra8_ok;
}

/** @brief Provide the file-local cov dir open test helper. @details Exercises the cov dir open path with bounded caller-owned fixture state and verifies its documented result. @param[in,out] mount_ctx Injected filesystem-mount context retained by the fixture. @param[in] path Normalized path supplied to the fake filesystem backend. @param[in,out] directory_state Caller-owned directory cursor state. @param[in] state_bytes Capacity of @p directory_state in bytes. @return RA8 status from the exercised fixture operation. @retval k_ra8_ok The fixture operation completed successfully. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since 0.1.0 */
RA8_INTERNAL static ra8_err_t internal_cov_dir_open(void*       mount_ctx,
                                                    const char* path,
                                                    void*       directory_state,
                                                    uint32_t    state_bytes)
{
  (void)mount_ctx;
  (void)path;
  (void)directory_state;
  (void)state_bytes;
  return k_ra8_ok;
}

/** @brief Provide the file-local cov dir next test helper. @details Exercises the cov dir next path with bounded caller-owned fixture state and verifies its documented result. @param[in,out] directory_state Caller-owned directory cursor state. @param[out] out Caller-owned result object populated by the callback. @param[out] out_entry Receives the next directory entry. @return RA8 status from the exercised fixture operation. @retval k_ra8_ok The fixture operation completed successfully. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since 0.1.0 */
RA8_INTERNAL static ra8_err_t
internal_cov_dir_next(void* directory_state, ra8_fs_dirent_t* out, bool* out_entry)
{
  (void)directory_state;
  *out       = (ra8_fs_dirent_t){};
  *out_entry = false;
  return k_ra8_ok;
}

/** @brief Provide the file-local cov dir close test helper. @details Exercises the cov dir close path with bounded caller-owned fixture state and verifies its documented result. @param[in,out] directory_state Caller-owned directory cursor state. @return RA8 status from the exercised fixture operation. @retval k_ra8_ok The fixture operation completed successfully. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since 0.1.0 */
RA8_INTERNAL static ra8_err_t internal_cov_dir_close(void* directory_state)
{
  (void)directory_state;
  return k_ra8_ok;
}

/** @brief Provide the file-local cov path test helper. @details Exercises the cov path path with bounded caller-owned fixture state and verifies its documented result. @param[in,out] mount_ctx Injected filesystem-mount context retained by the fixture. @param[in] path Normalized path supplied to the fake filesystem backend. @return RA8 status from the exercised fixture operation. @retval k_ra8_ok The fixture operation completed successfully. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since 0.1.0 */
RA8_INTERNAL static ra8_err_t internal_cov_path(void* mount_ctx, const char* path)
{
  (void)mount_ctx;
  (void)path;
  return k_ra8_ok;
}

/** @brief Provide the file-local cov rename test helper. @details Exercises the cov rename path with bounded caller-owned fixture state and verifies its documented result. @param[in,out] mount_ctx Injected filesystem-mount context retained by the fixture. @param[in] old_path Existing normalized path supplied to rename. @param[in] new_path Destination normalized path supplied to rename. @return RA8 status from the exercised fixture operation. @retval k_ra8_ok The fixture operation completed successfully. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since 0.1.0 */
RA8_INTERNAL static ra8_err_t
internal_cov_rename(void* mount_ctx, const char* old_path, const char* new_path)
{
  (void)mount_ctx;
  (void)old_path;
  (void)new_path;
  return k_ra8_ok;
}

/** @brief Provide the file-local cov space test helper. @details Exercises the cov space path with bounded caller-owned fixture state and verifies its documented result. @param[in,out] mount_ctx Injected filesystem-mount context retained by the fixture. @param[out] out Caller-owned result object populated by the callback. @return RA8 status from the exercised fixture operation. @retval k_ra8_ok The fixture operation completed successfully. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since 0.1.0 */
RA8_INTERNAL static ra8_err_t internal_cov_space(void* mount_ctx, ra8_fs_space_t* out)
{
  (void)mount_ctx;
  *out = (ra8_fs_space_t){};
  return k_ra8_ok;
}

static const ra8_io_fsfmt_ops_t s_valid_ops = {
  .probe      = internal_cov_probe,
  .mount      = internal_cov_mount,
  .unmount    = internal_cov_unmount,
  .open       = internal_cov_open,
  .close      = internal_cov_close,
  .read       = internal_cov_read,
  .write      = internal_cov_write,
  .seek       = internal_cov_seek,
  .tell       = internal_cov_tell,
  .size       = internal_cov_size,
  .sync       = internal_cov_sync,
  .stat       = internal_cov_stat,
  .listdir    = internal_cov_listdir,
  .dir_open   = internal_cov_dir_open,
  .dir_next   = internal_cov_dir_next,
  .dir_close  = internal_cov_dir_close,
  .unlink     = internal_cov_path,
  .rename     = internal_cov_rename,
  .mkdir      = internal_cov_path,
  .rmdir      = internal_cov_path,
  .free_space = internal_cov_space,
};

static const ra8_io_fsfmt_t s_valid_format = {
  .name = "cov",
  .caps = {},
  .ops  = &s_valid_ops,
};

/**
 * @brief Every mandatory descriptor member is rejected independently.
 *
 * @par MC/DC:
 * `ra8_io_fsfmt_register` has no compound decision: every required-pointer
 * guard is a separate N=1 decision. Builtin registration supplies each
 * `member == nullptr -> false` vector; this test nulls the descriptor, name,
 * ops table, and each required operation in turn to supply the corresponding
 * `true` vectors. @details Executes the required ops scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since 0.1.0 */
RA8_INTERNAL static void internal_test_required_ops(void)
{
  TEST_BEGIN("fsfmt required ops validation");
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_fsfmt_init());
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_io_fsfmt_register(nullptr));
  ra8_io_fsfmt_t fmt = s_valid_format;
  fmt.name           = nullptr;
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_io_fsfmt_register(&fmt));
  fmt     = s_valid_format;
  fmt.ops = nullptr;
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_io_fsfmt_register(&fmt));

  ra8_io_fsfmt_ops_t ops = s_valid_ops;
  /** @brief Assert that clearing one mandatory operation rejects the descriptor. */
#define EXPECT_REQUIRED_NULL(member_)                                                              \
  do {                                                                                             \
    ops         = s_valid_ops;                                                                     \
    ops.member_ = nullptr;                                                                         \
    fmt         = s_valid_format;                                                                  \
    fmt.ops     = &ops;                                                                            \
    TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_io_fsfmt_register(&fmt));                               \
  } while (false)
  EXPECT_REQUIRED_NULL(probe);
  EXPECT_REQUIRED_NULL(mount);
  EXPECT_REQUIRED_NULL(unmount);
  EXPECT_REQUIRED_NULL(open);
  EXPECT_REQUIRED_NULL(close);
  EXPECT_REQUIRED_NULL(read);
  EXPECT_REQUIRED_NULL(seek);
  EXPECT_REQUIRED_NULL(tell);
  EXPECT_REQUIRED_NULL(size);
  EXPECT_REQUIRED_NULL(stat);
  EXPECT_REQUIRED_NULL(listdir);
#undef EXPECT_REQUIRED_NULL
  TEST_END("fsfmt required ops validation");
}

/** @brief Prove every incremental-cursor capability invariant independently. @details Exercises the cursor capability consistency path with bounded caller-owned fixture state and verifies its documented result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since 0.1.0 */
RA8_INTERNAL static void internal_cursor_capability_consistency(void)
{
  ra8_io_fsfmt_t fmt                 = s_valid_format;
  fmt.caps.supports_dir_cursor       = true;
  fmt.caps.directory_workspace_bytes = 1U;
  fmt.caps.max_open_directories      = 1U;
  fmt.caps.directory_workspace_align = 1U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_fsfmt_init());
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_fsfmt_register(&fmt));
  ra8_io_fsfmt_ops_t ops = s_valid_ops;
/** @brief Reject one missing cursor operation with otherwise coherent facts. */
#define EXPECT_CURSOR_OP_MISMATCH(member_)                                                         \
  do {                                                                                             \
    TEST_ASSERT_EQ(k_ra8_ok, ra8_io_fsfmt_init());                                                 \
    ops         = s_valid_ops;                                                                     \
    ops.member_ = nullptr;                                                                         \
    fmt.ops     = &ops;                                                                            \
    TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_io_fsfmt_register(&fmt));                            \
  } while (false)
  EXPECT_CURSOR_OP_MISMATCH(dir_open);
  EXPECT_CURSOR_OP_MISMATCH(dir_next);
  EXPECT_CURSOR_OP_MISMATCH(dir_close);
#undef EXPECT_CURSOR_OP_MISMATCH
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_fsfmt_init());
  ops                                = s_valid_ops;
  fmt.ops                            = &ops;
  fmt.caps.directory_workspace_bytes = 0U;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_io_fsfmt_register(&fmt));
  fmt.caps.directory_workspace_bytes = 1U;
  fmt.caps.max_open_directories      = 0U;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_io_fsfmt_register(&fmt));
  fmt.caps.max_open_directories      = 1U;
  fmt.caps.directory_workspace_align = 0U;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_io_fsfmt_register(&fmt));
  fmt.caps.directory_workspace_align = 3U;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_io_fsfmt_register(&fmt));
}

/**
 * @brief Every capability-to-operation invariant is checked independently.
 *
 * @par MC/DC:
 * Capability validation uses nested single-condition decisions, not `&&` or
 * `||`. The valid descriptor supplies `capability=false`; each mismatch sets
 * only that capability true and its operation null, exercising the advertised
 * and missing-operation branches. Builtins provide the true/non-null vectors
 * for their supported operations; durable-sync true with sync false exercises
 * its incoherent branch. @details Executes the capability consistency scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since 0.1.0 */
RA8_INTERNAL static void internal_test_capability_consistency(void)
{
  TEST_BEGIN("fsfmt capability consistency");
  ra8_io_fsfmt_t     fmt = s_valid_format;
  ra8_io_fsfmt_ops_t ops = s_valid_ops;
  /** @brief Assert that an advertised capability requires its operation. */
#define EXPECT_CAP_MISMATCH(cap_, member_)                                                         \
  do {                                                                                             \
    TEST_ASSERT_EQ(k_ra8_ok, ra8_io_fsfmt_init());                                                 \
    ops           = s_valid_ops;                                                                   \
    ops.member_   = nullptr;                                                                       \
    fmt           = s_valid_format;                                                                \
    fmt.caps.cap_ = true;                                                                          \
    fmt.ops       = &ops;                                                                          \
    TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_io_fsfmt_register(&fmt));                            \
  } while (false)
  EXPECT_CAP_MISMATCH(supports_streaming_write, write);
  EXPECT_CAP_MISMATCH(supports_mkdir, mkdir);
  EXPECT_CAP_MISMATCH(supports_rmdir, rmdir);
  EXPECT_CAP_MISMATCH(supports_free_space, free_space);
  EXPECT_CAP_MISMATCH(supports_sync, sync);
  EXPECT_CAP_MISMATCH(atomic_rename, rename);
#undef EXPECT_CAP_MISMATCH

  internal_cursor_capability_consistency();

  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_fsfmt_init());
  fmt                   = s_valid_format;
  fmt.caps.durable_sync = true;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_io_fsfmt_register(&fmt));
  TEST_END("fsfmt capability consistency");
}

/**
 * @brief Registry, builtin lookup, probe-success and probe-miss branches.
 *
 * @par MC/DC:
 * These registry paths contain only N=1 decisions. Inserts below capacity make
 * `s_count >= max` false and the overflow insert makes it true; builtin lookup
 * drives each type comparison both ways; probe calls with an empty backend make
 * the probe predicate false and the marker-backed call makes it true. @details Executes the registry boundaries scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since 0.1.0 */
RA8_INTERNAL static void internal_test_registry_boundaries(void)
{
  TEST_BEGIN("fsfmt registry boundaries");
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_fsfmt_init());
  for (uint32_t i = 2U; i < (uint32_t)k_ra8_io_fsfmt_max; ++i) {
    TEST_ASSERT_EQ(k_ra8_ok, ra8_io_fsfmt_register(&s_valid_format));
  }
  TEST_ASSERT_EQ(k_ra8_err_no_mem, ra8_io_fsfmt_register(&s_valid_format));

  const ra8_io_fsfmt_t* out = nullptr;
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_io_fsfmt_get_builtin(k_ra8_fs_type_fat16, nullptr));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_fsfmt_get_builtin(k_ra8_fs_type_fat12, &out));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_fsfmt_get_builtin(k_ra8_fs_type_fat16, &out));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_fsfmt_get_builtin(k_ra8_fs_type_fat32, &out));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_fsfmt_get_builtin(k_ra8_fs_type_exfat, &out));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_io_fsfmt_get_builtin(k_ra8_fs_type_unknown, &out));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_fsfmt_init());
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_fsfmt_register(&s_valid_format));
  ra8_fs_backend_t backend = {};
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_io_fsfmt_probe(nullptr, &out));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_io_fsfmt_probe(&backend, nullptr));
  TEST_ASSERT_EQ(k_ra8_err_not_found, ra8_io_fsfmt_probe(&backend, &out));
  uint8_t marker = 1U;
  backend.ctx    = &marker;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_fsfmt_probe(&backend, &out));
  TEST_ASSERT(out == &s_valid_format);
  TEST_END("fsfmt registry boundaries");
}

/**
 * @brief Discard expected validation log bytes in the hosted test.
 * @param[in] ctx Unused sink context.
 * @param[in] byte Unused emitted byte.
 * @pre Installed before any expected registration error executes.
 * @post No state is modified and no host MMIO is accessed.
 * @note Single-threaded hosted test sink.
 * @since 0.1.0 @details Exercises the log sink path with bounded caller-owned fixture state and verifies its documented result. @pre Fixed-capacity fixture storage required by this operation is available. @post Documented outputs contain the exercised result when the operation succeeds. */
RA8_INTERNAL static void internal_log_sink(void* ctx, uint8_t byte)
{
  (void)ctx;
  (void)byte;
}

int32_t main(void)
{
  ra8_log_set_byte_sink(internal_log_sink, nullptr);
  internal_test_required_ops();
  internal_test_capability_consistency();
  internal_test_registry_boundaries();
  return 0;
}
