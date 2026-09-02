/**
 * @file alphabet_soup_cli_internal.h
 * @brief Private host-file helpers for the Alphabet Soup CLI wrapper.
 * @details Declares the path, stream, and bounded-ingestion seams shared only
 * by the CLI implementation and its host tests.
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

#include "fw_if_fs_types.h"
#include "ra8_attributes.h"
#include "ra8_err.h"

RA8_PRIV [[nodiscard]] ra8_err_t priv_alphabet_soup_split_path(const char* filepath,
                                                               char*       out_root,
                                                               size_t      root_cap,
                                                               char*       out_leaf,
                                                               size_t      leaf_cap);

RA8_PRIV [[nodiscard]] ra8_err_t priv_alphabet_soup_read_all(fw_fs_file_t* file,
                                                             uint8_t*      buffer,
                                                             uint32_t      capacity,
                                                             uint32_t*     out_size);

RA8_PRIV [[nodiscard]] ra8_err_t priv_alphabet_soup_load_file_contents(const char* filepath,
                                                                       uint8_t*    out_buf,
                                                                       uint32_t    buf_cap,
                                                                       uint32_t*   out_len);
