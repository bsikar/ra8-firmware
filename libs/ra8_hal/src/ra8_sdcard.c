/**
 * @file ra8_sdcard.c
 * @brief SD card driver implementation -- SD Physical Layer state machine
 *
 * @par Tag
 * [Ring 3 / HAL] {World: NS}
 *
 * @details
 * Implements the SD Physical Layer initialization sequence on top of
 * the lower-level ::ra8_sdhi register driver:
 *
 *   CMD0  -> CMD8 -> ACMD41 (loop) -> CMD2 -> CMD3 -> CMD9 -> CMD7
 *
 * After the card is parked in TRAN state the bus clock is bumped from
 * the 400 kHz identification rate to 25 MHz default-speed by way of
 * ::ra8_sdhi_set_clock. Block I/O calls fall through to the polled
 * ::ra8_sdhi_read_block / ::ra8_sdhi_write_block primitives.
 *
 * The CSD register is decoded for both v1 (SDSC byte-addressed) and
 * v2 (SDHC/SDXC block-addressed) layouts. Capacity is normalized to
 * 512-byte blocks regardless of card class so callers always speak in
 * sector counts.
 *
 * Every SDHI register access cites HUM Ch 47 "SD/MMC Host Interface
 * (SDHI)" (HUM pages 3122-3179). SD command numbers and response
 * formats follow the SD Physical Layer Specification v6.00 -- HUM
 * Ch 47.1 "Overview" pins SDHI to that document directly.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra8_sdcard.h"

#include <stdint.h>

#include "ra8_attributes.h"
#include "ra8_check.h"
#include "ra8_err.h"
#include "ra8_log.h"
#include "ra8_sdhi.h"

/** @brief Module log tag. */
static const char* s_tag = "SDCARD";

/** @brief SD CSD register field masks, shifts and block size. */
typedef enum : uint32_t {
  k_sd_block_size_bytes  = 512U,         /**< SD logical block size.          */
  k_sd_csd_struct_shift  = 30U,          /**< CSD_STRUCTURE field shift.      */
  k_sd_rca_mask          = 0xFFFFU,      /**< Relative card address (16-bit). */
  k_sd_read_bl_mask      = 0xFU,         /**< READ_BL_LEN (4-bit).            */
  k_sd_csize_v2_lo_mask  = 0x0000003FUL, /**< CSD v2 C_SIZE byte-2 bits.      */
  k_sd_csize_v2_mid_mask = 0x0000FFFFUL, /**< CSD v2 C_SIZE byte-1 bits.      */
  k_sd_csize_v2_mask     = 0x003FFFFFUL, /**< CSD v2 C_SIZE (22-bit).         */
  k_sd_csize_v1_shift    = 10U,          /**< CSD v1 C_SIZE high-bit shift.   */
  k_sd_csize_v1_lo_shift = 22U,          /**< CSD v1 C_SIZE low-bit shift.    */
  k_sd_csize_v1_lo_mask  = 0x000003FFUL, /**< CSD v1 C_SIZE low 10 bits.      */
  k_sd_csize_v1_mask     = 0xFFFU,       /**< CSD v1 C_SIZE (12-bit).         */
  k_sd_csize_mult_shift  = 7U,           /**< C_SIZE_MULT shift.              */
  k_sd_csize_mult_mask   = 0x7U,         /**< C_SIZE_MULT (3-bit).            */
} sd_csd_field_t;

/**
 * @enum ra8_sdcard_cmd_t
 * @brief SD Physical Layer command indices used by ::ra8_sdcard_init.
 *
 * @details
 * Matches the SD Physical Layer Specification v6.00, Table 4-24
 * "Card commands". Values are bare command indices; SDHI infers the
 * response type and data direction from the index for the standard
 * commands used here.
 */
