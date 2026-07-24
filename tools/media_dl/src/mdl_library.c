/*
 * Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */
/**
 * @file mdl_library.c
 * @brief Enumerate tracked series and delete a series tree (host dirent/nftw).
 */
#include "mdl_library.h"

#include <dirent.h>
#include <ftw.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "ra8_attributes.h"

/** @brief Bounds for the library walk (defensive caps, not real limits). */
typedef enum : uint32_t {
  k_lib_max_series   = 100000U, /**< Ceiling on directory entries scanned. */
  k_lib_nftw_max_fds = 16,      /**< File descriptors nftw may keep open.  */
} mdl_library_bound_t;

/** @brief The state-file basename that marks a directory as a tracked series. */
RA8_INTERNAL static const char* state_basename(void)
{
  return ".mdl_state";
}

/** @brief True when `path` names an existing regular file. */
RA8_INTERNAL static bool is_file(const char* path)
{
  struct stat st;
  return (stat(path, &st) == 0) && S_ISREG(st.st_mode);
}

/** @brief True when directory entry `name` is neither "." nor "..". */
RA8_INTERNAL static bool is_real_entry(const char* name)
{
  return (strcmp(name, ".") != 0) && (strcmp(name, "..") != 0);
}

/** @brief Visit one candidate entry; false to stop the walk (rc set in *rc). */
RA8_INTERNAL static bool
visit_entry(const char* out_dir, const char* name, mdl_library_fn cb, void* ctx, ra8_err_t* rc)
{
  char      series[PATH_MAX];
  char      state[PATH_MAX];
  const int sn = snprintf(series, sizeof(series), "%s/%s", out_dir, name);
  const int tn = snprintf(state, sizeof(state), "%s/%s/%s", out_dir, name, state_basename());
  if ((sn < 0) || ((size_t)sn >= sizeof(series)) || (tn < 0) || ((size_t)tn >= sizeof(state))) {
    return true; /* path too long: skip, keep walking */
  }
  struct stat st;
  if ((stat(series, &st) != 0) || !S_ISDIR(st.st_mode) || !is_file(state)) {
    return true; /* not a tracked-series directory */
  }
  *rc = cb(series, state, ctx);
  return (*rc == k_ra8_ok);
}

ra8_err_t mdl_library_for_each(const char* out_dir, mdl_library_fn cb, void* ctx)
{
  if ((out_dir == nullptr) || (cb == nullptr)) {
    return k_ra8_err_invalid_arg;
  }
  DIR* dir = opendir(out_dir);
  if (dir == nullptr) {
    return k_ra8_ok; /* an absent library tracks nothing */
  }
  ra8_err_t            rc = k_ra8_ok;
  const struct dirent* ent;
  uint32_t             seen = 0U;
  while (((ent = readdir(dir)) != nullptr) && (seen < (uint32_t)k_lib_max_series)) {
    ++seen;
    if (!is_real_entry(ent->d_name)) {
      continue;
    }
    if (!visit_entry(out_dir, ent->d_name, cb, ctx, &rc)) {
      break; /* callback asked to stop */
    }
  }
  (void)closedir(dir);
  return rc;
}

/** @brief nftw() post-order callback: remove each visited path. */
RA8_INTERNAL static int
rm_entry(const char* path, const struct stat* sb, int typeflag, struct FTW* ftwbuf)
{
  (void)sb;
  (void)typeflag;
  (void)ftwbuf;
  return remove(path);
}

ra8_err_t mdl_library_remove_tree(const char* dir)
{
  if ((dir == nullptr) || (dir[0] == '\0')) {
    return k_ra8_err_invalid_arg;
  }
  struct stat st;
  if (stat(dir, &st) != 0) {
    return k_ra8_ok; /* already gone */
  }
  /* FTW_DEPTH: children before parents; FTW_PHYS: never follow a symlink out. */
  if (nftw(dir, rm_entry, (int)k_lib_nftw_max_fds, FTW_DEPTH | FTW_PHYS) != 0) {
    return k_ra8_fail;
  }
  return k_ra8_ok;
}
