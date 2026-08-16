/**
 * @file mdl_net.c
 * @brief Backend-agnostic dispatchers for the ::mdl_net_iface_t vtable seam.
 *
 * @details
 * The thin layer every network caller actually reaches: it validates the handle
 * and arguments, then forwards to the registered backend method with the
 * backend's opaque `ctx`. It pulls in no libcurl and no concrete backend, so it
 * links into the host unit tests alongside a scripted fake exactly as it links
 * into the production tool alongside the libcurl backend -- which is what makes
 * the argument-validation decisions here testable without a network.
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */
#include "mdl_net.h"

#include <stddef.h>
#include <string.h>

#include "mdl_net_internal.h"
#include "ra8_attributes.h"

/** @brief HTTP statuses that have downloader-specific handling. */
typedef enum : long {
  k_mdl_http_client_error = 400L, /**< First client-error status. */
  k_mdl_http_too_many     = 429L, /**< Too Many Requests.        */
  k_mdl_http_server_error = 500L, /**< First server-error status. */
  k_mdl_http_unavailable  = 503L, /**< Service Unavailable.       */
} mdl_http_status_t;

RA8_PRIV ra8_err_t priv_mdl_net_classify_http(long status)
{
  if ((status == k_mdl_http_too_many) || (status == k_mdl_http_unavailable)) {
    return k_ra8_err_busy;
  }
  if (status >= k_mdl_http_server_error) {
    return k_ra8_fail;
  }
  if (status >= k_mdl_http_client_error) {
    return k_ra8_err_not_found;
  }
  return k_ra8_ok;
}

/**
 * @brief Zero a caller-supplied response block so early returns leave it clean.
 * @details Accepts NULL so dispatcher validation paths can reset unconditionally.
 * @param[out] resp Optional response metadata block.
 * @return Nothing.
 * @pre @p resp is NULL or points to writable ::mdl_net_resp_t storage.
 * @pre The caller no longer needs the prior metadata.
 * @post A non-NULL response contains only zero bytes.
 * @post NULL input has no effect.
 * @note Not thread-safe when callers share @p resp.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_resp_reset(mdl_net_resp_t* resp)
{
  if (resp != nullptr) {
    memset(resp, 0, sizeof(*resp));
  }
}

ra8_err_t mdl_net_get_buf(mdl_net_iface_t*     net,
                          const char*          url,
                          const mdl_net_req_t* req,
                          char*                buf,
                          size_t               cap,
                          size_t*              out_len,
                          mdl_net_resp_t*      resp)
{
  internal_resp_reset(resp);
  if (net == nullptr) {
    return k_ra8_err_invalid_arg;
  }
  if ((net->vtable == nullptr) || (url == nullptr) || (req == nullptr) || (buf == nullptr) ||
      (cap == 0U)) {
    return k_ra8_err_invalid_arg;
  }
  return net->vtable->get_buf(net->ctx, url, req, buf, cap, out_len, resp);
}

ra8_err_t mdl_net_get_body(mdl_net_iface_t*     net,
                           const char*          url,
                           const mdl_net_req_t* req,
                           mdl_net_body_sink_t* sink,
                           size_t*              out_len,
                           mdl_net_resp_t*      resp)
{
  internal_resp_reset(resp);
  if (net == nullptr) {
    return k_ra8_err_invalid_arg;
  }
  if ((net->vtable == nullptr) || (net->vtable->get_body == nullptr) || (url == nullptr) ||
      (req == nullptr) || (sink == nullptr) || (sink->reset == nullptr) ||
      (sink->write == nullptr) || (sink->ctx == nullptr)) {
    return k_ra8_err_invalid_arg;
  }
  const ra8_err_t reset = sink->reset(sink->ctx);
  if (reset != k_ra8_ok) {
    return reset;
  }
  return net->vtable->get_body(net->ctx, url, req, sink, out_len, resp);
}

void mdl_net_destroy(mdl_net_iface_t* net)
{
  if (net == nullptr) {
    return;
  }
  if ((net->vtable != nullptr) && (net->vtable->destroy != nullptr)) {
    net->vtable->destroy(net->ctx);
  }
  *net = (mdl_net_iface_t){};
}