typedef enum : uint8_t {
  k_ra8_sdcard_cmd0_go_idle        = 0U,  /**< CMD0  GO_IDLE_STATE      */
  k_ra8_sdcard_cmd2_all_send_cid   = 2U,  /**< CMD2  ALL_SEND_CID       */
  k_ra8_sdcard_cmd3_send_rca       = 3U,  /**< CMD3  SEND_RELATIVE_ADDR */
  k_ra8_sdcard_cmd7_select_card    = 7U,  /**< CMD7  SELECT_CARD        */
  k_ra8_sdcard_cmd8_send_if_cond   = 8U,  /**< CMD8  SEND_IF_COND       */
  k_ra8_sdcard_cmd9_send_csd       = 9U,  /**< CMD9  SEND_CSD           */
  k_ra8_sdcard_cmd55_app_cmd       = 55U, /**< CMD55 APP_CMD prefix     */
  k_ra8_sdcard_acmd41_send_op_cond = 41U, /**< ACMD41 SD_SEND_OP_COND   */
} ra8_sdcard_cmd_t;

/**
 * @enum ra8_sdcard_proto_t
 * @brief SD protocol-level constants.
 *
 * @details
 * Magic numbers from the SD Physical Layer Specification v6.00. The
 * CMD8 check-pattern is fixed at 0xAA and the voltage-supplied field
 * is set to b0001 ("2.7-3.6 V") -- the only voltage range the host
 * supports.
 */
typedef enum : uint32_t {
  k_ra8_sdcard_cmd8_pattern       = 0x000001AAUL, /**< VHS=0001, check=0xAA                */
  k_ra8_sdcard_cmd8_pattern_mask  = 0x00000FFFUL, /**< low 12 bits echoed                  */
  k_ra8_sdcard_acmd41_hcs         = 0x40000000UL, /**< HCS bit -- request HC support       */
  k_ra8_sdcard_acmd41_voltage_win = 0x00FF8000UL, /**< OCR voltage window 2.7-3.6 V        */
  k_ra8_sdcard_ocr_busy_done_mask = 0x80000000UL, /**< OCR.busy clears when ready          */
  k_ra8_sdcard_ocr_ccs_mask       = 0x40000000UL, /**< OCR.CCS = 1 -> SDHC/SDXC            */
  k_ra8_sdcard_init_retry_max     = 1000U,        /**< ACMD41 retry budget                 */
  k_ra8_sdcard_default_clk_div    = 0x00000004UL, /**< CLKSEL=4 -> 25 MHz at 200 MHz PCLKB */
} ra8_sdcard_proto_t;

/**
 * @enum ra8_sdcard_csd_t
 * @brief CSD register decoding constants.
 *
 * @details
 * SD CSD layout (Section 5.3 of the Physical Layer Spec):
 *   - CSD_STRUCTURE lives in CSD[127:126] (top 2 bits of word [3]).
 *   - V2 (SDHC/SDXC): C_SIZE in CSD[69:48] (22 bits), capacity =
 *     (C_SIZE + 1) * 1024 blocks of 512 bytes (i.e. 512 KiB units).
 *   - V1 (SDSC): C_SIZE in CSD[73:62] (12 bits), C_SIZE_MULT in
 *     CSD[49:47] (3 bits), READ_BL_LEN in CSD[83:80] (4 bits);
 *     capacity in bytes = (C_SIZE+1) * 2^(C_SIZE_MULT+2) * 2^READ_BL_LEN.
 *
 * The SDHI hardware drops the 8-bit CRC and 1-bit end bit, so what
 * shows up in ``out_rsp[0..3]`` is bits [127:8] left-shifted by 8.
 * The decoder below expects that 120-bit format -- matching FSP
 * ``r_sdhi_csd_extract``.
 */
typedef enum : uint32_t {
  k_ra8_sdcard_csd_v2_blocks_per_unit = 1024UL,    /**< 512 KiB / 512 B               */
  k_ra8_sdcard_sdhc_threshold_blocks  = 4194304UL, /**< 2 GiB / 512 B = SDXC boundary */
} ra8_sdcard_csd_t;

/**
 * @struct ra8_sdcard_state_t
 * @brief Internal per-driver state.
 *
 * @details
 * Single-instance driver: only one card is supported at a time.
 * Holds the SDHI instance index plus the protocol-level state
 * captured during init (RCA, capacity, card type). ``initialized``
 * gates every public API except ::ra8_sdcard_init / ::ra8_sdcard_deinit.
 */
