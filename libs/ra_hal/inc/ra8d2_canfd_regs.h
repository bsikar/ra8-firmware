/**
 * @file ra8d2_canfd_regs.h
 * @brief CANFD Lite controller register layout for the Renesas RA8D2
 *
 * @details
 * Two CANFD Lite instances live at `0x40380000` (CANFD0) and
 * `0x40382000` (CANFD1). The RA8D2 uses the "CANFD Lite" IP (not the
 * older "CANFD_B" on some RA6 parts), which exposes:
 *
 *  - One **global block** shared between both controllers, with
 *    global configuration, nominal + data bit-rate prescalers,
 *    error counters, and a per-instance reset bit.
 *  - Two **channel blocks** -- one per controller -- with channel
 *    control / status, transmit queue, receive buffers, and
 *    acceptance filters.
 *  - A **message buffer** area for TX / RX frames.
 *
 * This header models the field layouts documented in RA8D2 HUM
 * section "CANFD Lite" -- enough to programme nominal + data
 * bit-rate, send one frame, and poll for RX. DMA hooks and the full
 * acceptance-filter bank come later.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

typedef enum : uintptr_t {
  k_ra_canfd0_base_addr = 0x40380000UL,
  k_ra_canfd1_base_addr = 0x40382000UL,
} ra_canfd_addr_t;

typedef enum : uint8_t {
  k_ra_canfd_instance_count = 2U,
} ra_canfd_limits_t;

/**
 * @enum ra_canfd_frame_limits_t
 * @brief Message-frame limits common to the public API and register layer.
 */
typedef enum : uint8_t {
  k_ra_canfd_data_bytes_max = 64U, /**< CAN-FD payload cap in bytes.     */
  k_ra_canfd_dlc_max        = 15U, /**< 4-bit DLC max value (64 bytes).  */
  k_ra_canfd_df_word_count  = 16U, /**< 64-byte payload -> 16 x u32.     */
} ra_canfd_frame_limits_t;

/**
 * @enum ra_canfd_reserved_pad_t
 * @brief Reserved-padding sizes that sit between named CANFD registers.
 */
typedef enum : uint16_t {
  k_ra_canfd_pad_after_fdcrc  = 0xCCU, /**< Gap from CFDCNFDCRC to CFDGCTR.  */
  k_ra_canfd_pad_after_fddf   = 0x20U, /**< Gap from CFDRFDF[15] to CFDTMID. */
  k_ra_canfd_pad_after_tmsts  = 0x0CU, /**< Gap from CFDTMSTS to CFDTMDF[0]. */
  k_ra_canfd_pad_after_ffdsts = 0x04U, /**< Gap from CFDRFFDSTS to CFDRFDF.*/
} ra_canfd_reserved_pad_t;

/* =============================================================================
 * Per-channel register window
 * =============================================================================
 *
 * Offsets follow HUM Table "CANFD Lite register list". Only the
 * registers we currently touch are named; the rest of the channel
 * block is modelled as reserved padding so the offsets stay correct.
 */

/**
 * @struct r_canfd_channel_regs_t
 * @brief CANFD Lite per-channel register window (partial).
 */
typedef struct {
  volatile uint32_t CFDCNCFG;  /**< +0x000 Nominal Bit-Rate Config.        */
  volatile uint32_t CFDCNCTR;  /**< +0x004 Channel Control.                */
  volatile uint32_t CFDCNSTS;  /**< +0x008 Channel Status.                 */
  volatile uint32_t CFDCNERFL; /**< +0x00C Error Flag.                     */
  volatile uint8_t  _r0[0x10];
  volatile uint32_t CFDCNDCFG;  /**< +0x020 Data Bit-Rate Config (FD).      */
  volatile uint32_t CFDCNFDCFG; /**< +0x024 CAN FD Config.                  */
  volatile uint32_t CFDCNFDCTR; /**< +0x028 CAN FD Control.                 */
  volatile uint32_t CFDCNFDSTS; /**< +0x02C CAN FD Status.                  */
  volatile uint32_t CFDCNFDCRC; /**< +0x030 CAN FD CRC.                     */
  volatile uint8_t  _r1[k_ra_canfd_pad_after_fdcrc];
  volatile uint32_t CFDGCTR;    /**< +0x100 Global Control.                 */
  volatile uint32_t CFDRFCC;    /**< +0x104 RX FIFO Config/Control.         */
  volatile uint32_t CFDRFSTS;   /**< +0x108 RX FIFO Status.                 */
  volatile uint32_t CFDRFPCTR;  /**< +0x10C RX FIFO Pointer Control.        */
  volatile uint32_t CFDRFID;    /**< +0x110 RX FIFO ID.                     */
  volatile uint32_t CFDRFPTR;   /**< +0x114 RX FIFO Pointer.                */
  volatile uint32_t CFDRFFDSTS; /**< +0x118 RX FIFO FD Status.              */
  volatile uint8_t  _r2[k_ra_canfd_pad_after_ffdsts];
  volatile uint32_t CFDRFDF[k_ra_canfd_df_word_count]; /**< RX FIFO Data. */
  volatile uint8_t  _r3[k_ra_canfd_pad_after_fddf];
  volatile uint32_t CFDTMID;    /**< +0x180 TX Message Buffer ID.           */
  volatile uint32_t CFDTMPTR;   /**< +0x184 TX Message Buffer Pointer.      */
  volatile uint32_t CFDTMFDSTS; /**< +0x188 TX Message Buffer FD Status.    */
  volatile uint32_t CFDTMC;     /**< +0x18C TX Message Buffer Control.     */
  volatile uint32_t CFDTMSTS;   /**< +0x190 TX Message Buffer Status.      */
  volatile uint8_t  _r4[k_ra_canfd_pad_after_tmsts];
  volatile uint32_t CFDTMDF[k_ra_canfd_df_word_count]; /**< TX Data. */
} r_canfd_channel_regs_t;

