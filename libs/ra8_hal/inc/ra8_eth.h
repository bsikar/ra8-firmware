/**
 * @file ra8_eth.h
 * @brief Ethernet Switch Module (ESWM) + frame TX/RX driver
 * @ingroup grp_hal_net
 *
 * @par Tag
 * [Ring 3 / HAL] {World: NS}
 *
 * @details
 * Driver for the RA8D2 Layer-3 ESWM block plus the polling-first
 * frame TX / RX path that lifts this driver from "controller bring-
 * up only" to "can move bytes on the wire".
 *
 * The RA8D2 ethernet subsystem is split into five cooperating
 * controller drivers:
 *
 * - ra8_eth   (this file) -- Layer 3 Ethernet Switch Module + NIC API
 * - ra8_eth_mfwd          -- Message Forwarding Engine
 * - ra8_eth_coma          -- Common Agent
 * - ra8_eth_gwca          -- CPU Agent
 * - ra8_eth_gptp          -- Generic PTP Timer
 *
 * All five share the ra8_mstp gate ``k_ra8_mstp_eswm`` and follow the
 * same lifecycle + status + IRQ + power-transition shape.
 *
 * The frame-level surface (``ra8_eth_open``, ``ra8_eth_close``,
 * ``ra8_eth_write``, ``ra8_eth_read``, ``ra8_eth_link_status``,
 * ``ra8_eth_get_stats``) mirrors the FSP ``r_ether`` API simplified
 * for polling-first use: a fixed-size descriptor ring lives in this
 * compilation unit, software bumps a write pointer (TX) and a read
 * pointer (RX), and the EDMAC engine walks the ring on the silicon
 * side. Hardware ownership is encoded by the descriptor TACT/RACT
 * bit per FSP convention.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "ra8_err.h"

/**
 * @typedef ra8_eth_event_fn_t
 * @brief Ethernet switch event callback.
 */
typedef void (*ra8_eth_event_fn_t)(void* ctx, uint32_t status_mask);

/**
 * @brief Initialise the ESWM block (MSTP enable + reset regs).
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_eth_init(void);

/**
 * @brief Tear down the ESWM block.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_eth_deinit(void);

/**
 * @brief Read the ESWM_STS status register.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_eth_get_status(uint32_t* out_mask);

/**
 * @brief Clear bits in ESWM_STS via ESWM_ICLR.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_eth_clear_status(uint32_t mask);

/**
 * @brief Replace the shared Ethernet event callback.
 *
 * @details
 * Stores one callback/context pair for ::ra8_eth_dispatch. Passing NULL for
 * @p fn detaches the current callback. Slot replacement performs no fallible
 * work; callers observe completion when this function returns.
 *
 * @param[in] fn Callback to install, or NULL to detach.
 * @param[in] ctx Context passed to @p fn; ignored while @p fn is NULL.
 *
 * @pre Caller serializes this update with ::ra8_eth_dispatch.
 * @pre @p ctx remains valid for as long as the installed callback uses it.
 * @post Subsequent dispatches observe the supplied callback/context pair.
 * @post Passing NULL for @p fn prevents subsequent callback invocation.
 *
 * @note Not thread-safe; update the slot with Ethernet IRQ delivery masked.
 * @since 0.1.0
 */
void ra8_eth_attach_handler(ra8_eth_event_fn_t fn, void* ctx);

/**
 * @brief Dispatch an ESWM event -- snapshot + fire callback.
 * @since 0.1.0
 *
 * @details See implementation.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 */
void ra8_eth_dispatch(void);

/**
 * @brief Put the ethernet switch into MSTP-gated stop.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_eth_enter_stop(void);

/**
 * @brief Exit MSTP-gated stop.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_eth_exit_stop(void);

/**
 * @enum ra8_eth_mii_port_t
 * @brief ESWM media-interface port selector (m = 0, 1).
 *
 * @details
 * The RA8 ESWM wraps each MAC pin group with a media-interface mux. Port
 * ``m`` maps 1:1 to RMAC``m`` / ETHA``m`` and to the ESWM ``MIICR``m
 * control register plus the ``MIIRR.RGRST``m per-port enable bit. Passed
 * to ::ra8_eth_rgmii_select to route a given port's pins to RGMII.
 *
 * @see ra8_eth_rgmii_select
 */
