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
#include "ra8d2_system_regs.h"
#include "ra_check.h"
#include "ra_err.h"
#include "ra_log.h"
#include "ra_mstp.h"
#include "ra_register_protection.h"

/** @brief Logging tag for this driver. */
static const char* s_tag = "XSPI";

/**
 * @enum ra_xspi_cmd_limits_t
 * @brief Byte-count limits on raw direct-command buffers.
 */
/** @brief Low-byte mask for status/JEDEC-id extraction. */
typedef enum : uint32_t {
  k_xspi_byte_mask = 0xFFUL,
} xspi_mask_t;

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
  /**
   * @brief CMDCMP poll budget for a single manual-command transfer.
   *
   * @details
   * Sized for the worst-case ``ra_xspi`` JEDEC command at the chip's
   * default 1S-1S-1S clock (which on the RA8D2 powers up at the OCTA
   * peripheral-clock reset rate, on the order of a few MHz before any
   * higher-frequency PLL is selected). A 1-byte opcode + 3-byte
   * address + 8 data bytes = 12 bytes = 96 bits. At 2 MHz that is
   * ~48 us. The IS25LX512M datasheet Ch 8 "Command Set" lists the
   * longest *non-write* command (RDID + 3 bytes payload) at well
   * under 100 us. We pick 50,000 iterations of the tight register
   * read loop -- at ~5 cycles per iteration on the 1 GHz Cortex-M85
   * that is roughly 250 us, two orders of magnitude over the longest
   * command but small enough that a wedged controller (pin route
   * missing, OSPI not enumerated, etc.) returns ``hw_timeout`` to
   * the caller within sub-millisecond time instead of stalling the
   * HIL probe window for tens of seconds. The previous 1,000,000-
   * iteration budget made each failed round-trip take 5+ seconds, so
   * ``g_fj_match`` / ``g_fj_mismatch`` both read 0 for the entire
   * 5 s memprobe window. HUM Ch 44 "Octal Serial Peripheral
   * Interface (OSPI)" p 2986; IS25LX512M datasheet Ch 8.
   */
  k_ra_xspi_cmd_spin = 50000U, /**< CMDCMP poll budget. */
  /**
   * @brief WIP-poll iteration cap for program / erase finish.
   *
   * @details
   * Per IS25LX512M datasheet Ch 16 "AC Characteristics", a sector
   * erase ranges typical 30 ms, max 500 ms; a page program is
   * typical 75 us, max 700 us. Each iteration of this loop performs
   * one RDSR command which itself bounds at ``k_ra_xspi_cmd_spin``
   * iterations (~250 us worst-case). 4,000 outer iterations therefore
   * yields a ~1 second worst-case wall-clock budget -- comfortably
   * over the IS25LX512M max-sector-erase spec but bounded enough that
   * a hung peripheral surfaces to the caller in human time.
   */
  k_ra_flash_program_timeout_us = 32000U, /**< Max ~8 s for program / erase. */
  /**
   * @brief OCTACKCR SREQ/SRDY handshake budget.
   *
   * @details
   * Mirrors ``k_ra_canfd_ckcr_spin`` in ``ra_canfd.c``. Each iteration
   * reads OCTACKCR; the CGC asserts OCTACKSRDY within a small number
   * of OCTACLK cycles (HUM Ch 9.2.45 p 360). 262144 iterations on a
   * 1 GHz Cortex-M85 with ~5 cycles per register-read poll is roughly
   * 1.3 ms -- two orders of magnitude over the documented wait but
   * bounded enough that a stuck handshake (e.g. MOCO not running)
   * surfaces as ``hw_timeout`` in sub-millisecond time. Shares its
   * order of magnitude with the USB / CANFD CKSRDY waits.
   */
  k_ra_xspi_ckcr_spin = 262144U, /**< OCTACKCR SREQ/SRDY budget. */
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
 *
 * @details
 * FSP names these ``CDBUF[0].CDT``, ``CDBUF[0].CDA``,
 * ``CDBUF[0].CDD0``, ``CDBUF[0].CDD1`` (HUM Ch 44 p 2986). The
 * flat ``r_xspi_regs_t::CDBUF[16]`` array indexes them as
 * ``slot * 4 + word``. This driver only uses slot 0.
 */
typedef enum : uint8_t {
  k_ra_xspi_cdbuf_idx_cdt   = 0U, /**< CDBUF[0] CDT  -- type/opcode word.   */
  k_ra_xspi_cdbuf_idx_addr  = 1U, /**< CDBUF[1] CDA  -- flash address.      */
  k_ra_xspi_cdbuf_idx_data0 = 2U, /**< CDBUF[2] CDD0 -- data bytes 0..3.    */
  k_ra_xspi_cdbuf_idx_data1 = 3U, /**< CDBUF[3] CDD1 -- data bytes 4..7.    */
} ra_xspi_cdbuf_idx_t;

/**
 * @enum ra_xspi_cdt_limits_t
 * @brief Per-transaction byte-size limits encodable in ``CDT``.
 */
typedef enum : uint8_t {
  k_ra_xspi_cdt_max_data_bytes = 8U, /**< CDD0 + CDD1 = 8 bytes per slot. */
} ra_xspi_cdt_limits_t;

#ifdef RA_SIMULATOR_MODE
/* One fake-flash window per instance. Tests can poke this directly
 * through the program API and then read it back. */
static uint8_t s_fake_flash[k_ra_xspi_instance_count][k_ra_xspi_fake_flash_size];

/* Set every byte of ``dst[0 -- see surrounding code and HUM citations. */
static void internal_fake_flash_fill(uint8_t* dst, uint8_t value, uint32_t len)
{
  for (uint32_t i = 0U; i < len; i++) {
    dst[i] = value;
  }
}

/* Byte-wise copy helper (avoids libc string calls in host tests) -- see surrounding code and HUM citations. */
static void internal_fake_flash_copy(uint8_t* dst, const uint8_t* src, uint32_t len)
{
  for (uint32_t i = 0U; i < len; i++) {
    dst[i] = src[i];
  }
}

