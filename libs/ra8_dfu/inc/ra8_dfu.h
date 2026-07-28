/**
 * @file ra8_dfu.h
 * @brief Controller-agnostic USB-DFU MRAM bootloader core for the RA8D2.
 * @ingroup grp_security
 *
 * @par Tag
 * [Ring 4 / Service] {World: S}
 *
 * @details
 * Shared core behind the real `dfu_bootloader` app and the two
 * bidirectional HIL self-loop twins (`dfu_selftest_hs_host` /
 * `dfu_selftest_fs_host`). It bundles three concerns:
 *
 *  - **Pure boot logic** (this header + `ra8_dfu_boot.c`): image-header
 *    validation, A/B slot selection, the boot-vs-DFU decision, and a
 *    software CRC32. No MMIO, no USB -- trivially host-unit-testable.
 *  - **MRAM program/verify** (`ra8_dfu_program.c`): stage + erase +
 *    program + read-back-verify of the *inactive* slot over `ra8_flash`,
 *    with the write path placed in SRAM (the code-MRAM program loop must
 *    not execute from MRAM -- see `ra8_flash.h`).
 *  - **DFU device + host glue** (`ra8_dfu_device.c` / `ra8_dfu_host.c`):
 *    USBX DFU class callbacks wired to real MRAM, and the polled host
 *    DFU driver -- both bound to EITHER controller through the
 *    `ux_dcd_ra8_usb` bridge via a `ra8_usb_speed_t` parameter.
 *
 * ## Bank layout (1 MiB MRAM, fixed bootloader + software A/B slots)
 *
 * ```
 * 0x02000000  bootloader  128 KiB  (immutable; never erased by DFU)
 * 0x02020000  Slot A      448 KiB  [app image | 32B header (last page)]
 * 0x02090000  Slot B      448 KiB  [app image | 32B header (last page)]
 * 0x02100000  (end)
 * ```
 *
 * The bootloader is 128 KiB so a full USBX + ThreadX DFU device fits inside the
 * immutable region; the two app slots fill the rest of the 1 MiB array.
 *
 * ## Copy-to-run (one image, either slot)
 *
 * A slot is *staging* only: the image body occupies `[slot_base, slot_base +
 * img_len)` and the 32-byte header is the slot's LAST page (`slot_base +
 * k_ra8_dfu_hdr_offset`), programmed last so a torn download leaves an invalid
 * (CRC-mismatching) slot rather than a half-valid one. On a valid-slot boot the
 * bootloader COPIES the body to a single fixed SRAM run base
 * (::k_ra8_dfu_run_base) and launches it there. A payload is therefore linked
 * ONCE, at the run base, and the *identical* image boots from either slot -- no
 * per-slot build and no "which slot am I building for?" footgun. This sidesteps
 * true position-independent code (ROPI/RWPI), which cannot relocate the absolute
 * function pointers this codebase's interface structs store in `.rodata`.
 *
 * The header's `entry` records that run base. The bootloader copies to the
 * trusted ::k_ra8_dfu_run_base constant (not to `entry`), so a corrupted `entry`
 * cannot redirect the copy -- it only fails the ::ra8_dfu_run_target_valid
 * cross-check, which drops the boot to DFU. (Header-last is kept purely for the
 * torn-write atomicity above; the old VTOR-alignment reason no longer applies,
 * since VTOR is set to the aligned run base, not the slot.)
 *
 * The bootloader is never swapped (no BTFLG / startup-area swap); it
 * reads both slot headers and jumps to the valid slot with the higher
 * sequence number in software. A freshly-written bad slot fails its CRC,
 * so the older valid slot is chosen automatically -- brick-safe by
 * construction, and SWD recovery is always available because the
 * bootloader region is never written.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "ra8_err.h"

/**
 * @enum ra8_dfu_layout_t
 * @brief Fixed MRAM bank-layout addresses and image-header constants.
 *
 * @details
 * The 1 MiB code-MRAM window splits into a 64 KiB immutable bootloader
 * plus two equal 480 KiB application slots. Every numeric address /
 * size / magic the core needs is named here (CLAUDE.md "C23 typed
 * enums"; NASA Rule 8). HUM Ch 59 "Code-MRAM" p 3541 sites the window.
 *
 * @invariant `k_ra8_dfu_slot_a_base + k_ra8_dfu_slot_size ==
 *            k_ra8_dfu_slot_b_base`.
 * @invariant `k_ra8_dfu_slot_b_base + k_ra8_dfu_slot_size ==
 *            k_ra8_dfu_mram_base + k_ra8_dfu_mram_size`.
 */
