/**
 * @file ra8_eth.c
 * @brief Ethernet Switch Module (ESWM) + frame TX/RX driver implementation
 *
 * @par Tag
 * [Ring 3 / HAL] {World: NS}
 *
 * @details
 * Driver for the RA8D2 Layer-3 ESWM block plus the polling-first
 * NIC API (``ra8_eth_open`` / ``write`` / ``read`` / ``close`` /
 * ``link_status`` / ``get_stats``). Owns the shared ethernet MSTP
 * gate (``k_ra8_mstp_eswm``) which is also referenced by the
 * ra8_eth_mfwd / ra8_eth_coma / ra8_eth_gwca / ra8_eth_gptp sub-drivers;
 * ra8_mstp keeps a reference count so concurrent enables / disables
 * interleave safely.
 *
 * The NIC frame path sits on top of the GWCA "default-state" API
 * (see ra8_eth_gwca.h / ra8_eth_mfwd.h). One TX queue + one RX queue
 * are wired through a fixed LINKFIX table, each backed by a static
 * descriptor chain + per-slot buffer pool in BSS. ra8_eth_open walks
 * the canonical bring-up (LINKFIX install + queue configure + OPC
 * transition to OPERATION) and programs MFWD so inbound frames land
 * on the RX queue; ra8_eth_write/ra8_eth_read delegate to the
 * default_send/default_recv helpers.
 *
 * Every register access carries a HUM Ch 29 / Ch 34 citation.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra8_eth.h"

#include <stdint.h>

#include "ra8_attributes.h"
#include "ra8_check.h"
#include "ra8_err.h"
#include "ra8_eth_gwca.h"
#include "ra8_eth_internal.h"
#include "ra8_eth_mfwd.h"
#include "ra8_etha.h"
#include "ra8_etha_regs.h"
#include "ra8_ether_regs.h"
#include "ra8_log.h"
#include "ra8_mstp.h"
#include "ra8_mstp_regs.h"
#include "ra8_rmac.h"
#include "ra8_rmac_regs.h"
#include "ra8_time.h"

/**
 * @var s_tag
 * @brief Logger tag used by every ra8_eth_* call.
 */
static const char* s_tag = "ETH";

/**
 * @var s_eth_fn
 * @brief Attached ESWM event callback (nullptr if none).
 */
static ra8_eth_event_fn_t s_eth_fn;

/**
 * @var s_eth_ctx
 * @brief Opaque cookie passed to ::s_eth_fn on dispatch.
 */
static void* s_eth_ctx;

/** @brief Per-port FWPBFC filter mask (7-bit). */
typedef enum : uint8_t {
  k_eth_fwpbfc_mask = 0x7FU, /**< Ethernet fwpbfc mask. */
} eth_fwpbfc_t;

/**
 * @enum ra8_eth_layout_t
 * @brief Static layout constants for the GWCA-backed NIC rings.
 *
 * @details
 * The driver wires one RX queue + one TX queue through a 4-entry
 * LINKFIX table. RX uses queue index 0, TX uses queue index 1, and
 * the remaining LINKFIX slots stay LEMPTY (queue disabled). Each
 * chain has ``k_ra8_eth_num_*_desc`` data slots plus one trailing
 * LINK terminator that ::ra8_eth_gwca_init_ring reserves.
 */
typedef enum : uint16_t {
  k_ra8_eth_linkfix_count = 4U,                                     /**< LINKFIX entries.  */
  k_ra8_eth_def_rx_q_idx  = 0U,                                     /**< RX LINKFIX index. */
  k_ra8_eth_def_tx_q_idx  = 1U,                                     /**< TX LINKFIX index. */
  k_ra8_eth_rx_ring_depth = (uint16_t)(k_ra8_eth_num_rx_desc + 1U), /**< +1 LINK term.     */
  k_ra8_eth_tx_ring_depth = (uint16_t)(k_ra8_eth_num_tx_desc + 1U), /**< +1 LINK term.     */
} ra8_eth_layout_t;

/**
 * @enum ra8_eth_step_t
 * @brief Step values bumped through ::g_ra8_eth_open_step for
 *        bench-side post-mortem via J-Link mem32.
 *
 * @details Successful progression uses 0..4. Error codes are
 * ``0x10 | N`` so a JTAG-attached operator can tell at a glance
 * whether the chip parked on a happy-path step or an error path.
 */
typedef enum : uint32_t {
  k_ra8_eth_step_ok_1   = 1U,    /**< RA8 Ethernet step ok 1.   */
  k_ra8_eth_step_ok_2   = 2U,    /**< RA8 Ethernet step ok 2.   */
  k_ra8_eth_step_ok_3   = 3U,    /**< RA8 Ethernet step ok 3.   */
  k_ra8_eth_step_ok_4   = 4U,    /**< RA8 Ethernet step ok 4.   */
  k_ra8_eth_step_fail_1 = 0x11U, /**< RA8 Ethernet step fail 1. */
  k_ra8_eth_step_fail_3 = 0x13U, /**< RA8 Ethernet step fail 3. */
  k_ra8_eth_step_fail_4 = 0x14U, /**< RA8 Ethernet step fail 4. */
} ra8_eth_step_t;

