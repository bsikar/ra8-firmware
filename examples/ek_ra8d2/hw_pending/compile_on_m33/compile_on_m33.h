/**
 * @file examples/ek_ra8d2/hw_pending/compile_on_m33/compile_on_m33.h
 * @brief Shared-SRAM contract for the "RABOOK1 emitter on the M33" demo (#149b)
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * This header pins the cross-core mailbox and the shared output buffer for the
 * first #149(b) increment: running the `.rabook` compiler *back-end* (the
 * RABOOK1 emitter, `libs/ra_rabook_compile`) on the RA8D2's Cortex-M33 secondary
 * core. The Cortex-M85 (primary, "CPU0") releases the M33, then PARKS while the
 * slow core drives the zero-heap emitter API over a hand-built tiny DOM and lays
 * a complete RABOOK1 blob into shared SRAM. The M85 then validates that blob with
 * @ref ra_book_validate -- proving the secondary core produced a well-formed,
 * CRC-intact compiled book the primary core can consume.
 *
 * Why a fixed address: each core is a separate compiled image with its own
 * linker script, so a `static` global in one image is invisible to the other.
 * The one name both images resolve identically is a hard-coded address.
 *
 * Memory budget (all in the shared upper SRAM window, see the address enum):
 *   - mailbox        : 32 bytes at 0x22100000 (8 `volatile uint32_t` fields).
 *   - output blob    : ::k_com33_blob_cap bytes at 0x22100000 + 0x100; the M33
 *                      emitter writes the finalized RABOOK1 blob here so the M85
 *                      reads it with no copy. 8 KiB is ample for the tiny
 *                      2-chapter book this demo builds (a few hundred bytes).
 *   - emitter scratch: the chapter / node / attr / string-pool arenas the
 *                      emitter appends into are NOT shared -- they live in the
 *                      M33 image's own SRAM_CPU1 `.bss` (0x22190000 bank) and the
 *                      M85 never touches them. Only the finished blob is shared.
 *
 * Coherency: this app's `system_init.c` leaves the M85 data cache OFF, so a store
 * from one core is visible to the other once a `dsb` has drained the write
 * buffer; no cache clean / invalidate dance is needed. The mailbox fields are
 * `volatile` so the compiler emits a real load / store on every access.
 *
 * Protocol (the compile handoff):
 *   1. M85 zeros the mailbox, stamps ::k_com33_magic, `dsb`.
 *   2. M85 releases the M33 and PARKS. The M33 now runs the emitter.
 *   3. M33 stamps ::k_com33_m33_sig, builds a tiny DOM, drives the emitter
 *      (intern / add_element / add_text / link / add_chapter / set_metadata /
 *      finalize) into the shared output blob, then publishes `blob_base`,
 *      `blob_len`, `blob_crc` (the blob header's body CRC-32) and `chapter_count`.
 *   4. M33 sets `status = ok` then `done = 1` (or `status = build_fail`, `done`).
 *   5. M85 runs @ref ra_book_validate over the shared blob, cross-checks the
 *      reported CRC and chapter count, and logs "compile_on_m33 PASS".
 *
 * @note This increment proves the emitter (back-end) runs on the M33. A full
 *       `ra_epub_open` (unzip + XHTML parse + image transcode) on the M33 is a
 *       heavier later increment of #149(b): it pulls in miniz, tinyxml2 and
 *       stb_image, none of which this freestanding M33 image links today.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @enum com33_addr_t
 * @brief Fixed shared-SRAM addresses for the mailbox and the output blob.
 *
 * @details Both addresses sit in the upper on-chip SRAM region that NEITHER
 * linker script claims: the M85 image owns the lower 1 MiB (0x22000000 ..
 * 0x22100000) and the M33 image owns the top 64 KiB of SRAM3 (0x22190000 ..
 * 0x221A0000), so the 576 KiB window starting at 0x22100000 (the start of data
 * bank SRAM2) is free for cross-core sharing. The mailbox occupies the first 32
 * bytes; the output blob starts 256 bytes in, leaving the mailbox its own cache
 * line and growth room. Declared `uintptr_t` so the same constant casts to a
 * pointer correctly on the 32-bit target and the 64-bit unit-test host.
 *
 * @invariant ::k_com33_blob_addr + ::k_com33_blob_cap stays below 0x22190000.
 * @see com33_mailbox()
 * @see com33_blob()
 * @since 0.1.0
 */
/* HUM Ch 5.1 "Address Space (Table 5.1)" p 239 */
/* HUM Ch 58.1 "SRAM" Table 58.1 p 3527 -- dual-core on-chip SRAM spans
   0x22000000..0x221A0000; SRAM2 (the shared upper window) begins at 0x22100000. */
typedef enum : uintptr_t {
  k_com33_mailbox_addr = 0x22100000U, /**< Shared mailbox base (SRAM2 start).        */
  k_com33_blob_addr    = 0x22100100U, /**< Shared output blob base (mailbox + 256B). */
} com33_addr_t;

