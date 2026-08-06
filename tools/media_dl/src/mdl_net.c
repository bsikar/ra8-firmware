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
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */
#include "mdl_net.h"

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "ra8_attributes.h"

/** @brief Zero a caller-supplied response block so early returns leave it clean. */
RA8_INTERNAL static void resp_reset(mdl_net_resp_t* resp)
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
  resp_reset(resp);
  if (net == nullptr) {
    return k_ra8_err_invalid_arg;
  }
  if ((net->vtable == nullptr) || (url == nullptr) || (req == nullptr) || (buf == nullptr) ||
      (cap == 0U)) {
    return k_ra8_err_invalid_arg;
  }
  return net->vtable->get_buf(net->ctx, url, req, buf, cap, out_len, resp);
}

ra8_err_t mdl_net_get_file(mdl_net_iface_t*     net,
                           const char*          url,
                           const mdl_net_req_t* req,
                           const char*          out_path,
                           size_t*              out_len,
                           mdl_net_resp_t*      resp)
{
  resp_reset(resp);
  if (net == nullptr) {
    return k_ra8_err_invalid_arg;
  }
  if ((net->vtable == nullptr) || (url == nullptr) || (req == nullptr) || (out_path == nullptr)) {
    return k_ra8_err_invalid_arg;
  }
  return net->vtable->get_file(net->ctx, url, req, out_path, out_len, resp);
}

void mdl_net_destroy(mdl_net_iface_t* net)
{
  if (net == nullptr) {
    return;
  }
  if ((net->vtable != nullptr) && (net->vtable->destroy != nullptr)) {
    net->vtable->destroy(net->ctx);
  }
  /* net is arena-allocated; lifetime is managed by arena reset. */
}
