/*
 * Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */
/**
 * @file mdl_net_curl.c
 * @brief libcurl host backend for the mdl_net streaming-GET seam.
 *
 * @details
 * Host-only implementation of @ref mdl_net.h. libcurl handles TLS, redirects,
 * gzip and the connection pool -- exactly the parts we do not want to hand-roll
 * before the on-device NetX/Mbed TLS stack exists. One easy handle is reused
 * across requests so the cookie jar and keep-alive connections persist.
 */
#include <curl/curl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mdl_net.h"

/** @brief Backend tunables. */
typedef enum : uint16_t {
  k_curl_max_redirects  = 8,   /**< Redirect hops to follow.               */
  k_http_status_err_min = 400, /**< First HTTP status treated as an error. */
} mdl_curl_limits_t;

/** @brief Concrete libcurl network interface. */
struct mdl_net_iface {
  CURL* curl; /**< Reused easy handle: persistent pool + cookie jar. */
};

/** @brief Bounded-buffer sink state for a page fetch. */
typedef struct {
  char*  buf;      /**< Destination buffer.              */
  size_t cap;      /**< Capacity in bytes.               */
  size_t len;      /**< Bytes written so far.            */
  bool   overflow; /**< Set once the body exceeds `cap`. */
} buf_sink_t;

/** @brief libcurl write callback: append into a bounded buffer. */
/* cppcheck-suppress constParameterCallback ; libcurl WRITEFUNCTION ABI is char* */
static size_t on_buf_write(char* data, size_t size, size_t nmemb, void* user)
{
  buf_sink_t*  sink  = (buf_sink_t*)user;
  const size_t bytes = size * nmemb;
  if (sink == nullptr) {
    return 0U;
  }
  if ((sink->len + bytes) > sink->cap) {
    sink->overflow = true;
    return 0U; /* Signals libcurl to abort the transfer. */
  }
  memcpy(sink->buf + sink->len, data, bytes);
  sink->len += bytes;
  return nmemb;
}

/** @brief libcurl write callback: append to an open FILE*. */
/* cppcheck-suppress constParameterCallback ; libcurl WRITEFUNCTION ABI is char* */
static size_t on_file_write(char* data, size_t size, size_t nmemb, void* user)
{
  FILE* fp = (FILE*)user;
  if (fp == nullptr) {
    return 0U;
  }
  return fwrite(data, size, nmemb, fp);
}

mdl_net_iface_t* mdl_net_curl_create(void)
{
  static bool s_global_ready = false;
  if (!s_global_ready) {
    if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) {
      return nullptr;
    }
    s_global_ready = true;
  }

  mdl_net_iface_t* net = (mdl_net_iface_t*)calloc(1U, sizeof(*net));
  if (net == nullptr) {
    return nullptr;
  }
  net->curl = curl_easy_init();
  if (net->curl == nullptr) {
    free(net);
    return nullptr;
  }

  /* Options that hold for the life of the handle. */
  curl_easy_setopt(net->curl, CURLOPT_FOLLOWLOCATION, 1L);
  curl_easy_setopt(net->curl, CURLOPT_MAXREDIRS, (long)k_curl_max_redirects);
  curl_easy_setopt(net->curl, CURLOPT_ACCEPT_ENCODING, ""); /* all supported */
  curl_easy_setopt(net->curl, CURLOPT_COOKIEFILE, "");      /* enable jar    */
  curl_easy_setopt(net->curl, CURLOPT_NOSIGNAL, 1L);
  return net;
}

void mdl_net_curl_destroy(mdl_net_iface_t* net)
{
  if (net == nullptr) {
    return;
  }
  if (net->curl != nullptr) {
    curl_easy_cleanup(net->curl);
  }
  free(net);
}

/** @brief Apply the per-request options shared by both fetch paths. */
static void apply_req(CURL* curl, const char* url, const mdl_net_req_t* req)
{
  curl_easy_setopt(curl, CURLOPT_URL, url);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, (long)req->timeout_ms);
  if (req->user_agent != nullptr) {
    curl_easy_setopt(curl, CURLOPT_USERAGENT, req->user_agent);
  }
  /* CURLOPT_REFERER with NULL clears any prior value -- exactly what we want. */
  curl_easy_setopt(curl, CURLOPT_REFERER, req->referer);
}

/** @brief Map a completed libcurl transfer to an ra8_err_t. */
static ra8_err_t classify(CURL* curl, CURLcode code, bool overflow)
{
  if (overflow) {
    return k_ra8_err_no_mem;
  }
  if (code == CURLE_OPERATION_TIMEDOUT) {
    return k_ra8_err_timeout;
  }
  if (code != CURLE_OK) {
    return k_ra8_fail;
  }
  long status = 0;
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
  if (status >= (long)k_http_status_err_min) {
    return k_ra8_fail;
  }
  return k_ra8_ok;
}

ra8_err_t mdl_net_get_buf(mdl_net_iface_t*     net,
                          const char*          url,
                          const mdl_net_req_t* req,
                          char*                buf,
                          size_t               cap,
                          size_t*              out_len)
{
  if ((net == nullptr) || (url == nullptr) || (req == nullptr) || (buf == nullptr) || (cap == 0U)) {
    return k_ra8_err_invalid_arg;
  }

  buf_sink_t sink = {.buf = buf, .cap = cap, .len = 0U, .overflow = false};
  apply_req(net->curl, url, req);
  curl_easy_setopt(net->curl, CURLOPT_WRITEFUNCTION, on_buf_write);
  curl_easy_setopt(net->curl, CURLOPT_WRITEDATA, &sink);

  const CURLcode  code = curl_easy_perform(net->curl);
  const ra8_err_t rc   = classify(net->curl, code, sink.overflow);
  if (rc != k_ra8_ok) {
    return rc;
  }

  if (sink.len < cap) {
    buf[sink.len] = '\0'; /* NUL-terminate when room remains. */
  }
  if (out_len != nullptr) {
    *out_len = sink.len;
  }
  return k_ra8_ok;
}

ra8_err_t mdl_net_get_file(mdl_net_iface_t*     net,
                           const char*          url,
                           const mdl_net_req_t* req,
                           const char*          out_path,
                           size_t*              out_len)
{
  if ((net == nullptr) || (url == nullptr) || (req == nullptr) || (out_path == nullptr)) {
    return k_ra8_err_invalid_arg;
  }

  FILE* fp = fopen(out_path, "wb");
  if (fp == nullptr) {
    return k_ra8_fail;
  }

  apply_req(net->curl, url, req);
  curl_easy_setopt(net->curl, CURLOPT_WRITEFUNCTION, on_file_write);
  curl_easy_setopt(net->curl, CURLOPT_WRITEDATA, fp);

  const CURLcode  code = curl_easy_perform(net->curl);
  const ra8_err_t rc   = classify(net->curl, code, false);

  const long fsize = ftell(fp);
  if (fclose(fp) != 0) {
    return k_ra8_fail;
  }
  if (rc != k_ra8_ok) {
    (void)remove(out_path); /* Do not leave a truncated image behind. */
    return rc;
  }
  if (out_len != nullptr) {
    *out_len = (fsize < 0) ? 0U : (size_t)fsize;
  }
  return k_ra8_ok;
}
