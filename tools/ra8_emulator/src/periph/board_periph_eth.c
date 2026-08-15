/**
 * @file board_periph_eth.c
 * @brief R-Switch (ESWM / MFWD / COMA / ETHA / RMAC / GWCA) model for ra8_emulator
 *
 * @details
 * Models the RA8D2 Layer-3 Ethernet switch register cluster so a networking
 * app (@c threadx_netx_tcp_echo and the @c eth_* demos) runs the GENUINE
 * ra8_eth / ra8_etha / ra8_rmac / ra8_eth_gwca drivers headless, instead of the
 * function-level seam hooks that used to intercept @c ra8_eth_write /
 * @c ra8_eth_read / @c ra8_eth_link_status and bypass the register path
 * entirely. Retires those seams (see #219 / #218).
 *
 * The block owns the contiguous cluster window @c [0x403C0000, 0x403CF000):
 * MFWD (@c 0x403C0000), ESWM (@c 0x403C8000), COMA (@c 0x403C9000),
 * ETHA0/1 (@c 0x403CA000 / @c 0x403CC000), RMAC0/1 (@c 0x403CB000 /
 * @c 0x403CD000) and GWCA (@c 0x403CE000). Config registers read back what
 * was written (a flat byte shadow); the load-bearing status / action
 * registers are modelled faithfully:
 *
 *  - **ETHA EAMS.OPS mirrors EAMC.OPC** (HUM Ch 32.3.1.2 p 1631): the mode
 *    state machine converges to the commanded mode, so the driver's
 *    CONFIG / DISABLE equality-poll completes instead of spinning ~1e8 MMIO
 *    callbacks against the sparse fallback.
 *  - **GWCA GWMS.OPS mirrors GWMC.OPC** (HUM Ch 34.3.2 p 1798) and
 *    **GWARIRM.ARR** asserts after ARIOG, and **GWDCC[i].BALR** self-clears.
 *  - **RMAC MPSM** MDIO: a Clause-22 read returns the modelled PHY register
 *    (BMSR link-up + auto-neg-complete, ANLPAR = 100BASE-TX FD), a write
 *    self-clears BMCR.RESET; the whole PHY chip-init + link bring-up runs.
 *  - **GWCA descriptor DMA**: on the GWTRC TX kick the model walks the queue's
 *    LINKFIX -> chain-head descriptor in emulated SRAM, moves the frame bytes
 *    out to the ::board_net virtual peer and writes the descriptor back to
 *    FEMPTY (so the driver's TX-complete wait resolves); once per tick it
 *    pulls the peer's reply frames into free RX descriptors so the driver's
 *    poll delivers them. MTGFCE / MRFC / MRGFCE count the moved frames.
 *
 * Honest-model note: an unmodelled descriptor type or an un-configured queue
 * is a no-op (the driver's own timeout fires), never a faked success; the
 * link is reported up at 100 Mb/s full-duplex because ::board_net is a real
 * peer on the wire, not because the value is invented.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>
#include <stdio.h>

#include "board_console.h"
#include "board_net.h"
#include "board_periph_block.h"
#include "emu_host_io_internal.h"
#include "ra8_etha_regs.h"
#include "ra8_ether_regs.h"
#include "ra8_rmac_regs.h"

/**
 * @enum eth_win_geom_t
 * @brief Absolute window the block owns (the contiguous R-Switch cluster).
 *
 * @details Spans MFWD (the lowest base) through the end of the GWCA register
 * bank. The ESWM media-mux sub-block (MIIRR / MIICR at @c 0x403E1400) sits
 * outside this window and falls to the sparse fallback -- it is config-reflect
 * only, so that is faithful. GPTP (@c 0x403E0000) sits outside this window too
 * but is modelled by its own block (board_periph_gptp.c), whose free-running
 * counter really advances.
 */
typedef enum : uint64_t {
  k_eth_win_base = (uint64_t)k_ra8_mfwd_base_addr, /**< MFWD base = window base. */
  k_eth_win_span = 0xF000UL,                       /**< MFWD..GWCA end.          */
} eth_win_geom_t;

/**
 * @enum eth_model_const_t
 * @brief Descriptor-byte layout, MDIO decode, and ring-walk bounds.
 *
 * @details The GWCA descriptor is little-endian: byte 0 = DS[7:0], byte 1 low
 * nibble = DS[11:8], byte 2 bits [7:4] = DT, bytes 4..7 = PTR[31:0] (HUM
 * Ch 34.5.1.2 "General Formats"). MRFC has no named offset in the register
 * header, so it is declared here with its HUM-cited value.
 */
typedef enum : uint32_t {
  k_eth_desc_bytes    = 8U,          /**< Basic-descriptor / LINKFIX entry size.    */
  k_eth_desc_dt_byte  = 2U,          /**< DT lives in descriptor byte 2.            */
  k_eth_desc_dt_shift = 4U,          /**< DT occupies bits [7:4] of that byte.      */
  k_eth_desc_dt_lo    = 0x0FU,       /**< Low-nibble mask (bits kept on a DT set).  */
  k_eth_desc_ds_byte1 = 1U,          /**< DS[11:8] in low nibble of byte 1.         */
  k_eth_desc_ds_hi    = 0x0FU,       /**< DS high-nibble mask.                      */
  k_eth_desc_ds_keep  = 0xF0U,       /**< Byte-1 high-nibble kept on a DS set.      */
  k_eth_desc_ptr_off  = 4U,          /**< PTR[31:0] at descriptor byte 4.           */
  k_eth_reg32_bytes   = 4U,          /**< One 32-bit register.                      */
  k_eth_byte_mask     = 0xFFU,       /**< Low 8 bits of a value.                    */
  k_eth_shift_byte    = 8U,          /**< One-byte shift.                           */
  k_eth_gwarirm_ariog = 1UL << 0U,   /**< GWARIRM.ARIOG AXI-init request.           */
  k_eth_gwarirm_arr   = 1UL << 1U,   /**< GWARIRM.ARR   AXI-init response.          */
  k_eth_mpsm_mff      = 1UL << 2U,   /**< MPSM.MFF: 1 = Clause-45 frame format.     */
  k_eth_rmac_off_mrfc = 0x0438U,     /**< RMAC MRFC (RX total frame count).         */
  k_eth_frame_max     = 1536U,       /**< Marshal buffer cap (>= max 802.3 frame).  */
  k_eth_ring_walk_max = 64U,         /**< Bound on a descriptor-ring scan.          */
  k_eth_rx_inject_max = 8U,          /**< Max frames injected into the ring / tick. */
  k_eth_console_cap   = 48U,         /**< Console NET-tab line capacity.            */
  k_eth_queue_none    = 0xFFFFFFFFU, /**< "No RX queue configured yet" sentinel.    */
} eth_model_const_t;