typedef struct {
  uint32_t               capacity_blocks; /**< 512-byte block count, 0 if not init'd */
  uint16_t               rca;             /**< Card-assigned relative address        */
  ra8_sdcard_card_type_t type;            /**< SDSC / SDHC / SDXC                    */
  uint8_t                instance;        /**< SDHI instance index                   */
  uint8_t                initialized;     /**< Non-zero once init succeeded          */
} ra8_sdcard_state_t;

/** @brief Module-private state. */
static ra8_sdcard_state_t s_sdcard;

/* Issue ACMD41 with the host capacity-support bit set and loop -- see implementation for details. */
RA8_INTERNAL
static ra8_err_t internal_run_acmd41(uint8_t instance, uint32_t* out_ocr)
{
  RA8_CHECK_NULL_PTR(out_ocr, s_tag, "ocr");

  uint32_t rsp[4] = {0U, 0U, 0U, 0U};
  uint32_t ocr    = 0U;
  uint8_t  done   = 0U;
  for (uint32_t i = 0U; i < (uint32_t)k_ra8_sdcard_init_retry_max; ++i) {
    /* HUM Ch 47.2.1 "SD_CMD : Command Type Register" p 3125 */
    /* CMD55 prefix is required before every ACMDxx. */
    const ra8_err_t e55 =
      ra8_sdhi_send_command(instance, (uint32_t)k_ra8_sdcard_cmd55_app_cmd, 0U, rsp);
    if (e55 != k_ra8_ok) {
      return e55;
    }
    /* HUM Ch 47.2.2 "SD_ARG : SD Command Argument" p 3128 */
    /* HCS=1 (host supports SDHC/SDXC) | voltage window 2.7-3.6 V. */
    const uint32_t arg =
      (uint32_t)k_ra8_sdcard_acmd41_hcs | (uint32_t)k_ra8_sdcard_acmd41_voltage_win;
    const ra8_err_t e41 =
      ra8_sdhi_send_command(instance, (uint32_t)k_ra8_sdcard_acmd41_send_op_cond, arg, rsp);
    if (e41 != k_ra8_ok) {
      return e41;
    }
    ocr = rsp[0];
    if ((ocr & (uint32_t)k_ra8_sdcard_ocr_busy_done_mask) != 0U) {
      done = 1U;
      break;
    }
  }
  if (done == 0U) {
    return k_ra8_err_hw_init_failed;
  }
  *out_ocr = ocr;
  return k_ra8_ok;
}

/**
 * @brief Decode a CSD response (v1 or v2) into a capacity-in-blocks count.
 *
 * @param[in]  rsp        CSD register contents from CMD9 (rsp[0..3]).
 * @param[out] out_blocks Receives the 512-byte block count.
 *
 * @details
 * The SDHI hardware strips the CRC + end bit, so each rsp word holds
 * the 32-bit CSD slice already shifted to the standard layout.
 * ``rsp[3]`` carries the high 32 bits; CSD_STRUCTURE sits at
 * ``rsp[3][31:30]``.
 *
 * @pre  ``rsp`` is non-NULL.
 * @pre  ``out_blocks`` is non-NULL.
 * @post On v2 success ``*out_blocks = (C_SIZE + 1) * 1024``.
 *
 * @since 0.1.0
 *
 * @return Result code or value; see implementation.
 * @retval 0 Success or default value.
 * @post Side effects bounded to documented state.
 * @note Not thread-safe unless documented otherwise.
 */
