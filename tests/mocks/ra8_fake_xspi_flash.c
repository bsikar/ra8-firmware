/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file ra8_fake_xspi_flash.c
 * @brief Host-side register-level JEDEC NOR-flash model behind the xSPI engine
 *
 * @par Tag
 * [Ring 3 / HAL] {World: S} (host test-only)
 *
 * @details
 * See ``ra8_fake_xspi_flash.h`` for the model contract. The implementation
 * mirrors the ra8_emulator peripheral model
 * (``tools/ra8_emulator/src/periph/board_periph_xspi.c``), including the inverted
 * backing-store trick: each byte stores the complement of the flash
 * content, so the all-zero BSS image IS the erased (0xFF) part and a fresh
 * test process commits no resident pages for the 16 MiB windows until a
 * sector is actually programmed or erased.
 *
 * @since 0.1.0
 */

#include "ra8_fake_xspi_flash.h"

#include <stdint.h>

#include "ra8_err.h"
#include "ra8_fake_mmio.h"
#include "ra8_ospi_regs.h"
#include "ra8_xspi_internal.h"

/**
 * @enum ra8_fake_xspi_op_t
 * @brief JEDEC opcodes the model decodes (all others complete as no-ops).
 *
 * @details
 * Mirrors the opcode set the ``ra8_xspi_flash.c`` driver emits and the
 * ra8_emulator model decodes. Software-reset / suspend / resume opcodes fall
 * through the ``default`` arm: they retire with CMDCMP but touch no flash.
 *
 * @see ra8_fake_xspi_flash_install() Model installation.
 * @since 0.1.0
 */
typedef enum : uint8_t {
  k_fake_xspi_op_pp    = 0x02U, /**< Page program (AND into the array). */
  k_fake_xspi_op_read  = 0x03U, /**< Normal read.                       */
  k_fake_xspi_op_rdsr  = 0x05U, /**< Read Status Register.              */
  k_fake_xspi_op_wren  = 0x06U, /**< Write enable (implicit latch).     */
  k_fake_xspi_op_erase = 0x20U, /**< 4 KiB sector erase.                */
  k_fake_xspi_op_rdid  = 0x9FU, /**< Read JEDEC ID.                     */
} ra8_fake_xspi_op_t;

/**
 * @enum ra8_fake_xspi_lit_t
 * @brief Bit-manipulation literals for the CDT decode and CDD packing.
 *
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_fake_xspi_byte_bits       = 8U,         /**< Bits per byte.                  */
  k_fake_xspi_byte_mask       = 0xFFU,      /**< One opcode / data byte.         */
  k_fake_xspi_word_bytes      = 4U,         /**< Data bytes per CDD word.        */
  k_fake_xspi_cmd_field_bytes = 2U,         /**< CMD[31:16] holds 2 bytes.       */
  k_fake_xspi_status_wip      = 0x01U,      /**< RDSR bit 0: Write-In-Progress.  */
  k_fake_xspi_erased          = 0xFFU,      /**< NOR erased byte value.          */
  k_fake_xspi_jedec_cdd0      = 0x1A5A9DUL, /**< {0x9D,0x5A,0x1A} on-wire order. */
} ra8_fake_xspi_lit_t;

/**
 * @var s_fake_xspi_flash_neg
 * @brief Per-instance NOR backing arrays, stored bit-INVERTED.
 *
 * @details
 * Each byte holds the complement of the flash content so the zero-filled
 * BSS image is the erased (0xFF) part. NOR semantics translate directly:
 * page-program clears flash bits = SETS stored bits (OR); sector erase
 * restores 0xFF = clears stored bytes to 0.
 *
 * @note Access only through the ``internal_fake_xspi_*`` helpers so the
 *       inversion cannot leak.
 * @warning Never memset this to 0xFF: that would mean "every bit
 *          programmed" AND commit 16 MiB of resident pages per instance.
 * @since 0.1.0
 */
static uint8_t s_fake_xspi_flash_neg[k_ra8_xspi_instance_count][k_ra8_fake_xspi_flash_bytes];

/**
 * @var s_fake_xspi_dirty_hi
 * @brief Per-instance exclusive high-water mark of bytes ever touched.
 *
 * @details
 * Lets ::ra8_fake_xspi_flash_install wipe only the touched prefix instead of
 * sweeping (and committing) the whole 16 MiB window on every test case.
 *
 * @note Raised by the program / erase arms of the command decoder only.
 * @warning Do not reset outside ::ra8_fake_xspi_flash_install.
 * @since 0.1.0
 */
