/**
 * @file sim_elf.h
 * @brief ELF32 image services for the board emulator (load / symbols / vectors)
 *
 * @details
 * The emulator's view of a firmware image: read the file, copy its PT_LOAD
 * segments into Unicorn memory at their LMA, resolve function/object symbols
 * from the .symtab (for the seam installers, `--dump-sym` and `--stop-sym`),
 * and derive the vector-table base of an image (for the TrustZone Non-Secure
 * world switch). The raw ELF32 field offsets live here too so every scanner
 * (MVE / long-shift / div-0 seam installs, the profiler's symbol collection)
 * parses program and section headers with one shared set of named constants.
 *
 * Split out of the board_sim main translation unit; the behaviour is the
 * emulator's original ELF handling, moved verbatim.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 *
 * @since 0.1.0
 */

#pragma once

#include <stdint.h>
#include <unicorn/unicorn.h>

#include "ra8_attributes.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @enum sim_elf_layout_t
 * @brief ELF32 file-layout constants (header sizes and field byte offsets).
 *
 * @details
 * Named offsets into the little-endian ELF32 file header, program headers,
 * section headers, and symbol-table entries, as fixed by the System V ABI.
 * Shared by the segment loader, the symbol resolver, and every image scanner
 * that walks executable PT_LOAD segments (seam installers, profiler).
 *
 * @invariant Values match the ELF32 object-file format; they are file-format
 *            facts, not tunables.
 *
 * @see load_elf()  Walks program headers using these offsets.
 * @see elf_sym_addr()  Walks section headers / .symtab using these offsets.
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_elf_ehdr_size      = 52U,   /**< ELF32 file-header size.            */
  k_elf_em_arm         = 40U,   /**< e_machine == EM_ARM.               */
  k_elf_e_phoff_off    = 28U,   /**< e_phoff in the file header.        */
  k_elf_ph_offset_off  = 4U,    /**< p_offset in a program header.      */
  k_elf_ph_vaddr_off   = 8U,    /**< p_vaddr (VMA) in a program header. */
  k_elf_ph_paddr_off   = 12U,   /**< p_paddr in a program header.       */
  k_elf_ph_filesz_off  = 16U,   /**< p_filesz in a program header.      */
  k_elf_ph_flags_off   = 24U,   /**< p_flags in a program header.       */
  k_elf_pf_x           = 1U,    /**< PF_X: segment is executable.       */
  k_elf_pt_load        = 1U,    /**< p_type == PT_LOAD.                 */
  k_elf_shentsize_min  = 40U,   /**< ELF32 section-header entry size.   */
  k_elf_sh_size_off    = 20U,   /**< sh_size in a section header.       */
  k_elf_sh_link_off    = 24U,   /**< sh_link in a section header.       */
  k_elf_sh_entsize_off = 36U,   /**< sh_entsize in a section header.    */
  k_elf_sym_info_off   = 12U,   /**< st_info in a symbol-table entry.   */
  k_elf_st_type_mask   = 0x0FU, /**< Low nibble of st_info is the type. */
} sim_elf_layout_t;

/**
 * @brief Read a whole file into a fresh heap buffer.
 *
 * @details
 * Opens @p path in binary mode, sizes it via seek/tell, allocates one buffer
 * and reads the full contents. The caller owns the returned buffer and frees
 * it with free(); board_sim keeps the primary firmware image alive for the
 * whole run because warm reboots and the cpu1 engine re-read it.
 *
 * @param[in]  path    File to read (NUL-terminated path).
 * @param[out] out_len Receives the file length in bytes on success.
 *
 * @return Newly-allocated buffer holding the file bytes.
 * @retval nullptr The file could not be opened, sized, or fully read.
 *
 * @pre @p path and @p out_len are non-null.
 * @pre The process may allocate a buffer of the file's size.
 * @post On success, @p out_len holds the byte count and the buffer holds the
 *       complete file contents.
 * @post On failure, no allocation is leaked.
 *
 * @note Not thread-safe against concurrent modification of the file; the
 *       emulator is single-threaded host-side.
 * @see load_elf()  Copies the returned image into emulated memory.
 * @since 0.1.0
 */
RA8_PRIV uint8_t* read_file(const char* path, long* out_len);