/**
 * @enum eth_phy_reg_t
 * @brief Modelled Clause-22 PHY register indices, seed values, and mode bits.
 *
 * @details Standard IEEE 802.3 MII register numbers. The seeds describe a PHY
 * that has auto-negotiated a 100BASE-TX full-duplex link (consistent with the
 * ::board_net peer): BMSR reports LINK + AN-complete, ANLPAR advertises
 * 100BASE-TX FD as the top ability. BMCR.RESET / .AN_RESTART are self-clearing.
 */
typedef enum : uint16_t {
  k_eth_phy_reg_bmcr    = 0U,      /**< BMCR   (basic control).             */
  k_eth_phy_reg_bmsr    = 1U,      /**< BMSR   (basic status).              */
  k_eth_phy_reg_anlpar  = 5U,      /**< ANLPAR (link-partner ability).      */
  k_eth_phy_reg_count   = 32U,     /**< Clause-22 register-space size.      */
  k_eth_phy_bmcr_seed   = 0x3100U, /**< AN enable + 100 Mb/s + full duplex. */
  k_eth_phy_bmsr_seed   = 0x782DU, /**< 100/10 abilities + AN done + LINK.  */
  k_eth_phy_anlpar_seed = 0x01E1U, /**< 100F/100H/10F/10H + selector.       */
  k_eth_phy_bmcr_reset  = 0x8000U, /**< BMCR.RESET (self-clearing).         */
  k_eth_phy_bmcr_anrst  = 0x0200U, /**< BMCR.AN_RESTART (self-clearing).    */
} eth_phy_reg_t;

/** @brief Per-tick order slot: after the DTC/DMAC transfer engines. */
typedef enum : uint32_t {
  k_eth_block_order = 58U, /**< Report + tick order (just after DTC=55). */
} eth_order_t;

/**
 * @struct eth_state_t
 * @brief Runtime state of the R-Switch model.
 *
 * @details @c win is the flat register shadow for the whole window (config
 * registers read back what was written). @c linkfix_base captures GWDCBAC1 so
 * the DMA can find the descriptor rings; @c rx_queue is the queue index the
 * driver configured for reception (GWDCC.DQT == 0). @c phy holds the modelled
 * Clause-22 PHY register file. The counters mirror MTGFCE / MRFC / MRGFCE.
 */
typedef struct {
  uint8_t  win[k_eth_win_span];      /**< Flat register shadow (config reflect).  */
  uint32_t linkfix_base;             /**< GWDCBAC descriptor-chain base address.  */
  uint32_t rx_queue;                 /**< Configured RX queue index (or NONE).    */
  uint16_t phy[k_eth_phy_reg_count]; /**< Modelled PHY Clause-22 register file.   */
  uint32_t tx_frames;                /**< Frames moved firmware -> peer (MTGFCE). */
  uint32_t rx_frames;                /**< Frames moved peer -> firmware (MRFC).   */
} eth_state_t;

/** @brief Singleton R-Switch model state. */
static eth_state_t s_eth;

/* ------------------------------------------------------------------------- */
/* Flat-shadow + descriptor byte helpers. */
/* ------------------------------------------------------------------------- */

/**
 * @brief Read up to @p size bytes little-endian from the register shadow.
 *
 * @param[in] off  Byte offset inside the window.
 * @param[in] size Access width (1 / 2 / 4).
 * @return The assembled value.
 * @retval value Little-endian shadow bytes.
 * @pre @p off is inside @c [0, k_eth_win_span).
 * @pre @p size is 1, 2, or 4.
 * @post No state is modified.
 * @post Bytes past the window read as zero.
 * @note Not thread-safe.
 * @since 0.1.0
  * @details Read up to @p size bytes little-endian from the register shadow; this step is contained within the board periph Ethernet model and uses bounded caller or module-owned storage.
 */
RA8_INTERNAL static uint64_t internal_eth_shadow_read(uint64_t off, unsigned size)
{
  uint64_t v = 0U;
  for (unsigned i = 0U; (i < size) && ((off + (uint64_t)i) < (uint64_t)k_eth_win_span); ++i) {
    v |= (uint64_t)s_eth.win[off + (uint64_t)i] << (k_eth_shift_byte * i);
  }
  return v;
}

/**
 * @brief Write up to @p size bytes little-endian into the register shadow.
 *
 * @param[in] off   Byte offset inside the window.
 * @param[in] size  Access width (1 / 2 / 4).
 * @param[in] value Value to store.
 * @return Nothing.
 * @pre @p off is inside @c [0, k_eth_win_span).
 * @pre @p size is 1, 2, or 4.
 * @post The shadow bytes reflect @p value.
 * @post Bytes past the window are dropped.
 * @note Not thread-safe.
 * @since 0.1.0
  * @details Write up to @p size bytes little-endian into the register shadow; this step is contained within the board periph Ethernet model and uses bounded caller or module-owned storage.
 */
RA8_INTERNAL static void internal_eth_shadow_write(uint64_t off, unsigned size, uint64_t value)
{
  for (unsigned i = 0U; (i < size) && ((off + (uint64_t)i) < (uint64_t)k_eth_win_span); ++i) {
    s_eth.win[off + (uint64_t)i] = (uint8_t)(value >> (k_eth_shift_byte * i));
  }
}

/**
 * @brief Read a 32-bit shadow register at window offset @p off.
 * @details Read a 32-bit shadow register at window offset @p off; this step is contained within the board periph Ethernet model and uses bounded caller or module-owned storage.
 * @param[in] off Register or byte offset addressed by the operation.
 * @return The Ethernet shadow u32 result produced by the board periph Ethernet model.
 * @retval value The operation-specific Ethernet shadow u32 value.
 * @pre Arguments satisfy the ranges documented for Ethernet shadow u32. @pre The call executes on the emulator's single owning thread.
 * @post State changes remain confined to the board periph Ethernet model and documented output objects. @post Ownership of caller-supplied storage is unchanged.
 * @note The operation is synchronous and does not transfer heap ownership.
 * @since 0.1.0
 */