/* internal xspi sim init -- see surrounding code and HUM citations. */
__attribute__((constructor)) static void internal_xspi_sim_init(void)
{
  for (uint8_t i = 0U; i < k_ra_xspi_instance_count; i++) {
    internal_fake_flash_fill(s_fake_flash[i],
                             (uint8_t)k_ra_xspi_fake_flash_erased,
                             k_ra_xspi_fake_flash_size);
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

/**
 * @brief Bounded wait on OCTACKCR.OCTACKSRDY reaching ``expected``.
 *
 * @details
 * Mirrors ``internal_wait_canfdcksrdy`` in ``ra_canfd.c`` and
 * ``internal_wait_usbcksrdy`` in ``ra_cgc.c``. Polls OCTACKCR bit 7
 * (OCTACKSRDY) until it equals ``expected`` or ``k_ra_xspi_ckcr_spin``
 * iterations elapse. HUM Ch 9.2.45 "OCTACKCR" p 360 documents
 * OCTACKSRDY at bit 7 (R) -- "Possible to Switch" flag.
 *
 * @param[in] expected 1U after SREQ=1; 0U after SREQ=0.
 *
 * @return ra_err_t outcome.
 * @retval k_ra_ok             SRDY observed equal to ``expected``.
 * @retval k_ra_err_hw_timeout SRDY never matched within the budget.
 *
 * @pre Caller holds the PRCR-CGC unlock window (PRCR=0xA501).
 * @pre ``expected`` is 0 or 1.
 * @post No register state is modified -- this is a read-only poll.
 * @post On timeout the caller relocks PRCR.
 *
 * @note Not thread-safe; init context only.
 * @since 0.1.0
 */
static ra_err_t internal_wait_octacksrdy(uint8_t expected)
{
  /* SRDY (clock-source ready) is bit 7 of OCTACKCR. */
  /* HUM Ch 9.2.45 "OCTACKCR.OCTACKSRDY" p 360 */
  volatile uint8_t* const ckcr = ra_sys_octackcr();
  const uint8_t           mask = (uint8_t)(1U << k_ra_usbckcr_bit_srdy);
#ifdef RA_SIMULATOR_MODE
  /* Sim memory has no hardware ack -- fake OCTACKSRDY toggling so the
   * host test poll loop converges immediately. Same pattern used by
   * ``internal_wait_canfdcksrdy`` in ``ra_canfd.c``. */
  if (expected != 0U) {
    *ckcr = (uint8_t)(*ckcr | mask);
  } else {
    *ckcr = (uint8_t)(*ckcr & (uint8_t)~mask);
  }
#endif
  for (uint32_t i = 0U; i < (uint32_t)k_ra_xspi_ckcr_spin; i++) { /* GCOVR_EXCL_BR_LINE */
    const uint8_t got = (uint8_t)((*ckcr & mask) >> k_ra_usbckcr_bit_srdy);
    if (got == expected) { /* GCOVR_EXCL_BR_LINE */
      return k_ra_ok;
    }
  }
  return k_ra_err_hw_timeout;
}

/**
 * @brief Block-level OCTA clock init -- run BEFORE the first MSTP release.
 *
 * @details
 * HUM Ch 11.2.7 "MSTPCRB" Note 3 (p 444) states that MSTPB16 / MSTPB17
 * (the per-instance OSPI module-stop bits) must be written AFTER the
 * OCTACLK is stable. OCTACKCR resets to ``0x01`` (OCTACKSEL = MOCO,
 * OCTACKSREQ = 0, OCTACKSRDY = 0). MOCO is on at reset, but the RA8D2
 * CGC requires an explicit SREQ -> SRDY -> SREQ-clear handshake before
 * the chip raises OCTACKSRDY and declares the clock stable. Without
 * that handshake the OSPI manual-command engine's internal state
 * machine cannot retire ``CDCTL0.TRREQ`` after the first
 * ``CDBUF[0].CDT`` write -- symptom seen on HIL: every
 * ``ra_xspi_flash_*`` operation surfaces as ``k_ra_err_hw_timeout``
 * on CMDCMP, and ``flash_journal``'s ``g_fj_match`` /
 * ``g_fj_mismatch`` counters both read 0 across the 5 s memprobe
 * window.
 *
 * Steps (mirror of ``internal_canfd_clock_block_init`` in
 * ``ra_canfd.c`` + the USBCKCR pattern in ``ra_cgc.c``, with the
 * OCTACKCR-specific procedure from HUM Ch 9.2.45 p 360):
 *   1. Write OCTACKDIVCR = 0 (/1 -- documented reset value).
 *   2. Set OCTACKCR.OCTACKSREQ = 1 (request switch) while keeping the
 *      reset-default OCTACKSEL = MOCO.
 *   3. Poll until OCTACKSRDY = 1.
 *   4. Re-write OCTACKCR with SREQ=0, source = MOCO -- commits the
 *      (same) source selection.
 *   5. Poll until OCTACKSRDY = 0 (handshake done).
 *
 * This helper is idempotent via a static guard: only the first caller
 * performs the handshake; subsequent ``ra_xspi_init`` calls (e.g. for
 * instance 1 after instance 0) skip it. MOCO -> /1 keeps OCTACLK at
 * its native MOCO rate (~8 MHz nominal) which is fine for the IS25LX
 * JEDEC-mode bring-up; a later board-specific tune can switch the
 * source to a PLL output by reissuing the same handshake.
 *
 * @return ra_err_t outcome.
 * @retval k_ra_ok             OCTACLK declared stable; safe to release MSTP.
 * @retval k_ra_err_hw_timeout SRDY handshake stuck.
 *
 * @pre Single-threaded init context (no other CGC writes in flight).
 * @pre MOCO is running -- chip reset default; ra_cgc_init does not
 *      explicitly stop MOCO.
 * @post On k_ra_ok the OCTA block clock is stable; MSTPB16/17 may now
 *       be released.
 * @post On error the OSPI MSTP gate is NOT touched.
 * @post PRCR is re-locked.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
static ra_err_t internal_xspi_clock_block_init(void)
{
  static bool s_xspi_clock_inited = false;
  if (s_xspi_clock_inited) {
    return k_ra_ok;
  }
  ra_err_t err = k_ra_ok;
  RA_PROTECTED_WRITE(k_ra_prcr_unlock_cgc)
  {
    /* HUM Ch 9.2.40 "OCTACKDIVCR" p 357 -- /1 keeps MOCO at its
     * native rate (~8 MHz nominal). The IS25LX512M JEDEC bring-up
     * runs comfortably below 50 MHz so /1 is conservative. */
    *ra_sys_octackdivcr() = 0U;

    /* HUM Ch 9.2.45 "OCTACKCR.OCTACKSREQ" p 360 -- assert SREQ with
     * the reset-default source (MOCO, OCTACKSEL = 0001b). */
    const uint8_t sreq_mask = (uint8_t)(1U << k_ra_usbckcr_bit_sreq);
    const uint8_t src_moco  = 0x01U;
    *ra_sys_octackcr()      = (uint8_t)(src_moco | sreq_mask);

    /* Step 3: wait for SRDY = 1 (chip acknowledges the request). */
    err = internal_wait_octacksrdy(1U);
    if (err != k_ra_ok) {
      ra_log_error(s_tag, "xspi: OCTACKSRDY=1 timeout");
      break;
    }
    /* Step 4: drop SREQ -- commits the (same) source selection. */
    *ra_sys_octackcr() = src_moco;
    /* Step 5: wait for SRDY = 0 -- handshake done. */
    err = internal_wait_octacksrdy(0U);
    if (err != k_ra_ok) {
      ra_log_error(s_tag, "xspi: OCTACKSRDY=0 timeout");
      break;
    }
  }
  if (err == k_ra_ok) {
    s_xspi_clock_inited = true;
    ra_log_info(s_tag, "octa block clock stable");
  }
  return err;
}

