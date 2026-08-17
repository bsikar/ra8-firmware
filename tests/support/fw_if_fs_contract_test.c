/**
 * @file fw_if_fs_contract_test.c
 * @brief Fault vectors for portable filesystem facade output contracts, plus
 *        the firmware VFS adapter's init/lifecycle guard vector.
 *
 * @details
 * Supplies synthetic namespace, stream, and transaction callbacks that return
 * structurally impossible success values. The shared vector binds only local
 * vtable copies, proving facade validation independently of any filesystem
 * backend and without mutating the truthful binding used as its baseline.
 * Also carries the firmware VFS adapter's argument and media-policy guard
 * vector, moved here from tests/test_fw_if_fs.c to keep both files under the
 * repository's per-file line cap; it takes its one native mount dependency as
 * a parameter rather than reaching a file-static in the caller.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "fw_if_fs_contract_test.h"

#include <stddef.h>
#include <stdint.h>

#include "fw_if_fs_backend.h"
#include "fw_if_fs_ra8_vfs.h"
#include "ra8_attributes.h"
#include "ra8_err.h"
#include "unity_minimal.h"

/** @brief Caller-owned results returned by configurable hostile callbacks. */
typedef struct {
  fw_fs_stat_t  stat;             /**< Namespace metadata result.      */
  fw_fs_space_t space;            /**< Volume accounting result.       */
  ra8_err_t     space_status;     /**< Volume-query callback status.   */
  ra8_err_t     commit_status;    /**< Transaction-commit status.      */
  bool          commit_published; /**< Transaction publication result. */
} contract_results_t;

/** @brief Return incoherent success metadata for facade fault injection.
 * @details Implements the bounded contract stat fixture step using caller-owned
 * state. @param[in,out] ctx Caller-owned fixture or filesystem state.
 * @param[in] path Validated fixture path or name value. @param[out] out
 * Caller-owned output populated on success. @return Status, selected object, or
 * bounded value produced by the named operation. @retval k_ra8_ok The requested
 * operation completed. @retval k_ra8_err_* Validation or backend work failed.
 * @pre Pointer arguments address their documented readable or writable extents.
 * @pre Required fixture and backend state is initialized before the call. @post
 * No access exceeds a caller-advertised capacity. @post The return value or
 * assertions describe the observed filesystem state. @note Test-only helpers
 * retain no hidden ownership beyond documented fixture state. @since 0.1.0 */
RA8_INTERNAL static ra8_err_t internal_contract_stat(void* ctx, const char* path, fw_fs_stat_t* out)
{
  (void)path;
  const contract_results_t* results = (const contract_results_t*)ctx;
  *out                              = results->stat;
  return k_ra8_ok;
}

/** @brief Report more directory entries than the caller permitted. @details
 * Implements the bounded contract list fixture step using caller-owned state.
 * @param[in,out] ctx Caller-owned fixture or filesystem state. @param[in] path
 * Validated portable path. @param[in] max_entries Maximum accepted entry count.
 * @param[in] callback Caller callback, not invoked by this fault.
 * @param[in,out] callback_ctx Caller callback context. @param[out] out_count
 * Receives the impossible count. @param[out] out_complete Receives completion
 * state. @return Synthetic backend status. @retval k_ra8_ok The impossible
 * output was supplied for facade validation. @pre All output pointers designate
 * writable objects. @pre The caller already validated the path and callback
 * arguments. @post @p out_count exceeds @p max_entries by one. @post @p
 * out_complete is true and no callback was invoked. @note Test-only helper with
 * no retained state. @since 0.1.0 */
RA8_INTERNAL static ra8_err_t internal_contract_list(void*           ctx,
                                                     const char*     path,
                                                     uint32_t        max_entries,
                                                     fw_fs_list_fn_t callback,
                                                     void*           callback_ctx,
                                                     uint32_t*       out_count,
                                                     bool*           out_complete)
{
  (void)ctx;
  (void)path;
  (void)callback;
  (void)callback_ctx;
  *out_count    = max_entries + 1U;
  *out_complete = true;
  return k_ra8_ok;
}

/**
 * @brief Valid callback shape for the bounded-list fault.
 * @details Accepts one entry so the facade can validate a hostile count.
 * @param[in,out] ctx Unused callback context.
 * @param[in] entry Unused delivered entry.
 * @param[out] out_continue Receives true.
 * @return Callback status.
 * @retval k_ra8_ok Delivery may continue.
 * @pre @p entry and @p out_continue are non-null.
 * @pre The caller owns @p out_continue.
 * @post @p out_continue is true.
 * @post No caller or backend state is retained.
 * @note Test-only helper.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t
internal_contract_entry(void* ctx, const fw_fs_dirent_t* entry, bool* out_continue)
{
  (void)ctx;
  (void)entry;
  *out_continue = true;
  return k_ra8_ok;
}

/** @brief Return impossible volume accounting for facade fault injection.
 * @details Produces free bytes greater than total bytes. @param[in,out] ctx
 * Unused backend context. @param[out] out Receives incoherent accounting.
 * @return Synthetic backend status. @retval k_ra8_ok The impossible output was
 * supplied. @pre @p out designates writable storage. @pre The facade
 * initialized @p out before dispatch. @post Free bytes exceed total bytes.
 * @post No backend state is changed. @note Test-only helper. @since 0.1.0 */
