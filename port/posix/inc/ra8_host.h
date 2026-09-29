/**
 * @file ra8_host.h
 * @brief One-call host composition root: filesystem, standard streams, arena.
 * @ingroup grp_io
 *
 * @par Tag
 * [Ring 4 / Host Port] {World: Host}
 *
 * @details
 * Every host binary in the tree stands the platform up the same way: bind a
 * root-confined POSIX filesystem, bind the standard output pair through the
 * portable byte-stream facade, ignore `SIGPIPE` so a closed pipe surfaces as
 * `EPIPE` to the adapter instead of killing the process, and carve scratch out
 * of one caller-owned block. The order matters and the teardown obligations
 * are real, but both lived in prose, so each tool re-derived them and each got
 * a slightly different subset right.
 *
 * ::ra8_host_open does that ritual once and ::ra8_host_close undoes exactly
 * what it did, so a partially-bound host cannot leak a root descriptor. Each
 * piece is optional: a tool that wants a filesystem and no streams, or scratch
 * and nothing else, leaves the other request fields alone.
 *
 * This is the default door, not the only one. ::fw_fs_posix_init,
 * ::ra8_io_stream_posix_init and ::ra8_arena_init all stay public and callable
 * for a tool that needs two arenas, a non-standard descriptor pair, or a root
 * bound on some schedule of its own.
 *
 * @code
 * static alignas(max_align_t) uint8_t s_scratch[64U * 1024U];
 * ra8_host_t             host = {};
 * const ra8_host_cfg_t   cfg  = {
 *     .root_path       = "/",
 *     .bind_filesystem = true,
 *     .bind_streams    = true,
 *     .arena_base      = s_scratch,
 *     .arena_bytes     = sizeof(s_scratch),
 * };
 * if (ra8_host_open(&host, &cfg) != k_ra8_ok) {
 *   return 1;
 * }
 * (void)ra8_io_stream_puts(host.output, "ready\n");
 * (void)ra8_host_close(&host);
 * @endcode
 *
 * @note Not thread-safe; host bring-up runs single-threaded.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#pragma once

#ifndef RA8_OFF_TARGET
#error "port/posix is host-only and must never be compiled or linked into target firmware."
#endif

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "fw_if_fs.h"
#include "fw_if_fs_posix.h"
#include "ra8_arena.h"
#include "ra8_err.h"
#include "ra8_io_stream.h"
#include "ra8_io_stream_posix.h"

/**
 * @struct ra8_host_cfg_t
 * @brief What the caller wants stood up, and the storage to stand it up in.
 *
 * @details
 * Every field is a request, and an absent request is not a failure: clearing
 * @ref bind_filesystem leaves @ref ra8_host_t::fs null, clearing
 * @ref bind_streams leaves both stream pointers null, and a zero
 * @ref arena_bytes leaves the arena unbound. An all-zero configuration is
 * accepted and binds nothing, which is what a tool that only wants the
 * `SIGPIPE` disposition asks for.
 *
 * @since 0.1.0
 */
typedef struct {
  const char* root_path;       /**< Existing host directory used as `/`.           */
  void*       arena_base;      /**< Caller-owned scratch block, or NULL.           */
  uint32_t    arena_bytes;     /**< Scratch length; zero leaves the arena unbound. */
  bool        bind_filesystem; /**< Bind the root-confined POSIX filesystem.       */
  bool        bind_streams;    /**< Bind stdout and stderr as byte streams.        */
  bool        removable_media; /**< Truthful property of the selected root.        */
} ra8_host_cfg_t;

/**
 * @struct ra8_host_t
 * @brief Caller-owned composition state and the handles it published.
 *
 * @details
 * Zero-initialise (`= {}`) and pass to ::ra8_host_open. The three pointers are
 * the whole point of the type: they address storage inside this object, so the
 * host must out-live every call made through them. A pointer is non-NULL
 * exactly when the matching request in ::ra8_host_cfg_t was set and its
 * binding succeeded. Treat every other field as private.
 *
 * @invariant A non-NULL @ref fs, @ref output or @ref diagnostic implies
 *            @ref opened is set.
 *
 * @since 0.1.0
 */
