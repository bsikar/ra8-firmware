/**
 * @file ra_xspi.c
 * @brief xSPI / Octo-SPI driver with SPI NOR flash read / program / erase
 *
 * @details
 * Provides a minimal flash-driver surface layered on top of the
 * RA8D2 xSPI controller's direct-command mode. The driver supports:
 *
 *  - `ra_xspi_init()` -- configure LIOCFG, clear interrupts.
 *  - `ra_xspi_direct_command()` -- raw command-buffer poke.
 *  - `ra_xspi_flash_read()` -- 0x03 read + COMSTT poll.
 *  - `ra_xspi_flash_program()` -- 0x06 WREN, 0x02 PP, 0x05 WIP poll.
 *  - `ra_xspi_flash_erase_sector()` -- 0x06 WREN, 0x20 SE, 0x05 WIP poll.
 *  - `ra_xspi_flash_read_status()` -- 0x05.
 *  - `ra_xspi_flash_read_id()` -- 0x9F JEDEC ID.
 *
 * In `RA_SIMULATOR_MODE` every read/program/erase goes through an
 * in-process 4 KiB fake-flash buffer, so unit tests can round-trip
 * data and branch coverage stays high without real hardware.
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
#include "ra_mstp.h"

static const char* s_tag = "XSPI";

typedef enum : uint8_t {
  k_ra_xspi_cmd_max_bytes = 16U,
} ra_xspi_limits2_t;

/**
 * @enum ra_spi_flash_op_t
 * @brief Standard JEDEC NOR-flash command opcodes used by this driver.
 */
typedef enum : uint8_t {
  k_ra_spi_flash_op_write_enable = 0x06U,
  k_ra_spi_flash_op_page_program = 0x02U,
  k_ra_spi_flash_op_read_status  = 0x05U,
  k_ra_spi_flash_op_read_id      = 0x9FU,
  k_ra_spi_flash_op_read         = 0x03U,
  k_ra_spi_flash_op_erase_sector = 0x20U,
} ra_spi_flash_op_t;

/**
 * @enum ra_flash_status_bit_t
 * @brief Bit positions in the SPI flash Status Register.
 */
typedef enum : uint8_t {
  k_ra_flash_status_bit_wip = 0U, /**< Write-In-Progress (busy).    */
  k_ra_flash_status_bit_wel = 1U, /**< Write Enable Latch.          */
} ra_flash_status_bit_t;

/**
 * @enum ra_xspi_timeouts_t
 * @brief Bounded spin budgets for host + target builds.
 */
typedef enum : uint32_t {
  k_ra_xspi_cmd_spin            = 64U,      /**< Cmd-complete poll budget.    */
  k_ra_flash_program_timeout_us = 1000000U, /**< Max 1 s for a program op.    */
} ra_xspi_timeouts_t;

/**
 * @enum ra_xspi_jedec_t
 * @brief Synthetic JEDEC ID returned in RA_SIMULATOR_MODE.
 *
 * @details
 * Must match the layout ``(manufacturer << 16) | (type << 8) |
 * capacity`` used by `ra_xspi_flash_read_id()`. The tests assert on
 * this exact value.
 */
typedef enum : uint32_t {
  k_ra_sim_jedec_manufacturer = 0xC2U,      /**< Macronix manufacturer ID. */
  k_ra_sim_jedec_memory_type  = 0x20U,      /**< MX25 family.              */
  k_ra_sim_jedec_capacity     = 0x1AU,      /**< 512 Mbit.                 */
  k_ra_sim_jedec_id           = 0xC2201AUL, /**< Packed JEDEC ID word.     */
} ra_xspi_jedec_t;

typedef enum : uint32_t {
  k_ra_xspi_int_clear_all = 0xFFFFFFFFUL, /**< Write-1-to-clear mask for INTC. */
} ra_xspi_intc_val_t;

typedef enum : uint32_t {
  k_ra_xspi_cmdcfg0_cmd_1byte  = 1UL << 8U, /**< CMDCFG0: 1-byte opcode.       */
  k_ra_xspi_cmdcfg0_addr_3byte = 3UL << 4U, /**< CMDCFG0: 3-byte address.      */
  k_ra_xspi_fake_flash_erased  = 0xFFU,     /**< Erased-flash pattern.         */
  k_ra_xspi_fake_flash_size    = 4096U,     /**< Fake-flash backing size.      */
} ra_xspi_cmdcfg_vals_t;

