/**
 * @file ra_sdcard.c
 * @brief SD card driver implementation -- SD Physical Layer state machine
 *
 * @par Tag
 * [Ring 3 / HAL] {World: NS}
 *
 * @details
 * Implements the SD Physical Layer initialization sequence on top of
 * the lower-level ::ra_sdhi register driver:
 *
 *   CMD0  -> CMD8 -> ACMD41 (loop) -> CMD2 -> CMD3 -> CMD9 -> CMD7
 *
 * After the card is parked in TRAN state the bus clock is bumped from
 * the 400 kHz identification rate to 25 MHz default-speed by way of
 * ::ra_sdhi_set_clock. Block I/O calls fall through to the polled
 * ::ra_sdhi_read_block / ::ra_sdhi_write_block primitives.
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

#include "ra_sdcard.h"

#include <stdint.h>

#include "ra_check.h"
#include "ra_err.h"
#include "ra_log.h"
#include "ra_sdhi.h"

/* NOLINTBEGIN(readability-magic-numbers,readability-function-size,readability-function-cognitive-complexity) */

/** @brief Module log tag. */
static const char* s_tag = "SDCARD";

/**
 * @enum ra_sdcard_cmd_t
 * @brief SD Physical Layer command indices used by ::ra_sdcard_init.
 *
 * @details
 * Matches the SD Physical Layer Specification v6.00, Table 4-24
 * "Card commands". Values are bare command indices; SDHI infers the
 * response type and data direction from the index for the standard
 * commands used here.
 */
typedef enum : uint8_t {
  k_ra_sdcard_cmd0_go_idle           = 0U,  /**< CMD0  GO_IDLE_STATE */
  k_ra_sdcard_cmd2_all_send_cid      = 2U,  /**< CMD2  ALL_SEND_CID */
  k_ra_sdcard_cmd3_send_rca          = 3U,  /**< CMD3  SEND_RELATIVE_ADDR */
  k_ra_sdcard_cmd7_select_card       = 7U,  /**< CMD7  SELECT_CARD */
  k_ra_sdcard_cmd8_send_if_cond      = 8U,  /**< CMD8  SEND_IF_COND */
  k_ra_sdcard_cmd9_send_csd          = 9U,  /**< CMD9  SEND_CSD */
  k_ra_sdcard_cmd55_app_cmd          = 55U, /**< CMD55 APP_CMD prefix */
  k_ra_sdcard_acmd41_send_op_cond    = 41U, /**< ACMD41 SD_SEND_OP_COND */
} ra_sdcard_cmd_t;

/**
 * @enum ra_sdcard_proto_t
 * @brief SD protocol-level constants.
 *
 * @details
 * Magic numbers from the SD Physical Layer Specification v6.00. The
 * CMD8 check-pattern is fixed at 0xAA and the voltage-supplied field
 * is set to b0001 ("2.7-3.6 V") -- the only voltage range the host
 * supports.
 */
typedef enum : uint32_t {
  k_ra_sdcard_cmd8_pattern        = 0x000001AAUL, /**< VHS=0001, check=0xAA */
  k_ra_sdcard_cmd8_pattern_mask   = 0x00000FFFUL, /**< low 12 bits echoed */
  k_ra_sdcard_acmd41_hcs          = 0x40000000UL, /**< HCS bit -- request HC support */
  k_ra_sdcard_acmd41_voltage_win  = 0x00FF8000UL, /**< OCR voltage window 2.7-3.6 V */
  k_ra_sdcard_ocr_busy_done_mask  = 0x80000000UL, /**< OCR.busy clears when ready */
  k_ra_sdcard_ocr_ccs_mask        = 0x40000000UL, /**< OCR.CCS = 1 -> SDHC/SDXC */
  k_ra_sdcard_init_retry_max      = 1000U,        /**< ACMD41 retry budget */
  k_ra_sdcard_default_clk_div     = 0x00000004UL, /**< CLKSEL=4 -> 25 MHz at 200 MHz PCLKB */
} ra_sdcard_proto_t;

/**
 * @enum ra_sdcard_csd_t
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
  k_ra_sdcard_csd_v2_blocks_per_unit = 1024UL, /**< 512 KiB / 512 B */
  k_ra_sdcard_sdhc_threshold_blocks  = 4194304UL, /**< 2 GiB / 512 B = SDXC boundary */
} ra_sdcard_csd_t;

/**
 * @struct ra_sdcard_state_t
 * @brief Internal per-driver state.
 *
 * @details
 * Single-instance driver: only one card is supported at a time.
 * Holds the SDHI instance index plus the protocol-level state
 * captured during init (RCA, capacity, card type). ``initialised``
 * gates every public API except ::ra_sdcard_init / ::ra_sdcard_deinit.
 */