/**
 * @var s_linkfix_table
 * @brief Static LINKFIX table covering RX + TX queues + slack.
 *
 * @details
 * Lives in BSS, 16-byte aligned. Slots 0/1 are wired by
 * ::ra8_eth_gwca_default_open to the RX / TX chains; the remaining
 * slots stay LEMPTY so traffic only reaches the queues we own.
 *
 * @note File-scope, not thread-safe.
 * @since 0.1.0
 */
alignas(16) static ra8_gwca_basic_descriptor_t s_linkfix_table[k_ra8_eth_linkfix_count];

/**
 * @var s_rx_chain
 * @brief Static RX descriptor chain (BSS, 16-byte aligned).
 *
 * @details
 * ``k_ra8_eth_num_rx_desc`` FEMPTY slots + 1 LINK terminator.
 *
 * @note File-scope, not thread-safe.
 * @since 0.1.0
 */
alignas(16) static ra8_gwca_basic_descriptor_t s_rx_chain[k_ra8_eth_rx_ring_depth];

/**
 * @var s_tx_chain
 * @brief Static TX descriptor chain (BSS, 16-byte aligned).
 *
 * @details
 * ``k_ra8_eth_num_tx_desc`` FEMPTY slots + 1 LINK terminator. The TX
 * queue uses 16-byte EXTENDED descriptors (GWDCC.EDE = 1) so each
 * frame carries its INFO1 routing metadata; hence the element type
 * is ::ra8_gwca_ext_descriptor_t, not the 8-byte basic descriptor the
 * RX chain and LINKFIX table use.
 *
 * @note File-scope, not thread-safe.
 * @since 0.1.0
 */
alignas(16) static ra8_gwca_ext_descriptor_t s_tx_chain[k_ra8_eth_tx_ring_depth];

#ifndef UNIT_TEST
/**
 * @var s_rx_pool_storage
 * @brief Static RX buffer pool, one ``k_ra8_eth_buf_size`` slice per slot.
 *
 * @details
 * On the chip target the pool lives in BSS, which fits in the 40-bit
 * PTR field every ::ra8_gwca_basic_descriptor_t encodes. On host the
 * BSS sits above the 40-bit cap; ::internal_eth_rx_pool / _tx_pool
 * redirect there to the fake mmap'd SRAM region instead so the
 * descriptor PTR round-trips through ::ra8_eth_gwca_default_recv.
 *
 * @note File-scope, not thread-safe.
 * @since 0.1.0
 */
alignas(16) static uint8_t s_rx_pool_storage[k_ra8_eth_num_rx_desc * k_ra8_eth_buf_size];

/**
 * @var s_tx_pool_storage
 * @brief Static TX buffer pool, one ``k_ra8_eth_buf_size`` slice per slot.
 *
 * @details See ::s_rx_pool_storage; the host build steers around BSS.
 *
 * @note File-scope, not thread-safe.
 * @since 0.1.0
 */
alignas(16) static uint8_t s_tx_pool_storage[k_ra8_eth_num_tx_desc * k_ra8_eth_buf_size];
#endif /* UNIT_TEST */

/**
 * @enum ra8_eth_host_pool_layout_t
 * @brief Host-test pool addresses in the fake mmap'd SRAM region.
 *
 * @details
 * The host SRAM mmap window is 2 MiB at 0x22000000; the RX + TX pools
 * + LINKFIX backing each get a 1 MiB-aligned slice well inside that
 * window. The chip path never sees these values because the
 * ::internal_eth_rx_pool / _tx_pool helpers compile to the BSS arrays
 * when UNIT_TEST is undefined.
 */
typedef enum : uintptr_t {
  k_ra8_eth_host_rx_pool_addr = 0x22100000UL, /**< Host RX pool base. */
  k_ra8_eth_host_tx_pool_addr = 0x22180000UL, /**< Host TX pool base. */
} ra8_eth_host_pool_layout_t;

