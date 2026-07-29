/**
 * @file ra8_c6link.h
 * @brief The single integration boundary between this firmware and the ESP32-C6.
 * @ingroup grp_net
 *
 * @par Tag
 * [Ring 4 / PAL] {World: NS}
 *
 * @details
 * Application code reaches the co-processor here and nowhere else. Above this
 * header there is no protobuf, no TLV envelope, no payload header, no checksum
 * and no SPI: there is a link you open, poll, send Ethernet frames on, and
 * receive events from. Below it, ::ra8_c6link_transport_t is the only thing
 * that touches hardware.
 *
 * @par What the wire actually is, and why this API looks like this
 * The esp-hosted control plane is one protobuf message type (`Rpc`) inside a
 * two-tag TLV envelope inside a twelve-byte payload header, and the data plane
 * is a bare 802.3 frame inside that same payload header. The co-processor
 * decodes **protobuf**, not C structures: `WifiStaConfig` is a message with
 * named fields and upstream's own host converts to it field by field, so no
 * struct layout, field order or padding on this side ever reaches the C6.
 * That was measured on the bench before this library was written (see #490),
 * and it is why this file defines the handful of small types a caller genuinely
 * needs rather than reproducing ESP-IDF's `wifi_*_t` surface for an ABI
 * compatibility the link does not have.
 *
 * @par Liveness is measured, never inferred
 * Neither boot announcement is load-bearing here. The `ESP_PRIV_IF`
 * `ESP_PRIV_EVENT_INIT` frame upstream normally uses is unusable on this
 * co-processor build -- transmitted with a non-zero `if_num` but checksummed as
 * if that nibble were zero, so every conformant host drops it (#529) -- and the
 * `Event_ESPInit` RPC event that replaces it is a **one-shot fired when the
 * co-processor boots**, which on this bench is not when the RA8 boots: the C6
 * has its own supply. Both are reported (::k_ra8_c6link_event_boot) and neither
 * is a precondition. ::ra8_c6link_await_ready establishes liveness by asking a
 * question and getting an answer, which is true whenever it is true.
 *
 * @par Threading
 * A link handle is single-threaded. Every entry point that can move bytes
 * (::ra8_c6link_poll and everything built on it) owns the transport for its
 * duration, so two threads must not share one handle without external
 * serialisation. Callbacks run inside those calls, on the calling thread.
 *
 * @par Example:
 * @code
 * static ra8_c6link_t     s_link;
 * static uint8_t          s_arena[k_ra8_c6link_arena_min];
 *
 * ra8_c6link_cfg_t cfg = {
 *   .transport   = seam,
 *   .arena       = s_arena,
 *   .arena_bytes = (uint32_t)sizeof s_arena,
 *   .event_cb    = on_event,
 *   .cb_ctx      = nullptr,
 * };
 * if (ra8_c6link_open(&s_link, &cfg) == k_ra8_ok) {
 *   ra8_c6link_fw_version_t fw = {};
 *   (void)ra8_c6link_fw_version(&s_link, &fw);
 * }
 * @endcode
 *
 * @see ra8_c6link_wifi.h  Station bring-up and association on top of this link
 * @see ra8_c6link_transport.h  The hardware seam this facade is built on
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 *
 * @since 0.1.0
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "ra8_c6link_transport.h"
#include "ra8_err.h"

/**
 * @enum ra8_c6link_geometry_t
 * @brief Fixed sizes of the esp-hosted wire this link speaks.
 *
 * @details
 * These are protocol facts, not tuning knobs. Each is cross-checked against the
 * vendored esp-hosted headers by a `static_assert` in `ra8_c6link_frame.c`, so
 * an upstream change breaks the build here instead of silently mis-framing.
 * They are restated rather than included so that this public header -- and
 * therefore every consumer of the facade, including the NetX Duo glue -- needs
 * no esp-hosted include path at all.
 *
 * @invariant ::k_ra8_c6link_frame_bytes is the byte count clocked in one
 *            full-duplex transaction, in both directions, always.
 * @invariant ::k_ra8_c6link_max_payload is
 *            ::k_ra8_c6link_frame_bytes - ::k_ra8_c6link_header_bytes.
 *
 * @par Example:
 * @code
 * static uint8_t frame[k_ra8_c6link_frame_bytes];
 * @endcode
 *
 * @see ra8_c6link_transport_t
 * @since 0.1.0
 */
