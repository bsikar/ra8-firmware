/**
 * @file ra8d2_can_regs.h
 * @brief Classical CAN controller register layout (stubbed) for the Renesas RA8D2
 *
 * @details
 * The RA8D2 group does NOT carry the legacy classical CAN block found
 * on older RX / RA4 / RA6 parts -- it ships with the CANFD Lite-style
 * peripheral covered by ``ra8d2_canfd_regs.h``. FSP keeps a separate
 * ``r_can`` driver targeted at the legacy block (see
 * ``fsp/src/r_can/r_can.c``) and several Renesas-supported parts in the
 * RX and RA4/RA6 ranges expose it.
 *
 * This header models the same memory-mapped surface so the
 * ``ra_can`` driver can compile and be exercised in the host unit-test
 * substrate. The address window is a placeholder inside the simulator
 * peripheral region; on real RA8D2 silicon the driver should not be
 * brought up because the underlying block is absent.
 *
 * Layout summary (offsets relative to the channel base; FSP
 * ``r_can.c`` programmes the same field semantics):
 *
 *  - 0x000 : MKR0..MKR7   -- mailbox-mask registers (8 entries)
 *  - 0x020 : FIDCR0/1     -- FIFO ID compare registers
 *  - 0x028 : MKIVLR       -- mask-invalid register
 *  - 0x02C : MIER         -- mailbox interrupt enable
 *  - 0x040 : MB[0..31]    -- 32 mailboxes; ID, DLC, DATA[8], TS
 *  - 0x820 : MCTL[0..31]  -- per-mailbox control bytes
 *  - 0x840 : CTLR         -- control register (MBM/IDFM/MLM/...)
 *  - 0x842 : STR          -- status register
 *  - 0x844 : BCR          -- bit configuration register
 *  - 0x848 : RFCR / RFPCR -- RX FIFO control / pointer ctrl
 *  - 0x84A : TFCR / TFPCR -- TX FIFO control / pointer ctrl
 *  - 0x84C : EIER         -- error-interrupt enable
 *  - 0x84D : EIFR         -- error interrupt factor
 *  - 0x84E : RECR / TECR  -- RX / TX error counters
 *  - 0x850 : ECSR         -- error code store
 *  - 0x852 : CSSR / MSSR  -- channel-search / mailbox-search support
 *  - 0x854 : MSMR         -- mailbox-search mode
 *  - 0x856 : TSR          -- timestamp register
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>
#include <stdint.h>

/* =============================================================================
 * Limits
 * =============================================================================
 */

/**
 * @enum ra_can_limits_t
 * @brief Mailbox / payload limits common to FSP ``r_can``.
 */
typedef enum : uint8_t {
  k_ra_can_channel_count  = 1U,    /**< RA8D2 stub: one classical CAN channel.       */
  k_ra_can_mailbox_count  = 32U,   /**< Mailbox count per channel.                   */
  k_ra_can_data_bytes     = 8U,    /**< Classical CAN payload limit.                 */
  k_ra_can_dlc_max        = 8U,    /**< 4-bit DLC capped at 8 for classical CAN.     */
  k_ra_can_dlc_field_mask = 0x0FU, /**< DLC field is the low 4 bits of the DLC reg.  */
  k_ra_can_mkr_count      = 8U,    /**< MKR[0..7] mask-register array length.        */
} ra_can_limits_t;

/**
 * @enum ra_can_pad_t
 * @brief Reserved-region byte counts inside the channel block.
 */
typedef enum : uint16_t {
  k_ra_can_reserved0_bytes = 0x10U,  /**< Reserved hole 0x030..0x03F.       */
  k_ra_can_reserved1_bytes = 0x560U, /**< Reserved hole 0x2C0..0x81F.       */
} ra_can_pad_t;

/* =============================================================================
 * Base address (placeholder)
 * =============================================================================
 *
 * The address sits inside the peripheral window already mapped by the
 * host-test mmap helper. A real classical-CAN block would live at the
 * address documented in HUM Ch "CAN Module"; the RA8D2 HUM does not
 * carry that chapter, hence the placeholder.
 */

/**
 * @enum ra_can_addr_t
 * @brief Channel base addresses.
 */
typedef enum : uintptr_t {
  k_ra_can0_base_addr = 0x40710000UL, /**< Placeholder; classical CAN absent on RA8D2 silicon. */
} ra_can_addr_t;

/* =============================================================================
 * Mailbox layout
 * =============================================================================
 */

/**
 * @struct r_can_mb_t
 * @brief One classical-CAN mailbox.
 *
 * @invariant ``DLC <= k_ra_can_dlc_max``.
 */
typedef struct {
  volatile uint32_t ID;                        /**< +0x00 ID + IDE + RTR.       */
  volatile uint16_t DLC;                       /**< +0x04 Data length code.     */
  volatile uint16_t RESERVED0;                 /**< +0x06 reserved.             */
  volatile uint8_t  DATA[k_ra_can_data_bytes]; /**< +0x08 Payload bytes.        */
  volatile uint16_t TS;                        /**< +0x10 Timestamp.            */
  volatile uint16_t RESERVED1;                 /**< +0x12 reserved (pad to 0x14).*/
} r_can_mb_t;

/* =============================================================================
 * Channel register block
 * =============================================================================
 */

/**
 * @struct r_can_t
 * @brief Memory-mapped layout of the classical CAN controller.
 */
