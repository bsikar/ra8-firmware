/**
 * @file ra_eth.c
 * @brief Ethernet Switch Module (ESWM) + frame TX/RX driver implementation
 *
 * @par Tag
 * [Ring 3 / HAL] {World: NS}
 *
 * @details
 * Driver for the RA8D2 Layer-3 ESWM block plus the polling-first
 * NIC API (``ra_eth_open`` / ``write`` / ``read`` / ``close`` /
 * ``link_status`` / ``get_stats``). Owns the shared ethernet MSTP
 * gate (``k_ra_mstp_eswm``) which is also referenced by the
 * ra_eth_mfwd / ra_eth_coma / ra_eth_gwca / ra_eth_gptp sub-drivers;
 * ra_mstp keeps a reference count so concurrent enables / disables
 * interleave safely.
 *
 * The NIC frame path sits on top of the GWCA "default-state" API
 * (see ra_eth_gwca.h / ra_eth_mfwd.h). One TX queue + one RX queue
 * are wired through a fixed LINKFIX table, each backed by a static
 * descriptor chain + per-slot buffer pool in BSS. ra_eth_open walks
 * the canonical bring-up (LINKFIX install + queue configure + OPC
 * transition to OPERATION) and programs MFWD so inbound frames land
 * on the RX queue; ra_eth_write/ra_eth_read delegate to the
 * default_send/default_recv helpers.
 *
 * Every register access carries a HUM Ch 29 / Ch 34 citation.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra_eth.h"

#include <stdint.h>

#include "ra8d2_ether_regs.h"
#include "ra8d2_etha_regs.h"
#include "ra8d2_mstp_regs.h"
#include "ra8d2_rmac_regs.h"
#include "ra_check.h"
#include "ra_err.h"
#include "ra_etha.h"
#include "ra_eth_gwca.h"
#include "ra_eth_mfwd.h"
#include "ra_log.h"
#include "ra_mstp.h"
#include "ra_rmac.h"
#include "ra_time.h"

/**
 * @var s_tag
 * @brief Logger tag used by every ra_eth_* call.
 */
static const char* s_tag = "ETH";

/**
 * @var s_eth_fn
 * @brief Attached ESWM event callback (nullptr if none).
 */
static ra_eth_event_fn_t s_eth_fn;

/**
 * @var s_eth_ctx
 * @brief Opaque cookie passed to ::s_eth_fn on dispatch.
 */
static void* s_eth_ctx;

/**
 * @enum ra_eth_phy_t
 * @brief PHY-side MIIM constants used by ::ra_eth_link_status.
 */
typedef enum : uint16_t {
  k_ra_eth_phy_addr_default   = 0U,      /**< EK-RA8D2 PHY MDC address.    */
  k_ra_eth_phy_reg_bmcr       = 0U,      /**< BMCR (basic mode control).   */
  k_ra_eth_phy_reg_bmsr       = 1U,      /**< BMSR (basic mode status).    */
  k_ra_eth_phy_reg_anlpar     = 5U,      /**< ANLPAR (10/100 link partner).*/
  k_ra_eth_phy_reg_gbsr       = 10U,     /**< GBSR (1G link-partner ability). */
  k_ra_eth_phy_bmsr_link_up   = 0x0004U, /**< BMSR.LINK_STATUS bit 2.      */
  k_ra_eth_phy_bmsr_an_done   = 0x0020U, /**< BMSR.AUTONEG_COMPLETE bit 5. */
  k_ra_eth_phy_bmcr_speed100  = 0x2000U, /**< BMCR.SPEED_SELECT bit 13.    */
  k_ra_eth_phy_bmcr_speed1000 = 0x0040U, /**< BMCR.SPEED_MS bit 6 (1Gb).   */
  k_ra_eth_phy_bmcr_duplex    = 0x0100U, /**< BMCR.DUPLEX_MODE bit 8.      */
  k_ra_eth_phy_anlpar_10h     = 0x0020U, /**< ANLPAR bit 5: 10BASE-T.      */
  k_ra_eth_phy_anlpar_10f     = 0x0040U, /**< ANLPAR bit 6: 10BASE-T FD.   */
  k_ra_eth_phy_anlpar_100h    = 0x0080U, /**< ANLPAR bit 7: 100BASE-TX.    */
  k_ra_eth_phy_anlpar_100f    = 0x0100U, /**< ANLPAR bit 8: 100BASE-TX FD. */
  k_ra_eth_phy_gbsr_1000h     = 0x0400U, /**< GBSR bit 10: 1000BASE-T HD.  */
  k_ra_eth_phy_gbsr_1000f     = 0x0800U, /**< GBSR bit 11: 1000BASE-T FD.  */
  k_ra_eth_phy_speed_10       = 10U,     /**< 10 Mbps.                     */
  k_ra_eth_phy_speed_100      = 100U,    /**< 100 Mbps.                    */
  k_ra_eth_phy_speed_1000     = 1000U,   /**< 1 Gbps.                      */
} ra_eth_phy_t;

/**
 * @enum ra_eth_layout_t
 * @brief Static layout constants for the GWCA-backed NIC rings.
 *
 * @details
 * The driver wires one RX queue + one TX queue through a 4-entry
 * LINKFIX table. RX uses queue index 0, TX uses queue index 1, and
 * the remaining LINKFIX slots stay LEMPTY (queue disabled). Each
 * chain has ``k_ra_eth_num_*_desc`` data slots plus one trailing
 * LINK terminator that ::ra_eth_gwca_init_ring reserves.
 */
typedef enum : uint16_t {
  k_ra_eth_linkfix_count = 4U,                              /**< LINKFIX entries.    */
  k_ra_eth_def_rx_q_idx  = 0U,                              /**< RX LINKFIX index.   */
  k_ra_eth_def_tx_q_idx  = 1U,                              /**< TX LINKFIX index.   */
  k_ra_eth_rx_ring_depth = (uint16_t)(k_ra_eth_num_rx_desc + 1U), /**< +1 LINK term. */
  k_ra_eth_tx_ring_depth = (uint16_t)(k_ra_eth_num_tx_desc + 1U), /**< +1 LINK term. */
} ra_eth_layout_t;

/**
 * @enum ra_eth_step_t
 * @brief Step values bumped through ::g_ra_eth_open_step for
 *        bench-side post-mortem via J-Link mem32.
 *
 * @details Successful progression uses 0..4. Error codes are
 * ``0x10 | N`` so a JTAG-attached operator can tell at a glance
 * whether the chip parked on a happy-path step or an error path.
 */
typedef enum : uint32_t {
  k_ra_eth_step_ok_1   = 1U,
  k_ra_eth_step_ok_2   = 2U,
  k_ra_eth_step_ok_3   = 3U,
  k_ra_eth_step_ok_4   = 4U,
  k_ra_eth_step_fail_1 = 0x11U,
  k_ra_eth_step_fail_3 = 0x13U,
  k_ra_eth_step_fail_4 = 0x14U,
} ra_eth_step_t;

/**
 * @struct ra_eth_state_t
 * @brief Driver-private NIC state.
 *
 * @details
 * Holds the cached configuration, the cumulative software counters,
 * and the "open" flag. The per-queue head/tail cursors live inside
 * ::s_gwca_state (managed by the GWCA default-state API).
 */
typedef struct {
  uint8_t        opened; /**< 1 once ::ra_eth_open succeeds. */
  ra_eth_cfg_t   cfg;    /**< Captured configuration.        */
  ra_eth_stats_t stats;  /**< Cumulative counters.           */
} ra_eth_state_t;

/**
 * @var s_linkfix_table
 * @brief Static LINKFIX table covering RX + TX queues + slack.
 *
 * @details
 * Lives in BSS, 16-byte aligned. Slots 0/1 are wired by
 * ::ra_eth_gwca_default_open to the RX / TX chains; the remaining
 * slots stay LEMPTY so traffic only reaches the queues we own.
 *
 * @note File-scope, not thread-safe.
 * @since 0.1.0
 */
__attribute__((aligned(16))) static ra_gwca_basic_descriptor_t
  s_linkfix_table[k_ra_eth_linkfix_count];