typedef enum : uint32_t {
  k_ra8_dfu_mram_base     = 0x02000000U, /**< Code-MRAM window base.             */
  k_ra8_dfu_mram_size     = 0x00100000U, /**< Code-MRAM window size (1 MiB).     */
  k_ra8_dfu_bl_size       = 0x00020000U, /**< Immutable bootloader size (128K).  */
  k_ra8_dfu_slot_a_base   = 0x02020000U, /**< Slot A base (app vectors here).    */
  k_ra8_dfu_slot_b_base   = 0x02090000U, /**< Slot B base (app vectors here).    */
  k_ra8_dfu_slot_size     = 0x00070000U, /**< Per-slot size (448 KiB).           */
  k_ra8_dfu_page_size     = 0x00000020U, /**< MRAM program page (32 bytes).      */
  k_ra8_dfu_hdr_size      = 0x00000020U, /**< Image header size (32 bytes).      */
  k_ra8_dfu_img_max       = 0x0006FFE0U, /**< Max image bytes (slot - header).   */
  k_ra8_dfu_hdr_offset    = 0x0006FFE0U, /**< Header offset (slot's last page).  */
  k_ra8_dfu_hdr_magic     = 0x52413844U, /**< Valid-image header magic ("RA8D"). */
  k_ra8_dfu_trigger_magic = 0xDF00B007U, /**< No-init SRAM DFU-request magic.    */
} ra8_dfu_layout_t;

/**
 * @enum ra8_dfu_run_t
 * @brief Fixed SRAM copy-to-run execution base.
 *
 * @details
 * On a valid-slot boot the bootloader copies the staged image body out of the
 * slot to this single SRAM address and launches it there, so every payload is
 * linked at exactly this base and one image runs from either slot. The value
 * clears the bootloader's own low-SRAM footprint (< 64 KiB) with margin, and a
 * maximum `k_ra8_dfu_img_max` image copied here ends well below the bootloader's
 * stack at the top of SRAM -- so the copy never clobbers live bootloader state.
 * The payload's linker `ORIGIN` MUST equal this value.
 *
 * @invariant `k_ra8_dfu_run_base` lies in the 2 MiB SRAM window (0x22000000) and
 *            `k_ra8_dfu_run_base + k_ra8_dfu_img_max` stays inside it.
 */
typedef enum : uint32_t {
  k_ra8_dfu_run_base = 0x22020000U, /**< SRAM copy-to-run / payload link base. */
} ra8_dfu_run_t;

/**
 * @enum ra8_dfu_slot_t
 * @brief Application-slot identifier.
 */
typedef enum : uint8_t {
  k_ra8_dfu_slot_a    = 0U, /**< Slot A (0x02020000).   */
  k_ra8_dfu_slot_b    = 1U, /**< Slot B (0x02090000).   */
  k_ra8_dfu_slot_none = 2U, /**< No valid slot present. */
} ra8_dfu_slot_t;

/**
 * @enum ra8_dfu_action_t
 * @brief Outcome of the reset-time boot decision.
 */
typedef enum : uint8_t {
  k_ra8_dfu_action_dfu    = 0U, /**< Enter the DFU device; do not jump. */
  k_ra8_dfu_action_jump_a = 1U, /**< Jump to the Slot A application.    */
  k_ra8_dfu_action_jump_b = 2U, /**< Jump to the Slot B application.    */
} ra8_dfu_action_t;

