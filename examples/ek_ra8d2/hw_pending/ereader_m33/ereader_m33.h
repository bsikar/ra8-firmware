/**
 * @file examples/ek_ra8d2/hw_pending/ereader_m33/ereader_m33.h
 * @brief Shared-SRAM contract for the "e-reader on the M33" dual-core demo
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * This header pins the cross-core progress mailbox for the #150 power-saving
 * model at one fixed shared-SRAM address both core images agree on. The
 * Cortex-M85 (primary, "CPU0") does the one-time heavy work -- it owns the
 * baked book and could open / compile it at 1 GHz -- then hands the *reader*
 * to the Cortex-M33 (secondary, "CPU1") and PARKS. The slow core runs the
 * actual reader page-turn loop: it walks the real `RABOOK1` chapter / DOM
 * structure (`ra_book.h` inline accessors), paginates the extracted text, and
 * advances through a short sequence of pages -- exactly the idle / hold / turn
 * loop an e-reader spends almost all its time in, now at a fraction of the
 * power while the M85 @ 1 GHz sleeps in WFI.
 *
 * Why a fixed address: each core is a separate compiled image with its own
 * linker script, so a `static` global in one image is invisible to the other.
 * Per HUM Ch 58.1 "SRAM" Table 58.1 p 3527 the on-chip data SRAM is SRAM0..SRAM2
 * (512 KiB each) plus SRAM3 (128 KiB), spanning 0x22000000..0x221A0000. The M85
 * image owns SRAM0+SRAM1 (0x22000000..0x22100000) and the M33 image owns the top
 * 64 KiB of SRAM3 (0x22190000..0x221A0000); the mailbox word at 0x22100000 (the
 * start of SRAM2) is left unclaimed by both, so it is backed by the same
 * physical SRAM on both sides with no overlap.
 *
 * Coherency: this app's `system_init.c` leaves the M85 data cache OFF, so a
 * store from one core is visible to the other once a `dsb` has drained the
 * write buffer; no cache clean / invalidate dance is needed. The fields are
 * `volatile` so the compiler emits a real load / store on every access.
 *
 * Protocol (the reader handoff):
 *   1. M85 zeros the block, stamps ::k_erm33_magic, `dsb`.
 *   2. M85 releases the M33 and PARKS (WFI). The M33 now runs the reader.
 *   3. M33 stamps ::k_erm33_m33_sig, validates the baked book, then for each
 *      chapter walks its DOM and turns a page every ::k_erm33_page_chars
 *      characters of extracted text: it bumps `page_idx`, mirrors the current
 *      `chapter_idx`, and advances `heartbeat` on each turn.
 *   4. On the last page the M33 publishes `total_pages` and sets `done = 1`.
 *   5. M85 narrates each page advance from the mailbox and logs
 *      "ereader_m33 PASS" once the M33 reaches the last page.
 *
 * @note The next increment (actual pixel render on the M33) needs a panel /
 *       framebuffer plane handed to the M33 and an M33-side `ra_gfx` path; this
 *       increment proves the secondary core runs the reader's *data* path.
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
 * @enum erm33_mailbox_addr_t
 * @brief Fixed shared-SRAM base address of the progress mailbox.
 *
 * @details The start of data-SRAM bank SRAM2 (0x22100000), the first word above
 * the M85 image's 1 MiB SRAM0+SRAM1 allocation and well below the M33 image's
 * SRAM3 block at 0x22190000, so both linker scripts leave it free. Declared
 * `uintptr_t` so the same constant casts to a pointer correctly on the 32-bit
 * target and the 64-bit unit-test host.
 *
 * @invariant The address is 16-byte aligned (cache-line safe).
 * @see erm33_mailbox()
 * @since 0.1.0
 */
/* HUM Ch 58.1 "SRAM" Table 58.1 p 3527 -- SRAM2 (data bank 2) begins at 0x22100000 */
typedef enum : uintptr_t {
  k_erm33_mailbox_addr = 0x22100000U, /**< Shared mailbox base (SRAM2 start). */
} erm33_mailbox_addr_t;

/**
 * @enum erm33_const_t
 * @brief Signatures, status codes, and the pagination width shared by both
 *        core images.
 *
 * @details ::k_erm33_magic ("ERM3") lets the M33 confirm the mailbox is live
 * before trusting it; ::k_erm33_m33_sig is the boot sentinel the M33 stamps so
 * the M85 can prove the second core left reset. ::k_erm33_page_chars is the
 * fixed character width of one rendered page -- the M33 turns a page each time
 * it accumulates this many characters of extracted chapter text.
 *
 * @see erm33_mailbox_t
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_erm33_magic           = 0x45524D33U, /**< "ERM3" -- M85 stamps it when ready.    */
  k_erm33_m33_sig         = 0x33524452U, /**< "RDR3" boot sentinel written by M33.   */
  k_erm33_status_running  = 0U,          /**< status: M33 is walking the book.       */
  k_erm33_status_ok       = 1U,          /**< status: baked book validated + walked. */
  k_erm33_status_bad_book = 2U,          /**< status: baked book failed validation.  */
  k_erm33_page_chars      = 64U,         /**< Characters of text per rendered page.  */
} erm33_const_t;

/**
 * @struct erm33_mailbox_t
 * @brief Cross-core progress block backed by a fixed shared-SRAM address.
 *
 * @details All fields are `volatile` so each core re-reads memory rather than
 * caching a value in a register. The M85 owns `magic` (writer); the M33 owns
 * every other field (writer). The M85 only reads what it does not own.
 *
 * @invariant `magic` is 0 until the M85 stamps ::k_erm33_magic, then holds.
 * @invariant `page_idx` increases monotonically from 0 to `total_pages`.
 * @invariant `done` is 0 until the M33 publishes the last page, then 1 forever.
 *
 * @par Example:
 * @code
 * volatile erm33_mailbox_t* mb = erm33_mailbox();
 * while (mb->done == 0U) {}              // wait for the M33 reader
 * uint32_t pages = mb->total_pages;      // read the page count it walked
 * @endcode
 *
 * @see erm33_mailbox()
 * @since 0.1.0
 */
typedef struct {
  volatile uint32_t magic;       /**< M85 stamps ::k_erm33_magic when ready.          */
  volatile uint32_t m33_sig;     /**< M33 stamps ::k_erm33_m33_sig on boot.           */
  volatile uint32_t status;      /**< Walk status (::erm33_const_t status codes).     */
  volatile uint32_t chapter_idx; /**< Chapter the M33 is currently reading (0-based). */
  volatile uint32_t page_idx;    /**< Pages turned so far / current page number.      */
  volatile uint32_t total_pages; /**< Final page count; published with `done`.        */
  volatile uint32_t heartbeat;   /**< M33 bumps on every page turn (liveness).        */
  volatile uint32_t done;        /**< Set to 1 by the M33 on the last page.           */
} erm33_mailbox_t;

/**
 * @brief Typed pointer to the fixed-address shared progress mailbox.
 *
 * @details Inlined so both core images compute the identical address with no
 * shared translation unit. Returns the same physical SRAM location on the M85
 * and the M33.
 *
 * @return Pointer to the mailbox at ::k_erm33_mailbox_addr.
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
static inline volatile erm33_mailbox_t* erm33_mailbox(void)
{
  return (volatile erm33_mailbox_t*)(uintptr_t)k_erm33_mailbox_addr;
}

#ifdef __cplusplus
}
#endif
