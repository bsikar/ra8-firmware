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
 * The descriptor ring is a fixed-size static array allocated in BSS:
 * 8 TX descriptors + 8 RX descriptors, each backed by a 1536-byte
 * buffer, all 16-byte aligned per FSP convention. Hardware ownership
 * is encoded in the descriptor TACT (TX) / RACT (RX) bit -- on TX,
 * software writes the buffer then sets TACT=1; on RX, software polls
 * for RACT=0 and clears the descriptor before handing it back with
 * RACT=1.
 *
 * Every register access carries a HUM Ch 29 / Ch 34 citation.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra_eth.h"

#include <stdint.h>

#include "ra8d2_ether_regs.h"
#include "ra8d2_mstp_regs.h"
#include "ra8d2_rmac_regs.h"
#include "ra_check.h"
#include "ra_err.h"
#include "ra_log.h"
#include "ra_mstp.h"
#include "ra_rmac.h"

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
 * @enum ra_eth_internal_t
 * @brief Driver-private constants used by the NIC path.
 *
 * @details
 * Names every bit position / sentinel used by the descriptor ring
 * and the GWCA-side trigger so the source file is free of bare
 * literals (NASA Power-of-10 Rule 8 + project no-magic-numbers
 * policy).
 */
typedef enum : uint32_t {
  k_ra_eth_desc_tact      = 0x80000000UL, /**< TX descriptor active bit.  */
  k_ra_eth_desc_ract      = 0x80000000UL, /**< RX descriptor active bit.  */
  k_ra_eth_desc_tdle      = 0x40000000UL, /**< TX descriptor end-of-list. */
  k_ra_eth_desc_rdle      = 0x40000000UL, /**< RX descriptor end-of-list. */
  k_ra_eth_desc_tfp_ofs   = 0x30000000UL, /**< TX TFP1|TFP0 single-frame. */
  k_ra_eth_gwca_edtr      = 0x00000001UL, /**< GWCA TX engine enable.     */
  k_ra_eth_gwca_edrr      = 0x00000002UL, /**< GWCA RX engine enable.     */
  k_ra_eth_size_field_max = 0x0000FFFFUL, /**< 16-bit length field mask.  */
} ra_eth_internal_t;

/**
 * @enum ra_eth_phy_t
 * @brief PHY-side MIIM constants used by ::ra_eth_link_status.
 */
typedef enum : uint16_t {
  k_ra_eth_phy_addr_default  = 0U,      /**< EK-RA8D2 PHY MDC address.   */
  k_ra_eth_phy_reg_bmcr      = 0U,      /**< BMCR (basic mode control).  */
  k_ra_eth_phy_reg_bmsr      = 1U,      /**< BMSR (basic mode status).   */
  k_ra_eth_phy_bmsr_link_up  = 0x0004U, /**< BMSR.LINK_STATUS bit 2.     */
  k_ra_eth_phy_bmcr_speed100 = 0x2000U, /**< BMCR.SPEED_SELECT bit 13.   */
  k_ra_eth_phy_bmcr_duplex   = 0x0100U, /**< BMCR.DUPLEX_MODE bit 8.     */
  k_ra_eth_phy_speed_10      = 10U,     /**< 10 Mbps.                    */
  k_ra_eth_phy_speed_100     = 100U,    /**< 100 Mbps.                   */
} ra_eth_phy_t;

/**
 * @struct ra_eth_descriptor_t
 * @brief Software view of the FSP-style EDMAC TX/RX descriptor.
 *
 * @details
 * The descriptor matches the FSP ``ether_instance_descriptor_t``
 * layout: a 32-bit status word (TACT/RACT + flags), a 16-bit
 * ``size`` field (frame length on TX; RX-side always-empty), a
 * 16-bit ``buffer_size`` field, a buffer pointer, and a next-pointer.
 * Total size 16 bytes, naturally 16-byte aligned. The driver lays
 * the descriptors and buffers contiguously in BSS.
 */