/** @brief Get pointer to CANFD channel N (0..1). */
static inline volatile r_canfd_channel_regs_t* ra_canfd(uint8_t channel)
{
  if (channel >= (uint8_t)k_ra_canfd_instance_count) {
    return nullptr;
  }
  const uintptr_t base = (channel == 0U) ? k_ra_canfd0_base_addr : k_ra_canfd1_base_addr;
  return (volatile r_canfd_channel_regs_t*)base;
}

/* =============================================================================
 * Channel Control (CFDCNCTR) bit positions
 * =============================================================================
 */

typedef enum : uint8_t {
  k_ra_cnctr_bit_chmdc = 0U,  /**< Channel Mode Control [2:0].          */
  k_ra_cnctr_bit_csli  = 4U,  /**< Channel Sleep Request.               */
  k_ra_cnctr_bit_bom   = 21U, /**< Bus-Off Mode.                        */
} ra_cnctr_bit_t;

/**
 * @enum ra_chmdc_mode_t
 * @brief Channel Mode Control (`CFDCNCTR.CHMDC[2:0]`) values.
 */
typedef enum : uint8_t {
  k_ra_chmdc_operation = 0U, /**< Bus operation mode.         */
  k_ra_chmdc_reset     = 1U, /**< Reset mode.                 */
  k_ra_chmdc_halt      = 2U, /**< Halt mode.                  */
} ra_chmdc_mode_t;

/* =============================================================================
 * Channel Status (CFDCNSTS) bit positions
 * =============================================================================
 */

typedef enum : uint8_t {
  k_ra_cnsts_bit_crstst = 0U, /**< Channel Reset Status.        */
  k_ra_cnsts_bit_chltst = 1U, /**< Channel Halt Status.         */
  k_ra_cnsts_bit_cslpst = 2U, /**< Channel Sleep Status.        */
  k_ra_cnsts_bit_epst   = 3U, /**< Error Passive Status.        */
  k_ra_cnsts_bit_bosst  = 4U, /**< Bus-Off Status.              */
} ra_cnsts_bit_t;

/* =============================================================================
 * Nominal Bit-Rate Config (CFDCNCFG) and Data Bit-Rate Config (CFDCNDCFG)
 * =============================================================================
 *
 * Both registers share the same prescaler/TSEG1/TSEG2/SJW layout, but
 * with different field widths. For the RA8D2 CANFD Lite block the
 * nominal register packs them as:
 *
 *   BRP   [9:0]   nominal baud-rate prescaler (prescaler - 1)
 *   TSEG1 [15:10] phase-segment 1 (TSEG1 - 1)
 *   TSEG2 [22:16] phase-segment 2 (TSEG2 - 1)
 *   SJW   [26:24] synchronization jump width (SJW - 1)
 *
 * The data-phase register has smaller TSEG1/TSEG2 fields appropriate
 * for the faster data-phase clock but the shifts stay the same.
 */

typedef enum : uint8_t {
  k_ra_cncfg_shift_brp   = 0U,
  k_ra_cncfg_shift_tseg1 = 10U,
  k_ra_cncfg_shift_tseg2 = 16U,
  k_ra_cncfg_shift_sjw   = 24U,
} ra_cncfg_shift_t;

typedef enum : uint32_t {
  k_ra_cncfg_mask_brp   = 0x3FFUL, /**< [9:0]  10-bit prescaler field. */
  k_ra_cncfg_mask_tseg1 = 0x3FUL,  /**< [15:10] 6-bit TSEG1 field.     */
  k_ra_cncfg_mask_tseg2 = 0x7FUL,  /**< [22:16] 7-bit TSEG2 field.     */
  k_ra_cncfg_mask_sjw   = 0x07UL,  /**< [26:24] 3-bit SJW field.       */
} ra_cncfg_mask_t;