/**
 * @brief Resolve the RX buffer pool pointer for the current build.
 *
 * @return Address of an ``k_ra8_eth_num_rx_desc * k_ra8_eth_buf_size``
 *         byte buffer.
 *
 * @details See implementation.
 * @retval pointer Non-null pool base address.
 * @pre Storage is reserved (BSS on chip; fake-mapped SRAM on host).
 * @pre Caller invokes this once per ::ra8_eth_open.
 * @post Returned pointer is 16-byte aligned.
 * @post No global state is modified.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL
static inline uint8_t* internal_eth_rx_pool(void)
{
#ifdef UNIT_TEST
  return (uint8_t*)k_ra8_eth_host_rx_pool_addr;
#else
  return s_rx_pool_storage;
#endif
}

/**
 * @brief Resolve the TX buffer pool pointer for the current build.
 *
 * @return Address of an ``k_ra8_eth_num_tx_desc * k_ra8_eth_buf_size``
 *         byte buffer.
 *
 * @details See implementation.
 * @retval pointer Non-null pool base address.
 * @pre Storage is reserved (BSS on chip; fake-mapped SRAM on host).
 * @pre Caller invokes this once per ::ra8_eth_open.
 * @post Returned pointer is 16-byte aligned.
 * @post No global state is modified.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL
static inline uint8_t* internal_eth_tx_pool(void)
{
#ifdef UNIT_TEST
  return (uint8_t*)k_ra8_eth_host_tx_pool_addr;
#else
  return s_tx_pool_storage;
#endif
}

/**
 * @var s_gwca_state
 * @brief GWCA default-state block backing ::ra8_eth_open / write / read.
 *
 * @details
 * Populated by ::ra8_eth_open before calling
 * ::ra8_eth_gwca_default_open; carries the rings + buffer pools +
 * cursors for one RX + one TX queue.
 *
 * @note File-scope, not thread-safe.
 * @since 0.1.0
 */
static ra8_eth_gwca_default_state_t s_gwca_state;

/**
 * @var s_eth_state
 * @brief Singleton NIC runtime state.
 *
 * @details
 * Defined here and shared with the split-out link-status TU
 * (ra8_eth_link.c) via ra8_eth_internal.h.
 *
 * @note File-scope, not thread-safe.
 * @since 0.1.0
 */
ra8_eth_state_t s_eth_state;

/**
 * @var g_eth_mac_speed_resynced
 * @brief Latch -- true once ::ra8_eth_link_status has re-programmed
 *        MPIC.LSC / MPIC.PIPP to match the PHY's negotiated link.
 *
 * @details
 * Reset to false in ::ra8_eth_open / ::ra8_eth_close. The MAC speed
 * resync only needs to fire once per link bring-up; further calls
 * to ::ra8_eth_link_status skip the MPIC write so they remain
 * read-only status pollers. Defined here and updated from
 * ra8_eth_link.c via ra8_eth_internal.h.
 *
 * @note File-scope state, not thread-safe.
 * @since 0.1.0
 */
bool g_eth_mac_speed_resynced = false;

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
RA8_INTERNAL
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
RA8_INTERNAL
static inline void internal_byte_copy(uint8_t* dst, const uint8_t* src, uint32_t n)
{
  for (uint32_t i = 0U; i < n; ++i) {
    dst[i] = src[i];
  }
}

ra8_rmac_port_t priv_ra8_eth_channel_to_port(uint8_t channel)
{
  if (channel == 0U) {
    return k_ra8_rmac_port_0;
  }
  return k_ra8_rmac_port_1;
}

/**
 * @brief Validate the cfg ring sizes and resolve zero-as-default values.
 *
 * @param[in]  cfg      User cfg (already null-checked).
 * @param[out] tx_count Resolved TX descriptor count.
 * @param[out] rx_count Resolved RX descriptor count.
 * @param[out] buf_size Resolved per-descriptor buffer size.
 *
 * @return ::ra8_err_t Error code.
 * @retval k_ra8_ok               Sizes resolved.
 * @retval k_ra8_err_invalid_arg  Some count was out of range.
 *
 * @pre cfg is non-null.
 * @pre out pointers are non-null.
 * @post On k_ra8_ok the *tx_count / *rx_count / *buf_size are set.
 * @post On error no out param is touched in a way that would mislead.
 *
 * @details See implementation.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_resolve_sizes(const ra8_eth_cfg_t* cfg,
                                        uint16_t*            tx_count,
                                        uint16_t*            rx_count,
                                        uint16_t*            buf_size)
{
  uint16_t tx = cfg->num_tx_descriptors;
  uint16_t rx = cfg->num_rx_descriptors;
  uint16_t bs = cfg->buffer_size;
  if (tx == 0U) {
    tx = k_ra8_eth_num_tx_desc;
  }
  if (rx == 0U) {
    rx = k_ra8_eth_num_rx_desc;
  }
  if (bs == 0U) {
    bs = k_ra8_eth_buf_size;
  }
  /* mcdc-deactivated: tx normalized to nonzero above; first OR-condition unreachable. */
  if ((tx == 0U) || (tx > k_ra8_eth_num_tx_desc)) {
    return k_ra8_err_invalid_arg;
  }
  /* mcdc-deactivated: rx normalized to nonzero above; first OR-condition unreachable. */
  if ((rx == 0U) || (rx > k_ra8_eth_num_rx_desc)) {
    return k_ra8_err_invalid_arg;
  }
  if ((bs < k_ra8_eth_min_frame) || (bs > k_ra8_eth_buf_size)) {
    return k_ra8_err_invalid_arg;
  }
  *tx_count = tx;
  *rx_count = rx;
  *buf_size = bs;
  return k_ra8_ok;
}

