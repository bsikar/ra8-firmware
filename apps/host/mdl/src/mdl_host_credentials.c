/**
 * @file mdl_host_credentials.c
 * @brief Pure host credential-snapshot metadata comparison.
 * @details Keeps platform-specific nanosecond fields out of portable policy and
 *          makes the mutation decision directly fault-testable.
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */
#include <sys/stat.h>

#include "mdl_host_credentials_internal.h"
#include "ra8_attributes.h"

RA8_PRIV bool priv_mdl_host_credential_stat_unchanged(const struct stat* before,
                                                      const struct stat* after)
{
  if ((before == nullptr) || (after == nullptr)) {
    return false;
  }
#ifdef __APPLE__
  const bool times_unchanged = (before->st_mtimespec.tv_sec == after->st_mtimespec.tv_sec) &&
                               (before->st_mtimespec.tv_nsec == after->st_mtimespec.tv_nsec) &&
                               (before->st_ctimespec.tv_sec == after->st_ctimespec.tv_sec) &&
                               (before->st_ctimespec.tv_nsec == after->st_ctimespec.tv_nsec);
#else
  const bool times_unchanged = (before->st_mtim.tv_sec == after->st_mtim.tv_sec) &&
                               (before->st_mtim.tv_nsec == after->st_mtim.tv_nsec) &&
                               (before->st_ctim.tv_sec == after->st_ctim.tv_sec) &&
                               (before->st_ctim.tv_nsec == after->st_ctim.tv_nsec);
#endif
  return times_unchanged && (before->st_dev == after->st_dev) &&
         (before->st_ino == after->st_ino) && (before->st_mode == after->st_mode) &&
         (before->st_size == after->st_size);
}
