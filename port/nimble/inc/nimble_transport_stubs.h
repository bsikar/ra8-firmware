/**
 * @file port/nimble/inc/nimble_transport_stubs.h
 * @brief Weak link stubs standing in for the NimBLE transport core
 *
 * @par Tag
 * [Ring 4 / PORT] {World: S}
 *
 * @details
 * The curated build links NimBLE's host and NPL but not its
 * ``nimble/transport`` object library, so the symbols that library would
 * define are missing at link time while ``ble_hci_ra8_ble.c`` still
 * references them. ``nimble_npl_threadx.c`` supplies weak no-op
 * definitions to close the link; the linker prefers the strong upstream
 * symbols the moment the real transport TUs are added to the build, so
 * nothing here has to be removed first.
 *
 * These prototypes cannot come from upstream. The declarations live in
 * ``nimble/transport.h``, ``nimble/transport_impl.h`` and
 * ``os/os_mbuf.h``, and reaching them means pulling in the whole Mynewt
 * porting layer (``os/os_mempool.h`` hard-errors without a matching
 * ``OS_ALIGNMENT``) -- precisely the dependency the curated build is
 * avoiding. So the port declares what the port defines, which is also
 * what puts a prototype in front of each definition: without one, a stub
 * whose signature drifts from the upstream contract it is standing in
 * for compiles clean and fails at link, or worse, links against a
 * mismatched ABI.
 *
 * Signatures here are copied from the upstream headers named above and
 * must stay identical to them.
 *
 * @warning UNVALIDATED SCAFFOLD (issue #286). Every symbol declared here
 * is a link-only no-op, not a working BLE transport.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

struct os_mbuf;

/**
 * @brief Allocate an HCI event buffer (upstream ``ble_transport.h``).
 *
 * @details Stub: the curated build has no transport buffer pool, so the
 * allocation always fails and the caller drops the event.
 *
 * @param[in] discardable Non-zero when the caller can tolerate a NULL
 *                        return (low-priority event). Unused by the stub,
 *                        kept for ABI parity with upstream.
 *
 * @return Pointer to an event buffer.
 * @retval NULL Always, in the stub build.
 *
 * @pre The NimBLE port has been initialised.
 * @pre Caller is prepared for a NULL return.
 * @post No allocation is performed.
 * @post No global state changes.
 *
 * @note Not thread-safe unless the real transport core replaces it.
 *
 * @see ble_transport_free()
 *
 * @since 0.1.0
 */
void* ble_transport_alloc_evt(int discardable);

/**
 * @brief Allocate an ACL mbuf for the LL-to-host direction.
 *
 * @details Stub: no mbuf pool exists in the curated build.
 *
 * @return Pointer to a freshly allocated ACL mbuf chain.
 * @retval NULL Always, in the stub build.
 *
 * @pre The NimBLE port has been initialised.
 * @pre Caller is prepared for a NULL return.
 * @post No allocation is performed.
 * @post No global state changes.
 *
 * @note Not thread-safe unless the real transport core replaces it.
 *
 * @see ble_transport_to_hs_acl()
 *
 * @since 0.1.0
 */
struct os_mbuf* ble_transport_alloc_acl_from_ll(void);

/**
 * @brief Release a buffer obtained from ble_transport_alloc_evt().
 *
 * @details Stub: nothing was ever allocated, so nothing is freed.
 *
 * @param[in] buf Buffer to release. May be NULL.
 *
 * @pre @p buf came from ble_transport_alloc_evt() or is NULL.
 * @pre The NimBLE port has been initialised.
 * @post The buffer is no longer owned by the caller.
 * @post No global state changes in the stub build.
 *
 * @note Not thread-safe unless the real transport core replaces it.
 *
 * @see ble_transport_alloc_evt()
 *
 * @since 0.1.0
 */
void ble_transport_free(void* buf);

/**
 * @brief Push an HCI event up to the host stack.
 *
 * @details Stub: the event is dropped and success is reported so the
 * controller side does not treat the missing host as an error.
 *
 * @param[in] buf Event buffer from ble_transport_alloc_evt().
 *
 * @return Upstream transport status code.
 * @retval 0 Always, in the stub build.
 *
 * @pre @p buf is a valid event buffer.
 * @pre The NimBLE port has been initialised.
 * @post Ownership of @p buf passes to the transport.
 * @post No data reaches a host in the stub build.
 *
 * @note Not thread-safe unless the real transport core replaces it.
 *
 * @see ble_transport_to_hs_acl()
 *
 * @since 0.1.0
 */