/**
 * @brief Bring the per-channel RMAC port up and program the MAC address.
 *
 * @param[in] cfg User configuration.
 *
 * @return ::ra8_err_t Error code.
 *
 * @details See implementation.
 * @retval k_ra8_ok Operation succeeded.
 * @pre cfg is non-null and cfg->channel is in [0, 1].
 * @pre Caller has already enabled the ESWM MSTP gate.
 * @post On success the RMAC port carries cfg->mac_address.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_bring_up_rmac(const ra8_eth_cfg_t* cfg)
{
  /* The board-level ``ra8_board_ethernet_init`` (or its equivalent for
   * non-EK boards) is expected to have already programmed RMAC's MPIC
   * (PIS / LSC / PIPP / PSMCS), enabled the MSTP gate, and brought the
   * matching ETHA into OPERATION mode. Overwriting MPIC here would
   * clobber the carefully-computed MDC clock divider (PSMCS) and force
   * the wrong PHY interface mode (MII instead of RGMII). The only
   * RMAC-level state ``ra8_eth_open`` still has to programme is the
   * MAC address the application chose.
   *
   * HUM Ch 33.4 "MRMAC0 / MRMAC1" p 1707 -- the MAC address registers
   * are paired with MPIC under the same write-once gate: silent on
   * MAC writes while ETHA is in OPERATION. Bench-confirmed on
   * EK-RA8D2: without the DISABLE -> CONFIG -> {MAC write} -> DISABLE
   * -> OPERATION bracket, MRMAC0 / MRMAC1 read back as 0 after
   * ra8_eth_open completes, and every wire-side ARP "who-has 192.168.
   * .1.42" gets dropped at the RMAC perfect-match comparator before
   * the GWCA descriptor ring ever sees it.
   *
   * The board layer ran the bracket once during ra8_rmac_init; we have
   * to repeat it here so the application-supplied MAC actually lands
   * in MRMAC0/MRMAC1. */
  const ra8_rmac_port_t rmac_port = priv_ra8_eth_channel_to_port(cfg->channel);
  const ra8_etha_port_t etha_port =
    (cfg->channel == 0U) ? (ra8_etha_port_t)k_ra8_etha_port_0 : (ra8_etha_port_t)k_ra8_etha_port_1;
  uint8_t mac_copy[k_ra8_eth_mac_len];
  for (uint8_t i = 0U; i < (uint8_t)k_ra8_eth_mac_len; ++i) {
    mac_copy[i] = cfg->mac_address[i];
  }

  /* HUM Ch 32.3.1.1 "EAMC : Mode Command Register" p 1631 -- ETHA
   * mode-transition sequence. DISABLE before CONFIG so the ESWM
   * accepts the request even if the port was already in OPERATION. */
  ra8_err_t err = ra8_etha_set_mode(etha_port, k_ra8_etha_opc_disable);
  if (err != k_ra8_ok) {
    return err;
  }
  err = ra8_etha_set_mode(etha_port, k_ra8_etha_opc_config);
  if (err != k_ra8_ok) {
    return err;
  }

  /* HUM Ch 33.4 "MRMAC0 / MRMAC1" p 1707 -- writes only stick while
   * the paired ETHA is in CONFIG. */
  const ra8_err_t mac_err = ra8_rmac_set_mac_address(rmac_port, mac_copy);

  /* Always return to OPERATION even on MAC write failure so the chip
   * doesn't end up parked in CONFIG. */
  const ra8_err_t dis_err = ra8_etha_set_mode(etha_port, k_ra8_etha_opc_disable);
  const ra8_err_t op_err  = ra8_etha_set_mode(etha_port, k_ra8_etha_opc_operation);
  if (mac_err != k_ra8_ok) {
    return mac_err;
  }
  if (dis_err != k_ra8_ok) {
    return dis_err;
  }
  return op_err;
}

ra8_err_t ra8_eth_init(void)
{
  /* HUM Ch 11.2.8 "MSTPCRC : Module Stop Control Register C" p 446 */
  const ra8_err_t mst_err = ra8_mstp_enable(k_ra8_mstp_eswm);
  RA8_RETURN_ON_ERROR(mst_err, s_tag, "eth_init: mstp enable"); /* GCOVR_EXCL_BR_LINE */

  volatile r_eswm_regs_t* reg = ra8_eswm();
  /* HUM Ch 29 "Layer 3 Ethernet Switch Module (ESWM)" p 1287 */
  reg->ESWM_CTRL = 0U;
  reg->ESWM_STS  = 0U;
  reg->ESWM_IE   = 0U;
  reg->ESWM_ICLR = 0U;
  ra8_log_info(s_tag, "eth_init (ESWM)");
  return k_ra8_ok;
}

