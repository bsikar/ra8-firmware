/**
 * @file ra_dfu_program.c
 * @brief MRAM slot program / verify for the USB-DFU bootloader core.
 *
 * @par Tag
 * [Ring 4 / Service] {World: S}
 *
 * @details
 * Erases, programs, and read-back-verifies the *inactive* application slot
 * over `ra_flash`, and reads slot headers for the boot decision. The MRAM
 * controller's program loop must NOT execute from the MRAM it touches (see
 * `ra_flash.h`); on the firmware target each consumer's linker script places
 * this TU and `ra_flash` in SRAM. Under `RA_SIMULATOR_MODE` the code-MRAM
 * window is backed by simulator memory, so the program -> read-back -> verify
 * round-trip is exercised directly by the host unit tests.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <string.h>

#include "ra_dfu.h"
#include "ra_flash.h"
#include "ra_register_guard.h"

/**
 * @enum ra_dfu_prog_cfg_t
 * @brief MRAM clock notifications used to (re)open the controller.
 * @details Mirrors the EK-RA8D2 code/extra MRAM operating clocks the apps
 * advertise via MRCFREQ / MREFREQ (HUM Ch 59.4.3 p 3550).
 */
typedef enum : uint16_t {
  k_ra_dfu_prog_mrcfreq_mhz = 250U, /**< Code-MRAM clock notification (MHz).  */
  k_ra_dfu_prog_mrefreq_mhz = 125U, /**< Extra-MRAM clock notification (MHz). */
} ra_dfu_prog_cfg_t;

/**
 * @enum ra_dfu_prog_fill_t
 * @brief MRAM erased-state fill byte.
 * @details Value every byte holds after an MRAM erase; used to seed the
 * SRAM-resident all-ones page so the erase baseline operand never has to be
 * read out of code-MRAM while the array is busy (see ::internal_write_secure).
 */
typedef enum : uint8_t {
  k_ra_dfu_prog_erased_byte = 0xFFU, /**< Erased-state byte for an MRAM page. */
} ra_dfu_prog_fill_t;

/** @brief Default controller bring-up descriptor for ::ra_dfu_program_prepare. */
static const ra_flash_cfg_t s_ra_dfu_flash_cfg = {
  .mrcfreq_mhz        = (uint16_t)k_ra_dfu_prog_mrcfreq_mhz,
  .mrefreq_mhz        = (uint8_t)k_ra_dfu_prog_mrefreq_mhz,
  .prefetch_en        = true,
  .ecc_encoder_enable = true,
  .ecc_decoder_enable = true,
};

/**
 * @brief Program @p len bytes at @p addr through the SECURE MRAM gate.
 *
 * @details
 * The DFU core runs in the secure world and its slot MRAM carries secure
 * attribution, so it must be programmed via ``MRCPC1``
 * (``k_ra_flash_world_s``). ``ra_flash_write`` cannot be used here: its
 * internal ``internal_world_for_addr`` unconditionally selects the
 * non-secure gate ``MRCPC0``, which raises an imprecise bus fault on a
 * store to secure MRAM. This helper therefore drives
 * ``ra_flash_write_block`` directly, one 32-byte page at a time, with IRQs
 * masked across each page program so no ISR fetches code-MRAM while the
 * array is busy. The soft write-window installed by
 * ::ra_dfu_program_prepare is still enforced inside
 * ``ra_flash_write_block``, so an out-of-slot destination is rejected.
 *
 * @param[in] addr 32-byte aligned MRAM destination.
 * @param[in] src  Non-NULL source buffer of at least @p len bytes.
 * @param[in] len  Non-zero multiple of the 32-byte page size.
 *
 * @return ``ra_err_t`` from the first failing page, else ``k_ra_ok``.
 * @retval k_ra_ok Every page committed.
 * @retval k_ra_err_invalid_arg @p src was NULL or @p len was zero.
 *
 * @pre @p src non-null and @p len a non-zero multiple of the page size.
 * @pre This TU is linked into ``.sram_text`` (SRAM-resident program loop).
 * @post On success every page in [addr, addr+len) holds @p src.
 * @post The secure program gate is re-locked on every exit path.
 *
 * @note Thread-safe: no; masks IRQs across each page program.
 */
