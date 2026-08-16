/**
 * @file mdl_net_c6link.c
 * @brief Digest-verified media network backend over C6link.
 * @details Adapts one bounded C6 transfer to either a caller text buffer or the
 * downloader's reset/write body sink without owning storage or hash memory.
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "mdl_net_c6link.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "mdl_net_internal.h"
#include "ra8_attributes.h"

/** @brief Output destination selected for one synchronous adapter call. */
typedef enum : uint8_t {
  k_mdl_c6_output_buffer = 1U, /**< Bounded NUL-terminated caller buffer. */
  k_mdl_c6_output_sink   = 2U, /**< Injected streaming body sink.         */
} mdl_c6_output_kind_t;

/** @brief Transaction state that turns one C6 body into a network response. */
typedef struct {
  mdl_c6_output_kind_t kind;   /**< Selected output union member. */
  char*                buffer; /**< Text buffer or null.          */
  size_t               cap;    /**< Text-buffer capacity.         */
  mdl_net_body_sink_t* sink;   /**< Streaming sink or null.       */
  size_t               length; /**< Bytes accepted so far.        */
} mdl_c6_output_t;

static_assert((uint32_t)k_mdl_retry_after_max == (uint32_t)k_ra8_mdl_retry_after_max,
              "C6 and downloader Retry-After capacities must match");
static_assert((uint32_t)k_mdl_etag_max == (uint32_t)k_ra8_mdl_etag_max,
              "C6 and downloader ETag capacities must match");
static_assert((uint32_t)k_mdl_last_mod_max == (uint32_t)k_ra8_mdl_http_date_max,
              "C6 and downloader HTTP-date capacities must match");
static_assert((uint32_t)k_mdl_content_type_max == (uint32_t)k_ra8_mdl_content_type_max,
              "C6 and downloader Content-Type capacities must match");