/**
 * @var s_rx_chain
 * @brief Static RX descriptor chain (BSS, 16-byte aligned).
 *
 * @details
 * ``k_ra_eth_num_rx_desc`` FEMPTY slots + 1 LINK terminator.
 *
 * @note File-scope, not thread-safe.
 * @since 0.1.0
 */
__attribute__((aligned(16))) static ra_gwca_basic_descriptor_t
  s_rx_chain[k_ra_eth_rx_ring_depth];

/**
 * @var s_tx_chain
 * @brief Static TX descriptor chain (BSS, 16-byte aligned).
 *
 * @details
 * ``k_ra_eth_num_tx_desc`` FEMPTY slots + 1 LINK terminator.
 *
 * @note File-scope, not thread-safe.
 * @since 0.1.0
 */
__attribute__((aligned(16))) static ra_gwca_basic_descriptor_t
  s_tx_chain[k_ra_eth_tx_ring_depth];

#ifndef UNIT_TEST
/**
 * @var s_rx_pool_storage
 * @brief Static RX buffer pool, one ``k_ra_eth_buf_size`` slice per slot.
 *
 * @details
 * On the chip target the pool lives in BSS, which fits in the 40-bit
 * PTR field every ::ra_gwca_basic_descriptor_t encodes. On host the
 * BSS sits above the 40-bit cap; ::internal_eth_rx_pool / _tx_pool
 * redirect there to the simulator-mmap'd SRAM region instead so the
 * descriptor PTR round-trips through ::ra_eth_gwca_default_recv.
 *
 * @note File-scope, not thread-safe.
 * @since 0.1.0
 */
__attribute__((aligned(16))) static uint8_t
  s_rx_pool_storage[k_ra_eth_num_rx_desc * k_ra_eth_buf_size];

/**
 * @var s_tx_pool_storage
 * @brief Static TX buffer pool, one ``k_ra_eth_buf_size`` slice per slot.
 *
 * @details See ::s_rx_pool_storage; the host build steers around BSS.
 *
 * @note File-scope, not thread-safe.
 * @since 0.1.0
 */
__attribute__((aligned(16))) static uint8_t
  s_tx_pool_storage[k_ra_eth_num_tx_desc * k_ra_eth_buf_size];
#endif /* UNIT_TEST */

/**
 * @enum ra_eth_host_pool_layout_t
 * @brief Host-test pool addresses in the simulator-mmap'd SRAM region.
 *
 * @details
 * The host SRAM mmap window is 2 MiB at 0x22000000; the RX + TX pools
 * + LINKFIX backing each get a 1 MiB-aligned slice well inside that
 * window. The chip path never sees these values because the
 * ::internal_eth_rx_pool / _tx_pool helpers compile to the BSS arrays
 * when UNIT_TEST is undefined.
 */
typedef enum : uintptr_t {
  k_ra_eth_host_rx_pool_addr = 0x22100000UL, /**< Host RX pool base. */
  k_ra_eth_host_tx_pool_addr = 0x22180000UL, /**< Host TX pool base. */
} ra_eth_host_pool_layout_t;

/**
 * @brief Resolve the RX buffer pool pointer for the current build.
 *
 * @return Address of an ``k_ra_eth_num_rx_desc * k_ra_eth_buf_size``
 *         byte buffer.
 *
 * @details See implementation.
 * @retval pointer Non-null pool base address.
 * @pre Storage is reserved (BSS on chip; sim-mmap'd SRAM on host).
 * @pre Caller invokes this once per ::ra_eth_open.
 * @post Returned pointer is 16-byte aligned.
 * @post No global state is modified.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
static inline uint8_t* internal_eth_rx_pool(void)
{
#ifdef UNIT_TEST
  return (uint8_t*)k_ra_eth_host_rx_pool_addr;
#else
  return s_rx_pool_storage;
#endif
}

/**
 * @brief Resolve the TX buffer pool pointer for the current build.
 *
 * @return Address of an ``k_ra_eth_num_tx_desc * k_ra_eth_buf_size``
 *         byte buffer.
 *
 * @details See implementation.
 * @retval pointer Non-null pool base address.
 * @pre Storage is reserved (BSS on chip; sim-mmap'd SRAM on host).
 * @pre Caller invokes this once per ::ra_eth_open.
 * @post Returned pointer is 16-byte aligned.
 * @post No global state is modified.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
static inline uint8_t* internal_eth_tx_pool(void)
{
#ifdef UNIT_TEST
  return (uint8_t*)k_ra_eth_host_tx_pool_addr;
#else
  return s_tx_pool_storage;
#endif
}

/**
 * @var s_gwca_state
 * @brief GWCA default-state block backing ::ra_eth_open / write / read.
 *
 * @details
 * Populated by ::ra_eth_open before calling
 * ::ra_eth_gwca_default_open; carries the rings + buffer pools +
 * cursors for one RX + one TX queue.
 *
 * @note File-scope, not thread-safe.
 * @since 0.1.0
 */
static ra_eth_gwca_default_state_t s_gwca_state;

/**
 * @var s_state
 * @brief Singleton NIC runtime state.
 */
static ra_eth_state_t s_state;

/**
 * @var s_mac_speed_resynced
 * @brief Latch -- true once ::ra_eth_link_status has re-programmed
 *        MPIC.LSC / MPIC.PIPP to match the PHY's negotiated link.
 *
 * @details
 * Reset to false in ::ra_eth_open / ::ra_eth_close. The MAC speed
 * resync only needs to fire once per link bring-up; further calls
 * to ::ra_eth_link_status skip the MPIC write so they remain
 * read-only status pollers.
 *
 * @note File-scope state, not thread-safe.
 * @since 0.1.0
 */
static bool s_mac_speed_resynced = false;

/**
 * @brief Saturating-add helper for the 32-bit software counters.
 *
 * @param[in,out] counter Pointer to the counter to bump.
 *
 * @pre counter is non-null.
 * @pre Module state is consistent.
 * @post *counter increases by one unless it was already UINT32_MAX.
 * @post Caller-visible state matches the documented contract.
 *
 * @details See implementation.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
static inline void internal_stat_inc(uint32_t* counter)
{
  if (*counter != UINT32_MAX) {
    *counter += 1U;
  }
}

/**
 * @brief Bytewise-copy n bytes from src into dst.
 *
 * @details
 * Used in place of ``memcpy`` so clang-tidy's deprecated-buffer-handling
 * checker stays happy. The driver carries enough invariants
 * (length-clamped, buffer-aligned) that a plain byte-by-byte loop is
 * equivalent and produces identical code on -O2.
 *
 * @param[out] dst Destination buffer.
 * @param[in]  src Source buffer.
 * @param[in]  n   Bytes to copy.
 *
 * @pre dst and src are non-null and do not overlap.
 * @pre n bytes are valid in both buffers.
 * @post First n bytes of dst equal src.
 * @post Caller-visible state matches the documented contract.
 *
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
static inline void internal_byte_copy(uint8_t* dst, const uint8_t* src, uint32_t n)
{
  for (uint32_t i = 0U; i < n; ++i) {
    dst[i] = src[i];
  }
}

/**
 * @brief Map a logical channel index to an RMAC port identifier.
 *
 * @param[in] channel Channel id from ::ra_eth_cfg_t (0 or 1).
 * @return Matching ::ra_rmac_port_t.
 *
 * @details See implementation.
 * @retval k_ra_ok Operation succeeded.
 * @pre channel was already range-checked by the caller.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post No global state is mutated.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
static inline ra_rmac_port_t internal_channel_to_port(uint8_t channel)
{
  if (channel == 0U) {
    return k_ra_rmac_port_0;
  }
  return k_ra_rmac_port_1;
}

/**
 * @brief Validate the cfg ring sizes and resolve zero-as-default values.
 *
 * @param[in]  cfg      User cfg (already null-checked).
 * @param[out] tx_count Resolved TX descriptor count.
 * @param[out] rx_count Resolved RX descriptor count.
 * @param[out] buf_size Resolved per-descriptor buffer size.
 *
 * @return ::ra_err_t Error code.
 * @retval k_ra_ok               Sizes resolved.
 * @retval k_ra_err_invalid_arg  Some count was out of range.
 *
 * @pre cfg is non-null.
 * @pre out pointers are non-null.
 * @post On k_ra_ok the *tx_count / *rx_count / *buf_size are set.
 * @post On error no out param is touched in a way that would mislead.
 *
 * @details See implementation.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
static ra_err_t internal_resolve_sizes(const ra_eth_cfg_t* cfg,
                                       uint16_t*           tx_count,
                                       uint16_t*           rx_count,
                                       uint16_t*           buf_size)
{
  uint16_t tx = cfg->num_tx_descriptors;
  uint16_t rx = cfg->num_rx_descriptors;
  uint16_t bs = cfg->buffer_size;
  if (tx == 0U) {
    tx = k_ra_eth_num_tx_desc;
  }
  if (rx == 0U) {
    rx = k_ra_eth_num_rx_desc;
  }
  if (bs == 0U) {
    bs = k_ra_eth_buf_size;
  }
  /* mcdc-deactivated: tx normalized to nonzero above; first OR-condition unreachable. */
  if ((tx == 0U) || (tx > k_ra_eth_num_tx_desc)) {
    return k_ra_err_invalid_arg;
  }
  /* mcdc-deactivated: rx normalized to nonzero above; first OR-condition unreachable. */
  if ((rx == 0U) || (rx > k_ra_eth_num_rx_desc)) {
    return k_ra_err_invalid_arg;
  }
  if ((bs < k_ra_eth_min_frame) || (bs > k_ra_eth_buf_size)) {
    return k_ra_err_invalid_arg;
  }
  *tx_count = tx;
  *rx_count = rx;
  *buf_size = bs;
  return k_ra_ok;
}

