/**
 * @file emu_seam_sd.c
 * @brief --fast-sd whole-block SD serving seam
 *
 * @details
 * Serves whole 512-byte SD blocks in C instead of clocking 512 SPI bytes
 * each -- an opt-in emulation-speed seam whose data path is byte-identical
 * to the CMD17 stream (the FAT parse, inflate and render all still run on
 * the real firmware). Moved verbatim out of the ra8_emulator main translation
 * unit; the full rationale rides with the code below.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdio.h>

#include "board_periph_sd.h"
#include "emu_elf.h"
#include "emu_engine.h"
#include "emu_host_io_internal.h"
#include "emu_seams.h"
#include "emu_trace.h"

/* =============================================================================
 * --fast-sd: serve whole SD blocks in C instead of clocking 512 SPI bytes each.
 * =============================================================================
 *
 * The firmware reads an SD card over SCI0 in Simple-SPI mode one byte at a time:
 * ra8_sci_spi_xfer8() issues a FIXED five MMIO accesses per byte (poll TDRE, write
 * TDR, poll RDRF, read RDR, clear RDRF), and a 512-byte block is 512 of those. A
 * book-sized read is millions of MMIO callbacks -- tens of wall-clock seconds in
 * Unicorn (QEMU-TCG) -- versus ~0.1 s on the real 25 MHz bus, so the cost is a
 * pure emulation artifact, not firmware behaviour.
 *
 * The right granularity to skip is the BLOCK, not the byte: redirecting a hooked
 * function back to its caller needs a uc_emu_stop/relaunch, which is far more
 * expensive than an MMIO callback, so a per-byte hook is a net loss (millions of
 * relaunches). Opt-in --fast-sd instead installs a UC_HOOK_CODE at the entry of
 * ra8_sdmmc_spi_read_block(lba, buf): with a card attached it copies the 512-byte
 * block straight from the image via ::board_sd_read_block -- the byte-identical
 * data the CMD17 path would stream -- writes it to @c buf, returns k_ra8_ok and
 * jumps to LR. One relaunch per sector (hundreds per book) instead of millions
 * per byte, so the rendered framebuffer is byte-for-byte identical while the read
 * drops from tens of seconds to well under one.
 *
 * Faithfulness: this serves block DATA directly, bypassing the SD-over-SPI block
 * protocol (CMD17 / 0xFE token / CRC16 / the per-byte SPI loop) for the data
 * path. The FAT parse, inflate, and render all still run on the real firmware and
 * see identical bytes; the bypassed block protocol is covered by the host unit
 * test (apps/shared_libs/reflow/tests/src/test_ra8_sdmmc_card_reflow.c) and by hardware. It is
 * OFF by default:
 * HIL gates and the default run exercise the full handshake; turn it on only to
 * load a large book fast for an interactive or recorded capture. A card-absent
 * call, an out-of-range LBA, or a firmware without the symbol falls through to
 * the real driver, so nothing else is affected.
 */

/** @enum fast_sd_const_t @brief --fast-sd block-serving constants. */
typedef enum : uint16_t {
  k_fast_sd_block = 512U, /**< SD block size served per hook entry. */
} fast_sd_const_t;

/**
 * @var s_fast_sd
 * @brief True once `--fast-sd` is requested on the command line.
 * @details Gates ::fast_sd_seam_install; when false the hook is never armed and
 *          the SD path runs the full faithful per-byte MMIO block protocol.
 * @warning Single-threaded run loop only.
 * @since 0.1.0
 */
static bool s_fast_sd;

/**
 * @brief Perform on sdmmc read block for the emu seam SD model.
 * @details Perform on sdmmc read block for the emu seam sd model; this step is contained within the emu seam SD model and uses bounded caller or module-owned storage.
 * @param[in,out] uc Unicorn engine whose emulated state is read or updated.
 * @param[in] address Guest address involved in the operation.
 * @param[in] size Size of the requested region or access in bytes.
 * @param[in,out] user Hook context supplied when the callback was registered.
 * @pre Arguments satisfy the ranges documented for on sdmmc read block. @pre The call executes on the emulator's single owning thread.
 * @post State changes remain confined to the emu seam SD model and documented output objects. @post Ownership of caller-supplied storage is unchanged.
 * @note The operation is synchronous and does not transfer heap ownership.
 * @since 0.1.0
 */
RA8_INTERNAL static void
internal_on_sdmmc_read_block(uc_engine* uc, uint64_t address, uint32_t size, void* user)
{
  (void)address;
  (void)size;
  (void)user;
  uint32_t lba     = 0U;
  uint32_t buf_ptr = 0U;
  (void)uc_reg_read(uc, UC_ARM_REG_R0, &lba);
  (void)uc_reg_read(uc, UC_ARM_REG_R1, &buf_ptr);
  uint8_t blk[k_fast_sd_block];
  /* No card, a null buffer, or an out-of-range LBA -> run the real driver so the
   * card-absent / error path stays exactly as on hardware. */
  if (!board_sd_attached() || (buf_ptr == 0U) || !board_sd_read_block(lba, blk)) {
    return;
  }
  (void)emu_mem_write(uc, (uint64_t)buf_ptr, blk, sizeof blk);
  /* Relaunch as a zero-time seam (no chunk / SysTick cost) so a whole book-sized
   * read drains within one settle window. */
  emu_seam_request_relaunch();
  eth_hook_return(uc, 0U); /* k_ra8_ok: set r0 + jump to LR, skipping the body. */
}

/**
 * @brief Install the `--fast-sd` block-read hook if opted-in and the symbol exists.
 *
 * @param[in,out] uc  Active Unicorn engine.
 * @param[in]     elf Loaded ELF image (for symbol resolution).
 *
 * @pre @p uc is initialised and @p elf is a validated loaded image.
 * @post With @c s_fast_sd and the symbol present, a UC_HOOK_CODE fires
 *       ::internal_on_sdmmc_read_block at the function entry; otherwise nothing is armed.
 *
 * @note A firmware without ra8_sdmmc_spi_read_block (no SD path) is reported once
 *       and left on the default per-byte MMIO path.
 * @since 0.1.0
 */
void fast_sd_seam_install(uc_engine* uc, const emu_elf_source_t* elf)
{
  if (!s_fast_sd) {
    return;
  }
  const uint32_t addr = elf_sym_addr(elf, "ra8_sdmmc_spi_read_block", nullptr);
  if (addr == 0U) {
    (void)priv_emu_io_errf(
      "  [fast-sd] ra8_sdmmc_spi_read_block not found -- SD stays on the per-byte path\n");
    return;
  }
  static uc_hook s_h_fast_sd;
  (void)uc_hook_add(uc,
                    &s_h_fast_sd,
                    UC_HOOK_CODE,
                    (void*)internal_on_sdmmc_read_block,
                    nullptr,
                    (uint64_t)addr,
                    (uint64_t)addr);
  (void)priv_emu_io_errf("  [fast-sd] ra8_sdmmc_spi_read_block block-hook armed @ 0x%08X "
                         "(SD blocks served direct from the image)\n",
                         (unsigned)addr);
}

/** @brief Implementation of `emu_fast_sd_enable()` -- CLI opt-in latch. */
void emu_fast_sd_enable(void)
{
  s_fast_sd = true;
}
