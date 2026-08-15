/**
 * @file emu_memmap.h
 * @brief Explicit sparse backing and Unicorn memory-map bindings
 * @details Owns three unlinked raw descriptors for aliased SRAM, SDRAM, and
 * OSPI state. Caller-owned bindings mirror committed bytes into Unicorn-owned
 * guest pages while keeping the descriptors authoritative.
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
  /** @brief Unicorn and sparse-page granule. */
  k_page_size = 0x1000U,
} ram_region_t;

/** @brief Fixed caller and sparse-backing limits. */
typedef enum : size_t {
  k_emu_memmap_backing_count = 3U,    /**< SRAM, SDRAM, and OSPI descriptors.     */
  k_emu_memmap_binding_count = 2U,    /**< CPU0 and optional CPU1 engines.        */
  k_emu_memmap_scratch_bytes = 4096U, /**< Exact page-transfer scratch.           */
  k_emu_memmap_dirty_bytes   = 4155U, /**< One bit for each logical backing page. */
} emu_memmap_limit_t;

/** @brief Semantic result for backing lifecycle operations. */
typedef enum : uint8_t {
  k_emu_memmap_ok = 0U,  /**< Operation completed exactly.                 */
  k_emu_memmap_invalid,  /**< Pointer, state, or engine was invalid.       */
  k_emu_memmap_capacity, /**< Caller scratch was smaller than required.    */
  k_emu_memmap_io,       /**< Raw-descriptor operation failed.             */
  k_emu_memmap_unicorn,  /**< Unicorn map, hook, or mirror failed.         */
  k_emu_memmap_poisoned, /**< A failed publish made further output unsafe. */
} emu_memmap_status_t;

/** @brief Exact requirements and operation outcome. */
typedef struct {
  emu_memmap_status_t status;                 /**< Semantic completion status.       */
  size_t              required_scratch_bytes; /**< Exact caller scratch requirement. */
  size_t              supplied_scratch_bytes; /**< Caller-provided capacity.         */
  uint64_t            logical_backing_bytes;  /**< Sum of sparse fd logical lengths. */
  int                 os_error;               /**< Captured errno for host I/O.      */
} emu_memmap_result_t;

/** @brief One owned unlinked sparse descriptor. */
typedef struct {
  int      fd;   /**< Owned descriptor, or -1.   */
  uint64_t size; /**< Exact logical byte length. */
} emu_memmap_backing_t;

struct emu_memmap_workspace;

/** @brief One caller-owned association between a Unicorn engine and backing. */
typedef struct {
  struct emu_memmap_workspace* workspace;  /**< Shared authoritative backing. */
  uc_engine*                   uc;         /**< Bound Unicorn engine.         */
  uc_hook                      write_hook; /**< Guest-write mirror hook.      */
  bool                         active;     /**< Binding and hook are live.    */
} emu_memmap_binding_t;

/** @brief Independent sparse backing with at most two engine bindings. */
typedef struct emu_memmap_workspace {
  /** @brief Owned raw file descriptors. */
  emu_memmap_backing_t backings[k_emu_memmap_backing_count];
  /** @brief Active Unicorn engine views. */
  emu_memmap_binding_t bindings[k_emu_memmap_binding_count];
  /** @brief Borrowed page-transfer scratch. */
  uint8_t* scratch;
  /** @brief Exact bound scratch prefix. */
  size_t scratch_bytes;
  /** @brief Committed-page bits. */
  uint8_t dirty[k_emu_memmap_dirty_bytes];
  /** @brief First fatal host error. */
  int os_error;
  /** @brief Whether descriptors are owned. */
  bool open;
  /** @brief Publication failed; outputs must stop. */
  bool poisoned;
  /** @brief Internal recursion guard for mirrors. */
  bool publishing;
  /** @brief Failed guest store needs boundary reconciliation. */
  bool restore_pending;
  /** @brief Backing index for pending reconciliation. */
  size_t restore_backing;
  /** @brief Backing offset for pending reconciliation. */
  uint64_t restore_offset;
  /** @brief Byte count for pending reconciliation. */
  size_t restore_count;
  /** @brief Successful host publications. */
  uint64_t host_writes;
  /** @brief Successful guest publications. */
  uint64_t guest_writes;
} emu_memmap_workspace_t;

/**
 * @brief Return exact scratch and sparse logical-size requirements.
 * @return Successful immutable requirement values.
 * @post Reports 4096 caller bytes and 136134656 logical descriptor bytes.
 * @note Pure and safe to call before any engine or workspace exists.
 * @since 0.1.0
 */
[[nodiscard]] emu_memmap_result_t emu_memmap_requirements(void);

/**
 * @brief Create three independent unlinked sparse backing descriptors.
 * @param[in,out] scratch Caller-owned page scratch.
 * @param[in] supplied_scratch_bytes Writable scratch capacity.
 * @param[out] workspace Receives owned backing on success.
 * @return Exact requirements and completion status.
 * @pre @p workspace is non-null.
 * @pre Non-null @p scratch spans @p supplied_scratch_bytes bytes.
 * @post Capacity failure preserves workspace and every scratch byte.
 * @post Success leaves no named temporary paths.
 * @note Distinct workspaces own independent descriptors and state.
 * @since 0.1.0
 */
[[nodiscard]] emu_memmap_result_t
emu_memmap_open(void* scratch, size_t supplied_scratch_bytes, emu_memmap_workspace_t* workspace);