/**
 * @brief Bring the per-channel RMAC port up and program the MAC address.
 *
 * @param[in] cfg User configuration.
 *
 * @return ::ra_err_t Error code.
 *
 * @details See implementation.
 * @retval k_ra_ok Operation succeeded.
 * @pre cfg is non-null and cfg->channel is in [0, 1].
 * @pre Caller has already enabled the ESWM MSTP gate.
 * @post On success the RMAC port carries cfg->mac_address.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
static ra_err_t internal_bring_up_rmac(const ra_eth_cfg_t* cfg)
{
  /* The board-level ``ra_board_ethernet_init`` (or its equivalent for
   * non-EK boards) is expected to have already programmed RMAC's MPIC
   * (PIS / LSC / PIPP / PSMCS), enabled the MSTP gate, and brought the
   * matching ETHA into OPERATION mode. Overwriting MPIC here would
   * clobber the carefully-computed MDC clock divider (PSMCS) and force
   * the wrong PHY interface mode (MII instead of RGMII). The only
   * RMAC-level state ``ra_eth_open`` still has to programme is the
   * MAC address the application chose.
   *
   * HUM Ch 33.4 "MRMAC0 / MRMAC1" p 1707 -- the MAC address registers
   * are paired with MPIC under the same write-once gate: silent on
   * MAC writes while ETHA is in OPERATION. Bench-confirmed on
   * EK-RA8D2: without the DISABLE -> CONFIG -> {MAC write} -> DISABLE
   * -> OPERATION bracket, MRMAC0 / MRMAC1 read back as 0 after
   * ra_eth_open completes, and every wire-side ARP "who-has 192.168.
   * .1.42" gets dropped at the RMAC perfect-match comparator before
   * the GWCA descriptor ring ever sees it.
   *
   * The board layer ran the bracket once during ra_rmac_init; we have
   * to repeat it here so the application-supplied MAC actually lands
   * in MRMAC0/MRMAC1. */
  const ra_rmac_port_t rmac_port = internal_channel_to_port(cfg->channel);
  const ra_etha_port_t etha_port = (cfg->channel == 0U)
                                       ? (ra_etha_port_t)k_ra_etha_port_0
                                       : (ra_etha_port_t)k_ra_etha_port_1;
  uint8_t              mac_copy[k_ra_eth_mac_len];
  for (uint8_t i = 0U; i < (uint8_t)k_ra_eth_mac_len; ++i) {
    mac_copy[i] = cfg->mac_address[i];
  }

  /* HUM Ch 32.3.1.1 "EAMC : Mode Command Register" p 1631 -- ETHA
   * mode-transition sequence. DISABLE before CONFIG so the ESWM
   * accepts the request even if the port was already in OPERATION. */
  ra_err_t err = ra_etha_set_mode(etha_port, k_ra_etha_opc_disable);
  if (err != k_ra_ok) {
    return err;
  }
  err = ra_etha_set_mode(etha_port, k_ra_etha_opc_config);
  if (err != k_ra_ok) {
    return err;
  }

  /* HUM Ch 33.4 "MRMAC0 / MRMAC1" p 1707 -- writes only stick while
   * the paired ETHA is in CONFIG. */
  const ra_err_t mac_err = ra_rmac_set_mac_address(rmac_port, mac_copy);

  /* Always return to OPERATION even on MAC write failure so the chip
   * doesn't end up parked in CONFIG. */
  const ra_err_t dis_err = ra_etha_set_mode(etha_port, k_ra_etha_opc_disable);
  const ra_err_t op_err  = ra_etha_set_mode(etha_port, k_ra_etha_opc_operation);
  if (mac_err != k_ra_ok) {
    return mac_err;
  }
  if (dis_err != k_ra_ok) {
    return dis_err;
  }
  return op_err;
}

/* Implementation of ra_eth_init (see header for full contract) -- see header for the documented contract. */
ra_err_t ra_eth_init(void)
{
  /* HUM Ch 11.2.8 "MSTPCRC : Module Stop Control Register C" p 446 */
  const ra_err_t mst_err = ra_mstp_enable(k_ra_mstp_eswm);
  RA_RETURN_ON_ERROR(mst_err, s_tag, "eth_init: mstp enable"); /* GCOVR_EXCL_BR_LINE */

  volatile r_eswm_regs_t* reg = ra_eswm();
  /* HUM Ch 29 "Layer 3 Ethernet Switch Module (ESWM)" p 1287 */
  reg->ESWM_CTRL = 0U;
  reg->ESWM_STS  = 0U;
  reg->ESWM_IE   = 0U;
  reg->ESWM_ICLR = 0U;
  ra_log_info(s_tag, "eth_init (ESWM)");
  return k_ra_ok;
}

/* Implementation of ra_eth_deinit (see header for full contract) -- see header for the documented contract. */
ra_err_t ra_eth_deinit(void)
{
  volatile r_eswm_regs_t* reg = ra_eswm();
  /* HUM Ch 29 "Layer 3 Ethernet Switch Module (ESWM)" p 1287 */
  reg->ESWM_CTRL = 0U;
  reg->ESWM_IE   = 0U;
  s_eth_fn       = nullptr;
  s_eth_ctx      = nullptr;
  return ra_mstp_disable(k_ra_mstp_eswm);
}

/* Implementation of ra_eth_get_status (see header for full contract) -- see header for the documented contract. */
ra_err_t ra_eth_get_status(uint32_t* out_mask)
{
  RA_CHECK_NULL_PTR(out_mask, s_tag, "out_mask must not be nullptr");
  /* HUM Ch 29 "Layer 3 Ethernet Switch Module (ESWM)" p 1287 */
  *out_mask = ra_eswm()->ESWM_STS;
  return k_ra_ok;
}

/* Implementation of ra_eth_clear_status (see header for full contract) -- see header for the documented contract. */
ra_err_t ra_eth_clear_status(uint32_t mask)
{
  volatile r_eswm_regs_t* reg = ra_eswm();
  /* HUM Ch 29 "Layer 3 Ethernet Switch Module (ESWM)" p 1287 */
  reg->ESWM_ICLR = mask;
  reg->ESWM_STS  = reg->ESWM_STS & ~mask;
  return k_ra_ok;
}

