/**
 * @file ra_xspi.c
 * @brief Octal Serial Peripheral Interface (OSPI / xSPI) driver
 *
 * @par Tag
 * [Ring 3 / HAL] {World: S}
 *
 * @details
 * driver for the RA8D2 OSPI block. Provides a minimal
 * SPI NOR flash driver layered on top of the xSPI manual-command
 * engine (``CDCTL0`` + ``CDBUF[]`` + ``INTS.CMDCMP``).
 *
 * Supported operations:
 *
 * - ``ra_xspi_init()`` -- select a protocol mode + clear pending IRQs.
 * - ``ra_xspi_direct_command()`` -- raw CDBUF poke.
 * - ``ra_xspi_flash_read()`` -- 0x03 read + CMDCMP poll.
 * - ``ra_xspi_flash_program()`` -- 0x06 WREN, 0x02 PP, 0x05 WIP poll.
 * - ``ra_xspi_flash_erase_sector()`` -- 0x06 WREN, 0x20 SE, 0x05 WIP poll.
 * - ``ra_xspi_flash_read_status()`` -- 0x05.
 * - ``ra_xspi_flash_read_id()`` -- 0x9F JEDEC ID.
 * - ``ra_xspi_deinit / get_status / clear_status / attach_handler /
 * enter_stop / exit_stop`` lifecycle + IRQ + power surface.
 *
 * ## CDBUF convention used by this driver
 *
 * Each ``CDBUF`` slot is 4 x 32-bit words wide. This driver writes
 * exclusively to slot 0 and encodes a manual transfer as:
 *
 * - ``CDBUF[0]`` -- opcode byte in the low 8 bits.
 * - ``CDBUF[1]`` -- flash address (3- or 4-byte).
 * - ``CDBUF[2]`` -- write-data low word (bytes 0..3).
 * - ``CDBUF[3]`` -- write-data high word (bytes 4..7).
 *
 * Read responses (status, JEDEC ID, flash-read bytes) land in
 * ``CDBUF[2]`` / ``CDBUF[3]`` after the controller clears TRREQ.
 *
 * In ``RA_SIMULATOR_MODE`` every flash read/program/erase routes
 * through an in-process 4 KiB fake-flash buffer so unit tests can
 * round-trip data without real hardware. The register sequence is
 * still emitted in full so test cases can assert on register state
 * if they need to. Every register write carries a
 * ``HUM Ch 44 "Octal Serial Peripheral Interface (OSPI)" p 2986``
 * citation comment for the cite checker.
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

/** @brief Logging tag for this driver. */
static const char* s_tag = "XSPI";

/**
 * @enum ra_xspi_cmd_limits_t
 * @brief Byte-count limits on raw direct-command buffers.
 */
typedef enum : uint8_t {
  k_ra_xspi_cmd_max_bytes = 16U, /**< A CDBUF slot holds 16 bytes. */
} ra_xspi_cmd_limits_t;

/**
 * @enum ra_spi_flash_op_t
 * @brief Standard JEDEC NOR-flash command opcodes used by this driver.
 */
typedef enum : uint8_t {
  k_ra_spi_flash_op_write_enable = 0x06U, /**< 0x06 WREN. */
  k_ra_spi_flash_op_page_program = 0x02U, /**< 0x02 page program. */
  k_ra_spi_flash_op_read_status  = 0x05U, /**< 0x05 read status reg. */
  k_ra_spi_flash_op_read_id      = 0x9FU, /**< 0x9F JEDEC ID read. */
  k_ra_spi_flash_op_read         = 0x03U, /**< 0x03 normal read. */
  k_ra_spi_flash_op_erase_sector = 0x20U, /**< 0x20 sector erase. */
} ra_spi_flash_op_t;

/**
 * @enum ra_flash_status_bit_t
 * @brief Bit positions in the SPI flash Status Register.
 */
typedef enum : uint8_t {
  k_ra_flash_status_bit_wip = 0U, /**< Write-In-Progress (busy). */
  k_ra_flash_status_bit_wel = 1U, /**< Write Enable Latch. */
} ra_flash_status_bit_t;

