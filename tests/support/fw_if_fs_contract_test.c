/**
 * @file fw_if_fs_contract_test.c
 * @brief Fault vectors for portable filesystem facade output contracts.
 *
 * @details
 * Supplies synthetic namespace, stream, and transaction callbacks that return
 * structurally impossible success values. The shared vector binds only local
 * vtable copies, proving facade validation independently of any filesystem
 * backend and without mutating the truthful binding used as its baseline.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "fw_if_fs_contract_test.h"

#include <stdint.h>

#include "fw_if_fs_backend.h"
#include "ra8_attributes.h"
#include "ra8_err.h"
#include "unity_minimal.h"

/** @brief Return incoherent success metadata for facade fault injection. @details Implements the bounded contract stat fixture step using caller-owned state. @param[in,out] ctx Caller-owned fixture or filesystem state. @param[in] path Validated fixture path or name value. @param[out] out Caller-owned output populated on success. @return Status, selected object, or bounded value produced by the named operation. @retval k_ra8_ok The requested operation completed. @retval k_ra8_err_* Validation or backend work failed. @pre Pointer arguments address their documented readable or writable extents. @pre Required fixture and backend state is initialized before the call. @post No access exceeds a caller-advertised capacity. @post The return value or assertions describe the observed filesystem state. @note Test-only helpers retain no hidden ownership beyond documented fixture state. @since 0.1.0 */
RA8_INTERNAL static ra8_err_t internal_contract_stat(void* ctx, const char* path, fw_fs_stat_t* out)
{
  (void)ctx;
  (void)path;
  out->exists     = false;
  out->type       = k_fw_fs_node_file;
  out->size_bytes = 1U;
  return k_ra8_ok;
}

/** @brief Report more directory entries than the caller permitted. @details Implements the bounded contract list fixture step using caller-owned state. @param[in,out] ctx Caller-owned fixture or filesystem state. @param[in] path Validated portable path. @param[in] max_entries Maximum accepted entry count. @param[in] callback Caller callback, not invoked by this fault. @param[in,out] callback_ctx Caller callback context. @param[out] out_count Receives the impossible count. @param[out] out_complete Receives completion state. @return Synthetic backend status. @retval k_ra8_ok The impossible output was supplied for facade validation. @pre All output pointers designate writable objects. @pre The caller already validated the path and callback arguments. @post @p out_count exceeds @p max_entries by one. @post @p out_complete is true and no callback was invoked. @note Test-only helper with no retained state. @since 0.1.0 */
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

/** @brief Valid callback shape for the bounded-list fault. @details Accepts one entry so the facade can validate a hostile backend count. @param[in,out] ctx Unused callback context. @param[in] entry Unused delivered entry. @param[out] out_continue Receives true. @return Callback status. @retval k_ra8_ok Delivery may continue. @pre @p entry and @p out_continue are non-null. @pre The caller owns @p out_continue. @post @p out_continue is true. @post No caller or backend state is retained. @note Test-only helper. @since 0.1.0 */
RA8_INTERNAL static ra8_err_t
internal_contract_entry(void* ctx, const fw_fs_dirent_t* entry, bool* out_continue)
{
  (void)ctx;
  (void)entry;
  *out_continue = true;
  return k_ra8_ok;
}

/** @brief Return impossible volume accounting for facade fault injection. @details Produces free bytes greater than total bytes. @param[in,out] ctx Unused backend context. @param[out] out Receives incoherent accounting. @return Synthetic backend status. @retval k_ra8_ok The impossible output was supplied. @pre @p out designates writable storage. @pre The facade initialized @p out before dispatch. @post Free bytes exceed total bytes. @post No backend state is changed. @note Test-only helper. @since 0.1.0 */
RA8_INTERNAL static ra8_err_t internal_contract_space(void* ctx, fw_fs_space_t* out)
{
  (void)ctx;
  out->total_bytes = 10U;
  out->free_bytes  = 11U;
  out->used_bytes  = 0U;
  return k_ra8_ok;
}

/** @brief Report one byte beyond the supplied read capacity. @details Supplies an impossible successful transfer count. @param[in,out] ctx Unused backend context. @param[in,out] file_state Unused file state. @param[out] dst Unused destination. @param[in] cap Advertised destination capacity. @param[out] out_read Receives @p cap plus one. @return Synthetic backend status. @retval k_ra8_ok The impossible output was supplied. @pre @p out_read designates writable storage. @pre @p cap is representable with one additional byte in this vector. @post @p out_read exceeds @p cap by one. @post @p dst and backend state are unchanged. @note Test-only helper. @since 0.1.0 */
RA8_INTERNAL static ra8_err_t
internal_contract_read(void* ctx, void* file_state, uint8_t* dst, uint32_t cap, uint32_t* out_read)
{
  (void)ctx;
  (void)file_state;
  (void)dst;
  *out_read = cap + 1U;
  return k_ra8_ok;
}