ra8_err_t ra8_eth_deinit(void)
{
  volatile r_eswm_regs_t* reg = ra8_eswm();
  /* HUM Ch 29 "Layer 3 Ethernet Switch Module (ESWM)" p 1287 */
  reg->ESWM_CTRL = 0U;
  reg->ESWM_IE   = 0U;
  s_eth_fn       = nullptr;
  s_eth_ctx      = nullptr;
  return ra8_mstp_disable(k_ra8_mstp_eswm);
}

ra8_err_t ra8_eth_get_status(uint32_t* out_mask)
{
  RA8_CHECK_NULL_PTR(out_mask, s_tag, "out_mask must not be nullptr");
  /* HUM Ch 29 "Layer 3 Ethernet Switch Module (ESWM)" p 1287 */
  *out_mask = ra8_eswm()->ESWM_STS;
  return k_ra8_ok;
}

ra8_err_t ra8_eth_clear_status(uint32_t mask)
{
  volatile r_eswm_regs_t* reg = ra8_eswm();
  /* HUM Ch 29 "Layer 3 Ethernet Switch Module (ESWM)" p 1287 */
  reg->ESWM_ICLR = mask;
  reg->ESWM_STS  = reg->ESWM_STS & ~mask;
  return k_ra8_ok;
}

ra8_err_t ra8_eth_attach_handler(ra8_eth_event_fn_t fn, void* ctx)
{
  s_eth_fn  = fn;
  s_eth_ctx = ctx;
  return k_ra8_ok;
}

RA8_ISR_SAFE
void ra8_eth_dispatch(void)
{
  volatile r_eswm_regs_t* reg = ra8_eswm();
  /* HUM Ch 29 "Layer 3 Ethernet Switch Module (ESWM)" p 1287 */
  const uint32_t           mask = reg->ESWM_STS;
  const ra8_eth_event_fn_t fn   = s_eth_fn;
  void* const              ctx  = s_eth_ctx;
  reg->ESWM_ICLR                = mask;
  reg->ESWM_STS                 = 0U;
  if (fn != nullptr) {
    fn(ctx, mask);
  }
}

ra8_err_t ra8_eth_enter_stop(void)
{
  /* HUM Ch 29 "Layer 3 Ethernet Switch Module (ESWM)" p 1287 */
  ra8_eswm()->ESWM_CTRL = 0U;
  return ra8_mstp_disable(k_ra8_mstp_eswm);
}

ra8_err_t ra8_eth_exit_stop(void)
{
  return ra8_mstp_enable(k_ra8_mstp_eswm);
}

/* -----------------------------------------------------------------------
 * Frame-level NIC API.
 * -------------------------------------------------------------------- */