RA8_INTERNAL static uint32_t internal_eth_shadow_u32(uint64_t off)
{
  return (uint32_t)internal_eth_shadow_read(off, k_eth_reg32_bytes);
}

/**
 * @brief Assemble a little-endian 32-bit value from four bytes @p p[0..3].
 * @details Assemble a little-endian 32-bit value from four bytes @p p[0..3]; this step is contained within the board periph Ethernet model and uses bounded caller or module-owned storage.
 * @param[in] p Module-owned state object processed by the operation.
 * @return The Ethernet bytes u32 result produced by the board periph Ethernet model.
 * @retval value The operation-specific Ethernet bytes u32 value.
 * @pre Arguments satisfy the ranges documented for Ethernet bytes u32. @pre The call executes on the emulator's single owning thread.
 * @post State changes remain confined to the board periph Ethernet model and documented output objects. @post Ownership of caller-supplied storage is unchanged.
 * @note The operation is synchronous and does not transfer heap ownership.
 * @since 0.1.0
 */
RA8_INTERNAL static uint32_t internal_eth_bytes_u32(const uint8_t* p)
{
  uint32_t v = 0U;
  for (unsigned i = 0U; i < k_eth_reg32_bytes; ++i) {
    v |= (uint32_t)p[i] << (k_eth_shift_byte * i);
  }
  return v;
}

/**
 * @brief Read an 8-byte descriptor header from emulated memory @p addr.
 * @details Read an 8-byte descriptor header from emulated memory @p addr; this step is contained within the board periph Ethernet model and uses bounded caller or module-owned storage.
 * @param[in,out] uc Unicorn engine whose emulated state is read or updated.
 * @param[in] addr Guest address involved in the operation.
 * @param[in,out] out8 Out8 state or storage updated in place by the operation.
 * @pre Arguments satisfy the ranges documented for Ethernet desc read. @pre The call executes on the emulator's single owning thread.
 * @post State changes remain confined to the board periph Ethernet model and documented output objects. @post Ownership of caller-supplied storage is unchanged.
 * @note The operation is synchronous and does not transfer heap ownership.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_eth_desc_read(uc_engine* uc, uint32_t addr, uint8_t* out8)
{
  (void)emu_mem_read(uc, (uint64_t)addr, out8, (size_t)k_eth_desc_bytes);
}

/**
 * @brief Decode the DT (descriptor type) field of an 8-byte header.
 * @details Decode the dt (descriptor type) field of an 8-byte header; this step is contained within the board periph Ethernet model and uses bounded caller or module-owned storage.
 * @param[in] d8 D8 input used by the operation.
 * @return The Ethernet desc dt result produced by the board periph Ethernet model.
 * @retval value The operation-specific Ethernet desc dt value.
 * @pre Arguments satisfy the ranges documented for Ethernet desc dt. @pre The call executes on the emulator's single owning thread.
 * @post State changes remain confined to the board periph Ethernet model and documented output objects. @post Ownership of caller-supplied storage is unchanged.
 * @note The operation is synchronous and does not transfer heap ownership.
 * @since 0.1.0
 */
RA8_INTERNAL static uint32_t internal_eth_desc_dt(const uint8_t* d8)
{
  return ((uint32_t)d8[k_eth_desc_dt_byte] >> k_eth_desc_dt_shift) & (uint32_t)k_eth_desc_dt_lo;
}

/**
 * @brief Decode the 12-bit DS (descriptor size) field of an 8-byte header.
 * @details Decode the 12-bit ds (descriptor size) field of an 8-byte header; this step is contained within the board periph Ethernet model and uses bounded caller or module-owned storage.
 * @param[in] d8 D8 input used by the operation.
 * @return The Ethernet desc ds result produced by the board periph Ethernet model.
 * @retval value The operation-specific Ethernet desc ds value.
 * @pre Arguments satisfy the ranges documented for Ethernet desc ds. @pre The call executes on the emulator's single owning thread.
 * @post State changes remain confined to the board periph Ethernet model and documented output objects. @post Ownership of caller-supplied storage is unchanged.
 * @note The operation is synchronous and does not transfer heap ownership.
 * @since 0.1.0
 */
RA8_INTERNAL static uint32_t internal_eth_desc_ds(const uint8_t* d8)
{
  return (uint32_t)d8[0] |
         (((uint32_t)d8[k_eth_desc_ds_byte1] & (uint32_t)k_eth_desc_ds_hi) << k_eth_shift_byte);
}

/**
 * @brief Decode the 32-bit PTR (buffer / chain address) of an 8-byte header.
 * @details Decode the 32-bit ptr (buffer / chain address) of an 8-byte header; this step is contained within the board periph Ethernet model and uses bounded caller or module-owned storage.
 * @param[in] d8 D8 input used by the operation.
 * @return The Ethernet desc ptr result produced by the board periph Ethernet model.
 * @retval value The operation-specific Ethernet desc ptr value.
 * @pre Arguments satisfy the ranges documented for Ethernet desc ptr. @pre The call executes on the emulator's single owning thread.
 * @post State changes remain confined to the board periph Ethernet model and documented output objects. @post Ownership of caller-supplied storage is unchanged.
 * @note The operation is synchronous and does not transfer heap ownership.
 * @since 0.1.0
 */
RA8_INTERNAL static uint32_t internal_eth_desc_ptr(const uint8_t* d8)
{
  return internal_eth_bytes_u32(&d8[k_eth_desc_ptr_off]);
}

/**
 * @brief Write the DT field of the descriptor at emulated address @p addr.
 *
 * @param[in,out] uc   Active engine.
 * @param[in]     addr Descriptor base in emulated memory.
 * @param[in]     dt   New descriptor-type nibble.
 * @return Nothing.
 * @pre @p addr points at a live GWCA descriptor.
 * @pre @p dt is a valid ra8_gwdcc_dt_t nibble.
 * @post Byte 2 of the descriptor carries @p dt in bits [7:4].
 * @post Other descriptor bytes are untouched.
 * @note Not thread-safe.
 * @since 0.1.0
  * @details Write the dt field of the descriptor at emulated address @p addr; this step is contained within the board periph Ethernet model and uses bounded caller or module-owned storage.
 */
