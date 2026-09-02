/**
 * @file examples/ek_ra8d2/hw_pending/compile_on_m33/inc/compile_on_m33.h
 * @brief Shared-SRAM contract for the "RABOOK1 emitter on the M33" demo (#149b)
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * This header pins the cross-core JOB mailbox and the shared input/output buffers
 * for the #149 compiler offload: running the full text/CSS/SVG EPUB->`.rabook`
 * compile (`apps/shared_libs/rabook_compile` + `epub`) on the RA8D2's Cortex-M33
 * secondary core. The Cortex-M85 (primary, "CPU0") STAGES a source `.epub` into
 * shared SRAM, posts the job, releases the M33, then PARKS while the slow core
 * compiles the staged book and lays a complete RABOOK1 blob into the shared
 * output buffer. The M85 then validates that blob with @ref book_validate AND
 * byte-compares it to the desktop/M85 golden -- proving the secondary core
 * produced the identical compiled book the primary core would have.
 *
 * Why a fixed address: each core is a separate compiled image with its own
 * linker script, so a `static` global in one image is invisible to the other.
 * The one name both images resolve identically is a hard-coded address.
 *
 * Memory budget (all in the shared upper SRAM window, see the address enum):
 *   - mailbox       : the ::com33_mailbox_t request/response block at 0x22100000.
 *   - staged .epub  : ::k_com33_epub_cap bytes at ::k_com33_epub_addr; the M85
 *                     copies the source `.epub` here and the M33 epub_opens it.
 *   - output blob   : ::k_com33_blob_cap bytes at ::k_com33_blob_addr; the M33
 *                     writes the finalized RABOOK1 blob here so the M85 reads it
 *                     with no copy.
 *   - compile arenas: the M33's epub_book_t + builder pools + scratch are NOT
 *                     shared -- they live in the M33 image's external SDRAM
 *                     (`.sdram_bss`); only the staged .epub + the blob are shared.
 *
 * Coherency: this app's `system_init.c` leaves the M85 data cache OFF, so a store
 * from one core is visible to the other once a `dsb` has drained the write
 * buffer; no cache clean / invalidate dance is needed. The mailbox fields are
 * `volatile` so the compiler emits a real load / store on every access.
 *
 * Protocol (the compile job handoff):
 *   1. M85 zeros the response, stamps ::k_com33_magic, stages the `.epub` into
 *      shared SRAM, fills the request (epub base/len, out base/cap), and stamps
 *      ::k_com33_req_magic LAST behind a `dsb`.
 *   2. M85 releases the M33 and PARKS. The M33 now compiles.
 *   3. M33 stamps ::k_com33_m33_sig, reads the request, epub_opens the staged
 *      bytes, runs rabook_compile_from_epub_to_buffer into the shared output
 *      buffer, then publishes `blob_base` / `blob_len` / `blob_crc` (the blob
 *      header's body CRC-32) / `chapter_count`.
 *   4. M33 sets `status = ok` then `done = 1` (or a failure status, then `done`).
 *   5. M85 validates the shared blob, byte-compares it to the golden, and logs
 *      "compile_on_m33 PASS" (or traps so ra8_emulator's smoke flags a divergence).
 *
 * @note The M33 image links miniz + bounded XML reader + epub + the rabook pipeline but
 *       NOT stb_image (raster is compiled out via `RA8_RABOOK_NO_RASTER`; the SVG
 *       path stores verbatim) and NOT ra8_fs (the M85 owns the filesystem; the M33
 *       finalizes into the shared buffer).
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#pragma once

#include <stdint.h>

#include "ra8_board_ek_ra8d2_dualcore.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @enum com33_addr_t
 * @brief Fixed shared-SRAM addresses for the mailbox and the output blob.
 *
 * @details All three slices are carved from the CPU0 <-> CPU1 window at fixed
 * offsets: the mailbox occupies the first 32 bytes, the staged input starts 256
 * bytes in so the mailbox keeps its own cache line and some growth room, and
 * the output blob follows 32 KiB after that.
 * The window itself is a board fact, declared once in
 * ``ra8_board_ek_ra8d2_dualcore.h`` along with why both linker scripts leave
 * it free; this app only names its own slice of it.
 *
 * @invariant ::k_com33_blob_addr + ::k_com33_blob_cap stays below
 *            ::k_ra8_board_cpu1_sram_base -- the top of the shared window.
 * @see ra8_board_dualcore_addr_t
 * @see com33_mailbox()
 * @see com33_blob()
 * @since 0.1.0
 */
typedef enum : uintptr_t {
  /** @brief Staged input offset: past the mailbox's own cache line. */
  k_com33_epub_offset = 0x100U,
  /** @brief Output blob offset: past the 32 KiB staged input. */
  k_com33_blob_offset = 0x8100U,
  /** @brief Mailbox base -- the start of the board's shared window. */
  k_com33_mailbox_addr = (uintptr_t)k_ra8_board_shared_ram_base,
  /** @brief Staged input base, carved from the shared window. */
  k_com33_epub_addr = (uintptr_t)k_ra8_board_shared_ram_base + (uintptr_t)k_com33_epub_offset,
  /** @brief Output blob base, carved from the shared window. */
  k_com33_blob_addr = (uintptr_t)k_ra8_board_shared_ram_base + (uintptr_t)k_com33_blob_offset,
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
  k_com33_magic     = 0x434F4D33U,         /**< "COM3" -- M85 stamps it when ready.       */
  k_com33_req_magic = 0x52455130U,         /**< "REQ0" -- M85 stamps it once the job (the */
                                           /**< staged .epub + buffers) is ready to run. */
  k_com33_m33_sig           = 0x4D33C0DEU, /**< "M33 CODE" boot sentinel written by M33.   */
  k_com33_status_running    = 0U,          /**< status: M33 is still building the blob.    */
  k_com33_status_ok         = 1U,          /**< status: compile finalized a valid blob.    */
  k_com33_status_build_fail = 2U,          /**< status: a compile stage overflowed/failed. */
  k_com33_status_open_fail  = 3U,          /**< status: epub_open rejected the input.      */
  k_com33_status_no_request = 4U,          /**< status: req_magic absent (no staged job).  */
  k_com33_status_fault      = 5U,          /**< status: M33 took a hardware fault mid-run. */
} com33_const_t;