typedef struct {
  /** @brief Bound filesystem facade, or NULL when none was asked for. */
  fw_fs_t* fs;
  /** @brief Bound standard-output stream, or NULL. */
  ra8_io_stream_t* output;
  /** @brief Bound standard-error stream, or NULL. */
  ra8_io_stream_t* diagnostic;
  /** @brief Scratch arena; left unbound when no scratch was offered. */
  ra8_arena_t arena;
  /** @brief Storage behind @ref fs (private). */
  fw_fs_t fs_storage;
  /** @brief Adapter state retained by @ref fs (private). */
  fw_fs_posix_state_t fs_state;
  /** @brief Storage behind @ref output (private). */
  ra8_io_stream_t out_storage;
  /** @brief Storage behind @ref diagnostic (private). */
  ra8_io_stream_t err_storage;
  /** @brief Descriptor state retained by @ref output (private). */
  ra8_io_stream_posix_state_t out_state;
  /** @brief Descriptor state retained by @ref diagnostic (private). */
  ra8_io_stream_posix_state_t err_state;
  /** @brief A root descriptor is open (private). */
  bool fs_bound;
  /** @brief Lifecycle guard (private). */
  bool opened;
} ra8_host_t;

/**
 * @brief Stand the platform up on a host in one call.
 *
 * @details
 * Runs the audited order: install the `SIGPIPE` disposition, bind the
 * filesystem, bind the standard stream pair, bind the arena. The first failing
 * step unwinds whatever the earlier steps bound, so a non-ok return leaves
 * @p host as unbound as it was on entry and never leaves a root descriptor
 * open. The `SIGPIPE` disposition is process-wide and deliberately not
 * restored by ::ra8_host_close, because a second composition in the same
 * process would otherwise race the first one's teardown.
 *
 * @param[out] host Caller-owned composition state (zero-initialised).
 * @param[in]  cfg  Requested bindings.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok               Every requested binding is ready.
 * @retval k_ra8_err_null_ptr     @p host or @p cfg was NULL.
 * @retval k_ra8_err_invalid_arg  A filesystem was requested with no root path.
 * @retval k_ra8_err_exists       @p host is already open.
 * @retval k_ra8_err_comm_error   The signal disposition could not be installed.
 * @retval other                  The failing binding's own error.
 *
 * @pre @p host is zero-initialised or was closed by ::ra8_host_close.
 * @pre `cfg->arena_base` addresses at least `cfg->arena_bytes` writable bytes.
 * @post On success each requested handle is non-NULL and the rest stay NULL.
 * @post On any non-ok return nothing is left bound and no descriptor is open.
 *
 * @note Not thread-safe; installs a process-wide signal disposition.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_host_open(ra8_host_t* host, const ra8_host_cfg_t* cfg);

/**
 * @brief Release everything ::ra8_host_open bound, exactly once.
 *
 * @details
 * Closes the root descriptor when one is open and clears every published
 * handle. Streams borrow their descriptors and own nothing, so closing a host
 * never closes stdout or stderr. Closing an unopened host is a no-op that
 * reports success, so an error path may call it unconditionally.
 *
 * @param[in,out] host Composition state to release.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok           The host is released, or was never open.
 * @retval k_ra8_err_null_ptr @p host was NULL.
 * @retval other              The filesystem adapter refused to close.
 *
 * @pre No operation through a published handle is still in flight.
 * @post Every published handle is NULL and the host may be reopened.
 * @post A failed close still clears the handles; the descriptor state is the
 *       adapter's to report on.
 *
 * @note Not thread-safe. The `SIGPIPE` disposition is intentionally kept.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_host_close(ra8_host_t* host);

#ifdef __cplusplus
}
#endif
