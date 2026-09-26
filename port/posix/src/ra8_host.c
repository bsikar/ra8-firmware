/**
 * @file ra8_host.c
 * @brief Composition of the host filesystem, standard streams, and arena.
 * @ingroup grp_io
 *
 * @par Tag
 * [Ring 4 / Host Port] {World: Host}
 *
 * @details
 * Runs the bring-up order the host binaries previously hand-wrote, and owns
 * the unwind so a half-bound composition cannot escape. Nothing here is new
 * capability: every step calls a public initializer that was already
 * reachable, and the value is that the order and the teardown are stated once.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#ifndef RA8_OFF_TARGET
#error "port/posix is host-only and must never be compiled or linked into target firmware."
#endif

/* `sigaction` is a POSIX.1 interface the strict-C23 dialect hides by default. */
#ifndef _GNU_SOURCE
/* NOLINTNEXTLINE(bugprone-reserved-identifier) */
#define _GNU_SOURCE
#endif

#include "ra8_host.h"

#include <signal.h>
#include <stddef.h> // ra8-keep-include: `nullptr` used directly
#include <unistd.h>

#include "ra8_attributes.h" // ra8-keep-include: `RA8_INTERNAL` used directly
#include "ra8_err.h"        // ra8-keep-include: `ra8_err_t` used directly

/**
 * @brief Ignore `SIGPIPE` so a closed reader surfaces as a write error.
 *
 * @details
 * Without this a `| head` on any host tool kills the process mid-write and the
 * adapter never gets to report the failure. The disposition is process-wide
 * and is deliberately never restored.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok             The disposition is installed.
 * @retval k_ra8_err_comm_error The host refused the signal action.
 *
 * @pre The process has not already installed a conflicting handler it needs.
 * @post Success leaves `SIGPIPE` ignored for the process lifetime.
 * @post Failure changes no disposition.
 *
 * @note Not thread-safe with concurrent `sigaction` on the same signal.
 *
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_ignore_sigpipe(void)
{
  struct sigaction action = {.sa_handler = SIG_IGN};
  if ((sigemptyset(&action.sa_mask) != 0) || (sigaction(SIGPIPE, &action, nullptr) != 0)) {
    return k_ra8_err_comm_error;
  }
  return k_ra8_ok;
}

/**
 * @brief Bind the root-confined filesystem into the host's own storage.
 *
 * @param[in,out] host Composition state receiving the binding.
 * @param[in]     cfg  Requested bindings.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok              Bound, or no filesystem was requested.
 * @retval k_ra8_err_invalid_arg A filesystem was requested with no root path.
 * @retval other                 The POSIX adapter's own failure.
 *
 * @pre @p host and @p cfg are non-NULL and @p host has nothing bound yet.
 * @post Success with a request publishes @ref ra8_host_t::fs.
 * @post Any non-ok return leaves no root descriptor open.
 *
 * @note Not thread-safe.
 *
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_bind_fs(ra8_host_t* host, const ra8_host_cfg_t* cfg)
{
  if (!cfg->bind_filesystem) {
    return k_ra8_ok;
  }
  if (cfg->root_path == nullptr) {
    return k_ra8_err_invalid_arg;
  }
  const fw_fs_posix_cfg_t fs_cfg = {.root_path       = cfg->root_path,
                                    .removable_media = cfg->removable_media};
  host->fs_state.root_fd         = -1;
  const ra8_err_t err = fw_fs_posix_init(&host->fs_storage, &host->fs_state, &fs_cfg);
  if (err != k_ra8_ok) {
    return err;
  }
  host->fs_bound = true;
  host->fs       = &host->fs_storage;
  return k_ra8_ok;
}

/**
 * @brief Bind stdout and stderr through the portable byte-stream facade.
 *
 * @param[in,out] host Composition state receiving the bindings.
 * @param[in]     cfg  Requested bindings.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok Bound, or no streams were requested.
 * @retval other    The raw-descriptor adapter's own failure.
 *
 * @pre @p host and @p cfg are non-NULL.
 * @post Success with a request publishes both stream pointers, never one.
 * @post Failure publishes neither.
 *
 * @note The descriptors are borrowed; the host never closes them.
 *
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_bind_streams(ra8_host_t* host, const ra8_host_cfg_t* cfg)
{
  if (!cfg->bind_streams) {
    return k_ra8_ok;
  }
  ra8_err_t err = ra8_io_stream_posix_init(&host->out_storage, &host->out_state, STDOUT_FILENO);
  if (err != k_ra8_ok) {
    return err;
  }
  err = ra8_io_stream_posix_init(&host->err_storage, &host->err_state, STDERR_FILENO);
  if (err != k_ra8_ok) {
    return err;
  }
  host->output     = &host->out_storage;
  host->diagnostic = &host->err_storage;
  return k_ra8_ok;
}

/**
 * @brief Bind the caller-owned scratch block as a bump arena.
 *
 * @param[in,out] host Composition state receiving the arena.
 * @param[in]     cfg  Requested bindings.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok              Bound, or no scratch was offered.
 * @retval k_ra8_err_invalid_arg Scratch bytes were named with no base pointer.
 * @retval other                 The arena's own failure.
 *
 * @pre @p host and @p cfg are non-NULL.
 * @post Success with a request leaves @ref ra8_host_t::arena empty and ready.
 *
 * @note Not thread-safe.
 *
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_bind_arena(ra8_host_t* host, const ra8_host_cfg_t* cfg)
{
  if (cfg->arena_bytes == 0U) {
    return k_ra8_ok;
  }
  if (cfg->arena_base == nullptr) {
    return k_ra8_err_invalid_arg;
  }
  return ra8_arena_init(&host->arena, cfg->arena_base, cfg->arena_bytes);
}

/**
 * @brief Drop every published handle and close the root descriptor.
 *
 * @param[in,out] host Composition state to clear.
 *
 * @return ra8_err_t The adapter's close status, or ::k_ra8_ok when unbound.
 *
 * @pre @p host is non-NULL.
 * @post Every published handle is NULL whatever the adapter reported.
 *
 * @note Shared by the open-path unwind and ::ra8_host_close.
 *
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_release(ra8_host_t* host)
{
  ra8_err_t err = k_ra8_ok;
  if (host->fs_bound) {
    err = fw_fs_posix_deinit(&host->fs_state);
  }
  host->fs         = nullptr;
  host->output     = nullptr;
  host->diagnostic = nullptr;
  host->arena      = (ra8_arena_t){};
  host->fs_bound   = false;
  host->opened     = false;
  return err;
}

ra8_err_t ra8_host_open(ra8_host_t* host, const ra8_host_cfg_t* cfg)
{
  if ((host == nullptr) || (cfg == nullptr)) {
    return k_ra8_err_null_ptr;
  }
  if (host->opened) {
    return k_ra8_err_exists;
  }
  ra8_err_t err = internal_ignore_sigpipe();
  if (err == k_ra8_ok) {
    err = internal_bind_fs(host, cfg);
  }
  if (err == k_ra8_ok) {
    err = internal_bind_streams(host, cfg);
  }
  if (err == k_ra8_ok) {
    err = internal_bind_arena(host, cfg);
  }
  if (err != k_ra8_ok) {
    (void)internal_release(host);
    return err;
  }
  host->opened = true;
  return k_ra8_ok;
}

ra8_err_t ra8_host_close(ra8_host_t* host)
{
  if (host == nullptr) {
    return k_ra8_err_null_ptr;
  }
  if (!host->opened) {
    return k_ra8_ok;
  }
  return internal_release(host);
}