RA8_INTERNAL static ra8_err_t internal_contract_space(void* ctx, fw_fs_space_t* out)
{
  const contract_results_t* results = (const contract_results_t*)ctx;
  *out                              = results->space;
  return results->space_status;
}

/**
 * @brief Report one byte beyond the supplied read capacity.
 * @details Supplies an impossible successful transfer count.
 * @param[in,out] ctx Unused backend context.
 * @param[in,out] file_state Unused file state.
 * @param[out] dst Unused destination.
 * @param[in] cap Advertised destination capacity.
 * @param[out] out_read Receives @p cap plus one.
 * @return Synthetic backend status.
 * @retval k_ra8_ok The impossible output was supplied.
 * @pre @p out_read designates writable storage.
 * @pre @p cap is representable with one additional byte in this vector.
 * @post @p out_read exceeds @p cap by one.
 * @post @p dst and backend state are unchanged.
 * @note Test-only helper.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t
/* NOLINTNEXTLINE(readability-non-const-parameter) -- assigned to the non-const fw_if_fs_backend_t stream vtable slot. */
internal_contract_read(void* ctx, void* file_state, uint8_t* dst, uint32_t cap, uint32_t* out_read)
{
  (void)ctx;
  (void)file_state;
  (void)dst;
  *out_read = cap + 1U;
  return k_ra8_ok;
}

/**
 * @brief Report one byte beyond the supplied write length.
 * @details Supplies an impossible successful transfer count.
 * @param[in,out] ctx Unused backend context.
 * @param[in,out] state Unused operation state.
 * @param[in] source Unused source bytes.
 * @param[in] length Advertised source length.
 * @param[out] out_written Receives @p length plus one.
 * @return Synthetic backend status.
 * @retval k_ra8_ok The impossible output was supplied.
 * @pre @p out_written designates writable storage.
 * @pre @p length is representable with one additional byte in this vector.
 * @post @p out_written exceeds @p length by one.
 * @post Source and backend state are unchanged.
 * @note Test-only helper.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_contract_write(void*          ctx,
                                                      void*          state,
                                                      const uint8_t* source,
                                                      uint32_t       length,
                                                      uint32_t*      out_written)
{
  (void)ctx;
  (void)state;
  (void)source;
  *out_written = length + 1U;
  return k_ra8_ok;
}

/** @brief Fail without touching a 64-bit output. @details Exercises facade
 * failure-output initialization. @param[in,out] ctx Unused backend context.
 * @param[in,out] state Unused operation state. @param[out] out Intentionally
 * untouched scalar output. @return Synthetic backend status. @retval k_ra8_fail
 * The injected operation failed. @pre @p out designates writable storage. @pre
 * The facade initialized @p out before dispatch. @post @p out is unchanged by
 * this callback. @post No backend state is changed. @note Test-only helper.
 * @since 0.1.0 */
/* NOLINTNEXTLINE(readability-non-const-parameter) -- assigned to the non-const fw_if_fs_backend_t tell vtable slot. */
RA8_INTERNAL static ra8_err_t internal_contract_u64_error(void* ctx, void* state, uint64_t* out)
{
  (void)ctx;
  (void)state;
  (void)out;
  return k_ra8_fail;
}

/**
 * @brief Return a configurable transaction publication result.
 * @details Copies the caller-owned status and publication flag independently.
 * @param[in,out] ctx Caller-owned ::contract_results_t configuration.
 * @param[in,out] state Unused transaction state.
 * @param[out] out_published Receives the configured publication flag.
 * @return Configured transaction status.
 * @retval k_ra8_ok The configured successful status was returned.
 * @retval k_ra8_fail The configured failure status was returned.
 * @pre @p ctx and @p out_published designate live objects.
 * @pre The transaction facade is active and validated.
 * @post @p out_published equals the configured flag.
 * @post No namespace or backend state changes.
 * @note Test-only helper.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_contract_commit(void* ctx, void* state, bool* out_published)
{
  (void)state;
  const contract_results_t* results = (const contract_results_t*)ctx;
  *out_published                    = results->commit_published;
  return results->commit_status;
}

/** @brief Accept cleanup of the synthetic transaction state. @details Provides
 * a valid abort callback after the hostile commit result. @param[in,out] ctx
 * Unused backend context. @param[in,out] state Unused transaction state.
 * @return Synthetic backend status. @retval k_ra8_ok Cleanup is accepted. @pre
 * The transaction facade is still active. @pre @p state is the facade's
 * borrowed test state. @post No namespace or backend state changes. @post The
 * facade may consume its transaction handle. @note Test-only helper. @since
 * 0.1.0 */
RA8_INTERNAL static ra8_err_t internal_contract_abort(void* ctx, void* state)
{
  (void)ctx;
  (void)state;
  return k_ra8_ok;
}

/** @brief Assert that one locally mutated vtable binding is rejected. @details
 * Calls the public binder with truthful context and capabilities while exposing
 * exactly one missing required callback. @param[in] fs Truthful baseline
 * binding. @param[in] names Candidate namespace table. @param[in] streams
 * Candidate stream table. @param[in] transactions Candidate transaction table.
 * @pre All non-null tables remain live for the call. @pre Exactly one required
 * callback is absent. @post The binder reports invalid argument. @post The
 * baseline binding remains unchanged. @note Test-only assertion helper. @since
 * 0.1.0 */