/* Implementation of ra_eth_attach_handler (see header for full contract) -- see header for the documented contract. */
ra_err_t ra_eth_attach_handler(ra_eth_event_fn_t fn, void* ctx)
{
  s_eth_fn  = fn;
  s_eth_ctx = ctx;
  return k_ra_ok;
}

/* Implementation of ra_eth_dispatch (see header for full contract) -- see header for the documented contract. */
void ra_eth_dispatch(void)
{
  volatile r_eswm_regs_t* reg = ra_eswm();
  /* HUM Ch 29 "Layer 3 Ethernet Switch Module (ESWM)" p 1287 */
  const uint32_t          mask = reg->ESWM_STS;
  const ra_eth_event_fn_t fn   = s_eth_fn;
  void* const             ctx  = s_eth_ctx;
  reg->ESWM_ICLR               = mask;
  reg->ESWM_STS                = 0U;
  if (fn != nullptr) {
    fn(ctx, mask);
  }
}

/* Implementation of ra_eth_enter_stop (see header for full contract) -- see header for the documented contract. */
ra_err_t ra_eth_enter_stop(void)
{
  /* HUM Ch 29 "Layer 3 Ethernet Switch Module (ESWM)" p 1287 */
  ra_eswm()->ESWM_CTRL = 0U;
  return ra_mstp_disable(k_ra_mstp_eswm);
}

/* Implementation of ra_eth_exit_stop (see header for full contract) -- see header for the documented contract. */
ra_err_t ra_eth_exit_stop(void)
{
  return ra_mstp_enable(k_ra_mstp_eswm);
}

/* -----------------------------------------------------------------------
 * Frame-level NIC API.
 * -------------------------------------------------------------------- */

/**
 * @brief Capture cfg into ::s_state and reset the counters.
 *
 * @param[in] cfg Validated configuration.
 *
 * @pre cfg is non-null.
 * @pre ::ra_eth_gwca_default_open has already succeeded.
 * @post ::s_state holds the new cfg and zeroed counters.
 * @post ::s_state.opened is 1.
 *
 * @details See implementation.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
static void internal_capture_state(const ra_eth_cfg_t* cfg)
{
  s_state.opened       = 1U;
  s_state.cfg          = *cfg;
  s_state.stats.tx_ok  = 0U;
  s_state.stats.rx_ok  = 0U;
  s_state.stats.tx_err = 0U;
  s_state.stats.rx_err = 0U;
}

/**
 * @brief Populate ::s_gwca_state with the static rings + pools + queue indices.
 *
 * @param[in] cfg Already-validated user cfg (channel used as MAC port).
 *
 * @pre cfg is non-null and cfg->channel is in [0, 1].
 * @pre Static descriptors and pools exist (BSS-resident).
 * @post ::s_gwca_state fields point at the static storage.
 * @post ::s_gwca_state cursors (rx_head, tx_tail) are zero.
 *
 * @details See implementation.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
static void internal_populate_gwca_state(const ra_eth_cfg_t* cfg)
{
  s_gwca_state.linkfix_table  = s_linkfix_table;
  s_gwca_state.linkfix_count  = (uint32_t)k_ra_eth_linkfix_count;
  s_gwca_state.rx_chain       = s_rx_chain;
  s_gwca_state.rx_depth       = (uint32_t)k_ra_eth_rx_ring_depth;
  s_gwca_state.rx_slot_bytes  = (uint32_t)k_ra_eth_buf_size;
  s_gwca_state.rx_pool        = internal_eth_rx_pool();
  s_gwca_state.rx_queue_index = (uint32_t)k_ra_eth_def_rx_q_idx;
  s_gwca_state.rx_head        = 0U;
  s_gwca_state.tx_chain       = s_tx_chain;
  s_gwca_state.tx_depth       = (uint32_t)k_ra_eth_tx_ring_depth;
  s_gwca_state.tx_slot_bytes  = (uint32_t)k_ra_eth_buf_size;
  s_gwca_state.tx_pool        = internal_eth_tx_pool();
  s_gwca_state.tx_queue_index = (uint32_t)k_ra_eth_def_tx_q_idx;
  s_gwca_state.tx_tail        = 0U;
  s_gwca_state.mac_port       = cfg->channel;
}

/**
 * @brief Validate cfg, resolve ring sizes, and bring up MSTP + RMAC.
 *
 * @details
 * Rolled out of ::ra_eth_open so that single function stays under
 * the project's clang-tidy function-size threshold. Performs:
 * 1. cfg channel range check.
 * 2. Ring size resolution (for ABI validation only; defaults if 0).
 * 3. MSTP gate enable.
 * 4. Per-channel RMAC bring-up + MAC address program.
 *
 * @param[in]  cfg User configuration (already null-checked by caller).
 *
 * @return ::ra_err_t Error code.
 *
 * @retval k_ra_ok Operation succeeded.
 * @pre cfg is non-null.
 * @pre Caller is single-threaded with respect to this driver.
 * @post On success the ESWM MSTP gate is enabled and the RMAC port
 *       carries cfg->mac_address.
 * @post On failure no state is changed visibly to the caller.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
static ra_err_t internal_open_prep(const ra_eth_cfg_t* cfg)
{
  if (cfg->channel > 1U) {
    ra_log_error(s_tag, "open: channel out of range");
    return k_ra_err_invalid_arg;
  }
  uint16_t       tx_count   = 0U;
  uint16_t       rx_count   = 0U;
  uint16_t       buf_size   = 0U;
  const ra_err_t resolve_rc = internal_resolve_sizes(cfg, &tx_count, &rx_count, &buf_size);
  if (resolve_rc != k_ra_ok) {
    return resolve_rc;
  }
  const ra_err_t mst_err = ra_mstp_enable(k_ra_mstp_eswm);
  if (mst_err != k_ra_ok) {
    return mst_err;
  }
  return internal_bring_up_rmac(cfg);
}

/**
 * @var g_ra_eth_open_step
 * @brief Bench-side debug trail -- bumped by ::ra_eth_open to record
 *        the highest open-sub-step the chip reached before parking.
 *
 * @details
 * Read externally via J-Link `mem32` after the firmware halts so the
 * caller can identify which open-path primitive returned non-ok:
 *   1 = prep ok (channel/sizes validated, MSTP + RMAC up).
 *   2 = populated s_gwca_state.
 *   3 = default_open ok (GWCA in OPERATION).
 *   4 = mfwd_route_queue ok.
 *   5 = capture_state ok (final).
 * Plus the symmetric error codes ``0x10|N`` for failures at step N.
 *
 * @note Read externally by J-Link only; firmware never reads back.
 * @since 0.1.0
 */
volatile uint32_t g_ra_eth_open_step;

/**
 * @var g_ra_eth_phy_bmsr_after_wait
 * @brief BMSR snapshot AFTER the ::internal_wait_for_autoneg poll.
 * @note Read externally by J-Link only; firmware never reads back.
 * @since 0.1.0
 */
volatile uint16_t g_ra_eth_phy_bmsr_after_wait;

/**
 * @var g_ra_eth_anlpar
 * @brief Last-read ANLPAR (PHY reg 5) snapshot for bench debugging.
 * @note Read externally by J-Link only; firmware never reads back.
 * @since 0.1.0
 */
volatile uint16_t g_ra_eth_anlpar;

/**
 * @var g_ra_eth_gbsr
 * @brief Last-read GBSR (PHY reg 10) snapshot for bench debugging.
 * @note Read externally by J-Link only; firmware never reads back.
 * @since 0.1.0
 */
volatile uint16_t g_ra_eth_gbsr;

/**
 * @var g_ra_eth_resync_speed_lsc
 * @brief Speed-LSC value the resync chose. 0=10M, 1=100M, 2=1000M.
 * @note Read externally by J-Link only; firmware never reads back.
 * @since 0.1.0
 */
volatile uint32_t g_ra_eth_resync_speed_lsc;

/**
 * @var g_ra_eth_resync_duplex
 * @brief Duplex value the resync chose. 0=half, 1=full.
 * @note Read externally by J-Link only; firmware never reads back.
 * @since 0.1.0
 */