static uint32_t s_fake_xspi_dirty_hi[k_ra8_xspi_instance_count];

/**
 * @var s_fake_xspi_busy_cfg
 * @brief Per-instance busy budget armed onto each subsequent PP / SE.
 *
 * @details
 * Configured by ::ra8_fake_xspi_flash_set_busy_polls; zero means every
 * program / erase completes before its first RDSR poll.
 *
 * @note Test-configuration state; consumed indirectly via
 *       ::s_fake_xspi_busy_left.
 * @warning Reset to zero by ::ra8_fake_xspi_flash_install.
 * @since 0.1.0
 */
static uint32_t s_fake_xspi_busy_cfg[k_ra8_xspi_instance_count];

/**
 * @var s_fake_xspi_busy_left
 * @brief Per-instance remaining RDSR polls that still report WIP=1.
 *
 * @details
 * Loaded from ::s_fake_xspi_busy_cfg on each PP / SE and decremented by
 * each RDSR while non-zero, modelling a program / erase in flight.
 *
 * @note Mutated only by the command decoder.
 * @warning Reset to zero by ::ra8_fake_xspi_flash_install.
 * @since 0.1.0
 */
static uint32_t s_fake_xspi_busy_left[k_ra8_xspi_instance_count];

/**
 * @brief Extract the left-justified 1/2-byte opcode from a CDT word.
 *
 * @details
 * ``internal_make_cdt`` in the driver left-justifies the opcode inside the
 * 16-bit ``CMD[31:16]`` field: ``shift = 8 * (2 - CMDSIZE)``. Recover the
 * low opcode byte the same way the ra8_emulator model does. Raw
 * ``ra8_xspi_direct_command`` payloads decode to arbitrary bytes here and
 * fall into the decoder's no-op arm.
 *
 * @param[in] cdt CDBUF slot-0 CDT descriptor word.
 *
 * @return The transmitted opcode byte.
 *
 * @pre @p cdt was staged by the driver into CDBUF[0].
 * @pre The CMDSIZE field is 0..2 (masked here regardless).
 * @post No state is modified (pure decode).
 * @post The return is the byte the controller would clock out first.
 *
 * @note Test-only. Not thread-safe (tests are single-threaded).
 * @since 0.1.0
 */
static uint8_t internal_fake_xspi_opcode(uint32_t cdt)
{
  const uint32_t cmdsize =
    (cdt >> (uint32_t)k_ra8_xspi_cdt_pos_cmdsize) & (uint32_t)k_ra8_xspi_cdt_mask_cmdsize;
  const uint32_t cmdfield =
    (cdt >> (uint32_t)k_ra8_xspi_cdt_pos_cmd) & (uint32_t)k_ra8_xspi_cdt_mask_cmd;
  const uint32_t shift =
    k_fake_xspi_byte_bits *
    (k_fake_xspi_cmd_field_bytes - (cmdsize & (uint32_t)k_ra8_xspi_cdt_mask_cmdsize));
  return (uint8_t)((cmdfield >> shift) & k_fake_xspi_byte_mask);
}

/**
 * @brief Serve a READ: pack backing bytes at @p addr into CDD0 / CDD1.
 *
 * @details
 * Bytes 0..3 land in CDD0 and 4..7 in CDD1, little-endian per byte index,
 * matching how ``internal_flash_read_chunk`` unpacks them. Addresses past
 * the backing window read as erased (0xFF), like a real part returning
 * open-bus ones -- unreachable through the driver, which range-checks the
 * 3-byte address space this backing fully covers.
 *
 * @param[in]     instance xSPI instance index (0 or 1).
 * @param[in,out] reg      Register window; CDD0 / CDD1 are written.
 * @param[in]     addr     Flash byte address from CDA.
 * @param[in]     n        DATASIZE byte count (0..8).
 *
 * @pre @p reg is the register window for @p instance.
 * @pre @p n is at most the 8 bytes one CDBUF slot carries.
 * @post CDD0 / CDD1 hold the @p n flash bytes at @p addr.
 * @post No flash content is modified.
 *
 * @note Test-only. Not thread-safe (tests are single-threaded).
 * @since 0.1.0
 */