/**
 * @struct ra8_dfu_img_hdr_t
 * @brief 32-byte application-image header at the base of each slot.
 *
 * @details
 * Stored in the slot's LAST 32-byte page (`slot_base + k_ra8_dfu_hdr_offset`)
 * and programmed last (after the image body) so a torn write leaves an
 * invalid (CRC-mismatching) slot rather than a half-valid one. The image body
 * starts at `slot_base`; `img_crc32` covers exactly
 * `[slot_base, slot_base + img_len)`.
 *
 * @invariant `img_len` is a non-zero multiple of `k_ra8_dfu_page_size`
 *            and `<= k_ra8_dfu_img_max` for a valid image.
 * @invariant `entry == k_ra8_dfu_run_base` for a valid image (copy-to-run).
 *
 * @par Example:
 * @code
 * ra8_dfu_img_hdr_t h = { .magic = k_ra8_dfu_hdr_magic, .seq = 7,
 *                        .img_len = 0x4000, .img_crc32 = crc,
 *                        .entry = k_ra8_dfu_run_base };
 * @endcode
 *
 * @see ra8_dfu_hdr_valid
 * @see ra8_dfu_run_target_valid
 */
typedef struct {
  uint32_t magic;     /**< Must equal ::k_ra8_dfu_hdr_magic.              */
  uint32_t seq;       /**< Monotonic sequence; higher valid slot wins.    */
  uint32_t img_len;   /**< Image body length in bytes (32-byte multiple). */
  uint32_t img_crc32; /**< CRC32 (IEEE) over the image body.              */
  uint32_t entry;     /**< SRAM run base (== ::k_ra8_dfu_run_base).       */
  uint32_t rsv0;      /**< Reserved; programmed 0.                        */
  uint32_t rsv1;      /**< Reserved; programmed 0.                        */
  uint32_t rsv2;      /**< Reserved; programmed 0.                        */
} ra8_dfu_img_hdr_t;

static_assert(sizeof(ra8_dfu_img_hdr_t) == (uint32_t)k_ra8_dfu_hdr_size,
              "image header must be exactly one 32-byte MRAM page");

/**
 * @brief Compute the IEEE-802.3 CRC32 of a byte range (software).
 *
 * @details
 * Standard reflected CRC32 (poly 0xEDB88320, init 0xFFFFFFFF, final
 * XOR 0xFFFFFFFF) -- the same value `crc32`/`zlib` produce, so a host
 * tool can pre-compute the header `img_crc32`. Pure and bounded: one
 * pass over `len` bytes, no table state retained between calls.
 *
 * @param[in] data Source bytes; may be NULL only when `len == 0`.
 * @param[in] len  Number of bytes to fold in.
 *
 * @return The 32-bit CRC of `data[0 .. len)`; 0 of an empty range folds
 *         to 0x00000000 (init ^ final over zero bytes).
 * @retval 0x00000000 Empty range (`len == 0`); init XOR final over zero bytes.
 *
 * @pre `data != NULL` whenever `len > 0`.
 * @pre `len` does not exceed the addressable image (caller-checked).
 * @post No state is mutated; the result depends only on the inputs.
 * @post The polynomial accumulator is not retained between calls (stateless).
 *
 * @note Thread-safe and reentrant (no statics).
 * @see ra8_dfu_hdr_valid
 * @since 0.1.0
 */
uint32_t ra8_dfu_crc32(const uint8_t* data, uint32_t len);

