/**
 * @file ra_ptp.h
 * @brief IEEE 1588 Precision Time Protocol (PTP) driver
 *
 * @par Tag
 * [Ring 3 / HAL] {World: NS}
 *
 * @details
 * Public surface for the RA8D2 PTP block. This is a peer of the
 * existing ``ra_eth_gptp`` scaffold (which only owns the GPTP timer
 * lifecycle): ``ra_ptp`` adds the SYNFP / STCA layer that turns the
 * raw timestamp counter into an IEEE 1588-2019 ordinary or boundary
 * clock. The driver exposes role selection, Sync / Announce
 * generation, time get/set, slave servo (offset + rate correction),
 * and a per-message receive callback.
 *
 * IEEE 1588-2019 sec 13 "PTP message formats" defines ten message
 * types; the driver uses the same numeric encoding so a host stack
 * can demultiplex on ``message_type`` directly.
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

/**
 * @enum ra_ptp_msg_type_t
 * @brief IEEE 1588-2019 sec 13.3.2.3 messageType values.
 */
typedef enum : uint8_t {
  k_ra_ptp_msg_sync                  = 0x00U, /**< Sync.                  */
  k_ra_ptp_msg_delay_req             = 0x01U, /**< Delay_Req.             */
  k_ra_ptp_msg_pdelay_req            = 0x02U, /**< Pdelay_Req.            */
  k_ra_ptp_msg_pdelay_resp           = 0x03U, /**< Pdelay_Resp.           */
  k_ra_ptp_msg_follow_up             = 0x08U, /**< Follow_Up.             */
  k_ra_ptp_msg_delay_resp            = 0x09U, /**< Delay_Resp.            */
  k_ra_ptp_msg_pdelay_resp_follow_up = 0x0AU, /**< Pdelay_Resp_Follow_Up. */
  k_ra_ptp_msg_announce              = 0x0BU, /**< Announce.              */
  k_ra_ptp_msg_signaling             = 0x0CU, /**< Signaling.             */
  k_ra_ptp_msg_management            = 0x0DU, /**< Management.            */
} ra_ptp_msg_type_t;

/**
 * @enum ra_ptp_role_t
 * @brief PTP port role per IEEE 1588-2019 sec 9.2.5 portState.
 */
typedef enum : uint8_t {
  k_ra_ptp_role_master            = 0U, /**< Ordinary or boundary master.       */
  k_ra_ptp_role_slave             = 1U, /**< Ordinary slave.                    */
  k_ra_ptp_role_boundary_clock    = 2U, /**< Boundary clock (multi-port).       */
  k_ra_ptp_role_transparent_clock = 3U, /**< Transparent clock (forward only).  */
} ra_ptp_role_t;

/**
 * @enum ra_ptp_clock_class_t
 * @brief IEEE 1588-2019 sec 7.6.2.5 clockClass values used in BMCA.
 */
typedef enum : uint8_t {
  k_ra_ptp_clock_class_primary_ref  = 6U,   /**< Primary reference (GPS).        */
  k_ra_ptp_clock_class_app_specific = 13U,  /**< Application-specific master.    */
  k_ra_ptp_clock_class_holdover     = 52U,  /**< Holdover (degraded).            */
  k_ra_ptp_clock_class_default      = 248U, /**< Default for ordinary clocks.    */
  k_ra_ptp_clock_class_slave_only   = 255U, /**< Slave-only (cannot be master).  */
} ra_ptp_clock_class_t;

/**
 * @enum ra_ptp_sync_interval_t
 * @brief log2 of sync interval in seconds (IEEE 1588-2019 sec 7.7.2.3).
 */
typedef enum : int8_t {
  k_ra_ptp_sync_int_1_128 = -7, /**< 1/128 s. */
  k_ra_ptp_sync_int_1_64  = -6, /**< 1/64 s.  */
  k_ra_ptp_sync_int_1_32  = -5, /**< 1/32 s.  */
  k_ra_ptp_sync_int_1_16  = -4, /**< 1/16 s.  */
  k_ra_ptp_sync_int_1_8   = -3, /**< 1/8 s.   */
  k_ra_ptp_sync_int_1_4   = -2, /**< 1/4 s.   */
  k_ra_ptp_sync_int_1_2   = -1, /**< 1/2 s.   */
  k_ra_ptp_sync_int_1     = 0,  /**< 1 s.     */
  k_ra_ptp_sync_int_2     = 1,  /**< 2 s.     */
  k_ra_ptp_sync_int_4     = 2,  /**< 4 s.     */
  k_ra_ptp_sync_int_8     = 3,  /**< 8 s.     */
} ra_ptp_sync_interval_t;

