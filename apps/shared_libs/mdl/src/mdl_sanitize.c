/**
 * @file mdl_sanitize.c
 * @brief Implementation of the untrusted-name sanitisers.
 * @details Every rule this file used to spell out now lives in the platform
 *          name policy at `libs/if/inc/ra8_path.h` (#749). What stays here is
 *          the `bool` dialect media_dl's call sites are written against: the
 *          platform entry points separate "I refused this call" from "the
 *          answer is no", and these three wrappers fold both back to `false`
 *          so no caller changes behaviour.
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */
#include "mdl_sanitize.h"

#include "ra8_err.h"
#include "ra8_path.h"

bool mdl_sanitize_segment(const char* raw, char* out, size_t cap)
{
  bool verbatim = false;
  if (ra8_path_sanitize_segment(raw, out, cap, &verbatim) != k_ra8_ok) {
    return false;
  }
  return verbatim;
}

bool mdl_path_contained(const char* parent, const char* candidate)
{
  bool contained = false;
  if (ra8_path_contained(parent, candidate, &contained) != k_ra8_ok) {
    return false;
  }
  return contained;
}

bool mdl_path_join(const char* parent, const char* seg, char* out, size_t cap)
{
  return ra8_path_join_under(parent, seg, out, cap) == k_ra8_ok;
}