static void
internal_fake_xspi_read(uint8_t instance, volatile r_xspi_regs_t* reg, uint32_t addr, uint32_t n)
{
  uint32_t d0 = 0U;
  uint32_t d1 = 0U;
  for (uint32_t i = 0U; i < n; i++) {
    uint8_t b = (uint8_t)k_fake_xspi_erased;
    if ((addr + i) < (uint32_t)k_ra8_fake_xspi_flash_bytes) {
      b = (uint8_t)~s_fake_xspi_flash_neg[instance][addr + i];
    }
    if (i < k_fake_xspi_word_bytes) {
      d0 |= (uint32_t)b << (i * k_fake_xspi_byte_bits);
    } else {
      d1 |= (uint32_t)b << ((i - k_fake_xspi_word_bytes) * k_fake_xspi_byte_bits);
    }
  }
  /* HUM Ch 44 "Octal Serial Peripheral Interface (OSPI)" p 2986 */
  reg->CDBUF[k_ra8_xspi_cdbuf_idx_data0] = d0;
  reg->CDBUF[k_ra8_xspi_cdbuf_idx_data1] = d1;
}

/**
 * @brief Serve a page-program: AND CDD0 / CDD1 bytes into the backing array.
 *
 * @details
 * NOR programming clears bits only: ``flash &= data``. In the inverted
 * store that is ``neg |= ~data``. Loads the WIP busy budget so subsequent
 * RDSR polls can model a program in flight.
 *
 * @param[in] instance xSPI instance index (0 or 1).
 * @param[in] reg      Register window; CDD0 / CDD1 are read.
 * @param[in] addr     Flash byte address from CDA.
 * @param[in] n        DATASIZE byte count (0..8).
 *
 * @pre @p reg is the register window for @p instance.
 * @pre @p n is at most the 8 bytes one CDBUF slot carries.
 * @post The @p n flash bytes at @p addr have @p data's zero bits cleared.
 * @post The instance's WIP busy budget is re-armed from its configuration.
 *
 * @note Test-only. Not thread-safe (tests are single-threaded).
 * @since 0.1.0
 */
static void
internal_fake_xspi_program(uint8_t instance, volatile r_xspi_regs_t* reg, uint32_t addr, uint32_t n)
{
  /* HUM Ch 44 "Octal Serial Peripheral Interface (OSPI)" p 2986 */
  const uint32_t d0 = reg->CDBUF[k_ra8_xspi_cdbuf_idx_data0];
  /* HUM Ch 44 "Octal Serial Peripheral Interface (OSPI)" p 2986 */
  const uint32_t d1 = reg->CDBUF[k_ra8_xspi_cdbuf_idx_data1];
  for (uint32_t i = 0U; i < n; i++) {
    if ((addr + i) >= (uint32_t)k_ra8_fake_xspi_flash_bytes) {
      continue; /* Unreachable via the driver's 3-byte range validation. */
    }
    const uint32_t w = (i < k_fake_xspi_word_bytes) ? d0 : d1;
    const uint32_t s = (i < k_fake_xspi_word_bytes) ? i : (i - k_fake_xspi_word_bytes);
    const uint8_t  b = (uint8_t)((w >> (s * k_fake_xspi_byte_bits)) & k_fake_xspi_byte_mask);
    s_fake_xspi_flash_neg[instance][addr + i] |= (uint8_t)~b;
    if ((addr + i + 1U) > s_fake_xspi_dirty_hi[instance]) {
      s_fake_xspi_dirty_hi[instance] = addr + i + 1U;
    }
  }
  s_fake_xspi_busy_left[instance] = s_fake_xspi_busy_cfg[instance];
}

/**
 * @brief Serve a sector erase: restore the 4 KiB sector at @p addr to 0xFF.
 *
 * @details
 * Erased flash is 0xFF, which is an all-zero byte in the inverted store.
 * Loads the WIP busy budget so subsequent RDSR polls can model an erase in
 * flight.
 *
 * @param[in] instance xSPI instance index (0 or 1).
 * @param[in] addr     Any flash byte address inside the target sector.
 *
 * @pre @p instance is below ``k_ra8_xspi_instance_count``.
 * @pre The sector base derived from @p addr lies inside the backing array
 *      (guaranteed for driver-issued commands by its range validation).
 * @post Every byte of the containing 4 KiB sector reads back 0xFF.
 * @post The instance's WIP busy budget is re-armed from its configuration.
 *
 * @note Test-only. Not thread-safe (tests are single-threaded).
 * @since 0.1.0
 */