typedef enum : uint16_t {
  k_ra8_c6link_frame_bytes = 1600U,
  /**< Bytes in one transaction; `ESP_TRANSPORT_SPI_MAX_BUF_SIZE` upstream. */
  k_ra8_c6link_header_bytes = 12U,
  /**< `sizeof(struct esp_payload_header)` on this ABI. */
  k_ra8_c6link_max_payload = 1588U,
  /**< Largest payload one frame can carry. */
  k_ra8_c6link_mac_bytes = 6U,
  /**< Octets in an IEEE 802 address. */
  k_ra8_c6link_ssid_max = 32U,
  /**< Octets in the longest SSID 802.11 allows. */
  k_ra8_c6link_dma_align = 64U,
  /**< Alignment the transport expects of a transaction buffer. The handle's
       two frames carry it so a DMA-capable backend can clock them without a
       bounce buffer. */
  k_ra8_c6link_arena_min = 2048U,
  /**< Smallest decode arena ::ra8_c6link_open accepts. The largest message
       this library decodes is the station's AP record, whose nested country
       and HE sub-messages plus their binary fields fit inside this with room
       to spare; a smaller arena would fail a decode at run time rather than
       at open, which is the wrong place to find out. */
} ra8_c6link_geometry_t;

/**
 * @enum ra8_c6link_budget_t
 * @brief Default pacing and bounds the link works to.
 *
 * @details
 * Every one of these exists to give a loop a statically provable bound (NASA
 * Power of 10 Rule 2) and to turn "the co-processor never answered" into a
 * returned error instead of a hang. The transaction rate is the bench-proven
 * one: `c6_hosted_init` and `c6_fw_version` both ran the link at 5 MHz with a
 * two-millisecond gap between transactions and zero bad checksums.
 *
 * @invariant ::k_ra8_c6link_hs_giveup is at least two, so one missed sample is
 *            never enough to declare the co-processor absent.
 * @invariant ::k_ra8_c6link_rpc_transfers x ::k_ra8_c6link_hs_wait_ms bounds
 *            the worst-case wall time of one RPC call.
 *
 * @par Example:
 * @code
 * (void)ra8_c6link_poll(&link, (uint16_t)k_ra8_c6link_rpc_transfers, &stats);
 * @endcode
 *
 * @see ra8_c6link_poll
 * @since 0.1.0
 */
typedef enum : uint16_t {
  k_ra8_c6link_hs_wait_ms = 200U,
  /**< Milliseconds to wait for HANDSHAKE before abandoning a transaction. */
  k_ra8_c6link_hs_poll_ms = 1U,
  /**< Sampling period while waiting for HANDSHAKE. */
  k_ra8_c6link_hs_giveup = 3U,
  /**< Consecutive HANDSHAKE timeouts after which a pump stops early. A
       co-processor that has not armed the line three times running is not
       there, and spending the rest of the budget re-asking turns "absent"
       into a minutes-long hang. */
  k_ra8_c6link_gap_ms = 2U,
  /**< Settling gap between consecutive transactions. */
  k_ra8_c6link_rpc_transfers = 64U,
  /**< Transactions one RPC call may clock while waiting for its response. */
  k_ra8_c6link_announce_transfers = 8U,
  /**< Transactions ::ra8_c6link_await_ready spends on the announcement before
       it probes. Enough to place the capabilities frame and drain whatever the
       co-processor volunteers behind it; the bench bring-up settled on the same
       figure. Readiness is decided by the probe that follows, not by this
       budget, so a larger one would only lengthen the absent-hardware case. */
} ra8_c6link_budget_t;

/**
 * @struct ra8_c6link_mac
 * @brief One IEEE 802 address, by value.
 *
 * @details
 * A named type rather than a bare `uint8_t[6]` so it can be returned, copied
 * and compared without an array-decay parameter that loses its length.
 *
 * @invariant Exactly ::k_ra8_c6link_mac_bytes octets, in transmission order.
 * @invariant An all-zero value is never a valid station address, so it doubles
 *            as the "not yet read" sentinel.
 *
 * @par Example:
 * @code
 * ra8_c6link_mac_t mac = {};
 * (void)ra8_c6link_wifi_mac(&link, &mac);
 * @endcode
 *
 * @see ra8_c6link_wifi_mac
 * @since 0.1.0
 */
typedef struct ra8_c6link_mac {
  uint8_t octet[k_ra8_c6link_mac_bytes]; /**< Address octets, first on the wire first. */
} ra8_c6link_mac_t;

/**
 * @struct ra8_c6link_fw_version
 * @brief Identity the co-processor reports about itself.
 *
 * @details
 * Filled by ::ra8_c6link_fw_version. The version is the host/co-processor
 * version lock: this host is built against a pinned esp-hosted commit, and a
 * co-processor running a different one may encode a message differently.
 *
 * @invariant `target_len` never exceeds the capacity of `target`.
 * @invariant `chip_id` is the co-processor's `CONFIG_IDF_FIRMWARE_CHIP_ID`;
 *            0x0D is the ESP32-C6 this board carries.
 *
 * @par Example:
 * @code
 * ra8_c6link_fw_version_t fw = {};
 * if (ra8_c6link_fw_version(&link, &fw) == k_ra8_ok) { use(fw.major); }
 * @endcode
 *
 * @see ra8_c6link_fw_version
 * @since 0.1.0
 */
