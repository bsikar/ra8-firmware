/**
 * @file emu_memmap.h
 * @brief Shared aliased-memory backing and Unicorn memory-map bindings
 * @details Owns three page-aligned, lazily-committed host mappings for the
 * aliased SRAM, SDRAM and OSPI apertures. Every attached Unicorn engine binds
 * each aperture AND its TrustZone bit[28] alias onto those same host pages, so
 * the Secure view, the Non-secure view and the second CPU engine are one state
 * by construction rather than by a per-store reconciliation protocol.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#pragma once

#include <stddef.h>
#include <stdint.h>
#include <unicorn/unicorn.h>

#include "emu_memory_access.h"
#include "ra8_attributes.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief One emulated memory-region table row. */
typedef struct {
  const char* name; /**< Human-readable region name. */
  uint64_t    base; /**< Guest base address.         */
  uint64_t    size; /**< Region byte length.         */
} mem_region_t;

/** @brief Address constants shared with framebuffer and alias validation. */
typedef enum : uint32_t {
  /** @brief Data TCM start. */
  k_dtcm_base = 0x20000000U,
  /** @brief Data TCM end. */
  k_dtcm_end = 0x20010000U,
  /** @brief On-chip SRAM start. */
  k_sram_base = 0x22000000U,
  /** @brief CPU0 SRAM end used by framebuffer checks. */
  k_sram_end = 0x22100000U,
  /** @brief External SDRAM start. */
  k_sdram_base = 0x68000000U,
  /** @brief External SDRAM end. */
  k_sdram_end = 0x6C000000U,
  /** @brief Non-secure SDRAM alias. */
  k_ns_sdram_base = 0x78000000U,
  /** @brief Unicorn and host mapping granule. */
  k_page_size = 0x1000U,
} ram_region_t;

/** @brief Fixed backing and binding limits. */
typedef enum : size_t {
  k_emu_memmap_backing_count = 3U, /**< SRAM, SDRAM, and OSPI host mappings. */
  k_emu_memmap_binding_count = 2U, /**< CPU0 and optional CPU1 engines.      */
} emu_memmap_limit_t;

/** @brief Semantic result for backing lifecycle operations. */
typedef enum : uint8_t {
  k_emu_memmap_ok = 0U, /**< Operation completed exactly.               */
  k_emu_memmap_invalid, /**< Pointer, state, or engine was invalid.     */
  k_emu_memmap_backing, /**< A host aperture mapping could not be made. */
  k_emu_memmap_unicorn, /**< Unicorn map or window install failed.      */
} emu_memmap_status_t;

/** @brief Exact requirements and operation outcome. */
typedef struct {
  emu_memmap_status_t status;                /**< Semantic completion status.        */
  uint64_t            logical_backing_bytes; /**< Sum of the three aperture lengths. */
  int                 os_error;              /**< Captured errno for host mapping.   */
} emu_memmap_result_t;

/** @brief One owned host aperture mapping. */
typedef struct {
  uint8_t* host; /**< Page-aligned host mapping, or nullptr. */
  uint64_t size; /**< Exact logical byte length.             */
} emu_memmap_backing_t;

/** @brief One caller-owned association between a Unicorn engine and backing. */
typedef struct {
  uc_engine* uc;     /**< Bound Unicorn engine.                          */
  bool       active; /**< Binding occupies this slot and must be undone. */
} emu_memmap_binding_t;

/** @brief Independent aperture backing with at most two engine bindings. */
typedef struct emu_memmap_workspace {
  /** @brief Owned host aperture mappings. */
  emu_memmap_backing_t backings[k_emu_memmap_backing_count];
  /** @brief Active Unicorn engine views. */
  emu_memmap_binding_t bindings[k_emu_memmap_binding_count];
  /** @brief First fatal host error. */
  int os_error;
  /** @brief Whether the host mappings are owned. */
  bool open;
} emu_memmap_workspace_t;

/**
 * @brief Report the immutable aliased-aperture backing geometry.
 * @details Names the exact number of logical guest bytes the three shared
 * apertures span, before any workspace or engine exists, so a caller can
 * report the emulated memory footprint without opening anything.
 * @return Successful immutable requirement values.
 * @retval value A result whose status is always ::k_emu_memmap_ok.
 * @pre No precondition: the reported geometry is a compile-time constant.
 * @pre The call executes on the emulator's single owning thread.
 * @post Reports 136134656 logical aperture bytes.
 * @post No workspace, mapping, or engine state changes.
 * @note Pure and safe to call before any engine or workspace exists.
 * @since 0.1.0
 */
[[nodiscard]] emu_memmap_result_t emu_memmap_requirements(void);

/**
 * @brief Create three independent, lazily-committed host aperture mappings.
 * @details Acquires one page-aligned zero-filled host mapping per shared
 * aperture (SRAM, SDRAM, OSPI). The mappings are anonymous, so the 130 MiB of
 * logical guest memory costs resident memory only for the pages the firmware
 * actually touches. Distinct workspaces own distinct mappings and never share
 * bytes.
 * @param[out] workspace Receives the owned mappings on success.
 * @return Exact geometry and completion status.
 * @retval k_emu_memmap_ok Every aperture mapping was acquired.
 * @retval k_emu_memmap_invalid @p workspace was null.
 * @retval k_emu_memmap_backing A host mapping failed; @c os_error holds errno.
 * @pre @p workspace is non-null.
 * @pre @p workspace is not already open.
 * @post Success leaves every aperture zero-filled and no page resident.
 * @post Failure acquires nothing and leaves @p workspace untouched.
 * @note Distinct workspaces own independent mappings and state.
 * @since 0.1.0
 */
