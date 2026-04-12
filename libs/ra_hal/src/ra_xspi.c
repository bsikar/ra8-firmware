/**
 * @file ra_xspi.c
 * @brief xSPI / Octo-SPI driver framework
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra_xspi.h"

#include <stdint.h>

#include "ra8d2_ospi_regs.h"
#include "ra_check.h"
#include "ra_err.h"
#include "ra_log.h"

static const char* s_tag = "XSPI";

typedef enum : uint8_t {
  k_ra_xspi_cmd_max_bytes = 16U,
} ra_xspi_limits2_t;

typedef enum : uint32_t {
  k_ra_xspi_int_clear_all = 0xFFFFFFFFUL, /**< Write-1-to-clear mask for INTC. */
} ra_xspi_intc_val_t;

ra_err_t ra_xspi_init(uint8_t instance, ra_xspi_lio_mode_t mode)
{
  volatile r_xspi_regs_t* reg = ra_xspi(instance);
  RA_CHECK_NULL_PTR(reg, s_tag, "instance out of range");

  reg->WRAPCFG = 0U;
  reg->COMCFG  = 0U;
  reg->LIOCFG  = (uint32_t)mode;
  reg->INTC    = (uint32_t)k_ra_xspi_int_clear_all;

  ra_log_info_val(s_tag, "xspi_init inst", (uint32_t)instance);
  return k_ra_ok;
}

ra_err_t ra_xspi_direct_command(uint8_t instance, const uint8_t* cmd_buf, uint8_t len)
{
  RA_CHECK_NULL_PTR(cmd_buf, s_tag, "cmd_buf must not be nullptr");
  if (len > k_ra_xspi_cmd_max_bytes) {
    return k_ra_err_invalid_size;
  }
  volatile r_xspi_regs_t* reg = ra_xspi(instance);
  if (reg == nullptr) {
    return k_ra_err_out_of_range;
  }

  /* Pack the caller's bytes into the 4-word CMDBUF as little-endian
   * words. The xSPI controller interprets the layout based on
   * CMDCFG0/1/2; real driver will programme those per-operation. */
  uint32_t word = 0U;
  for (uint8_t i = 0U; i < len; i++) {
    word |= ((uint32_t)cmd_buf[i]) << ((i % 4U) * 8U);
    if ((i % 4U) == 3U) {
      reg->CMDBUF[i / 4U] = word;
      word                = 0U;
    }
  }
  if ((len % 4U) != 0U) {
    reg->CMDBUF[len / 4U] = word;
  }

  return k_ra_ok;
}