/**
 * @brief Decide whether a slot header describes a valid bootable image.
 *
 * @details
 * A header is valid iff the magic matches, the length is a non-zero
 * multiple of the 32-byte page within `[k_ra8_dfu_page_size,
 * k_ra8_dfu_img_max]`, and the supplied freshly-computed CRC equals the
 * stored `img_crc32`. The caller computes `computed_crc` over the live
 * slot image (`ra8_dfu_crc32`) before calling, so this function stays
 * pure and host-testable.
 *
 * @param[in] hdr          Header to test (may be NULL -> invalid).
 * @param[in] computed_crc CRC32 the caller computed over the image body.
 *
 * @return `true` iff the header + CRC describe a valid image.
 * @retval true  Magic, length bounds/alignment, and CRC all pass.
 * @retval false `hdr` is NULL, or any check fails.
 *
 * @pre `computed_crc` was produced by ::ra8_dfu_crc32 over exactly
 *      `hdr->img_len` bytes of the slot's image body.
 * @pre The slot's image body is readable when the caller computes the CRC.
 * @post No state is mutated.
 * @post The caller's `hdr` pointer and `computed_crc` value are not modified.
 *
 * @note Thread-safe (pure). Carries compound boolean decisions -- see the
 *       `@par MC/DC:` block in `tests/test_ra8_dfu_boot.c`.
 * @see ra8_dfu_select_slot
 * @since 0.1.0
 */
bool ra8_dfu_hdr_valid(const ra8_dfu_img_hdr_t* hdr, uint32_t computed_crc);

/**
 * @brief Decide whether a validated slot's image may be copied-to-run.
 *
 * @details
 * The boot-time cross-check before the bootloader copies a slot body to SRAM
 * and launches it: the header's `entry` must be the fixed ::k_ra8_dfu_run_base
 * (so the image was linked for the run base, and a corrupted `entry` is caught),
 * and `img_len` must be a non-zero 32-byte multiple within
 * `[k_ra8_dfu_page_size, k_ra8_dfu_img_max]` (so the copy length is sane and the
 * image fits the SRAM run window). Pure -- the bootloader still copies to the
 * trusted ::k_ra8_dfu_run_base constant, never to `entry` itself.
 *
 * @param[in] entry   The candidate image's header `entry` field.
 * @param[in] img_len The candidate image's body length, bytes.
 *
 * @return `true` iff the image may be copied to ::k_ra8_dfu_run_base and run.
 * @retval true  `entry == k_ra8_dfu_run_base` and `img_len` is in range/aligned.
 * @retval false `entry` is not the run base, or `img_len` is zero / too large /
 *               not a 32-byte multiple.
 *
 * @pre `entry` and `img_len` are the live header fields of a slot that already
 *      passed ::ra8_dfu_hdr_valid (magic + length + body CRC).
 * @pre Neither `entry` nor `img_len` is taken from an untrusted external source
 *      without a prior ::ra8_dfu_hdr_valid check.
 * @post No state is mutated.
 * @post The bootloader never uses `entry` as the copy destination; this
 *       function only validates it against the trusted ::k_ra8_dfu_run_base.
 *
 * @note Thread-safe (pure). Compound decision -- MC/DC vectors in the test.
 * @see ra8_dfu_hdr_valid
 * @since 0.1.0
 */
bool ra8_dfu_run_target_valid(uint32_t entry, uint32_t img_len);

/**
 * @brief Copy an image to the SRAM run base and branch to it (copy-to-run).
 *
 * @details
 * The shared copy-to-run hand-off (firmware only): validates `entry` /
 * `img_len` with ::ra8_dfu_run_target_valid, copies `img_len` bytes from `src`
 * to the trusted ::k_ra8_dfu_run_base, then sets the Secure VTOR to the run base,
 * loads the image's initial MSP, and branches to its reset vector. Used by the
 * `dfu_bootloader` (launching a slot body) and the `dfu_copy_to_run` HIL demo
 * (launching an embedded image). The copy destination is always the run-base
 * constant, never `entry`, so a corrupted `entry` cannot redirect it.
 *
 * Interrupts are masked across the copy and switch; the launched image re-enables
 * them after its own bring-up. Under `RA8_OFF_TARGET` the branch is elided (a
 * host cannot reset MSP/VTOR), so the function validates and returns.
 *
 * @param[in] src     Source image base (image body, vector table first); non-zero.
 * @param[in] img_len Image body length in bytes; a non-zero 32-byte multiple.
 * @param[in] entry   The image's recorded run base; must equal ::k_ra8_dfu_run_base.
 *
 * @return void -- returns to the caller ONLY when the inputs fail validation
 *         (`src == 0`, or ::ra8_dfu_run_target_valid is false); on a valid image
 *         it does not return (control passes to the image).
 *
 * @pre `src` points at a readable image whose first two words are MSP + reset.
 * @pre The image was linked at ::k_ra8_dfu_run_base; the caller masks nothing
 *      special (this masks IRQs itself).
 * @post On a valid image, control is at the image's reset vector with VTOR / MSP
 *       pointing at the run base.
 * @post On invalid inputs, no copy or branch happens and control returns.
 *
 * @note Firmware only (inline MSP/VTOR/branch); not thread-safe.
 * @see ra8_dfu_run_target_valid
 * @since 0.1.0
 */