volatile uint32_t g_ra_eth_resync_duplex;

/**
 * @brief Walk the GWCA bring-up + MFWD routing setup for ::ra_eth_open.
 *
 * @details
 * Splits the GWCA + MFWD wiring out of ::ra_eth_open so the entry
 * point stays under the function-size budget. Calls
 * ::ra_eth_gwca_default_open to install the LINKFIX table, configure
 * the RX + TX queues and transition GWMC.OPC to OPERATION, then
 * programs ``MFWD.FWPBFCSDC0[port].PBCSD = rx_queue_index`` via
 * ::ra_eth_mfwd_route_queue so port-to-host frames reach the RX
 * queue on real silicon. ``g_ra_eth_open_step`` is bumped along
 * the way so a JTAG-attached operator can see exactly which sub
 * primitive failed.
 *
 * @return ::ra_err_t Error code.
 * @retval k_ra_ok              GWCA in OPERATION + MFWD routing live.
 * @retval k_ra_err_invalid_arg ::s_gwca_state fields invalid.
 * @retval k_ra_err_hw_timeout  GWMC.OPC transition never converged.
 *
 * @pre ::internal_populate_gwca_state has run.
 * @pre ::internal_open_prep returned ok.
 * @post On success GWMC.OPC == OPERATION and MFWD routes inbound
 *       frames from the configured port into the RX queue.
 * @post On failure GWCA may be in DISABLE; caller may retry by
 *       re-invoking ::ra_eth_open.
 *
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
static ra_err_t internal_open_gwca_path(void)
{
  /* Note: ``ra_board_ethernet_init`` already runs its own COMA reset
   * (RR pulse + RCEC|ACE enable) via ``internal_eth_coma_reset`` --
   * the full FSP CABPIRM.BPIOG/BPR handshake is intentionally skipped
   * there because BPR never asserts on EK-RA8D2 silicon unless GWCA
   * is also being brought up, and the board can't depend on that. */

  /* Per FSP r_layer3_switch_open: program the per-port forwarding
   * destination masks BEFORE bringing the GWCA up. 0x7F = allow all
   * destinations (permissive baseline; tightens once L3 filtering
   * lands). */
  static const uint8_t s_fwpbfc_masks[3] = {0x7FU, 0x7FU, 0x7FU};
  (void)ra_eth_mfwd_set_forwarding_masks(s_fwpbfc_masks);

  const ra_err_t err = ra_eth_gwca_default_open(&s_gwca_state);
  if (err != k_ra_ok) {
    g_ra_eth_open_step = (uint32_t)k_ra_eth_step_fail_3;
    return err;
  }
  g_ra_eth_open_step      = (uint32_t)k_ra_eth_step_ok_3;
  const ra_err_t mfwd_err = ra_eth_mfwd_route_queue(s_gwca_state.mac_port,
                                                    (uint8_t)s_gwca_state.rx_queue_index);
  if (mfwd_err != k_ra_ok) {
    g_ra_eth_open_step = (uint32_t)k_ra_eth_step_fail_4;
    return mfwd_err;
  }
  g_ra_eth_open_step = (uint32_t)k_ra_eth_step_ok_4;
  return k_ra_ok;
}

/* Implementation of ra_eth_open (see header for full contract) -- see header for the documented contract. */
ra_err_t ra_eth_open(const ra_eth_cfg_t* cfg)
{
  RA_CHECK_NULL_PTR(cfg, s_tag, "open: cfg must not be nullptr");

  g_ra_eth_open_step = 0U;
  const ra_err_t prep_rc = internal_open_prep(cfg);
  if (prep_rc != k_ra_ok) {
    g_ra_eth_open_step = (uint32_t)k_ra_eth_step_fail_1;
    return prep_rc;
  }
  g_ra_eth_open_step = (uint32_t)k_ra_eth_step_ok_1;

  internal_populate_gwca_state(cfg);
  g_ra_eth_open_step = (uint32_t)k_ra_eth_step_ok_2;

  const ra_err_t bring_up_rc = internal_open_gwca_path();
  if (bring_up_rc != k_ra_ok) {
    return bring_up_rc;
  }

  /* Force a fresh MPIC re-sync on the next ::ra_eth_link_status call
   * so the on-chip RMAC's MPIC.LSC matches whatever speed the PHY
   * actually negotiated (HUM Ch 33.4.1.2 "MPIC : PHY Interfaces
   * Configuration Register" p 1707). */
  s_mac_speed_resynced = false;

  internal_capture_state(cfg);
  ra_log_info(s_tag, "eth_open: gwca rings ready");
  return k_ra_ok;
}

/* Implementation of ra_eth_close (see header for full contract) -- see header for the documented contract. */
ra_err_t ra_eth_close(void)
{
  if (s_state.opened == 0U) {
    return k_ra_err_not_initialized;
  }

  /* Park the GWCA state machine in DISABLE. Best-effort -- ignore
   * the err so teardown always reaches the MSTP-gate balance below.
   * HUM Ch 34.3.1 "GWMC : Mode Configuration Register" p 1797 */
  (void)ra_eth_gwca_set_operation_mode(k_ra_gwmc_opc_disable);

  /* Tear down the local RMAC port (MSTP-gate ref-counted). */
  const ra_rmac_port_t rmac_port = internal_channel_to_port(s_state.cfg.channel);
  (void)ra_rmac_deinit(rmac_port);

  /* Balance the second ESWM-MSTP enable that ::ra_eth_gwca_default_open
   * acquired via ::ra_eth_gwca_init. The first enable, owned by
   * ::internal_open_prep, is balanced by the final ra_mstp_disable
   * call below. */
  (void)ra_eth_gwca_deinit();

  s_state.opened       = 0U;
  s_mac_speed_resynced = false;
  return ra_mstp_disable(k_ra_mstp_eswm);
}

/* Implementation of ra_eth_write (see header for full contract) -- see header for the documented contract. */
ra_err_t ra_eth_write(const uint8_t* buf, uint32_t len)
{
  RA_CHECK_NULL_PTR(buf, s_tag, "write: buf must not be nullptr");
  if (s_state.opened == 0U) {
    return k_ra_err_not_initialized;
  }
  if ((len < k_ra_eth_min_frame) || (len > k_ra_eth_max_frame)) {
    ra_log_error(s_tag, "write: frame length out of range");
    return k_ra_err_invalid_arg;
  }

  const ra_err_t err = ra_eth_gwca_default_send(&s_gwca_state, buf, len);
  if (err == k_ra_ok) {
    internal_stat_inc(&s_state.stats.tx_ok);
    return k_ra_ok;
  }
  internal_stat_inc(&s_state.stats.tx_err);
  if (err == k_ra_err_no_data) {
    /* find_slot returns no_data when all FEMPTY slots are exhausted
     * -- surface that to callers as the documented busy retval. */
    return k_ra_err_busy;
  }
  return err;
}

/* Implementation of ra_eth_read (see header for full contract) -- see header for the documented contract. */
ra_err_t ra_eth_read(uint8_t* buf, uint32_t max_len, uint32_t* got_len)
{
  RA_CHECK_NULL_PTR(buf, s_tag, "read: buf must not be nullptr");
  RA_CHECK_NULL_PTR(got_len, s_tag, "read: got_len must not be nullptr");
  if (s_state.opened == 0U) {
    return k_ra_err_not_initialized;
  }
  if (max_len == 0U) {
    return k_ra_err_invalid_arg;
  }

  const ra_err_t err = ra_eth_gwca_default_recv(&s_gwca_state, buf, max_len, got_len);
  if (err == k_ra_ok) {
    internal_stat_inc(&s_state.stats.rx_ok);
    return k_ra_ok;
  }
  internal_stat_inc(&s_state.stats.rx_err);
  if (err == k_ra_err_no_data) {
    *got_len = 0U;
    return k_ra_err_no_data;
  }
  return err;
}