/**
 * @enum ra_xspi_timeouts_t
 * @brief Bounded spin budgets for host + target builds.
 */
typedef enum : uint32_t {
  k_ra_xspi_cmd_spin            = 64U,      /**< CMDCMP poll budget. */
  k_ra_flash_program_timeout_us = 1000000U, /**< Max 1 s for a program op. */
} ra_xspi_timeouts_t;

/**
 * @enum ra_xspi_jedec_t
 * @brief Synthetic JEDEC ID returned in RA_SIMULATOR_MODE.
 *
 * @details
 * Must match the packed layout
 * ``(manufacturer << 16) | (type << 8) | capacity`` used by
 * ``ra_xspi_flash_read_id()``. Tests assert on this exact value.
 */
typedef enum : uint32_t {
  k_ra_sim_jedec_manufacturer = 0xC2U,      /**< Macronix manufacturer ID. */
  k_ra_sim_jedec_memory_type  = 0x20U,      /**< MX25 family. */
  k_ra_sim_jedec_capacity     = 0x1AU,      /**< 512 Mbit. */
  k_ra_sim_jedec_id           = 0xC2201AUL, /**< Packed JEDEC ID word. */
} ra_xspi_jedec_t;

/**
 * @enum ra_xspi_fake_flash_vals_t
 * @brief Constants used by the RA_SIMULATOR_MODE fake-flash backing store.
 */
typedef enum : uint32_t {
  k_ra_xspi_fake_flash_erased = 0xFFU, /**< Erased-flash pattern. */
  k_ra_xspi_fake_flash_size   = 4096U, /**< Fake-flash backing size. */
} ra_xspi_fake_flash_vals_t;

/**
 * @enum ra_xspi_cdbuf_idx_t
 * @brief Word indices into CDBUF slot 0 used by this driver.
 */
typedef enum : uint8_t {
  k_ra_xspi_cdbuf_idx_opcode = 0U, /**< CDBUF[0] holds the opcode byte. */
  k_ra_xspi_cdbuf_idx_addr   = 1U, /**< CDBUF[1] holds the flash address. */
  k_ra_xspi_cdbuf_idx_data0  = 2U, /**< CDBUF[2] holds data bytes 0..3. */
  k_ra_xspi_cdbuf_idx_data1  = 3U, /**< CDBUF[3] holds data bytes 4..7. */
} ra_xspi_cdbuf_idx_t;

#ifdef RA_SIMULATOR_MODE
/* One fake-flash window per instance. Tests can poke this directly
 * through the program API and then read it back. */
static uint8_t s_fake_flash[k_ra_xspi_instance_count][k_ra_xspi_fake_flash_size];

/**
 * @brief Set every byte of ``dst[0..len-1]`` to ``value`` via a plain loop.
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
 * have their own MSTPB bit (16/17) per HUM Ch 11.2.7 p 444.
 * Each bit also covers the matching DOTF channel.
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

  /* HUM Ch 44 "Octal Serial Peripheral Interface (OSPI)" p 2986 */
  /* Wake the wrapper, idle the common config, set the link-IO
   * protocol for slave 0, and clear any latent interrupt flags. */
  reg->WRAPCFG     = 0U;
  reg->COMCFG      = 0U;
  reg->LIOCFGCS[0] = (uint32_t)mode;
  reg->INTC        = (uint32_t)k_ra_xspi_ints_mask_all;

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

  /* HUM Ch 44 "Octal Serial Peripheral Interface (OSPI)" p 2986 */
  /* Pack the caller's bytes into CDBUF slot 0 as little-endian
   * words. Slot 0 is the full 16-byte CDBUF window (4 u32), so
   * raw callers can use any of the four indices. */
  uint32_t word = 0U;
  for (uint8_t i = 0U; i < len; i++) {
    word |= ((uint32_t)cmd_buf[i]) << ((i % 4U) * 8U);
    if ((i % 4U) == 3U) {
      reg->CDBUF[i / 4U] = word;
      word               = 0U;
    }
  }
  if ((len % 4U) != 0U) {
    reg->CDBUF[len / 4U] = word;
  }

  return k_ra_ok;
}

