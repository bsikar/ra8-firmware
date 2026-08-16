/**
 * @file ra8_c6_model.h
 * @brief A modelled ESP32-C6 behind the `ra8_c6link` transport seam (#490).
 *
 * @details
 * Not a recorded byte stream. The model *decodes* what the host transmits with
 * the same generated protobuf codec the co-processor runs, and *synthesises*
 * the answer the co-processor would send -- so a request this host encodes
 * wrongly fails to decode here, and an answer it decodes wrongly fails its
 * assertion. A replay of captured bytes cannot give that property.
 *
 * It also models the one thing about a full-duplex link that is easy to get
 * wrong: the co-processor latches its transmit buffer *before* it sees the
 * host's, so an answer can never appear in the same transaction as its
 * question. The transfer row serves the receive side from its queue before it
 * looks at the transmit side, exactly as the silicon does.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stdint.h>

#include "ra8_c6link.h"
#include "ra8_c6link_wifi.h"
#include "ra8_err.h"
#include "ra8_mdl_format.h"
#include "ra8_mdl_protocol.h"

/**
 * @enum ra8_c6_model_const_t
 * @brief Fixture sizes, and the identity the model reports about itself.
 *
 * @details
 * The version and chip id are the ones the bench co-processor reported on
 * 2026-07-28, so a test that asserts them is asserting the same numbers the
 * hardware produced.
 *
 * @invariant ::k_c6m_queue is at least three, so a test can queue two
 *            announcements ahead of an answer.
 * @invariant ::k_c6m_eth_len is below ::k_ra8_c6link_max_payload, so a modelled
 *            frame always fits one transaction.
 *
 * @par Example:
 * @code
 * TEST_ASSERT_EQ(k_c6m_fw_major, fw.major);
 * @endcode
 *
 * @see ra8_c6_model_t
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_c6m_queue                 = 10U,   /**< Frames the model may hold for the host.      */
  k_c6m_seen                  = 16U,   /**< Request ids the model records, in order.     */
  k_c6m_fw_major              = 2U,    /**< Firmware major version reported.             */
  k_c6m_fw_minor              = 12U,   /**< Firmware minor version reported.             */
  k_c6m_fw_patch              = 11U,   /**< Firmware patch version reported.             */
  k_c6m_chip_id               = 0x0DU, /**< `ESP_PRIV_FIRMWARE_CHIP_ESP32C6`.            */
  k_c6m_channel               = 6U,    /**< Channel the modelled AP is on.               */
  k_c6m_reason                = 15U,   /**< 802.11 four-way-handshake timeout.           */
  k_c6m_rssi_mag              = 55U,   /**< Magnitude of the modelled RSSI, in dBm.      */
  k_c6m_eth_len               = 64U,   /**< Length of the modelled Ethernet frame.       */
  k_c6m_wifi_ev               = 2U,    /**< `WIFI_EVENT_STA_START`, as the bench saw it. */
  k_c6m_bssid_first           = 2U,    /**< First octet of the modelled AP address; the
rest ascend from it.                        */
  k_c6m_mac_first             = 9U,    /**< First octet of the modelled station address;
the rest descend from it.                   */
  k_c6m_eth_first             = 0x80U, /**< First octet of the modelled 802.3 frame.      */
  k_c6m_caps_bytes            = 17U,   /**< Octets in the host-capabilities announcement. */
  k_c6m_mdl_digest_fill       = 0xA5U, /**< Deterministic media digest test octet.        */
  k_c6m_custom_response_bytes = 1200U, /**< CustomRpc response scratch capacity.          */
} ra8_c6_model_const_t;

/**
 * @enum ra8_c6_model_err_t
 * @brief The failure code the model answers with when told to refuse.
 *
 * @details
 * A negative `esp_err_t`, which is what the far side really returns. Kept as a
 * named value so a test asserting on the fault slot is comparing against the
 * same constant the model transmitted.
 *
 * @invariant It is non-zero, so it is distinguishable from success.
 * @invariant It is negative, matching ESP-IDF's convention.
 *
 * @par Example:
 * @code
 * TEST_ASSERT_EQ(k_c6m_esp_fail, fault.resp);
 * @endcode
 *
 * @see ra8_c6_model_t
 * @since 0.1.0
 */
typedef enum : int32_t {
  k_c6m_esp_fail = -1, /**< The refusal code the model reports. */
} ra8_c6_model_err_t;