void ra8_dfu_launch(uintptr_t src, uint32_t img_len, uint32_t entry);

/**
 * @brief Pick the active slot from the two slots' validity + sequence.
 *
 * @details
 * The valid slot with the higher sequence number wins; Slot A wins a tie
 * (deterministic). If only one slot is valid, it wins; if neither is
 * valid, ::k_ra8_dfu_slot_none is returned so the caller enters DFU.
 *
 * @param[in] a_valid `true` iff Slot A passed ::ra8_dfu_hdr_valid.
 * @param[in] a_seq   Slot A header sequence number.
 * @param[in] b_valid `true` iff Slot B passed ::ra8_dfu_hdr_valid.
 * @param[in] b_seq   Slot B header sequence number.
 *
 * @return The selected slot, or ::k_ra8_dfu_slot_none.
 * @retval k_ra8_dfu_slot_a    A valid and (B invalid or `a_seq >= b_seq`).
 * @retval k_ra8_dfu_slot_b    B valid and (A invalid or `b_seq > a_seq`).
 * @retval k_ra8_dfu_slot_none Neither slot valid.
 *
 * @pre `a_valid` / `b_valid` reflect a prior ::ra8_dfu_hdr_valid result.
 * @pre `a_seq` / `b_seq` are the live header sequence values.
 * @post No state is mutated.
 * @post The returned slot identifier is always a defined ::ra8_dfu_slot_t value.
 *
 * @note Thread-safe (pure). Compound decision -- MC/DC vectors in the test.
 * @see ra8_dfu_boot_decide
 * @since 0.1.0
 */
ra8_dfu_slot_t ra8_dfu_select_slot(bool a_valid, uint32_t a_seq, bool b_valid, uint32_t b_seq);

/**
 * @brief Reset-time boot decision: jump to a slot, or enter DFU.
 *
 * @details
 * Returns ::k_ra8_dfu_action_dfu when the no-init DFU trigger is set or
 * when neither slot is valid; otherwise maps ::ra8_dfu_select_slot onto a
 * jump action. This is the single decision the bootloader's `main`
 * branches on.
 *
 * @param[in] dfu_trigger `true` iff the no-init SRAM DFU-request magic was set.
 * @param[in] a_valid     Slot A validity (::ra8_dfu_hdr_valid result).
 * @param[in] a_seq       Slot A header sequence number.
 * @param[in] b_valid     Slot B validity (::ra8_dfu_hdr_valid result).
 * @param[in] b_seq       Slot B header sequence number.
 *
 * @return The action the bootloader should take.
 * @retval k_ra8_dfu_action_dfu    Trigger set, or no valid slot.
 * @retval k_ra8_dfu_action_jump_a Boot Slot A.
 * @retval k_ra8_dfu_action_jump_b Boot Slot B.
 *
 * @pre Validity flags came from ::ra8_dfu_hdr_valid on the live headers.
 * @pre `dfu_trigger` reflects the one-shot no-init magic read at reset.
 * @post No state is mutated.
 * @post The returned action is always a defined ::ra8_dfu_action_t value.
 *
 * @note Thread-safe (pure). Compound decision -- MC/DC vectors in the test.
 * @see ra8_dfu_select_slot
 * @since 0.1.0
 */
ra8_dfu_action_t
ra8_dfu_boot_decide(bool dfu_trigger, bool a_valid, uint32_t a_seq, bool b_valid, uint32_t b_seq);