/** @brief Report one byte beyond the supplied write length. @details Supplies an impossible successful transfer count. @param[in,out] ctx Unused backend context. @param[in,out] state Unused operation state. @param[in] source Unused source bytes. @param[in] length Advertised source length. @param[out] out_written Receives @p length plus one. @return Synthetic backend status. @retval k_ra8_ok The impossible output was supplied. @pre @p out_written designates writable storage. @pre @p length is representable with one additional byte in this vector. @post @p out_written exceeds @p length by one. @post Source and backend state are unchanged. @note Test-only helper. @since 0.1.0 */
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

/** @brief Fail without touching a 64-bit output. @details Exercises facade failure-output initialization. @param[in,out] ctx Unused backend context. @param[in,out] state Unused operation state. @param[out] out Intentionally untouched scalar output. @return Synthetic backend status. @retval k_ra8_fail The injected operation failed. @pre @p out designates writable storage. @pre The facade initialized @p out before dispatch. @post @p out is unchanged by this callback. @post No backend state is changed. @note Test-only helper. @since 0.1.0 */
RA8_INTERNAL static ra8_err_t internal_contract_u64_error(void* ctx, void* state, uint64_t* out)
{
  (void)ctx;
  (void)state;
  (void)out;
  return k_ra8_fail;
}

/** @brief Claim commit success without publishing anything. @details Supplies a contradictory successful transaction result. @param[in,out] ctx Unused backend context. @param[in,out] state Unused transaction state. @param[out] out_published Receives false. @return Synthetic backend status. @retval k_ra8_ok Commit is claimed successful. @pre @p out_published designates writable storage. @pre The transaction facade is active and validated. @post @p out_published is false. @post No namespace or backend state changes. @note Test-only helper. @since 0.1.0 */
RA8_INTERNAL static ra8_err_t internal_contract_commit(void* ctx, void* state, bool* out_published)
{
  (void)ctx;
  (void)state;
  *out_published = false;
  return k_ra8_ok;
}

/** @brief Accept cleanup of the synthetic transaction state. @details Provides a valid abort callback after the hostile commit result. @param[in,out] ctx Unused backend context. @param[in,out] state Unused transaction state. @return Synthetic backend status. @retval k_ra8_ok Cleanup is accepted. @pre The transaction facade is still active. @pre @p state is the facade's borrowed test state. @post No namespace or backend state changes. @post The facade may consume its transaction handle. @note Test-only helper. @since 0.1.0 */
RA8_INTERNAL static ra8_err_t internal_contract_abort(void* ctx, void* state)
{
  (void)ctx;
  (void)state;
  return k_ra8_ok;
}

/** @brief Prove optional operation absence and reject dishonest capabilities. @details Binds local vtable copies with optional operations removed, then contradicts those tables with durability flags. @param[in] fs Truthful baseline binding. @pre @p fs is fully bound. @pre Baseline namespace and stream tables remain live. @post Honest optional absence is accepted. @post Contradictory sync capabilities are rejected. @note Assertions report contract failures. @since 0.1.0 */
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

/** @brief Reject incoherent namespace metadata, counts, and space totals. @details Replaces namespace callbacks in a local vtable copy with hostile successes. @param[in] fs Truthful baseline binding. @pre @p fs is fully bound. @pre The baseline namespace table remains live. @post Every impossible namespace output is rejected. @post Caller-visible outputs are reset to safe empty values. @note Assertions report contract failures. @since 0.1.0 */
RA8_INTERNAL static void internal_check_namespace_contracts(const fw_fs_t* fs)
{
  fw_fs_namespace_iface_t names    = *fs->names.iface;
  names.stat                       = internal_contract_stat;
  names.listdir                    = internal_contract_list;
  names.space                      = internal_contract_space;
  fw_fs_namespace_t namespace_port = {.iface = &names, .ctx = (void*)fs, .caps = fs->caps};
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

/** @brief Reject impossible stream counts, scalars, and publication results. @details Replaces stream and transaction callbacks in local vtable copies with hostile results. @param[in] fs Truthful baseline binding. @pre @p fs is fully bound. @pre Baseline stream and transaction tables remain live. @post Impossible transfers, scalars, and publication are rejected. @post Failed outputs are reset and transaction cleanup remains possible. @note Assertions report contract failures. @since 0.1.0 */
RA8_INTERNAL static void internal_check_stream_contracts(const fw_fs_t* fs)
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

  fw_fs_transaction_iface_t transactions = *fs->transactions.iface;
  transactions.write                     = internal_contract_write;
  transactions.commit                    = internal_contract_commit;
  transactions.abort                     = internal_contract_abort;
  fw_fs_transaction_t transaction        = {.iface     = &transactions,
                                            .ctx       = (void*)fs,
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
}

/* see header for full description */
RA8_TEST_HELPER void ra8_test_fw_if_fs_check_contract_guards(const fw_fs_t* fs)
{
  internal_check_optional_contracts(fs);
  internal_check_namespace_contracts(fs);
  internal_check_stream_contracts(fs);
}