RA8_INTERNAL static void
internal_expect_interface_rejection(const fw_fs_t*                   fs,
                                    const fw_fs_namespace_iface_t*   names,
                                    const fw_fs_stream_iface_t*      streams,
                                    const fw_fs_transaction_iface_t* transactions)
{
  fw_fs_t bound = {};
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 fw_fs_bind(&bound, names, streams, transactions, (void*)fs, &fs->caps));
}

/**
 * @brief Toggle every required directory-walk namespace callback independently.
 * @details Starts from a truthful table before removing one directory-walk
 * member at a time (the first half of the grouped namespace decisions).
 * @param[in] fs Truthful baseline binding.
 * @pre The baseline namespace, stream, and transaction tables are complete.
 * @pre The baseline capability record remains immutable.
 * @post Every missing directory-walk namespace member is rejected independently.
 * @post The baseline binding remains unchanged.
 * @note Supplies half the true vectors for the namespace callback decisions.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_check_required_namespace_dir_ops(const fw_fs_t* fs)
{
  fw_fs_namespace_iface_t names = *fs->names.iface;
  /** @brief Reject one namespace vtable with the selected member cleared. */
#define EXPECT_MISSING_NAMESPACE(member)                                                           \
  do {                                                                                             \
    names        = *fs->names.iface;                                                               \
    names.member = nullptr;                                                                        \
    internal_expect_interface_rejection(fs, &names, fs->streams.iface, fs->transactions.iface);    \
  } while (0)
  EXPECT_MISSING_NAMESPACE(stat);
  EXPECT_MISSING_NAMESPACE(listdir);
  EXPECT_MISSING_NAMESPACE(dir_open);
  EXPECT_MISSING_NAMESPACE(dir_next);
  EXPECT_MISSING_NAMESPACE(dir_close);
#undef EXPECT_MISSING_NAMESPACE
}

/**
 * @brief Toggle every required path-mutation namespace callback independently.
 * @details Starts from a truthful table before removing one path-mutation
 * member at a time (the second half of the grouped namespace decisions).
 * @param[in] fs Truthful baseline binding.
 * @pre The baseline namespace, stream, and transaction tables are complete.
 * @pre The baseline capability record remains immutable.
 * @post Every missing path-mutation namespace member is rejected independently.
 * @post The baseline binding remains unchanged.
 * @note Supplies the remaining true vectors for the namespace callback decisions.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_check_required_namespace_path_ops(const fw_fs_t* fs)
{
  fw_fs_namespace_iface_t names = *fs->names.iface;
  /** @brief Reject one namespace vtable with the selected member cleared. */
#define EXPECT_MISSING_NAMESPACE(member)                                                           \
  do {                                                                                             \
    names        = *fs->names.iface;                                                               \
    names.member = nullptr;                                                                        \
    internal_expect_interface_rejection(fs, &names, fs->streams.iface, fs->transactions.iface);    \
  } while (0)
  EXPECT_MISSING_NAMESPACE(mkdir);
  EXPECT_MISSING_NAMESPACE(unlink);
  EXPECT_MISSING_NAMESPACE(rmdir);
  EXPECT_MISSING_NAMESPACE(rename);
#undef EXPECT_MISSING_NAMESPACE
}

/**
 * @brief Toggle every required namespace callback independently.
 * @details Delegates to the directory-walk and path-mutation halves, in the
 * same toggle order the unsplit routine used, so every operand in the
 * grouped namespace decisions is still exercised from one call.
 * @param[in] fs Truthful baseline binding.
 * @pre The baseline namespace, stream, and transaction tables are complete.
 * @pre The baseline capability record remains immutable.
 * @post Every missing namespace member is rejected independently.
 * @post The baseline binding remains unchanged.
 * @note Supplies the true vectors for the namespace callback decisions.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_check_required_namespace(const fw_fs_t* fs)
{
  internal_check_required_namespace_dir_ops(fs);
  internal_check_required_namespace_path_ops(fs);
}

/** @brief Toggle every required stream callback independently. @details Starts
 * from a truthful stream table before removing each grouped member in turn.
 * @param[in] fs Truthful baseline binding. @pre The baseline namespace, stream,
 * and transaction tables are complete. @pre The baseline capability record
 * remains immutable. @post Every missing stream member is rejected
 * independently. @post The baseline binding remains unchanged. @note Supplies
 * the true vectors for the stream callback decisions. @since 0.1.0 */
RA8_INTERNAL static void internal_check_required_streams(const fw_fs_t* fs)
{
  fw_fs_stream_iface_t streams = *fs->streams.iface;
  /** @brief Reject one stream vtable with the selected member cleared. */
#define EXPECT_MISSING_STREAM(member)                                                              \
  do {                                                                                             \
    streams        = *fs->streams.iface;                                                           \
    streams.member = nullptr;                                                                      \
    internal_expect_interface_rejection(fs, fs->names.iface, &streams, fs->transactions.iface);    \
  } while (0)
  EXPECT_MISSING_STREAM(open);
  EXPECT_MISSING_STREAM(read);
  EXPECT_MISSING_STREAM(write);
  EXPECT_MISSING_STREAM(close);
  EXPECT_MISSING_STREAM(seek);
  EXPECT_MISSING_STREAM(tell);
  EXPECT_MISSING_STREAM(size);
#undef EXPECT_MISSING_STREAM
}