RA8_INTERNAL static void internal_eth_desc_set_dt(uc_engine* uc, uint32_t addr, uint32_t dt)
{
  uint8_t b = 0U;
  (void)emu_mem_read(uc, (uint64_t)addr + (uint64_t)k_eth_desc_dt_byte, &b, 1U);
  b = (uint8_t)((b & (uint8_t)k_eth_desc_dt_lo) |
                (uint8_t)((dt & (uint32_t)k_eth_desc_dt_lo) << k_eth_desc_dt_shift));
  (void)emu_mem_write(uc, (uint64_t)addr + (uint64_t)k_eth_desc_dt_byte, &b, 1U);
}

/**
 * @brief Write the 12-bit DS field of the descriptor at address @p addr.
 *
 * @param[in,out] uc   Active engine.
 * @param[in]     addr Descriptor base in emulated memory.
 * @param[in]     ds   Frame length to store (0..4095).
 * @return Nothing.
 * @pre @p addr points at a live GWCA descriptor.
 * @pre @p ds fits in 12 bits.
 * @post Bytes 0..1 of the descriptor carry @p ds (byte-1 high nibble kept).
 * @post Other descriptor bytes are untouched.
 * @note Not thread-safe.
 * @since 0.1.0
  * @details Write the 12-bit ds field of the descriptor at address @p addr; this step is contained within the board periph Ethernet model and uses bounded caller or module-owned storage.
 */
RA8_INTERNAL static void internal_eth_desc_set_ds(uc_engine* uc, uint32_t addr, uint32_t ds)
{
  const uint8_t lo = (uint8_t)(ds & (uint32_t)k_eth_byte_mask);
  uint8_t       hi = 0U;
  (void)emu_mem_read(uc, (uint64_t)addr + (uint64_t)k_eth_desc_ds_byte1, &hi, 1U);
  hi = (uint8_t)((hi & (uint8_t)k_eth_desc_ds_keep) |
                 (uint8_t)((ds >> k_eth_shift_byte) & (uint32_t)k_eth_desc_ds_hi));
  (void)emu_mem_write(uc, (uint64_t)addr, &lo, 1U);
  (void)emu_mem_write(uc, (uint64_t)addr + (uint64_t)k_eth_desc_ds_byte1, &hi, 1U);
}

/* ------------------------------------------------------------------------- */
/* Sub-block address predicates (bases + offsets from the register headers). */
/* ------------------------------------------------------------------------- */

/**
 * @brief True if @p addr is the given @p off in either ETHA port window.
 * @details True if @p addr is the given @p off in either etha port window; this step is contained within the board periph Ethernet model and uses bounded caller or module-owned storage.
 * @param[in] addr Guest address involved in the operation.
 * @param[in] off Register or byte offset addressed by the operation.
 * @return The Ethernet is etha reg result produced by the board periph Ethernet model.
 * @retval true The Ethernet is etha reg condition holds or completed successfully; false otherwise.
 * @pre Arguments satisfy the ranges documented for Ethernet is etha reg. @pre The call executes on the emulator's single owning thread.
 * @post State changes remain confined to the board periph Ethernet model and documented output objects. @post Ownership of caller-supplied storage is unchanged.
 * @note The operation is synchronous and does not transfer heap ownership.
 * @since 0.1.0
 */
RA8_INTERNAL static bool internal_eth_is_etha_reg(uint64_t addr, uint64_t off)
{
  return (addr == (uint64_t)k_ra8_etha0_base_addr + off) ||
         (addr == (uint64_t)k_ra8_etha1_base_addr + off);
}

/**
 * @brief True if @p addr is the given @p off in either RMAC port window.
 * @details True if @p addr is the given @p off in either rmac port window; this step is contained within the board periph Ethernet model and uses bounded caller or module-owned storage.
 * @param[in] addr Guest address involved in the operation.
 * @param[in] off Register or byte offset addressed by the operation.
 * @return The Ethernet is rmac reg result produced by the board periph Ethernet model.
 * @retval true The Ethernet is rmac reg condition holds or completed successfully; false otherwise.
 * @pre Arguments satisfy the ranges documented for Ethernet is rmac reg. @pre The call executes on the emulator's single owning thread.
 * @post State changes remain confined to the board periph Ethernet model and documented output objects. @post Ownership of caller-supplied storage is unchanged.
 * @note The operation is synchronous and does not transfer heap ownership.
 * @since 0.1.0
 */
RA8_INTERNAL static bool internal_eth_is_rmac_reg(uint64_t addr, uint64_t off)
{
  return (addr == (uint64_t)k_ra8_rmac0_base_addr + off) ||
         (addr == (uint64_t)k_ra8_rmac1_base_addr + off);
}

/**
 * @brief True if @p addr is one of GWCA GWDCC[0..31]; sets @p *queue.
 * @details True if @p addr is one of gwca gwdcc[0..31]; sets @p; this step is contained within the board periph Ethernet model and uses bounded caller or module-owned storage.
 * @param[in] addr Guest address involved in the operation.
 * @param[in,out] queue Queue state or storage updated in place by the operation.
 * @return The Ethernet is gwdcc result produced by the board periph Ethernet model.
 * @retval true The Ethernet is gwdcc condition holds or completed successfully; false otherwise.
 * @pre Arguments satisfy the ranges documented for Ethernet is gwdcc. @pre The call executes on the emulator's single owning thread.
 * @post State changes remain confined to the board periph Ethernet model and documented output objects. @post Ownership of caller-supplied storage is unchanged.
 * @note The operation is synchronous and does not transfer heap ownership.
 * @since 0.1.0
 */
RA8_INTERNAL static bool internal_eth_is_gwdcc(uint64_t addr, uint32_t* queue)
{
  const uint64_t base = (uint64_t)k_ra8_gwca0_base_addr + (uint64_t)k_ra8_gwca_off_gwdcc_base;
  const uint64_t top  = base + ((uint64_t)k_eth_phy_reg_count * (uint64_t)k_eth_reg32_bytes);
  if ((addr < base) || (addr >= top) || (((addr - base) % (uint64_t)k_eth_reg32_bytes) != 0U)) {
    return false;
  }
  *queue = (uint32_t)((addr - base) / (uint64_t)k_eth_reg32_bytes);
  return true;
}