/**
 * @brief Compute the highest auto-negotiated speed + duplex from ANLPAR+GBSR.
 *
 * @details
 * Mirrors FSP r_rmac_phy.c::R_RMAC_PHY_LinkPartnerAbilityGet. Picks
 * the highest priority intersection of our local PHY advertisement
 * and the link partner's: 1000F > 100F > 100H > 10F > 10H. BMCR's
 * speed bits are NOT the negotiated speed -- they reflect what the
 * host commanded the PHY to be (not what the auto-neg landed on).
 *
 * @param[in]  anlpar Auto-negotiation Link Partner Ability register.
 * @param[in]  gbsr   1000BASE-T Status register (link-partner-ability bits).
 * @param[out] out_speed  Receives the LSC value for ::ra_rmac_set_link.
 * @param[out] out_duplex Receives the duplex value.
 *
 * @pre out_speed and out_duplex are non-null.
 * @pre Auto-neg has completed (caller checked BMSR.AN_COMPLETE).
 * @post On return ``*out_speed`` reflects the highest mutual ability.
 * @post On return ``*out_duplex`` reflects matching duplex.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
static void internal_pick_negotiated_speed(uint16_t          anlpar,
                                           uint16_t          gbsr,
                                           ra_rmac_lsc_t*    out_speed,
                                           ra_rmac_duplex_t* out_duplex)
{
  /* Default: lowest tier. */
  *out_speed  = k_ra_rmac_lsc_10mbit;
  *out_duplex = k_ra_rmac_duplex_half;
  if ((anlpar & (uint16_t)k_ra_eth_phy_anlpar_10h) != 0U) {
    *out_speed  = k_ra_rmac_lsc_10mbit;
    *out_duplex = k_ra_rmac_duplex_half;
  }
  if ((anlpar & (uint16_t)k_ra_eth_phy_anlpar_10f) != 0U) {
    *out_speed  = k_ra_rmac_lsc_10mbit;
    *out_duplex = k_ra_rmac_duplex_full;
  }
  if ((anlpar & (uint16_t)k_ra_eth_phy_anlpar_100h) != 0U) {
    *out_speed  = k_ra_rmac_lsc_100mbit;
    *out_duplex = k_ra_rmac_duplex_half;
  }
  if ((anlpar & (uint16_t)k_ra_eth_phy_anlpar_100f) != 0U) {
    *out_speed  = k_ra_rmac_lsc_100mbit;
    *out_duplex = k_ra_rmac_duplex_full;
  }
  if ((gbsr & (uint16_t)k_ra_eth_phy_gbsr_1000h) != 0U) {
    *out_speed  = k_ra_rmac_lsc_1000mbit;
    *out_duplex = k_ra_rmac_duplex_half;
  }
  if ((gbsr & (uint16_t)k_ra_eth_phy_gbsr_1000f) != 0U) {
    *out_speed  = k_ra_rmac_lsc_1000mbit;
    *out_duplex = k_ra_rmac_duplex_full;
  }
}

/**
 * @brief Re-program MPIC.LSC / MPIC.PIPP to match the PHY's auto-neg result.
 *
 * @details
 * The board layer programs MPIC for 1Gbps at boot because that is
 * the maximum the EK-RA8D2's PEF7071/GPY111 PHY supports. When the
 * link partner only offers 10/100 (typical USB-Ethernet adapter),
 * the PHY auto-negotiates down -- but MPIC stays at 1Gbps and the
 * on-chip RMAC ignores every RGMII edge as out-of-spec framing.
 * Symptom (bench-confirmed on EK-RA8D2): PHY reports link-up, ARP
 * arrives on the wire, but MRGFCE stays at 0 and no frame ever
 * lands in the GWCA RX chain.
 *
 * This helper reads the auto-neg result registers (ANLPAR reg 5 +
 * GBSR reg 10) -- NOT BMCR, whose speed bits are the host command --
 * picks the highest mutually-supported speed/duplex via
 * ::internal_pick_negotiated_speed, then brackets the MPIC write
 * with the ETHA DISABLE -> CONFIG -> {MPIC} -> DISABLE -> OPERATION
 * transitions required by HUM Ch 33.4.1.2.
 *
 * Mirrors FSP r_rmac_phy.c::R_RMAC_PHY_LinkPartnerAbilityGet.
 *
 * @param[in] port RMAC port whose MPIC needs updating.
 * @param[in] bmcr Unused -- kept for API stability with the old caller.
 *
 * @return ::ra_err_t error code.
 * @retval k_ra_ok              MPIC re-programmed (or no-op).
 * @retval k_ra_err_invalid_arg ::ra_rmac_set_link rejected an arg.
 * @retval k_ra_err_hw_timeout  An ETHA mode transition or MDIO timed out.
 *
 * @pre ::ra_eth_open has succeeded (::s_state.opened == 1).
 * @pre ::s_mac_speed_resynced is false on first call after open.
 * @post On k_ra_ok the on-chip RMAC's MPIC.LSC matches the PHY's
 *       negotiated speed and MPIC.PIPP matches its duplex.
 * @post ::s_mac_speed_resynced is true on success.
 *
 * @note Not thread-safe; firmware drives this from a single bring-up
 *       thread.
 *
 * @since 0.1.0
 */
/**
 * @enum ra_eth_an_poll_t
 * @brief Bound on the AUTONEG_COMPLETE poll loop.
 *
 * @details Spec: auto-neg should complete within ~3 s of link-up.
 * We poll BMSR.AN_COMPLETE for up to 4 s in 50 ms increments before
 * giving up and falling back to whatever ANLPAR / GBSR happen to
 * read (typically zero -> 10M HD).
 */
typedef enum : uint32_t {
  k_ra_eth_an_poll_period_ms = 50U,
  k_ra_eth_an_poll_max_iters = 80U, /**< 80 * 50 ms = 4 s ceiling.   */
} ra_eth_an_poll_t;

/**
 * @brief Poll BMSR.AUTONEG_COMPLETE until it asserts or the budget runs out.
 *
 * @details
 * BMSR.LINK_STATUS can fire before AN_COMPLETE, but ANLPAR / GBSR
 * only latch the link-partner advertisement at AN_COMPLETE. Reading
 * them too early returns 0. Polls every k_ra_eth_an_poll_period_ms
 * for up to k_ra_eth_an_poll_max_iters iterations (~4 s ceiling).
 *
 * @param[in] port RMAC port whose PHY to poll.
 *
 * @pre Caller is single-threaded.
 * @pre PHY is enumerated at ::k_ra_eth_phy_addr_default.
 * @post Returns once AN_COMPLETE is set or the budget is exhausted.
 * @post Side-effect free apart from the busy-wait loop.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
static void internal_wait_for_autoneg(ra_rmac_port_t port)
{
  uint16_t bmsr = 0U;
  for (uint32_t i = 0U; i < (uint32_t)k_ra_eth_an_poll_max_iters; ++i) {
    if (ra_rmac_mdio_c22_read(port,
                              (uint8_t)k_ra_eth_phy_addr_default,
                              (uint8_t)k_ra_eth_phy_reg_bmsr,
                              &bmsr) != k_ra_ok) {
      g_ra_eth_phy_bmsr_after_wait = bmsr;
      return;
    }
    if ((bmsr & (uint16_t)k_ra_eth_phy_bmsr_an_done) != 0U) {
      g_ra_eth_phy_bmsr_after_wait = bmsr;
      return;
    }
    ra_delay_ms((uint32_t)k_ra_eth_an_poll_period_ms);
  }
  g_ra_eth_phy_bmsr_after_wait = bmsr;
}

/**
 * @brief Read ANLPAR + GBSR and pick the negotiated speed/duplex.
 *
 * @details
 * Helper extracted from ::internal_resync_mac_speed so each function
 * fits under the 60-line / 40-statement cap. Reads PHY registers 5
 * (ANLPAR) and 10 (GBSR), snapshots them into the bench-side debug
 * vars, then calls ::internal_pick_negotiated_speed to compute the
 * highest mutually-supported speed/duplex.
 *
 * @param[in]  port       RMAC port whose PHY to query.
 * @param[out] out_speed  Receives the LSC value.
 * @param[out] out_duplex Receives the duplex value.
 *
 * @return ::ra_err_t MDIO read outcome (propagated from the first
 *         failing read).
 * @retval k_ra_ok            Both registers fetched.
 * @retval other              MDIO read failure.
 *
 * @pre Auto-neg has had a chance to settle (call
 *      ::internal_wait_for_autoneg first).
 * @pre out_speed / out_duplex are non-null.
 * @post On success the resync debug snapshots are updated.
 * @post On failure ``*out_speed`` / ``*out_duplex`` are unmodified.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
static ra_err_t internal_query_negotiated_speed(ra_rmac_port_t    port,
                                                ra_rmac_lsc_t*    out_speed,
                                                ra_rmac_duplex_t* out_duplex)
{
  uint16_t anlpar = 0U;
  /* HUM Ch 33.4.1.1 "MPSM : PHY Station Management Register" p 1707 */
  const ra_err_t anlpar_err = ra_rmac_mdio_c22_read(port,
                                                    (uint8_t)k_ra_eth_phy_addr_default,
                                                    (uint8_t)k_ra_eth_phy_reg_anlpar,
                                                    &anlpar);
  if (anlpar_err != k_ra_ok) {
    return anlpar_err;
  }
  uint16_t       gbsr     = 0U;
  const ra_err_t gbsr_err = ra_rmac_mdio_c22_read(port,
                                                  (uint8_t)k_ra_eth_phy_addr_default,
                                                  (uint8_t)k_ra_eth_phy_reg_gbsr,
                                                  &gbsr);
  if (gbsr_err != k_ra_ok) {
    return gbsr_err;
  }
  g_ra_eth_anlpar = anlpar;
  g_ra_eth_gbsr   = gbsr;
  internal_pick_negotiated_speed(anlpar, gbsr, out_speed, out_duplex);
  g_ra_eth_resync_speed_lsc = (uint32_t)*out_speed;
  g_ra_eth_resync_duplex    = (uint32_t)*out_duplex;
  return k_ra_ok;
}