RA8_INTERNAL
static ra8_err_t internal_decode_csd(const uint32_t* rsp, uint32_t* out_blocks)
{
  RA8_CHECK_NULL_PTR(rsp, s_tag, "rsp");
  RA8_CHECK_NULL_PTR(out_blocks, s_tag, "blocks");

  /* CSD_STRUCTURE is rsp[3][31:30]. SDHI shifts the response left by
   * 8 (CRC drop), so the architectural CSD[127:126] lands at
   * rsp[3][31:30] -- the same place FSP r_sdhi.c reads it from. */
  const uint8_t csd_structure = (uint8_t)((rsp[3] >> k_sd_csd_struct_shift) & 0x3U);
  if (csd_structure == 1U) {
    /* CSD v2 (SDHC/SDXC): C_SIZE is bits [69:48], i.e. rsp[1][29:8]
     * once you account for the CRC shift. The 22-bit field needs the
     * low 6 bits of rsp[2] OR'd with the high 16 bits of rsp[1]. */
    const uint32_t c_size =
      (((rsp[2] & k_sd_csize_v2_lo_mask) << 16U) | ((rsp[1] >> 16U) & k_sd_csize_v2_mid_mask)) &
      k_sd_csize_v2_mask;
    *out_blocks = (c_size + 1U) * (uint32_t)k_ra8_sdcard_csd_v2_blocks_per_unit;
    return k_ra8_ok;
  }
  if (csd_structure == 0U) {
    /* CSD v1 (SDSC): READ_BL_LEN [83:80], C_SIZE [73:62],
     * C_SIZE_MULT [49:47]. Compute capacity in bytes then divide by
     * 512 to land on the block count. */
    const uint8_t  read_bl_len = (uint8_t)((rsp[2] >> 16U) & k_sd_read_bl_mask);
    const uint16_t c_size =
      (uint16_t)((((rsp[2] & 0x00000003UL) << k_sd_csize_v1_shift) |
                  ((rsp[1] >> k_sd_csize_v1_lo_shift) & k_sd_csize_v1_lo_mask)) &
                 k_sd_csize_v1_mask);
    const uint8_t c_size_mult = (uint8_t)((rsp[1] >> k_sd_csize_mult_shift) & k_sd_csize_mult_mask);
    const uint32_t mult       = 1U << ((uint32_t)c_size_mult + 2U);
    const uint32_t block_len  = 1U << (uint32_t)read_bl_len;
    const uint32_t blocknr    = ((uint32_t)c_size + 1U) * mult;
    /* Convert to 512-byte sectors. */
    *out_blocks = (blocknr * block_len) / k_sd_block_size_bytes;
    return k_ra8_ok;
  }
  return k_ra8_err_hw_init_failed;
}

/**
 * @brief Drive CMD0 + CMD8 to take the card from power-on into idle.
 *
 * @details
 * HUM Ch 47.2.1 SD_CMD command sequence. CMD0 has no response but
 * still drives the RSPEND handshake; CMD8 echoes the low 12 bits of
 * the argument back through R7. Mismatch indicates a v1 (no-CMD8)
 * card or no card present.
 *
 * @param[in] instance SDHI instance index.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok                   Identification echo matched.
 * @retval k_ra8_err_hw_init_failed   CMD8 pattern mismatch.
 *
 * @pre ra8_sdhi_init() succeeded for ``instance``.
 * @pre Single-threaded init context.
 * @post Card is in IDLE with v2 interface confirmed.
 * @post No host-side global state is modified.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_sdcard_identify(uint8_t instance)
{
  uint32_t rsp[4] = {0U, 0U, 0U, 0U};

  /* HUM Ch 47.2.1 "SD_CMD : Command Type Register" p 3125 */
  /* CMD0 GO_IDLE_STATE -- no response, but send_command still polls
   * RSPEND so the host stays in lockstep with the bus state machine. */
  const ra8_err_t e0 =
    ra8_sdhi_send_command(instance, (uint32_t)k_ra8_sdcard_cmd0_go_idle, 0U, rsp);
  RA8_RETURN_ON_ERROR(e0, s_tag, "cmd0"); /* GCOVR_EXCL_BR_LINE */

  /* HUM Ch 47.2.5 "SD_RSP10 : Response Register 10" p 3132 */
  /* CMD8 SEND_IF_COND -- echoes the low 12 bits of the argument back
   * in R7. Mismatch means a v1 (no-CMD8) card or no card at all. */
  const ra8_err_t e8 = ra8_sdhi_send_command(instance,
                                             (uint32_t)k_ra8_sdcard_cmd8_send_if_cond,
                                             (uint32_t)k_ra8_sdcard_cmd8_pattern,
                                             rsp);
  RA8_RETURN_ON_ERROR(e8, s_tag, "cmd8"); /* GCOVR_EXCL_BR_LINE */
  if ((rsp[0] & (uint32_t)k_ra8_sdcard_cmd8_pattern_mask) !=
      ((uint32_t)k_ra8_sdcard_cmd8_pattern & (uint32_t)k_ra8_sdcard_cmd8_pattern_mask)) {
    return k_ra8_err_hw_init_failed;
  }
  return k_ra8_ok;
}