typedef struct ra_eth_descriptor {
  volatile uint32_t         status;      /**< TACT/RACT + flags.    */
  volatile uint16_t         size;        /**< Frame size (TX use).  */
  volatile uint16_t         buffer_size; /**< Buffer size in bytes. */
  uint8_t*                  p_buffer;    /**< Backing buffer ptr.   */
  struct ra_eth_descriptor* p_next;      /**< Linked-ring next ptr. */
} ra_eth_descriptor_t;

/**
 * @struct ra_eth_state_t
 * @brief Driver-private NIC state.
 *
 * @details
 * Holds the cached configuration, the static descriptor rings, the
 * software ring pointers (write pointer for TX, read pointer for
 * RX), the cumulative software counters, and an "open" flag.
 */
typedef struct {
  uint8_t        opened;       /**< 1 once ::ra_eth_open succeeds.  */
  ra_eth_cfg_t   cfg;          /**< Captured configuration.         */
  uint16_t       tx_count;     /**< Active TX descriptors.          */
  uint16_t       rx_count;     /**< Active RX descriptors.          */
  uint16_t       buf_size;     /**< Per-descriptor buffer size.     */
  uint16_t       tx_write_idx; /**< Software TX write pointer.      */
  uint16_t       rx_read_idx;  /**< Software RX read pointer.       */
  ra_eth_stats_t stats;        /**< Cumulative counters.            */
} ra_eth_state_t;

/**
 * @var s_tx_descriptors
 * @brief Static TX descriptor ring (16-byte aligned, BSS-resident).
 */
__attribute__((aligned(16))) static ra_eth_descriptor_t s_tx_descriptors[k_ra_eth_num_tx_desc];

/**
 * @var s_rx_descriptors
 * @brief Static RX descriptor ring (16-byte aligned, BSS-resident).
 */
__attribute__((aligned(16))) static ra_eth_descriptor_t s_rx_descriptors[k_ra_eth_num_rx_desc];

/**
 * @var s_tx_buffers
 * @brief Static TX buffer pool, one buffer per descriptor.
 */
__attribute__((aligned(16))) static uint8_t s_tx_buffers[k_ra_eth_num_tx_desc][k_ra_eth_buf_size];

/**
 * @var s_rx_buffers
 * @brief Static RX buffer pool, one buffer per descriptor.
 */
__attribute__((aligned(16))) static uint8_t s_rx_buffers[k_ra_eth_num_rx_desc][k_ra_eth_buf_size];

/**
 * @var s_state
 * @brief Singleton NIC runtime state.
 */
static ra_eth_state_t s_state;

/**
 * @brief Saturating-add helper for the 32-bit software counters.
 *
 * @param[in,out] counter Pointer to the counter to bump.
 *
 * @pre counter is non-null.
 * @post *counter increases by one unless it was already UINT32_MAX.
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
 */
static inline ra_rmac_port_t internal_channel_to_port(uint8_t channel)
{
  if (channel == 0U) {
    return k_ra_rmac_port_0;
  }
  return k_ra_rmac_port_1;
}

/**
 * @brief Reset both descriptor rings to a known initial state.
 *
 * @details
 * - Every TX descriptor is cleared (TACT=0, software-owned, ready
 *   for the first ::ra_eth_write call) and the last descriptor
 *   sets TDLE so the EDMAC engine wraps to entry 0.
 * - Every RX descriptor is set hardware-owned (RACT=1) so the
 *   EDMAC engine can fill it as frames arrive; the last descriptor
 *   sets RDLE.
 *
 * @param[in] tx_count Active TX descriptor count.
 * @param[in] rx_count Active RX descriptor count.
 * @param[in] buf_size Bytes available in each backing buffer.
 *
 * @pre tx_count <= k_ra_eth_num_tx_desc.
 * @pre rx_count <= k_ra_eth_num_rx_desc.
 * @post Every TX descriptor has TACT=0 and a valid p_buffer.
 * @post Every RX descriptor has RACT=1 and a valid p_buffer.
 */