/**
 * @brief Drop ETHA to CONFIG, write MPIC, return to OPERATION.
 *
 * @details Bracketed write per HUM Ch 33.4.1.2 -- MPIC only sticks
 * while ETHA is in CONFIG. Always returns ETHA to OPERATION even if
 * the MPIC write itself failed, so the chip doesn't park in CONFIG.
 *
 * @param[in] port   RMAC port whose MPIC needs updating.
 * @param[in] speed  Target LSC value.
 * @param[in] duplex Target duplex value.
 *
 * @return ::ra_err_t error code.
 * @retval k_ra_ok              MPIC programmed.
 * @retval k_ra_err_hw_timeout  ETHA mode transition timed out.
 * @retval k_ra_err_invalid_arg ra_rmac_set_link rejected the args.
 *
 * @pre Auto-neg result has been read.
 * @pre port and speed/duplex are in range.
 * @post On k_ra_ok ETHA is in OPERATION, MPIC reflects the args.
 * @post On error ETHA is restored to OPERATION before returning.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
static ra_err_t internal_program_mpic(ra_rmac_port_t   port,
                                      ra_rmac_lsc_t    speed,
                                      ra_rmac_duplex_t duplex)
{
  const ra_etha_port_t etha_port = (port == k_ra_rmac_port_0)
                                       ? (ra_etha_port_t)k_ra_etha_port_0
                                       : (ra_etha_port_t)k_ra_etha_port_1;
  ra_err_t err = ra_etha_set_mode(etha_port, k_ra_etha_opc_disable);
  if (err != k_ra_ok) {
    return err;
  }
  err = ra_etha_set_mode(etha_port, k_ra_etha_opc_config);
  if (err != k_ra_ok) {
    return err;
  }
  /* HUM Table 29.11: MPIC.PIS tracks link speed, not the external
   * RGMII-ness -- GMII for 1 Gbps, MII for 10/100 Mbps. The ESWM
   * MIICR1.MIISEL field (programmed once by the board) is what
   * actually selects external RGMII. */
  ra_rmac_pis_t pis = k_ra_rmac_pis_mii;
  if (speed == k_ra_rmac_lsc_1000mbit) {
    pis = k_ra_rmac_pis_gmii;
  }
  const ra_err_t set_err = ra_rmac_set_link(port, pis, speed, duplex);
  const ra_err_t dis_err = ra_etha_set_mode(etha_port, k_ra_etha_opc_disable);
  const ra_err_t op_err  = ra_etha_set_mode(etha_port, k_ra_etha_opc_operation);
  if (set_err != k_ra_ok) {
    return set_err;
  }
  if (dis_err != k_ra_ok) {
    return dis_err;
  }
  return op_err;
}

/**
 * @brief Resync MPIC.LSC / MPIC.PIPP to match the PHY auto-neg result.
 *
 * @details
 * Top-level helper. Polls for AUTONEG_COMPLETE, reads ANLPAR / GBSR
 * for the actual negotiated speed/duplex, then writes MPIC inside
 * the ETHA CONFIG bracket. See ::internal_query_negotiated_speed and
 * ::internal_program_mpic for the per-step details.
 *
 * @param[in] port RMAC port whose MPIC needs updating.
 * @param[in] bmcr Unused -- kept for API stability with the old caller.
 *
 * @return ::ra_err_t error code.
 * @retval k_ra_ok              MPIC re-programmed.
 * @retval k_ra_err_invalid_arg ra_rmac_set_link rejected an arg.
 * @retval k_ra_err_hw_timeout  An ETHA mode transition or MDIO timed out.
 *
 * @pre ::ra_eth_open has succeeded (::s_state.opened == 1).
 * @pre ::s_mac_speed_resynced is false on first call after open.
 * @post On success MPIC matches the PHY's negotiated speed/duplex.
 * @post On success ::s_mac_speed_resynced is true.
 *
 * @note Not thread-safe; firmware drives this from a single thread.
 * @since 0.1.0
 */
static ra_err_t internal_resync_mac_speed(ra_rmac_port_t port, uint16_t bmcr)
{
  (void)bmcr;
  internal_wait_for_autoneg(port);
  ra_rmac_lsc_t    speed  = k_ra_rmac_lsc_10mbit;
  ra_rmac_duplex_t duplex = k_ra_rmac_duplex_half;
  const ra_err_t   q_err  = internal_query_negotiated_speed(port, &speed, &duplex);
  if (q_err != k_ra_ok) {
    return q_err;
  }
  const ra_err_t mpic_err = internal_program_mpic(port, speed, duplex);
  if (mpic_err != k_ra_ok) {
    return mpic_err;
  }
  s_mac_speed_resynced = true;
  ra_log_info(s_tag, "link_status: MPIC resynced to PHY speed/duplex");
  return k_ra_ok;
}

/**
 * @brief Read PHY BMSR + BMCR over MDIO and populate out_status.
 *
 * @details
 * Helper extracted from ra_eth_link_status so the per-condition
 * decoding (link_up, duplex, speed) stays under the NASA Rule 4
 * function-size threshold and clang-tidy
 * readability-function-size threshold.
 *
 * @param[in]  port       RMAC port carrying the PHY.
 * @param[out] out_status Link-state fields populated in place.
 * @param[out] out_bmcr   BMCR value (caller uses it for the MAC
 *                        speed-resync write).
 *
 * @return ::ra_err_t MDIO read outcome.
 * @retval k_ra_ok           Both BMSR + BMCR fetched cleanly.
 * @retval other             Underlying MDIO error from the first
 *                           failing read.
 *
 * @pre out_status != nullptr.
 * @pre out_bmcr != nullptr.
 * @post On success out_status->{bmsr, link_up, full_duplex,
 *       speed_mbps} are populated; out_bmcr holds the raw BMCR.
 * @post On error out_status / out_bmcr are unmodified.
 *
 * @note Not thread-safe; serialise PHY access with the rest of
 *       the ethernet bring-up.
 * @since 0.1.0
 */