/**
 * @brief Capture cfg into ::s_eth_state and reset the counters.
 *
 * @param[in] cfg Validated configuration.
 *
 * @pre cfg is non-null.
 * @pre ::ra8_eth_gwca_default_open has already succeeded.
 * @post ::s_eth_state holds the new cfg and zeroed counters.
 * @post ::s_eth_state.opened is 1.
 *
 * @details See implementation.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
RA8_INTERNAL
static void internal_capture_state(const ra8_eth_cfg_t* cfg)
{
  s_eth_state.opened       = 1U;
  s_eth_state.cfg          = *cfg;
  s_eth_state.stats.tx_ok  = 0U;
  s_eth_state.stats.rx_ok  = 0U;
  s_eth_state.stats.tx_err = 0U;
  s_eth_state.stats.rx_err = 0U;
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
RA8_INTERNAL
static void internal_populate_gwca_state(const ra8_eth_cfg_t* cfg)
{
  s_gwca_state.linkfix_table  = s_linkfix_table;
  s_gwca_state.linkfix_count  = (uint32_t)k_ra8_eth_linkfix_count;
  s_gwca_state.rx_chain       = s_rx_chain;
  s_gwca_state.rx_depth       = (uint32_t)k_ra8_eth_rx_ring_depth;
  s_gwca_state.rx_slot_bytes  = (uint32_t)k_ra8_eth_buf_size;
  s_gwca_state.rx_pool        = internal_eth_rx_pool();
  s_gwca_state.rx_queue_index = (uint32_t)k_ra8_eth_def_rx_q_idx;
  s_gwca_state.rx_head        = 0U;
  s_gwca_state.tx_chain       = s_tx_chain;
  s_gwca_state.tx_depth       = (uint32_t)k_ra8_eth_tx_ring_depth;
  s_gwca_state.tx_slot_bytes  = (uint32_t)k_ra8_eth_buf_size;
  s_gwca_state.tx_pool        = internal_eth_tx_pool();
  s_gwca_state.tx_queue_index = (uint32_t)k_ra8_eth_def_tx_q_idx;
  s_gwca_state.tx_tail        = 0U;
  s_gwca_state.mac_port       = cfg->channel;
}

/**
 * @brief Validate cfg, resolve ring sizes, and bring up MSTP + RMAC.
 *
 * @details
 * Rolled out of ::ra8_eth_open so that single function stays under
 * the project's clang-tidy function-size threshold. Performs:
 * 1. cfg channel range check.
 * 2. Ring size resolution (for ABI validation only; defaults if 0).
 * 3. MSTP gate enable.
 * 4. Per-channel RMAC bring-up + MAC address program.
 *
 * @param[in]  cfg User configuration (already null-checked by caller).
 *
 * @return ::ra8_err_t Error code.
 *
 * @retval k_ra8_ok Operation succeeded.
 * @pre cfg is non-null.
 * @pre Caller is single-threaded with respect to this driver.
 * @post On success the ESWM MSTP gate is enabled and the RMAC port
 *       carries cfg->mac_address.
 * @post On failure no state is changed visibly to the caller.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_open_prep(const ra8_eth_cfg_t* cfg)
{
  if (cfg->channel > 1U) {
    ra8_log_error(s_tag, "open: channel out of range");
    return k_ra8_err_invalid_arg;
  }
  uint16_t        tx_count   = 0U;
  uint16_t        rx_count   = 0U;
  uint16_t        buf_size   = 0U;
  const ra8_err_t resolve_rc = internal_resolve_sizes(cfg, &tx_count, &rx_count, &buf_size);
  if (resolve_rc != k_ra8_ok) {
    return resolve_rc;
  }
  const ra8_err_t mst_err = ra8_mstp_enable(k_ra8_mstp_eswm);
  if (mst_err != k_ra8_ok) {
    return mst_err;
  }
  return internal_bring_up_rmac(cfg);
}

/**
 * @var g_ra8_eth_open_step
 * @brief Bench-side debug trail -- bumped by ::ra8_eth_open to record
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
volatile uint32_t g_ra8_eth_open_step;

/**
 * @brief Walk the GWCA bring-up + MFWD routing setup for ::ra8_eth_open.
 *
 * @details
 * Splits the GWCA + MFWD wiring out of ::ra8_eth_open so the entry
 * point stays under the function-size budget. Calls
 * ::ra8_eth_gwca_default_open to install the LINKFIX table, configure
 * the RX + TX queues and transition GWMC.OPC to OPERATION, then
 * programs ``MFWD.FWPBFCSDC0[port].PBCSD = rx_queue_index`` via
 * ::ra8_eth_mfwd_route_queue so port-to-host frames reach the RX
 * queue on real silicon. ``g_ra8_eth_open_step`` is bumped along
 * the way so a JTAG-attached operator can see exactly which sub
 * primitive failed.
 *
 * @return ::ra8_err_t Error code.
 * @retval k_ra8_ok              GWCA in OPERATION + MFWD routing live.
 * @retval k_ra8_err_invalid_arg ::s_gwca_state fields invalid.
 * @retval k_ra8_err_hw_timeout  GWMC.OPC transition never converged.
 *
 * @pre ::internal_populate_gwca_state has run.
 * @pre ::internal_open_prep returned ok.
 * @post On success GWMC.OPC == OPERATION and MFWD routes inbound
 *       frames from the configured port into the RX queue.
 * @post On failure GWCA may be in DISABLE; caller may retry by
 *       re-invoking ::ra8_eth_open.
 *
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_open_gwca_path(void)
{
  /* Note: ``ra8_board_ethernet_init`` already runs its own COMA reset
   * (RR pulse + RCEC|ACE enable) via ``internal_eth_coma_reset`` --
   * the full FSP CABPIRM.BPIOG/BPR handshake is intentionally skipped
   * there because BPR never asserts on EK-RA8D2 silicon unless GWCA
   * is also being brought up, and the board can't depend on that. */

  /* Per FSP r_layer3_switch_open: program the per-port forwarding
   * destination masks BEFORE bringing the GWCA up. 0x7F = allow all
   * destinations (permissive baseline; tightens once L3 filtering
   * lands). */
  static const uint8_t local_fwpbfc_masks[3] = {k_eth_fwpbfc_mask,
                                                k_eth_fwpbfc_mask,
                                                k_eth_fwpbfc_mask};
  (void)ra8_eth_mfwd_set_forwarding_masks(local_fwpbfc_masks);

  const ra8_err_t err = ra8_eth_gwca_default_open(&s_gwca_state);
  if (err != k_ra8_ok) {
    g_ra8_eth_open_step = (uint32_t)k_ra8_eth_step_fail_3;
    return err;
  }
  g_ra8_eth_open_step = (uint32_t)k_ra8_eth_step_ok_3;
  const ra8_err_t mfwd_err =
    ra8_eth_mfwd_route_queue(s_gwca_state.mac_port, (uint8_t)s_gwca_state.rx_queue_index);
  if (mfwd_err != k_ra8_ok) {
    g_ra8_eth_open_step = (uint32_t)k_ra8_eth_step_fail_4;
    return mfwd_err;
  }
  g_ra8_eth_open_step = (uint32_t)k_ra8_eth_step_ok_4;
  return k_ra8_ok;
}