typedef struct ra8_c6link_fw_version {
  uint32_t major;      /**< Firmware major version.                    */
  uint32_t minor;      /**< Firmware minor version.                    */
  uint32_t patch;      /**< Firmware patch version.                    */
  uint32_t chip_id;    /**< Co-processor chip identifier.              */
  char     target[16]; /**< `CONFIG_IDF_TARGET`, NUL-terminated.       */
  uint8_t  target_len; /**< Characters in `target`, excluding the NUL. */
} ra8_c6link_fw_version_t;

/**
 * @enum ra8_c6link_event_kind_t
 * @brief The co-processor announcements this link surfaces to its owner.
 *
 * @details
 * Deliberately short. These are the four unsolicited messages a station join
 * and its supervision actually depend on; every other event the protocol
 * defines is counted as unhandled rather than half-decoded into a type nobody
 * consumes.
 *
 * @invariant ::k_ra8_c6link_event_boot is the only announcement that can
 *            arrive before any request has been sent.
 * @invariant Every kind maps one-to-one onto a generated `RPC_ID__Event_*`.
 *
 * @par Example:
 * @code
 * if (ev->kind == k_ra8_c6link_event_sta_disconnected) { link_down(); }
 * @endcode
 *
 * @see ra8_c6link_event_t
 * @since 0.1.0
 */
typedef enum : uint8_t {
  k_ra8_c6link_event_boot = 0U,
  /**< `Event_ESPInit`: the co-processor has booted and is accepting RPC. */
  k_ra8_c6link_event_sta_connected = 1U,
  /**< `Event_StaConnected`: the station has associated with an AP. */
  k_ra8_c6link_event_sta_disconnected = 2U,
  /**< `Event_StaDisconnected`: the station has lost or left its AP. */
  k_ra8_c6link_event_wifi = 3U,
  /**< `Event_WifiEventNoArgs`: a Wi-Fi event carrying only its own id,
       reported verbatim in ::ra8_c6link_event::wifi_event_id. */
} ra8_c6link_event_kind_t;

/**
 * @struct ra8_c6link_event
 * @brief One decoded co-processor announcement.
 *
 * @details
 * A flat record rather than a union: the whole thing is 48 bytes, it is passed
 * by const pointer into a callback that must not keep it, and a union would buy
 * nothing but a discriminant nobody can forget to check.
 *
 * Which fields carry meaning depends on `kind`:
 * `boot` sets `reset_reason`; `sta_connected` sets `ssid`, `ssid_len`, `bssid`
 * and `channel`; `sta_disconnected` sets `ssid`, `ssid_len`, `bssid`, `reason`
 * and `rssi`; `wifi` sets `wifi_event_id`. Every other field is zero.
 *
 * @invariant `ssid_len` never exceeds ::k_ra8_c6link_ssid_max.
 * @invariant `ssid` is NUL-terminated at `ssid_len`, so it is printable
 *            directly even when the co-processor sent no terminator.
 *
 * @par Example:
 * @code
 * static void on_event(void* ctx, const ra8_c6link_event_t* ev)
 * {
 *   (void)ctx;
 *   if (ev->kind == k_ra8_c6link_event_boot) { announce(); }
 * }
 * @endcode
 *
 * @see ra8_c6link_event_cb_t
 * @since 0.1.0
 */
typedef struct ra8_c6link_event {
  ra8_c6link_event_kind_t kind;          /**< Which announcement this is.             */
  uint8_t                 ssid_len;      /**< Octets of `ssid` the C6 supplied.       */
  uint8_t                 channel;       /**< Primary channel, `sta_connected`.       */
  int8_t                  rssi;          /**< Signal level, `sta_disconnected`.       */
  uint16_t                reason;        /**< 802.11 reason code, `sta_disconnected`. */
  int32_t                 wifi_event_id; /**< Raw id, `wifi`.                         */
  uint32_t                reset_reason;  /**< Co-processor reset cause, `boot`.       */
  ra8_c6link_mac_t        bssid;         /**< AP address for the two station events.  */
  char                    ssid[k_ra8_c6link_ssid_max + 1U]; /**< AP SSID, NUL-terminated. */
} ra8_c6link_event_t;

/**
 * @struct ra8_c6link_fault
 * @brief What the co-processor said when a request did not succeed.
 *
 * @details
 * Recorded per handle by every request this library issues, so a bring-up can
 * print *which* request failed and *what the far side thought* rather than one
 * opaque error code. A successful request clears it.
 *
 * @invariant `rpc_id` is zero exactly when no request has failed since open.
 * @invariant `resp` is the co-processor's own result code, which is an
 *            `esp_err_t` on the far side; zero means it succeeded.
 *
 * @par Example:
 * @code
 * ra8_c6link_fault_t f = {};
 * (void)ra8_c6link_last_fault(&link, &f);
 * @endcode
 *
 * @see ra8_c6link_last_fault
 * @since 0.1.0
 */