/**
 * @brief Run CMD2 + CMD3 so the card publishes its relative address.
 *
 * @details
 * SD Physical Layer Spec Section 4.2.2 identification flow: CMD2
 * ALL_SEND_CID broadcasts the 136-bit CID (which this host does not
 * decode), then CMD3 SEND_RELATIVE_ADDR asks the card to publish the
 * 16-bit RCA in ``rsp[0][31:16]``.
 *
 * @param[in]  instance SDHI instance index.
 * @param[out] out_rca  Receives the card-published relative address.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok  RCA published and captured.
 *
 * @pre Card has cleared ACMD41 (ready state).
 * @pre ``out_rca`` is non-NULL.
 * @post ``*out_rca`` holds the published RCA on success.
 * @post On error the card may be in any state; caller deinits SDHI.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_sdcard_publish_rca(uint8_t instance, uint16_t* out_rca)
{
  uint32_t rsp[4] = {0U, 0U, 0U, 0U};

  /* HUM Ch 47.2.5 "SD_RSP10..76 : Response Registers" p 3132 */
  /* CMD2 ALL_SEND_CID -- 136-bit R2; we don't decode the CID. */
  const ra8_err_t e2 =
    ra8_sdhi_send_command(instance, (uint32_t)k_ra8_sdcard_cmd2_all_send_cid, 0U, rsp);
  RA8_RETURN_ON_ERROR(e2, s_tag, "cmd2"); /* GCOVR_EXCL_BR_LINE */

  /* CMD3 SEND_RELATIVE_ADDR -- card publishes its RCA in rsp[0][31:16]. */
  const ra8_err_t e3 =
    ra8_sdhi_send_command(instance, (uint32_t)k_ra8_sdcard_cmd3_send_rca, 0U, rsp);
  RA8_RETURN_ON_ERROR(e3, s_tag, "cmd3"); /* GCOVR_EXCL_BR_LINE */
  *out_rca = (uint16_t)((rsp[0] >> 16U) & k_sd_rca_mask);
  return k_ra8_ok;
}

/**
 * @brief Publish the RCA, decode the CSD, and put the card in TRAN.
 *
 * @details
 * HUM Ch 47.2.5 response registers / SD Physical Layer Spec
 * Section 4.7. Delegates CMD2/CMD3 to ::internal_sdcard_publish_rca,
 * then runs CMD9 (CSD) and CMD7 with the published RCA to move the
 * card into TRAN.
 *
 * @param[in]  instance    SDHI instance index.
 * @param[out] out_rca     Receives the card's relative address.
 * @param[out] out_blocks  Receives capacity in 512-byte blocks.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok                   Card parked in TRAN.
 * @retval k_ra8_err_hw_init_failed   CSD decode rejected the layout.
 *
 * @pre Card has cleared ACMD41 (ready state).
 * @pre Both output pointers are non-NULL.
 * @post Card is in TRAN; ``*out_rca`` / ``*out_blocks`` populated.
 * @post On error the card may be in any state; caller deinits SDHI.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t
internal_sdcard_publish_and_select(uint8_t instance, uint16_t* out_rca, uint32_t* out_blocks)
{
  uint16_t        rca     = 0U;
  const ra8_err_t rca_err = internal_sdcard_publish_rca(instance, &rca);
  if (rca_err != k_ra8_ok) {
    return rca_err;
  }
  const uint32_t rca_arg = ((uint32_t)rca) << 16U;
  uint32_t       rsp[4]  = {0U, 0U, 0U, 0U};

  /* CMD9 SEND_CSD -- arg = RCA<<16; R2 holds the 128-bit CSD. */
  const ra8_err_t e9 =
    ra8_sdhi_send_command(instance, (uint32_t)k_ra8_sdcard_cmd9_send_csd, rca_arg, rsp);
  RA8_RETURN_ON_ERROR(e9, s_tag, "cmd9"); /* GCOVR_EXCL_BR_LINE */

  const ra8_err_t dec_err = internal_decode_csd(rsp, out_blocks);
  if (dec_err != k_ra8_ok) {
    return dec_err;
  }

  /* CMD7 SELECT_CARD -- arg = RCA<<16 to put this card into TRAN. */
  const ra8_err_t e7 =
    ra8_sdhi_send_command(instance, (uint32_t)k_ra8_sdcard_cmd7_select_card, rca_arg, rsp);
  RA8_RETURN_ON_ERROR(e7, s_tag, "cmd7"); /* GCOVR_EXCL_BR_LINE */

  *out_rca = rca;
  return k_ra8_ok;
}

