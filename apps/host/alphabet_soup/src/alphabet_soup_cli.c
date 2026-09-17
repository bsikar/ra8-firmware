/**
 * @file alphabet_soup_cli.c
 * @brief Bounded POSIX file ingestion for the Alphabet Soup host CLI.
 * @details Splits host paths, opens them through the portable filesystem
 * facade, and reads source bytes into caller-owned bounded buffers.
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <string.h>

#include "alphabet_soup.h"
#include "alphabet_soup_cli_internal.h"
#include "fw_if_fs.h"
#include "fw_if_fs_posix.h"

/** @brief Buffer sizes and limits for host CLI file ingestion. */
typedef enum : uint32_t {
  k_cli_file_work_capacity   = 64U,   /**< File handle workspace bytes. */
  k_cli_read_chunk_bytes     = 1024U, /**< Bounded read chunk bytes.    */
  k_cli_path_buffer_capacity = 512U,  /**< Host path buffer bytes.      */
} alphabet_soup_cli_limit_t;

RA8_PRIV ra8_err_t priv_alphabet_soup_split_path(const char* filepath,
                                                 char*       out_root,
                                                 size_t      root_cap,
                                                 char*       out_leaf,
                                                 size_t      leaf_cap)
{
  if ((filepath == nullptr) || (out_root == nullptr) || (out_leaf == nullptr)) {
    return k_ra8_err_null_ptr;
  }
  const size_t len = strlen(filepath);
  if ((len == 0U) || (len >= root_cap) || ((len + 1U) >= leaf_cap)) {
    return k_ra8_err_invalid_size;
  }
  const char* last_slash = strrchr(filepath, '/');
  if (last_slash == nullptr) {
    out_root[0] = '.';
    out_root[1] = '\0';
    out_leaf[0] = '/';
    (void)memcpy(&out_leaf[1], filepath, len + 1U);
  } else if (last_slash == filepath) {
    out_root[0] = '/';
    out_root[1] = '\0';
    (void)memcpy(out_leaf, last_slash, len + 1U);
  } else {
    const size_t dir_len = len - strlen(last_slash);
    if (dir_len >= root_cap) {
      return k_ra8_err_invalid_size;
    }
    (void)memcpy(out_root, filepath, dir_len);
    out_root[dir_len] = '\0';
    (void)memcpy(out_leaf, last_slash, len - dir_len + 1U);
  }
  return k_ra8_ok;
}

RA8_PRIV ra8_err_t priv_alphabet_soup_read_all(fw_fs_file_t* file,
                                               uint8_t*      buffer,
                                               uint32_t      capacity,
                                               uint32_t*     out_size)
{
  if ((file == nullptr) || (buffer == nullptr) || (out_size == nullptr)) {
    return k_ra8_err_null_ptr;
  }
  uint32_t total       = 0U;
  uint32_t chunk_bytes = 0U;
  for (uint32_t step = 0U;
       step < (((uint32_t)k_soup_max_file_capacity / (uint32_t)k_cli_read_chunk_bytes) + 2U);
       ++step) {
    chunk_bytes        = 0U;
    uint32_t remaining = capacity - total;
    if (remaining == 0U) {
      return k_ra8_err_invalid_size;
    }
    const uint32_t read_limit =
      (remaining < k_cli_read_chunk_bytes) ? remaining : k_cli_read_chunk_bytes;
    const ra8_err_t err = fw_fs_read(file, &buffer[total], read_limit, &chunk_bytes);
    if (err != k_ra8_ok) {
      return err;
    }
    if (chunk_bytes == 0U) {
      break;
    }
    total += chunk_bytes;
  }
  *out_size = total;
  return k_ra8_ok;
}

RA8_PRIV ra8_err_t priv_alphabet_soup_load_file_contents(const char* filepath,
                                                         uint8_t*    out_buf,
                                                         uint32_t    buf_cap,
                                                         uint32_t*   out_len)
{
  char root_dir[k_cli_path_buffer_capacity]      = {};
  char portable_path[k_cli_path_buffer_capacity] = {};
  if (priv_alphabet_soup_split_path(filepath,
                                    root_dir,
                                    sizeof(root_dir),
                                    portable_path,
                                    sizeof(portable_path)) != k_ra8_ok) {
    return k_ra8_err_invalid_arg;
  }
  fw_fs_t                 fs          = {};
  fw_fs_posix_state_t     posix_state = {.root_fd = -1};
  const fw_fs_posix_cfg_t cfg         = {.root_path = root_dir, .removable_media = false};
  if (fw_fs_posix_init(&fs, &posix_state, &cfg) != k_ra8_ok) {
    return k_ra8_fail;
  }
  alignas(max_align_t) uint8_t file_work[k_cli_file_work_capacity] = {};
  fw_fs_file_t                 file                                = {};
  if (fw_fs_open(&fs.streams,
                 portable_path,
                 k_fw_fs_open_read,
                 &file,
                 file_work,
                 sizeof(file_work)) != k_ra8_ok) {
    (void)fw_fs_posix_deinit(&posix_state);
    return k_ra8_err_not_found;
  }
  const ra8_err_t err = priv_alphabet_soup_read_all(&file, out_buf, buf_cap, out_len);
  (void)fw_fs_close(&file);
  (void)fw_fs_posix_deinit(&posix_state);
  return err;
}
