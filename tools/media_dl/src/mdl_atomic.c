/**
 * @file mdl_atomic.c
 * @brief Implementation of the write-to-temp-then-rename helpers.
 *
 * @details
 * Three short functions with no state: the whole design lives in
 * @ref mdl_atomic.h. Deliberately free of any "write the destination anyway"
 * fallback -- the failure mode this file removes is precisely a writer
 * deciding it may open the destination when the careful path is inconvenient.
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */
#include "mdl_atomic.h"

#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "ra8_attributes.h"

/**
 * @var s_mdl_atomic_marker
 * @brief Leaf-name prefix that marks a file as this tool's in-progress output.
 * @details Leading dot so `list_pages()` and the AppleDouble filter already
 *          skip it; the rest makes an abandoned temp attributable at a glance.
 * @note Read-only; ::mkstemps supplies the unique six-character token.
 * @since 0.1.0
 */
static const char s_mdl_atomic_marker[] = ".mdl-tmp-";

bool mdl_atomic_tmp_path(const char* final_path, char* out, size_t cap)
{
  if ((final_path == nullptr) || (out == nullptr) || (cap == 0U)) {
    return false;
  }
  if (final_path[0] == '\0') {
    return false;
  }

  /* Split at the last separator so the temp is a SIBLING: rename() cannot
   * cross a filesystem, and a temp elsewhere would fail with EXDEV. */
  const char* leaf = strrchr(final_path, '/');
  const char* name = (leaf == nullptr) ? final_path : (leaf + 1);
  int         n    = 0;
  if (leaf == nullptr) {
    n = snprintf(out, cap, "%sXXXXXX-%s", s_mdl_atomic_marker, name);
  } else {
    const size_t dir_len = (size_t)(leaf - final_path) + 1U; /* keep the '/' */
    n = snprintf(out, cap, "%.*s%sXXXXXX-%s", (int)dir_len, final_path, s_mdl_atomic_marker, name);
  }
  /* A truncated temp path could name a DIFFERENT file than intended, so a
   * short buffer aborts the write rather than silently retargeting it. */
  if ((n <= 0) || ((size_t)n >= cap)) {
    out[0] = '\0';
    return false;
  }
  const size_t suffix_len = strlen(name) + 1U; /* '-' plus the original leaf */
  if (suffix_len > (size_t)INT_MAX) {
    out[0] = '\0';
    return false;
  }
  const int fd = mkstemps(out, (int)suffix_len);
  if (fd < 0) {
    out[0] = '\0';
    return false;
  }
  if (close(fd) != 0) {
    (void)remove(out);
    out[0] = '\0';
    return false;
  }
  return true;
}

/** @brief Open and fsync a regular temp file without following a symlink. */
RA8_INTERNAL static bool sync_regular(const char* path)
{
  const int fd = open(path, O_RDONLY | O_NOFOLLOW);
  if (fd < 0) {
    return false;
  }
  struct stat st;
  const bool  ok = (fstat(fd, &st) == 0) && S_ISREG(st.st_mode) && (fsync(fd) == 0);
  (void)close(fd);
  return ok;
}

/** @brief Open the containing directory so the rename can be made durable. */
RA8_INTERNAL static int open_parent(const char* path)
{
  char         dir[PATH_MAX];
  const size_t len = strnlen(path, sizeof(dir));
  if ((len == 0U) || (len >= sizeof(dir))) {
    return -1;
  }
  memcpy(dir, path, len + 1U);
  char* slash = strrchr(dir, '/');
  if (slash == nullptr) {
    (void)snprintf(dir, sizeof(dir), ".");
  } else if (slash == dir) {
    slash[1] = '\0';
  } else {
    *slash = '\0';
  }
  return open(dir, O_RDONLY);
}

bool mdl_atomic_commit(const char* tmp_path, const char* final_path)
{
  if ((tmp_path == nullptr) || (final_path == nullptr)) {
    return false;
  }
  if (!sync_regular(tmp_path)) {
    (void)remove(tmp_path);
    return false;
  }
  const int dir_fd = open_parent(final_path);
  if (dir_fd < 0) {
    (void)remove(tmp_path);
    return false;
  }
  if (rename(tmp_path, final_path) != 0) {
    /* rename() failing leaves final_path exactly as it was -- that is the
     * property the caller is relying on. Only the debris needs clearing. */
    (void)remove(tmp_path);
    (void)close(dir_fd);
    return false;
  }
  const bool durable = fsync(dir_fd) == 0;
  (void)close(dir_fd);
  return durable;
}

void mdl_atomic_abort(const char* tmp_path)
{
  if (tmp_path == nullptr) {
    return;
  }
  if (tmp_path[0] == '\0') {
    return;
  }
  (void)remove(tmp_path);
}