/**
 * @enum com33_blob_cap_t
 * @brief Capacities, in bytes, of the shared staged-input and output buffers.
 * @details The M85 stages the source `.epub` into ::k_com33_epub_addr (up to
 *          ::k_com33_epub_cap bytes) before posting the job; the M33 lays the
 *          finalized RABOOK1 blob into ::k_com33_blob_addr (the buffer's
 *          `out_cap` = ::k_com33_blob_cap). 32 KiB of input dwarfs the parity
 *          fixture `.epub` (~1 KiB) and 8 KiB of output dwarfs its blob (~700 B),
 *          so neither buffer overflows for this demo.
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_com33_epub_cap = 0x8000U, /**< Shared staged-input .epub buffer capacity (32 KiB). */
  k_com33_blob_cap = 8192U,   /**< Shared output-blob buffer capacity in bytes.        */
} com33_blob_cap_t;

/**
 * @struct com33_mailbox_t
 * @brief Cross-core handoff block backed by a fixed shared-SRAM address.
 *
 * @details All fields are `volatile` so each core re-reads memory rather than
 * caching a value in a register. The M85 owns the REQUEST half (`magic`,
 * `req_magic`, `epub_base/len`, `out_base/cap`); the M33 owns the RESPONSE half
 * (`m33_sig`, `status`, `blob_*`, `chapter_count`, `done`). Each core only reads
 * fields it does not own, and the M85 reads the response only after observing
 * `done == 1` behind a `dmb`.
 *
 * @invariant `magic` is 0 until the M85 stamps ::k_com33_magic, then holds.
 * @invariant `req_magic` is 0 until the M85 has staged the .epub + buffers, then
 *            ::k_com33_req_magic; the M33 does not compile until it observes it.
 * @invariant `done` is 0 until the M33 publishes the blob, then 1 forever.
 * @invariant On `status == ok`, `blob_base == out_base` and
 *            `0 < blob_len <= out_cap`.
 *
 * @par Example:
 * @code
 * volatile com33_mailbox_t* mb = com33_mailbox();
 * while (mb->done == 0U) {}                         // wait for the M33 emitter
 * ra8_err_t rc = book_validate((const void*)mb->blob_base, mb->blob_len);
 * @endcode
 *
 * @see com33_mailbox()
 * @since 0.1.0
 */
typedef struct {
  volatile uint32_t magic;         /**< M85 stamps ::k_com33_magic when ready.         */
  volatile uint32_t req_magic;     /**< M85 stamps ::k_com33_req_magic when the job is */
                                   /**< staged; the M33 spins on it before compiling. */
  volatile uint32_t epub_base;     /**< M85: address of the staged source .epub bytes.   */
  volatile uint32_t epub_len;      /**< M85: length of the staged .epub, bytes.          */
  volatile uint32_t out_base;      /**< M85: address the M33 writes the finalized blob.  */
  volatile uint32_t out_cap;       /**< M85: capacity of the output buffer, bytes.       */
  volatile uint32_t m33_sig;       /**< M33 stamps ::k_com33_m33_sig on boot.            */
  volatile uint32_t status;        /**< Compile outcome (::com33_const_t status codes).  */
  volatile uint32_t blob_base;     /**< Address of the finalized blob (= out_base).      */
  volatile uint32_t blob_len;      /**< Finalized RABOOK1 blob length, bytes.            */
  volatile uint32_t blob_crc;      /**< Blob header body CRC-32, echoed for cross-check. */
  volatile uint32_t chapter_count; /**< Chapters the M33 emitted into the blob.          */
  volatile uint32_t done;          /**< Set to 1 by the M33 once the blob is published.  */
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

/**
 * @brief Typed pointer to the fixed-address shared staged-input .epub buffer.
 *
 * @details The M85 copies the source `.epub` bytes here before posting the job;
 * the M33 reads them via @ref epub_mem_media_t at the same physical SRAM base,
 * so no shared translation unit is needed.
 *
 * @return Pointer to the staged-input buffer at ::k_com33_epub_addr.
 * @retval non-NULL Always; the address is a compile-time constant.
 *
 * @pre Both linker scripts leave the ::k_com33_epub_cap bytes from
 *      ::k_com33_epub_addr unallocated.
 * @pre The M85 data cache is disabled (see file header).
 * @post Returns a valid pointer; never NULL.
 * @post No side effects.
 *
 * @note Callable from either core; the pointer arithmetic is identical.
 * @since 0.1.0
 */
static inline uint8_t* com33_epub(void)
{
  return (uint8_t*)(uintptr_t)k_com33_epub_addr;
}

#ifdef __cplusplus
}
#endif