/**
 * @brief Map one Unicorn engine and bind it to authoritative backing.
 * @param[in,out] workspace Open sparse backing.
 * @param[in,out] uc Fresh Unicorn engine.
 * @return Lifecycle result with exact requirements.
 * @pre @p uc has no guest memory mappings.
 * @post Success mirrors every committed dirty page and installs write hooks.
 * @post Unicorn owns its opaque guest pages; first-party code owns no page
 * buffer.
 * @post A protected metadata page outside every RA8 region associates this
 * engine with @p workspace without a process-global registry.
 * @note Attach fails if a future RA8 map overlaps the metadata page.
 * @since 0.1.0
 */
[[nodiscard]] emu_memmap_result_t emu_memmap_attach(emu_memmap_workspace_t* workspace,
                                                    uc_engine*              uc);

/**
 * @brief Remove one engine binding before closing that Unicorn engine.
 * @param[in,out] workspace Open backing containing the binding.
 * @param[in,out] uc Live bound engine.
 * @return Whether exactly one active binding was removed.
 * @post Success deletes the guest-write hook and releases the binding slot.
 * @note The caller still owns and must close @p uc.
 * @since 0.1.0
  * @details Remove one engine binding before closing that unicorn engine; this step is contained within the emu memmap model and uses bounded caller or module-owned storage.
 * @retval true The emu memmap detach condition holds or completed successfully; false otherwise.
 * @pre Arguments satisfy the ranges documented for emu memmap detach. @pre The call executes on the emulator's single owning thread.
 * @post Ownership of caller-supplied storage is unchanged.
 */
bool emu_memmap_detach(emu_memmap_workspace_t* workspace, uc_engine* uc);

/**
 * @brief Close all sparse descriptors after every engine was detached.
 * @param[in,out] workspace Open or already-closed caller workspace.
 * @return True when closed/already closed; false while a binding is active.
 * @post Success invalidates every owned descriptor and clears borrowed state.
 * @note Failure preserves the workspace so its live engines can be detached.
 * @since 0.1.0
  * @details Close all sparse descriptors after every engine was detached; this step is contained within the emu memmap model and uses bounded caller or module-owned storage.
 * @retval true The emu memmap close condition holds or completed successfully; false otherwise.
 * @pre Arguments satisfy the ranges documented for emu memmap close. @pre The call executes on the emulator's single owning thread.
 * @post Ownership of caller-supplied storage is unchanged.
 */
bool emu_memmap_close(emu_memmap_workspace_t* workspace);

/**
 * @brief Whether a failed publication requires aborting output and execution.
 * @param[in] workspace Run backing, or nullptr as an unsafe state.
 * @return True for null or poisoned state; otherwise false.
 * @post No workspace or descriptor state changes.
 * @since 0.1.0
  * @details Whether a failed publication requires aborting output and execution; this step is contained within the emu memmap model and uses bounded caller or module-owned storage.
 * @retval true The emu memmap is poisoned condition holds or completed successfully; false otherwise.
 * @pre Arguments satisfy the ranges documented for emu memmap is poisoned. @pre The call executes on the emulator's single owning thread.
 * @post Ownership of caller-supplied storage is unchanged.
 * @note The operation is synchronous and does not transfer heap ownership.
 */
bool emu_memmap_is_poisoned(const emu_memmap_workspace_t* workspace);

/**
 * @brief Reconcile a failed guest store from authoritative descriptor bytes.
 * @param[in,out] workspace Open poisoned workspace.
 * @return True when no restore is pending or every live view was restored.
 * @post Success clears only the pending restore; poison remains output-sticky.
 * @post Failure leaves the restore pending and output suppressed.
 * @note Call immediately after Unicorn returns, before further execution.
 * @since 0.1.0
  * @details Reconcile a failed guest store from authoritative descriptor bytes; this step is contained within the emu memmap model and uses bounded caller or module-owned storage.
 * @retval true The emu memmap reconcile condition holds or completed successfully; false otherwise.
 * @pre Arguments satisfy the ranges documented for emu memmap reconcile. @pre The call executes on the emulator's single owning thread.
 */
bool emu_memmap_reconcile(emu_memmap_workspace_t* workspace);

/**
 * @brief Return the static emulated memory-region table.
 * @param[out] count Receives the immutable table row count.
 * @return Process-lifetime immutable region table.
 * @pre @p count is non-null.
 * @post No memory-map or engine state changes.
 * @since 0.1.0
  * @details Return the static emulated memory-region table; this step is contained within the emu memmap model and uses bounded caller or module-owned storage.
 * @retval nullptr No emu memmap regions result is available; otherwise the returned pointer identifies it.
 * @pre The call executes on the emulator's single owning thread.
 * @post Ownership of caller-supplied storage is unchanged.
 * @note The operation is synchronous and does not transfer heap ownership.
 */
const mem_region_t* emu_memmap_regions(uint32_t* count);

/**
 * @brief Return the MRAM boot-vector base.
 * @return Immutable MRAM base address.
 * @post No memory-map or engine state changes.
 * @since 0.1.0
  * @details Return the mram boot-vector base; this step is contained within the emu memmap model and uses bounded caller or module-owned storage.
 * @retval value The operation-specific emu memmap MRAM base value.
 * @pre Arguments satisfy the ranges documented for emu memmap MRAM base. @pre The call executes on the emulator's single owning thread.
 * @post Ownership of caller-supplied storage is unchanged.
 * @note The operation is synchronous and does not transfer heap ownership.
 */
uint64_t emu_memmap_mram_base(void);

#ifdef __cplusplus
}
#endif
