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
 * Protocol (the reader handoff + the page render handshake):
 *   1. M85 zeros the block, stamps ::k_erm33_magic, `dsb`.
 *   2. M85 releases the M33 and PARKS (WFI). The M33 now runs the reader.
 *   3. M33 stamps ::k_erm33_m33_sig, validates the baked book, then for each
 *      chapter walks its DOM and fills a page every ::k_erm33_page_chars
 *      characters of extracted text. For each page the M33 RENDERS the page's
 *      text into the shared 1-bpp framebuffer (::erm33_framebuffer, a compact
 *      baked-font mono blit), bumps `page_idx`, mirrors `chapter_idx`, advances
 *      `heartbeat`, then publishes `frame_seq` and HOLDS the rendered page until
 *      the M85 acknowledges it via `frame_ack` (the slow core's "hold the page"
 *      posture) before walking on.
 *   4. On the last page the M33 publishes `total_pages` and sets `done = 1`.
 *   5. For every new `frame_seq` the M85 reads the framebuffer the M33 produced,
 *      logs a per-page CRC (proving real pixels crossed shared memory, not a
 *      blank plane), writes `frame_ack` back, and logs "ereader_m33 PASS" once
 *      the M33 reaches the last page.
 *
 * @note The framebuffer is a small 1-bpp text plane the M33 owns in shared SRAM
 *       (::erm33_framebuffer); it is NOT yet the GLCDC scan-out plane. Wiring an
 *       M33-side `ra_gfx` + real panel path is the next increment; this one
 *       proves the secondary core renders genuine page pixels, not just walks
 *       the reader's *data* path.
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
 * @enum erm33_framebuffer_addr_t
 * @brief Fixed shared-SRAM base address of the M33-owned page framebuffer.
 *
 * @details Sits 4 KiB above the mailbox base, still inside SRAM2
 * (0x22100000..0x22180000), so it is clear of the mailbox header at
 * 0x22100000, the M85 image (SRAM0+SRAM1, below 0x22100000), and the M33 image
 * (top 64 KiB of SRAM3, 0x22190000..0x221A0000). The 4 KiB gap leaves the
 * mailbox room to grow without ever reaching the framebuffer. The M33 writes
 * the page pixels here; the parked M85 reads them back to CRC each page.
 * Declared `uintptr_t` so the constant casts to a pointer correctly on both the
 * 32-bit target and the 64-bit unit-test host.
 *
 * @invariant The address is 4 KiB aligned and inside SRAM2.
 * @see erm33_framebuffer()
 * @since 0.1.0
 */
/* HUM Ch 58.1 "SRAM" Table 58.1 p 3527 -- SRAM2 (0x22100000..0x22180000) */
typedef enum : uintptr_t {
  k_erm33_fb_addr = 0x22101000U, /**< Shared framebuffer base (SRAM2 + 4 KiB). */
} erm33_framebuffer_addr_t;

/**
 * @enum erm33_framebuffer_geom_t
 * @brief Geometry of the shared 1-bpp page framebuffer both images agree on.
 *
 * @details A monochrome (1 bit per pixel, MSB-left) text plane sized to hold one
 * rendered page. With the 8-pixel-wide baked font each glyph cell is exactly one
 * byte, so the byte column equals the glyph column and the mono blit is a plain
 * per-row byte copy. The plane is ::k_erm33_fb_cols glyph columns by
 * ::k_erm33_fb_rows glyph rows, comfortably more than the ::k_erm33_page_chars
 * characters one page carries.
 *
 * @invariant `k_erm33_fb_stride == k_erm33_fb_width / 8`.
 * @invariant `k_erm33_fb_bytes  == k_erm33_fb_stride * k_erm33_fb_height`.
 * @invariant `k_erm33_fb_cols * k_erm33_fb_rows >= k_erm33_page_chars`.
 * @see erm33_framebuffer()
 * @since 0.1.0
 */
typedef enum : uint16_t {
  k_erm33_fb_width  = 256U,  /**< Plane width in pixels.                       */
  k_erm33_fb_height = 64U,   /**< Plane height in pixels (4 glyph rows of 16). */
  k_erm33_fb_stride = 32U,   /**< Bytes per pixel row (width / 8, 1 bpp).      */
  k_erm33_fb_bytes  = 2048U, /**< Total plane size in bytes (stride * height). */
  k_erm33_fb_cols   = 32U,   /**< Glyph columns (width / 8-px glyph).          */
  k_erm33_fb_rows   = 4U,    /**< Glyph rows (height / 16-px glyph).           */
} erm33_framebuffer_geom_t;

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
 * @invariant `frame_ack <= frame_seq` always (the M85 trails the M33 producer).
 *
 * @par Example:
 * @code
 * volatile erm33_mailbox_t* mb = erm33_mailbox();
 * while (mb->done == 0U) {}              // wait for the M33 reader
 * uint32_t pages = mb->total_pages;      // read the page count it walked
 * @endcode
 *
 * @see erm33_mailbox()
 * @see erm33_framebuffer()
 * @since 0.1.0
 */
typedef struct {
  volatile uint32_t magic;       /**< M85 stamps ::k_erm33_magic when ready.           */
  volatile uint32_t m33_sig;     /**< M33 stamps ::k_erm33_m33_sig on boot.            */
  volatile uint32_t status;      /**< Walk status (::erm33_const_t status codes).      */
  volatile uint32_t chapter_idx; /**< Chapter the M33 is currently reading (0-based).  */
  volatile uint32_t page_idx;    /**< Pages turned so far / current page number.       */
  volatile uint32_t total_pages; /**< Final page count; published with `done`.         */
  volatile uint32_t heartbeat;   /**< M33 bumps on every page turn (liveness).         */
  volatile uint32_t done;        /**< Set to 1 by the M33 on the last page.            */
  volatile uint32_t frame_seq;   /**< M33 sets to the page whose pixels are in the FB. */
  volatile uint32_t frame_ack;   /**< M85 sets after it CRCs `frame_seq`'s pixels.     */
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

/**
 * @brief Typed pointer to the fixed-address shared page framebuffer.
 *
 * @details Inlined so both core images compute the identical address with no
 * shared translation unit. Returns the same physical SRAM2 location on the M33
 * (writer) and the M85 (reader). The plane is ::k_erm33_fb_bytes bytes of
 * 1-bpp pixels (::erm33_framebuffer_geom_t); element @c [y * k_erm33_fb_stride
 * + x] holds 8 horizontally-packed pixels, MSB leftmost.
 *
 * @return Pointer to the framebuffer at ::k_erm33_fb_addr.
 * @retval non-NULL Always; the address is a compile-time constant.
 *
 * @pre The linker scripts of both images leave SRAM2 above the mailbox free.
 * @pre The M85 data cache is disabled (see file header) so a `dsb`-published
 *      M33 store is visible to the M85 read.
 * @post Returns a valid `volatile` pointer; never NULL.
 * @post No side effects.
 *
 * @note Callable from either core; the pointer arithmetic is identical.
 * @see erm33_mailbox()
 * @since 0.1.0
 */
static inline volatile uint8_t* erm33_framebuffer(void)
{
  return (volatile uint8_t*)(uintptr_t)k_erm33_fb_addr;
}

#ifdef __cplusplus
}
#endif
