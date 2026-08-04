/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file mdl_pathfs.c
 * @brief Implementation of the guarded filesystem directory join.
 */
#include "mdl_pathfs.h"

#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>

#include "mdl_sanitize.h"
#include "ra8_attributes.h"

/** @brief Filesystem constants for the guarded directory join. */
typedef enum : uint16_t {
  k_pathfs_dir_mode = 0755, /**< mkdir() permission bits for a created dir. */
} mdl_pathfs_const_t;

/** @brief True if `child` resolves to a path inside `parent_abs`. */
RA8_INTERNAL static bool path_under(const char* parent_abs, const char* child)
{
  char resolved[PATH_MAX];
  if (realpath(child, resolved) == nullptr) {
    return false;
  }
  return mdl_path_contained(parent_abs, resolved);
}

bool mdl_join_dir_under(const char* parent_abs, const char* seg, char* out, size_t cap)
{
  if (!mdl_path_join(parent_abs, seg, out, cap)) {
    (void)fprintf(stderr, "  refusing unsafe path segment '%s' under %s\n", seg, parent_abs);
    return false;
  }
  (void)mkdir(out, (mode_t)k_pathfs_dir_mode);
  if (!path_under(parent_abs, out)) {
    (void)fprintf(stderr, "  refusing dir outside %s: %s\n", parent_abs, out);
    return false;
  }
  return true;
}