typedef struct {
  uint32_t              capacity_blocks; /**< 512-byte block count, 0 if not init'd */
  uint16_t              rca;             /**< Card-assigned relative address */
  ra_sdcard_card_type_t type;            /**< SDSC / SDHC / SDXC */
  uint8_t               instance;        /**< SDHI instance index */
  uint8_t               initialised;     /**< Non-zero once init succeeded */
} ra_sdcard_state_t;

/** @brief Module-private state. */
static ra_sdcard_state_t s_sdcard;

/**
 * @brief Issue ACMD41 with the host capacity-support bit set and loop
 *        until the card clears the busy bit in OCR.
 *
 * @param[in]  instance SDHI instance.
 * @param[out] out_ocr  Receives the final OCR value.
 *
 * @return ra_err_t Error code.
 * @retval k_ra_ok                Card transitioned out of busy.
 * @retval k_ra_err_hw_init_failed Retry budget exhausted.
 *
 * @pre  ``out_ocr`` is non-NULL.
 * @pre  ::ra_sdhi_init has been called.
 * @post On success ``*out_ocr & 0x80000000`` is non-zero.
 *
 * @since 0.1.0
 */
static ra_err_t internal_run_acmd41(uint8_t instance, uint32_t* out_ocr)
{
  RA_CHECK_NULL_PTR(out_ocr, s_tag, "ocr");

  uint32_t rsp[4]   = {0U, 0U, 0U, 0U};
  uint32_t ocr      = 0U;
  uint8_t  done     = 0U;
  for (uint32_t i = 0U; i < (uint32_t)k_ra_sdcard_init_retry_max; ++i) {
    /* HUM Ch 47.2.1 "SD_CMD : Command Type Register" p 3125 */
    /* CMD55 prefix is required before every ACMDxx. */
    const ra_err_t e55 =
      ra_sdhi_send_command(instance, (uint32_t)k_ra_sdcard_cmd55_app_cmd, 0U, rsp);
    if (e55 != k_ra_ok) {
      return e55;
    }
    /* HUM Ch 47.2.2 "SD_ARG : SD Command Argument" p 3128 */
    /* HCS=1 (host supports SDHC/SDXC) | voltage window 2.7-3.6 V. */
    const uint32_t arg =
      (uint32_t)k_ra_sdcard_acmd41_hcs | (uint32_t)k_ra_sdcard_acmd41_voltage_win;
    const ra_err_t e41 =
      ra_sdhi_send_command(instance, (uint32_t)k_ra_sdcard_acmd41_send_op_cond, arg, rsp);
    if (e41 != k_ra_ok) {
      return e41;
    }
    ocr = rsp[0];
    if ((ocr & (uint32_t)k_ra_sdcard_ocr_busy_done_mask) != 0U) {
      done = 1U;
      break;
    }
  }
  if (done == 0U) {
    return k_ra_err_hw_init_failed;
  }
  *out_ocr = ocr;
  return k_ra_ok;
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
 */
static ra_err_t internal_decode_csd(const uint32_t* rsp, uint32_t* out_blocks)
{
  RA_CHECK_NULL_PTR(rsp, s_tag, "rsp");
  RA_CHECK_NULL_PTR(out_blocks, s_tag, "blocks");

  /* CSD_STRUCTURE is rsp[3][31:30]. SDHI shifts the response left by
   * 8 (CRC drop), so the architectural CSD[127:126] lands at
   * rsp[3][31:30] -- the same place FSP r_sdhi.c reads it from. */
  const uint8_t csd_structure = (uint8_t)((rsp[3] >> 30U) & 0x3U);
  if (csd_structure == 1U) {
    /* CSD v2 (SDHC/SDXC): C_SIZE is bits [69:48], i.e. rsp[1][29:8]
     * once you account for the CRC shift. The 22-bit field needs the
     * low 6 bits of rsp[2] OR'd with the high 16 bits of rsp[1]. */
    const uint32_t c_size =
      (((rsp[2] & 0x0000003FUL) << 16U) | ((rsp[1] >> 16U) & 0x0000FFFFUL)) & 0x003FFFFFUL;
    *out_blocks = (c_size + 1U) * (uint32_t)k_ra_sdcard_csd_v2_blocks_per_unit;
    return k_ra_ok;
  }
  if (csd_structure == 0U) {
    /* CSD v1 (SDSC): READ_BL_LEN [83:80], C_SIZE [73:62],
     * C_SIZE_MULT [49:47]. Compute capacity in bytes then divide by
     * 512 to land on the block count. */
    const uint8_t  read_bl_len = (uint8_t)((rsp[2] >> 16U) & 0xFU);
    const uint16_t c_size =
      (uint16_t)((((rsp[2] & 0x00000003UL) << 10U) | ((rsp[1] >> 22U) & 0x000003FFUL)) & 0xFFFU);
    const uint8_t  c_size_mult = (uint8_t)((rsp[1] >> 7U) & 0x7U);
    const uint32_t mult        = 1UL << ((uint32_t)c_size_mult + 2U);
    const uint32_t block_len   = 1UL << (uint32_t)read_bl_len;
    const uint32_t blocknr     = ((uint32_t)c_size + 1U) * mult;
    /* Convert to 512-byte sectors. */
    *out_blocks = (blocknr * block_len) / 512U;
    return k_ra_ok;
  }
  return k_ra_err_hw_init_failed;
}

ra_err_t ra_sdcard_init(const ra_sdcard_cfg_t* cfg)
{
  RA_CHECK_NULL_PTR(cfg, s_tag, "cfg");
  if (s_sdcard.initialised != 0U) {
    return k_ra_err_invalid_state;
  }

  /* HUM Ch 47.1 "SDHI Overview" p 3122 */ /* module bring-up first. */
  const ra_err_t hw_err = ra_sdhi_init(cfg->instance);
  RA_RETURN_ON_ERROR(hw_err, s_tag, "sdhi_init");

  uint32_t rsp[4] = {0U, 0U, 0U, 0U};

  /* HUM Ch 47.2.1 "SD_CMD : Command Type Register" p 3125 */
  /* CMD0 GO_IDLE_STATE -- no response, but send_command still polls
   * RSPEND so the host stays in lockstep with the bus state machine. */
  const ra_err_t e0 =
    ra_sdhi_send_command(cfg->instance, (uint32_t)k_ra_sdcard_cmd0_go_idle, 0U, rsp);
  RA_RETURN_ON_ERROR(e0, s_tag, "cmd0");

  /* HUM Ch 47.2.5 "SD_RSP10 : Response Register 10" p 3132 */
  /* CMD8 SEND_IF_COND -- echoes the low 12 bits of the argument back
   * in R7. Mismatch means a v1 (no-CMD8) card or no card at all. */
  const ra_err_t e8 = ra_sdhi_send_command(
    cfg->instance, (uint32_t)k_ra_sdcard_cmd8_send_if_cond,
    (uint32_t)k_ra_sdcard_cmd8_pattern, rsp);
  RA_RETURN_ON_ERROR(e8, s_tag, "cmd8");
  if ((rsp[0] & (uint32_t)k_ra_sdcard_cmd8_pattern_mask) !=
      ((uint32_t)k_ra_sdcard_cmd8_pattern & (uint32_t)k_ra_sdcard_cmd8_pattern_mask)) {
    (void)ra_sdhi_deinit(cfg->instance);
    return k_ra_err_hw_init_failed;
  }

  /* ACMD41 op-cond loop -- card moves from idle to ready. */
  uint32_t       ocr   = 0U;
  const ra_err_t e41   = internal_run_acmd41(cfg->instance, &ocr);
  if (e41 != k_ra_ok) {
    (void)ra_sdhi_deinit(cfg->instance);
    return e41;
  }
  const uint8_t high_capacity =
    ((ocr & (uint32_t)k_ra_sdcard_ocr_ccs_mask) != 0U) ? 1U : 0U;

  /* HUM Ch 47.2.5 "SD_RSP10..76 : Response Registers" p 3132 */
  /* CMD2 ALL_SEND_CID -- 136-bit response (R2). We don't decode the
   * CID for runtime use; just clock it through to advance the state
   * machine. */
  const ra_err_t e2 =
    ra_sdhi_send_command(cfg->instance, (uint32_t)k_ra_sdcard_cmd2_all_send_cid, 0U, rsp);
  RA_RETURN_ON_ERROR(e2, s_tag, "cmd2");

  /* CMD3 SEND_RELATIVE_ADDR -- card publishes its RCA in rsp[0][31:16]. */
  const ra_err_t e3 =
    ra_sdhi_send_command(cfg->instance, (uint32_t)k_ra_sdcard_cmd3_send_rca, 0U, rsp);
  RA_RETURN_ON_ERROR(e3, s_tag, "cmd3");
  const uint16_t rca = (uint16_t)((rsp[0] >> 16U) & 0xFFFFU);

  /* CMD9 SEND_CSD -- arg = RCA<<16. R2 response holds the 128-bit CSD
   * (with CRC stripped by the SDHI engine). */
  const uint32_t rca_arg = ((uint32_t)rca) << 16U;
  const ra_err_t e9 =
    ra_sdhi_send_command(cfg->instance, (uint32_t)k_ra_sdcard_cmd9_send_csd, rca_arg, rsp);
  RA_RETURN_ON_ERROR(e9, s_tag, "cmd9");

  uint32_t       blocks  = 0U;
  const ra_err_t dec_err = internal_decode_csd(rsp, &blocks);
  if (dec_err != k_ra_ok) {
    (void)ra_sdhi_deinit(cfg->instance);
    return dec_err;
  }

  /* CMD7 SELECT_CARD -- arg = RCA<<16 to put this card into TRAN. */
  const ra_err_t e7 =
    ra_sdhi_send_command(cfg->instance, (uint32_t)k_ra_sdcard_cmd7_select_card, rca_arg, rsp);
  RA_RETURN_ON_ERROR(e7, s_tag, "cmd7");

  /* HUM Ch 47.2.18 "SD_CLK_CTRL : SD Clock Control Register" p 3145 */
  /* Bump the bus from the 400 kHz identification rate to default
   * speed. The SDR50 / DDR50 negotiation lives in the future
   * ::ra_sdcard_set_speed extension. */
  const ra_err_t eclk =
    ra_sdhi_set_clock(cfg->instance, (uint32_t)k_ra_sdcard_default_clk_div);
  RA_RETURN_ON_ERROR(eclk, s_tag, "set_clock");

  s_sdcard.instance        = cfg->instance;
  s_sdcard.rca             = rca;
  s_sdcard.capacity_blocks = blocks;
  if (high_capacity == 0U) {
    s_sdcard.type = k_ra_sdcard_type_sdsc;
  } else if (blocks > (uint32_t)k_ra_sdcard_sdhc_threshold_blocks * 16U) {
    /* SDXC boundary is the SDA marketing threshold (32 GB), but the
     * exact cutover is fuzzy; treat anything above a generous 32 GB
     * as SDXC and the rest as SDHC. */
    s_sdcard.type = k_ra_sdcard_type_sdxc;
  } else {
    s_sdcard.type = k_ra_sdcard_type_sdhc;
  }
  s_sdcard.initialised = 1U;

  ra_log_info_val(s_tag, "sdcard init blocks", blocks);
  return k_ra_ok;
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
 */
static uint32_t internal_to_card_address(uint32_t lba)
{
  if (s_sdcard.type == k_ra_sdcard_type_sdsc) {
    return lba * 512U;
  }
  return lba;
}

ra_err_t ra_sdcard_read_blocks(uint32_t lba, uint8_t* buf, uint32_t count)
{
  RA_CHECK_NULL_PTR(buf, s_tag, "buf");
  if (count == 0U) {
    return k_ra_err_invalid_arg;
  }
  if (s_sdcard.initialised == 0U) {
    return k_ra_err_invalid_state;
  }
  if ((lba + count) > s_sdcard.capacity_blocks) {
    return k_ra_err_out_of_range;
  }
  const uint32_t card_lba = internal_to_card_address(lba);
  return ra_sdhi_read_block(s_sdcard.instance, card_lba, buf, count);
}

ra_err_t ra_sdcard_write_blocks(uint32_t lba, const uint8_t* buf, uint32_t count)
{
  RA_CHECK_NULL_PTR(buf, s_tag, "buf");
  if (count == 0U) {
    return k_ra_err_invalid_arg;
  }
  if (s_sdcard.initialised == 0U) {
    return k_ra_err_invalid_state;
  }
  if ((lba + count) > s_sdcard.capacity_blocks) {
    return k_ra_err_out_of_range;
  }
  const uint32_t card_lba = internal_to_card_address(lba);
  return ra_sdhi_write_block(s_sdcard.instance, card_lba, buf, count);
}

ra_err_t ra_sdcard_get_capacity(uint32_t* out_blocks)
{
  RA_CHECK_NULL_PTR(out_blocks, s_tag, "out_blocks");
  if (s_sdcard.initialised == 0U) {
    return k_ra_err_invalid_state;
  }
  *out_blocks = s_sdcard.capacity_blocks;
  return k_ra_ok;
}

ra_err_t ra_sdcard_get_type(ra_sdcard_card_type_t* out_type)
{
  RA_CHECK_NULL_PTR(out_type, s_tag, "out_type");
  if (s_sdcard.initialised == 0U) {
    return k_ra_err_invalid_state;
  }
  *out_type = s_sdcard.type;
  return k_ra_ok;
}

ra_err_t ra_sdcard_deinit(void)
{
  if (s_sdcard.initialised == 0U) {
    return k_ra_ok;
  }
  const uint8_t  inst = s_sdcard.instance;
  s_sdcard.initialised     = 0U;
  s_sdcard.capacity_blocks = 0U;
  s_sdcard.rca             = 0U;
  s_sdcard.type            = k_ra_sdcard_type_unknown;
  const ra_err_t err       = ra_sdhi_deinit(inst);
  if (err != k_ra_ok) {
    return k_ra_err_invalid_state;
  }
  return k_ra_ok;
}

/* NOLINTEND(readability-magic-numbers,readability-function-size,readability-function-cognitive-complexity) */