/**
 * @brief Classify card type from OCR.CCS and decoded block count.
 *
 * @details
 * SDA marketing tiers: SDSC < 2 GiB (byte-addressed), SDHC up to
 * 32 GiB, SDXC above. The exact cutover is fuzzy; treat anything
 * above a generous 32 GB as SDXC and the rest as SDHC.
 *
 * @param[in] high_capacity Non-zero if OCR.CCS = 1.
 * @param[in] blocks        Capacity in 512-byte blocks.
 *
 * @return ::ra8_sdcard_card_type_t classification.
 * @retval k_ra8_sdcard_type_sdsc  high_capacity == 0 (byte-addressed card).
 * @retval k_ra8_sdcard_type_sdhc  high_capacity != 0 and blocks <= 32 GiB threshold.
 * @retval k_ra8_sdcard_type_sdxc  high_capacity != 0 and blocks > 32 GiB threshold.
 *
 * @pre OCR has been validated by ACMD41.
 * @pre ``blocks`` is the decoded CSD block count (512-byte units).
 * @post Return value is one of the three documented types.
 * @post No global or module-private state is mutated.
 *
 * @note Pure function; reentrant and ISR-safe.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_sdcard_card_type_t internal_classify(uint8_t high_capacity, uint32_t blocks)
{
  if (high_capacity == 0U) {
    return k_ra8_sdcard_type_sdsc;
  }
  if (blocks > (uint32_t)k_ra8_sdcard_sdhc_threshold_blocks * 16U) {
    return k_ra8_sdcard_type_sdxc;
  }
  return k_ra8_sdcard_type_sdhc;
}

/**
 * @brief Negotiate the requested SD data-bus width after card selection.
 *
 * @details
 * Only the SD 4-bit path is negotiable from the SD-card driver; 1-bit
 * needs no action and any other request (including the eMMC-only 8-bit
 * width) is treated as "stay 1-bit". Widening is a best-effort
 * optimization: the card was already identified and selected at 1-bit,
 * so ANY ACMD6 failure (a card that declines, or a bus timeout) simply
 * falls back to the proven 1-bit mode and is logged -- it never fails
 * ::ra8_sdcard_init, which is why this returns ``void``.
 *
 * @param[in] instance SDHI instance index.
 * @param[in] width    Requested bus width from ::ra8_sdcard_cfg_t.
 * @param[in] rca      Card relative address published by CMD3.
 *
 * @pre  Card is in TRAN state with a valid ``rca``.
 * @pre  ::ra8_sdhi_init has been called for ``instance``.
 * @post On a clean ACMD6 the host SD_OPTION reflects the 4-bit width.
 * @post On any ACMD6 failure the host is left at the 1-bit default.
 *
 * @note Not thread-safe; single-threaded init context.
 * @since 0.1.0
 */
RA8_INTERNAL
static void
internal_sdcard_negotiate_width(uint8_t instance, ra8_sdhi_bus_width_t width, uint16_t rca)
{
  if (width != k_ra8_sdhi_bus_width_4bit) {
    return;
  }
  const ra8_err_t we = ra8_sdhi_set_bus_width_4bit(instance, rca);
  if (we != k_ra8_ok) {
    /* Best-effort: fall back to the proven 1-bit mode. */
    ra8_log_info_val(s_tag, "4-bit negotiation failed; staying 1-bit", (uint32_t)we);
  }
}