typedef struct ra8_c6link_fault {
  uint32_t rpc_id; /**< `RPC_ID__Req_*` of the request that failed, or zero. */
  int32_t  resp;   /**< The co-processor's result code, or zero if none.     */
} ra8_c6link_fault_t;

/**
 * @struct ra8_c6link_stats
 * @brief What one pump did on the wire, counted rather than inferred.
 *
 * @details
 * "The co-processor sent nothing" and "the co-processor sent frames this host
 * rejected" are a bench fault and a firmware fault respectively, and a bare
 * pass/fail cannot tell them apart. Every counter is reported, including the
 * zeroes: a zero `bad_checksum` beside a non-zero `transfers` is itself the
 * evidence that the link is clean.
 *
 * @invariant `data + idle + bad_checksum + malformed` never exceeds
 *            `transfers`: each counted frame came from one transaction.
 * @invariant `transfers` never exceeds the budget the caller passed.
 *
 * @par Example:
 * @code
 * ra8_c6link_stats_t st = {};
 * (void)ra8_c6link_poll(&link, 8U, &st);
 * @endcode
 *
 * @see ra8_c6link_poll
 * @since 0.1.0
 */
typedef struct ra8_c6link_stats {
  uint16_t transfers;    /**< Full-duplex transactions clocked.               */
  uint16_t data;         /**< Well-formed frames carrying a payload.          */
  uint16_t idle;         /**< Filler frames the co-processor returned.        */
  uint16_t bad_checksum; /**< Frames whose recomputed checksum disagreed.     */
  uint16_t malformed;    /**< Frames whose offset or length was not credible. */
  uint16_t rpc_in;       /**< Control-plane frames decoded as `Rpc`.          */
  uint16_t events;       /**< Announcements delivered to the event callback.  */
  uint16_t eth_in;       /**< 802.3 frames delivered to the receive callback. */
  uint16_t undecodable;  /**< Control-plane frames the codec would not parse. */
  uint16_t unrouted;     /**< Frames on an interface this link does not use.  */
  uint16_t hs_timeouts;  /**< Transactions abandoned waiting for HANDSHAKE.   */
} ra8_c6link_stats_t;

/**
 * @typedef ra8_c6link_event_cb_t
 * @brief Receiver for one decoded co-processor announcement.
 *
 * @param[in] ctx Opaque context from ::ra8_c6link_cfg::cb_ctx.
 * @param[in] ev Decoded announcement; never null and valid only for the
 *               duration of the call.
 *
 * @return Nothing.
 *
 * @note Runs inside ::ra8_c6link_poll on the polling thread, with the transport
 *       in use. It must not call back into the link.
 * @since 0.1.0
 */
typedef void (*ra8_c6link_event_cb_t)(void* ctx, const ra8_c6link_event_t* ev);

/**
 * @typedef ra8_c6link_rx_cb_t
 * @brief Receiver for one 802.3 frame the co-processor forwarded.
 *
 * @param[in] ctx Opaque context from ::ra8_c6link_cfg::cb_ctx.
 * @param[in] frame Frame bytes, starting at the destination address; never
 *                  null and valid only for the duration of the call.
 * @param[in] len Frame length in bytes; never zero, never above
 *                ::k_ra8_c6link_max_payload.
 *
 * @return Nothing.
 *
 * @note Runs inside ::ra8_c6link_poll on the polling thread, with the transport
 *       in use. Copy anything that must outlive the call.
 * @since 0.1.0
 */
typedef void (*ra8_c6link_rx_cb_t)(void* ctx, const uint8_t* frame, uint16_t len);

/**
 * @struct ra8_c6link_cfg
 * @brief Everything ::ra8_c6link_open needs that the link cannot discover.
 *
 * @details
 * The decode arena is the reason this library needs no heap: the generated
 * protobuf decoder allocates, and it is given a bump allocator over this
 * caller-supplied buffer which is reset after every message. That keeps the
 * whole path inside NASA Power of 10 Rule 3 without borrowing an RTOS pool.
 *
 * @invariant `arena` covers `arena_bytes`, and `arena_bytes` is at least
 *            ::k_ra8_c6link_arena_min.
 * @invariant `event_cb` and `rx_cb` are independently optional; `cb_ctx` is
 *            handed to whichever of them is present.
 *
 * @par Example:
 * @code
 * ra8_c6link_cfg_t cfg = { .transport = seam, .arena = buf,
 *                          .arena_bytes = (uint32_t)sizeof buf };
 * @endcode
 *
 * @see ra8_c6link_open
 * @since 0.1.0
 */