typedef enum : uint8_t {
  k_ra8_eth_mii_port_0     = 0U, /**< ESWM media port 0 (RMAC0 / ETHA0). */
  k_ra8_eth_mii_port_1     = 1U, /**< ESWM media port 1 (RMAC1 / ETHA1). */
  k_ra8_eth_mii_port_count = 2U, /**< Number of media-interface ports.   */
} ra8_eth_mii_port_t;

/**
 * @brief Select RGMII on a port's ESWM media-interface mux and enable it.
 *
 * @details
 * The chip-generic ESWM RGMII media-select every RA8 RGMII board runs once
 * during Ethernet bring-up. After power-on reset ``MIICR``m.MIISEL reads 0
 * (GMII/MII) and ``MIIRR.RGRST``m reads 0 (RGMII block held in reset), so
 * the RGMII data path is dead until the driver explicitly:
 *
 *   1. Sets ``MIICR``m = MIISEL = RGMII (01b) | TXCIDE (on-chip TX clock
 *      delay), matching the FSP "RGMII + 2 ns TX skew" board profile.
 *   2. Sets ``MIIRR.RGRST``m = 1. HUM Ch 29.2.1.2: RGRSTm is 1 = Enable,
 *      0 = Reset -- an ENABLE bit, not an active-high reset. Without it TXC
 *      is never generated and the RMAC RX state machine is unclocked, so
 *      every RMAC RX counter stays at 0.
 *
 * It previously lived open-coded in the EK-RA8D2 board Ethernet bring-up;
 * the accesses are chip-generic, so they belong here in the HAL. Which port
 * is wired, and any board TX/RX skew choices in the PHY, remain board
 * concerns.
 *
 * @param[in] port ESWM media-interface port to route to RGMII. Must be a
 *                 valid ::ra8_eth_mii_port_t (< ::k_ra8_eth_mii_port_count).
 *
 * @return ::ra8_err_t Result code.
 * @retval k_ra8_ok              Port routed to RGMII and its block enabled.
 * @retval k_ra8_err_invalid_arg ``port`` is out of range.
 *
 * @pre The ESWM module-stop gate (::k_ra8_mstp_eswm) has been released.
 * @pre COMA has been brought up (::ra8_eth_coma_bringup) so the media
 *      window is accessible.
 * @post On success ``MIICR``port = TXCIDE | RGMII and ``MIIRR.RGRST``port = 1.
 * @post On k_ra8_err_invalid_arg no register was written.
 *
 * @note Not thread-safe; call from a single-threaded init context.
 * @note eth is HW-blocked on silicon (issue #21); this path is host-tested
 *       and sim-modeled, not hardware-validated.
 *
 * @par Example:
 * @code
 * (void)ra8_eth_coma_bringup();
 * (void)ra8_eth_rgmii_select(k_ra8_eth_mii_port_1);  // EK-RA8D2 RJ45 = port 1
 * @endcode
 *
 * @see ra8_eth_coma_bringup
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_eth_rgmii_select(ra8_eth_mii_port_t port);

/* -----------------------------------------------------------------------
 * Frame-level NIC API (FSP r_ether shape).
 * -------------------------------------------------------------------- */

/**
 * @enum ra8_eth_limits_t
 * @brief Compile-time NIC ring sizing constants.
 *
 * @details
 * The descriptor ring is statically allocated -- there is no malloc
 * anywhere in the driver. Defaults match the values requested by
 * Sweep 2 / Task 1: 8 TX, 8 RX, 1536-byte buffers (jumbo frames are
 * out of scope). Descriptors and buffers are aligned to 16 bytes
 * per FSP requirement.
 */