static ra_err_t internal_write_secure(uintptr_t addr, const uint8_t* src, uint32_t len)
{
  if ((src == nullptr) || (len == 0U)) {
    return k_ra_err_invalid_arg;
  }
  /* Erased-state pattern in SRAM. ra_flash_erase_block would read the driver's
   * 0xFF constant out of code-MRAM while the array is busy programming, which
   * faults the bus (ra_flash.h SRAM-resident warning); sourcing 0xFF from this
   * stack buffer keeps every operand in SRAM. */
  uint8_t ones[k_ra_dfu_page_size];
  (void)memset(ones, (int)k_ra_dfu_prog_erased_byte, sizeof(ones));
  uint32_t off = 0U;
  /* NASA Rule 2: bound is the caller-validated, page-aligned len. */
  while (off < len) {
    uint32_t chunk = len - off;
    if (chunk > (uint32_t)k_ra_dfu_page_size) {
      chunk = (uint32_t)k_ra_dfu_page_size;
    }
    /* Erase the page to all-ones first: code-MRAM cannot be reliably
     * re-programmed over stale (non-0xFF) contents with ECC enabled, so each
     * page is driven to the erased baseline before the body is written. Both
     * the erase and the program run IRQ-masked so no ISR fetches code-MRAM
     * while the array is busy. */
    ra_register_guard_t guard;
    ra_register_guard_enter(&guard);
    ra_err_t err = ra_flash_write_block((uint32_t)(addr + off), ones, chunk, k_ra_flash_world_s);
    if (err == k_ra_ok) {
      err = ra_flash_write_block((uint32_t)(addr + off), &src[off], chunk, k_ra_flash_world_s);
    }
    ra_register_guard_exit(&guard);
    if (err != k_ra_ok) {
      return err;
    }
    off += chunk;
  }
  return k_ra_ok;
}

uintptr_t ra_dfu_slot_base(ra_dfu_slot_t slot)
{
  uintptr_t base = 0U;
  switch (slot) {
    case k_ra_dfu_slot_a:
      base = (uintptr_t)k_ra_dfu_slot_a_base;
      break;
    case k_ra_dfu_slot_b:
      base = (uintptr_t)k_ra_dfu_slot_b_base;
      break;
    case k_ra_dfu_slot_none:
    default:
      base = 0U;
      break;
  }
  return base;
}

ra_dfu_slot_t ra_dfu_other_slot(ra_dfu_slot_t slot)
{
  return (slot == k_ra_dfu_slot_a) ? k_ra_dfu_slot_b : k_ra_dfu_slot_a;
}

ra_err_t ra_dfu_read_header(ra_dfu_slot_t slot, ra_dfu_img_hdr_t* out_hdr)
{
  if (out_hdr == nullptr) {
    return k_ra_err_null_ptr;
  }
  const uintptr_t base = ra_dfu_slot_base(slot);
  if (base == 0U) {
    return k_ra_err_invalid_arg;
  }
  /* HUM Ch 59.1 "Code-MRAM" p 3543 -- the slot header is the LAST 32-byte data
   * page of the slot (so the app vector table can sit at the aligned base);
   * this is a plain MRAM data read, not a control register access. */
  (void)memcpy(out_hdr, (const void*)(base + (uintptr_t)k_ra_dfu_hdr_offset), sizeof(*out_hdr));
  return k_ra_ok;
}

/**
 * @brief Implementation of `ra_dfu_slot_valid()` -- guards the CRC read so a
 *        bogus `img_len` cannot drive an out-of-bounds MRAM fold.
 */
bool ra_dfu_slot_valid(ra_dfu_slot_t slot)
{
  ra_dfu_img_hdr_t hdr  = {};
  const uintptr_t  base = ra_dfu_slot_base(slot);
  if (base == 0U) {
    return false;
  }
  if (ra_dfu_read_header(slot, &hdr) != k_ra_ok) {
    return false;
  }
  /* Only fold the CRC over a length that is in-range + aligned; otherwise
   * ra_dfu_hdr_valid rejects on the length anyway, and we must not read past
   * the slot. */
  uint32_t crc = 0U;
  // mcdc-deactivated: ra_dfu_slot_valid length-bound AND (3 conditions) is a
  // defensive duplicate of ra_dfu_hdr_valid's len_ok, which IS MC/DC-tested in
  // test_ra_dfu_boot.c. Its false branches guard a stray MRAM over-read; driving
  // them needs a header whose img_len ra_dfu_program_commit refuses to program,
  // so no in-simulator vector can flip a single condition independently here.
  if ((hdr.img_len != 0U) && (hdr.img_len <= (uint32_t)k_ra_dfu_img_max) &&
      ((hdr.img_len % (uint32_t)k_ra_dfu_page_size) == 0U)) {
    crc = ra_dfu_crc32((const uint8_t*)base, hdr.img_len);
  }
  return ra_dfu_hdr_valid(&hdr, crc);
}