static void internal_fake_xspi_erase(uint8_t instance, uint32_t addr)
{
  const uint32_t base = addr & ~((uint32_t)k_ra8_xspi_sector_len - 1U);
  if (base < (uint32_t)k_ra8_fake_xspi_flash_bytes) {
    for (uint32_t i = 0U; i < (uint32_t)k_ra8_xspi_sector_len; i++) {
      s_fake_xspi_flash_neg[instance][base + i] = 0U;
    }
    if ((base + (uint32_t)k_ra8_xspi_sector_len) > s_fake_xspi_dirty_hi[instance]) {
      s_fake_xspi_dirty_hi[instance] = base + (uint32_t)k_ra8_xspi_sector_len;
    }
  }
  s_fake_xspi_busy_left[instance] = s_fake_xspi_busy_cfg[instance];
}

/**
 * @brief Serve an RDSR: report WEL=1 plus WIP while a busy budget remains.
 *
 * @details
 * Matches the ra8_emulator model's implicit write-enable latch (WEL always
 * reads 1) and adds the WIP window ::ra8_fake_xspi_flash_set_busy_polls
 * arms, decrementing it once per poll so the driver's bounded WIP-clear
 * loop sees busy exactly @c n times.
 *
 * @param[in]     instance xSPI instance index (0 or 1).
 * @param[in,out] reg      Register window; CDD0 is written.
 *
 * @pre @p reg is the register window for @p instance.
 * @pre A manual RDSR command was decoded for this service call.
 * @post CDD0 holds the status byte (WEL=1, WIP per the busy budget).
 * @post A non-zero busy budget is one poll closer to idle.
 *
 * @note Test-only. Not thread-safe (tests are single-threaded).
 * @since 0.1.0
 */
static void internal_fake_xspi_status(uint8_t instance, volatile r_xspi_regs_t* reg)
{
  uint32_t status = (uint32_t)k_ra8_fake_xspi_flash_status_idle;
  if (s_fake_xspi_busy_left[instance] > 0U) {
    status |= k_fake_xspi_status_wip;
    s_fake_xspi_busy_left[instance]--;
  }
  /* HUM Ch 44 "Octal Serial Peripheral Interface (OSPI)" p 2986 */
  reg->CDBUF[k_ra8_xspi_cdbuf_idx_data0] = status;
}

/**
 * @brief Execute the pending CDBUF slot-0 command and retire it.
 *
 * @details
 * Decodes the descriptor exactly like the ra8_emulator model, dispatches the
 * flash-affecting opcodes, then raises ``INTS.CMDCMP`` and self-clears
 * ``CDCTL0.TRREQ`` -- the controller-completion behaviour the driver's
 * real CMDCMP poll observes.
 *
 * @param[in]     instance xSPI instance index (0 or 1).
 * @param[in,out] reg      Register window with ``TRREQ`` pending.
 *
 * @pre ``CDCTL0.TRREQ`` is set (a command was kicked).
 * @pre CDBUF slot 0 holds the descriptor the driver staged.
 * @post ``INTS.CMDCMP`` is set and ``CDCTL0.TRREQ`` is clear.
 * @post Any flash / status side effect of the opcode has been applied.
 *
 * @note Test-only. Not thread-safe (tests are single-threaded).
 * @since 0.1.0
 */