#ifdef RA_SIMULATOR_MODE
/* One fake-flash window per instance. Tests can poke this directly
 * through the program API and then read it back. */
static uint8_t s_fake_flash[k_ra_xspi_instance_count][k_ra_xspi_fake_flash_size];

/**
 * @brief Set every byte of `dst[0..len-1]` to `value` via a plain loop.
 */
static void internal_fake_flash_fill(uint8_t* dst, uint8_t value, uint32_t len)
{
  for (uint32_t i = 0U; i < len; i++) {
    dst[i] = value;
  }
}

/**
 * @brief Byte-wise copy helper (avoids libc string calls in host tests).
 */
static void internal_fake_flash_copy(uint8_t* dst, const uint8_t* src, uint32_t len)
{
  for (uint32_t i = 0U; i < len; i++) {
    dst[i] = src[i];
  }
}

__attribute__((constructor)) static void internal_xspi_sim_init(void)
{
  for (uint8_t i = 0U; i < (uint8_t)k_ra_xspi_instance_count; i++) {
    internal_fake_flash_fill(s_fake_flash[i],
                             (uint8_t)k_ra_xspi_fake_flash_erased,
                             (uint32_t)k_ra_xspi_fake_flash_size);
  }
}
#endif /* RA_SIMULATOR_MODE */

/**
 * @var s_xspi_mstp_table
 * @brief Instance-index -> MSTP id lookup. OSPI0 and OSPI1 each
 *        have their own MSTPB bit (16/17) per HUM Ch 11.2.7 p 444.
 *        Each bit also covers the matching DOTF channel.
 */
static const ra_mstp_t s_xspi_mstp_table[] = {
  k_ra_mstp_ospi0,
  k_ra_mstp_ospi1,
};

