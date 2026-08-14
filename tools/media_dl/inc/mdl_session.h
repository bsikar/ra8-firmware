/**
 * @file mdl_session.h
 * @brief Session identity (honest User-Agent) and robots.txt gating.
 *
 * @details
 * Ties the network backend, a truthful configurable User-Agent, and the
 * per-host robots.txt cache into one object the download loop consults. The
 * User-Agent identifies the tool by name, version and a project URL rather than
 * impersonating a browser -- a lying UA is what gets a well-behaved bot
 * hard-banned instead of soft-throttled. Robots gating is on by default and
 * fetches `/robots.txt` per host on first contact.
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "mdl_net.h"
#include "mdl_robots.h"

/** @brief Fixed session buffer sizes. */
typedef enum : uint32_t {
  k_mdl_session_scratch = 65536U, /**< robots.txt fetch scratch bytes. */
  k_mdl_ua_max          = 256U,   /**< Built User-Agent buffer bytes.  */
} mdl_session_size_t;

/**
 * @struct mdl_session_t
 * @brief One download session's identity and robots.txt state.
 * @details Large enough to embed the per-host cache and a fetch scratch buffer;
 *          declare one at file scope rather than on the stack.
 * @invariant `user_agent` outlives the session.
 * @see mdl_session_url_allowed()
 * @since 0.1.0
 */
typedef struct {
  mdl_net_iface_t*   net;                            /**< Backend used for robots.txt fetches. */
  const char*        user_agent;                     /**< Full UA header (caller-owned).       */
  bool               honor_robots;                   /**< False disables all robots gating.    */
  mdl_robots_cache_t cache;                          /**< Per-host robots cache.               */
  char               scratch[k_mdl_session_scratch]; /**< Fetch scratch.                       */
} mdl_session_t;

/**
 * @brief Build the truthful default User-Agent into `out`.
 *
 * @details
 * Emits `media_dl/<version> (+<project-url>; <contact>)`, or the same without
 * the contact clause when none is supplied. No browser-impersonation string is
 * ever produced.
 *
 * @param[in]  contact Operator contact (email/URL), or NULL/empty for none.
 * @param[out] out     Destination buffer for the NUL-terminated UA string.
 * @param[in]  cap     Capacity of `out` in bytes.
 *
 * @return Whether a contact clause was included.
 * @retval true  `contact` was non-empty and appears in the UA.
 * @retval false No contact was configured (caller should warn once).
 *
 * @pre `out`, when non-NULL, has room for at least `cap` bytes.
 * @pre `cap` is large enough for the fixed portion (>= 64 recommended).
 * @post `out` is NUL-terminated whenever `cap > 0`.
 *
 * @note Thread-safe: writes only caller-provided storage.
 * @since 0.1.0
 */
bool mdl_session_build_ua(const char* contact, char* out, size_t cap);

/**
 * @brief The robots.txt product token this tool matches on (`"media_dl"`).
 * @return Borrowed pointer to a static token string.
 * @retval non-NULL Always: the constant product token.
 * @pre None.
 * @post No state is modified.
 * @note Thread-safe: returns a constant.
 * @since 0.1.0
 */
const char* mdl_session_ua_token(void);

/**
 * @brief Initialise a session over a network backend.
 *
 * @param[out] session      Session to initialise (non-NULL).
 * @param[in]  net          Backend used for robots.txt fetches.
 * @param[in]  user_agent   Full UA header string (must outlive the session).
 * @param[in]  honor_robots Whether robots.txt is consulted (false = ignore).
 *
 * @return Nothing.
 *
 * @pre `session`, `net`, and `user_agent` are non-NULL.
 * @pre `user_agent` outlives every call using `session`.
 * @post The per-host cache is empty.
 * @post `session->honor_robots` equals @p honor_robots.
 *
 * @note Not thread-safe: initialises caller storage.
 * @since 0.1.0
 */
void mdl_session_init(mdl_session_t*   session,
                      mdl_net_iface_t* net,
                      const char*      user_agent,
                      bool             honor_robots);

/**
 * @brief Decide whether `url` may be fetched under robots.txt, and its delay.
 *
 * @details
 * Consults the per-host robots cache (fetching `/robots.txt` on first contact),
 * and refuses -- printing a message that names the blocking rule -- a URL our
 * user-agent is disallowed from. When robots gating is off (`--ignore-robots`)
 * every URL is permitted. The host's `Crawl-delay` is reported so the caller
 * can raise its per-host politeness floor.
 *
 * @param[in]  session        Initialised session.
 * @param[in]  url            Absolute http(s) URL to test.
 * @param[out] crawl_delay_ms Receives the host's Crawl-delay in ms (0 if none).
 *
 * @return Whether the fetch is permitted.
 * @retval true  Robots allows it, or gating was explicitly disabled.
 * @retval false A rule forbids it, robots is inaccessible/oversized, or the
 *               URL cannot be parsed safely; a message was printed to stderr.
 *
 * @pre `session` and `url` are non-NULL.
 * @pre `url` uses an http(s) scheme.
 * @post `*crawl_delay_ms` is set when `crawl_delay_ms` is non-NULL.
 * @post The session cache may gain an entry for `url`'s host.
 *
 * @note Not thread-safe: mutates the session cache.
 * @since 0.1.0
 */
bool mdl_session_url_allowed(mdl_session_t* session, const char* url, uint32_t* crawl_delay_ms);