/* ------------------------------------------------------------------------- */
/* PHY (MDIO) model. */
/* ------------------------------------------------------------------------- */

/**
 * @brief Read a modelled Clause-22 PHY register (out-of-range reads 0).
 * @details Read a modelled clause-22 phy register (out-of-range reads 0); this step is contained within the board periph Ethernet model and uses bounded caller or module-owned storage.
 * @param[in] reg Register index or value selected by the operation.
 * @return The Ethernet phy read result produced by the board periph Ethernet model.
 * @retval value The operation-specific Ethernet phy read value.
 * @pre Arguments satisfy the ranges documented for Ethernet phy read. @pre The call executes on the emulator's single owning thread.
 * @post State changes remain confined to the board periph Ethernet model and documented output objects. @post Ownership of caller-supplied storage is unchanged.
 * @note The operation is synchronous and does not transfer heap ownership.
 * @since 0.1.0
 */
RA8_INTERNAL static uint16_t internal_eth_phy_read(uint32_t reg)
{
  if (reg >= (uint32_t)k_eth_phy_reg_count) {
    return 0U;
  }
  return s_eth.phy[reg];
}

/**
 * @brief Write a modelled Clause-22 PHY register (self-clearing BMCR bits).
 *
 * @param[in] reg   PHY register index (0..31).
 * @param[in] value 16-bit value written over MDIO.
 * @return Nothing.
 * @pre @p reg addresses the modelled Clause-22 space.
 * @pre The caller has decoded a C22 write transaction.
 * @post BMCR.RESET / .AN_RESTART are dropped so the driver's poll sees them
 *       self-clear, exactly as a real PHY does.
 * @post Any other register stores @p value verbatim.
 * @note Not thread-safe.
 * @since 0.1.0
  * @details Write a modelled clause-22 phy register (self-clearing bmcr bits); this step is contained within the board periph Ethernet model and uses bounded caller or module-owned storage.
 */
RA8_INTERNAL static void internal_eth_phy_write(uint32_t reg, uint16_t value)
{
  if (reg >= (uint32_t)k_eth_phy_reg_count) {
    return;
  }
  if (reg == (uint32_t)k_eth_phy_reg_bmcr) {
    value = (uint16_t)(value & (uint16_t)~(k_eth_phy_bmcr_reset | k_eth_phy_bmcr_anrst));
  }
  s_eth.phy[reg] = value;
}

/**
 * @brief Execute an MPSM (PHY Station Management) transaction on write.
 *
 * @details HUM Ch 33.4.1.1 "MPSM : PHY Station Management Register" p 1707.
 * PSME (bit 0) starts the transfer and self-clears on completion. A Clause-22
 * read loads the PHY register into PRD[31:16]; a write updates the modelled
 * PHY. The stored word always has PSME cleared so the driver's drain / wait
 * poll resolves. Clause-45 is not modelled beyond clearing PSME (the eth path
 * uses C22 only).
 *
 * @param[in] mpsm_off Window offset of the MPSM register.
 * @param[in] value    Value written to MPSM.
 * @return Nothing.
 * @pre @p mpsm_off addresses an RMAC MPSM register.
 * @pre @p value has PSME set (a live transaction).
 * @post The MPSM shadow reads back with PSME == 0 and PRD holding the result.
 * @post A C22 write has updated the modelled PHY register file.
 * @note Not thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_eth_mpsm_exec(uint64_t mpsm_off, uint32_t value)
{
  const uint32_t pop =
    (value >> (uint32_t)k_ra8_rmac_shift_mpsm_pop) & (uint32_t)k_ra8_rmac_mask_mpsm_op;
  const uint32_t pra =
    (value >> (uint32_t)k_ra8_rmac_shift_mpsm_pra) & (uint32_t)k_ra8_rmac_mask_mpsm_phy_reg;
  const uint32_t prd =
    (value >> (uint32_t)k_ra8_rmac_shift_mpsm_prd) & (uint32_t)k_ra8_rmac_mask_mpsm_data;
  const bool c45 = (value & (uint32_t)k_eth_mpsm_mff) != 0U;
  uint32_t   out = value & ~(uint32_t)k_ra8_rmac_mpsm_psme; /* PSME self-clears. */
  if (!c45 && (pop == (uint32_t)k_ra8_rmac_mdio_op_c22_read)) {
    out = ((uint32_t)internal_eth_phy_read(pra) & (uint32_t)k_ra8_rmac_mask_mpsm_data)
          << (uint32_t)k_ra8_rmac_shift_mpsm_prd;
  } else if (!c45 && (pop == (uint32_t)k_ra8_rmac_mdio_op_c22_write)) {
    internal_eth_phy_write(pra, (uint16_t)prd);
  }
  internal_eth_shadow_write(mpsm_off, k_eth_reg32_bytes, out);
}

/* ------------------------------------------------------------------------- */
/* TX descriptor DMA (GWTRC kick -> peer). */
/* ------------------------------------------------------------------------- */