static void internal_fake_xspi_exec(uint8_t instance, volatile r_xspi_regs_t* reg)
{
  /* HUM Ch 44 "Octal Serial Peripheral Interface (OSPI)" p 2986 */
  const uint32_t cdt = reg->CDBUF[k_ra8_xspi_cdbuf_idx_cdt];
  /* HUM Ch 44 "Octal Serial Peripheral Interface (OSPI)" p 2986 */
  const uint32_t addr = reg->CDBUF[k_ra8_xspi_cdbuf_idx_addr];
  const uint32_t n =
    (cdt >> (uint32_t)k_ra8_xspi_cdt_pos_datasize) & (uint32_t)k_ra8_xspi_cdt_mask_datasize;
  switch (internal_fake_xspi_opcode(cdt)) {
    case (uint8_t)k_fake_xspi_op_rdid:
      /* HUM Ch 44 "Octal Serial Peripheral Interface (OSPI)" p 2986 */
      reg->CDBUF[k_ra8_xspi_cdbuf_idx_data0] = (uint32_t)k_fake_xspi_jedec_cdd0;
      break;
    case (uint8_t)k_fake_xspi_op_rdsr:
      internal_fake_xspi_status(instance, reg);
      break;
    case (uint8_t)k_fake_xspi_op_read:
      internal_fake_xspi_read(instance, reg, addr, n);
      break;
    case (uint8_t)k_fake_xspi_op_pp:
      internal_fake_xspi_program(instance, reg, addr, n);
      break;
    case (uint8_t)k_fake_xspi_op_erase:
      internal_fake_xspi_erase(instance, addr);
      break;
    default:
      break; /* WREN, software reset, suspend/resume: no flash effect. */
  }
  /* HUM Ch 44 "Octal Serial Peripheral Interface (OSPI)" p 2986 */
  reg->INTS |= (uint32_t)k_ra8_xspi_ints_mask_cmdcmp;
  /* HUM Ch 44 "Octal Serial Peripheral Interface (OSPI)" p 2986 */
  reg->CDCTL0 &= ~(uint32_t)k_ra8_xspi_cdctl0_mask_trreq;
}

/**
 * @brief Per-poll servicing hook: W1C INTC, then execute any pending kick.
 *
 * @details
 * Runs synchronously on the driver's own poll thread (installed via
 * ::ra8_fake_mmio_set_poll_hook). For each instance it first applies the
 * ``INTC`` write-1-to-clear semantics against ``INTS`` -- so the driver's
 * post-command ``INTC = INTS`` genuinely retires the status bits -- and
 * then services a pending ``TRREQ`` exactly once.
 *
 * @pre The model was installed by ::ra8_fake_xspi_flash_install.
 * @pre Register RAM is mapped (``ra8_fake_mmap`` backing is live).
 * @post Every pending W1C clear has been applied to ``INTS``.
 * @post Any pending command on either instance has retired with CMDCMP.
 *
 * @note Test-only. Not thread-safe (tests are single-threaded).
 * @since 0.1.0
 */
static void internal_fake_xspi_poll_hook(void)
{
  for (uint8_t i = 0U; i < (uint8_t)k_ra8_xspi_instance_count; i++) {
    volatile r_xspi_regs_t* reg = ra8_xspi(i);
    /* HUM Ch 44 "Octal Serial Peripheral Interface (OSPI)" p 2986 */
    const uint32_t w1c = reg->INTC;
    if (w1c != 0U) {
      /* HUM Ch 44 "Octal Serial Peripheral Interface (OSPI)" p 2986 */
      reg->INTS &= ~w1c;
      reg->INTC = 0U;
    }
    /* HUM Ch 44 "Octal Serial Peripheral Interface (OSPI)" p 2986 */
    if ((reg->CDCTL0 & (uint32_t)k_ra8_xspi_cdctl0_mask_trreq) != 0U) {
      internal_fake_xspi_exec(i, reg);
    }
  }
}

void ra8_fake_xspi_flash_install(void)
{
  for (uint8_t i = 0U; i < (uint8_t)k_ra8_xspi_instance_count; i++) {
    /* Only the dirty prefix ever left the erased state; sweeping the whole
     * 16 MiB would needlessly commit resident pages in every test binary. */
    for (uint32_t b = 0U; b < s_fake_xspi_dirty_hi[i]; b++) {
      s_fake_xspi_flash_neg[i][b] = 0U;
    }
    s_fake_xspi_dirty_hi[i]  = 0U;
    s_fake_xspi_busy_cfg[i]  = 0U;
    s_fake_xspi_busy_left[i] = 0U;
  }
  ra8_fake_mmio_set_poll_hook(internal_fake_xspi_poll_hook);
}

ra8_err_t ra8_fake_xspi_flash_set_busy_polls(uint8_t instance, uint32_t n)
{
  if (instance >= (uint8_t)k_ra8_xspi_instance_count) {
    return k_ra8_err_invalid_arg;
  }
  s_fake_xspi_busy_cfg[instance] = n;
  return k_ra8_ok;
}