/**
 * @brief Begin the local half of one transfer transaction.
 * @details Validates the coordinator placeholder and resets only the selected
 *          local output shape before the first remote body chunk arrives.
 * @param[in,out] ctx Bound ::mdl_c6_output_t.
 * @param[in] destination Nonempty coordinator placeholder.
 * @return Local readiness status.
 * @retval k_ra8_ok Output state was reset for a new body.
 * @retval k_ra8_err_invalid_arg Context or destination is invalid.
 * @pre @p ctx points to exclusively owned adapter-call state.
 * @pre Streaming sinks were reset by ::mdl_net_get_body before dispatch.
 * @post Success sets the accepted length to zero.
 * @post Buffer output starts as an empty C string.
 * @note Does not reset a streaming sink a second time.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_c6_output_begin(void* ctx, const char* destination)
{
  mdl_c6_output_t* output = (mdl_c6_output_t*)ctx;
  if ((output == nullptr) || (destination == nullptr) || (destination[0] == '\0')) {
    return k_ra8_err_invalid_arg;
  }
  output->length = 0U;
  if (output->kind == k_mdl_c6_output_buffer) {
    output->buffer[0] = '\0';
  }
  return k_ra8_ok;
}

/**
 * @brief Append one verified-order C6 chunk to the selected output.
 * @details Copies into the bounded text destination or delegates to the
 *          injected streaming sink while enforcing exact progress.
 * @param[in,out] ctx Bound ::mdl_c6_output_t.
 * @param[in] data Remote body bytes.
 * @param[in] len Valid byte count.
 * @param[out] written Exact bytes accepted.
 * @return Sink status.
 * @retval k_ra8_ok Every byte was accepted.
 * @retval k_ra8_err_no_mem Text output lacks room for bytes plus trailing NUL.
 * @retval k_ra8_err_invalid_size A sink reported impossible progress.
 * @pre All pointers are non-null and @p data spans @p len bytes.
 * @pre Chunks arrive in the order enforced by the transfer coordinator.
 * @post Success advances output length by exactly @p len.
 * @post Failure reports zero accepted bytes to the coordinator.
 * @note Synchronous and not thread-safe for a shared output.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t
internal_c6_output_write(void* ctx, const uint8_t* data, uint16_t len, uint16_t* written)
{
  mdl_c6_output_t* output = (mdl_c6_output_t*)ctx;
  *written                = 0U;
  if (output->kind == k_mdl_c6_output_buffer) {
    if ((output->length >= output->cap) || ((size_t)len > (output->cap - output->length - 1U))) {
      return k_ra8_err_no_mem;
    }
    (void)memcpy(&output->buffer[output->length], data, len);
  } else {
    uint32_t        accepted    = 0U;
    const ra8_err_t sink_result = output->sink->write(output->sink->ctx, data, len, &accepted);
    if (sink_result != k_ra8_ok) {
      return sink_result;
    }
    if (accepted != len) {
      return k_ra8_err_invalid_size;
    }
  }
  output->length += len;
  *written = len;
  return k_ra8_ok;
}

/**
 * @brief Publish the completed response to the caller-visible output.
 * @details Terminates bounded text output after digest validation; streaming
 *          sinks are already caller-visible and require no extra operation.
 * @param[in,out] ctx Bound ::mdl_c6_output_t.
 * @return Publication status.
 * @retval k_ra8_ok Output is complete and visible.
 * @pre Transport length and digest validation have completed.
 * @pre Text output retains at least one byte for its terminator.
 * @post Buffer output is NUL-terminated at the exact body length.
 * @post Streaming output is unchanged because its sink owns publication.
 * @note Synchronous and allocation-free.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_c6_output_commit(void* ctx)
{
  mdl_c6_output_t* output = (mdl_c6_output_t*)ctx;
  if (output->kind == k_mdl_c6_output_buffer) {
    output->buffer[output->length] = '\0';
  }
  return k_ra8_ok;
}

/**
 * @brief Discard caller-visible partial bytes after a failed transfer.
 * @details Clears bounded text directly or delegates reset to the injected
 *          body sink, preserving cleanup failure for coordinator precedence.
 * @param[in,out] ctx Bound ::mdl_c6_output_t.
 * @return Cleanup status.
 * @retval k_ra8_ok Partial output was cleared.
 * @pre @p ctx points to live adapter-call state.
 * @pre A prior begin succeeded.
 * @post Buffer output is empty; streaming output was reset.
 * @post Accepted length is zero on successful cleanup.
 * @note A sink reset failure is propagated only when no earlier cause wins.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_c6_output_abort(void* ctx)
{
  mdl_c6_output_t* output = (mdl_c6_output_t*)ctx;
  ra8_err_t        result = k_ra8_ok;
  if (output->kind == k_mdl_c6_output_buffer) {
    output->buffer[0] = '\0';
  } else {
    result = output->sink->reset(output->sink->ctx);
  }
  if (result == k_ra8_ok) {
    output->length = 0U;
  }
  return result;
}

/**
 * @brief Execute one digest-verified policy-preserving C6 GET.
 * @details Binds the selected output as a transfer storage transaction, runs
 *          the C6 coordinator, and copies only metadata proven by the wire.
 * @param[in,out] backend Bound C6 network state.
 * @param[in] url HTTPS source URL.
 * @param[in] req Request policy to validate.
 * @param[in,out] output Selected caller output transaction.
 * @param[out] out_len Optional completed byte length.
 * @param[out] resp Optional observed response metadata.
 * @return Transfer, sink, or digest status.
 * @retval k_ra8_ok The complete verified body has HTTP status below 400.
 * @retval k_ra8_err_busy HTTP status 429 or 503 requires backoff.
 * @retval k_ra8_err_not_found Another HTTP 4xx status was observed.
 * @retval k_ra8_fail An HTTP 5xx status was observed.
 * @pre Pointer arguments are non-null except optional outputs.
 * @pre The C6 link and SHA context are exclusively owned.
 * @post Success reports the exact terminal HTTP status and selected headers.
 * @post Failure leaves partial output cleared by the transaction abort.
 * @note Classification is shared exactly with the curl backend.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_c6_get(mdl_net_c6link_t*    backend,
                                              const char*          url,
                                              const mdl_net_req_t* req,
                                              mdl_c6_output_t*     output,
                                              size_t*              out_len,
                                              mdl_net_resp_t*      resp)
{
  const ra8_mdl_transfer_config_t config = {
    .storage     = {.begin    = internal_c6_output_begin,
                    .write    = internal_c6_output_write,
                    .validate = nullptr,
                    .commit   = internal_c6_output_commit,
                    .abort    = internal_c6_output_abort,
                    .ctx      = output},
    .sha256      = backend->sha256,
    .format      = k_ra8_mdl_format_loose,
    .http        = {.user_agent        = req->user_agent,
                    .referer           = req->referer,
                    .if_none_match     = req->if_none_match,
                    .if_modified_since = req->if_modified_since,
                    .timeout_ms        = req->timeout_ms},
    .chunk_bytes = backend->chunk_bytes,
    .max_chunks  = backend->max_chunks,
  };
  ra8_mdl_transfer_result_t transfer = {};
  const ra8_err_t           result =
    ra8_c6link_mdl_transfer(backend->link, url, "c6-response", &config, &transfer);
  if (result == k_ra8_ok) {
    if (transfer.bytes_stored > SIZE_MAX) {
      return k_ra8_err_invalid_size;
    }
    if (resp != nullptr) {
      resp->status = transfer.response.status;
      (void)memcpy(resp->retry_after, transfer.response.retry_after, sizeof(resp->retry_after));
      (void)memcpy(resp->etag, transfer.response.etag, sizeof(resp->etag));
      (void)memcpy(resp->last_modified,
                   transfer.response.last_modified,
                   sizeof(resp->last_modified));
      (void)memcpy(resp->content_type, transfer.response.content_type, sizeof(resp->content_type));
    }
    const ra8_err_t classified = priv_mdl_net_classify_http(transfer.response.status);
    if (classified != k_ra8_ok) {
      (void)internal_c6_output_abort(output);
      return classified;
    }
    if (out_len != nullptr) {
      *out_len = (size_t)transfer.bytes_stored;
    }
  }
  return result;
}

/** @copydoc mdl_net_vtable_t::get_buf */
RA8_INTERNAL static ra8_err_t internal_c6_get_buf(void*                ctx,
                                                  const char*          url,
                                                  const mdl_net_req_t* req,
                                                  char*                buf,
                                                  size_t               cap,
                                                  size_t*              out_len,
                                                  mdl_net_resp_t*      resp)
{
  mdl_c6_output_t output = {
    .kind   = k_mdl_c6_output_buffer,
    .buffer = buf,
    .cap    = cap,
  };
  return internal_c6_get((mdl_net_c6link_t*)ctx, url, req, &output, out_len, resp);
}