/**
 * @brief Move the frame queued on GWCA queue @p queue out to the peer.
 *
 * @details On the TX kick the model reads LINKFIX[queue] to find the chain
 * head, and if the head descriptor is FSINGLE (the only type the NIC TX path
 * emits) copies its frame out of emulated SRAM to ::board_net_on_tx and marks
 * the descriptor FEMPTY so the driver's TX-complete wait resolves. A head that
 * is not FSINGLE is left untouched (the driver's own timeout fires) -- never a
 * faked completion.
 *
 * @param[in,out] uc    Active engine (reads the rings from SRAM).
 * @param[in]     queue GWCA queue number that was kicked.
 * @return Nothing.
 * @pre The descriptor rings live in mapped emulated memory.
 * @pre @c s_eth.linkfix_base was captured from GWDCBAC1.
 * @post A single-fragment frame has been delivered and its slot freed.
 * @post @c s_eth.tx_frames / the MTGFCE mirror advanced on delivery.
 * @note Not thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_eth_tx_kick_queue(uc_engine* uc, uint32_t queue)
{
  if (s_eth.linkfix_base == 0U) {
    return;
  }
  uint8_t        lf[k_eth_desc_bytes];
  const uint32_t lf_addr = s_eth.linkfix_base + (queue * (uint32_t)k_eth_desc_bytes);
  internal_eth_desc_read(uc, lf_addr, lf);
  const uint32_t chain = internal_eth_desc_ptr(lf);
  if (chain == 0U) {
    return;
  }
  uint8_t head[k_eth_desc_bytes];
  internal_eth_desc_read(uc, chain, head);
  if (internal_eth_desc_dt(head) != (uint32_t)k_ra8_gwdcc_dt_fsingle) {
    return; /* multi-fragment / empty head is unmodelled here -- honest no-op. */
  }
  const uint32_t len = internal_eth_desc_ds(head);
  const uint32_t buf = internal_eth_desc_ptr(head);
  if ((len == 0U) || (len > (uint32_t)k_eth_frame_max) || (buf == 0U)) {
    return;
  }
  uint8_t frame[k_eth_frame_max];
  if (emu_mem_read(uc, (uint64_t)buf, frame, (size_t)len) != UC_ERR_OK) {
    return;
  }
  board_net_on_tx(frame, len);
  internal_eth_desc_set_dt(uc, chain, (uint32_t)k_ra8_gwdcc_dt_fempty);
  s_eth.tx_frames++;
  char ln[k_eth_console_cap];
  (void)snprintf(ln, sizeof(ln), "ETH tx q%u %uB", (unsigned)queue, (unsigned)len);
  board_console_push(k_board_console_ch_net, ln);
}

/**
 * @brief Fire ::internal_eth_tx_kick_queue for every set bit of a GWTRC word.
 * @details Fire ::internal_eth_tx_kick_queue for every set bit of a gwtrc word; this step is contained within the board periph Ethernet model and uses bounded caller or module-owned storage.
 * @param[in,out] uc Unicorn engine whose emulated state is read or updated.
 * @param[in] bits Bits input used by the operation.
 * @param[in] base_queue Base queue input used by the operation.
 * @pre Arguments satisfy the ranges documented for Ethernet gwtrc kick. @pre The call executes on the emulator's single owning thread.
 * @post State changes remain confined to the board periph Ethernet model and documented output objects. @post Ownership of caller-supplied storage is unchanged.
 * @note The operation is synchronous and does not transfer heap ownership.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_eth_gwtrc_kick(uc_engine* uc, uint32_t bits, uint32_t base_queue)
{
  for (uint32_t i = 0U; i < (uint32_t)k_eth_phy_reg_count; ++i) {
    if ((bits & (1UL << i)) != 0U) {
      internal_eth_tx_kick_queue(uc, base_queue + i);
    }
  }
}

/* ------------------------------------------------------------------------- */
/* RX descriptor DMA (peer -> ring, once per tick). */
/* ------------------------------------------------------------------------- */

/**
 * @brief Walk an RX ring from @p chain for the next FEMPTY slot.
 *
 * @param[in,out] uc       Active engine.
 * @param[in]     chain    Ring chain-head address in emulated memory.
 * @param[out]    out_slot Address of the first FEMPTY descriptor.
 * @return true if a free slot was found, else false (ring full / malformed).
 * @retval true  @p *out_slot holds a FEMPTY descriptor address.
 * @retval false No free slot within the walk bound.
 * @pre @p chain addresses a live descriptor ring.
 * @pre @p out_slot is non-null.
 * @post On true, @p *out_slot is a FEMPTY descriptor; on false it is untouched.
 * @post The walk is bounded by ::k_eth_ring_walk_max (NASA P10 Rule 2).
 * @note Not thread-safe.
 * @since 0.1.0
  * @details Walk an rx ring from @p chain for the next fempty slot; this step is contained within the board periph Ethernet model and uses bounded caller or module-owned storage.
 */
RA8_INTERNAL static bool
internal_eth_rx_find_slot(uc_engine* uc, uint32_t chain, uint32_t* out_slot)
{
  uint32_t cur = chain;
  for (uint32_t i = 0U; i < (uint32_t)k_eth_ring_walk_max; ++i) {
    uint8_t d8[k_eth_desc_bytes];
    internal_eth_desc_read(uc, cur, d8);
    const uint32_t dt = internal_eth_desc_dt(d8);
    if (dt == (uint32_t)k_ra8_gwdcc_dt_fempty) {
      *out_slot = cur;
      return true;
    }
    if ((dt == (uint32_t)k_ra8_gwdcc_dt_link) || (dt == (uint32_t)k_ra8_gwdcc_dt_linkfix)) {
      const uint32_t next = internal_eth_desc_ptr(d8);
      if ((next == 0U) || (next == cur)) {
        return false;
      }
      cur = next;
    } else {
      cur += (uint32_t)k_eth_desc_bytes;
    }
  }
  return false;
}

/**
 * @brief Drain waiting peer frames into free slots of the RX ring @p chain.
 *
 * @details For each free FEMPTY slot the model pulls one frame from
 * ::board_net_poll_rx (bounded by the slot buffer capacity), writes it into the
 * slot buffer, sets DS to the length and DT to FSINGLE so the driver's poll
 * delivers it. The peer is polled ONLY when a free slot exists, so a frame is
 * never dequeued and dropped -- a full ring is honest backpressure.
 *
 * @param[in,out] uc    Active engine.
 * @param[in]     chain RX ring chain-head address.
 * @return Nothing.
 * @pre The RX ring lives in mapped emulated memory.
 * @pre The GWCA is in OPERATION (checked by the caller).
 * @post Up to ::k_eth_rx_inject_max frames are staged FSINGLE in the ring.
 * @post @c s_eth.rx_frames / the MRFC / MRGFCE mirrors advance per frame.
 * @note Not thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_eth_rx_drain_peer(uc_engine* uc, uint32_t chain)
{
  for (uint32_t n = 0U; n < (uint32_t)k_eth_rx_inject_max; ++n) {
    uint32_t slot = 0U;
    if (!internal_eth_rx_find_slot(uc, chain, &slot)) {
      return; /* ring full -- backpressure, peer frame stays queued. */
    }
    uint8_t d8[k_eth_desc_bytes];
    internal_eth_desc_read(uc, slot, d8);
    const uint32_t cap = internal_eth_desc_ds(d8);
    const uint32_t buf = internal_eth_desc_ptr(d8);
    if ((buf == 0U) || (cap == 0U)) {
      return;
    }
    const uint32_t want = (cap < (uint32_t)k_eth_frame_max) ? cap : (uint32_t)k_eth_frame_max;
    uint8_t        frame[k_eth_frame_max];
    const uint32_t got = board_net_poll_rx(frame, want);
    if (got == 0U) {
      return; /* peer has nothing more. */
    }
    (void)emu_mem_write(uc, (uint64_t)buf, frame, (size_t)got);
    internal_eth_desc_set_ds(uc, slot, got);
    internal_eth_desc_set_dt(uc, slot, (uint32_t)k_ra8_gwdcc_dt_fsingle);
    s_eth.rx_frames++;
  }
}