typedef struct ra8_c6link_cfg {
  ra8_c6link_transport_t transport;   /**< The hardware seam; every row required. */
  uint8_t*               arena;       /**< Protobuf decode arena.                 */
  uint32_t               arena_bytes; /**< Bytes available at `arena`.            */
  ra8_c6link_event_cb_t  event_cb;    /**< Announcement sink, or null to count.   */
  ra8_c6link_rx_cb_t     rx_cb;       /**< 802.3 receive sink, or null to count.  */
  void*                  cb_ctx;      /**< Context handed to both callbacks.      */
} ra8_c6link_cfg_t;

/**
 * @typedef ra8_c6link_take_fn_t
 * @brief Extractor that copies an answer's fields out. Treat as private.
 *
 * @details
 * Declared here only because ::ra8_c6link_wait_t must have a complete layout
 * for ::ra8_c6link_t to be caller-allocated. The `msg` argument is really a
 * `const Rpc*` from the generated codec; it is typed opaquely so this public
 * header needs no esp-hosted include path. Only translation units inside
 * `libs/ra8_c6link/src/` ever produce or call one.
 *
 * @param[in] ctx Extractor context supplied alongside the request.
 * @param[in] msg The decoded answer, valid only for the duration of the call.
 *
 * @return ra8_err_t Whatever the extractor decided about the answer.
 * @retval k_ra8_ok The answer was accepted and its fields copied out.
 *
 * @note Runs inside the pump, on the polling thread.
 * @since 0.1.0
 */
typedef ra8_err_t (*ra8_c6link_take_fn_t)(void* ctx, const void* msg);

/**
 * @struct ra8_c6link_wait
 * @brief The one outstanding request a link is waiting on. Treat as private.
 *
 * @details
 * Exposed only because ::ra8_c6link_t is caller-allocated and C requires the
 * layout to be complete. Nothing outside `libs/ra8_c6link/src/` may read or
 * write these fields.
 *
 * @invariant `armed` is true only between staging a request and either
 *            receiving its response or exhausting the budget.
 * @invariant `uid` is unique per request within a link's lifetime.
 *
 * @par Example:
 * @code
 * // Private. Callers use ra8_c6link_fw_version() and friends instead.
 * @endcode
 *
 * @see ra8_c6link_t
 * @since 0.1.0
 */
typedef struct ra8_c6link_wait {
  uint32_t             uid;       /**< UID stamped on the request; echoed back. */
  uint32_t             resp_id;   /**< `RPC_ID__Resp_*` that answers it.        */
  ra8_c6link_take_fn_t take;      /**< Extractor for the answer's fields.       */
  void*                take_ctx;  /**< Context handed to the extractor.         */
  ra8_err_t            result;    /**< What the extractor decided.              */
  bool                 armed;     /**< A request is outstanding.                */
  bool                 satisfied; /**< Its answer arrived and was extracted.    */
} ra8_c6link_wait_t;

/**
 * @struct ra8_c6link
 * @brief Caller-allocated link handle. Treat every field as private.
 *
 * @details
 * Zero-initialise (`= {}`) and pass to ::ra8_c6link_open. The two frame buffers
 * dominate its size (3.2 KB together) and are aligned for the transport's DMA;
 * everything else is bookkeeping. Declare one at file scope rather than on a
 * stack.
 *
 * @invariant `open` is true exactly between a successful ::ra8_c6link_open and
 *            the matching ::ra8_c6link_close.
 * @invariant `tx_len` is non-zero only while a payload is staged and not yet
 *            clocked out.
 *
 * @par Example:
 * @code
 * static ra8_c6link_t s_link;
 * @endcode
 *
 * @see ra8_c6link_open
 * @since 0.1.0
 */
typedef struct ra8_c6link {
  ra8_c6link_transport_t transport;   /**< Bound hardware seam.         */
  ra8_c6link_event_cb_t  event_cb;    /**< Announcement sink, or null.  */
  ra8_c6link_rx_cb_t     rx_cb;       /**< 802.3 receive sink, or null. */
  void*                  cb_ctx;      /**< Context for both callbacks.  */
  uint8_t*               arena;       /**< Protobuf decode arena.       */
  uint32_t               arena_bytes; /**< Bytes available at `arena`.  */
  uint32_t               arena_used;  /**< Bump offset into `arena`.    */
  uint32_t               arena_last;  /**< Newest live block's offset plus one,
                                           or zero when there is none.         */
  uint32_t               next_uid;    /**< UID for the next request issued.   */
  ra8_c6link_wait_t      wait;        /**< The outstanding request, if any.   */
  ra8_c6link_fault_t     fault;       /**< The last failing request.          */
  ra8_c6link_stats_t*    stats;       /**< Counters for the running pump.     */
  uint16_t               tx_len;      /**< Staged payload length, or zero.    */
  uint8_t                tx_if;       /**< Interface the staged payload uses. */
  bool                   open;        /**< The handle is initialised.         */
  bool                   boot_seen;   /**< An `Event_ESPInit` has arrived.    */
  alignas(k_ra8_c6link_dma_align) uint8_t tx[k_ra8_c6link_frame_bytes];
  /**< Transmit transaction. */
  alignas(k_ra8_c6link_dma_align) uint8_t rx[k_ra8_c6link_frame_bytes];
  /**< Receive transaction. */
} ra8_c6link_t;