/* =============================================================================
 * MRAM slot access + program / verify (ra8_dfu_program.c)
 *
 * These touch the code-MRAM window. On the firmware target the program path
 * must execute from SRAM (the code-MRAM program loop must not run from MRAM --
 * see ra8_flash.h); each consumer's linker script places this TU + ra8_flash in
 * SRAM. The host test backs the MRAM window with fake memory, so the
 * full program -> read-back -> verify round-trip is unit-testable.
 * =============================================================================
 */

/**
 * @brief Return the MRAM base address of a slot.
 *
 * @details
 * Maps the ::ra8_dfu_slot_t identifier to its fixed MRAM window start address.
 * Slot A maps to ::k_ra8_dfu_slot_a_base and Slot B maps to
 * ::k_ra8_dfu_slot_b_base. Returns 0 for ::k_ra8_dfu_slot_none or any
 * out-of-range value so callers can detect the sentinel without a separate
 * validity test.
 *
 * @param[in] slot Slot identifier.
 *
 * @return The slot base address, or 0 for ::k_ra8_dfu_slot_none / an invalid id.
 * @retval k_ra8_dfu_slot_a_base Slot A (`slot == k_ra8_dfu_slot_a`).
 * @retval k_ra8_dfu_slot_b_base Slot B (`slot == k_ra8_dfu_slot_b`).
 * @retval 0                    `slot` is ::k_ra8_dfu_slot_none or out of range.
 *
 * @pre `slot` is a defined ::ra8_dfu_slot_t value.
 * @pre The caller does not pass a raw integer that has not been validated as a
 *      ::ra8_dfu_slot_t member.
 * @post No state is mutated.
 * @post The returned address, when non-zero, lies within the MRAM window
 *       `[k_ra8_dfu_mram_base, k_ra8_dfu_mram_base + k_ra8_dfu_mram_size)`.
 *
 * @note Thread-safe (pure; no statics).
 * @since 0.1.0
 */
uintptr_t ra8_dfu_slot_base(ra8_dfu_slot_t slot);

/**
 * @brief Return the opposite slot (A<->B).
 *
 * @details
 * Inverts the slot identifier: Slot A returns Slot B and any other value
 * (Slot B or ::k_ra8_dfu_slot_none) returns Slot A. Used by the DFU device
 * and host glue to locate the inactive (write-target) slot when the active
 * slot is known. Passing ::k_ra8_dfu_slot_none returns ::k_ra8_dfu_slot_a by
 * the else branch, consistent with "no preference -> start with A".
 *
 * @param[in] slot Slot identifier (A or B).
 *
 * @return The complementary slot identifier.
 * @retval k_ra8_dfu_slot_b Input was ::k_ra8_dfu_slot_a.
 * @retval k_ra8_dfu_slot_a Input was ::k_ra8_dfu_slot_b or ::k_ra8_dfu_slot_none.
 *
 * @pre `slot` is a defined ::ra8_dfu_slot_t value.
 * @pre The caller understands that ::k_ra8_dfu_slot_none maps to
 *      ::k_ra8_dfu_slot_a (not a programming error; see details above).
 * @post No state is mutated.
 * @post The returned slot is always either ::k_ra8_dfu_slot_a or
 *       ::k_ra8_dfu_slot_b (never ::k_ra8_dfu_slot_none).
 *
 * @note Thread-safe (pure; no statics).
 * @since 0.1.0
 */
ra8_dfu_slot_t ra8_dfu_other_slot(ra8_dfu_slot_t slot);