/** @brief Toggle every required transaction callback independently. @details
 * Starts from a truthful transaction table before removing each grouped member
 * in turn. @param[in] fs Truthful baseline binding. @pre The baseline
 * namespace, stream, and transaction tables are complete. @pre The baseline
 * capability record remains immutable. @post Every missing transaction member
 * is rejected independently. @post The baseline binding remains unchanged.
 * @note Supplies the true vectors for the transaction callback decisions.
 * @since 0.1.0 */
RA8_INTERNAL static void internal_check_required_transactions(const fw_fs_t* fs)
{
  fw_fs_transaction_iface_t transactions = *fs->transactions.iface;
  /** @brief Reject one transaction vtable with the selected member cleared. */
#define EXPECT_MISSING_TRANSACTION(member)                                                         \
  do {                                                                                             \
    transactions        = *fs->transactions.iface;                                                 \
    transactions.member = nullptr;                                                                 \
    internal_expect_interface_rejection(fs, fs->names.iface, fs->streams.iface, &transactions);    \
  } while (0)
  EXPECT_MISSING_TRANSACTION(begin);
  EXPECT_MISSING_TRANSACTION(write);
  EXPECT_MISSING_TRANSACTION(seek);
  EXPECT_MISSING_TRANSACTION(validate);
  EXPECT_MISSING_TRANSACTION(commit);
  EXPECT_MISSING_TRANSACTION(abort);
#undef EXPECT_MISSING_TRANSACTION
}

/**
 * @brief Exercise every null member of the binder's pointer tuples.
 * @details Uses a truthful all-present vector before removing each operand.
 * @param[in] fs Truthful baseline binding.
 * @pre All baseline tables and capabilities are complete.
 * @pre The baseline binding remains live for the test.
 * @post Each null tuple member reports null pointer.
 * @post A complete tuple binds successfully.
 * @note Supplies N+1 vectors for both binder null decisions.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_check_bind_null_tuples(const fw_fs_t* fs)
{
  fw_fs_t bound = {};
  TEST_ASSERT_EQ(k_ra8_ok,
                 fw_fs_bind(&bound,
                            fs->names.iface,
                            fs->streams.iface,
                            fs->transactions.iface,
                            (void*)fs,
                            &fs->caps));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 fw_fs_bind(nullptr,
                            fs->names.iface,
                            fs->streams.iface,
                            fs->transactions.iface,
                            (void*)fs,
                            &fs->caps));
  TEST_ASSERT_EQ(
    k_ra8_err_null_ptr,
    fw_fs_bind(&bound, nullptr, fs->streams.iface, fs->transactions.iface, (void*)fs, &fs->caps));
  TEST_ASSERT_EQ(
    k_ra8_err_null_ptr,
    fw_fs_bind(&bound, fs->names.iface, nullptr, fs->transactions.iface, (void*)fs, &fs->caps));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 fw_fs_bind(&bound,
                            fs->names.iface,
                            fs->streams.iface,
                            fs->transactions.iface,
                            nullptr,
                            &fs->caps));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 fw_fs_bind(&bound,
                            fs->names.iface,
                            fs->streams.iface,
                            fs->transactions.iface,
                            (void*)fs,
                            nullptr));
}

/** @brief Exercise grouped capability and workspace coherence decisions.
 * @details Builds honest false/false and true/false combinations before
 * supplying each contradictory true operand independently. @param[in] fs
 * Truthful baseline binding. @pre The baseline tables and capabilities agree.
 * @pre Optional callbacks are live when advertised. @post Every
 * capability/table contradiction is rejected. @post Every directory workspace
 * operand independently changes the result. @note Supplies the non-null binder
 * MC/DC vectors. @since 0.1.0 */
RA8_INTERNAL static void internal_check_bind_capabilities(const fw_fs_t* fs)
{
  fw_fs_namespace_iface_t names   = *fs->names.iface;
  fw_fs_stream_iface_t    streams = *fs->streams.iface;
  fw_fs_caps_t            caps    = fs->caps;
  fw_fs_t                 bound   = {};
  caps.flags &= ~((uint32_t)k_fw_fs_cap_space_query | (uint32_t)k_fw_fs_cap_file_sync |
                  (uint32_t)k_fw_fs_cap_durable_file_sync | (uint32_t)k_fw_fs_cap_transactions);
  names.space  = nullptr;
  streams.sync = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, fw_fs_bind(&bound, &names, &streams, nullptr, (void*)fs, &caps));
  caps.flags |= (uint32_t)k_fw_fs_cap_space_query;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 fw_fs_bind(&bound, &names, &streams, nullptr, (void*)fs, &caps));
  caps.flags &= ~(uint32_t)k_fw_fs_cap_space_query;
  caps.flags |= (uint32_t)k_fw_fs_cap_transactions;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 fw_fs_bind(&bound, &names, &streams, nullptr, (void*)fs, &caps));
  caps.flags &= ~(uint32_t)k_fw_fs_cap_transactions;
  caps.flags |= (uint32_t)k_fw_fs_cap_durable_file_sync;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 fw_fs_bind(&bound, &names, &streams, nullptr, (void*)fs, &caps));
  streams.sync = fs->streams.iface->sync;
  caps.flags |= (uint32_t)k_fw_fs_cap_file_sync;
  TEST_ASSERT_EQ(k_ra8_ok, fw_fs_bind(&bound, &names, &streams, nullptr, (void*)fs, &caps));

  caps                           = fs->caps;
  caps.directory_workspace_bytes = 0U;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 fw_fs_bind(&bound,
                            fs->names.iface,
                            fs->streams.iface,
                            fs->transactions.iface,
                            (void*)fs,
                            &caps));
  caps                      = fs->caps;
  caps.max_open_directories = 0U;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 fw_fs_bind(&bound,
                            fs->names.iface,
                            fs->streams.iface,
                            fs->transactions.iface,
                            (void*)fs,
                            &caps));
  caps                           = fs->caps;
  caps.directory_workspace_align = 3U;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 fw_fs_bind(&bound,
                            fs->names.iface,
                            fs->streams.iface,
                            fs->transactions.iface,
                            (void*)fs,
                            &caps));
}

