/**
 * @file ra8_board_ek_ra8d2_pdm.c
 * @brief EK-RA8D2 SPH0690 PDM microphone routing and filter policy.
 * @ingroup grp_board
 * @details Encodes the board's microphone clock/data pins and validated PDM
 *          filter defaults, then delegates peripheral setup to the PDM HAL.
 *
 * @par Tag
 * [Ring 5 / BSP] {World: S}
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>

#include "ra8_board_ek_ra8d2.h"
#include "ra8_pdm.h"
#include "ra8_port_constants.h"
#include "ra8_port_utils.h"

typedef enum : uint32_t {
  k_board_pdm_sample_rate_hz = 16000U, /**< Decimated SPH0690 sample rate. */
} ra8_board_pdm_rate_t;

typedef enum : uint8_t {
  k_board_pdm_valid_bits = 20U, /**< PDM-IF signed PCM payload width. */
} ra8_board_pdm_width_t;

static const ra8_port_pin_t k_board_pdm_pin_clk = RA8_PIN(k_ra8_port_8, k_ra8_pin_12);
static const ra8_port_pin_t k_board_pdm_pin_dat = RA8_PIN(k_ra8_port_5, k_ra8_pin_2);

/* NOLINTBEGIN(readability-magic-numbers) -- HUM Table 49.7 row and reset filter coefficients. */
static const ra8_pdm_channel_cfg_t k_board_pdm_sph0690 = {
  .sinc_order   = 4U,
  .clock_div    = 0U,
  .sinc_dec     = 0x7CU,
  .sinc_range   = 0x05U,
  .data_shift   = 0U,
  .edge         = 0U,
  .hpf_shift    = 0U,
  .cf_shift     = 0U,
  .lpf_shift    = 0U,
  .rx_threshold = 4U,
  .hpf_s0       = 0x3F61U,
  .hpf_k1       = 0x3EC1U,
  .hpf_h        = {0x4000U, 0xC000U},
  .comp_h       = {0x1FE8U,
                   0x0039U,
                   0x003CU,
                   0x1E56U,
                   0x01DCU,
                   0x06E1U,
                   0x01DCU,
                   0x1E56U,
                   0x003CU,
                   0x0039U,
                   0x1FE8U},
  .lpf_h0       = 0x0400U,
  .lpf_h1       = {0x1FF8U, 0x000AU, 0x1FF0U, 0x0018U, 0x1FDCU, 0x0034U, 0x1FB3U,
                   0x0076U, 0x1F2EU, 0x0289U, 0x0289U, 0x1F2EU, 0x0076U, 0x1FB3U,
                   0x0034U, 0x1FDCU, 0x0018U, 0x1FF0U, 0x000AU, 0x1FF8U},
};
/* NOLINTEND(readability-magic-numbers) */

ra8_err_t ra8_board_pdm_mic_route(void)
{
  ra8_err_t err = ra8_pfs_route_peripheral(k_board_pdm_pin_clk, k_ra8_psel_pdm, "board.pdm.clk");
  if (err != k_ra8_ok) {
    return err;
  }
  err = ra8_pfs_route_peripheral(k_board_pdm_pin_dat, k_ra8_psel_pdm, "board.pdm.dat");
  return err;
}

ra8_err_t ra8_board_pdm_mic_get_config(ra8_board_pdm_mic_t         microphone,
                                       ra8_board_pdm_mic_config_t* out_config)
{
  if (out_config == nullptr) {
    return k_ra8_err_null_ptr;
  }
  if ((uint8_t)microphone >= (uint8_t)k_ra8_board_pdm_mic_count) {
    return k_ra8_err_invalid_arg;
  }
  *out_config = (ra8_board_pdm_mic_config_t){
    .pdm            = k_board_pdm_sph0690,
    .sample_rate_hz = (uint32_t)k_board_pdm_sample_rate_hz,
    .channel        = (uint8_t)k_ra8_pdm_ch2,
    .valid_bits     = (uint8_t)k_board_pdm_valid_bits,
  };
  out_config->pdm.edge = (microphone == k_ra8_board_pdm_mic1) ? (uint8_t)0U : (uint8_t)1U;
  return k_ra8_ok;
}