/**
 * @brief Load ELF32 PT_LOAD segments into emulated memory at their LMA.
 *
 * @details
 * Validates the ELF magic, class (ELFCLASS32) and machine (EM_ARM), then
 * walks the program headers and writes each non-empty PT_LOAD segment's file
 * bytes to its physical load address (p_paddr) in @p uc memory -- exactly
 * what a flash programmer does with the image. One `loaded ... @ ...` stderr
 * line is emitted per segment. .bss is untouched (the firmware's own
 * Reset_Handler zeroes it), matching real boot.
 *
 * @param[in,out] uc  Unicorn engine whose memory receives the segments.
 * @param[in]     elf In-memory ELF image.
 * @param[in]     len Length of @p elf in bytes.
 *
 * @return 0 on success, -1 on a malformed image or a failed memory write.
 * @retval 0  At least one PT_LOAD segment was written.
 * @retval -1 Not a 32-bit ARM ELF, or a segment write failed, or no segment
 *            was loadable.
 *
 * @pre @p uc has every segment's target region mapped.
 * @pre @p elf points to at least @p len valid bytes.
 * @post On success, every non-empty PT_LOAD segment is resident at its LMA.
 * @post On failure, a diagnostic has been printed to stderr.
 *
 * @note Not thread-safe; call during single-threaded setup or a warm reboot.
 * @see read_file()  Produces the image buffer this consumes.
 * @since 0.1.0
 */
RA8_PRIV int load_elf(uc_engine* uc, const uint8_t* elf, long len);

/**
 * @brief Vector-table base of an ELF: lowest executable PT_LOAD VMA.
 *
 * @details
 * Walks the program headers and returns the lowest p_vaddr among non-empty,
 * executable (PF_X) PT_LOAD segments. On Cortex-M the vector table is linked
 * at the start of the executable text region, so this is the image's vector
 * base: 0x32100000 for the RAM-resident NS layout (a single RWE segment runs
 * from the NS_SRAM2 alias) and 0x90000000 for the OSPI-XIP layout (the R-E
 * segment executes in place from the flash window). The executable filter is
 * essential for XIP: that image also carries a writable .data segment whose
 * VMA is 0x32100000 (its initialisers live in flash, copied to RAM at NS
 * startup) -- lower than the vectors -- so a plain minimum-VMA scan would
 * wrongly pick the RAM alias. board_sim feeds the result to the TrustZone NS
 * vector-base tracking so the BLXNS world switch reads the NS MSP/reset from
 * wherever the loaded image placed them.
 *
 * @param[in] elf Mapped ELF image (little-endian ELF32 ARM).
 * @param[in] len ELF image length in bytes.
 * @return Lowest executable PT_LOAD p_vaddr, or 0 if none / the header is bad.
 * @retval 0 No executable PT_LOAD segment, or a truncated header table.
 *
 * @pre @p elf points to at least @ref k_elf_ehdr_size bytes.
 * @pre The program-header table lies within the first @p len bytes.
 * @post The return value is the VMA of some executable PT_LOAD segment, or 0.
 * @note Not thread-safe; board_sim is single-threaded.
 * @see elf_sym_addr()  The symbol-level companion lookup.
 * @since 0.1.0
 */
RA8_PRIV uint32_t elf_vector_base(const uint8_t* elf, long len);

/**
 * @brief Resolve a function symbol's entry address from the ELF .symtab.
 *
 * @details
 * Walks the ELF32 section headers for the SHT_SYMTAB table and its linked
 * string table, then matches @p name and returns its st_value with the Thumb
 * bit cleared (so it can be used as a UC_HOOK_CODE address). Used by the seam
 * installers to shim first-party firmware APIs and by the `--dump-sym` /
 * `--stop-sym` probes. Returns 0 if the symbol (or a symbol table) is absent.
 *
 * @param[in]  elf      Mapped ELF image.
 * @param[in]  len      ELF image length in bytes.
 * @param[in]  name     NUL-terminated symbol name to find.
 * @param[out] size_out If non-NULL, receives the symbol's st_size (0 if
 *                      absent).
 *
 * @return Even (Thumb-cleared) symbol address, or 0 if not found.
 * @retval 0 Symbol not present, or the image carries no symbol table.
 *
 * @pre @p elf points to at least @p len valid bytes.
 * @pre @p name is NUL-terminated.
 * @post On a hit, @p size_out (when non-NULL) holds the symbol size.
 * @post The image is unmodified (read-only walk).
 *
 * @note Not thread-safe; board_sim is single-threaded.
 * @see elf_vector_base()  The segment-level companion lookup.
 * @since 0.1.0
 */
RA8_PRIV uint32_t elf_sym_addr(const uint8_t* elf, long len, const char* name, uint32_t* size_out);