/**
 * @brief Bind a transport to a link handle and make it usable.
 *
 * @details
 * Validates the seam and the arena, copies both into the handle, and resets
 * every counter. No hardware is touched: bringing the transport itself up (pin
 * routing, bus open, clocking) belongs to the backend that fills the seam, and
 * happens before this call.
 *
 * @param[out] link Handle to initialise; must be non-null.
 * @param[in] cfg Configuration; must be non-null with every transport row
 *                filled and an arena of at least ::k_ra8_c6link_arena_min
 *                bytes.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok The link is open.
 * @retval k_ra8_err_null_ptr @p link, @p cfg, a transport row, or the arena
 *         pointer was null.
 * @retval k_ra8_err_invalid_size `arena_bytes` is below
 *         ::k_ra8_c6link_arena_min.
 * @retval k_ra8_err_invalid_state @p link is already open.
 *
 * @pre The transport backend is already up and can clock a transaction.
 * @pre @p link is zero-initialised, or has been closed.
 * @post On success the handle reports open and every counter is zero.
 * @post On failure @p link is not modified.
 *
 * @note Not thread-safe; open once from a single-threaded bring-up path.
 * @warning The arena and everything the transport seam points at must outlive
 *          the link.
 *
 * @par Example:
 * @code
 * if (ra8_c6link_open(&s_link, &cfg) != k_ra8_ok) { report(); }
 * @endcode
 *
 * @see ra8_c6link_close
 * @since 0.1.0
 *
 * @par NASA Power of 10 Compliance:
 * - Rule 3: no allocation; the arena is the caller's.
 * - Rule 5: four preconditions and two postconditions are checked.
 */
[[nodiscard]] ra8_err_t ra8_c6link_open(ra8_c6link_t* link, const ra8_c6link_cfg_t* cfg);

/**
 * @brief Release a link handle.
 *
 * @details
 * Drops the transport binding, the callbacks and the arena reference, and marks
 * the handle closed. The transport itself is not torn down -- whoever brought it
 * up owns that.
 *
 * @param[in,out] link Handle to release; must be non-null and open.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok The handle is closed.
 * @retval k_ra8_err_null_ptr @p link was null.
 * @retval k_ra8_err_not_initialized @p link was not open.
 *
 * @pre No pump is running against @p link.
 * @pre The caller no longer needs the last fault or counters.
 * @post The handle reports closed.
 * @post No callback registered through @p link is invoked again.
 *
 * @note Not thread-safe; close from the same context that opened.
 *
 * @par Example:
 * @code
 * (void)ra8_c6link_close(&s_link);
 * @endcode
 *
 * @see ra8_c6link_open
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_c6link_close(ra8_c6link_t* link);

/**
 * @brief Report whether a handle is currently open.
 *
 * @param[in] link Handle to inspect; null reads as closed.
 *
 * @return true when ::ra8_c6link_open has succeeded and no close has run since.
 * @retval true The handle is usable.
 * @retval false The handle is null, never opened, or closed.
 *
 * @pre None; safe at any time, including before any open.
 * @pre The caller tolerates a value a concurrent close may stale.
 * @post No state is modified.
 * @post The returned value reflects the flag at the moment of the read.
 *
 * @note Safe from any context; a single aligned load.
 *
 * @par Example:
 * @code
 * if (ra8_c6link_is_open(&s_link)) { (void)ra8_c6link_close(&s_link); }
 * @endcode
 *
 * @see ra8_c6link_open
 * @since 0.1.0
 */
[[nodiscard]] bool ra8_c6link_is_open(const ra8_c6link_t* link);