/**
 * @brief Per-tick RX pump: stage peer frames once the GWCA is in OPERATION.
 * @details Per-tick rx pump: stage peer frames once the gwca is in operation; this step is contained within the board periph Ethernet model and uses bounded caller or module-owned storage.
 * @param[in,out] uc Unicorn engine whose emulated state is read or updated.
 * @pre Arguments satisfy the ranges documented for Ethernet tick. @pre The call executes on the emulator's single owning thread.
 * @post State changes remain confined to the board periph Ethernet model and documented output objects. @post Ownership of caller-supplied storage is unchanged.
 * @note The operation is synchronous and does not transfer heap ownership.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_eth_tick(uc_engine* uc)
{
  if ((s_eth.linkfix_base == 0U) || (s_eth.rx_queue == (uint32_t)k_eth_queue_none)) {
    return;
  }
  const uint64_t gwmc_woff =
    ((uint64_t)k_ra8_gwca0_base_addr - (uint64_t)k_eth_win_base) + (uint64_t)k_ra8_gwca_off_gwmc;
  const uint32_t opc = internal_eth_shadow_u32(gwmc_woff) & (uint32_t)k_ra8_gwmc_opc_mask;
  if (opc != (uint32_t)k_ra8_gwmc_opc_operation) {
    return; /* rings only live in OPERATION mode. */
  }
  uint8_t        lf[k_eth_desc_bytes];
  const uint32_t lf_addr = s_eth.linkfix_base + (s_eth.rx_queue * (uint32_t)k_eth_desc_bytes);
  internal_eth_desc_read(uc, lf_addr, lf);
  const uint32_t chain = internal_eth_desc_ptr(lf);
  if (chain != 0U) {
    internal_eth_rx_drain_peer(uc, chain);
  }
}

/* ------------------------------------------------------------------------- */
/* MMIO dispatch. */
/* ------------------------------------------------------------------------- */

/**
 * @brief MMIO read handler for the R-Switch window.
 *
 * @param[in,out] uc   Active engine (unused: reads never touch SRAM).
 * @param[in]     addr Absolute register address.
 * @param[in]     size Access width.
 * @return The register value.
 * @retval value Computed status or the config shadow.
 * @pre @p addr is inside @c [k_eth_win_base, k_eth_win_base + k_eth_win_span).
 * @pre @p size is 1, 2, or 4.
 * @post No emulated memory is modified.
 * @post Status registers converge to their commanded / modelled value.
 * @note Not thread-safe.
 * @since 0.1.0
  * @details MMIO read handler for the r-switch window; this step is contained within the board periph Ethernet model and uses bounded caller or module-owned storage.
 */
RA8_INTERNAL static uint64_t internal_eth_read(uc_engine* uc, uint64_t addr, unsigned size)
{
  (void)uc;
  const uint64_t off   = addr - (uint64_t)k_eth_win_base;
  uint32_t       queue = 0U;
  if (internal_eth_is_etha_reg(addr, (uint64_t)k_ra8_etha_off_eams)) {
    /* EAMS.OPS mirrors the commanded EAMC.OPC (the mode machine converges). */
    /* HUM Ch 32.3.1.2 "EAMS : Mode Status Register" p 1631 */
    const uint64_t eamc = off - ((uint64_t)k_ra8_etha_off_eams - (uint64_t)k_ra8_etha_off_eamc);
    return internal_eth_shadow_u32(eamc) & (uint32_t)k_ra8_etha_mask_ops;
  }
  if (addr == (uint64_t)k_ra8_gwca0_base_addr + (uint64_t)k_ra8_gwca_off_gwms) {
    /* GWMS.OPS mirrors the commanded GWMC.OPC. */
    /* HUM Ch 34.3.2 "GWMS : Mode Status Register" p 1792 */
    const uint64_t gwmc = off - ((uint64_t)k_ra8_gwca_off_gwms - (uint64_t)k_ra8_gwca_off_gwmc);
    return internal_eth_shadow_u32(gwmc) & (uint32_t)k_ra8_gwmc_opc_mask;
  }
  if (addr == (uint64_t)k_ra8_gwca0_base_addr + (uint64_t)k_ra8_gwca_off_gwarirm) {
    /* GWARIRM.ARR (AXI-init done) follows the ARIOG request. */
    /* HUM Ch 34 "Ethernet CPU Agent (GWCA)" p 1787 */
    const uint32_t v = internal_eth_shadow_u32(off);
    return ((v & (uint32_t)k_eth_gwarirm_ariog) != 0U) ? (v | (uint32_t)k_eth_gwarirm_arr) : v;
  }
  if (addr == (uint64_t)k_ra8_coma_base_addr + (uint64_t)k_ra8_coma_off_cabpirm) {
    /* CABPIRM.BPR self-sets after the BPIOG kick (buffer-pool init completes
     * ~512 clocks later); modelling it lets the board bring-up's non-emulator
     * BPR poll converge. */
    /* HUM Ch 31.3.2.7 "CABPIRM" p 1599 */
    const uint32_t v = internal_eth_shadow_u32(off);
    return ((v & (uint32_t)k_ra8_coma_cabpirm_bpiog) != 0U) ? (v | (uint32_t)k_ra8_coma_cabpirm_bpr)
                                                            : v;
  }
  if (internal_eth_is_gwdcc(addr, &queue)) {
    /* GWDCC.BALR self-clears after the reload completes. */
    /* HUM Ch 34.3 "GWDCCi" p 1811 */
    return internal_eth_shadow_u32(off) & ~(uint32_t)k_ra8_gwdcc_balr;
  }
  if (internal_eth_is_rmac_reg(addr, (uint64_t)k_ra8_rmac_off_mtgfce)) {
    return s_eth.tx_frames;
  }
  if (internal_eth_is_rmac_reg(addr, (uint64_t)k_eth_rmac_off_mrfc) ||
      internal_eth_is_rmac_reg(addr, (uint64_t)k_ra8_rmac_off_mrgfce)) {
    return s_eth.rx_frames;
  }
  return internal_eth_shadow_read(off, size);
}