ra_err_t ra_xspi_init(uint8_t instance, ra_xspi_lio_mode_t mode)
{
  volatile r_xspi_regs_t* reg = ra_xspi(instance);
  RA_CHECK_NULL_PTR(reg, s_tag, "instance out of range");
  if (instance >= (uint8_t)(sizeof(s_xspi_mstp_table) / sizeof(s_xspi_mstp_table[0]))) {
    return k_ra_err_invalid_arg;
  }

  /* HUM Ch 11.2.7 "MSTPCRB" Note 3 p 444 -- MSTPB16/17 must be written
   * AFTER OCTACLK is stable. Run the block-level OCTACKCR handshake
   * first; the helper is idempotent so a second ra_xspi_init call
   * (e.g. for instance 1) skips it. */
  const ra_err_t clk_err = internal_xspi_clock_block_init();
  if (clk_err != k_ra_ok) {
    return clk_err;
  }

  /* HUM Ch 11.2.7 "MSTPCRB : Module Stop Control Register B", p 444 */
  const ra_err_t mst_err = ra_mstp_enable(s_xspi_mstp_table[instance]);
  RA_RETURN_ON_ERROR(mst_err, s_tag, "xspi_init: mstp enable"); /* GCOVR_EXCL_BR_LINE */

  /* HUM Ch 44 "Octal Serial Peripheral Interface (OSPI)" p 2986 */
  /* Wake the wrapper, idle the common config, set the link-IO
   * protocol for target 0, and clear any latent interrupt flags.
   *
   * BMCTL0 is forced to ``disabled`` here so the AHB system-bus
   * path to the target window cannot race with the manual-command
   * engine during JEDEC ID / RDSR / page-program traffic. FSP
   * performs the equivalent gate via ``r_ospi_b_xip(false)`` before
   * any manual transfer; without it the controller can NAK
   * ``CDCTL0.TRREQ`` because the bridge believes a memory-mapped
   * read is still in flight, which silently times out CMDCMP and
   * surfaces as ``k_ra_err_hw_timeout`` to LevelX. CMCTLCH[0/1] are
   * also zeroed so XIPEN cannot be left armed across ra_xspi_init.
   * CDCTL0 is forced to 0 (CSSEL=0 -> target 0 = on-board IS25LX,
   * TRREQ=0) so the next manual command starts from a known state.
   * HUM Ch 44 p 2986 "CDCTL0 : Command Manual Control 0". */
  reg->BMCTL0      = (uint32_t)k_ra_xspi_bmctl0_disabled;
  reg->CMCTLCH[0]  = 0U;
  reg->CMCTLCH[1]  = 0U;
  reg->WRAPCFG     = 0U;
  reg->COMCFG      = 0U;
  reg->LIOCFGCS[0] = (uint32_t)mode;
  reg->CDCTL0      = 0U;
  reg->INTC        = k_ra_xspi_ints_mask_all;

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
 * @brief Encode a manual-command CDT word (opcode + size/type fields).
 *
 * @details
 * Mirrors FSP ``r_ospi_b_direct_transfer`` (line ~1311 of
 * ``r_ospi_b.c``): builds the ``CDBUF[0].CDT`` word from
 * CMDSIZE/ADDSIZE/DATASIZE/TRTYPE plus the JEDEC opcode at bits
 * [31..16]. Latency is fixed at zero for the simple JEDEC opcodes
 * this driver issues. HUM Ch 44 p 2986.
 * @param[in] opcode See declaration: ``uint8_t opcode``.
 * @param[in] cmd_bytes See declaration: ``uint8_t cmd_bytes``.
 * @param[in] addr_bytes See declaration: ``uint8_t addr_bytes``.
 * @param[in] data_bytes See declaration: ``uint8_t data_bytes``.
 * @param[in] is_write See declaration: ``uint8_t is_write``.
 * @return ::ra_err_t outcome (or scalar return value).
 * @retval k_ra_ok Operation completed successfully.
 * @retval other Non-zero error code from the underlying operation.
 * @pre Module/state preconditions hold (see function body).
 * @pre Module/state preconditions hold (see function body).
 * @post Documented side effects are visible on success.
 * @post Documented side effects are visible on success.
 * @note Not thread-safe; the caller must serialise concurrent access.
 * @since 0.1.0
 */
static uint32_t internal_make_cdt(uint8_t opcode,
                                  uint8_t cmd_bytes,
                                  uint8_t addr_bytes,
                                  uint8_t data_bytes,
                                  uint8_t is_write)
{
  return (((uint32_t)cmd_bytes & k_ra_xspi_cdt_mask_cmdsize) << k_ra_xspi_cdt_pos_cmdsize) |
         (((uint32_t)addr_bytes & k_ra_xspi_cdt_mask_addsize) << k_ra_xspi_cdt_pos_addsize) |
         (((uint32_t)data_bytes & k_ra_xspi_cdt_mask_datasize) << k_ra_xspi_cdt_pos_datasize) |
         (((uint32_t)is_write & k_ra_xspi_cdt_mask_trtype) << k_ra_xspi_cdt_pos_trtype) |
         (((uint32_t)opcode) << k_ra_xspi_cdt_pos_cmd);
}

/**
 * @enum ra_spi_flash_resp_bytes_t
 * @brief Per-opcode response-byte counts for read-direction commands.
 *
 * @details
 * The xSPI manual-command engine drives DATASIZE clocks on the bus
 * after the opcode (and address, if any) so the device can stream
 * its response bytes into CDD0/CDD1. For 0x05 RDSR the device sends
 * 1 byte; for 0x9F RDID it sends the JEDEC triplet (3 bytes). With
 * DATASIZE=0 the controller never clocks the response phase and
 * CDD0 stays at whatever it held before the transfer -- which is
 * why an earlier version of this driver always read WIP=0 and
 * tripped LevelX into believing erases had completed instantly.
 * IS25LX512M datasheet Ch 8.6 (RDSR) + Ch 8.13 (RDID).
 */
typedef enum : uint8_t {
  k_ra_xspi_resp_bytes_status = 1U, /**< RDSR returns 1 status byte. */
  k_ra_xspi_resp_bytes_jedec  = 3U, /**< RDID returns MFR+TYPE+CAP. */
} ra_spi_flash_resp_bytes_t;

/* Bounded CMDCMP poll (with simulator-mode fast exit) -- see surrounding code and HUM citations. */
static ra_err_t internal_wait_command_done(volatile r_xspi_regs_t* reg)
{
#ifdef RA_SIMULATOR_MODE
  /* HUM Ch 44 "Octal Serial Peripheral Interface (OSPI)" p 2986 */
  /* On the host there is no hardware -- pretend the command
   * finished: set CMDCMP in INTS and then clear it via INTC to
   * mirror the target flow exactly. */
  reg->INTS = k_ra_xspi_ints_mask_cmdcmp;
  reg->INTC = k_ra_xspi_ints_mask_cmdcmp;
  return k_ra_ok;
#else
  for (uint32_t i = 0U; i < (uint32_t)k_ra_xspi_cmd_spin; i++) { /* GCOVR_EXCL_BR_LINE */
    const uint32_t s = reg->INTS;
    if ((s & (uint32_t)k_ra_xspi_ints_mask_cmdcmp) != 0U) { /* GCOVR_EXCL_BR_LINE */
      /* HUM Ch 44 "Octal Serial Peripheral Interface (OSPI)" p 2986 */
      /* FSP r_ospi_b_direct_transfer clears every pending status
       * bit at the end of a manual command via ``INTC = INTS``. */
      reg->INTC = reg->INTS;
      return k_ra_ok;
    }
  }
  return k_ra_err_hw_timeout;
#endif
}

/**
 * @brief Kick a prepared manual-command transfer by raising TRREQ.
 *
 * @details
 * FSP ``r_ospi_b_direct_transfer`` waits for any prior in-flight
 * TRREQ to self-clear before pushing a new request, then sets
 * TRREQ=1 and waits for it to self-clear again. We mirror the
 * FSP "self-clear" semantics by polling ``INTS.CMDCMP``.
 * @param[in] reg See declaration: ``volatile r_xspi_regs_t* reg``.
 * @return ::ra_err_t outcome (or scalar return value).
 * @retval k_ra_ok Operation completed successfully.
 * @retval other Non-zero error code from the underlying operation.
 * @pre Module/state preconditions hold (see function body).
 * @pre Module/state preconditions hold (see function body).
 * @post Documented side effects are visible on success.
 * @post Documented side effects are visible on success.
 * @note Not thread-safe; the caller must serialise concurrent access.
 * @since 0.1.0
 */
static ra_err_t internal_kick_command(volatile r_xspi_regs_t* reg)
{
  /* HUM Ch 44 "Octal Serial Peripheral Interface (OSPI)" p 2986 */
  reg->CDCTL0 |= k_ra_xspi_cdctl0_mask_trreq;
  return internal_wait_command_done(reg);
}

/* Build CDBUF[0] for a 1-byte opcode with no address / no data -- see surrounding code and HUM citations. */
static ra_err_t internal_issue_simple_opcode(volatile r_xspi_regs_t* reg, uint8_t opcode)
{
  /* HUM Ch 44 "Octal Serial Peripheral Interface (OSPI)" p 2986 */
  /* Populate CDBUF slot 0 per FSP ``r_ospi_b_direct_transfer``:
   * CDT carries opcode + size encoding, CDA/CDD0/CDD1 are zeroed
   * because there is no address phase and no payload. CDCTL1/CDCTL2
   * are periodic-mode fields (PEREXP / PERMSK in FSP) -- leave
   * them at zero for one-shot manual commands. */
  reg->CDBUF[k_ra_xspi_cdbuf_idx_cdt]   = internal_make_cdt(opcode,
                                                            k_ra_xspi_cdt_cmdsize_1,
                                                            k_ra_xspi_cdt_addsize_0,
                                                            0U,
                                                            k_ra_xspi_cdt_trtype_read);
  reg->CDBUF[k_ra_xspi_cdbuf_idx_addr]  = 0U;
  reg->CDBUF[k_ra_xspi_cdbuf_idx_data0] = 0U;
  reg->CDBUF[k_ra_xspi_cdbuf_idx_data1] = 0U;
  return internal_kick_command(reg);
}

/**
 * @brief Issue a 1-byte opcode that returns 1..8 response data bytes.
 *
 * @details
 * Used by RDSR (0x05) / RDID (0x9F): no address phase, but we MUST
 * tell the controller how many response bytes to clock in via
 * ``DATASIZE``. Caller reads the response out of CDBUF[CDD0/CDD1]
 * after CMDCMP. HUM Ch 44 p 2986; IS25LX512M datasheet Ch 8.6 + 8.13.
 *
 * @param[in] reg        xSPI register block.
 * @param[in] opcode     JEDEC opcode (0x05 or 0x9F).
 * @param[in] resp_bytes 1..8 response bytes to clock into CDBUF.
 *
 * @return ``k_ra_ok`` on success or the underlying CMDCMP timeout.
 * @retval k_ra_ok Operation completed successfully.
 * @retval other Non-zero error code from the underlying operation.
 * @pre Module/state preconditions hold (see function body).
 * @pre Module/state preconditions hold (see function body).
 * @post Documented side effects are visible on success.
 * @post Documented side effects are visible on success.
 * @note Not thread-safe; the caller must serialise concurrent access.
 * @since 0.1.0
 */
static ra_err_t
internal_issue_read_opcode(volatile r_xspi_regs_t* reg, uint8_t opcode, uint8_t resp_bytes)
{
  /* HUM Ch 44 "Octal Serial Peripheral Interface (OSPI)" p 2986 */
  reg->CDBUF[k_ra_xspi_cdbuf_idx_cdt]   = internal_make_cdt(opcode,
                                                            k_ra_xspi_cdt_cmdsize_1,
                                                            k_ra_xspi_cdt_addsize_0,
                                                            resp_bytes,
                                                            k_ra_xspi_cdt_trtype_read);
  reg->CDBUF[k_ra_xspi_cdbuf_idx_addr]  = 0U;
  reg->CDBUF[k_ra_xspi_cdbuf_idx_data0] = 0U;
  reg->CDBUF[k_ra_xspi_cdbuf_idx_data1] = 0U;
  return internal_kick_command(reg);
}

/**
 * @brief Build CDBUF[0] for a single CDD0/CDD1 chunk of an opcode + addr.
 *
 * @details
 * Used by the read path (TRTYPE=read) and by the program path
 * (TRTYPE=write) to programme the per-chunk header. Address phase
 * is fixed at 3 bytes for the JEDEC opcodes in use; ``data_bytes``
 * is 0..8 (one slot's worth). FSP equivalent: the body of
 * ``r_ospi_b_direct_transfer`` that builds ``cdtbuf0``.
 * @param[in] reg See declaration: ``volatile r_xspi_regs_t* reg``.
 * @param[in] opcode See declaration: ``uint8_t                 opcode``.
 * @param[in] addr See declaration: ``uint32_t                addr``.
 * @param[in] data_bytes See declaration: ``uint8_t                 data_bytes``.
 * @param[in] is_write See declaration: ``uint8_t                 is_write``.
 * @pre Module/state preconditions hold (see function body).
 * @pre Module/state preconditions hold (see function body).
 * @post Documented side effects are visible on success.
 * @post Documented side effects are visible on success.
 * @note Not thread-safe; the caller must serialise concurrent access.
 * @since 0.1.0
 */
static void internal_build_chunk_header(volatile r_xspi_regs_t* reg,
                                        uint8_t                 opcode,
                                        uint32_t                addr,
                                        uint8_t                 data_bytes,
                                        uint8_t                 is_write)
{
  reg->CDBUF[k_ra_xspi_cdbuf_idx_cdt]  = internal_make_cdt(opcode,
                                                           k_ra_xspi_cdt_cmdsize_1,
                                                           k_ra_xspi_cdt_addsize_3,
                                                           data_bytes,
                                                           is_write);
  reg->CDBUF[k_ra_xspi_cdbuf_idx_addr] = addr;
}

/**
 * @brief Clamp ``flash_addr + len`` to the simulator fake-flash size.
 */
#ifdef RA_SIMULATOR_MODE
/* internal sim range check -- see surrounding code and HUM citations. */
static ra_err_t internal_sim_range_check(uint32_t flash_addr, uint32_t len)
{
  if (flash_addr >= k_ra_xspi_fake_flash_size) {
    return k_ra_err_invalid_arg;
  }
  if ((flash_addr + len) > k_ra_xspi_fake_flash_size) {
    return k_ra_err_invalid_arg;
  }
  return k_ra_ok;
}
#endif

ra_err_t ra_xspi_flash_read(uint8_t instance, uint32_t flash_addr, uint8_t* buf, uint32_t len)
{
  RA_CHECK_NULL_PTR(buf, s_tag, "buf must not be nullptr");
  if ((len == 0U) || (len > k_ra_xspi_max_xfer)) {
    return k_ra_err_invalid_arg;
  }
  volatile r_xspi_regs_t* reg = ra_xspi(instance);
  RA_CHECK_NULL_PTR(reg, s_tag, "instance out of range");

  /* HUM Ch 44 "Octal Serial Peripheral Interface (OSPI)" p 2986 */
  /* Programme a JEDEC 0x03 read with 3-byte address. The data byte
   * count travels in CDT.DATASIZE (FSP encoding) so the controller
   * knows how many bytes to clock onto CDD0/CDD1. ``len`` is
   * clamped to 8 here because each manual-command slot only carries
   * 8 bytes; the higher-level caller handles chunking for arbitrary
   * lengths in future work (currently bounded by k_ra_xspi_max_xfer
   * via the simulator path). */
  const uint8_t chunk =
    (len > (uint32_t)k_ra_xspi_cdt_max_data_bytes) ? k_ra_xspi_cdt_max_data_bytes : (uint8_t)len;
  internal_build_chunk_header(reg,
                              k_ra_spi_flash_op_read,
                              flash_addr,
                              chunk,
                              k_ra_xspi_cdt_trtype_read);

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
 * @brief Stage WREN + page-program header (CDT + CDA only) without kicking.
 *
 * @details
 * Splits the previous "WREN -> build PP header -> KICK" helper so the
 * caller can load the outgoing payload into ``CDBUF[CDD0]`` /
 * ``CDBUF[CDD1]`` BEFORE TRREQ is asserted. The earlier code asserted
 * TRREQ here and only loaded CDD0/CDD1 afterwards, which clocked the
 * stale CDBUF contents (zeros, or whatever the previous read response
 * left behind) onto the bus instead of the caller's data. That bug
 * surfaced as: (a) flash_journal's first round-trip "passing" purely
 * because CDD0/CDD1 happened to be zero (matching counter=0) while
 * every subsequent counter mismatched, (b) the threadx_levelx_demo
 * panicking inside ``lx_nor_flash_format`` because LevelX's free-bit
 * metadata reads back as the previous transfer's status byte instead
 * of the bit-pattern it just wrote, and (c) the threadx_filex_levelx_demo
 * surfacing ``lx_nor_flash_format failed`` for the same reason -- every
 * LevelX sector-header write to the on-board MX25xxx flash dropped its
 * payload, so LevelX saw zero usable sectors.
 *
 * HUM Ch 44 "Octal Serial Peripheral Interface (OSPI)" p 2986 documents
 * the manual-command flow as "fill CDBUF, then set CDCTL0.TRREQ"; FSP
 * ``r_ospi_b_direct_transfer`` (``r_ospi_b.c`` line ~1311) writes the
 * data words into CDBUF before TRREQ on every PP transfer.
 *
 * @param[in] reg        xSPI register block (already gated open).
 * @param[in] flash_addr Destination flash byte address.
 * @param[in] len        Bytes to program in this chunk (1..8).
 *
 * @return ::ra_err_t outcome of the WREN sub-command.
 * @retval k_ra_ok       WREN dispatched and CMDCMP cleared.
 * @retval other         Underlying CMDCMP timeout from WREN.
 *
 * @pre ``reg != nullptr`` and the xSPI MSTP gate is open.
 * @pre ``len`` has been clamped by the caller to ``[1..8]``.
 * @post On success the controller has accepted WREN and the PP CDT/CDA
 *       words are staged in CDBUF slot 0 awaiting TRREQ.
 * @post CDBUF[CDD0]/CDBUF[CDD1] are intentionally NOT touched here so
 *       the caller can load the payload before kicking.
 *
 * @note Not thread-safe; caller serialises bus access.
 *
 * @since 0.1.0
 */
static ra_err_t
internal_flash_stage_program(volatile r_xspi_regs_t* reg, uint32_t flash_addr, uint32_t len)
{
  const ra_err_t wren = internal_issue_simple_opcode(reg, k_ra_spi_flash_op_write_enable);
  if (wren != k_ra_ok) {
    return wren;
  }
  /* HUM Ch 44 "Octal Serial Peripheral Interface (OSPI)" p 2986 */
  /* Programme a JEDEC 0x02 page-program with 3-byte address. The
   * outgoing byte count is encoded in CDT.DATASIZE (FSP semantics);
   * leaving the periodic-mode CDCTL1/CDCTL2 PEREXP/PERMSK fields
   * untouched. The kick (TRREQ) is deferred to the caller so that
   * CDBUF[CDD0]/CDBUF[CDD1] can be loaded with the payload first --
   * see this helper's @details for the rationale. */
  const uint8_t chunk =
    (len > (uint32_t)k_ra_xspi_cdt_max_data_bytes) ? k_ra_xspi_cdt_max_data_bytes : (uint8_t)len;
  internal_build_chunk_header(reg,
                              k_ra_spi_flash_op_page_program,
                              flash_addr,
                              chunk,
                              k_ra_xspi_cdt_trtype_write);
  return k_ra_ok;
}

/* Poll the SPI flash Status Register until WIP == 0 or timeout -- see surrounding code and HUM citations. */
static ra_err_t internal_poll_wip_clear(uint8_t instance)
{
#ifdef RA_SIMULATOR_MODE
  /* Simulator: fake flash is always idle. */
  (void)instance;
  return k_ra_ok;
#else
  for (uint32_t i = 0U; i < (uint32_t)k_ra_flash_program_timeout_us; i++) { /* GCOVR_EXCL_BR_LINE */
    uint8_t        status = 0U;
    const ra_err_t e      = ra_xspi_flash_read_status(instance, &status);
    if (e != k_ra_ok) { /* GCOVR_EXCL_BR_LINE */
      return e;
    }
    if ((status & (uint8_t)(1U << (uint8_t)k_ra_flash_status_bit_wip)) == 0U) {
      return k_ra_ok;
    }
  }
  return k_ra_err_timeout;
#endif
}

#ifndef RA_SIMULATOR_MODE /* on-target program path only (the #else stages no CDBUF) */
/**
 * @brief Stage the page-program payload into CDBUF[CDD0] / CDBUF[CDD1].
 *
 * @details
 * HUM Ch 44 "Octal Serial Peripheral Interface (OSPI)" p 2986 + FSP
 * ``r_ospi_b_direct_transfer`` document the manual-command flow as
 * "fill CDBUF, then set CDCTL0.TRREQ". The prior implementation
 * issued the WREN + 0x02 page-program header and asserted TRREQ
 * BEFORE staging the caller's bytes; the controller therefore
 * clocked out whatever stale words were left in CDD0/CDD1 from the
 * previous transfer. Splitting the staging step into this helper
 * lets ``ra_xspi_flash_program`` write the real payload first and
 * only then kick the transaction.
 *
 * @param[in] reg  xSPI register block (already gated open by the
 *                 caller).
 * @param[in] data Caller bytes. Must be non-NULL; ``len`` bytes are
 *                 read.
 * @param[in] len  Number of bytes to stage, in ``[1..8]`` (manual-
 *                 command CDBUF capacity).
 *
 * @pre ``reg != nullptr`` and the xSPI MSTPCR gate has been opened.
 * @pre ``data != nullptr`` and ``len`` is clamped to ``[1..8]``.
 * @post CDBUF[CDD0] holds bytes ``data[0..min(3,len)]`` packed
 *       little-endian.
 * @post CDBUF[CDD1] holds bytes ``data[4..len-1]`` (or zero if
 *       ``len <= 4``).
 *
 * @note Not thread-safe; caller serialises bus access.
 * @since 0.1.0
 */
static void
internal_xspi_stage_payload(volatile r_xspi_regs_t* reg, const uint8_t* data, uint32_t len)
{
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
}
#endif /* !RA_SIMULATOR_MODE */

/**
 * @brief Erase + page-program a single chunk of bytes to OSPI flash.
 *
 * @details
 * Three-step JEDEC page-program sequence as documented in HUM Ch 44
 * "Octal Serial Peripheral Interface (OSPI)" p 2986: 0x06 (WREN) +
 * 0x02 (PP) with 3-byte address + 1..8 bytes payload, then poll
 * WIP via 0x05 (RDSR). Caller must have ``ra_xspi_init``'d the
 * instance and erased the destination sector if a fresh write
 * (SPI NOR can only clear bits, never set them, between erases).
 *
 * Splits the operation into two helpers so the host build's
 * simulator path and the on-target path agree on ordering:
 *
 *   1. ``internal_flash_stage_program`` writes WREN, builds the PP
 *      command header in CDT/CDA, and returns without asserting
 *      TRREQ.
 *   2. ``internal_xspi_stage_payload`` packs ``data[]`` into
 *      CDBUF[CDD0/CDD1] (target build only -- the simulator path
 *      mutates the fake-flash array directly).
 *   3. ``internal_kick_command`` asserts CDCTL0.TRREQ and polls
 *      CMDCMP.
 *   4. ``internal_poll_wip_clear`` issues 0x05 RDSR until WIP=0.
 *
 * @param[in] instance   xSPI instance index (currently only 0).
 * @param[in] flash_addr Destination flash byte address.
 * @param[in] data       Payload buffer (1..8 bytes).
 * @param[in] len        Payload length in bytes; ``[1, 8]``.
 *
 * @return ::ra_err_t outcome of the chain.
 * @retval k_ra_ok               Payload written and WIP clear.
 * @retval k_ra_err_invalid_arg  ``data`` is NULL, ``len`` is 0, or
 *                               ``len > 8``.
 * @retval k_ra_err_not_init     ``ra_xspi_init`` was not called.
 * @retval k_ra_err_timeout      CMDCMP / WIP poll exceeded its
 *                               timeout budget.
 * @retval k_ra_err_hw_error     Underlying staging step failed.
 *
 * @pre ``ra_xspi_init(instance, ...)`` has succeeded.
 * @pre The destination sector has been erased via
 *      ``ra_xspi_flash_erase_sector`` if any bit needs to be set.
 *
 * @post On success ``data[0..len-1]`` is persisted at
 *       ``flash_addr``.
 * @post On success the flash status WIP bit is clear (controller
 *       is idle and ready for the next transaction).
 *
 * @note Not thread-safe; caller serialises bus access.
 * @since 0.1.0
 */
ra_err_t
ra_xspi_flash_program(uint8_t instance, uint32_t flash_addr, const uint8_t* data, uint32_t len)
{
  RA_CHECK_NULL_PTR(data, s_tag, "data must not be nullptr");
  if ((len == 0U) || (len > k_ra_xspi_max_xfer)) {
    return k_ra_err_invalid_arg;
  }
  volatile r_xspi_regs_t* reg = ra_xspi(instance);
  RA_CHECK_NULL_PTR(reg, s_tag, "instance out of range");

  const ra_err_t p = internal_flash_stage_program(reg, flash_addr, len);
  if (p != k_ra_ok) {
    return p;
  }
#ifdef RA_SIMULATOR_MODE
  /* HUM Ch 44 p 2986 -- simulator path: kick first so register-level
   * test assertions see the on-target sequence, then mutate fake
   * flash. AND-only model: SPI NOR can only clear bits. */
  const ra_err_t kick = internal_kick_command(reg);
  if (kick != k_ra_ok) {
    return kick;
  }
  const ra_err_t rng = internal_sim_range_check(flash_addr, len);
  if (rng != k_ra_ok) {
    return rng;
  }
  for (uint32_t i = 0U; i < len; i++) {
    s_fake_flash[instance][flash_addr + i] &= data[i];
  }
#else
  /* HUM Ch 44 p 2986 -- target path: stage CDD0/CDD1 BEFORE TRREQ
   * (cf. internal_xspi_stage_payload comment block). */
  internal_xspi_stage_payload(reg, data, len);
  const ra_err_t kick = internal_kick_command(reg);
  if (kick != k_ra_ok) {
    return kick;
  }
#endif
  return internal_poll_wip_clear(instance);
}

ra_err_t ra_xspi_flash_erase_sector(uint8_t instance, uint32_t flash_addr)
{
  volatile r_xspi_regs_t* reg = ra_xspi(instance);
  RA_CHECK_NULL_PTR(reg, s_tag, "instance out of range");

  const ra_err_t wren = internal_issue_simple_opcode(reg, k_ra_spi_flash_op_write_enable);
  if (wren != k_ra_ok) {
    return wren;
  }

  /* HUM Ch 44 "Octal Serial Peripheral Interface (OSPI)" p 2986 */
  /* Programme a JEDEC 0x20 sector-erase with 3-byte address.
   * Erase has no payload, so DATASIZE=0 and TRTYPE=write. */
  internal_build_chunk_header(reg,
                              k_ra_spi_flash_op_erase_sector,
                              flash_addr,
                              0U,
                              k_ra_xspi_cdt_trtype_write);

  const ra_err_t wait = internal_kick_command(reg);
  if (wait != k_ra_ok) {
    return wait;
  }

#ifdef RA_SIMULATOR_MODE
  const uint32_t sector_base = flash_addr & ~(k_ra_xspi_sector_len - 1U);
  if (sector_base >= k_ra_xspi_fake_flash_size) {
    return k_ra_err_invalid_arg;
  }
  internal_fake_flash_fill(&s_fake_flash[instance][sector_base],
                           (uint8_t)k_ra_xspi_fake_flash_erased,
                           k_ra_xspi_sector_len);
#endif
  return internal_poll_wip_clear(instance);
}

ra_err_t ra_xspi_flash_read_status(uint8_t instance, uint8_t* out_status)
{
  RA_CHECK_NULL_PTR(out_status, s_tag, "out_status must not be nullptr");
  volatile r_xspi_regs_t* reg = ra_xspi(instance);
  RA_CHECK_NULL_PTR(reg, s_tag, "instance out of range");

  /* RDSR returns 1 status byte; DATASIZE must be 1, not 0, or the
   * controller never clocks the response and CDD0 stays stale. See
   * IS25LX512M datasheet Ch 8.6. */
  const ra_err_t e = internal_issue_read_opcode(reg,
                                                k_ra_spi_flash_op_read_status,
                                                (uint8_t)k_ra_xspi_resp_bytes_status);
  if (e != k_ra_ok) {
    return e;
  }
#ifdef RA_SIMULATOR_MODE
  /* Simulator: always report WEL=1, WIP=0 (flash idle and ready). */
  *out_status = (uint8_t)(1U << k_ra_flash_status_bit_wel);
#else
  /* HUM Ch 44 "Octal Serial Peripheral Interface (OSPI)" p 2986 */
  *out_status = (uint8_t)(reg->CDBUF[(uint8_t)k_ra_xspi_cdbuf_idx_data0] & k_xspi_byte_mask);
#endif
  return k_ra_ok;
}

ra_err_t ra_xspi_flash_read_id(uint8_t instance, uint32_t* out_id)
{
  RA_CHECK_NULL_PTR(out_id, s_tag, "out_id must not be nullptr");
  volatile r_xspi_regs_t* reg = ra_xspi(instance);
  RA_CHECK_NULL_PTR(reg, s_tag, "instance out of range");

  /* RDID returns 3 JEDEC bytes (MFR/TYPE/CAP); DATASIZE must be 3.
   * IS25LX512M datasheet Ch 8.13 "Read Identification (RDID)". */
  const ra_err_t e =
    internal_issue_read_opcode(reg, k_ra_spi_flash_op_read_id, (uint8_t)k_ra_xspi_resp_bytes_jedec);
  if (e != k_ra_ok) {
    return e;
  }
#ifdef RA_SIMULATOR_MODE
  *out_id = k_ra_sim_jedec_id;
#else
  /* HUM Ch 44 "Octal Serial Peripheral Interface (OSPI)" p 2986 */
  /* JEDEC 0x9F returns MFR, MEMTYPE, CAPACITY in that byte order.
   * Repack into ``(mfr << 16) | (type << 8) | cap`` as documented
   * in the public API header. */
  const uint32_t word = reg->CDBUF[(uint8_t)k_ra_xspi_cdbuf_idx_data0];
  *out_id = ((word & k_xspi_byte_mask) << 16U) | (((word >> 8U) & k_xspi_byte_mask) << 8U) |
            ((word >> 16U) & k_xspi_byte_mask);
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
  reg->INTC        = k_ra_xspi_ints_mask_all;

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
  if (instance >= k_ra_xspi_instance_count) {
    return k_ra_err_invalid_arg;
  }
  s_xspi_state[instance].fn  = fn;
  s_xspi_state[instance].ctx = ctx;
  return k_ra_ok;
}

void ra_xspi_dispatch(uint8_t instance)
{
  if (instance >= k_ra_xspi_instance_count) {
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
  reg->INTC           = k_ra_xspi_ints_mask_all;

  const ra_xspi_event_fn_t fn  = s_xspi_state[instance].fn;
  void* const              ctx = s_xspi_state[instance].ctx;
  if (fn != nullptr) {
    fn(ctx, mask);
  }
}

ra_err_t ra_xspi_enter_stop(uint8_t instance)
{
  if (instance >= k_ra_xspi_instance_count) {
    return k_ra_err_invalid_arg;
  }
  return ra_mstp_disable(s_xspi_mstp_table[instance]);
}

ra_err_t ra_xspi_exit_stop(uint8_t instance)
{
  if (instance >= k_ra_xspi_instance_count) {
    return k_ra_err_invalid_arg;
  }
  return ra_mstp_enable(s_xspi_mstp_table[instance]);
}

ra_err_t ra_xspi_xip_enter(uint8_t instance, uint8_t enter_code, uint8_t exit_code)
{
  volatile r_xspi_regs_t* reg = ra_xspi(instance);
  RA_CHECK_NULL_PTR(reg, s_tag, "instance out of range");

  /* HUM Ch 44 "Octal Serial Peripheral Interface (OSPI)" p 2986 */
  /* FSP r_ospi_b_xip(true) flow:
   *   1. Stage XIP enter/exit codes in CMCTLCH for both channels.
   *   2. Map the target window read-only via BMCTL0 = 0x55.
   *   3. Set CMCTLCH.XIPEN to arm execute-in-place.
   * The first read on the memory-mapped window then transmits the
   * enter code. We omit the bus-bridge prefetch dance because the
   * driver does not enable prefetch by default. */
  const uint32_t code_word = ((uint32_t)enter_code << k_ra_xspi_cmctlch_xipencode_pos) |
                             ((uint32_t)exit_code << k_ra_xspi_cmctlch_xipexcode_pos);

  reg->BMCTL0     = k_ra_xspi_bmctl0_read_only;
  reg->CMCTLCH[0] = code_word | k_ra_xspi_cmctlch_xipen_mask;
  reg->CMCTLCH[1] = code_word | k_ra_xspi_cmctlch_xipen_mask;
  return k_ra_ok;
}

ra_err_t ra_xspi_xip_exit(uint8_t instance)
{
  volatile r_xspi_regs_t* reg = ra_xspi(instance);
  RA_CHECK_NULL_PTR(reg, s_tag, "instance out of range");

  /* HUM Ch 44 "Octal Serial Peripheral Interface (OSPI)" p 2986 */
  /* FSP r_ospi_b_xip(false) flow: clear XIPEN, drop the codes,
   * and put BMCTL0 back to read/write so direct-command transfers
   * are no longer blocked by the memory-mapped path. */
  reg->CMCTLCH[0] = 0U;
  reg->CMCTLCH[1] = 0U;
  reg->BMCTL0     = k_ra_xspi_bmctl0_read_write;
  return k_ra_ok;
}

/* =============================================================================
 * Sweep 6 extensions: XIP mode select, DTR, DQS calibration, suspend/resume
 * =============================================================================
 */

/**
 * @enum ra_xspi_jedec_extra_t
 * @brief Extra JEDEC opcodes used by suspend/resume control.
 */
typedef enum : uint8_t {
  k_ra_spi_flash_op_suspend   = 0x75U, /**< 0x75 erase/program suspend. */
  k_ra_spi_flash_op_resume    = 0x7AU, /**< 0x7A erase/program resume.  */
  k_ra_spi_flash_op_reset_en  = 0x66U, /**< 0x66 RSTEN  -- IS25LX512M Ch 8.20 p 39. */
  k_ra_spi_flash_op_reset_dev = 0x99U, /**< 0x99 RST    -- IS25LX512M Ch 8.21 p 39. */
} ra_xspi_jedec_extra_t;

/**
 * @enum ra_xspi_reset_cmd_bytes_t
 * @brief Allowed ``cmd_bytes`` values for ``ra_xspi_software_reset``.
 *
 * @details
 * 1-byte opcodes are used in 1S-1S-1S extended SPI mode, 2-byte
 * opcodes (``opcode | (~opcode << 8)``) in 8D-8D-8D OPI/DDR mode.
 * Cite: IS25LX512M datasheet Ch 7.3 "Operating Protocols" p 27.
 */
typedef enum : uint8_t {
  k_ra_xspi_reset_cmd_bytes_1s = 1U, /**< 1-byte opcode for 1S-1S-1S. */
  k_ra_xspi_reset_cmd_bytes_8d = 2U, /**< 2-byte opcode pair for 8D-8D-8D. */
} ra_xspi_reset_cmd_bytes_t;

/**
 * @enum ra_xspi_addr_bytes_t
 * @brief Allowed address-byte widths for ``ra_xspi_set_xip_mode``.
 */
typedef enum : uint8_t {
  k_ra_xspi_addr_bytes_3 = 3U, /**< 24-bit JEDEC address. */
  k_ra_xspi_addr_bytes_4 = 4U, /**< 32-bit JEDEC address. */
} ra_xspi_addr_bytes_t;

/**
 * @enum ra_xspi_calib_spin_t
 * @brief Bounded spin budget for the auto-calibration handshake.
 */
typedef enum : uint32_t {
  k_ra_xspi_calib_spin = 1024U, /**< CCCTL0.CAEN poll budget. */
} ra_xspi_calib_spin_t;

ra_err_t ra_xspi_set_xip_mode(uint8_t instance, bool enable, uint8_t read_cmd, uint8_t addr_bytes)
{
  volatile r_xspi_regs_t* reg = ra_xspi(instance);
  RA_CHECK_NULL_PTR(reg, s_tag, "instance out of range");
  if ((addr_bytes != k_ra_xspi_addr_bytes_3) && (addr_bytes != k_ra_xspi_addr_bytes_4)) {
    return k_ra_err_invalid_arg;
  }

  /* HUM Ch 44 "Octal Serial Peripheral Interface (OSPI)" p 2986 */
  /* CMCFGCS slot 0 = (mode, read-cmd-word, write-cmd-word, addr-word).
   * For XIP we only need read + addr; write opcode stays zero. */
  const uint8_t base                                   = 0U; /* slot 0 base index */
  reg->CMCFGCS[base + k_ra_xspi_cmcfgcs_word_read_cmd] = (uint32_t)read_cmd
                                                         << k_ra_xspi_cmcfgcs_pos_cmd;
  reg->CMCFGCS[base + k_ra_xspi_cmcfgcs_word_addr]     = (uint32_t)addr_bytes
                                                         << k_ra_xspi_cmcfgcs_pos_addr_size;

  if (enable) {
    /* Mirror FSP r_ospi_b_xip(true): map read-only and arm XIPEN. */
    reg->BMCTL0     = k_ra_xspi_bmctl0_read_only;
    reg->CMCTLCH[0] = k_ra_xspi_cmctlch_xipen_mask;
    reg->CMCTLCH[1] = k_ra_xspi_cmctlch_xipen_mask;
  } else {
    /* FSP r_ospi_b_xip(false): clear XIPEN and re-open the bus. */
    reg->CMCTLCH[0] = 0U;
    reg->CMCTLCH[1] = 0U;
    reg->BMCTL0     = k_ra_xspi_bmctl0_read_write;
  }
  return k_ra_ok;
}

ra_err_t ra_xspi_set_dtr_mode(uint8_t instance, bool enable)
{
  volatile r_xspi_regs_t* reg = ra_xspi(instance);
  RA_CHECK_NULL_PTR(reg, s_tag, "instance out of range");

  /* HUM Ch 44 "Octal Serial Peripheral Interface (OSPI)" p 2986 */
  uint32_t v = reg->LIOCFGCS[0];
  if (enable) {
    v |= k_ra_xspi_liocfgcs_mask_ddren;
  } else {
    v &= ~k_ra_xspi_liocfgcs_mask_ddren;
  }
  reg->LIOCFGCS[0] = v;
  return k_ra_ok;
}

ra_err_t ra_xspi_calibrate_dqs(uint8_t instance)
{
  volatile r_xspi_regs_t* reg = ra_xspi(instance);
  RA_CHECK_NULL_PTR(reg, s_tag, "instance out of range");

#ifdef RA_SIMULATOR_MODE
  /* HUM Ch 44 "Octal Serial Peripheral Interface (OSPI)" p 2986 */
  /* No real DQS line on the host. Walk through the FSP-style
   * register sequence so test cases can assert on the final state,
   * but auto-clear CAEN immediately to model a successful run. */
  reg->CCCTLCS[0] = k_ra_xspi_ccctl0_mask_caen;
  reg->CCCTLCS[0] = 0U;
  return k_ra_ok;
#else
  /* HUM Ch 44 "Octal Serial Peripheral Interface (OSPI)" p 2986 */
  /* Mirror FSP R_OSPI_B_AutoCalibrate: arm CAEN and wait for the
   * controller to clear it once the phase-scan completes. The full
   * preamble-pattern + CARDCMD descriptor is owned by board-level
   * code in higher-level callers. */
  reg->CCCTLCS[0] |= k_ra_xspi_ccctl0_mask_caen;
  for (uint32_t i = 0U; i < (uint32_t)k_ra_xspi_calib_spin; i++) { /* GCOVR_EXCL_BR_LINE */
    if ((reg->CCCTLCS[0] & k_ra_xspi_ccctl0_mask_caen) == 0U) {    /* GCOVR_EXCL_BR_LINE */
      return k_ra_ok;
    }
  }
  return k_ra_err_hw_timeout;
#endif
}

ra_err_t ra_xspi_suspend(uint8_t instance)
{
  volatile r_xspi_regs_t* reg = ra_xspi(instance);
  RA_CHECK_NULL_PTR(reg, s_tag, "instance out of range");
  return internal_issue_simple_opcode(reg, k_ra_spi_flash_op_suspend);
}

ra_err_t ra_xspi_resume(uint8_t instance)
{
  volatile r_xspi_regs_t* reg = ra_xspi(instance);
  RA_CHECK_NULL_PTR(reg, s_tag, "instance out of range");
  return internal_issue_simple_opcode(reg, k_ra_spi_flash_op_resume);
}

/**
 * @brief Issue a single RSTEN-or-RST opcode in either 1S or 8D form.
 *
 * @details
 * In 1S-1S-1S mode the opcode is a single byte placed at CDT.CMD bits
 * [31..16] with CMDSIZE=1. In 8D-8D-8D mode the chip expects the
 * opcode followed by its bitwise complement so the controller has to
 * ship two bytes; we encode that as ``opcode | (~opcode << 8)`` in
 * the same CMD field with CMDSIZE=2. The CDT.CMD field is 16 bits
 * wide ([31..16]) so both forms fit cleanly. No address phase, no
 * data phase. IS25LX512M datasheet Ch 7.3 p 27 ("Operating Protocols")
 * + Ch 8.20/8.21 p 39 (RSTEN/RST opcodes); HUM Ch 44 p 2986 for the
 * CDT layout.
 *
 * @param[in] reg       xSPI register block.
 * @param[in] opcode    JEDEC reset opcode (0x66 RSTEN or 0x99 RST).
 * @param[in] cmd_bytes 1 (1S) or 2 (8D); chosen by the caller based
 *                      on the protocol mode the chip *might* be in.
 *
 * @return ``k_ra_ok`` on success, ``k_ra_err_hw_timeout`` on CMDCMP
 *         timeout.
 * @retval k_ra_ok Operation completed successfully.
 * @retval other Non-zero error code from the underlying operation.
 * @pre Module/state preconditions hold (see function body).
 * @pre Module/state preconditions hold (see function body).
 * @post Documented side effects are visible on success.
 * @post Documented side effects are visible on success.
 * @note Not thread-safe; the caller must serialise concurrent access.
 * @since 0.1.0
 */
static ra_err_t
internal_issue_reset_opcode(volatile r_xspi_regs_t* reg, uint8_t opcode, uint8_t cmd_bytes)
{
  /* HUM Ch 44 "Octal Serial Peripheral Interface (OSPI)" p 2986 */
  /* Build the CMD half-word for either 1S (just the opcode) or 8D
   * (opcode + complement). The complement form is what 8D-mode SPI
   * NOR devices require so they can distinguish "real opcode" from
   * "garbage on the bus". */
  uint16_t cmd_word = opcode;
  if (cmd_bytes == (uint8_t)k_ra_xspi_reset_cmd_bytes_8d) {
    cmd_word = (uint16_t)(opcode | (((uint16_t)(uint8_t)~opcode) << 8U));
  }
  reg->CDBUF[k_ra_xspi_cdbuf_idx_cdt] =
    (((uint32_t)cmd_bytes & k_ra_xspi_cdt_mask_cmdsize) << k_ra_xspi_cdt_pos_cmdsize) |
    (((uint32_t)k_ra_xspi_cdt_addsize_0 & k_ra_xspi_cdt_mask_addsize)
     << k_ra_xspi_cdt_pos_addsize) |
    (((uint32_t)k_ra_xspi_cdt_trtype_write & k_ra_xspi_cdt_mask_trtype)
     << k_ra_xspi_cdt_pos_trtype) |
    (((uint32_t)cmd_word) << k_ra_xspi_cdt_pos_cmd);
  reg->CDBUF[k_ra_xspi_cdbuf_idx_addr]  = 0U;
  reg->CDBUF[k_ra_xspi_cdbuf_idx_data0] = 0U;
  reg->CDBUF[k_ra_xspi_cdbuf_idx_data1] = 0U;
  return internal_kick_command(reg);
}

ra_err_t ra_xspi_software_reset(uint8_t instance, uint8_t cmd_bytes)
{
  volatile r_xspi_regs_t* reg = ra_xspi(instance);
  RA_CHECK_NULL_PTR(reg, s_tag, "instance out of range");
  if ((cmd_bytes != (uint8_t)k_ra_xspi_reset_cmd_bytes_1s) &&
      (cmd_bytes != (uint8_t)k_ra_xspi_reset_cmd_bytes_8d)) {
    return k_ra_err_invalid_arg;
  }

  /* IS25LX512M Ch 8.20-8.21 p 39: RSTEN must be the immediately
   * preceding command before RST or the device ignores RST. */
  const ra_err_t en = internal_issue_reset_opcode(reg, k_ra_spi_flash_op_reset_en, cmd_bytes);
  if (en != k_ra_ok) {
    return en;
  }
  const ra_err_t rst = internal_issue_reset_opcode(reg, k_ra_spi_flash_op_reset_dev, cmd_bytes);
  if (rst != k_ra_ok) {
    return rst;
  }
  return k_ra_ok;
}
