/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Brighton Sikarskie */

#ifndef RA8_OTA_PARSE_ABI_H
#define RA8_OTA_PARSE_ABI_H

/*
 * Narrow declaration surface for the Zig-owned parser archive. The broader
 * ra8_ota_internal.h remains the shared cross-TU header for C orchestration.
 */
#include "ra8_ota_internal.h"

ra8_err_t priv_ota_validate_cfg(const ra8_ota_cfg_t* cfg);
ra8_err_t priv_ota_manifest_decode(const char* json, ra8_ota_manifest_t* out);
ra8_err_t priv_ota_json_u32(const char* json, const char* key, uint32_t* out_v);
bool priv_ota_char_in_range(char c, char lo, char hi);
bool priv_ota_download_state_invalid(uint32_t state_idle_val,
                                     uint32_t state_downloading_val,
                                     uint32_t state);

#endif /* RA8_OTA_PARSE_ABI_H */