/** @brief Prove optional operation absence and reject dishonest capabilities.
 * @details Binds local vtable copies with optional operations removed, then
 * contradicts those tables with durability flags. @param[in] fs Truthful
 * baseline binding. @pre @p fs is fully bound. @pre Baseline namespace and
 * stream tables remain live. @post Honest optional absence is accepted. @post
 * Contradictory sync capabilities are rejected. @note Assertions report
 * contract failures. @since 0.1.0 */
RA8_INTERNAL static void internal_check_optional_contracts(const fw_fs_t* fs)
{
  fw_fs_namespace_iface_t names         = *fs->names.iface;
  fw_fs_stream_iface_t    streams       = *fs->streams.iface;
  fw_fs_caps_t            optional_caps = fs->caps;
  optional_caps.flags &=
    ~((uint32_t)k_fw_fs_cap_file_sync | (uint32_t)k_fw_fs_cap_durable_file_sync |
      (uint32_t)k_fw_fs_cap_transactions);
  streams.sync  = nullptr;
  fw_fs_t bound = {};
  TEST_ASSERT_EQ(k_ra8_ok,
                 fw_fs_bind(&bound, &names, &streams, nullptr, (void*)fs, &optional_caps));
  fw_fs_file_t file = {.iface = &streams, .ctx = (void*)fs, .state = &bound, .is_open = true};
  TEST_ASSERT_EQ(k_ra8_err_not_supported, fw_fs_sync(&file));
  uint8_t             transaction_work = 0U;
  fw_fs_transaction_t transaction      = {};
  TEST_ASSERT_EQ(k_ra8_err_not_supported,
                 fw_fs_transaction_begin(&bound.transactions,
                                         "/artifact",
                                         k_fw_fs_txn_create_new,
                                         &transaction,
                                         &transaction_work,
                                         sizeof(transaction_work)));

  fw_fs_caps_t dishonest_caps = optional_caps;
  dishonest_caps.flags |= (uint32_t)k_fw_fs_cap_file_sync;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 fw_fs_bind(&bound, &names, &streams, nullptr, (void*)fs, &dishonest_caps));
  dishonest_caps.flags &= ~(uint32_t)k_fw_fs_cap_file_sync;
  dishonest_caps.flags |= (uint32_t)k_fw_fs_cap_durable_file_sync;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 fw_fs_bind(&bound, &names, &streams, nullptr, (void*)fs, &dishonest_caps));
}

/** @brief Toggle every public facade null-tuple operand independently. @details
 * Uses live facades for the all-present paths and supplies one null argument at
 * a time to each grouped guard. @param[in] fs Truthful baseline binding. @pre
 * The filesystem is fully bound. @pre The baseline operations remain live.
 * @post Every null tuple member reports null pointer. @post No backend call
 * observes a rejected tuple. @note Normal conformance supplies the all-present
 * vectors. @since 0.1.0 */
RA8_INTERNAL static void internal_check_public_null_tuples(const fw_fs_t* fs)
{
  fw_fs_caps_t caps = {};
  TEST_ASSERT_EQ(k_ra8_ok, fw_fs_get_caps(fs, &caps));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, fw_fs_get_caps(nullptr, &caps));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, fw_fs_get_caps(fs, nullptr));

  fw_fs_file_t file = {};
  uint8_t      byte = 0U;
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 fw_fs_open(nullptr, "/unused", k_fw_fs_open_read, &file, &byte, sizeof(byte)));
  TEST_ASSERT_EQ(
    k_ra8_err_null_ptr,
    fw_fs_open(&fs->streams, "/unused", k_fw_fs_open_read, nullptr, &byte, sizeof(byte)));

  fw_fs_stream_iface_t streams = *fs->streams.iface;
  streams.read                 = internal_contract_read;
  streams.write                = internal_contract_write;
  file = (fw_fs_file_t){.iface = &streams, .ctx = (void*)fs, .state = &byte, .is_open = true};
  uint32_t count = 0U;
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, fw_fs_read(&file, nullptr, 1U, &count));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, fw_fs_read(&file, &byte, 1U, nullptr));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, fw_fs_write(&file, nullptr, 1U, &count));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, fw_fs_write(&file, &byte, 1U, nullptr));

  fw_fs_transaction_t transaction = {};
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 fw_fs_transaction_begin(nullptr,
                                         "/unused",
                                         k_fw_fs_txn_create_new,
                                         &transaction,
                                         &byte,
                                         sizeof(byte)));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 fw_fs_transaction_begin(&fs->transactions,
                                         "/unused",
                                         k_fw_fs_txn_create_new,
                                         nullptr,
                                         &byte,
                                         sizeof(byte)));
  fw_fs_transaction_iface_t transactions = *fs->transactions.iface;
  transactions.write                     = internal_contract_write;
  transaction =
    (fw_fs_transaction_t){.iface = &transactions, .ctx = (void*)fs, .state = &byte, .active = true};
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, fw_fs_transaction_write(&transaction, nullptr, 1U, &count));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, fw_fs_transaction_write(&transaction, &byte, 1U, nullptr));
}