typedef enum : uint16_t {
  k_ra8_eth_num_tx_desc = 8U,    /**< Number of TX descriptors in the ring.   */
  k_ra8_eth_num_rx_desc = 8U,    /**< Number of RX descriptors in the ring.   */
  k_ra8_eth_buf_size    = 1536U, /**< Per-descriptor buffer size in bytes.    */
  k_ra8_eth_mac_len     = 6U,    /**< Length of an Ethernet MAC address.      */
  k_ra8_eth_desc_align  = 16U,   /**< Descriptor / buffer alignment in bytes. */
  k_ra8_eth_min_frame   = 60U,   /**< Minimum 802.3 frame (excl. FCS).        */
  k_ra8_eth_max_frame   = 1514U, /**< Maximum 802.3 untagged frame.           */
} ra8_eth_limits_t;

/**
 * @struct ra8_eth_cfg_t
 * @brief Per-channel NIC configuration.
 *
 * @details
 * Passed by const-pointer so future additions never break the ABI.
 * ``num_tx_descriptors`` / ``num_rx_descriptors`` may be 0 to mean
 * "use the compile-time defaults"; if non-zero they must satisfy
 * 1 <= count <= ::k_ra8_eth_num_tx_desc / ::k_ra8_eth_num_rx_desc.
 * ``buffer_size`` is similarly capped at ::k_ra8_eth_buf_size.
 */
typedef struct {
  uint8_t  mac_address[6];     /**< Local MAC address (octets 0..5).    */
  uint8_t  channel;            /**< Logical channel index (0..1).       */
  uint16_t num_tx_descriptors; /**< Active TX descriptors (<=8, 0=def). */
  uint16_t num_rx_descriptors; /**< Active RX descriptors (<=8, 0=def). */
  uint16_t buffer_size;        /**< Buffer size per descriptor.         */
} ra8_eth_cfg_t;

/**
 * @struct ra8_eth_link_t
 * @brief PHY link state snapshot.
 *
 * @details
 * Returned by ::ra8_eth_link_status. Mirrors the IEEE-802.3 BMSR
 * link-up bit and the negotiated speed / duplex from BMCR / GBSR.
 */
typedef struct {
  uint8_t  link_up;     /**< 1 if BMSR.LINK_STATUS asserted, else 0. */
  uint16_t speed_mbps;  /**< Negotiated PHY speed (10/100/1000).     */
  uint8_t  full_duplex; /**< 1 if full duplex, 0 if half.            */
  uint16_t bmsr;        /**< Raw BMSR (PHY register 0x01) snapshot.  */
} ra8_eth_link_t;

/**
 * @struct ra8_eth_stats_t
 * @brief NIC software counters.
 *
 * @details
 * Maintained by the driver at descriptor turnover time. Counters are
 * 32-bit and saturate at UINT32_MAX rather than wrapping (matching
 * NASA Power-of-10 deterministic-counter convention).
 */
typedef struct {
  uint32_t tx_ok;  /**< Frames successfully enqueued.  */
  uint32_t rx_ok;  /**< Frames successfully received.  */
  uint32_t tx_err; /**< TX failures (ring full, etc.). */
  uint32_t rx_err; /**< RX failures (no data, etc.).   */
} ra8_eth_stats_t;