/**
 * @enum ra_ptp_domain_t
 * @brief IEEE 1588-2019 sec 7.1 named clock domains.
 */
typedef enum : uint8_t {
  k_ra_ptp_domain_default  = 0U,   /**< Default PTP domain.             */
  k_ra_ptp_domain_alt1     = 1U,   /**< Alternate domain 1.             */
  k_ra_ptp_domain_alt2     = 2U,   /**< Alternate domain 2.             */
  k_ra_ptp_domain_alt3     = 3U,   /**< Alternate domain 3.             */
  k_ra_ptp_domain_user_min = 4U,   /**< First user-defined domain.      */
  k_ra_ptp_domain_user_max = 127U, /**< Last user-defined domain.       */
} ra_ptp_domain_t;

/**
 * @enum ra_ptp_limits_t
 * @brief Argument limits for PTP setters.
 */
typedef enum : uint32_t {
  k_ra_ptp_mac_byte_count = 6UL,          /**< IEEE 802 MAC address length. */
  k_ra_ptp_ns_per_sec     = 1000000000UL, /**< Nanoseconds in one second. */
} ra_ptp_limits_t;

/**
 * @struct ra_ptp_cfg_t
 * @brief Static configuration for ``ra_ptp_open``.
 *
 * @details
 * cppcheck cannot see tests/ so it flags every field as unused; each
 * member is read in ``ra_ptp_open`` in ``libs/ra_hal/src/ra_ptp.c``.
 */
/* cppcheck-suppress-begin [unusedStructMember] */
typedef struct {
  uint8_t                domain;        /**< clockDomain (0..127).             */
  ra_ptp_sync_interval_t sync_interval; /**< log2 sync interval in seconds.    */
  uint8_t                mac_addr[k_ra_ptp_mac_byte_count]; /**< Local MAC.    */
  ra_ptp_clock_class_t   clock_class; /**< Advertised clockClass.            */
} ra_ptp_cfg_t;
/* cppcheck-suppress-end [unusedStructMember] */

/**
 * @typedef ra_ptp_msg_fn_t
 * @brief Callback signature for received PTP messages.
 *
 * @param[in] ctx     Context pointer registered via
 *                    ``ra_ptp_attach_message_handler``.
 * @param[in] type    The IEEE 1588 message type that arrived.
 * @param[in] sec     Receive timestamp seconds field.
 * @param[in] nsec    Receive timestamp nanoseconds field.
 */
typedef void (*ra_ptp_msg_fn_t)(void* ctx, ra_ptp_msg_type_t type, uint64_t sec, uint32_t nsec);