/**
 * @enum ra_canfd_bit_timing_limits_t
 * @brief Prescaler / TSEG / SJW resolution bounds.
 *
 * @details
 * Per HUM "CANFD Lite" the nominal bit-rate register carries a 10-bit
 * prescaler (1..256 usable), 6-bit TSEG1 (2..64), 7-bit TSEG2 (2..64),
 * and 3-bit SJW (1..4). The driver picks a 75% sample point and an
 * SJW of `min(4, TSEG2)` which keeps every resolved value safely in
 * range for both nominal and data phases.
 */
typedef enum : uint32_t {
  k_ra_canfd_tq_per_bit    = 20U,  /**< Chosen time quanta per bit.     */
  k_ra_canfd_sample_num    = 15U,  /**< TSEG1 = 15 tq  -> 75% sample.   */
  k_ra_canfd_sample_den    = 5U,   /**< TSEG2 = 4  tq  -> 25% balance.  */
  k_ra_canfd_prescaler_min = 1U,   /**< Smallest valid prescaler value. */
  k_ra_canfd_prescaler_max = 256U, /**< Largest valid prescaler value.  */
  k_ra_canfd_sjw_max       = 4U,   /**< SJW cap = min(4, TSEG2).        */
} ra_canfd_bit_timing_limits_t;

/* =============================================================================
 * Error-Flag register (CFDCNERFL)
 * =============================================================================
 */

typedef enum : uint8_t {
  k_ra_cnerfl_shift_tec = 16U, /**< Transmit Error Counter [23:16]. */
  k_ra_cnerfl_shift_rec = 24U, /**< Receive Error Counter [31:24].  */
} ra_cnerfl_shift_t;

typedef enum : uint32_t {
  k_ra_cnerfl_mask_tec = 0xFFUL, /**< TEC field mask (post-shift). */
  k_ra_cnerfl_mask_rec = 0xFFUL, /**< REC field mask (post-shift). */
} ra_cnerfl_mask_t;

/* =============================================================================
 * Global Control (CFDGCTR)
 * =============================================================================
 */

typedef enum : uint8_t {
  k_ra_gctr_bit_gmdc_operation = 0U, /**< Global mode: operation.   */
  k_ra_gctr_bit_gmdc_reset     = 1U, /**< Global mode: reset.       */
  k_ra_gctr_bit_gmdc_halt      = 2U, /**< Global mode: halt.        */
} ra_gctr_mode_t;

/* =============================================================================
 * RX FIFO status / control (CFDRFSTS, CFDRFPCTR)
 * =============================================================================
 */

typedef enum : uint32_t {
  k_ra_rfsts_bit_empty  = 0x01UL, /**< RFEMP: FIFO empty flag.    */
  k_ra_rfsts_bit_full   = 0x02UL, /**< RFFLL: FIFO full flag.     */
  k_ra_rfpctr_value_ack = 0xFFUL, /**< Dummy write to pop entry.  */
} ra_rxfifo_bits_t;

/* =============================================================================
 * Message-ID layout (CFDRFID / CFDTMID)
 * =============================================================================
 */

typedef enum : uint32_t {
  k_ra_canfd_id_std_mask = 0x000007FFUL, /**< 11-bit standard ID.   */
  k_ra_canfd_id_ext_mask = 0x1FFFFFFFUL, /**< 29-bit extended ID.   */
  k_ra_canfd_id_ide      = 1UL << 30U,   /**< IDE: extended flag.   */
  k_ra_canfd_id_rtr      = 1UL << 29U,   /**< RTR: remote frame.    */
} ra_canfd_id_bits_t;

/* =============================================================================
 * FD-status layout (CFDTMFDSTS / CFDRFFDSTS)
 * =============================================================================
 */

typedef enum : uint32_t {
  k_ra_canfd_fd_fdf = 1UL << 0U, /**< FDF: frame is CAN-FD.        */
  k_ra_canfd_fd_brs = 1UL << 1U, /**< BRS: bit-rate switch.        */
  k_ra_canfd_fd_esi = 1UL << 2U, /**< ESI: error state indicator.  */
} ra_canfd_fd_bits_t;

/* =============================================================================
 * Pointer layout (CFDTMPTR / CFDRFPTR): DLC lives in [31:28]
 * =============================================================================
 */

typedef enum : uint8_t {
  k_ra_canfd_ptr_shift_dlc = 28U, /**< DLC field shift.  */
} ra_canfd_ptr_shift_t;

typedef enum : uint32_t {
  k_ra_canfd_ptr_mask_dlc = 0xFUL, /**< 4-bit DLC mask. */
} ra_canfd_ptr_mask_t;

/* =============================================================================
 * TX control (CFDTMC)
 * =============================================================================
 */

typedef enum : uint32_t {
  k_ra_canfd_tmc_txreq = 1UL << 0U, /**< TMTR: transmit request. */
} ra_canfd_tmc_bits_t;

#ifdef __cplusplus
}
#endif