/**
 * @brief Warm-reboot the firmware: re-run from the reset vector in place.
 *
 * @details
 * Mirrors a Cortex-M reset without tearing down the Unicorn engine. Restores
 * the code image and the .data initial values by re-writing the ELF's
 * PT_LOAD segments (the firmware's own Reset_Handler then re-zeroes .bss and
 * re-copies .data), re-reads SP/PC from the vector table, resets the
 * peripheral models (the reset-cause RSTSRn and the VBATT backup domain
 * deliberately survive their reset hooks, so the reboot cause and
 * battery-backed state persist), and clears the host-side exception /
 * scheduler bookkeeping so the next boot starts clean. The installed Unicorn
 * hooks persist across this, so they are NOT re-added, and one-shot CLI
 * input (--keys / --input / --usb-in) is NOT re-injected.
 *
 * @param[in,out] uc    Unicorn engine to reboot (kept alive).
 * @param[in]     elf   The firmware ELF image bytes (re-loaded).
 * @param[in]     len   ELF image length in bytes.
 * @param[in]     trace Whether --trace is active (forwarded to the models).
 * @return The reset-vector PC to resume the run loop from (Thumb bit set).
 * @retval 0 The image re-load failed; the caller must end the run.
 * @pre The same @p elf / @p len loaded successfully at startup.
 * @pre The engine is idle (between chunks).
 * @post On success the core state matches a fresh boot of the image.
 * @note Not thread-safe; the emulator is single-threaded host-side.
 * @see load_elf()  Performs the segment re-write.
 * @since 0.1.0
 */
RA8_PRIV uint32_t warm_reboot(uc_engine* uc, const uint8_t* elf, long len, bool trace);

/**
 * @struct elf_exec_segment_t
 * @brief One executable PT_LOAD segment, already bounds-checked against the image.
 *
 * @details
 * Handed to an ::elf_exec_segment_fn by @ref elf_foreach_exec_segment so a
 * scanner works from validated offsets instead of re-decoding program-header
 * fields. `bytes` points into the caller's image buffer and stays valid only
 * for the duration of the callback.
 *
 * @invariant `bytes .. bytes + filesz` lies inside the image.
 * @invariant The segment is PT_LOAD with PF_X set and `filesz` is non-zero.
 *
 * @see elf_foreach_exec_segment  Produces these.
 * @since 0.1.0
 */
typedef struct {
  const uint8_t* bytes;  /**< Segment contents inside the image buffer. */
  uint32_t       vaddr;  /**< Segment virtual address (p_vaddr).        */
  uint32_t       filesz; /**< Segment length in the file (p_filesz).    */
} elf_exec_segment_t;

/**
 * @brief Per-segment callback for @ref elf_foreach_exec_segment.
 *
 * @param[in] seg Segment to scan.
 * @param[in] ctx Opaque pointer forwarded from the caller.
 * @return True to keep walking, false to stop early (e.g. a site cap was hit).
 * @since 0.1.0
 */
typedef bool (*elf_exec_segment_fn)(const elf_exec_segment_t* seg, void* ctx);

/**
 * @brief Walk every executable PT_LOAD segment of an ELF32 image.
 *
 * @details
 * Decodes the program-header table once, skips any segment that is not a
 * non-empty executable PT_LOAD or that would run past the end of the image,
 * and invokes @p fn for each survivor. Factored out because the seam
 * installers and the profiler each need exactly this walk; duplicating it left
 * the same twenty statements of bounds-checking in several functions, where a
 * fix to one copy would silently miss the others.
 *
 * @param[in]     elf Base of the mapped ELF image.
 * @param[in]     len Image length in bytes; every read is bounded by it.
 * @param[in]     fn  Callback invoked per executable segment.
 * @param[in,out] ctx Opaque pointer forwarded to @p fn.
 *
 * @return The number of segments handed to @p fn.
 * @retval 0 The image is too short, the header table is truncated, or the
 *           image carries no executable PT_LOAD segment.
 *
 * @pre @p elf and @p fn are non-NULL.
 * @pre @p len is the true length of the @p elf buffer.
 * @post No read touches a byte at or beyond `elf + len`.
 * @post The walk stops early once @p fn returns false.
 *
 * @note Not thread-safe with respect to the image it reads.
 *
 * @see div0_seam_install()  Scans for UDIV/SDIV sites.
 * @see mve_seam_install()   Scans for MVE VSTRW sites.
 * @since 0.1.0
 */
RA8_PRIV uint32_t elf_foreach_exec_segment(const uint8_t*      elf,
                                           long                len,
                                           elf_exec_segment_fn fn,
                                           void*               ctx);

#ifdef __cplusplus
}
#endif