ra_err_t ra_xspi_init(uint8_t instance, ra_xspi_lio_mode_t mode)
{
  volatile r_xspi_regs_t* reg = ra_xspi(instance);
  RA_CHECK_NULL_PTR(reg, s_tag, "instance out of range");
  if (instance >= (uint8_t)(sizeof(s_xspi_mstp_table) / sizeof(s_xspi_mstp_table[0]))) {
    return k_ra_err_invalid_arg;
  }

  /* HUM Ch 11.2.7 "MSTPCRB : Module Stop Control Register B", p 444 */
  const ra_err_t mst_err = ra_mstp_enable(s_xspi_mstp_table[instance]);
  RA_RETURN_ON_ERROR(mst_err, s_tag, "xspi_init: mstp enable"); /* GCOVR_EXCL_BR_LINE */

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
  if (len > (uint8_t)k_ra_xspi_cmd_max_bytes) {
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

/**
 * @brief Bounded COMSTT poll (with simulator-mode fast exit).
 */
static ra_err_t internal_wait_command_done(volatile r_xspi_regs_t* reg)
{
#ifdef RA_SIMULATOR_MODE
  /* On the host there is no hardware -- pretend the command finished. */
  reg->COMSTT = (uint32_t)(1UL << (uint32_t)k_ra_xspi_comstt_bit_done);
  reg->INTC   = (uint32_t)k_ra_xspi_intc_cmdcmp;
  return k_ra_ok;
#else
  for (uint32_t i = 0U; i < (uint32_t)k_ra_xspi_cmd_spin; i++) {
    const uint32_t s = reg->COMSTT;
    if ((s & (uint32_t)(1UL << (uint32_t)k_ra_xspi_comstt_bit_trbusy)) == 0U) {
      reg->INTC = (uint32_t)k_ra_xspi_intc_cmdcmp;
      return k_ra_ok;
    }
  }
  return k_ra_err_hw_timeout;
#endif
}

/**
 * @brief Drop a single 1-byte opcode into the command buffer and kick off a xfer.
 */
static ra_err_t internal_issue_simple_opcode(volatile r_xspi_regs_t* reg, uint8_t opcode)
{
  reg->CMDCFG0   = (uint32_t)k_ra_xspi_cmdcfg0_cmd_1byte;
  reg->CMDCFG1   = 0U;
  reg->CMDCFG2   = 0U;
  reg->CMDBUF[0] = (uint32_t)opcode;
  return internal_wait_command_done(reg);
}

/**
 * @brief Clamp `flash_addr + len` to the simulator fake-flash size.
 */
#ifdef RA_SIMULATOR_MODE
static ra_err_t internal_sim_range_check(uint32_t flash_addr, uint32_t len)
{
  if (flash_addr >= (uint32_t)k_ra_xspi_fake_flash_size) {
    return k_ra_err_invalid_arg;
  }
  if ((flash_addr + len) > (uint32_t)k_ra_xspi_fake_flash_size) {
    return k_ra_err_invalid_arg;
  }
  return k_ra_ok;
}
#endif

ra_err_t ra_xspi_flash_read(uint8_t instance, uint32_t flash_addr, uint8_t* buf, uint32_t len)
{
  RA_CHECK_NULL_PTR(buf, s_tag, "buf must not be nullptr");
  if ((len == 0U) || (len > (uint32_t)k_ra_xspi_max_xfer)) {
    return k_ra_err_invalid_arg;
  }
  volatile r_xspi_regs_t* reg = ra_xspi(instance);
  RA_CHECK_NULL_PTR(reg, s_tag, "instance out of range");

  reg->CMDCFG0   = (uint32_t)k_ra_xspi_cmdcfg0_cmd_1byte | (uint32_t)k_ra_xspi_cmdcfg0_addr_3byte;
  reg->CMDCFG1   = flash_addr;
  reg->CMDCFG2   = len;
  reg->CMDBUF[0] = (uint32_t)k_ra_spi_flash_op_read;

  const ra_err_t wait = internal_wait_command_done(reg);
  if (wait != k_ra_ok) {
    return wait;
  }

#ifdef RA_SIMULATOR_MODE
  const ra_err_t rng = internal_sim_range_check(flash_addr, len);
  if (rng != k_ra_ok) {
    return rng;
  }
  internal_fake_flash_copy(buf, &s_fake_flash[instance][flash_addr], len);
#else
  /* Copy the RDBUF shadow into the caller buffer. */
  for (uint32_t i = 0U; i < len; i++) {
    const uint32_t word = reg->RDBUF[(i / 4U) & 3U];
    buf[i]              = (uint8_t)(word >> ((i % 4U) * 8U));
  }
#endif
  return k_ra_ok;
}

/**
 * @brief Drive the page-program register sequence (WREN -> PP -> command buffer fill).
 */
static ra_err_t
internal_flash_start_program(volatile r_xspi_regs_t* reg, uint32_t flash_addr, uint32_t len)
{
  const ra_err_t wren = internal_issue_simple_opcode(reg, (uint8_t)k_ra_spi_flash_op_write_enable);
  if (wren != k_ra_ok) {
    return wren;
  }
  reg->CMDCFG0   = (uint32_t)k_ra_xspi_cmdcfg0_cmd_1byte | (uint32_t)k_ra_xspi_cmdcfg0_addr_3byte;
  reg->CMDCFG1   = flash_addr;
  reg->CMDCFG2   = len;
  reg->CMDBUF[0] = (uint32_t)k_ra_spi_flash_op_page_program;
  return internal_wait_command_done(reg);
}

/**
 * @brief Poll the SPI flash Status Register until WIP == 0 or timeout.
 */
static ra_err_t internal_poll_wip_clear(uint8_t instance)
{
#ifdef RA_SIMULATOR_MODE
  /* Simulator: fake flash is always idle. */
  (void)instance;
  return k_ra_ok;
#else
  for (uint32_t i = 0U; i < (uint32_t)k_ra_flash_program_timeout_us; i++) {
    uint8_t        status = 0U;
    const ra_err_t e      = ra_xspi_flash_read_status(instance, &status);
    if (e != k_ra_ok) {
      return e;
    }
    if ((status & (uint8_t)(1U << (uint8_t)k_ra_flash_status_bit_wip)) == 0U) {
      return k_ra_ok;
    }
  }
  return k_ra_err_timeout;
#endif
}

ra_err_t
ra_xspi_flash_program(uint8_t instance, uint32_t flash_addr, const uint8_t* data, uint32_t len)
{
  RA_CHECK_NULL_PTR(data, s_tag, "data must not be nullptr");
  if ((len == 0U) || (len > (uint32_t)k_ra_xspi_max_xfer)) {
    return k_ra_err_invalid_arg;
  }
  volatile r_xspi_regs_t* reg = ra_xspi(instance);
  RA_CHECK_NULL_PTR(reg, s_tag, "instance out of range");

  const ra_err_t p = internal_flash_start_program(reg, flash_addr, len);
  if (p != k_ra_ok) {
    return p;
  }
#ifdef RA_SIMULATOR_MODE
  const ra_err_t rng = internal_sim_range_check(flash_addr, len);
  if (rng != k_ra_ok) {
    return rng;
  }
  /* AND-only model -- SPI NOR flash can only clear bits, never set
   * them. Writing the same region twice without an erase will
   * drop bits. Keeps tests deterministic. */
  for (uint32_t i = 0U; i < len; i++) {
    s_fake_flash[instance][flash_addr + i] &= data[i];
  }
#else
  for (uint32_t i = 0U; i < len; i++) {
    reg->CMDBUF[1 + ((i / 4U) & 3U)] |= ((uint32_t)data[i]) << ((i % 4U) * 8U);
  }
#endif
  return internal_poll_wip_clear(instance);
}

ra_err_t ra_xspi_flash_erase_sector(uint8_t instance, uint32_t flash_addr)
{
  volatile r_xspi_regs_t* reg = ra_xspi(instance);
  RA_CHECK_NULL_PTR(reg, s_tag, "instance out of range");

  const ra_err_t wren = internal_issue_simple_opcode(reg, (uint8_t)k_ra_spi_flash_op_write_enable);
  if (wren != k_ra_ok) {
    return wren;
  }

  reg->CMDCFG0   = (uint32_t)k_ra_xspi_cmdcfg0_cmd_1byte | (uint32_t)k_ra_xspi_cmdcfg0_addr_3byte;
  reg->CMDCFG1   = flash_addr;
  reg->CMDCFG2   = 0U;
  reg->CMDBUF[0] = (uint32_t)k_ra_spi_flash_op_erase_sector;

  const ra_err_t wait = internal_wait_command_done(reg);
  if (wait != k_ra_ok) {
    return wait;
  }

#ifdef RA_SIMULATOR_MODE
  const uint32_t sector_base = flash_addr & ~((uint32_t)k_ra_xspi_sector_len - 1U);
  if (sector_base >= (uint32_t)k_ra_xspi_fake_flash_size) {
    return k_ra_err_invalid_arg;
  }
  internal_fake_flash_fill(&s_fake_flash[instance][sector_base],
                           (uint8_t)k_ra_xspi_fake_flash_erased,
                           (uint32_t)k_ra_xspi_sector_len);
#endif
  return internal_poll_wip_clear(instance);
}

ra_err_t ra_xspi_flash_read_status(uint8_t instance, uint8_t* out_status)
{
  RA_CHECK_NULL_PTR(out_status, s_tag, "out_status must not be nullptr");
  volatile r_xspi_regs_t* reg = ra_xspi(instance);
  RA_CHECK_NULL_PTR(reg, s_tag, "instance out of range");

  const ra_err_t e = internal_issue_simple_opcode(reg, (uint8_t)k_ra_spi_flash_op_read_status);
  if (e != k_ra_ok) {
    return e;
  }
#ifdef RA_SIMULATOR_MODE
  /* Simulator: always report WEL=1, WIP=0 (flash idle and ready). */
  *out_status = (uint8_t)(1U << (uint8_t)k_ra_flash_status_bit_wel);
#else
  *out_status = (uint8_t)(reg->RDBUF[0] & 0xFFUL);
#endif
  return k_ra_ok;
}

ra_err_t ra_xspi_flash_read_id(uint8_t instance, uint32_t* out_id)
{
  RA_CHECK_NULL_PTR(out_id, s_tag, "out_id must not be nullptr");
  volatile r_xspi_regs_t* reg = ra_xspi(instance);
  RA_CHECK_NULL_PTR(reg, s_tag, "instance out of range");

  const ra_err_t e = internal_issue_simple_opcode(reg, (uint8_t)k_ra_spi_flash_op_read_id);
  if (e != k_ra_ok) {
    return e;
  }
#ifdef RA_SIMULATOR_MODE
  *out_id = (uint32_t)k_ra_sim_jedec_id;
#else
  const uint32_t word = reg->RDBUF[0];
  *out_id = ((word & 0xFFUL) << 16U) | (((word >> 8U) & 0xFFUL) << 8U) | ((word >> 16U) & 0xFFUL);
#endif
  return k_ra_ok;
}