/**
 * @brief Bounded CMDCMP poll (with simulator-mode fast exit).
 */
static ra_err_t internal_wait_command_done(volatile r_xspi_regs_t* reg)
{
#ifdef RA_SIMULATOR_MODE
  /* HUM Ch 44 "Octal Serial Peripheral Interface (OSPI)" p 2986 */
  /* On the host there is no hardware -- pretend the command
   * finished: set CMDCMP in INTS and then clear it via INTC to
   * mirror the target flow exactly. */
  reg->INTS = (uint32_t)k_ra_xspi_ints_mask_cmdcmp;
  reg->INTC = (uint32_t)k_ra_xspi_ints_mask_cmdcmp;
  return k_ra_ok;
#else
  for (uint32_t i = 0U; i < (uint32_t)k_ra_xspi_cmd_spin; i++) {
    const uint32_t s = reg->INTS;
    if ((s & (uint32_t)k_ra_xspi_ints_mask_cmdcmp) != 0U) {
      /* HUM Ch 44 "Octal Serial Peripheral Interface (OSPI)" p 2986 */
      reg->INTC = (uint32_t)k_ra_xspi_ints_mask_cmdcmp;
      return k_ra_ok;
    }
  }
  return k_ra_err_hw_timeout;
#endif
}

/**
 * @brief Kick a prepared manual-command transfer by raising TRREQ.
 */
static ra_err_t internal_kick_command(volatile r_xspi_regs_t* reg)
{
  /* HUM Ch 44 "Octal Serial Peripheral Interface (OSPI)" p 2986 */
  reg->CDCTL0 = (uint32_t)k_ra_xspi_cdctl0_mask_trreq;
  return internal_wait_command_done(reg);
}

/**
 * @brief Drop a single 1-byte opcode into CDBUF slot 0 and kick off a xfer.
 */
static ra_err_t internal_issue_simple_opcode(volatile r_xspi_regs_t* reg, uint8_t opcode)
{
  /* HUM Ch 44 "Octal Serial Peripheral Interface (OSPI)" p 2986 */
  /* Populate CDBUF slot 0 with just the opcode, no address and no
   * data length. The manual-command engine knows how many bytes to
   * ship from the CDT sub-field encoding we mirror into CDBUF[0]. */
  reg->CDBUF[(uint8_t)k_ra_xspi_cdbuf_idx_opcode] = (uint32_t)opcode;
  reg->CDBUF[(uint8_t)k_ra_xspi_cdbuf_idx_addr]   = 0U;
  reg->CDBUF[(uint8_t)k_ra_xspi_cdbuf_idx_data0]  = 0U;
  reg->CDBUF[(uint8_t)k_ra_xspi_cdbuf_idx_data1]  = 0U;
  reg->CDCTL1                                     = 0U;
  reg->CDCTL2                                     = 0U;
  return internal_kick_command(reg);
}

/**
 * @brief Clamp ``flash_addr + len`` to the simulator fake-flash size.
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

  /* HUM Ch 44 "Octal Serial Peripheral Interface (OSPI)" p 2986 */
  /* Programme a JEDEC 0x03 read with 3-byte address. CDBUF[0]
   * carries the opcode, CDBUF[1] carries the address, CDCTL1
   * carries the data-length count, and CDCTL0.TRREQ kicks the
   * controller. Read data lands in CDBUF[2..3] when CMDCMP fires. */
  reg->CDBUF[(uint8_t)k_ra_xspi_cdbuf_idx_opcode] = (uint32_t)k_ra_spi_flash_op_read;
  reg->CDBUF[(uint8_t)k_ra_xspi_cdbuf_idx_addr]   = flash_addr;
  reg->CDCTL1                                     = len;

  const ra_err_t wait = internal_kick_command(reg);
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
  /* HUM Ch 44 "Octal Serial Peripheral Interface (OSPI)" p 2986 */
  /* Copy the CDBUF data words into the caller buffer (bytes 0..3
   * live in CDBUF[2], bytes 4..7 live in CDBUF[3]). */
  for (uint32_t i = 0U; i < len; i++) {
    const uint8_t cdbuf_word_idx =
      (i < 4U) ? (uint8_t)k_ra_xspi_cdbuf_idx_data0 : (uint8_t)k_ra_xspi_cdbuf_idx_data1;
    const uint32_t word = reg->CDBUF[cdbuf_word_idx];
    buf[i]              = (uint8_t)(word >> ((i % 4U) * 8U));
  }