/**
 * @brief Take an identified SDHI card from power-on to TRAN state.
 *
 * @details
 * Brings up the SDHI module, then runs the SD Physical Layer chain:
 * CMD0/CMD8 identification, the ACMD41 op-cond loop (capturing
 * OCR.CCS), then CMD2/CMD3/CMD9/CMD7 to publish the RCA, decode the
 * CSD, and select the card. On any failure after the module came up
 * the SDHI module is deinit'd, so the caller simply propagates the
 * error.
 *
 * @param[in]  instance          SDHI instance index.
 * @param[out] out_rca           Receives the published relative address.
 * @param[out] out_blocks        Receives capacity in 512-byte blocks.
 * @param[out] out_high_capacity Receives 1 when OCR.CCS = 1 (SDHC/SDXC).
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok                 Card parked in TRAN, outputs valid.
 * @retval k_ra8_err_hw_init_failed Identification / CSD decode failed.
 *
 * @pre The SDHI module for ``instance`` is not yet initialized.
 * @pre All three output pointers are non-NULL.
 * @post On success the card is in TRAN and every output is populated.
 * @post On error SDHI has been (or remained) deinitialized.
 *
 * @note Not thread-safe; single-threaded init context.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_sdcard_card_online(uint8_t   instance,
                                             uint16_t* out_rca,
                                             uint32_t* out_blocks,
                                             uint8_t*  out_high_capacity)
{
  /* HUM Ch 47.1 "SDHI Overview" p 3122 */ /* module bring-up first. */
  const ra8_err_t hw_err = ra8_sdhi_init(instance);
  RA8_RETURN_ON_ERROR(hw_err, s_tag, "sdhi_init"); /* GCOVR_EXCL_BR_LINE */

  const ra8_err_t id_err = internal_sdcard_identify(instance);
  if (id_err != k_ra8_ok) {
    (void)ra8_sdhi_deinit(instance);
    return id_err;
  }

  /* ACMD41 op-cond loop -- card moves from idle to ready. */
  uint32_t        ocr = 0U;
  const ra8_err_t e41 = internal_run_acmd41(instance, &ocr);
  if (e41 != k_ra8_ok) {
    (void)ra8_sdhi_deinit(instance);
    return e41;
  }
  *out_high_capacity = ((ocr & (uint32_t)k_ra8_sdcard_ocr_ccs_mask) != 0U) ? 1U : 0U;

  const ra8_err_t pub_err = internal_sdcard_publish_and_select(instance, out_rca, out_blocks);
  if (pub_err != k_ra8_ok) {
    (void)ra8_sdhi_deinit(instance);
    return pub_err;
  }
  return k_ra8_ok;
}

ra8_err_t ra8_sdcard_init(const ra8_sdcard_cfg_t* cfg)
{
  RA8_CHECK_NULL_PTR(cfg, s_tag, "cfg");
  if (s_sdcard.initialized != 0U) {
    return k_ra8_err_invalid_state;
  }

  uint16_t        rca           = 0U;
  uint32_t        blocks        = 0U;
  uint8_t         high_capacity = 0U;
  const ra8_err_t on_err =
    internal_sdcard_card_online(cfg->instance, &rca, &blocks, &high_capacity);
  if (on_err != k_ra8_ok) {
    return on_err;
  }

  /* Optional bus-width widening (best-effort, never fatal) while still
   * at the identification clock, before the default-speed clock bump. */
  internal_sdcard_negotiate_width(cfg->instance, cfg->bus_width, rca);

  /* HUM Ch 47.2.18 "SD_CLK_CTRL : SD Clock Control Register" p 3145 */
  /* Bump the bus from the 400 kHz identification rate to default
   * speed. The SDR50 / DDR50 negotiation lives in the future
   * ::ra8_sdcard_set_speed extension. */
  const ra8_err_t eclk = ra8_sdhi_set_clock(cfg->instance, (uint32_t)k_ra8_sdcard_default_clk_div);
  RA8_RETURN_ON_ERROR(eclk, s_tag, "set_clock"); /* GCOVR_EXCL_BR_LINE */

  s_sdcard.instance        = cfg->instance;
  s_sdcard.rca             = rca;
  s_sdcard.capacity_blocks = blocks;
  s_sdcard.type            = internal_classify(high_capacity, blocks);
  s_sdcard.initialized     = 1U;

  ra8_log_info_val(s_tag, "sdcard init blocks", blocks);
  return k_ra8_ok;
}

