/**
 * @file esp32_c6_http_model_internal.h
 * @brief Deterministic ESP-IDF stand-in surface for the C6 media adapter.
 * @details Publishes the host model the production ESP32-C6 HTTP backend is
 * linked against: its scripted response record, the retained client storage
 * behind the opaque ESP-IDF handle, and the reset entry point. Keeping the
 * stand-ins in their own translation unit leaves the vector suite holding
 * vectors only, and both stay inside the repository size cap.
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_idf_mdl_compat_internal.h"
#include "ra8_attributes.h"
#include "ra8_c6link_mdl.h"

/** @brief Fixed fixture capacities and HTTP values. */
typedef enum : uint16_t {
  k_c6_http_request_cap     = 256U,  /**< Packed request scratch.        */
  k_c6_http_response_cap    = 4608U, /**< Packed response scratch.       */
  k_c6_http_status_ok       = 200U,  /**< Canonical successful status.   */
  k_c6_http_status_low      = 99U,   /**< Below the HTTP status range.   */
  k_c6_http_status_redirect = 300U,  /**< Visible non-followed redirect. */
  k_c6_http_status_high     = 600U,  /**< Above the HTTP status range.   */
} c6_http_test_limits_t;

/** @brief Deterministic host model for the consumed ESP-IDF surface. */
typedef struct {
  const uint8_t* body;                                       /**< Response body.    */
  size_t         body_bytes;                                 /**< Body extent.      */
  size_t         cursor;                                     /**< Read position.    */
  int64_t        content_length;                             /**< Advertised size.  */
  int            status;                                     /**< HTTP status.      */
  bool           complete;                                   /**< Complete flag.    */
  bool           init_fail;                                  /**< Init fault.       */
  bool           set_url_fail;                               /**< URL fault.        */
  bool           set_header_fail;                            /**< Header fault.     */
  bool           set_timeout_fail;                           /**< Timeout fault.    */
  bool           open_fail;                                  /**< Open fault.       */
  bool           read_fail;                                  /**< Read fault.       */
  bool           read_oversize;                              /**< Oversize read.    */
  bool           sha_start_fail;                             /**< SHA init fault.   */
  bool           sha_update_fail;                            /**< SHA update fault. */
  bool           sha_finish_fail;                            /**< SHA final fault.  */
  uint32_t       init_calls;                                 /**< Init calls.       */
  uint32_t       close_calls;                                /**< Close calls.      */
  int            timeout_ms;                                 /**< Applied timeout.  */
  char           user_agent[k_ra8_mdl_user_agent_max];       /**< User-Agent.       */
  char           referer[k_ra8_mdl_referer_max];             /**< Referer.          */
  char           if_none_match[k_ra8_mdl_etag_max];          /**< ETag condition.   */
  char           if_modified_since[k_ra8_mdl_http_date_max]; /**< Date condition.   */
  const char*    retry_after;                                /**< Retry-After.      */
  const char*    etag;                                       /**< ETag.             */
  const char*    last_modified;                              /**< Last-Modified.    */
  const char*    content_type;                               /**< Content-Type.     */
  const char*    etag_key;                                   /**< ETag field name.  */
  bool           null_etag_value;                            /**< Valueless ETag.   */
} c6_http_model_t;

/** @brief Concrete storage behind the opaque ESP-IDF handle type. */
struct esp_http_client {
  const char*                url;           /**< Last URL accepted by the model. */
  esp_http_client_event_cb_t event_handler; /**< Configured event callback.      */
  void*                      user_data;     /**< Configured callback context.    */
};

/**
 * @brief Reach the one scripted ESP-IDF response model.
 * @details Exposes the process-lifetime singleton a vector scripts before a
 * call and asserts on afterwards.
 * @return Pointer to the model; never null.
 * @retval non-NULL The singleton, valid for the life of the test binary.
 * @pre None; the model exists before `main` runs.
 * @pre The caller reset it if it wants a clean slate.
 * @post No state is modified.
 * @post The same pointer is returned on every call.
 * @note Test-target-private; the host tests are single-threaded.
 * @since 0.1.0
 */
RA8_PRIV c6_http_model_t* priv_c6_http_model(void);

/**
 * @brief Reach the retained client storage behind the ESP-IDF handle.
 * @details Publishes the event callback and context the production adapter
 * registered, so a vector can drive that callback directly.
 * @return Pointer to the retained client; never null.
 * @retval non-NULL The singleton, valid for the life of the test binary.
 * @pre None; the storage exists before `main` runs.
 * @pre One-time production initialization has run before its fields are used.
 * @post No state is modified.
 * @post The same pointer is returned on every call.
 * @note Test-target-private; the host tests are single-threaded.
 * @since 0.1.0
 */
RA8_PRIV struct esp_http_client* priv_c6_http_client(void);

/**
 * @brief Reset the model to one successful deterministic response.
 * @details Clears every injected fault while retaining the production
 * service's independently owned one-time client state.
 * @param[in] body Borrowed response bytes.
 * @param[in] body_bytes Readable bytes at @p body.
 * @return Nothing.
 * @pre @p body is non-NULL when @p body_bytes is nonzero.
 * @pre No public handler call executes concurrently.
 * @post The next URL/open/read sequence starts at body offset zero.
 * @post Content-Length equals @p body_bytes and completeness defaults true.
 * @note Test-target-private; model mutation only.
 * @since 0.1.0
 */
RA8_PRIV void priv_c6_http_model_reset(const uint8_t* body, size_t body_bytes);