/**
 * @brief Power up the PTP block and apply a static configuration.
 *
 * @details
 * Enables the GPTP MSTP gate (HUM Ch 11.2.8 p 446), zeroes the
 * timestamp counter, programmes ``PTP_DOMAIN`` / ``PTP_SYNINT`` and
 * loads the local MAC address into ``PTP_MAC0`` / ``PTP_MAC1``. The
 * port role defaults to master; call ``ra_ptp_set_role`` to change it.
 *
 * @param[in] cfg Static configuration. Must not be ``nullptr``.
 *
 * @retval k_ra_ok                 Driver opened.
 * @retval k_ra_err_null_ptr       ``cfg == nullptr``.
 * @retval k_ra_err_invalid_arg    Domain out of range.
 * @retval k_ra_err_hw_init_failed MSTP enable failed.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_ptp_open(const ra_ptp_cfg_t* cfg);

/**
 * @brief Tear down the PTP block.
 *
 * @retval k_ra_ok          Driver closed.
 * @retval k_ra_err_invalid_state Driver was not open.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_ptp_close(void);

/**
 * @brief Switch the active PTP port role.
 *
 * @param[in] role New role.
 *
 * @retval k_ra_ok                Role applied.
 * @retval k_ra_err_invalid_state Driver not open.
 * @retval k_ra_err_invalid_arg   Role not in ``ra_ptp_role_t``.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_ptp_set_role(ra_ptp_role_t role);

/**
 * @brief Emit a Sync message tagged with the current PTP time.
 *
 * @retval k_ra_ok                Sync queued.
 * @retval k_ra_err_invalid_state Driver not open or not a master.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_ptp_send_sync(void);

/**
 * @brief Emit an Announce message advertising the local clock.
 *
 * @retval k_ra_ok                Announce queued.
 * @retval k_ra_err_invalid_state Driver not open or not a master.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_ptp_send_announce(void);

/**
 * @brief Read the current PTP wall-clock time.
 *
 * @param[out] sec  Seconds field. Must not be ``nullptr``.
 * @param[out] nsec Nanoseconds field (0..1e9-1). Must not be ``nullptr``.
 *
 * @retval k_ra_ok            Time copied.
 * @retval k_ra_err_null_ptr  Either output pointer is ``nullptr``.
 * @retval k_ra_err_invalid_state Driver not open.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_ptp_get_time(uint64_t* sec, uint32_t* nsec);

/**
 * @brief Load the PTP time counter to an absolute value.
 *
 * @param[in] sec  Seconds field.
 * @param[in] nsec Nanoseconds field. Must be < 1e9.
 *
 * @retval k_ra_ok              Time loaded.
 * @retval k_ra_err_invalid_arg ``nsec >= 1e9``.
 * @retval k_ra_err_invalid_state Driver not open.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_ptp_set_time(uint64_t sec, uint32_t nsec);

/**
 * @brief Apply a one-shot offset correction to the PTP counter.
 *
 * @details
 * Implements the IEEE 1588-2019 sec 11.2 "step" stage of the slave
 * servo: subtracts ``delta_ns`` from the counter so that local time
 * tracks the master.
 *
 * @param[in] delta_ns Signed offset in nanoseconds to add to the local
 *                     clock. Positive values move the clock forward.
 *
 * @retval k_ra_ok                Offset applied.
 * @retval k_ra_err_invalid_state Driver not open.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_ptp_adjust_time(int32_t delta_ns);

/**
 * @brief Apply a frequency correction to the local oscillator.
 *
 * @details
 * Implements the IEEE 1588-2019 sec 11.2 "rate" stage of the slave
 * servo. ``ppb`` is signed parts-per-billion: +1000 means run 1 ppm
 * fast.
 *
 * @param[in] ppb Signed parts-per-billion adjustment.
 *
 * @retval k_ra_ok                Rate applied.
 * @retval k_ra_err_invalid_state Driver not open.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_ptp_adjust_rate(int32_t ppb);

/**
 * @brief Register a callback for received PTP messages.
 *
 * @param[in] fn  Callback function (may be ``nullptr`` to clear).
 * @param[in] ctx Opaque context delivered to ``fn``.
 *
 * @retval k_ra_ok Callback installed.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_ptp_attach_message_handler(ra_ptp_msg_fn_t fn, void* ctx);

/**
 * @brief Read the last computed offset-from-master in nanoseconds.
 *
 * @param[out] offset_ns Last computed offset. Must not be ``nullptr``.
 *
 * @retval k_ra_ok            Offset copied.
 * @retval k_ra_err_null_ptr  ``offset_ns == nullptr``.
 * @retval k_ra_err_invalid_state Driver not open.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_ptp_get_offset(int32_t* offset_ns);

/**
 * @brief Internal hook used by the IRQ wrapper (and by tests) to
 *        feed a received-message event into the registered callback.
 *
 * @details
 * This is not part of the IEEE 1588 contract; it exists so the test
 * harness can drive the message dispatcher without simulating a full
 * Ethernet RX engine.
 *
 * @param[in] type Message type.
 * @param[in] sec  Receive timestamp seconds.
 * @param[in] nsec Receive timestamp nanoseconds.
 *
 * @since 0.1.0
 *
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 */
void ra_ptp_dispatch_message(ra_ptp_msg_type_t type, uint64_t sec, uint32_t nsec);

#ifdef __cplusplus
}
#endif