ra8_err_t ra8_eth_open(const ra8_eth_cfg_t* cfg)
{
  RA8_CHECK_NULL_PTR(cfg, s_tag, "open: cfg must not be nullptr");

  g_ra8_eth_open_step     = 0U;
  const ra8_err_t prep_rc = internal_open_prep(cfg);
  if (prep_rc != k_ra8_ok) {
    g_ra8_eth_open_step = (uint32_t)k_ra8_eth_step_fail_1;
    return prep_rc;
  }
  g_ra8_eth_open_step = (uint32_t)k_ra8_eth_step_ok_1;

  internal_populate_gwca_state(cfg);
  g_ra8_eth_open_step = (uint32_t)k_ra8_eth_step_ok_2;

  const ra8_err_t bring_up_rc = internal_open_gwca_path();
  if (bring_up_rc != k_ra8_ok) {
    return bring_up_rc;
  }

  /* Force a fresh MPIC re-sync on the next ::ra8_eth_link_status call
   * so the on-chip RMAC's MPIC.LSC matches whatever speed the PHY
   * actually negotiated (HUM Ch 33.4.1.2 "MPIC : PHY Interfaces
   * Configuration Register" p 1707). */
  g_eth_mac_speed_resynced = false;

  internal_capture_state(cfg);

  /* Bench-confirmed (issue #1 follow-up): leaving MPIC.PIS at the
   * default GMII (1000mbit) configured by ra8_board_ethernet_init
   * silently drops every RX frame when the actual link auto-
   * negotiates to 100 Mbps. The PIS must match the *internal* xMII
   * timing (MII for 10/100, GMII for 1G). Force a resync at the end
   * of open so the MAC is ready for RX before the IP stack hits the
   * driver -- without it MRGFCE stays at zero until something else
   * calls ra8_eth_link_status, which NetX never does at boot. */
  ra8_eth_link_t link = {.link_up = 0U, .speed_mbps = 0U, .full_duplex = 0U, .bmsr = 0U};
  (void)ra8_eth_link_status(&link);

  ra8_log_info(s_tag, "eth_open: gwca rings ready");
  return k_ra8_ok;
}

ra8_err_t ra8_eth_close(void)
{
  if (s_eth_state.opened == 0U) {
    return k_ra8_err_not_initialized;
  }

  /* Park the GWCA state machine in DISABLE. Best-effort -- ignore
   * the err so teardown always reaches the MSTP-gate balance below.
   * HUM Ch 34.3.1 "GWMC : Mode Configuration Register" p 1797 */
  (void)ra8_eth_gwca_set_operation_mode(k_ra8_gwmc_opc_disable);

  /* Tear down the local RMAC port (MSTP-gate ref-counted). */
  const ra8_rmac_port_t rmac_port = priv_ra8_eth_channel_to_port(s_eth_state.cfg.channel);
  (void)ra8_rmac_deinit(rmac_port);

  /* Balance the second ESWM-MSTP enable that ::ra8_eth_gwca_default_open
   * acquired via ::ra8_eth_gwca_init. The first enable, owned by
   * ::internal_open_prep, is balanced by the final ra8_mstp_disable
   * call below. */
  (void)ra8_eth_gwca_deinit();

  s_eth_state.opened       = 0U;
  g_eth_mac_speed_resynced = false;
  return ra8_mstp_disable(k_ra8_mstp_eswm);
}

ra8_err_t ra8_eth_write(const uint8_t* buf, uint32_t len)
{
  RA8_CHECK_NULL_PTR(buf, s_tag, "write: buf must not be nullptr");
  if (s_eth_state.opened == 0U) {
    return k_ra8_err_not_initialized;
  }
  if ((len < k_ra8_eth_min_frame) || (len > k_ra8_eth_max_frame)) {
    ra8_log_error(s_tag, "write: frame length out of range");
    return k_ra8_err_invalid_arg;
  }

  const ra8_err_t err = ra8_eth_gwca_default_send(&s_gwca_state, buf, len);
  if (err == k_ra8_ok) {
    internal_stat_inc(&s_eth_state.stats.tx_ok);
    return k_ra8_ok;
  }
  internal_stat_inc(&s_eth_state.stats.tx_err);
  return err;
}

ra8_err_t ra8_eth_read(uint8_t* buf, uint32_t max_len, uint32_t* got_len)
{
  RA8_CHECK_NULL_PTR(buf, s_tag, "read: buf must not be nullptr");
  RA8_CHECK_NULL_PTR(got_len, s_tag, "read: got_len must not be nullptr");
  if (s_eth_state.opened == 0U) {
    return k_ra8_err_not_initialized;
  }
  if (max_len == 0U) {
    return k_ra8_err_invalid_arg;
  }

  const ra8_err_t err = ra8_eth_gwca_default_recv(&s_gwca_state, buf, max_len, got_len);
  if (err == k_ra8_ok) {
    internal_stat_inc(&s_eth_state.stats.rx_ok);
    return k_ra8_ok;
  }
  internal_stat_inc(&s_eth_state.stats.rx_err);
  if (err == k_ra8_err_no_data) {
    *got_len = 0U;
    return k_ra8_err_no_data;
  }
  return err;
}

