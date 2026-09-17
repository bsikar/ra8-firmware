/**
 * @file ra8_c6_model_mdl_fault_internal.h
 * @brief Private malformed-media response seam for the C6 model.
 * @details Keeps protocol fault construction separate from the transport model
 * while exposing one synchronous, test-target-private mutation operation.
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_hosted_rpc.pb-c.h"
#include "ra8_attributes.h"
#include "ra8_c6_model.h"

/**
 * @brief Consume and apply one malformed media-response request.
 * @details Mutates either the outer CustomRpc envelope or the generated inner
 * response, preserving decoder ownership while repacking bounded bytes.
 * @param[in,out] fault Pending fault slot, cleared on a matching response.
 * @param[in,out] outer Initialized outer response envelope.
 * @param[in,out] body Initialized CustomRpc response body owned by @p outer.
 * @param[in,out] response Writable inner-response storage.
 * @param[in] response_cap Capacity of @p response.
 * @param[in,out] response_len Valid inner bytes before and after mutation.
 * @return Nothing.
 * @pre Every pointer is non-null and `*response_len <= response_cap`.
 * @pre @p body initially references @p response for `*response_len` bytes.
 * @post A compatible non-none fault is consumed exactly once.
 * @post A fault for a later operation remains pending.
 * @post Any retained inner payload remains packed within @p response_cap.
 * @note Test-only, synchronous, and not thread-safe.
 * @since 0.1.0
 */
RA8_PRIV void priv_c6_model_mdl_fault_apply(ra8_c6_model_mdl_fault_t* fault,
                                            Rpc*                      outer,
                                            RpcRespCustomRpc*         body,
                                            uint8_t*                  response,
                                            size_t                    response_cap,
                                            size_t*                   response_len);