/**
 * @brief MMIO write handler for the R-Switch window.
 *
 * @param[in,out] uc    Active engine (TX kick reads the rings from SRAM).
 * @param[in]     addr  Absolute register address.
 * @param[in]     size  Access width.
 * @param[in]     value Value written.
 * @return Nothing.
 * @pre @p addr is inside the block window.
 * @pre @p size is 1, 2, or 4.
 * @post Config registers reflect @p value; action registers run their model.
 * @post GWDCBAC1 captures the ring base; a GWDCC write records the RX queue.
 * @note Not thread-safe.
 * @since 0.1.0
  * @details MMIO write handler for the r-switch window; this step is contained within the board periph Ethernet model and uses bounded caller or module-owned storage.
 */
RA8_INTERNAL static void
internal_eth_write(uc_engine* uc, uint64_t addr, unsigned size, uint64_t value)
{
  const uint64_t off   = addr - (uint64_t)k_eth_win_base;
  uint32_t       queue = 0U;
  if (addr == (uint64_t)k_ra8_gwca0_base_addr + (uint64_t)k_ra8_gwca_off_gwtrc0) {
    internal_eth_shadow_write(off, size, 0U); /* request register clears when consumed. */
    internal_eth_gwtrc_kick(uc, (uint32_t)value, 0U);
    return;
  }
  if (addr == (uint64_t)k_ra8_gwca0_base_addr + (uint64_t)k_ra8_gwca_off_gwtrc1) {
    internal_eth_shadow_write(off, size, 0U);
    internal_eth_gwtrc_kick(uc, (uint32_t)value, (uint32_t)k_eth_phy_reg_count);
    return;
  }
  if (addr == (uint64_t)k_ra8_gwca0_base_addr + (uint64_t)k_ra8_gwca_off_gwdcbac1) {
    /* Capture the descriptor-chain base so the DMA can walk the rings. */
    /* HUM Ch 34 "Ethernet CPU Agent (GWCA)" p 1787 */
    s_eth.linkfix_base = (uint32_t)value;
    internal_eth_shadow_write(off, size, value);
    return;
  }
  if (internal_eth_is_rmac_reg(addr, (uint64_t)k_ra8_rmac_off_mpsm) &&
      ((value & (uint32_t)k_ra8_rmac_mpsm_psme) != 0U)) {
    internal_eth_mpsm_exec(off, (uint32_t)value);
    return;
  }
  internal_eth_shadow_write(off, size, value);
  if (internal_eth_is_gwdcc(addr, &queue) && ((value & (uint32_t)k_ra8_gwdcc_dqt) == 0U)) {
    s_eth.rx_queue = queue; /* GWDCC.DQT == 0 marks the reception queue. */
  }
}

/**
 * @brief Reset the R-Switch model and re-seed the modelled PHY register file.
 * @details Reset the r-switch model and re-seed the modelled phy register file; this step is contained within the board periph Ethernet model and uses bounded caller or module-owned storage.
 * @pre Arguments satisfy the ranges documented for Ethernet reset. @pre The call executes on the emulator's single owning thread.
 * @post State changes remain confined to the board periph Ethernet model and documented output objects. @post Ownership of caller-supplied storage is unchanged.
 * @note The operation is synchronous and does not transfer heap ownership.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_eth_reset(void)
{
  s_eth                           = (eth_state_t){};
  s_eth.rx_queue                  = (uint32_t)k_eth_queue_none;
  s_eth.phy[k_eth_phy_reg_bmcr]   = (uint16_t)k_eth_phy_bmcr_seed;
  s_eth.phy[k_eth_phy_reg_bmsr]   = (uint16_t)k_eth_phy_bmsr_seed;
  s_eth.phy[k_eth_phy_reg_anlpar] = (uint16_t)k_eth_phy_anlpar_seed;
}

/**
 * @brief End-of-run R-Switch section: frames moved through the register model.
 * @details End-of-run r-switch section: frames moved through the register model; this step is contained within the board periph Ethernet model and uses bounded caller or module-owned storage.
 * @pre Arguments satisfy the ranges documented for Ethernet report. @pre The call executes on the emulator's single owning thread.
 * @post State changes remain confined to the board periph Ethernet model and documented output objects. @post Ownership of caller-supplied storage is unchanged.
 * @note The operation is synchronous and does not transfer heap ownership.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_eth_report(void)
{
  if ((s_eth.tx_frames == 0U) && (s_eth.rx_frames == 0U)) {
    return; /* not a networking run -- stay quiet. */
  }
  (void)priv_emu_io_errf("  R-Switch      : TX %u frame(s) RX %u frame(s) via GWCA/ETHA/RMAC "
                         "regs; link 100M/FD (MDIO)\n",
                         s_eth.tx_frames,
                         s_eth.rx_frames);
}

/** @brief R-Switch cluster block descriptor. */
static const board_periph_block_t s_k_eth_block = {
  .base   = (uint64_t)k_eth_win_base,
  .span   = (uint64_t)k_eth_win_span,
  .order  = (uint32_t)k_eth_block_order,
  .read   = internal_eth_read,
  .write  = internal_eth_write,
  .tick   = internal_eth_tick,
  .reset  = internal_eth_reset,
  .report = internal_eth_report,
  .name   = "R-Switch(ETH)",
};

/** @brief Register the R-Switch block + seed the PHY file before main runs. */
[[gnu::constructor]] RA8_INTERNAL static void internal_eth_block_register(void)
{
  internal_eth_reset();
  board_periph_register_block(&s_k_eth_block);
}
