/**
 * @file ra8_mdl_service.h
 * @brief Public integration contract for the ESP32-C6 media RPC component
 *
 * @par Tag
 * [Ring 4 / PAL] {World: NS}
 *
 * @details The pinned ESP-hosted patch calls the synchronous hook by its
 * upstream ABI name. The first-party build script also verifies the component
 * marker as a strong text symbol after link. Publishing both declarations
 * here makes those externally linked entry points explicit; neither is a
 * module-private helper and neither may be renamed to the `priv_` vocabulary.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Return the linked first-party media component ABI version
 * @details The post-link build assertion requires this exact strong symbol.
 * @return Stable positive ABI version checked by the component itself.
 * @retval 1 First version of the concrete media service component.
 * @pre The first-party component object is strongly linked into the image.
 * @pre The upstream weak fallback has not replaced this component.
 * @post No global or peripheral state is modified.
 * @post Repeated calls return the same version for one image.
 * @note Thread-safe and allocation-free.
 * @since 0.1.0
 */
uint32_t ra8_mdl_service_component_abi(void);

/**
 * @brief Handle one synchronous ESP-hosted CustomRpc media operation
 * @details Dispatches a bounded generated-protobuf request through the
 * first-party media service on ESP-hosted's serialized control path.
 * @param[in] message_id Stable media RPC operation identifier.
 * @param[in] request Packed generated inner protobuf.
 * @param[in] request_len Valid bytes in @p request.
 * @param[out] response Caller-owned inner protobuf response buffer.
 * @param[in] response_cap Capacity of @p response.
 * @param[out] response_len Packed response byte count.
 * @return ESP-IDF status for the patched ESP-hosted dispatcher.
 * @retval ESP_OK A valid inner response was produced.
 * @retval ESP_FAIL Component initialisation or dispatch failed.
 * @pre ESP-hosted invokes the hook on its serialized control path.
 * @pre Request and response spans are valid and do not overlap.
 * @post Success sets @p response_len no greater than @p response_cap.
 * @post Failure does not claim a successful CustomRpc response.
 * @note Not thread-safe outside the serialized ESP-hosted control task.
 * @since 0.1.0
 */
esp_err_t esp_hosted_custom_rpc_sync_handler(uint32_t       message_id,
                                             const uint8_t* request,
                                             size_t         request_len,
                                             uint8_t*       response,
                                             size_t         response_cap,
                                             size_t*        response_len);

#ifdef __cplusplus
}
#endif
