/**
 * @file ra8_seam_symbols.h
 * @brief Canonical ELF, linker, bootloader, and RTOS inspection symbol names
 * @ingroup grp_core
 *
 * @par Tag
 * [Ring 1 / Core] {World: ALL}
 *
 * @details
 * Host tools (`ra8_emulator`, trace parsers, soak runners, ELF inspectors) look
 * up firmware entry points, linker-script region bounds, DFU handoff triggers,
 * debug channels, and RTOS/TXM scheduler structures by ELF symbol name. Before
 * this header existed, each host consumer typed its own string literals, so
 * renaming a linker or firmware symbol silently broke host inspection.
 *
 * This Ring-1 contract states every exported seam symbol name once as a pure
 * string-literal constant (`K_RA8_SEAM_SYM_*`). All constants resolve at
 * compile time, introduce no linkage or storage in firmware translation units,
 * and can be ingested directly by C consumers and Zig `@cImport` /
 * `translate-c` pipelines.
 *
 * Naming ThreadX / TXM symbol names as string constants keeps ELF tooling
 * aligned without introducing any RTOS code dependency in Ring 1.
 *
 * @note Pure 7-bit ASCII, host-friendly (`RA8_OFF_TARGET`), and free of any
 *       upward library dependencies.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/* =============================================================================
 * Boot & Linker Script Seam Symbols
 * =============================================================================
 */

/** @brief Primary core reset entry handler symbol in ELF vector table. */
#define K_RA8_SEAM_SYM_RESET_HANDLER "Reset_Handler"

/** @brief Linker-defined top of Secure main stack (`ORIGIN(SRAM) + LENGTH(SRAM)`). */
#define K_RA8_SEAM_SYM_STACK_TOP "g_ra8_ls_stack_top"

/** @brief Linker-defined reserved main stack size in bytes. */
#define K_RA8_SEAM_SYM_STACK_SIZE "g_ra8_ls_stack_size"

/** @brief Linker-defined start of Non-Secure zero-initialized `.ns_bss` section. */
#define K_RA8_SEAM_SYM_NS_BSS_START "g_ra8_ls_ns_bss_start"

/** @brief Linker-defined end of Non-Secure zero-initialized `.ns_bss` section. */
#define K_RA8_SEAM_SYM_NS_BSS_END "g_ra8_ls_ns_bss_end"

/** @brief Linker-defined initial Non-Secure main stack pointer (`MSP_NS`). */
#define K_RA8_SEAM_SYM_NS_STACK_TOP "g_ra8_ls_ns_stack_top"

/** @brief Linker-defined Non-Secure vector-table base (`VTOR_NS`). */
#define K_RA8_SEAM_SYM_NS_RUN_START "g_ra8_ls_ns_run_start"

/** @brief Linker-defined start of Secure Gateway veneer section (`.gnu.sgstubs`). */
#define K_RA8_SEAM_SYM_SGSTUBS_START "g_ra8_ls_sgstubs_start"

/** @brief Linker-defined end of Secure Gateway veneer section (`.gnu.sgstubs`). */
#define K_RA8_SEAM_SYM_SGSTUBS_END "g_ra8_ls_sgstubs_end"

/* =============================================================================
 * Bootloader & Firmware Diagnostic Seam Symbols
 * =============================================================================
 */

/** @brief Warm-reset `.noinit` trigger word polled by DFU bootloader. */
#define K_RA8_SEAM_SYM_DFU_TRIGGER "g_dfu_trigger"

/** @brief Versioned fixed-capacity UI widget inspection channel symbol. */
#define K_RA8_SEAM_SYM_WIDGET_DEBUG_TREE "ra8_widget_debug_tree"

/* =============================================================================
 * ThreadX & ThreadX-Modules ELF Inspection Seam Symbols
 * =============================================================================
 */

/** @brief ThreadX active thread control-block pointer symbol. */
#define K_RA8_SEAM_SYM_TX_THREAD_CURRENT_PTR "_tx_thread_current_ptr"

/** @brief ThreadX circular list head of created thread control blocks. */
#define K_RA8_SEAM_SYM_TX_THREAD_CREATED_PTR "_tx_thread_created_ptr"

/** @brief ThreadX count of created threads in the circular list. */
#define K_RA8_SEAM_SYM_TX_THREAD_CREATED_COUNT "_tx_thread_created_count"

/** @brief ThreadX Modules linked-list head of loaded modules. */
#define K_RA8_SEAM_SYM_TXM_LOADED_LIST_PTR "_txm_module_manager_loaded_list_ptr"

/** @brief ThreadX Modules last memory-protection fault diagnostic record. */
#define K_RA8_SEAM_SYM_TXM_MEMORY_FAULT_INFO "_txm_module_manager_memory_fault_info"

#ifdef __cplusplus
}
#endif