[[nodiscard]] emu_memmap_result_t emu_memmap_open(emu_memmap_workspace_t* workspace);

/**
 * @brief Map one Unicorn engine onto the shared aperture backing.
 * @details Maps every RA8D2 region into @p uc. The six aliased windows -- the
 * SRAM/SRAM2, SDRAM and OSPI Secure views and their IDAU bit[28] Non-secure
 * aliases -- are bound with @c uc_mem_map_ptr onto the workspace's host pages,
 * so a store through any of them is immediately visible through every other
 * one and in every other engine bound to the same workspace, at no per-store
 * cost. Every other region is an ordinary private Unicorn mapping.
 * @param[in,out] workspace Open aperture backing.
 * @param[in,out] uc Fresh Unicorn engine.
 * @return Lifecycle result with the aperture geometry.
 * @retval k_emu_memmap_ok The engine is mapped and the binding recorded.
 * @retval k_emu_memmap_invalid A null or unopened argument was supplied.
 * @retval k_emu_memmap_unicorn A region map or the MMIO window install failed.
 * @pre @p uc has no guest memory mappings.
 * @pre @p workspace is open and has a free binding slot.
 * @post Success makes the shared apertures coherent with every bound engine.
 * @post Success records a binding that ::emu_memmap_close refuses to outlive.
 * @note The caller still owns @p uc and must detach before closing it.
 * @since 0.1.0
 */
[[nodiscard]] emu_memmap_result_t emu_memmap_attach(emu_memmap_workspace_t* workspace,
                                                    uc_engine*              uc);

/**
 * @brief Remove one engine binding before closing that Unicorn engine.
 * @details Releases the binding slot @p uc occupies. The engine keeps its
 * mappings until the caller closes it, so detach must happen before
 * ::emu_memmap_close unmaps the host pages those mappings point at.
 * @param[in,out] workspace Open backing containing the binding.
 * @param[in,out] uc Live bound engine.
 * @return Whether exactly one active binding was removed.
 * @retval true The binding for @p uc was found and released.
 * @retval false A null argument, or @p uc was not bound.
 * @pre @p workspace and @p uc are non-null.
 * @pre The call executes on the emulator's single owning thread.
 * @post Success frees the binding slot for a later attach.
 * @post Ownership of caller-supplied storage is unchanged.
 * @note The caller still owns and must close @p uc.
 * @since 0.1.0
 */
bool emu_memmap_detach(emu_memmap_workspace_t* workspace, uc_engine* uc);

/**
 * @brief Release every host aperture mapping after all engines detached.
 * @details Refuses while any binding is active, because a bound engine holds
 * @c uc_mem_map_ptr references into the very pages this call unmaps.
 * @param[in,out] workspace Open or already-closed caller workspace.
 * @return True when closed or already closed; false while a binding is live.
 * @retval true Every aperture mapping was released, or none was owned.
 * @retval false A binding is still active and nothing was released.
 * @pre @p workspace is non-null.
 * @pre Every engine attached to @p workspace was detached first.
 * @post Success clears every backing pointer and the open flag.
 * @post Failure preserves the workspace so its engines can be detached.
 * @note Calling this while an engine is still bound would strand that engine
 * on unmapped host memory, so the refusal is load-bearing, not defensive.
 * @since 0.1.0
 */
bool emu_memmap_close(emu_memmap_workspace_t* workspace);

/**
 * @brief Return the static emulated memory-region table.
 * @details Exposes the immutable RA8D2 region table so other engine code
 * (framebuffer scanning, the cpu1 bring-up) can reason about the same map the
 * attach path installs.
 * @param[out] count Receives the immutable table row count.
 * @return Process-lifetime immutable region table.
 * @retval nullptr Never returned; the table has static storage duration.
 * @pre @p count is non-null.
 * @pre The call executes on the emulator's single owning thread.
 * @post No memory-map or engine state changes.
 * @post @p count holds the exact row count.
 * @note The operation is synchronous and does not transfer heap ownership.
 * @since 0.1.0
 */
const mem_region_t* emu_memmap_regions(uint32_t* count);

/**
 * @brief Return the MRAM boot-vector base.
 * @details The reset vector and the vector table live at the base of the MRAM
 * code region; the run loop and exception model resolve VTOR from here.
 * @return Immutable MRAM base address.
 * @retval value The MRAM region base recorded in the region table.
 * @pre No precondition: the value is a compile-time constant.
 * @pre The call executes on the emulator's single owning thread.
 * @post No memory-map or engine state changes.
 * @post Ownership of caller-supplied storage is unchanged.
 * @note The operation is synchronous and does not transfer heap ownership.
 * @since 0.1.0
 */
uint64_t emu_memmap_mram_base(void);

#ifdef __cplusplus
}
#endif