/**
 * @brief Copy a slot's 32-byte header out of MRAM.
 *
 * @param[in]  slot    Slot to read (A or B).
 * @param[out] out_hdr Destination header (non-NULL).
 * @return `ra8_err_t` outcome.
 * @retval k_ra8_ok              Header copied.
 * @retval k_ra8_err_invalid_arg `slot` is none/invalid.
 * @retval k_ra8_err_null_ptr    `out_hdr` is NULL.
 *
 * @pre `out_hdr` non-NULL; `slot` is A or B.
 * @pre The MRAM window is readable (always true post-reset / off-target).
 * @post `*out_hdr` holds the slot's first 32 bytes; no MRAM mutated.
 * @note Not thread-safe vs a concurrent program of the same slot. @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_dfu_read_header(ra8_dfu_slot_t slot, ra8_dfu_img_hdr_t* out_hdr);

/**
 * @brief Validate a slot live: header magic/length/CRC over its real image.
 *
 * @details Reads the slot header, then folds ::ra8_dfu_crc32 over the live
 * image body in MRAM and applies ::ra8_dfu_hdr_valid.
 *
 * @param[in] slot Slot to validate (A or B).
 * @return `true` iff the slot holds a valid bootable image.
 * @retval true  Header + image CRC pass.
 * @retval false `slot` is none/invalid, or any check fails.
 *
 * @pre `slot` is A or B; the MRAM window is readable.
 * @pre The caller has not concurrently issued a program operation to the
 *      same slot (read-only -- not thread-safe vs a concurrent writer).
 * @post No MRAM is mutated.
 * @post The function result accurately reflects the slot state at the time
 *       of the call; a concurrent write may invalidate it immediately after.
 *
 * @note Reads up to `img_len` bytes of MRAM; not thread-safe vs a writer.
 * @since 0.1.0
 */
bool ra8_dfu_slot_valid(ra8_dfu_slot_t slot);