/**
 * @brief Translate a logical block address into the SD on-card argument.
 *
 * @details
 * SDSC cards expect a byte address, SDHC/SDXC cards expect a block
 * index. The host always speaks in block indices; this helper applies
 * the byte-multiplication where it is needed.
 *
 * @param[in] lba Logical block address.
 * @return Argument value to load into SD_ARG.
 *
 * @since 0.1.0
 *
 * @retval 0 Success or default value.
 * @pre Module has been initialized.
 * @pre Caller has validated arguments.
 * @post Side effects bounded to documented state.
 * @post State reflects operation result.
 * @note Not thread-safe unless documented otherwise.
 */
RA8_INTERNAL
static uint32_t internal_to_card_address(uint32_t lba)
{
  if (s_sdcard.type == k_ra8_sdcard_type_sdsc) {
    return lba * k_sd_block_size_bytes;
  }
  return lba;
}

ra8_err_t ra8_sdcard_read_blocks(uint32_t lba, uint8_t* buf, uint32_t count)
{
  RA8_CHECK_NULL_PTR(buf, s_tag, "buf");
  if (count == 0U) {
    return k_ra8_err_invalid_arg;
  }
  if (s_sdcard.initialized == 0U) {
    return k_ra8_err_invalid_state;
  }
  if ((lba + count) > s_sdcard.capacity_blocks) {
    return k_ra8_err_out_of_range;
  }
  const uint32_t card_lba = internal_to_card_address(lba);
  return ra8_sdhi_read_block(s_sdcard.instance, card_lba, buf, count);
}

ra8_err_t ra8_sdcard_write_blocks(uint32_t lba, const uint8_t* buf, uint32_t count)
{
  RA8_CHECK_NULL_PTR(buf, s_tag, "buf");
  if (count == 0U) {
    return k_ra8_err_invalid_arg;
  }
  if (s_sdcard.initialized == 0U) {
    return k_ra8_err_invalid_state;
  }
  if ((lba + count) > s_sdcard.capacity_blocks) {
    return k_ra8_err_out_of_range;
  }
  const uint32_t card_lba = internal_to_card_address(lba);
  return ra8_sdhi_write_block(s_sdcard.instance, card_lba, buf, count);
}

ra8_err_t ra8_sdcard_get_capacity(uint32_t* out_blocks)
{
  RA8_CHECK_NULL_PTR(out_blocks, s_tag, "out_blocks");
  if (s_sdcard.initialized == 0U) {
    return k_ra8_err_invalid_state;
  }
  *out_blocks = s_sdcard.capacity_blocks;
  return k_ra8_ok;
}

ra8_err_t ra8_sdcard_get_type(ra8_sdcard_card_type_t* out_type)
{
  RA8_CHECK_NULL_PTR(out_type, s_tag, "out_type");
  if (s_sdcard.initialized == 0U) {
    return k_ra8_err_invalid_state;
  }
  *out_type = s_sdcard.type;
  return k_ra8_ok;
}

ra8_err_t ra8_sdcard_deinit(void)
{
  if (s_sdcard.initialized == 0U) {
    return k_ra8_ok;
  }
  const uint8_t inst       = s_sdcard.instance;
  s_sdcard.initialized     = 0U;
  s_sdcard.capacity_blocks = 0U;
  s_sdcard.rca             = 0U;
  s_sdcard.type            = k_ra8_sdcard_type_unknown;
  const ra8_err_t err      = ra8_sdhi_deinit(inst);
  if (err != k_ra8_ok) {
    /* ra8_sdhi_deinit fails only if ra8_mstp_disable times out on the MSTP
     * readback loop. Reached on host by arming the ra8_sim_mmio seam on the
     * SDHI module's MSTPCR register so that readback never settles. */
    return k_ra8_err_invalid_state;
  }
  return k_ra8_ok;
}
