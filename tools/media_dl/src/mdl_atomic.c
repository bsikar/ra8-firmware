/*
 * Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */
/**
 * @file mdl_atomic.c
 * @brief Implementation of the write-to-temp-then-rename helpers.
 *
 * @details
 * Three short functions with no state: the whole design lives in
 * @ref mdl_atomic.h. Deliberately free of any "write the destination anyway"
 * fallback -- the failure mode this file removes is precisely a writer
 * deciding it may open the destination when the careful path is inconvenient.
 */
#include "mdl_atomic.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>

/**
 * @var s_mdl_atomic_marker
 * @brief Leaf-name prefix that marks a file as this tool's in-progress output.
 * @details Leading dot so `list_pages()` and the AppleDouble filter already
 *          skip it; the rest makes an abandoned temp attributable at a glance.
 * @note Read-only; the pid is appended by ::mdl_atomic_tmp_path, not stored.
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
  int         n    = 0;
  if (leaf == nullptr) {
    n = snprintf(out, cap, "%s%ld-%s", s_mdl_atomic_marker, (long)getpid(), final_path);
  } else {
    const size_t dir_len = (size_t)(leaf - final_path) + 1U; /* keep the '/' */
    n                    = snprintf(out,
                                    cap,
                                    "%.*s%s%ld-%s",
                                    (int)dir_len,
                                    final_path,
                                    s_mdl_atomic_marker,
                                    (long)getpid(),
                                    leaf + 1);
  }
  /* A truncated temp path could name a DIFFERENT file than intended, so a
   * short buffer aborts the write rather than silently retargeting it. */
  return (n > 0) && ((size_t)n < cap);
}

bool mdl_atomic_commit(const char* tmp_path, const char* final_path)
{
  if ((tmp_path == nullptr) || (final_path == nullptr)) {
    return false;
  }
  if (rename(tmp_path, final_path) != 0) {
    /* rename() failing leaves final_path exactly as it was -- that is the
     * property the caller is relying on. Only the debris needs clearing. */
    (void)remove(tmp_path);
    return false;
  }
  return true;
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