/** @brief Test-only corruption applied to the next media response. */
typedef enum : uint8_t {
  k_c6m_mdl_fault_none = 0U,             /**< Leave the next chunk unchanged.   */
  k_c6m_mdl_fault_complete_no_sha,       /**< Omit the terminal digest.         */
  k_c6m_mdl_fault_complete_bad_total,    /**< Corrupt the terminal total.       */
  k_c6m_mdl_fault_failed,                /**< Emit a coherent failure.          */
  k_c6m_mdl_fault_failed_zero_status,    /**< Emit failure with zero status.    */
  k_c6m_mdl_fault_cancelled,             /**< Emit coherent cancellation.       */
  k_c6m_mdl_fault_cancelled_with_data,   /**< Attach data to cancellation.      */
  k_c6m_mdl_fault_downloading_error,     /**< Attach error to active data.      */
  k_c6m_mdl_fault_out_of_order,          /**< Increment the response sequence.  */
  k_c6m_mdl_fault_corrupt_data,          /**< Corrupt one data response byte.   */
  k_c6m_mdl_fault_unknown_field,         /**< Append an unknown protobuf field. */
  k_c6m_mdl_fault_accepted_bad_version,  /**< Change the accepted protocol.     */
  k_c6m_mdl_fault_accepted_zero_job,     /**< Clear the accepted job id.        */
  k_c6m_mdl_fault_accepted_zero_max,     /**< Clear the accepted chunk cap.     */
  k_c6m_mdl_fault_accepted_large_max,    /**< Exceed the client chunk cap.      */
  k_c6m_mdl_fault_accepted_wrong_format, /**< Echo a different artifact format.
                                          */
  k_c6m_mdl_fault_response_no_body,      /**< Omit the outer custom body.     */
  k_c6m_mdl_fault_response_wrong_id,     /**< Corrupt the outer operation id. */
  k_c6m_mdl_fault_response_empty_data,   /**< Omit the inner response bytes.  */
} ra8_c6_model_mdl_fault_t;

/**
 * @struct ra8_c6_model
 * @brief The modelled co-processor's whole observable state.
 *
 * @details
 * A test scripts the model by writing the fields above the observation ones,
 * then asserts on what it recorded. Everything is plain data: there is one
 * instance, reached through ::ra8_c6_model.
 *
 * @invariant `head` never exceeds `tail`, and `tail` never exceeds
 *            ::k_c6m_queue.
 * @invariant `seen_n` never exceeds ::k_c6m_seen.
 *
 * @par Example:
 * @code
 * ra8_c6_model()->fail_req = (uint32_t)RPC_ID__Req_SetWifiMode;
 * @endcode
 *
 * @see ra8_c6_model_reset
 * @since 0.1.0
 */
typedef struct ra8_c6_model {
  bool                     handshake;        /**< What HANDSHAKE reads.                      */
  bool                     fail_transfer;    /**< Make every transfer report a bus fault.    */
  uint32_t                 fail_req;         /**< Request id to refuse, or zero for none.    */
  ra8_c6_model_mdl_fault_t mdl_fault;        /**< Corrupt the next modelled media response.  */
  bool                     wrong_uid;        /**< Answer with a UID the host did not send.   */
  bool                     wrong_id;         /**< Answer with a message id nobody asked for. */
  bool                     mute;             /**< Answer nothing at all.                     */
  bool                     silent_boot;      /**< Do not answer the announcement.            */
  uint16_t                 transfers;        /**< Transactions the host has clocked.         */
  uint32_t                 delays;           /**< Times the host asked the seam to wait.     */
  uint16_t                 mdl_cancels;      /**< Media cancel operations accepted.          */
  ra8_mdl_format_t         mdl_format;       /**< Artifact identity observed by the service. */
  uint16_t                 last_delay_ms;    /**< Milliseconds the newest wait asked for.    */
  uint32_t                 seen[k_c6m_seen]; /**< Request ids observed, in order.            */
  uint8_t                  seen_n;           /**< Entries in `seen`.                         */
  /** SSID the host configured. */
  char    ssid[k_ra8_c6link_ssid_max + 1U];
  uint8_t ssid_len; /**< Its length as received. */
  /** Passphrase received. */
  char     pass[k_ra8_c6link_pass_max + 1U];
  uint8_t  pass_len;               /**< Its length as received.                   */
  bool     caps_seen;              /**< The host announced itself on ESP_PRIV_IF. */
  uint8_t  caps[k_c6m_caps_bytes]; /**< The announcement's octets, as received.   */
  uint8_t  caps_len;               /**< Octets of `caps` the host sent.           */
  uint16_t eth_tx_len;             /**< Length of the last 802.3 frame sent up.   */
  uint8_t  eth_tx[k_c6m_eth_len];  /**< Its leading octets.                       */
  /** Frames to send. */
  uint8_t queue[k_c6m_queue][k_ra8_c6link_frame_bytes];
  /** Next queue slot to transmit. */
  uint8_t head;
  /** Next free queue slot. */
  uint8_t tail;
} ra8_c6_model_t;