static void internal_init_rings(uint16_t tx_count, uint16_t rx_count, uint16_t buf_size)
{
  for (uint16_t i = 0U; i < tx_count; ++i) {
    s_tx_descriptors[i].status      = 0U;
    s_tx_descriptors[i].size        = 0U;
    s_tx_descriptors[i].buffer_size = buf_size;
    s_tx_descriptors[i].p_buffer    = s_tx_buffers[i];
    s_tx_descriptors[i].p_next      = &s_tx_descriptors[(i + 1U) % tx_count];
  }
  /* Mark the last descriptor as the wrap point so EDMAC restarts at 0. */
  if (tx_count > 0U) {
    s_tx_descriptors[tx_count - 1U].status |= k_ra_eth_desc_tdle;
  }

  for (uint16_t i = 0U; i < rx_count; ++i) {
    /* HUM Ch 34 -- RX descriptor handed to hardware via RACT=1. */
    s_rx_descriptors[i].status      = k_ra_eth_desc_ract;
    s_rx_descriptors[i].size        = 0U;
    s_rx_descriptors[i].buffer_size = buf_size;
    s_rx_descriptors[i].p_buffer    = s_rx_buffers[i];
    s_rx_descriptors[i].p_next      = &s_rx_descriptors[(i + 1U) % rx_count];
  }
  if (rx_count > 0U) {
    s_rx_descriptors[rx_count - 1U].status |= k_ra_eth_desc_rdle;
  }
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
  if ((tx == 0U) || (tx > k_ra_eth_num_tx_desc)) {
    return k_ra_err_invalid_arg;
  }
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
 */
static ra_err_t internal_bring_up_rmac(const ra_eth_cfg_t* cfg)
{
  const ra_rmac_port_t   rmac_port = internal_channel_to_port(cfg->channel);
  const ra_rmac_config_t rmac_cfg  = {.rx_filter       = k_ra_rmac_mrafc_promiscuous,
                                      .err_irq_enable  = 0U,
                                      .mon0_irq_enable = 0U,
                                      .mon1_irq_enable = 0U,
                                      .mon2_irq_enable = 0U,
                                      .phy_interface   = k_ra_rmac_pis_mii,
                                      .link_speed      = k_ra_rmac_lsc_100mbit,
                                      .duplex          = k_ra_rmac_duplex_full};
  const ra_err_t         rmac_err  = ra_rmac_init(rmac_port, &rmac_cfg);
  RA_RETURN_ON_ERROR(rmac_err, s_tag, "open: rmac init"); /* GCOVR_EXCL_BR_LINE */

  uint8_t mac_copy[k_ra_eth_mac_len];
  for (uint8_t i = 0U; i < (uint8_t)k_ra_eth_mac_len; ++i) {
    mac_copy[i] = cfg->mac_address[i];
  }
  return ra_rmac_set_mac_address(rmac_port, mac_copy);
}

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

ra_err_t ra_eth_get_status(uint32_t* out_mask)
{
  RA_CHECK_NULL_PTR(out_mask, s_tag, "out_mask must not be nullptr");
  /* HUM Ch 29 "Layer 3 Ethernet Switch Module (ESWM)" p 1287 */
  *out_mask = ra_eswm()->ESWM_STS;
  return k_ra_ok;
}

ra_err_t ra_eth_clear_status(uint32_t mask)
{
  volatile r_eswm_regs_t* reg = ra_eswm();
  /* HUM Ch 29 "Layer 3 Ethernet Switch Module (ESWM)" p 1287 */
  reg->ESWM_ICLR = mask;
  reg->ESWM_STS  = reg->ESWM_STS & ~mask;
  return k_ra_ok;
}

ra_err_t ra_eth_attach_handler(ra_eth_event_fn_t fn, void* ctx)
{
  s_eth_fn  = fn;
  s_eth_ctx = ctx;
  return k_ra_ok;
}

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

ra_err_t ra_eth_enter_stop(void)
{
  /* HUM Ch 29 "Layer 3 Ethernet Switch Module (ESWM)" p 1287 */
  ra_eswm()->ESWM_CTRL = 0U;
  return ra_mstp_disable(k_ra_mstp_eswm);
}

ra_err_t ra_eth_exit_stop(void)
{
  return ra_mstp_enable(k_ra_mstp_eswm);
}

/* -----------------------------------------------------------------------
 * Frame-level NIC API.
 * -------------------------------------------------------------------- */

/**
 * @brief Capture cfg + ring sizes into ::s_state and reset the counters.
 *
 * @param[in] cfg      Validated configuration.
 * @param[in] tx_count Resolved TX descriptor count.
 * @param[in] rx_count Resolved RX descriptor count.
 * @param[in] buf_size Resolved per-descriptor buffer size.
 *
 * @pre cfg is non-null.
 * @post ::s_state holds the new cfg and zeroed counters.
 */
static void internal_capture_state(const ra_eth_cfg_t* cfg,
                                   uint16_t            tx_count,
                                   uint16_t            rx_count,
                                   uint16_t            buf_size)
{
  s_state.opened       = 1U;
  s_state.cfg          = *cfg;
  s_state.tx_count     = tx_count;
  s_state.rx_count     = rx_count;
  s_state.buf_size     = buf_size;
  s_state.tx_write_idx = 0U;
  s_state.rx_read_idx  = 0U;
  s_state.stats.tx_ok  = 0U;
  s_state.stats.rx_ok  = 0U;
  s_state.stats.tx_err = 0U;
  s_state.stats.rx_err = 0U;
}

/**
 * @brief Validate cfg, resolve ring sizes, and bring up MSTP + RMAC.
 *
 * @details
 * Rolled out of ::ra_eth_open so that single function stays under
 * the project's clang-tidy function-size threshold. Performs:
 * 1. cfg null-check + channel range check.
 * 2. Ring size resolution (defaults if 0).
 * 3. MSTP gate enable.
 * 4. Per-channel RMAC bring-up + MAC address program.
 *
 * @param[in]  cfg      User configuration (already null-checked by caller).
 * @param[out] tx_count Resolved TX descriptor count.
 * @param[out] rx_count Resolved RX descriptor count.
 * @param[out] buf_size Resolved per-descriptor buffer size.
 *
 * @return ::ra_err_t Error code.
 */
static ra_err_t internal_open_prep(const ra_eth_cfg_t* cfg,
                                   uint16_t*           tx_count,
                                   uint16_t*           rx_count,
                                   uint16_t*           buf_size)
{
  if (cfg->channel > 1U) {
    ra_log_error(s_tag, "open: channel out of range");
    return k_ra_err_invalid_arg;
  }
  const ra_err_t resolve_rc = internal_resolve_sizes(cfg, tx_count, rx_count, buf_size);
  if (resolve_rc != k_ra_ok) {
    return resolve_rc;
  }
  const ra_err_t mst_err = ra_mstp_enable(k_ra_mstp_eswm);
  if (mst_err != k_ra_ok) {
    return mst_err;
  }
  return internal_bring_up_rmac(cfg);
}

ra_err_t ra_eth_open(const ra_eth_cfg_t* cfg)
{
  RA_CHECK_NULL_PTR(cfg, s_tag, "open: cfg must not be nullptr");

  uint16_t       tx_count = 0U;
  uint16_t       rx_count = 0U;
  uint16_t       buf_size = 0U;
  const ra_err_t prep_rc  = internal_open_prep(cfg, &tx_count, &rx_count, &buf_size);
  if (prep_rc != k_ra_ok) {
    return prep_rc;
  }

  internal_init_rings(tx_count, rx_count, buf_size);

  /* GWCA enable: set the EDTR / EDRR bits so the engine walks the rings.
   * HUM Ch 34 "Ethernet CPU Agent (GWCA)" p 1787 */
  volatile r_gwca_regs_t* gwca = ra_gwca();
  gwca->GWCA_CTRL              = k_ra_eth_gwca_edtr | k_ra_eth_gwca_edrr;

  internal_capture_state(cfg, tx_count, rx_count, buf_size);
  ra_log_info(s_tag, "eth_open: rings ready");
  return k_ra_ok;
}

ra_err_t ra_eth_close(void)
{
  if (s_state.opened == 0U) {
    return k_ra_err_not_initialized;
  }

  /* Halt the engine: clear EDTR / EDRR. HUM Ch 34 p 1787 */
  volatile r_gwca_regs_t* gwca = ra_gwca();
  gwca->GWCA_CTRL              = 0U;

  /* Tear down the local RMAC port (MSTP-gate ref-counted). */
  const ra_rmac_port_t rmac_port = internal_channel_to_port(s_state.cfg.channel);
  const ra_err_t       rmac_err  = ra_rmac_deinit(rmac_port);
  /* Ignore rmac_err on close to keep teardown best-effort. */
  (void)rmac_err;

  s_state.opened       = 0U;
  s_state.tx_write_idx = 0U;
  s_state.rx_read_idx  = 0U;
  return ra_mstp_disable(k_ra_mstp_eswm);
}

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

  ra_eth_descriptor_t* const desc = &s_tx_descriptors[s_state.tx_write_idx];

  /* If the descriptor at the write pointer is still hardware-owned
   * the engine has not finished sending the previous frame yet -- the
   * caller should retry later. */
  if ((desc->status & k_ra_eth_desc_tact) != 0U) {
    internal_stat_inc(&s_state.stats.tx_err);
    return k_ra_err_busy;
  }
  if (desc->p_buffer == nullptr) {
    internal_stat_inc(&s_state.stats.tx_err);
    return k_ra_err_invalid_arg;
  }

  uint32_t copy_len = len;
  if (copy_len > (uint32_t)s_state.buf_size) {
    copy_len = s_state.buf_size;
  }
  internal_byte_copy(desc->p_buffer, buf, copy_len);
  desc->size = (uint16_t)(copy_len & k_ra_eth_size_field_max);

  /* Hand the descriptor to hardware (TFP1|TFP0 = single-frame TX, TACT=1). */
  uint32_t status = desc->status & k_ra_eth_desc_tdle;
  status |= k_ra_eth_desc_tfp_ofs;
  status |= k_ra_eth_desc_tact;
  desc->status = status;

  /* Re-assert EDTR in case the engine completed the previous frame
   * and parked itself. HUM Ch 34 "Ethernet CPU Agent (GWCA)" p 1787 */
  volatile r_gwca_regs_t* gwca = ra_gwca();
  gwca->GWCA_CTRL              = gwca->GWCA_CTRL | k_ra_eth_gwca_edtr;

  /* Advance the software write pointer. */
  s_state.tx_write_idx = (uint16_t)((s_state.tx_write_idx + 1U) % s_state.tx_count);

  internal_stat_inc(&s_state.stats.tx_ok);
  return k_ra_ok;
}