ra8_err_t ra8_eth_get_stats(ra8_eth_stats_t* out_stats)
{
  RA8_CHECK_NULL_PTR(out_stats, s_tag, "get_stats: out must not be nullptr");
  if (s_eth_state.opened == 0U) {
    return k_ra8_err_not_initialized;
  }
  *out_stats = s_eth_state.stats;
  return k_ra8_ok;
}

#ifdef UNIT_TEST
/**
 * @enum ra8_eth_test_ds_bits_t
 * @brief 12-bit DS-field packing helpers for the test inject path.
 *
 * @details
 * Mirrors the ds_l/ds_h split applied throughout ra8_eth_gwca's
 * descriptor pack/unpack helpers so the inject path can produce a
 * descriptor that ::ra8_eth_gwca_default_recv decodes consistently.
 */
typedef enum : uint32_t {
  k_ra8_eth_test_ds_low_mask   = 0xFFU, /**< ds_l: low 8 bits.        */
  k_ra8_eth_test_ds_high_shift = 8U,    /**< ds_h packs bits 8..11.   */
  k_ra8_eth_test_ds_high_mask  = 0x0FU, /**< ds_h field width 4 bits. */
} ra8_eth_test_ds_bits_t;

/**
 * @brief Resolve the slot index the next inject_rx should target.
 *
 * @details
 * Mirrors the round-robin slot search ::ra8_eth_gwca_default_recv
 * performs: the inject always targets the slot the next read would
 * pop, so a single inject + read pair round-trips even after
 * earlier reads have advanced the rx_head cursor.
 *
 * @return Slot index in [0, k_ra8_eth_num_rx_desc).
 *
 * @details See implementation.
 * @retval index Index of the RX slot to populate.
 * @pre ::ra8_eth_open has succeeded.
 * @pre ::s_gwca_state.rx_depth > 1 (init_ring enforces this).
 * @post Returned value is < (rx_depth - 1) (skips the LINK terminator).
 * @post No global state is modified.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL
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
 * next ::ra8_eth_read will pop, then marks the matching descriptor
 * FSINGLE with the encoded DS so the read returns the bytes. The
 * pool address is supplied by ::internal_eth_rx_pool which yields
 * the chip BSS on target builds and the fake mmap'd SRAM
 * region (fits in 40 bits) on host. This function is gated by
 * ``UNIT_TEST`` so it cannot reach firmware targets.
 *
 * @param[in] payload Bytes to drop into the RX buffer.
 * @param[in] len     Frame length (clamped to ::k_ra8_eth_buf_size).
 *
 * @return ::ra8_err_t Error code.
 * @retval k_ra8_ok                  Frame staged.
 * @retval k_ra8_err_not_initialized ::ra8_eth_open was not called first.
 * @retval k_ra8_err_null_ptr        payload is nullptr.
 *
 * @pre Driver previously brought up via ::ra8_eth_open.
 * @pre payload points to len readable bytes.
 * @post Descriptor at the read pointer has dt = FSINGLE.
 * @post Descriptor's DS field equals min(len, k_ra8_eth_buf_size).
 *
 * @note Test-only; not compiled into firmware builds.
 * @since 0.1.0
 */
ra8_err_t ra8_eth_test_inject_rx(const uint8_t* payload, uint32_t len)
{
  RA8_CHECK_NULL_PTR(payload, s_tag, "test_inject_rx: payload nullptr");
  if (s_eth_state.opened == 0U) {
    return k_ra8_err_not_initialized;
  }
  const uint32_t               slot     = internal_test_rx_slot();
  ra8_gwca_basic_descriptor_t* desc     = &s_rx_chain[slot];
  uint32_t                     copy_len = len;
  if (copy_len > (uint32_t)k_ra8_eth_buf_size) {
    copy_len = (uint32_t)k_ra8_eth_buf_size;
  }
  /* The descriptor's PTR was set by ::ra8_eth_gwca_attach_buffers
   * during open. ::internal_eth_rx_pool returns the same low-address
   * (or BSS-on-chip) base; copy the payload to the matching slice so
   * default_recv pops the same bytes. */
  uint8_t* const pool_slot =
    internal_eth_rx_pool() + ((uintptr_t)slot * (uintptr_t)k_ra8_eth_buf_size);
  internal_byte_copy(pool_slot, payload, copy_len);
  desc->ds_l = (uint8_t)(copy_len & (uint32_t)k_ra8_eth_test_ds_low_mask);
  desc->ds_h = (uint8_t)((copy_len >> (uint32_t)k_ra8_eth_test_ds_high_shift) &
                         (uint32_t)k_ra8_eth_test_ds_high_mask);
  desc->dt   = (uint8_t)k_ra8_gwdcc_dt_fsingle;
  return k_ra8_ok;
}
#endif /* UNIT_TEST */
