/**
 * @file ra_usb_typec.h
 * @brief USB Type-C and Power Delivery driver public API
 *
 * @details
 * Hand-written driver for the RA8 family `R_USBCC` block, mirroring
 * the surface of FSP `r_usb_typec`. Adds USB Power Delivery 3.0
 * message framing on top of the FSP Type-C-only driver: the public
 * API exposes role configuration, plug-orientation detection,
 * attach status query, and a simple PD message send / receive
 * pipe.
 *
 * The PD message header is laid out per USB PD 3.0 spec section
 * 6.2.1.1 -- 16 bits: msg-type[4..0], port-data-role[5],
 * spec-revision[7..6], port-power-role[8], msg-id[11..9],
 * num-data-objects[14..12], extended[15].
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "ra_err.h"

/* =============================================================================
 * Constants and enums
 * =============================================================================
 */

/**
 * @enum ra_usb_typec_role_t
 * @brief Type-C port role.
 *
 * @details Selected at `init` time by mapping to the CCC RD/ZOPEN
 * / Rp bits per USB Type-C spec 1.3 sec 4.5.
 */
typedef enum : uint8_t {
  k_ra_usb_typec_role_sink   = 0U, /**< Rd termination on both CC.  */
  k_ra_usb_typec_role_source = 1U, /**< Rp termination on both CC.  */
  k_ra_usb_typec_role_drp    = 2U, /**< Dual-role, alternate Rp/Rd. */
} ra_usb_typec_role_t;

/**
 * @enum ra_usb_typec_orientation_t
 * @brief Plug orientation reported via TCS.PLUG / CC1S / CC2S.
 */
typedef enum : uint8_t {
  k_ra_usb_typec_orientation_unattached   = 0U, /**< Both CC open. */
  k_ra_usb_typec_orientation_cc1_attached = 1U, /**< CC1 has Rp.   */
  k_ra_usb_typec_orientation_cc2_attached = 2U, /**< CC2 has Rp.   */
} ra_usb_typec_orientation_t;

/**
 * @enum ra_usb_typec_attach_status_t
 * @brief Type-C attach state, derived from TCS.CNS.
 */
typedef enum : uint8_t {
  k_ra_usb_typec_attach_nothing          = 0U, /**< Disconnected.  */
  k_ra_usb_typec_attach_present_attached = 1U, /**< Attached SNK.  */
  k_ra_usb_typec_attach_debug_accessory  = 2U, /**< Debug accy.    */
  k_ra_usb_typec_attach_audio_accessory  = 3U, /**< Audio accy.    */
} ra_usb_typec_attach_status_t;

/**
 * @enum ra_usb_typec_pd_msg_type_t
 * @brief USB-PD 3.0 control / data message types (sec 6.3, 6.4).
 *
 * @details Only the five subset values the driver constructs
 * directly are listed. The header field is 5 bits so values
 * 0x00..0x1F are valid; opaque pass-through is supported by
 * casting an arbitrary uint8_t into this enum's underlying type.
 */
typedef enum : uint8_t {
  k_ra_usb_typec_pd_msg_source_caps = 0x01U, /**< Data: Source_Capabilities. */
  k_ra_usb_typec_pd_msg_request     = 0x02U, /**< Data: Request.             */
  k_ra_usb_typec_pd_msg_accept      = 0x03U, /**< Control: Accept.           */
  k_ra_usb_typec_pd_msg_reject      = 0x04U, /**< Control: Reject.           */
  k_ra_usb_typec_pd_msg_ps_rdy      = 0x06U, /**< Control: PS_RDY.           */
} ra_usb_typec_pd_msg_type_t;

/**
 * @enum ra_usb_typec_pd_role_t
 * @brief Port-power-role / port-data-role bit values (PD 3.0 6.2.1.1.4).
 */