/**
 * @brief Hand an RX descriptor back to hardware and re-arm EDRR.
 *
 * @param[in,out] desc Descriptor that was just drained.
 *
 * @pre desc is non-null.
 * @post desc->status has RACT=1 again, RDLE preserved.
 * @post GWCA_CTRL has EDRR set so the engine continues walking the ring.
 */
static void internal_release_rx_descriptor(ra_eth_descriptor_t* desc)
{
  const uint32_t rdle_bit = desc->status & k_ra_eth_desc_rdle;
  desc->size              = 0U;
  desc->status            = rdle_bit | k_ra_eth_desc_ract;

  /* HUM Ch 34 "Ethernet CPU Agent (GWCA)" p 1787 */
  volatile r_gwca_regs_t* gwca = ra_gwca();
  gwca->GWCA_CTRL              = gwca->GWCA_CTRL | k_ra_eth_gwca_edrr;
}

/**
 * @brief Compute the clamped copy length for an RX pop.
 *
 * @param[in] desc_size Descriptor's stored frame length.
 * @param[in] max_len   Caller buffer capacity.
 * @return min(desc_size, max_len, buffer_size).
 */
static inline uint32_t internal_clamp_rx_len(uint32_t desc_size, uint32_t max_len)
{
  uint32_t copy_len = desc_size;
  if (copy_len > max_len) {
    copy_len = max_len;
  }
  if (copy_len > (uint32_t)s_state.buf_size) {
    copy_len = s_state.buf_size;
  }
  return copy_len;
}

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

  ra_eth_descriptor_t* const desc = &s_rx_descriptors[s_state.rx_read_idx];

  if ((desc->status & k_ra_eth_desc_ract) != 0U) {
    internal_stat_inc(&s_state.stats.rx_err);
    *got_len = 0U;
    return k_ra_err_no_data;
  }
  if (desc->p_buffer == nullptr) {
    internal_stat_inc(&s_state.stats.rx_err);
    *got_len = 0U;
    return k_ra_err_invalid_arg;
  }

  const uint32_t copy_len = internal_clamp_rx_len((uint32_t)desc->size, max_len);
  internal_byte_copy(buf, desc->p_buffer, copy_len);
  *got_len = copy_len;

  internal_release_rx_descriptor(desc);
  s_state.rx_read_idx = (uint16_t)((s_state.rx_read_idx + 1U) % s_state.rx_count);
  internal_stat_inc(&s_state.stats.rx_ok);
  return k_ra_ok;
}