/**
 * @brief Open the NIC: bring up the controller stack and the descriptor rings.
 *
 * @details
 * Sequence (mirrors FSP ``R_ETHER_Open``):
 * 1. Enable the shared ESWM MSTP gate via ::ra8_eth_init.
 * 2. Capture cfg (MAC, channel, ring counts, buffer size).
 * 3. Reset the static descriptor ring -- every TX descriptor is set
 *    software-owned (TACT=0); every RX descriptor is hardware-owned
 *    (RACT=1) so the EDMAC engine can fill it.
 * 4. Programme EDMAC TX / RX descriptor base addresses (TDLAR / RDLAR
 *    in FSP terms, ``GWCA_CTRL`` register set on this chip) and assert
 *    EDRR / EDTR via the GWCA control bits to start the engine.
 * 5. Reset the software statistics counters.
 *
 * @param[in] cfg NIC configuration. Must not be nullptr.
 *
 * @return ::ra8_err_t Error code.
 * @retval k_ra8_ok               Controller and rings ready for IO.
 * @retval k_ra8_err_null_ptr     cfg is nullptr.
 * @retval k_ra8_err_invalid_arg  channel or ring count out of range.
 *
 * @pre Caller is single-threaded with respect to this driver.
 * @pre ESWM MSTP gate is unrestricted (S/NS mapping permits it).
 * @post EDMAC TX engine running (EDTRR.TR=1) on the fake and
 *       on silicon.
 * @post EDMAC RX engine running (EDRRR.RR=1).
 *
 * @note Not thread-safe; gate with a mutex if shared between contexts.
 *
 * @par Example:
 * @code
 * static const ra8_eth_cfg_t cfg = {
 *   .mac_address = { 0x02, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE },
 *   .channel = 0U,
 *   .num_tx_descriptors = 0U,  // use defaults
 *   .num_rx_descriptors = 0U,
 *   .buffer_size        = 0U,
 * };
 * (void)ra8_eth_open(&cfg);
 * @endcode
 *
 * @see ra8_eth_close
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_eth_open(const ra8_eth_cfg_t* cfg);

/**
 * @brief Close the NIC: gate clocks and idle the descriptor rings.
 *
 * @return ::ra8_err_t Error code.
 * @retval k_ra8_ok                  Controller idled.
 * @retval k_ra8_err_not_initialized ::ra8_eth_open was not called first.
 *
 * @pre Driver previously brought up via ::ra8_eth_open.
 * @pre No outstanding DMA against the ring.
 * @post EDMAC TX engine halted (EDTRR.TR=0).
 * @post EDMAC RX engine halted (EDRRR.RR=0).
 *
 * @note Stats are preserved; call ::ra8_eth_open to reset them.
 * @see ra8_eth_open
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_eth_close(void);

/**
 * @brief Transmit a frame through the GWCA TX queue.
 *
 * @details
 * Copies ``len`` bytes from ``buf`` into the driver-owned TX buffer,
 * fills the slot-0 extended descriptor (direct format, destination
 * vector = the bound MAC port), kicks the GWCA, and blocks until the
 * GWCA writes the descriptor back. Every call reuses descriptor
 * slot 0, so the send is synchronous: it returns only once the
 * previous frame has left the queue.
 *
 * @param[in] buf Pointer to frame bytes (MAC header + payload, no FCS).
 * @param[in] len Frame length in bytes (60..1514).
 *
 * @return ::ra8_err_t Error code.
 * @retval k_ra8_ok                  Frame transmitted.
 * @retval k_ra8_err_null_ptr        buf is nullptr.
 * @retval k_ra8_err_invalid_arg     len is out of range.
 * @retval k_ra8_err_hw_timeout      GWCA did not complete the descriptor.
 * @retval k_ra8_err_not_initialized ::ra8_eth_open was not called first.
 *
 * @pre Driver previously brought up via ::ra8_eth_open.
 * @pre len is between ::k_ra8_eth_min_frame and ::k_ra8_eth_max_frame.
 * @post On success, the frame has been handed to the GWCA TX queue.
 * @post On success, the tx_ok statistics counter has been incremented.
 *
 * @note Caller copies the data; ``buf`` may be released on return.
 * @see ra8_eth_read
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_eth_write(const uint8_t* buf, uint32_t len);

/**
 * @brief Pop the next received frame from the RX descriptor ring.
 *
 * @details
 * Inspects the descriptor at the current RX read pointer. If it is
 * software-owned (RACT=0, hardware released it), the buffer payload
 * is copied into ``buf``, ``*got_len`` is set to the frame length,
 * and the descriptor is returned to hardware (RACT=1) before the
 * read pointer advances. If the descriptor is still hardware-owned
 * (RACT=1) the call returns ::k_ra8_err_no_data without side effects.
 *
 * @param[out] buf      Caller buffer, at least ``max_len`` bytes.
 * @param[in]  max_len  Capacity of buf in bytes.
 * @param[out] got_len  Actual frame size written into buf.
 *
 * @return ::ra8_err_t Error code.
 * @retval k_ra8_ok                  Frame copied into buf.
 * @retval k_ra8_err_null_ptr        buf or got_len is nullptr.
 * @retval k_ra8_err_no_data         No frame released by hardware yet.
 * @retval k_ra8_err_invalid_arg     max_len is zero.
 * @retval k_ra8_err_not_initialized ::ra8_eth_open was not called first.
 *
 * @pre Driver previously brought up via ::ra8_eth_open.
 * @pre buf points to at least ``max_len`` writable bytes.
 * @post On k_ra8_ok the descriptor is RACT=1 again (handed back to HW).
 * @post On k_ra8_ok the read pointer advanced one slot.
 *
 * @note Drops bytes past ``max_len``; *got_len reflects what was copied.
 * @see ra8_eth_write
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_eth_read(uint8_t* buf, uint32_t max_len, uint32_t* got_len);

/**
 * @brief Query the PHY link state via MIIM (RMAC MDIO Clause-22).
 *
 * @details
 * Reads BMSR (PHY register 0x01) for link-status, then BMCR (0x00)
 * to extract the configured speed and duplex. The PHY address is
 * fixed at 0 (the EK-RA8D2 board's on-board PHY default).
 *
 * @param[out] out_status Destination structure.
 *
 * @return ::ra8_err_t Error code.
 * @retval k_ra8_ok                  Link snapshot returned.
 * @retval k_ra8_err_null_ptr        out_status is nullptr.
 * @retval k_ra8_err_not_initialized ::ra8_eth_open was not called first.
 * @retval k_ra8_err_hw_timeout      MDIO transaction did not complete.
 *
 * @pre Driver previously brought up via ::ra8_eth_open.
 * @pre PHY MDC clock programmed by RMAC init (MPIC.PSMCS).
 * @post out_status->link_up reflects BMSR bit 2.
 * @post out_status->bmsr is the raw PHY register snapshot.
 *
 * @see ra8_eth_get_stats
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_eth_link_status(ra8_eth_link_t* out_status);

/**
 * @brief Read the NIC software counters.
 *
 * @param[out] out_stats Destination structure.
 *
 * @return ::ra8_err_t Error code.
 * @retval k_ra8_ok                  Counter snapshot returned.
 * @retval k_ra8_err_null_ptr        out_stats is nullptr.
 * @retval k_ra8_err_not_initialized ::ra8_eth_open was not called first.
 *
 * @pre Driver previously brought up via ::ra8_eth_open.
 * @pre out_stats points to a writable structure.
 * @post out_stats fields reflect the live software counters.
 * @post Counters in the driver are NOT cleared.
 *
 * @note Counters saturate at UINT32_MAX rather than wrapping.
 * @see ra8_eth_open
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_eth_get_stats(ra8_eth_stats_t* out_stats);

#ifdef UNIT_TEST
/**
 * @brief Test-only hook: stage a frame as if the EDMAC engine released it.
 *
 * @details
 * Drops ``payload`` bytes into the RX descriptor buffer at the head of
 * the ring and clears RACT so the next ::ra8_eth_read pops it. The host
 * fake has no real EDMAC engine, so this is the bridge that lets
 * unit tests exercise the receive path. Gated on ``UNIT_TEST`` so it
 * cannot leak into firmware target builds.
 *
 * @param[in] payload Bytes to copy into the buffer (non-null).
 * @param[in] len     Frame length (will be clamped to buffer_size).
 *
 * @return ::ra8_err_t Error code.
 *
 * @pre ::ra8_eth_open was called.
 * @pre payload is non-null.
 * @post RX head descriptor has RACT=0 and size = min(len, buffer_size).
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_eth_test_inject_rx(const uint8_t* payload, uint32_t len);
#endif /* UNIT_TEST */

#ifdef __cplusplus
}
#endif