typedef enum : uint8_t {
  k_ra_usb_typec_pd_role_sink_or_ufp   = 0U,
  k_ra_usb_typec_pd_role_source_or_dfp = 1U,
} ra_usb_typec_pd_role_t;

/**
 * @enum ra_usb_typec_pd_spec_rev_t
 * @brief Spec-revision bits 7..6 (PD 3.0 6.2.1.1.5).
 */
typedef enum : uint8_t {
  k_ra_usb_typec_pd_spec_rev_1_0 = 0U,
  k_ra_usb_typec_pd_spec_rev_2_0 = 1U,
  k_ra_usb_typec_pd_spec_rev_3_0 = 2U,
} ra_usb_typec_pd_spec_rev_t;

/**
 * @enum ra_usb_typec_pmode_t
 * @brief Source-current detection mode for the MEC.PMODE field.
 */
typedef enum : uint8_t {
  k_ra_usb_typec_pmode_default      = 0U, /**< Default USB only.       */
  k_ra_usb_typec_pmode_default_1_5a = 1U, /**< Default + 1.5 A.        */
  k_ra_usb_typec_pmode_full         = 2U, /**< Default + 1.5 + 3.0 A.  */
} ra_usb_typec_pmode_t;

/**
 * @enum ra_usb_typec_pd_msg_limits_t
 * @brief Maximum data-object payload of a PD message header.
 *
 * @details PD 3.0 6.2.1.1.6 caps NumDataObjects at 7. Each object
 * is 32 bits, so the PD payload is at most 28 bytes plus the
 * 16-bit header.
 */
typedef enum : uint8_t {
  k_ra_usb_typec_pd_max_data_objects = 7U,  /**< NumDataObjects max. */
  k_ra_usb_typec_pd_header_bytes     = 2U,  /**< Header is 16 bits.  */
  k_ra_usb_typec_pd_max_msg_bytes    = 30U, /**< 2 hdr + 7*4 obj.    */
} ra_usb_typec_pd_msg_limits_t;

/**
 * @struct ra_usb_typec_cfg_t
 * @brief Configuration for `ra_usb_typec_init`.
 *
 * @invariant `pmode <= k_ra_usb_typec_pmode_full`.
 */
typedef struct {
  ra_usb_typec_role_t  role;           /**< Initial Type-C role.        */
  ra_usb_typec_pmode_t pmode;          /**< Source-current detect mode. */
  uint8_t              cc_debounce_ms; /**< tCCDebounce in ms.      */
  uint8_t              pd_debounce_ms; /**< tPDDebounce in ms.      */
} ra_usb_typec_cfg_t;

/**
 * @struct ra_usb_typec_pd_msg_t
 * @brief Decoded representation of a USB-PD 3.0 message.
 *
 * @details `header` is the on-wire 16-bit value built per spec
 * 6.2.1.1; `objects` is up to 7 little-endian data objects.
 */
typedef struct {
  uint16_t header;                                      /**< 16-bit PD header.        */
  uint8_t  num_objects;                                 /**< 0..7 data objects.       */
  uint32_t objects[k_ra_usb_typec_pd_max_data_objects]; /**< 32-bit data objects.     */
} ra_usb_typec_pd_msg_t;

/**
 * @struct ra_usb_typec_event_t
 * @brief Argument to a registered Type-C event callback.
 */
typedef struct {
  uint8_t  kind;      /**< 0=attach, 1=detach, 2=pd_msg.  */
  uint16_t pd_header; /**< Set when `kind == 2`.        */
  void*    ctx;       /**< User-supplied opaque context.  */
} ra_usb_typec_event_t;

/** Callback function signature. */
typedef void (*ra_usb_typec_callback_t)(const ra_usb_typec_event_t* event);

/**
 * @enum ra_usb_typec_event_kind_t
 * @brief Values for `ra_usb_typec_event_t::kind`.
 */