#endif
  return k_ra_ok;
}

/**
 * @brief Drive the page-program register sequence (WREN -> PP -> kick).
 */
static ra_err_t
internal_flash_start_program(volatile r_xspi_regs_t* reg, uint32_t flash_addr, uint32_t len)
{
  const ra_err_t wren = internal_issue_simple_opcode(reg, (uint8_t)k_ra_spi_flash_op_write_enable);
  if (wren != k_ra_ok) {
    return wren;
  }
  /* HUM Ch 44 "Octal Serial Peripheral Interface (OSPI)" p 2986 */
  /* Programme a JEDEC 0x02 page-program with 3-byte address. */
  reg->CDBUF[(uint8_t)k_ra_xspi_cdbuf_idx_opcode] = (uint32_t)k_ra_spi_flash_op_page_program;
  reg->CDBUF[(uint8_t)k_ra_xspi_cdbuf_idx_addr]   = flash_addr;
  reg->CDCTL1                                     = len;
  return internal_kick_command(reg);
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
  /* HUM Ch 44 "Octal Serial Peripheral Interface (OSPI)" p 2986 */
  /* Stage outgoing bytes into CDBUF data words 0/1. Up to 8
   * bytes per manual command; larger writes fall out to the
   * page-program loop the caller handles above. */
  uint32_t data_lo = 0U;
  uint32_t data_hi = 0U;
  for (uint32_t i = 0U; i < len; i++) {
    const uint32_t shift = (uint32_t)((i % 4U) * 8U);
    if (i < 4U) {
      data_lo |= ((uint32_t)data[i]) << shift;
    } else {
      data_hi |= ((uint32_t)data[i]) << shift;
    }
  }
  reg->CDBUF[(uint8_t)k_ra_xspi_cdbuf_idx_data0] = data_lo;
  reg->CDBUF[(uint8_t)k_ra_xspi_cdbuf_idx_data1] = data_hi;
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

  /* HUM Ch 44 "Octal Serial Peripheral Interface (OSPI)" p 2986 */
  /* Programme a JEDEC 0x20 sector-erase with 3-byte address. */
  reg->CDBUF[(uint8_t)k_ra_xspi_cdbuf_idx_opcode] = (uint32_t)k_ra_spi_flash_op_erase_sector;
  reg->CDBUF[(uint8_t)k_ra_xspi_cdbuf_idx_addr]   = flash_addr;
  reg->CDCTL1                                     = 0U;

  const ra_err_t wait = internal_kick_command(reg);
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
  /* HUM Ch 44 "Octal Serial Peripheral Interface (OSPI)" p 2986 */
  *out_status = (uint8_t)(reg->CDBUF[(uint8_t)k_ra_xspi_cdbuf_idx_data0] & 0xFFUL);
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
  /* HUM Ch 44 "Octal Serial Peripheral Interface (OSPI)" p 2986 */
  /* JEDEC 0x9F returns MFR, MEMTYPE, CAPACITY in that byte order.
   * Repack into ``(mfr << 16) | (type << 8) | cap`` as documented
   * in the public API header. */
  const uint32_t word = reg->CDBUF[(uint8_t)k_ra_xspi_cdbuf_idx_data0];
  *out_id = ((word & 0xFFUL) << 16U) | (((word >> 8U) & 0xFFUL) << 8U) | ((word >> 16U) & 0xFFUL);
#endif
  return k_ra_ok;
}

/* =============================================================================
 * lifecycle + IRQ + power transition
 * =============================================================================
 */