/** @brief Exercise every stat-output predicate independently. @details Supplies
 * valid missing/file/directory states plus exactly one invalid type, presence,
 * or size relationship at a time. @param[in] fs Truthful baseline binding. @pre
 * The namespace capabilities admit the root path. @pre The baseline stat
 * callback remains available only in the untouched binding. @post Valid states
 * pass and each isolated contradiction is rejected. @post Rejected caller
 * outputs are cleared. @note The case table supplies the minimal toggles for
 * nested and aggregate decisions. @since 0.1.0 */
RA8_INTERNAL static void internal_check_stat_contracts(const fw_fs_t* fs)
{
  typedef struct {
    fw_fs_stat_t stat;     /**< Candidate callback result. */
    ra8_err_t    expected; /**< Expected facade status.    */
  } stat_case_t;
  static const stat_case_t k_cases[] = {
    {.stat = {.exists = false, .type = k_fw_fs_node_none, .size_bytes = 0U}, .expected = k_ra8_ok},
    {.stat = {.exists = true, .type = k_fw_fs_node_file, .size_bytes = 1U}, .expected = k_ra8_ok},
    {.stat     = {.exists = true, .type = k_fw_fs_node_directory, .size_bytes = 0U},
     .expected = k_ra8_ok},
    {.stat     = {.exists = false, .type = k_fw_fs_node_file, .size_bytes = 0U},
     .expected = k_ra8_err_invalid_state},
    {.stat     = {.exists = false, .type = k_fw_fs_node_none, .size_bytes = 1U},
     .expected = k_ra8_err_invalid_state},
    {.stat     = {.exists = true, .type = k_fw_fs_node_none, .size_bytes = 0U},
     .expected = k_ra8_err_invalid_state},
    {.stat     = {.exists = true, .type = k_fw_fs_node_directory, .size_bytes = 1U},
     .expected = k_ra8_err_invalid_state},
    {.stat     = {.exists = true, .type = (fw_fs_node_type_t)UINT32_MAX, .size_bytes = 0U},
     .expected = k_ra8_err_invalid_state},
  };
  fw_fs_namespace_iface_t names = *fs->names.iface;
  names.stat                    = internal_contract_stat;
  contract_results_t results    = {};
  fw_fs_namespace_t  port       = {.iface = &names, .ctx = &results, .caps = fs->caps};
  for (size_t index = 0U; index < (sizeof(k_cases) / sizeof(k_cases[0])); ++index) {
    results.stat        = k_cases[index].stat;
    fw_fs_stat_t output = {};
    TEST_ASSERT_EQ(k_cases[index].expected, fw_fs_stat(&port, "/case", &output));
    if (k_cases[index].expected != k_ra8_ok) {
      TEST_ASSERT(!output.exists && (output.type == k_fw_fs_node_none) &&
                  (output.size_bytes == 0U));
    }
  }
}

/** @brief Exercise list, rename, and space compound decisions. @details Toggles
 * each nullable list output, each protected root operand, callback success, and
 * both impossible volume comparisons independently. @param[in] fs Truthful
 * baseline binding. @pre The binding advertises listing, no-replace rename, and
 * space query. @pre All baseline namespace callbacks remain live. @post
 * Null/root operands fail before backend dispatch. @post Invalid accounting is
 * cleared while valid/failing callbacks retain their status. @note Normal
 * conformance supplies the all-valid vectors. @since 0.1.0 */
RA8_INTERNAL static void internal_check_namespace_decisions(const fw_fs_t* fs)
{
  uint32_t count    = 0U;
  bool     complete = false;
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 fw_fs_listdir(&fs->names, "/", 1U, nullptr, nullptr, &count, &complete));
  TEST_ASSERT_EQ(
    k_ra8_err_null_ptr,
    fw_fs_listdir(&fs->names, "/", 1U, internal_contract_entry, nullptr, nullptr, &complete));
  TEST_ASSERT_EQ(
    k_ra8_err_null_ptr,
    fw_fs_listdir(&fs->names, "/", 1U, internal_contract_entry, nullptr, &count, nullptr));
  TEST_ASSERT_EQ(k_ra8_err_access_denied, fw_fs_rename(&fs->names, "/", "/unused", false));
  TEST_ASSERT_EQ(k_ra8_err_access_denied, fw_fs_rename(&fs->names, "/unused", "/", false));

  fw_fs_namespace_iface_t names = *fs->names.iface;
  names.space                   = internal_contract_space;
  contract_results_t results    = {};
  fw_fs_namespace_t  port       = {.iface = &names, .ctx = &results, .caps = fs->caps};
  port.caps.flags |= (uint32_t)k_fw_fs_cap_space_query;
  const fw_fs_space_t valid = {.total_bytes = 10U, .free_bytes = 4U, .used_bytes = 6U};
  results                   = (contract_results_t){.space = valid, .space_status = k_ra8_ok};
  fw_fs_space_t output      = {};
  TEST_ASSERT_EQ(k_ra8_ok, fw_fs_space(&port, &output));
  results = (contract_results_t){.space        = {.total_bytes = 10U, .free_bytes = 11U},
                                 .space_status = k_ra8_fail};
  TEST_ASSERT_EQ(k_ra8_fail, fw_fs_space(&port, &output));
  results.space_status = k_ra8_ok;
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, fw_fs_space(&port, &output));
  results.space = (fw_fs_space_t){.total_bytes = 10U, .used_bytes = 11U};
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, fw_fs_space(&port, &output));
}