int ble_transport_to_hs_evt(void* buf);

/**
 * @brief Push an ACL packet up to the host stack.
 *
 * @details Stub: the mbuf is dropped and success is reported.
 *
 * @param[in] om ACL mbuf chain to deliver.
 *
 * @return Upstream transport status code.
 * @retval 0 Always, in the stub build.
 *
 * @pre @p om is a valid mbuf chain.
 * @pre The NimBLE port has been initialised.
 * @post Ownership of @p om passes to the transport.
 * @post No data reaches a host in the stub build.
 *
 * @note Not thread-safe unless the real transport core replaces it.
 *
 * @see ble_transport_to_hs_evt()
 *
 * @since 0.1.0
 */
int ble_transport_to_hs_acl(struct os_mbuf* om);

/**
 * @brief Append bytes to an mbuf chain (upstream ``os_mbuf.h``).
 *
 * @details Stub: no mbuf pool exists, so the append is a no-op.
 *
 * @param[in,out] om   Chain to append to.
 * @param[in]     data Bytes to copy in.
 * @param[in]     len  Number of bytes at @p data.
 *
 * @return Mynewt status code.
 * @retval 0 Always, in the stub build.
 *
 * @pre @p om and @p data are valid for @p len bytes.
 * @pre The NimBLE port has been initialised.
 * @post @p om is unchanged in the stub build.
 * @post No allocation is performed.
 *
 * @note Not thread-safe unless the real mbuf pool replaces it.
 *
 * @see os_mbuf_len()
 *
 * @since 0.1.0
 */
int os_mbuf_append(struct os_mbuf* om, const void* data, uint16_t len);

/**
 * @brief Free every mbuf in a chain.
 *
 * @details Stub: nothing was allocated, so nothing is freed.
 *
 * @param[in] om Head of the chain to release. May be NULL.
 *
 * @return Mynewt status code.
 * @retval 0 Always, in the stub build.
 *
 * @pre @p om is a chain head or NULL.
 * @pre The NimBLE port has been initialised.
 * @post The caller no longer owns @p om.
 * @post No pool state changes in the stub build.
 *
 * @note Not thread-safe unless the real mbuf pool replaces it.
 *
 * @see os_mbuf_append()
 *
 * @since 0.1.0
 */
int os_mbuf_free_chain(struct os_mbuf* om);

/**
 * @brief Total payload length of an mbuf chain.
 *
 * @details Stub: reports an empty chain.
 *
 * @param[in] om Chain to measure. May be NULL.
 *
 * @return Payload length in bytes.
 * @retval 0 Always, in the stub build.
 *
 * @pre @p om is a chain head or NULL.
 * @pre The NimBLE port has been initialised.
 * @post @p om is not modified.
 * @post No pool state changes.
 *
 * @note Not thread-safe unless the real mbuf pool replaces it.
 *
 * @see os_mbuf_copydata()
 *
 * @since 0.1.0
 */
uint16_t os_mbuf_len(const struct os_mbuf* om);

/**
 * @brief Copy a byte range out of an mbuf chain.
 *
 * @details Stub: copies nothing and reports success.
 *
 * @param[in]  om  Chain to read from.
 * @param[in]  off Byte offset into the chain payload.
 * @param[in]  len Number of bytes to copy.
 * @param[out] dst Destination buffer, at least @p len bytes.
 *
 * @return Mynewt status code.
 * @retval 0 Always, in the stub build.
 *
 * @pre @p dst is writable for @p len bytes.
 * @pre @p off and @p len lie inside the chain payload.
 * @post @p dst is untouched in the stub build.
 * @post @p om is not modified.
 *
 * @note Not thread-safe unless the real mbuf pool replaces it.
 *
 * @see os_mbuf_len()
 *
 * @since 0.1.0
 */
int os_mbuf_copydata(const struct os_mbuf* om, int off, int len, void* dst);

/**
 * @brief Host-to-controller HCI command entry point.
 *
 * @details Stub wrapper: forwards straight to
 * ble_transport_to_ll_cmd_impl() so an app that bypasses the upstream
 * transport core still reaches this port's controller side.
 *
 * @param[in] buf HCI command buffer.
 *
 * @return Upstream transport status code.
 * @retval 0 Command accepted by the controller side.
 *
 * @pre @p buf holds a well-formed HCI command.
 * @pre ble_hci_ra8_ble_init() has succeeded.
 * @post Ownership of @p buf passes to the controller side.
 * @post No state changes when the controller rejects the command.
 *
 * @note Not thread-safe unless the real transport core replaces it.
 *
 * @see ble_transport_to_ll_cmd_impl()
 *
 * @since 0.1.0
 */