/**
 * @struct ra_xspi_state_t
 * @brief Per-instance callback state.
 */
typedef struct {
  ra_xspi_event_fn_t fn;  /**< User-supplied completion callback. */
  void*              ctx; /**< Opaque context pointer for ``fn``. */
} ra_xspi_state_t;

/** @brief Per-instance callback state (s_ prefix for file-static). */
static ra_xspi_state_t s_xspi_state[k_ra_xspi_instance_count];

ra_err_t ra_xspi_deinit(uint8_t instance)
{
  volatile r_xspi_regs_t* reg = ra_xspi(instance);
  RA_CHECK_NULL_PTR(reg, s_tag, "instance out of range");

  /* HUM Ch 44 "Octal Serial Peripheral Interface (OSPI)" p 2986 */
  reg->LIOCFGCS[0] = 0U;
  reg->INTE        = 0U;
  reg->INTC        = (uint32_t)k_ra_xspi_ints_mask_all;

  s_xspi_state[instance].fn  = nullptr;
  s_xspi_state[instance].ctx = nullptr;
  return ra_mstp_disable(s_xspi_mstp_table[instance]);
}

ra_err_t ra_xspi_get_status(uint8_t instance, uint32_t* out_mask)
{
  RA_CHECK_NULL_PTR(out_mask, s_tag, "out_mask must not be nullptr");
  volatile r_xspi_regs_t* reg = ra_xspi(instance);
  RA_CHECK_NULL_PTR(reg, s_tag, "instance out of range");
  /* HUM Ch 44 "Octal Serial Peripheral Interface (OSPI)" p 2986 */
  *out_mask = reg->COMSTT;
  return k_ra_ok;
}

ra_err_t ra_xspi_clear_status(uint8_t instance, uint32_t mask)
{
  volatile r_xspi_regs_t* reg = ra_xspi(instance);
  RA_CHECK_NULL_PTR(reg, s_tag, "instance out of range");
  /* HUM Ch 44 "Octal Serial Peripheral Interface (OSPI)" p 2986 */
  reg->INTC = mask;
  return k_ra_ok;
}

ra_err_t ra_xspi_attach_handler(uint8_t instance, ra_xspi_event_fn_t fn, void* ctx)
{
  if (instance >= (uint8_t)k_ra_xspi_instance_count) {
    return k_ra_err_invalid_arg;
  }
  s_xspi_state[instance].fn  = fn;
  s_xspi_state[instance].ctx = ctx;
  return k_ra_ok;
}

void ra_xspi_dispatch(uint8_t instance)
{
  if (instance >= (uint8_t)k_ra_xspi_instance_count) {
    return;
  }
  volatile r_xspi_regs_t* reg = ra_xspi(instance);
  if (reg == nullptr) { /* GCOVR_EXCL_BR_LINE -- instance bounded above */
    return;             /* GCOVR_EXCL_LINE */
  }
  /* HUM Ch 44 "Octal Serial Peripheral Interface (OSPI)" p 2986 */
  /* Snapshot INTS + COMSTT, clear every pending interrupt flag,
   * then hand the INTS mask off to the user callback so it can
   * decide whether the transfer was successful or errored. */
  const uint32_t mask = reg->INTS;
  reg->INTC           = (uint32_t)k_ra_xspi_ints_mask_all;

  const ra_xspi_event_fn_t fn  = s_xspi_state[instance].fn;
  void* const              ctx = s_xspi_state[instance].ctx;
  if (fn != nullptr) {
    fn(ctx, mask);
  }
}

ra_err_t ra_xspi_enter_stop(uint8_t instance)
{
  if (instance >= (uint8_t)k_ra_xspi_instance_count) {
    return k_ra_err_invalid_arg;
  }
  return ra_mstp_disable(s_xspi_mstp_table[instance]);
}

ra_err_t ra_xspi_exit_stop(uint8_t instance)
{
  if (instance >= (uint8_t)k_ra_xspi_instance_count) {
    return k_ra_err_invalid_arg;
  }
  return ra_mstp_enable(s_xspi_mstp_table[instance]);
}