ra_err_t ra_eth_link_status(ra_eth_link_t* out_status)
{
  RA_CHECK_NULL_PTR(out_status, s_tag, "link_status: out must not be nullptr");
  if (s_state.opened == 0U) {
    return k_ra_err_not_initialized;
  }

  const ra_rmac_port_t port = internal_channel_to_port(s_state.cfg.channel);
  uint16_t             bmsr = 0U;
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
  return k_ra_ok;
}

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
 * @brief Test-only hook: simulate the EDMAC engine releasing an RX frame.
 *
 * @details
 * Copies ``len`` bytes from ``payload`` into the RX descriptor buffer at
 * the head of the ring (``s_rx_descriptors[s_state.rx_read_idx]``), then
 * clears RACT so the next ::ra_eth_read sees a software-owned descriptor
 * and pops it. The host-side simulator has no real EDMAC engine so unit
 * tests need a way to inject frames. This function is gated by
 * ``UNIT_TEST`` so it never reaches the firmware target.
 *
 * @param[in] payload Bytes to drop into the RX buffer.
 * @param[in] len     Frame length (clamped to buffer_size).
 *
 * @return ::ra_err_t Error code.
 * @retval k_ra_ok                 Frame staged.
 * @retval k_ra_err_not_initialized ::ra_eth_open was not called first.
 * @retval k_ra_err_null_ptr        payload is nullptr.
 *
 * @pre Driver previously brought up via ::ra_eth_open.
 * @pre payload points to len readable bytes.
 * @post Descriptor at the read pointer has RACT=0.
 * @post Descriptor's size field equals min(len, buffer_size).
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
  ra_eth_descriptor_t* const desc     = &s_rx_descriptors[s_state.rx_read_idx];
  uint32_t                   copy_len = len;
  if (copy_len > (uint32_t)s_state.buf_size) {
    copy_len = s_state.buf_size;
  }
  internal_byte_copy(desc->p_buffer, payload, copy_len);
  desc->size = (uint16_t)(copy_len & k_ra_eth_size_field_max);
  /* Clear RACT -- pretend the EDMAC engine just landed a frame. */
  desc->status = desc->status & ~k_ra_eth_desc_ract;
  return k_ra_ok;
}
#endif /* UNIT_TEST */