/** @brief Reject incoherent namespace metadata, counts, and space totals.
 * @details Replaces namespace callbacks in a local vtable copy with hostile
 * successes. @param[in] fs Truthful baseline binding. @pre @p fs is fully
 * bound. @pre The baseline namespace table remains live. @post Every impossible
 * namespace output is rejected. @post Caller-visible outputs are reset to safe
 * empty values. @note Assertions report contract failures. @since 0.1.0 */
RA8_INTERNAL static void internal_check_namespace_contracts(const fw_fs_t* fs)
{
  fw_fs_namespace_iface_t names = *fs->names.iface;
  names.stat                    = internal_contract_stat;
  names.listdir                 = internal_contract_list;
  names.space                   = internal_contract_space;
  contract_results_t results    = {
    .stat         = {.exists = false, .type = k_fw_fs_node_file, .size_bytes = 1U},
    .space        = {.total_bytes = 10U, .free_bytes = 11U, .used_bytes = 0U},
    .space_status = k_ra8_ok,
  };
  fw_fs_namespace_t namespace_port = {.iface = &names, .ctx = &results, .caps = fs->caps};
  fw_fs_stat_t      stat           = {};
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, fw_fs_stat(&namespace_port, "/bad", &stat));
  TEST_ASSERT(!stat.exists && (stat.type == k_fw_fs_node_none) && (stat.size_bytes == 0U));
  uint32_t count    = UINT32_MAX;
  bool     complete = true;
  TEST_ASSERT_EQ(
    k_ra8_err_invalid_state,
    fw_fs_listdir(&namespace_port, "/", 1U, internal_contract_entry, nullptr, &count, &complete));
  TEST_ASSERT_EQ(0U, count);
  TEST_ASSERT(!complete);
  fw_fs_space_t space = {};
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, fw_fs_space(&namespace_port, &space));
  TEST_ASSERT((space.total_bytes == 0U) && (space.free_bytes == 0U) && (space.used_bytes == 0U));
}