int ble_transport_to_ll_cmd(void* buf);

/**
 * @brief Host-to-controller ACL entry point.
 *
 * @details Stub wrapper: forwards straight to
 * ble_transport_to_ll_acl_impl().
 *
 * @param[in] om ACL mbuf chain to send.
 *
 * @return Upstream transport status code.
 * @retval 0 Packet accepted by the controller side.
 *
 * @pre @p om is a valid ACL mbuf chain.
 * @pre ble_hci_ra8_ble_init() has succeeded.
 * @post Ownership of @p om passes to the controller side.
 * @post No state changes when the controller rejects the packet.
 *
 * @note Not thread-safe unless the real transport core replaces it.
 *
 * @see ble_transport_to_ll_acl_impl()
 *
 * @since 0.1.0
 */
int ble_transport_to_ll_acl(struct os_mbuf* om);

/**
 * @brief Bring this port's controller-side transport up.
 *
 * @details Upstream ``transport_impl.h`` calls this once from the
 * transport core's own init. Implemented by ``ble_hci_ra8_ble.c``.
 *
 * @pre ble_hci_ra8_ble_init() has succeeded.
 * @pre The NimBLE port has been initialised.
 * @post The controller side is ready to accept HCI traffic.
 * @post Repeated calls are harmless.
 *
 * @note Not thread-safe; call from the port bring-up path only.
 *
 * @see ble_transport_to_ll_cmd_impl()
 *
 * @since 0.1.0
 */
void ble_transport_ll_init(void);

/**
 * @brief Controller-side handler for an HCI command from the host.
 *
 * @details Hands the command to the RA8 BLE transport. The upstream
 * transport core reaches this through ble_transport_to_ll_cmd(); the
 * weak stub of that wrapper forwards here too, so an app that skips the
 * core still reaches the controller.
 *
 * @param[in] buf HCI command buffer.
 *
 * @return Upstream transport status code.
 * @retval 0 Command handed to the RA8 BLE transport.
 *
 * @pre @p buf holds a well-formed HCI command.
 * @pre ble_hci_ra8_ble_init() has succeeded.
 * @post Ownership of @p buf passes to the RA8 BLE transport.
 * @post No state changes on rejection.
 *
 * @note Not thread-safe; one HCI command in flight at a time.
 *
 * @see ble_transport_to_ll_cmd()
 *
 * @since 0.1.0
 */
int ble_transport_to_ll_cmd_impl(void* buf);

/**
 * @brief Controller-side handler for an ACL packet from the host.
 *
 * @details Hands the packet to the RA8 BLE transport, reached the same
 * two ways as ble_transport_to_ll_cmd_impl().
 *
 * @param[in] om ACL mbuf chain.
 *
 * @return Upstream transport status code.
 * @retval 0 Packet handed to the RA8 BLE transport.
 *
 * @pre @p om is a valid ACL mbuf chain.
 * @pre ble_hci_ra8_ble_init() has succeeded.
 * @post Ownership of @p om passes to the RA8 BLE transport.
 * @post No state changes on rejection.
 *
 * @note Not thread-safe; one ACL packet in flight at a time.
 *
 * @see ble_transport_to_ll_acl()
 *
 * @since 0.1.0
 */
int ble_transport_to_ll_acl_impl(struct os_mbuf* om);

/**
 * @brief Controller-side handler for an ISO packet from the host.
 *
 * @details The RA8 BLE transport carries no isochronous channels, so
 * this rejects every packet rather than silently dropping it.
 *
 * @param[in] om ISO mbuf chain.
 *
 * @return Upstream transport status code.
 * @retval 0 Packet accepted.
 *
 * @pre @p om is a valid ISO mbuf chain.
 * @pre ble_hci_ra8_ble_init() has succeeded.
 * @post Ownership of @p om passes to the RA8 BLE transport.
 * @post No state changes on rejection.
 *
 * @note Not thread-safe; one ISO packet in flight at a time.
 *
 * @see ble_transport_to_ll_acl_impl()
 *
 * @since 0.1.0
 */
int ble_transport_to_ll_iso_impl(struct os_mbuf* om);

#ifdef __cplusplus
}
#endif