typedef struct {
  volatile uint32_t MKR[k_ra_can_mkr_count]; /**< 0x000 Mailbox mask registers.        */
  volatile uint32_t FIDCR0;                  /**< 0x020 FIFO ID compare 0.             */
  volatile uint32_t FIDCR1;                  /**< 0x024 FIFO ID compare 1.             */
  volatile uint32_t MKIVLR;                  /**< 0x028 Mask invalid register.         */
  volatile uint32_t MIER;                    /**< 0x02C Mailbox interrupt enable.      */
  volatile uint8_t  RESERVED0[k_ra_can_reserved0_bytes]; /**< 0x030..0x03F reserved.        */
  r_can_mb_t        MB[k_ra_can_mailbox_count]; /**< 0x040 32 mailboxes.                  */
  volatile uint8_t  RESERVED1[k_ra_can_reserved1_bytes]; /**< 0x2C0..0x81F reserved.        */
  volatile uint8_t  MCTL[k_ra_can_mailbox_count]; /**< 0x820 Per-mailbox control bytes.     */
  volatile uint16_t CTLR;                         /**< 0x840 Control register.              */
  volatile uint16_t STR;                          /**< 0x842 Status register.               */
  volatile uint32_t BCR;                          /**< 0x844 Bit configuration register.    */
  volatile uint8_t  RFCR;                         /**< 0x848 RX FIFO control register.      */
  volatile uint8_t  RFPCR;                        /**< 0x849 RX FIFO pointer ctrl.          */
  volatile uint8_t  TFCR;                         /**< 0x84A TX FIFO control register.      */
  volatile uint8_t  TFPCR;                        /**< 0x84B TX FIFO pointer ctrl.          */
  volatile uint8_t  EIER;                         /**< 0x84C Error interrupt enable.        */
  volatile uint8_t  EIFR;                         /**< 0x84D Error interrupt factor.        */
  volatile uint8_t  RECR;                         /**< 0x84E RX error counter.              */
  volatile uint8_t  TECR;                         /**< 0x84F TX error counter.              */
  volatile uint8_t  ECSR;                         /**< 0x850 Error code store register.     */
  volatile uint8_t  CSSR;                         /**< 0x851 Channel-search support reg.    */
  volatile uint8_t  MSSR;                         /**< 0x852 Mailbox-search status reg.     */
  volatile uint8_t  MSMR;                         /**< 0x853 Mailbox-search mode reg.       */
  volatile uint16_t TSR;                          /**< 0x854 Timestamp register.            */
} r_can_t;

/**
 * @enum ra_can_block_size_t
 * @brief Total byte size of ``r_can_t`` (used by ``static_assert`` below).
 */
typedef enum : uint16_t {
  k_ra_can_block_bytes = 0x856U, /**< 0x856 bytes per channel. */
} ra_can_block_size_t;

static_assert(sizeof(r_can_t) >= (size_t)k_ra_can_block_bytes,
              "r_can_t layout shrunk below documented size");

/**
 * @brief Accessor for a classical-CAN channel register block.
 *
 * @param[in] channel Channel index (must be < ``k_ra_can_channel_count``).
 *
 * @return Pointer to the memory-mapped register block, or NULL when
 *         the channel index is out of range.
 *
 * @since 0.1.0
 */
static inline volatile r_can_t* ra_can(uint8_t channel)
{
  if (channel >= k_ra_can_channel_count) {
    return (volatile r_can_t*)0;
  }
  return (volatile r_can_t*)k_ra_can0_base_addr;
}

/* =============================================================================
 * Bit fields
 * =============================================================================
 */

/**
 * @enum ra_can_id_t
 * @brief Mailbox-ID encoding (matches FSP r_can field semantics).
 */
typedef enum : uint32_t {
  k_ra_can_id_std_mask = 0x000007FFUL, /**< 11-bit standard ID mask.       */
  k_ra_can_id_ext_mask = 0x1FFFFFFFUL, /**< 29-bit extended ID mask.       */
  k_ra_can_id_rtr_msk  = 0x40000000UL, /**< Remote-transmission request.   */
  k_ra_can_id_ide_msk  = 0x80000000UL, /**< Identifier-extension flag.     */
} ra_can_id_t;

/**
 * @enum ra_can_mctl_t
 * @brief MCTL[i] (per-mailbox control byte) bit positions.
 */
typedef enum : uint8_t {
  k_ra_can_mctl_newdata  = 0x01U, /**< NEWDATA latched on RX completion.  */
  k_ra_can_mctl_sentdata = 0x02U, /**< SENTDATA latched on TX completion. */
  k_ra_can_mctl_trmreq   = 0x80U, /**< TRMREQ -- start a transmission.    */
  k_ra_can_mctl_recreq   = 0x40U, /**< RECREQ -- arm for reception.       */
} ra_can_mctl_t;

/**
 * @enum ra_can_ctlr_t
 * @brief CTLR bit positions (mode / sleep request).
 */
typedef enum : uint16_t {
  k_ra_can_ctlr_sleep = (uint16_t)(1U << 5U), /**< SLPM -- sleep mode req. */
  k_ra_can_ctlr_halt  = (uint16_t)(1U << 9U), /**< Halt request.           */
  k_ra_can_ctlr_reset = (uint16_t)(1U << 0U), /**< CAN reset request.      */
} ra_can_ctlr_t;

/**
 * @enum ra_can_str_t
 * @brief STR bit positions (status flags consumed by tests).
 */
typedef enum : uint16_t {
  k_ra_can_str_reset_state   = (uint16_t)(1U << 8U),  /**< Reset-mode latched.    */
  k_ra_can_str_halt_state    = (uint16_t)(1U << 9U),  /**< Halt-mode latched.     */
  k_ra_can_str_error_passive = (uint16_t)(1U << 13U), /**< Error-passive state.   */
  k_ra_can_str_bus_off       = (uint16_t)(1U << 14U), /**< Bus-off state.         */
} ra_can_str_t;

#ifdef __cplusplus
}
#endif