ra_err_t ra_dfu_slot_seq(ra_dfu_slot_t slot, uint32_t* out_seq)
{
  if (out_seq == nullptr) {
    return k_ra_err_null_ptr;
  }
  ra_dfu_img_hdr_t hdr = {};
  const ra_err_t   err = ra_dfu_read_header(slot, &hdr);
  if (err != k_ra_ok) {
    return err;
  }
  *out_seq = (hdr.magic == (uint32_t)k_ra_dfu_hdr_magic) ? hdr.seq : 0U;
  return k_ra_ok;
}

ra_err_t ra_dfu_program_prepare(ra_dfu_slot_t inactive)
{
  const uintptr_t base = ra_dfu_slot_base(inactive);
  if (base == 0U) {
    return k_ra_err_invalid_arg;
  }
  const uintptr_t high = base + (uintptr_t)k_ra_dfu_slot_size;
  ra_err_t        err  = ra_flash_open(&s_ra_dfu_flash_cfg);
  if (err != k_ra_ok) {
    return err;
  }
  /* Fence every subsequent write to this one slot -- the bootloader and the
   * active slot are outside [base, high) and cannot be touched. No erase: MRAM
   * is byte-alterable, so ::ra_dfu_program_image writes the body directly and
   * ::ra_dfu_program_commit overwrites the header. A torn download leaves the
   * OLD header over a NEW partial image, whose CRC will not match -> invalid,
   * so torn-write safety holds without an erase. (Erasing here would also read
   * the driver's 0xFF constant out of code-MRAM while the array is busy
   * programming, faulting the bus -- see ra_flash.h's SRAM-resident warning.) */
  return ra_flash_set_window(base, high);
}

ra_err_t
ra_dfu_program_image(ra_dfu_slot_t inactive, uint32_t img_offset, const uint8_t* data, uint32_t len)
{
  if (data == nullptr) {
    return k_ra_err_null_ptr;
  }
  const uintptr_t base = ra_dfu_slot_base(inactive);
  if (base == 0U) {
    return k_ra_err_invalid_arg;
  }
  if ((len == 0U) || ((len % (uint32_t)k_ra_dfu_page_size) != 0U) ||
      ((img_offset % (uint32_t)k_ra_dfu_page_size) != 0U)) {
    return k_ra_err_invalid_arg;
  }
  if (((uint64_t)img_offset + (uint64_t)len) > (uint64_t)k_ra_dfu_img_max) {
    return k_ra_err_invalid_arg;
  }
  const uintptr_t dst = base + (uintptr_t)img_offset;
  return internal_write_secure(dst, data, len);
}

ra_err_t ra_dfu_program_commit(ra_dfu_slot_t inactive, uint32_t img_len, uint32_t seq)
{
  const uintptr_t base = ra_dfu_slot_base(inactive);
  if (base == 0U) {
    return k_ra_err_invalid_arg;
  }
  if ((img_len == 0U) || (img_len > (uint32_t)k_ra_dfu_img_max) ||
      ((img_len % (uint32_t)k_ra_dfu_page_size) != 0U)) {
    return k_ra_err_invalid_arg;
  }
  const uint32_t   body_crc = ra_dfu_crc32((const uint8_t*)base, img_len);
  ra_dfu_img_hdr_t hdr      = {};
  hdr.magic                 = (uint32_t)k_ra_dfu_hdr_magic;
  hdr.seq                   = seq;
  hdr.img_len               = img_len;
  hdr.img_crc32             = body_crc;
  hdr.entry                 = (uint32_t)k_ra_dfu_run_base;
  /* Header programmed LAST, into the slot's last page, so a torn write leaves
   * the header erased (invalid), never a valid header over a partial image.
   * internal_write_secure masks IRQs across the page program so no ISR fetches
   * code-MRAM while the header page is being written. */
  return internal_write_secure(base + (uintptr_t)k_ra_dfu_hdr_offset,
                               (const uint8_t*)&hdr,
                               (uint32_t)k_ra_dfu_hdr_size);
}

ra_err_t ra_dfu_program_verify(ra_dfu_slot_t slot)
{
  if (ra_dfu_slot_base(slot) == 0U) {
    return k_ra_err_invalid_arg;
  }
  return ra_dfu_slot_valid(slot) ? k_ra_ok : k_ra_err_crc_mismatch;
}