/**
 * @brief Reach the one modelled co-processor.
 * @details Exposes the process-lifetime singleton used for both scripted inputs
 * and observations made by the modelled transport.
 * @return Pointer to the model; never null.
 * @retval non-NULL The singleton, valid for the life of the test binary.
 * @pre None; the model exists before `main` runs.
 * @pre The caller has called ::ra8_c6_model_reset if it wants a clean slate.
 * @post No state is modified.
 * @post The same pointer is returned on every call.
 * @note Not thread-safe; the host tests are single-threaded.
 * @since 0.1.0
 */
ra8_c6_model_t* ra8_c6_model(void);

/**
 * @brief Clear the model and arm HANDSHAKE.
 * @details Restores deterministic transport defaults, rebinds the built-in
 * media artifact, and discards all queued frames and observations.
 * @return Nothing.
 * @pre No link is mid-transaction against the model.
 * @pre The caller re-opens its link afterwards if it held one.
 * @post Every scripted behaviour and every observation is cleared.
 * @post HANDSHAKE reads active, which is the normal case.
 * @note Not thread-safe.
 * @since 0.1.0
 */
void ra8_c6_model_reset(void);

/**
 * @brief Bind caller-owned bytes and their SHA-256 to the modelled C6 service.
 * @details Replaces the default six-byte media source until the next model
 * reset. The model borrows both spans and serves them through the real portable
 * C6 service dispatcher, so RA-side tests can exercise arbitrarily generated
 * artifacts without a second protocol fake.
 * @param[in] data Immutable source bytes.
 * @param[in] len Nonzero readable source length.
 * @param[in] sha256 Digest of exactly @p data.
 * @return Binding status.
 * @retval k_ra8_ok The next Start reads the supplied artifact.
 * @retval k_ra8_err_null_ptr A required pointer is null.
 * @retval k_ra8_err_invalid_size @p len is zero.
 * @pre The supplied spans outlive every media Start/Next/Cancel exchange.
 * @pre No modelled media job is active while the binding changes.
 * @post Success resets the source read offset to zero.
 * @post Failure leaves the prior source binding unchanged.
 * @note Test-only, no allocation, not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_c6_model_mdl_source(const uint8_t* data,
                                                uint32_t       len,
                                                const uint8_t  sha256[k_ra8_mdl_sha256_bytes]);

/**
 * @brief Fill a transport seam with the model's rows.
 * @details Connects transfer, handshake, and delay callbacks to the singleton
 * context without opening a production transport.
 * @param[out] out Seam to fill; must be non-null.
 * @return Nothing.
 * @pre ::ra8_c6_model_reset has run.
 * @pre @p out is otherwise unused.
 * @post All three rows and the context are set.
 * @post No model state is modified.
 * @note Not thread-safe.
 * @since 0.1.0
 */
void ra8_c6_model_bind(ra8_c6link_transport_t* out);

/**
 * @brief Reserve the next queue slot for a hand-built frame.
 * @details Advances the bounded producer index and clears the returned frame so
 * callers never inherit bytes from a prior model exchange.
 * @return The slot, ::k_ra8_c6link_frame_bytes long, or null when full.
 * @retval NULL The queue is full; the test has queued too much.
 * @pre The caller fills the whole slot, including its payload header.
 * @pre ::ra8_c6_model_reset has run.
 * @post The queue is one slot longer.
 * @post The slot's previous contents are the caller's to overwrite.
 * @note Used by tests that need a frame the model would never send.
 * @since 0.1.0
 */
uint8_t* ra8_c6_model_slot(void);

/**
 * @brief Queue the boot announcement the co-processor sends once per power-up.
 * @details Constructs the production ESP-init event shape and frames it through
 * the same encoder used by every other modelled announcement.
 * @return Nothing.
 * @pre The queue has room.
 * @pre ::ra8_c6_model_reset has run.
 * @post One `Event_ESPInit` frame is queued.
 * @post No other model state is modified.
 * @note This is the usable boot signal; the privileged INIT frame upstream
 *       normally uses is unusable on this co-processor build (#529).
 * @since 0.1.0
 */
void ra8_c6_model_emit_boot(void);

/**
 * @brief Queue a station-connected announcement naming the AP it reached.
 * @details Supplies deterministic SSID, BSSID, channel, authentication, and AP
 * identifier fields for decoder and state-update assertions.
 * @return Nothing.
 * @pre The queue has room.
 * @pre ::ra8_c6_model_reset has run.
 * @post One `Event_StaConnected` frame is queued.
 * @post No other model state is modified.
 * @note Carries an SSID, a BSSID and a channel, so the decoder's copy paths
 *       are exercised rather than only its dispatch.
 * @since 0.1.0
 */