static ra_err_t internal_phy_read_link(ra_rmac_port_t port,
                                       ra_eth_link_t* out_status,
                                       uint16_t*      out_bmcr)
{
  uint16_t bmsr = 0U;
  /* HUM Ch 33.4.1.1 "MPSM : PHY Station Management Register" p 1707 */
  const ra_err_t bmsr_err = ra_rmac_mdio_c22_read(port,
                                                  (uint8_t)k_ra_eth_phy_addr_default,
                                                  (uint8_t)k_ra_eth_phy_reg_bmsr,
                                                  &bmsr);
  RA_RETURN_ON_ERROR(bmsr_err, s_tag, "link_status: bmsr read"); /* GCOVR_EXCL_BR_LINE */

  uint16_t bmcr = 0U;
  /* HUM Ch 33.4.1.1 "MPSM : PHY Station Management Register" p 1707 */
  const ra_err_t bmcr_err = ra_rmac_mdio_c22_read(port,
                                                  (uint8_t)k_ra_eth_phy_addr_default,
                                                  (uint8_t)k_ra_eth_phy_reg_bmcr,
                                                  &bmcr);
  RA_RETURN_ON_ERROR(bmcr_err, s_tag, "link_status: bmcr read"); /* GCOVR_EXCL_BR_LINE */

  out_status->bmsr        = bmsr;
  out_status->link_up     = ((bmsr & k_ra_eth_phy_bmsr_link_up) != 0U) ? 1U : 0U;
  out_status->full_duplex = ((bmcr & k_ra_eth_phy_bmcr_duplex) != 0U) ? 1U : 0U;
  if ((bmcr & k_ra_eth_phy_bmcr_speed100) != 0U) {
    out_status->speed_mbps = k_ra_eth_phy_speed_100;
  } else {
    out_status->speed_mbps = k_ra_eth_phy_speed_10;
  }
  *out_bmcr = bmcr;
  return k_ra_ok;
}

/* Implementation of ra_eth_link_status (see header for full contract) -- see header for the documented contract. */
ra_err_t ra_eth_link_status(ra_eth_link_t* out_status)
{
  RA_CHECK_NULL_PTR(out_status, s_tag, "link_status: out must not be nullptr");
  if (s_state.opened == 0U) {
    return k_ra_err_not_initialized;
  }

  const ra_rmac_port_t port = internal_channel_to_port(s_state.cfg.channel);
  uint16_t             bmcr = 0U;
  const ra_err_t       err  = internal_phy_read_link(port, out_status, &bmcr);
  RA_RETURN_ON_ERROR(err, s_tag, "link_status: phy read");

  /* HUM Ch 33.4.1.2 "MPIC : PHY Interfaces Configuration Register"
   * p 1707: on first link-up after open, realign MPIC.LSC with the
   * PHY's negotiated speed. Sequential ifs (no compound boolean
   * operators) so each gate remains MC/DC-clean without a paired
   * test vector matrix. */
  if (out_status->link_up == 0U) {
    return k_ra_ok;
  }
  if (s_mac_speed_resynced) {
    return k_ra_ok;
  }
  return internal_resync_mac_speed(port, bmcr);
}

/* Implementation of ra_eth_get_stats (see header for full contract) -- see header for the documented contract. */
ra_err_t ra_eth_get_stats(ra_eth_stats_t* out_stats)
{
  RA_CHECK_NULL_PTR(out_stats, s_tag, "get_stats: out must not be nullptr");
  if (s_state.opened == 0U) {
    return k_ra_err_not_initialized;
  }
  *out_stats = s_state.stats;
  return k_ra_ok;
}

#ifdef UNIT_TEST
/**
 * @enum ra_eth_test_ds_bits_t
 * @brief 12-bit DS-field packing helpers for the test inject path.
 *
 * @details
 * Mirrors the ds_l/ds_h split applied throughout ra_eth_gwca's
 * descriptor pack/unpack helpers so the inject path can produce a
 * descriptor that ::ra_eth_gwca_default_recv decodes consistently.
 */
typedef enum : uint32_t {
  k_ra_eth_test_ds_low_mask   = 0xFFU, /**< ds_l: low 8 bits.        */
  k_ra_eth_test_ds_high_shift = 8U,    /**< ds_h packs bits 8..11.   */
  k_ra_eth_test_ds_high_mask  = 0x0FU, /**< ds_h field width 4 bits. */
} ra_eth_test_ds_bits_t;

/**
 * @brief Resolve the slot index the next inject_rx should target.
 *
 * @details
 * Mirrors the round-robin slot search ::ra_eth_gwca_default_recv
 * performs: the inject always targets the slot the next read would
 * pop, so a single inject + read pair round-trips even after
 * earlier reads have advanced the rx_head cursor.
 *
 * @return Slot index in [0, k_ra_eth_num_rx_desc).
 *
 * @details See implementation.
 * @retval index Index of the RX slot to populate.
 * @pre ::ra_eth_open has succeeded.
 * @pre ::s_gwca_state.rx_depth > 1 (init_ring enforces this).
 * @post Returned value is < (rx_depth - 1) (skips the LINK terminator).
 * @post No global state is modified.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
static uint32_t internal_test_rx_slot(void)
{
  const uint32_t data_slots = s_gwca_state.rx_depth - 1U;
  return s_gwca_state.rx_head % data_slots;
}

/**
 * @brief Test-only hook: simulate the GWCA RX engine releasing a frame.
 *
 * @details
 * Drops ``len`` bytes from ``payload`` into the RX buffer slice the
 * next ::ra_eth_read will pop, then marks the matching descriptor
 * FSINGLE with the encoded DS so the read returns the bytes. The
 * pool address is supplied by ::internal_eth_rx_pool which yields
 * the chip BSS on target builds and the simulator-mmap'd SRAM
 * region (fits in 40 bits) on host. This function is gated by
 * ``UNIT_TEST`` so it cannot reach firmware targets.
 *
 * @param[in] payload Bytes to drop into the RX buffer.
 * @param[in] len     Frame length (clamped to ::k_ra_eth_buf_size).
 *
 * @return ::ra_err_t Error code.
 * @retval k_ra_ok                  Frame staged.
 * @retval k_ra_err_not_initialized ::ra_eth_open was not called first.
 * @retval k_ra_err_null_ptr        payload is nullptr.
 *
 * @pre Driver previously brought up via ::ra_eth_open.
 * @pre payload points to len readable bytes.
 * @post Descriptor at the read pointer has dt = FSINGLE.
 * @post Descriptor's DS field equals min(len, k_ra_eth_buf_size).
 *
 * @note Test-only; not compiled into firmware builds.
 * @since 0.1.0
 */
ra_err_t ra_eth_test_inject_rx(const uint8_t* payload, uint32_t len)
{
  RA_CHECK_NULL_PTR(payload, s_tag, "test_inject_rx: payload nullptr");
  if (s_state.opened == 0U) {
    return k_ra_err_not_initialized;
  }
  const uint32_t              slot     = internal_test_rx_slot();
  ra_gwca_basic_descriptor_t* desc     = &s_rx_chain[slot];
  uint32_t                    copy_len = len;
  if (copy_len > (uint32_t)k_ra_eth_buf_size) {
    copy_len = (uint32_t)k_ra_eth_buf_size;
  }
  /* The descriptor's PTR was set by ::ra_eth_gwca_attach_buffers
   * during open. ::internal_eth_rx_pool returns the same low-address
   * (or BSS-on-chip) base; copy the payload to the matching slice so
   * default_recv pops the same bytes. */
  uint8_t* const pool_slot = internal_eth_rx_pool() + ((uintptr_t)slot * (uintptr_t)k_ra_eth_buf_size);
  internal_byte_copy(pool_slot, payload, copy_len);
  desc->ds_l = (uint8_t)(copy_len & (uint32_t)k_ra_eth_test_ds_low_mask);
  desc->ds_h = (uint8_t)((copy_len >> (uint32_t)k_ra_eth_test_ds_high_shift)
                         & (uint32_t)k_ra_eth_test_ds_high_mask);
  desc->dt   = (uint8_t)k_ra_gwdcc_dt_fsingle;
  return k_ra_ok;
}
#endif /* UNIT_TEST */