/** @copydoc mdl_net_vtable_t::get_body */
RA8_INTERNAL static ra8_err_t internal_c6_get_body(void*                ctx,
                                                   const char*          url,
                                                   const mdl_net_req_t* req,
                                                   mdl_net_body_sink_t* sink,
                                                   size_t*              out_len,
                                                   mdl_net_resp_t*      resp)
{
  mdl_c6_output_t output = {
    .kind = k_mdl_c6_output_sink,
    .sink = sink,
  };
  return internal_c6_get((mdl_net_c6link_t*)ctx, url, req, &output, out_len, resp);
}

/** @copydoc mdl_net_vtable_t::destroy */
RA8_INTERNAL static void internal_c6_destroy(void* ctx)
{
  (void)ctx;
}

ra8_err_t mdl_net_c6link_init(mdl_net_iface_t*              net,
                              mdl_net_c6link_t*             backend,
                              ra8_c6link_t*                 link,
                              const ra8_mdl_sha256_iface_t* sha256,
                              uint16_t                      chunk_bytes,
                              uint32_t                      max_chunks)
{
  if ((net == nullptr) || (backend == nullptr)) {
    return k_ra8_err_null_ptr;
  }
  *net     = (mdl_net_iface_t){};
  *backend = (mdl_net_c6link_t){};
  if ((link == nullptr) || (sha256 == nullptr) || (sha256->init == nullptr) ||
      (sha256->update == nullptr) || (sha256->final == nullptr) || (sha256->ctx == nullptr)) {
    return k_ra8_err_null_ptr;
  }
  if ((chunk_bytes == 0U) || (chunk_bytes > k_ra8_mdl_chunk_data_max) || (max_chunks == 0U)) {
    return k_ra8_err_invalid_size;
  }
  *backend = (mdl_net_c6link_t){
    .link        = link,
    .sha256      = *sha256,
    .chunk_bytes = chunk_bytes,
    .max_chunks  = max_chunks,
  };
  /** @brief Immutable method table for caller-owned C6 backend state. */
  static const mdl_net_vtable_t k_c6_vtable = {
    .get_buf  = internal_c6_get_buf,
    .get_body = internal_c6_get_body,
    .destroy  = internal_c6_destroy,
  };
  *net = (mdl_net_iface_t){.vtable = &k_c6_vtable, .ctx = backend};
  return k_ra8_ok;
}