/**
 * @brief Clock transactions, delivering whatever the co-processor sends.
 *
 * @details
 * The pump. Each iteration waits for HANDSHAKE, transmits either a staged
 * payload or an idle filler frame, clocks ::k_ra8_c6link_frame_bytes both ways,
 * and classifies what came back: filler is counted, malformed and
 * checksum-failing frames are counted and dropped, and well-formed frames are
 * routed by interface -- control plane to the RPC decoder, station traffic to
 * the receive callback.
 *
 * Call it to drive the link when no request is outstanding: to drain events, to
 * receive Ethernet frames, or to give a staged transmit somewhere to go.
 * Requests issued through this library pump internally and need no help.
 *
 * @param[in,out] link Open handle; must be non-null.
 * @param[in] max_transactions Transactions this call may clock; must be
 *                             non-zero.
 * @param[out] stats Counters describing the run, or null to discard them.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok The pump ran; @p stats says what happened.
 * @retval k_ra8_err_null_ptr @p link was null.
 * @retval k_ra8_err_not_initialized @p link is not open.
 * @retval k_ra8_err_invalid_arg @p max_transactions was zero.
 * @retval k_ra8_err_hw_timeout HANDSHAKE never went active, so not one
 *         transaction was clocked.
 * @retval k_ra8_err_spi_error The transport refused a transfer.
 *
 * @pre The transport is up and no other context is driving it.
 * @pre Callbacks registered at open are safe to invoke now.
 * @post At most @p max_transactions transactions were clocked.
 * @post @p stats, when non-null, holds the counts for exactly this call.
 *
 * @note Not thread-safe; one pump at a time owns the transport.
 * @warning Callbacks run inside this call and must not re-enter the link.
 *
 * @par Example:
 * @code
 * ra8_c6link_stats_t st = {};
 * (void)ra8_c6link_poll(&link, 16U, &st);
 * @endcode
 *
 * @see ra8_c6link_stats
 * @since 0.1.0
 *
 * @par NASA Power of 10 Compliance:
 * - Rule 2: the transaction loop is bounded by @p max_transactions and the
 *   handshake wait by ::k_ra8_c6link_hs_wait_ms.
 * - Rule 5: three preconditions and two postconditions are checked.
 */
[[nodiscard]] ra8_err_t
ra8_c6link_poll(ra8_c6link_t* link, uint16_t max_transactions, ra8_c6link_stats_t* stats);

/**
 * @brief Announce this host and prove the co-processor is answering.
 *
 * @details
 * Transmits the privileged host-capabilities frame -- upstream's
 * `send_slave_config()`, byte for byte -- drains the few transactions behind  LEGACY-OK: send_slave_config() is the upstream esp-hosted symbol name
 * it so anything the co-processor volunteers reaches the event callback, and
 * then decides readiness by **asking a question and getting an answer**: one
 * identity exchange, whose reply is handed back in @p out.
 *
 * **Readiness is deliberately not the boot event.** `Event_ESPInit` is emitted
 * once, when the *co-processor* boots. On this bench the ESP32-C6 has its own
 * supply, so resetting the RA8 does not reboot it and that event is long gone
 * -- it was consumed by whichever application was clocking the bus when it
 * fired. A facade that waited for it therefore worked exactly once, on a
 * freshly-flashed co-processor, and timed out on every run after. Waiting on a
 * one-shot announcement to decide a steady-state property is the same defect
 * #529 records against the `ESP_PRIV_IF` frame, wearing the RPC layer's
 * clothes; the fix is to stop inferring liveness and measure it.
 *
 * The announcement is still sent, because a co-processor that *has* just
 * booted services no RPC until the host has introduced itself. Sending it to
 * one that is already up is harmless -- it re-states capabilities that have not
 * changed.
 *
 * @param[in,out] link Open handle; must be non-null.
 * @param[in] max_transactions Transactions the announcement phase may clock;
 *                             must be non-zero. See
 *                             ::k_ra8_c6link_announce_transfers.
 * @param[out] out Receives the co-processor's identity; must be non-null.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok The co-processor answered; @p out is populated.
 * @retval k_ra8_err_null_ptr @p link or @p out was null.
 * @retval k_ra8_err_not_initialized @p link is not open.
 * @retval k_ra8_err_invalid_arg @p max_transactions was zero.
 * @retval k_ra8_err_busy A payload is already staged on @p link.
 * @retval k_ra8_err_invalid_size The capabilities frame would not fit, which
 *         is a build-time impossibility and therefore a corrupted handle.
 * @retval k_ra8_err_timeout The identity request went unanswered.
 * @retval k_ra8_err_hw_timeout The co-processor never armed HANDSHAKE, so no
 *         transaction was clocked.
 * @retval k_ra8_err_spi_error The transport refused a transfer.
 * @retval k_ra8_err_protocol_error The answer arrived malformed.
 *
 * @pre The transport is up.
 * @pre The caller has no payload staged on @p link.
 * @post On success @p out holds the identity and the link is usable.
 * @post At most @p max_transactions transactions were clocked announcing,
 *       plus ::k_ra8_c6link_rpc_transfers probing.
 *
 * @note Not thread-safe; it pumps.
 * @note A boot event that *does* arrive during the announcement phase still
 *       reaches the event callback as ::k_ra8_c6link_event_boot. It is
 *       reportable; it is simply not load-bearing.
 *
 * @par Example:
 * @code
 * ra8_c6link_fw_version_t fw = {};
 * ra8_err_t err = ra8_c6link_await_ready(
 *   &link, (uint16_t)k_ra8_c6link_announce_transfers, &fw);
 * @endcode
 *
 * @see ra8_c6link_fw_version
 * @see ra8_c6link_poll
 * @since 0.1.0
 *
 * @par NASA Power of 10 Compliance:
 * - Rule 2: both phases are bounded by explicit transaction budgets.
 * - Rule 5: four preconditions and two postconditions are checked.
 */