/**
 * @brief Reject impossible stream read/write/tell/size results.
 * @details Replaces stream callbacks in a local vtable copy with hostile
 * successes and confirms the facade resets every failed output.
 * @param[in] fs Truthful baseline binding.
 * @pre @p fs is fully bound.
 * @pre The baseline stream table remains live for the call.
 * @post Impossible transfers and scalars are rejected.
 * @post Failed outputs are reset to zero.
 * @note Assertions report contract failures.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_check_stream_transfer_contracts(const fw_fs_t* fs)
{
  fw_fs_stream_iface_t streams = *fs->streams.iface;
  streams.read                 = internal_contract_read;
  streams.write                = internal_contract_write;
  streams.tell                 = internal_contract_u64_error;
  streams.size                 = internal_contract_u64_error;
  fw_fs_t      bound           = {};
  fw_fs_file_t file = {.iface = &streams, .ctx = (void*)fs, .state = &bound, .is_open = true};
  uint8_t      byte = 0U;
  uint32_t     transferred = UINT32_MAX;
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, fw_fs_read(&file, &byte, 1U, &transferred));
  TEST_ASSERT_EQ(0U, transferred);
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, fw_fs_write(&file, &byte, 1U, &transferred));
  TEST_ASSERT_EQ(0U, transferred);
  uint64_t scalar = UINT64_MAX;
  TEST_ASSERT_EQ(k_ra8_fail, fw_fs_tell(&file, &scalar));
  TEST_ASSERT_EQ(0U, scalar);
  scalar = UINT64_MAX;
  TEST_ASSERT_EQ(k_ra8_fail, fw_fs_file_size(&file, &scalar));
  TEST_ASSERT_EQ(0U, scalar);
}

/**
 * @brief Reject impossible transaction write and publication results.
 * @details Replaces transaction callbacks in a local vtable copy with hostile
 * results across three transaction lifetimes: a rejected write/commit that
 * still permits abort, a failed commit that leaves the transaction open, and
 * a successful commit that closes it.
 * @param[in] fs Truthful baseline binding.
 * @pre @p fs is fully bound.
 * @pre The baseline transaction table remains live for the call.
 * @post Impossible writes and publications are rejected.
 * @post Transaction `active`/`validated` state matches each outcome exactly.
 * @note Assertions report contract failures.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_check_transaction_contracts(const fw_fs_t* fs)
{
  fw_fs_transaction_iface_t transactions = *fs->transactions.iface;
  transactions.write                     = internal_contract_write;
  transactions.commit                    = internal_contract_commit;
  transactions.abort                     = internal_contract_abort;
  fw_fs_t             bound              = {};
  uint8_t             byte               = 0U;
  uint32_t            transferred        = UINT32_MAX;
  contract_results_t  results            = {.commit_status = k_ra8_ok, .commit_published = false};
  fw_fs_transaction_t transaction        = {.iface     = &transactions,
                                            .ctx       = &results,
                                            .state     = &bound,
                                            .active    = true,
                                            .validated = false};
  TEST_ASSERT_EQ(k_ra8_err_invalid_state,
                 fw_fs_transaction_write(&transaction, &byte, 1U, &transferred));
  TEST_ASSERT_EQ(0U, transferred);
  transaction.validated = true;
  bool published        = true;
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, fw_fs_transaction_commit(&transaction, &published));
  TEST_ASSERT(!published);
  TEST_ASSERT(transaction.active && transaction.validated);
  TEST_ASSERT_EQ(k_ra8_ok, fw_fs_transaction_abort(&transaction));
  TEST_ASSERT(!transaction.active && !transaction.validated);

  results     = (contract_results_t){.commit_status = k_ra8_fail, .commit_published = false};
  transaction = (fw_fs_transaction_t){.iface     = &transactions,
                                      .ctx       = &results,
                                      .state     = &bound,
                                      .active    = true,
                                      .validated = true};
  TEST_ASSERT_EQ(k_ra8_fail, fw_fs_transaction_commit(&transaction, &published));
  TEST_ASSERT(transaction.active && transaction.validated && !published);
  results.commit_status    = k_ra8_ok;
  results.commit_published = true;
  TEST_ASSERT_EQ(k_ra8_ok, fw_fs_transaction_commit(&transaction, &published));
  TEST_ASSERT(!transaction.active && !transaction.validated && published);
}

/**
 * @brief Reject impossible stream counts, scalars, and publication results.
 * @details Delegates to the stream-transfer and transaction halves, in the
 * same order the unsplit routine ran them, so both hostile vtable replacements
 * are still exercised from one call.
 * @param[in] fs Truthful baseline binding.
 * @pre @p fs is fully bound.
 * @pre Baseline stream and transaction tables remain live.
 * @post Impossible transfers, scalars, and publication are rejected.
 * @post Failed outputs are reset and transaction cleanup remains possible.
 * @note Assertions report contract failures.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_check_stream_contracts(const fw_fs_t* fs)
{
  internal_check_stream_transfer_contracts(fs);
  internal_check_transaction_contracts(fs);
}

/* see header for full description */
RA8_TEST_HELPER void ra8_test_fw_if_fs_check_contract_guards(const fw_fs_t* fs)
{
  internal_check_required_namespace(fs);
  internal_check_required_streams(fs);
  internal_check_required_transactions(fs);
  internal_check_bind_null_tuples(fs);
  internal_check_bind_capabilities(fs);
  internal_check_optional_contracts(fs);
  internal_check_public_null_tuples(fs);
  internal_check_stat_contracts(fs);
  internal_check_namespace_decisions(fs);
  internal_check_namespace_contracts(fs);
  internal_check_stream_contracts(fs);
}

/* see header for full description */
RA8_TEST_HELPER void ra8_test_fw_if_fs_vfs_init_guards(ra8_fs_mount_t* mount)
{
  fw_fs_t               removable = {};
  fw_fs_ra8_vfs_state_t state     = {};
  ra8_fs_mount_t        idle      = {};
  fw_fs_ra8_vfs_cfg_t   cfg       = {.mount_name = "ram", .mount = mount};
  TEST_BEGIN("fw_if_fs VFS init guards");
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, fw_fs_ra8_vfs_init(nullptr, &state, &cfg));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, fw_fs_ra8_vfs_init(&removable, nullptr, &cfg));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, fw_fs_ra8_vfs_init(&removable, &state, nullptr));
  cfg.mount_name = nullptr;
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, fw_fs_ra8_vfs_init(&removable, &state, &cfg));
  cfg.mount_name = "ram";
  cfg.mount      = nullptr;
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, fw_fs_ra8_vfs_init(&removable, &state, &cfg));
  cfg.mount = &idle;
  TEST_ASSERT_EQ(k_ra8_err_not_initialized, fw_fs_ra8_vfs_init(&removable, &state, &cfg));
  cfg.mount      = mount;
  cfg.mount_name = "ra:m";
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, fw_fs_ra8_vfs_init(&removable, &state, &cfg));
  cfg.mount_name = "ra/m";
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, fw_fs_ra8_vfs_init(&removable, &state, &cfg));
  cfg.mount_name = "";
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, fw_fs_ra8_vfs_init(&removable, &state, &cfg));
  cfg.mount_name = "0123456789abcdef";
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, fw_fs_ra8_vfs_init(&removable, &state, &cfg));
  cfg.mount_name = "absent";
  TEST_ASSERT_EQ(k_ra8_err_not_found, fw_fs_ra8_vfs_init(&removable, &state, &cfg));
  cfg.mount_name      = "ram";
  cfg.removable_media = true;
  TEST_ASSERT_EQ(k_ra8_ok, fw_fs_ra8_vfs_init(&removable, &state, &cfg));
  TEST_ASSERT((removable.caps.flags & (uint32_t)k_fw_fs_cap_removable_media) != 0U);
  TEST_END("fw_if_fs VFS init guards");
}
