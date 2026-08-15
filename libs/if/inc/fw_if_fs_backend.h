/**
 * @file fw_if_fs_backend.h
 * @brief Backend-author interface for binding concrete filesystem ports.
 * @ingroup grp_io
 *
 * @par Tag
 * [Ring 2 / Interface] {World: Any}
 *
 * @details Applications do not construct these vtables. A concrete adapter
 * exports one `_init()` function which passes immutable vtables, a caller-owned
 * context, and truthful capabilities to ::fw_fs_bind.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "fw_if_fs.h"

/**
 * @brief Namespace backend operations.
 * @details `space` may be NULL only when ::k_fw_fs_cap_space_query is absent.
 *          A successful `stat` must report a coherent node kind/existence pair,
 *          and a successful `listdir` must not report more than its bound.
 */
struct fw_fs_namespace_iface {
  ra8_err_t (*stat)(void* ctx, const char* path, fw_fs_stat_t* out);
  ra8_err_t (*listdir)(void*           ctx,
                       const char*     path,
                       uint32_t        max_entries,
                       fw_fs_list_fn_t callback,
                       void*           callback_ctx,
                       uint32_t*       out_count,
                       bool*           out_complete);
  ra8_err_t (*mkdir)(void* ctx, const char* path);
  ra8_err_t (*unlink)(void* ctx, const char* path);
  ra8_err_t (*rmdir)(void* ctx, const char* path);
  ra8_err_t (*rename)(void* ctx, const char* old_path, const char* new_path, bool replace);
  ra8_err_t (*space)(void* ctx, fw_fs_space_t* out);
};

/**
 * @brief Open-file backend operations over caller-supplied state.
 * @details `sync` may be NULL only when ::k_fw_fs_cap_file_sync is absent.
 *          Reads and writes must never report a count larger than the caller's
 *          supplied bound, including when returning an error.
 */
struct fw_fs_stream_iface {
  ra8_err_t (*open)(void*             ctx,
                    const char*       path,
                    fw_fs_open_mode_t mode,
                    void*             file_state,
                    uint32_t          state_bytes);
  ra8_err_t (*read)(void* ctx, void* file_state, uint8_t* dst, uint32_t cap, uint32_t* out_read);
  ra8_err_t (
    *write)(void* ctx, void* file_state, const uint8_t* src, uint32_t len, uint32_t* out_written);
  ra8_err_t (*seek)(void* ctx, void* file_state, uint64_t absolute_offset);
  ra8_err_t (*tell)(void* ctx, void* file_state, uint64_t* out_offset);
  ra8_err_t (*size)(void* ctx, void* file_state, uint64_t* out_size);
  ra8_err_t (*sync)(void* ctx, void* file_state);
  ra8_err_t (*close)(void* ctx, void* file_state);
};

/**
 * @brief Staged-publication backend operations over caller-supplied state.
 * @details The entire interface may be NULL when
 *          ::k_fw_fs_cap_transactions is absent. A successful commit must set
 *          `out_published` true; an error may still set it true when
 * publication happened before a later durability operation failed.
 */
struct fw_fs_transaction_iface {
  ra8_err_t (*begin)(void*                      ctx,
                     void*                      transaction_state,
                     uint32_t                   state_bytes,
                     const char*                destination,
                     fw_fs_transaction_policy_t policy);
  ra8_err_t (*write)(void*          ctx,
                     void*          transaction_state,
                     const uint8_t* src,
                     uint32_t       len,
                     uint32_t*      out_written);
  ra8_err_t (*seek)(void* ctx, void* transaction_state, uint64_t absolute_offset);
  ra8_err_t (*validate)(void*               ctx,
                        void*               transaction_state,
                        fw_fs_validate_fn_t validator,
                        void*               validator_ctx);
  ra8_err_t (*commit)(void* ctx, void* transaction_state, bool* out_published);
  ra8_err_t (*abort)(void* ctx, void* transaction_state);
};

/**
 * @brief Bind segregated vtables and one context into a complete facade.
 * @details Namespace and stream capabilities/interfaces are mandatory. Optional
 *          operation pointers must agree with their advertised capability bits.
 */
[[nodiscard]] ra8_err_t fw_fs_bind(fw_fs_t*                         out,
                                   const fw_fs_namespace_iface_t*   namespace_iface,
                                   const fw_fs_stream_iface_t*      stream_iface,
                                   const fw_fs_transaction_iface_t* transaction_iface,
                                   void*                            ctx,
                                   const fw_fs_caps_t*              caps);

#ifdef __cplusplus
}
#endif