/**
 * @enum com33_const_t
 * @brief Magics, the boot signature, and the build-status codes shared by both
 *        core images.
 *
 * @details ::k_com33_magic ("COM3") lets the M33 confirm the mailbox is live
 * before trusting it; ::k_com33_m33_sig is the boot sentinel the M33 stamps so
 * the M85 can prove the second core left reset and is executing user code. The
 * status codes report the outcome of the emitter run the M33 performed.
 *
 * @see com33_mailbox_t
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_com33_magic             = 0x434F4D33U, /**< "COM3" -- M85 stamps it when ready.        */
  k_com33_m33_sig           = 0x4D33C0DEU, /**< "M33 CODE" boot sentinel written by M33.   */
  k_com33_status_running    = 0U,          /**< status: M33 is still building the blob.    */
  k_com33_status_ok         = 1U,          /**< status: emitter finalized a valid blob.    */
  k_com33_status_build_fail = 2U,          /**< status: an emitter call overflowed/failed. */
} com33_const_t;

/**
 * @enum com33_blob_cap_t
 * @brief Capacity, in bytes, of the shared output-blob buffer.
 * @details The emitter lays the finalized RABOOK1 blob into the shared buffer at
 *          ::k_com33_blob_addr; this is the buffer's `out_cap`. 8 KiB dwarfs the
 *          few-hundred-byte tiny book this demo emits, so the build never
 *          overflows the output arena.
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_com33_blob_cap = 8192U, /**< Shared output-blob buffer capacity in bytes. */
} com33_blob_cap_t;

/**
 * @struct com33_mailbox_t
 * @brief Cross-core handoff block backed by a fixed shared-SRAM address.
 *
 * @details All fields are `volatile` so each core re-reads memory rather than
 * caching a value in a register. The M85 owns `magic` (writer); the M33 owns
 * every other field (writer). The M85 only reads what it does not own, and only
 * after observing `done == 1` behind a `dmb`.
 *
 * @invariant `magic` is 0 until the M85 stamps ::k_com33_magic, then holds.
 * @invariant `done` is 0 until the M33 publishes the blob, then 1 forever.
 * @invariant On `status == ok`, `blob_base == k_com33_blob_addr` and
 *            `0 < blob_len <= k_com33_blob_cap`.
 *
 * @par Example:
 * @code
 * volatile com33_mailbox_t* mb = com33_mailbox();
 * while (mb->done == 0U) {}                         // wait for the M33 emitter
 * ra_err_t rc = ra_book_validate((const void*)mb->blob_base, mb->blob_len);
 * @endcode
 *
 * @see com33_mailbox()
 * @since 0.1.0
 */
typedef struct {
  volatile uint32_t magic;         /**< M85 stamps ::k_com33_magic when ready.            */
  volatile uint32_t m33_sig;       /**< M33 stamps ::k_com33_m33_sig on boot.             */
  volatile uint32_t status;        /**< Emitter outcome (::com33_const_t status codes).   */
  volatile uint32_t blob_base;     /**< Address of the finalized blob (= blob buffer).    */
  volatile uint32_t blob_len;      /**< Finalized RABOOK1 blob length, bytes.             */
  volatile uint32_t blob_crc;      /**< Blob header body CRC-32, echoed for cross-check.  */
  volatile uint32_t chapter_count; /**< Chapters the M33 emitted into the blob.           */
  volatile uint32_t done;          /**< Set to 1 by the M33 once the blob is published.   */
} com33_mailbox_t;

/**
 * @brief Typed pointer to the fixed-address shared mailbox.
 *
 * @details Inlined so both core images compute the identical address with no
 * shared translation unit. Returns the same physical SRAM location on the M85
 * and the M33.
 *
 * @return Pointer to the mailbox at ::k_com33_mailbox_addr.
 * @retval non-NULL Always; the address is a compile-time constant.
 *
 * @pre The linker scripts of both images leave the mailbox word unallocated.
 * @pre The M85 data cache is disabled (see file header).
 * @post Returns a valid `volatile` pointer; never NULL.
 * @post No side effects.
 *
 * @note Callable from either core; the pointer arithmetic is identical.
 * @since 0.1.0
 */
static inline volatile com33_mailbox_t* com33_mailbox(void)
{
  return (volatile com33_mailbox_t*)(uintptr_t)k_com33_mailbox_addr;
}

/**
 * @brief Typed pointer to the fixed-address shared output-blob buffer.
 *
 * @details The emitter's `out` arena and the M85's validation target are the
 * same physical SRAM bytes at ::k_com33_blob_addr; this inline returns that base
 * identically on both images so no shared translation unit is needed.
 *
 * @return Pointer to the blob buffer at ::k_com33_blob_addr.
 * @retval non-NULL Always; the address is a compile-time constant.
 *
 * @pre Both linker scripts leave the ::k_com33_blob_cap bytes from
 *      ::k_com33_blob_addr unallocated.
 * @pre The M85 data cache is disabled (see file header).
 * @post Returns a valid pointer; never NULL.
 * @post No side effects.
 *
 * @note Callable from either core; the pointer arithmetic is identical.
 * @since 0.1.0
 */
static inline uint8_t* com33_blob(void)
{
  return (uint8_t*)(uintptr_t)k_com33_blob_addr;
}

#ifdef __cplusplus
}
#endif