/**
 * @brief Read a slot header's sequence number (0 if the magic is wrong).
 *
 * @param[in]  slot    Slot to read (A or B).
 * @param[out] out_seq Destination sequence (non-NULL); 0 when magic mismatches.
 * @return `ra8_err_t` outcome.
 * @retval k_ra8_ok              `*out_seq` populated (0 if header magic wrong).
 * @retval k_ra8_err_invalid_arg `slot` is none/invalid.
 * @retval k_ra8_err_null_ptr    `out_seq` is NULL.
 *
 * @pre `out_seq` non-NULL; `slot` is A or B.
 * @post No MRAM mutated.
 * @note Used to pick the next monotonic seq (other-slot seq + 1). @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_dfu_slot_seq(ra8_dfu_slot_t slot, uint32_t* out_seq);

/**
 * @brief Open ra8_flash and fence all writes to one slot's window.
 *
 * @details Opens the MRAM controller (::ra8_flash_open) and sets the software
 * access window (::ra8_flash_set_window) to exactly the inactive slot so any
 * stray write outside it is rejected. It does NOT erase: there is no full-slot
 * erase (which would block USB long enough to time out the host's
 * DFU_GETSTATUS), and no header pre-erase either. Instead ::ra8_dfu_program_image
 * erases each 32-byte page to 0xFF immediately before programming its body, and
 * ::ra8_dfu_program_commit writes the header LAST -- so a torn download leaves
 * the OLD header over a NEW partial image, whose CRC will not match -> invalid.
 * Brick defense: the bootloader region and the active slot are outside the
 * window and untouchable.
 *
 * @param[in] inactive The slot to (re)program (A or B; must NOT be the running one).
 * @return `ra8_err_t` outcome.
 * @retval k_ra8_ok              Slot opened and the write window fenced to it.
 * @retval k_ra8_err_invalid_arg `inactive` is none/invalid.
 * @retval k_ra8_err_hw_error    Controller bring-up reported an error.
 *
 * @pre `inactive` is A or B and is not the slot the caller is executing from.
 * @pre Caller runs from SRAM (firmware) -- the program loop must not be in MRAM.
 * @post On success the access window is exactly `[slot_base, slot_base + size)`.
 * @post The MRAM program-control gate is locked on every exit path.
 * @note Not thread-safe; single programmer. @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_dfu_program_prepare(ra8_dfu_slot_t inactive);

/**
 * @brief Program one image chunk into the inactive slot's body.
 *
 * @details Writes `len` bytes at `slot_base + img_offset` through the SECURE
 * MRAM gate (`k_ra8_flash_world_s`). Each 32-byte page is erased to 0xFF then
 * programmed, IRQ-masked, by the SRAM-resident `internal_write_secure`
 * (ra8_dfu_program.c). The header page (the slot's last page, at
 * `slot_base + k_ra8_dfu_hdr_offset`) is left untouched until
 * ::ra8_dfu_program_commit, so a torn download never leaves a valid-looking
 * header over a partial image.
 *
 * @param[in] inactive   Slot being programmed (must match ::ra8_dfu_program_prepare).
 * @param[in] img_offset Byte offset into the image body; 32-byte aligned.
 * @param[in] data       Source bytes (non-NULL).
 * @param[in] len        Length in bytes; non-zero multiple of `k_ra8_dfu_page_size`.
 * @return `ra8_err_t` outcome.
 * @retval k_ra8_ok              Chunk programmed.
 * @retval k_ra8_err_null_ptr    `data` is NULL.
 * @retval k_ra8_err_invalid_arg `inactive` none/invalid, bad alignment, or the
 *                              chunk would exceed `k_ra8_dfu_img_max`.
 * @retval k_ra8_err_out_of_range The access window rejected the address.
 * @retval k_ra8_err_hw_error    Controller reported a program error.
 *
 * @pre ::ra8_dfu_program_prepare ran for `inactive`; `data` non-NULL.
 * @pre `img_offset` and `len` are 32-byte multiples; `img_offset + len <= img_max`.
 * @post On success the chunk holds `data`; the program gate is locked.
 * @note Not thread-safe; SRAM-resident on firmware. @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_dfu_program_image(ra8_dfu_slot_t inactive,
                                              uint32_t       img_offset,
                                              const uint8_t* data,
                                              uint32_t       len);

/**
 * @brief Finalize a slot: CRC the programmed body, then program the header.
 *
 * @details Folds ::ra8_dfu_crc32 over the just-programmed body in MRAM, builds
 * a ::ra8_dfu_img_hdr_t (magic, the given `seq`, `img_len`, that CRC, and
 * `entry = k_ra8_dfu_run_base`), and programs the 32-byte header LAST into the slot's
 * last page. After this the slot is bootable; a power loss before the header
 * lands leaves the slot header erased (invalid) -- never half-valid.
 *
 * @param[in] inactive Slot being committed.
 * @param[in] img_len  Total image-body length programmed; 32-byte multiple.
 * @param[in] seq      Sequence number to stamp (caller uses other-slot seq + 1).
 * @return `ra8_err_t` outcome.
 * @retval k_ra8_ok              Header programmed; slot now valid.
 * @retval k_ra8_err_invalid_arg `inactive` none/invalid or `img_len` out of range.
 * @retval k_ra8_err_hw_error    Controller reported a program error.
 *
 * @pre All body chunks for `inactive` were programmed via ::ra8_dfu_program_image.
 * @pre `img_len` is a non-zero 32-byte multiple `<= k_ra8_dfu_img_max`.
 * @post On success ::ra8_dfu_slot_valid(`inactive`) is true.
 * @note Not thread-safe; SRAM-resident on firmware. @since 0.1.0
 */
[[nodiscard]] ra8_err_t
ra8_dfu_program_commit(ra8_dfu_slot_t inactive, uint32_t img_len, uint32_t seq);

/**
 * @brief Read-back verify: re-CRC a slot's body against its stored header.
 *
 * @param[in] slot Slot to verify (A or B).
 * @return `ra8_err_t` outcome.
 * @retval k_ra8_ok               Header valid and the body CRC matches.
 * @retval k_ra8_err_invalid_arg  `slot` is none/invalid.
 * @retval k_ra8_err_crc_mismatch Header invalid or the recomputed CRC differs.
 *
 * @pre `slot` is A or B; the MRAM window is readable.
 * @post No MRAM mutated.
 * @note Independent of ::ra8_dfu_program_commit's in-flight CRC. @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_dfu_program_verify(ra8_dfu_slot_t slot);

#ifdef __cplusplus
}
#endif
