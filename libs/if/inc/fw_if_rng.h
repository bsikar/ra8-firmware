/**
 * @file fw_if_rng.h
 * @brief Architecture-neutral random-bytes port: entropy behind one narrow
 * vtable.
 * @ingroup grp_fw_rng
 *
 * @par Tag
 * [Ring 2 / Interface] {World: Any}
 *
 * @details
 * Portable code needs entropy for nonces, salt, padding and tie-breaking, and
 * every chip has its own answer for where the bytes come from: a hardware
 * TRNG, a seeded CSPRNG, a ring oscillator, a peripheral that must be polled.
 * None of that is a portable driver's business. This port carries the two
 * intents such a driver actually has -- fill a buffer, and answer whether the
 * source can serve right now -- and leaves the mechanism entirely to a
 * binding supplied by the chip.
 *
 * The fill contract is "never lies": on success the *whole* buffer is fresh
 * entropy, and on an error return the binding must leave the buffer untouched.
 * A portable caller re-derives nothing from a partially filled buffer, and the
 * membrane cannot verify a binding honored that, which is why it is written as
 * a contract rather than a check. A binding that cannot serve a request this
 * instant returns an error rather than blocking indefinitely or handing back
 * stale bytes.
 *
 * ::fw_rng_ready is the escape hatch for callers that cannot tolerate
 * blocking: ask first, then fill. A binding whose source always serves answers
 * true every time, and ::fw_rng_bytes on a not-ready source is the binding's
 * error to give, not this port's guess.
 *
 * The port deliberately has no seed, quality or reconfiguration surface. A
 * caller that needs a specific entropy scheme names a chip-specific binding;
 * this port serves one contract, and bytes that pass ::fw_rng_bytes are fit to
 * use as key material whenever the binding answered ok.
 *
 * Per the taxonomy in `docs/PORTS.md`, this uses (a) the caller-allocated
 * facade plus (b) a narrow ops struct the binding fills. Handles are
 * caller-owned. This interface performs no allocation and contains no chip,
 * operating-system or device header.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ra8_err.h"

/**
 * @struct fw_rng_iface_s
 * @brief The narrow ops struct a chip binding fills.
 *
 * @details
 * Two entries, because the port has two intents. Each takes the binding's own
 * context as its first argument, so one binding can serve more than one source
 * without a file-scope variable. A NULL op is a malformed binding, not a
 * declined capability, and ::fw_rng_bind rejects it.
 */
typedef struct fw_rng_iface_s {
  /** @brief Fill @p out with @p len bytes of entropy. May block until it can
   *         serve or fail; on error, @p out stays untouched. */
  ra8_err_t (*fill)(void *ctx, uint8_t *out, size_t len);
  /** @brief Whether the source can serve bytes right now. */
  ra8_err_t (*ready)(void *ctx, bool *out_ready);
} fw_rng_iface_t;

/**
 * @struct fw_rng_s
 * @brief Caller-owned binding handle.
 *
 * @details
 * Populated only by ::fw_rng_bind and treated as opaque afterwards. The
 * `bound` flag is what every entry point checks: a zeroed handle is not bound,
 * so a caller that forgets to bind gets ::k_ra8_err_not_initialized rather
 * than a jump through an uninitialised function pointer.
 */
typedef struct fw_rng_s {
  const fw_rng_iface_t *iface; /**< Binding ops, never NULL once bound. */
  void                   *ctx; /**< Binding context, may be NULL.       */
  bool                    bound; /**< Set by ::fw_rng_bind only.        */
} fw_rng_t;

/**
 * @brief Bind a handle to a chip entropy binding.
 *
 * @param[out] rng   Caller-owned storage, contents ignored on entry.
 * @param[in]  iface Ops struct; borrowed, must outlive @p rng.
 * @param[in]  ctx   Binding context, handed back to every op. May be NULL.
 * @return ::k_ra8_ok, or ::k_ra8_err_invalid_arg when @p rng or @p iface is
 *         NULL or @p iface leaves an op unset.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t
fw_rng_bind(fw_rng_t *rng, const fw_rng_iface_t *iface, void *ctx);

/**
 * @brief Fill @p out with @p len bytes of entropy.
 *
 * @details
 * A zero-length request is satisfied without waking the entropy source: a
 * caller that sizes a buffer conditionally should not pay a syscall to fill
 * nothing. On an error return the buffer carries whatever it carried on entry;
 * the binding contract above owns that, and the membrane simply never writes
 * to @p out itself.
 *
 * @param[in]  rng Bound handle.
 * @param[out] out Buffer to fill; may be NULL only when @p len is zero.
 * @param[in]  len Bytes to fill.
 * @return ::k_ra8_ok, ::k_ra8_err_invalid_arg for a NULL @p out with a
 *         nonzero @p len, ::k_ra8_err_not_initialized for an unbound handle,
 *         or whatever the binding reported.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t
fw_rng_bytes(const fw_rng_t *rng, uint8_t *out, size_t len);

/**
 * @brief Whether the entropy source can serve bytes right now.
 *
 * @details
 * This is a report, not a reservation: a true answer does not hold capacity,
 * and ::fw_rng_bytes right after it can still fail on a contended source.
 *
 * @param[in]  rng       Bound handle.
 * @param[out] out_ready On success, whether the source can serve now; false
 *                       on any failure.
 * @return ::k_ra8_ok, ::k_ra8_err_invalid_arg for a NULL output,
 *         ::k_ra8_err_not_initialized for an unbound handle, or whatever the
 *         binding reported.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t
fw_rng_ready(const fw_rng_t *rng, bool *out_ready);

#ifdef __cplusplus
}
#endif