void ra8_c6_model_emit_connected(void);

/**
 * @brief Queue a bare Wi-Fi event, the kind that carries only its own id.
 * @details Exercises the no-payload Wi-Fi event arm with a deterministic event
 * identifier and the production serial framing path.
 * @return Nothing.
 * @pre The queue has room.
 * @pre ::ra8_c6_model_reset has run.
 * @post One `Event_WifiEventNoArgs` frame is queued.
 * @post No other model state is modified.
 * @note This is the shape the co-processor actually raises most often -- the
 *       bench run saw `WIFI_EVENT_STA_START` and `WIFI_EVENT_STA_STOP` arrive
 *       this way.
 * @since 0.1.0
 */
void ra8_c6_model_emit_wifi_event(void);

/**
 * @brief Queue a station-disconnected announcement with a reason code.
 * @details Builds the nested disconnect payload so the host must decode and
 * preserve its reason field rather than treating it as a bare event.
 * @return Nothing.
 * @pre The queue has room.
 * @pre ::ra8_c6_model_reset has run.
 * @post One `Event_StaDisconnected` frame is queued.
 * @post No other model state is modified.
 * @note The reason code is the actionable field for an IP driver.
 * @since 0.1.0
 */
void ra8_c6_model_emit_disconnected(void);

/**
 * @brief Queue an 802.3 frame as if the AP had forwarded one to the station.
 * @details Writes a recognizable bounded byte ramp behind the station-interface
 * frame header for data-plane delivery checks.
 * @return Nothing.
 * @pre The queue has room.
 * @pre ::ra8_c6_model_reset has run.
 * @post One station-interface data frame is queued.
 * @post No other model state is modified.
 * @note The payload is a recognisable ramp so a test can check it arrived.
 * @since 0.1.0
 */
void ra8_c6_model_emit_eth(void);

/**
 * @brief Queue the two association announcements with their bodies absent.
 * @details Deliberately omits each optional nested protobuf message while
 * keeping the surrounding event envelopes well formed.
 * @return Nothing.
 * @pre The queue has room for two frames.
 * @pre ::ra8_c6_model_reset has run.
 * @post Two `Event_Sta*` frames are queued, each with a null inner message.
 * @post No other model state is modified.
 * @note protobuf makes the inner message optional, so a co-processor that omits
 *       it is a legal sender. The decoder must report the kind and leave every
 *       other field zero rather than dereference the absent body.
 * @since 0.1.0
 */
void ra8_c6_model_emit_hollow_events(void);

/**
 * @brief Queue an announcement whose id this facade does not model.
 * @details Uses a valid protocol event identifier outside the facade's modeled
 * set to exercise its ignore-without-corruption path.
 * @return Nothing.
 * @pre The queue has room.
 * @pre ::ra8_c6_model_reset has run.
 * @post One `Event_Heartbeat` frame is queued, carrying no payload.
 * @post No other model state is modified.
 * @note The protocol defines far more events than a station join needs. One it
 *       does not model must be dropped silently, not half-decoded into a record
 *       no caller can interpret.
 * @since 0.1.0
 */
void ra8_c6_model_emit_unmodelled_event(void);

/**
 * @brief Queue a REQUEST, which a host must never be sent.
 * @details Frames a syntactically valid inbound request so direction validation
 * is tested independently of protobuf and checksum validation.
 * @return Nothing.
 * @pre The queue has room.
 * @pre ::ra8_c6_model_reset has run.
 * @post One well-formed frame carrying `RPC_TYPE__Req` is queued.
 * @post No other model state is modified.
 * @note A request arriving at a host is a co-processor defect, not a message
 *       this side has any handler for. It must be counted and dropped, never
 *       acted on -- which is a different outcome from a frame that failed to
 *       decode, and this is how the two are told apart.
 * @since 0.1.0
 */
void ra8_c6_model_emit_inbound_request(void);

/**
 * @brief Queue an answer the host never asked for.
 * @details Builds a valid response with no matching outstanding UID to exercise
 * unsolicited-response accounting and rejection.
 * @return Nothing.
 * @pre The queue has room.
 * @pre ::ra8_c6_model_reset has run.
 * @post One unsolicited `Resp_GetCoprocessorFwVersion` frame is queued.
 * @post No other model state is modified.
 * @note Exists to prove the link counts and drops an answer that correlates
 *       with no outstanding request.
 * @since 0.1.0
 */
void ra8_c6_model_emit_stray(void);