typedef enum : uint8_t {
  k_ra_usb_typec_event_attach = 0U, /**< Cable plug attached. */
  k_ra_usb_typec_event_detach = 1U, /**< Cable plug removed.  */
  k_ra_usb_typec_event_pd_msg = 2U, /**< PD message arrived.  */
} ra_usb_typec_event_kind_t;

/* =============================================================================
 * API
 * =============================================================================
 */

/**
 * @brief Power up the Type-C block and configure CC terminations.
 *
 * @details
 * Enables the USBCC MSTP gate, releases the TCC.RESET soft-reset,
 * brings the CC-PHY out of power-down, programs MEC.MODE per
 * `cfg->role`, and unmasks the CC + VBUS interrupt sources. After
 * a successful return the block is in `Unattached.SNK` (sink) or
 * `Unattached.SRC` (source), waiting for plug detection.
 *
 * @param[in] cfg Configuration block, must be non-NULL.
 *
 * @return `ra_err_t` error code.
 * @retval k_ra_ok Type-C block ready.
 * @retval k_ra_err_null_ptr `cfg` was NULL.
 * @retval k_ra_err_invalid_arg `role` or `pmode` out of range.
 * @retval k_ra_err_invalid_state Driver already initialised.
 *
 * @pre `cfg != NULL`, `cfg->role <= k_ra_usb_typec_role_drp`.
 * @pre USBCC clock + MSTP supply path configured by `ra_cgc`.
 *
 * @post On success, `ra_usb_typec_get_attach_status` reads
 *       `k_ra_usb_typec_attach_nothing`.
 * @post `r_usb_typec_regs()->CCC` PDOWN bit cleared.
 *
 * @note Not thread-safe.
 * @see ra_usb_typec_close
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_usb_typec_init(const ra_usb_typec_cfg_t* cfg);

/**
 * @brief Reconfigure the active Type-C role at runtime.
 *
 * @param[in] role New role.
 *
 * @return `ra_err_t` error code.
 * @retval k_ra_ok Role applied.
 * @retval k_ra_err_invalid_state Driver not initialised.
 * @retval k_ra_err_invalid_arg `role` out of range.
 *
 * @pre `ra_usb_typec_init` succeeded.
 * @pre `role <= k_ra_usb_typec_role_drp`.
 *
 * @post MEC.MODE matches the new role.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_usb_typec_set_role(ra_usb_typec_role_t role);

/**
 * @brief Read the current plug orientation.
 *
 * @details Decodes TCS.PLUG, TCS.CC1S and TCS.CC2S to one of the
 * three orientation values.
 *
 * @param[out] out Receives the orientation, must be non-NULL.
 *
 * @return `ra_err_t` error code.
 * @retval k_ra_ok State copied.
 * @retval k_ra_err_null_ptr `out` was NULL.
 * @retval k_ra_err_invalid_state Driver not initialised.
 *
 * @pre `out != NULL`.
 *
 * @post No internal state mutated.
 *
 * @note Re-entrant.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_usb_typec_get_orientation(ra_usb_typec_orientation_t* out);

/**
 * @brief Read the Type-C connection-state-machine status.
 *
 * @param[out] out Receives the attach status, must be non-NULL.
 *
 * @return `ra_err_t` error code.
 * @retval k_ra_ok State copied.
 * @retval k_ra_err_null_ptr `out` was NULL.
 * @retval k_ra_err_invalid_state Driver not initialised.
 *
 * @pre `out != NULL`.
 *
 * @post No internal state mutated.
 *
 * @note Re-entrant.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_usb_typec_get_attach_status(ra_usb_typec_attach_status_t* out);

/**
 * @brief Send a USB-PD 3.0 message on the BMC channel.
 *
 * @details Stages the 16-bit header (LSB first) followed by
 * `num_objects * 4` bytes of payload into the PD FIFO data port,
 * then sets `PDFC.tx_start` to kick off transmission.
 *
 * @param[in] msg PD message to send, must be non-NULL.
 *
 * @return `ra_err_t` error code.
 * @retval k_ra_ok Message queued.
 * @retval k_ra_err_null_ptr `msg` was NULL.
 * @retval k_ra_err_invalid_arg `num_objects > 7`.
 * @retval k_ra_err_invalid_state Driver not initialised.
 *
 * @pre `msg != NULL`.
 * @pre `msg->num_objects <= k_ra_usb_typec_pd_max_data_objects`.
 *
 * @post `PDFC.tx_start` bit set.
 *
 * @note Not thread-safe.
 * @see USB PD 3.0 spec section 6.2.1.1 (header), 6.6 (BMC).
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_usb_typec_pd_send_message(const ra_usb_typec_pd_msg_t* msg);

/**
 * @brief Drain one PD message from the RX FIFO.
 *
 * @param[out] out Receives the decoded message, must be non-NULL.
 *
 * @return `ra_err_t` error code.
 * @retval k_ra_ok Message copied.
 * @retval k_ra_err_no_data RX FIFO was empty.
 * @retval k_ra_err_null_ptr `out` was NULL.
 * @retval k_ra_err_invalid_state Driver not initialised.
 *
 * @pre `out != NULL`.
 *
 * @post `PDFS.rx_avail` is updated by the controller.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_usb_typec_pd_recv_message(ra_usb_typec_pd_msg_t* out);

/**
 * @brief Register the attach / detach / PD-msg event callback.
 *
 * @param[in] fn  Callback function, may be NULL to clear.
 * @param[in] ctx Opaque user context, copied verbatim into events.
 *
 * @return `ra_err_t` error code.
 * @retval k_ra_ok Callback installed.
 * @retval k_ra_err_invalid_state Driver not initialised.
 *
 * @pre `ra_usb_typec_init` succeeded.
 *
 * @post Subsequent events invoke `fn` with `ctx`.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_usb_typec_set_callback(ra_usb_typec_callback_t fn, void* ctx);

/**
 * @brief Power down the Type-C block.
 *
 * @return `ra_err_t` error code.
 * @retval k_ra_ok Block released.
 * @retval k_ra_err_invalid_state Driver was not initialised.
 *
 * @pre `ra_usb_typec_init` succeeded.
 *
 * @post `CCC.PDOWN` set, MEC.GD set, all interrupts disabled.
 * @post Subsequent calls (other than `ra_usb_typec_init`) return
 *       `k_ra_err_invalid_state`.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_usb_typec_close(void);

/**
 * @brief Build a 16-bit USB-PD message header (PD 3.0 sec 6.2.1.1).
 *
 * @details Pure helper, no register access. Exposed for tests and
 * for callers that want to stage their own header.
 *
 * @param[in] msg_type        Message type (5 bits).
 * @param[in] port_data_role  PD 3.0 6.2.1.1.6 bit 5.
 * @param[in] spec_rev        PD 3.0 6.2.1.1.5 bits 7..6.
 * @param[in] port_power_role PD 3.0 6.2.1.1.4 bit 8.
 * @param[in] msg_id          0..7, bits 11..9.
 * @param[in] num_objects     0..7, bits 14..12.
 * @param[in] extended        1 bit, b15.
 *
 * @return The 16-bit header in host endianness.
 *
 * @pre `msg_id <= 7`, `num_objects <= 7`, `extended <= 1`.
 *
 * @post Bit 15 of the result equals `extended`.
 *
 * @note Re-entrant.
 * @since 0.1.0
 */
[[nodiscard]] uint16_t ra_usb_typec_pd_build_header(ra_usb_typec_pd_msg_type_t msg_type,
                                                    ra_usb_typec_pd_role_t     port_data_role,
                                                    ra_usb_typec_pd_spec_rev_t spec_rev,
                                                    ra_usb_typec_pd_role_t     port_power_role,
                                                    uint8_t                    msg_id,
                                                    uint8_t                    num_objects,
                                                    uint8_t                    extended);

#ifdef __cplusplus
}
#endif