[[nodiscard]] ra8_err_t
ra8_c6link_await_ready(ra8_c6link_t* link, uint16_t max_transactions, ra8_c6link_fw_version_t* out);

/**
 * @brief Ask the co-processor to identify itself.
 *
 * @details
 * Issues `Req_GetCoprocessorFwVersion` and decodes the answer. This is the
 * cheapest complete proof that the whole stack works -- framing, checksum, TLV,
 * protobuf, correlation -- because the answer is a fact the host can check
 * rather than merely receive.
 *
 * @param[in,out] link Open handle; must be non-null.
 * @param[out] out Identity to fill; must be non-null.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok @p out holds the co-processor's answer.
 * @retval k_ra8_err_null_ptr @p link or @p out was null.
 * @retval k_ra8_err_not_initialized @p link is not open.
 * @retval k_ra8_err_busy A request is already outstanding on @p link.
 * @retval k_ra8_err_timeout The co-processor did not answer within the budget.
 * @retval k_ra8_err_hw_timeout The co-processor never armed HANDSHAKE, so no
 *         transaction was clocked.
 * @retval k_ra8_err_protocol_error The answer arrived but reported a failure.
 * @retval k_ra8_err_spi_error The transport refused a transfer.
 *
 * @pre The transport is up.
 * @pre No other request is outstanding on @p link.
 * @post On success every field of @p out is set from the answer.
 * @post On failure the handle's last fault names this request.
 *
 * @note Not thread-safe; it pumps.
 *
 * @par Example:
 * @code
 * ra8_c6link_fw_version_t fw = {};
 * (void)ra8_c6link_fw_version(&link, &fw);
 * @endcode
 *
 * @see ra8_c6link_last_fault
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_c6link_fw_version(ra8_c6link_t* link, ra8_c6link_fw_version_t* out);

/**
 * @brief Hand one 802.3 frame to the co-processor for transmission.
 *
 * @details
 * Stages the frame on the station interface and pumps until a transaction has
 * carried it out. The data plane needs no protobuf: the frame is the payload,
 * behind the same twelve-byte header the control plane uses.
 *
 * @param[in,out] link Open handle; must be non-null.
 * @param[in] frame Frame bytes, starting at the destination address; must be
 *                  non-null.
 * @param[in] len Frame length; must be non-zero and at most
 *                ::k_ra8_c6link_max_payload.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok The frame was clocked out.
 * @retval k_ra8_err_null_ptr @p link or @p frame was null.
 * @retval k_ra8_err_not_initialized @p link is not open.
 * @retval k_ra8_err_invalid_size @p len was zero or above the payload cap.
 * @retval k_ra8_err_busy Another payload is already staged.
 * @retval k_ra8_err_hw_timeout The co-processor never armed HANDSHAKE.
 * @retval k_ra8_err_spi_error The transport refused a transfer.
 *
 * @pre The station has been started; frames sent before that are discarded by
 *      the co-processor.
 * @pre @p len bytes are readable at @p frame.
 * @post On success the frame has left this host.
 * @post Anything the co-processor sent during the same transactions was
 *       delivered to the registered callbacks.
 *
 * @note Not thread-safe; it pumps.
 * @warning There is no transmit queue: one frame is staged at a time, which is
 *          what an IP driver that transmits from one thread needs and no more.
 *
 * @par Example:
 * @code
 * (void)ra8_c6link_eth_send(&link, packet, packet_len);
 * @endcode
 *
 * @see ra8_c6link_rx_cb_t
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_c6link_eth_send(ra8_c6link_t* link, const uint8_t* frame, uint16_t len);

/**
 * @brief Report the last request that failed on this link.
 *
 * @param[in] link Open handle; must be non-null.
 * @param[out] out Fault record to fill; must be non-null.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok @p out holds the record; an all-zero record means no
 *         request has failed since the handle was opened.
 * @retval k_ra8_err_null_ptr @p link or @p out was null.
 *
 * @pre The handle has been opened at least once.
 * @pre The caller reads the record before issuing another request, which would
 *      overwrite it.
 * @post No link state is modified.
 * @post @p out is fully written, including on the no-fault path.
 *
 * @note Safe from any context; it copies two words.
 *
 * @par Example:
 * @code
 * ra8_c6link_fault_t f = {};
 * (void)ra8_c6link_last_fault(&link, &f);
 * @endcode
 *
 * @see ra8_c6link_fault
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_c6link_last_fault(const ra8_c6link_t* link, ra8_c6link_fault_t* out);

#ifdef __cplusplus
}
#endif
