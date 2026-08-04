/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file ra8_crashlog.h
 * @brief Cross-reset crash-log: persist the last fault + a reset-loop guard.
 * @ingroup grp_core
 *
 * @details
 * `ra8_exception.c` decodes every CPU fault / NMI into the fixed-SRAM
 * ::ra8_exception_last_t snapshot `g_ra8_exception_last` and then halts.
 * That snapshot lives in ordinary `.bss`-adjacent SRAM, so the very next
 * reset's `Reset_Handler` zero-fills it and the post-mortem is gone
 * before a field technician can read it. This module adds the missing
 * survival + guard layer ON TOP of that decode path:
 *
 *  1. A `.noinit` record (::ra8_crashlog_record_t) that the reset handler
 *     does NOT zero. It is pinned at the very top of SRAM, above the main
 *     stack, so it survives a warm / watchdog / pin reset (any reset that
 *     keeps SRAM powered) and never shifts the `.data` / `.bss` layout the
 *     TrustZone fixed windows depend on. The linker reservation lives in
 *     `libs/ra8_board_ek_ra8d2/ld/linker_script.ld` (`.noinit`).
 *
 *  2. Integrity: a 32-bit sentinel `magic` plus a CRC-32 over the payload.
 *     A cold power-on reset randomises SRAM; the sentinel and CRC then
 *     fail validation and the log reads as empty -- the intended
 *     fail-safe (there is no false post-mortem from garbage SRAM).
 *
 *  3. A monotonic `boot_loops` counter incremented on every recorded
 *     fault. If it climbs past ::k_ra8_crashlog_loop_threshold without the
 *     application reaching a known-good checkpoint and calling
 *     ra8_crashlog_claim(), ra8_crashlog_safe_mode_requested() latches true
 *     so the app can boot a minimal safe mode instead of the code that
 *     keeps crashing.
 *
 * ## Lifecycle
 *
 * @code
 * // Early in boot, before any risky bring-up:
 * ra8_crashlog_install();                 // arm the fault-persist hook
 * ra8_crashlog_record_t rec;
 * if (ra8_crashlog_peek(&rec)) {
 *   log_prior_crash(&rec);               // exc_number, pc, boot_loops
 *   if (ra8_crashlog_safe_mode_requested()) {
 *     ra8_crashlog_claim();               // reset the guard, boot safe mode
 *     enter_safe_mode();
 *   }
 * }
 * run_risky_bringup();                    // a fault here bumps boot_loops
 * ra8_crashlog_claim();                    // reached known-good: reset guard
 * @endcode
 *
 * ## What survives which reset class
 *
 * | Reset class                         | SRAM kept | Record survives |
 * |-------------------------------------|-----------|-----------------|
 * | Watchdog / IWDT underflow (warm)    | yes       | yes             |
 * | Software reset (SYSRESETREQ)        | yes       | yes             |
 * | Pin / debugger reset (no power cut) | yes       | yes             |
 * | Power-on reset / LVD brown-out      | no        | no (fail-safe)  |
 *
 * VBATT-backed / MRAM persistence across power loss is out of scope here
 * (VBATT `ra8_bkup` is silicon-blocked -- see issue #131) and is the named
 * follow-up for surviving a cold power cycle.
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "ra8_exception.h"

/**
 * @enum ra8_crashlog_magic_t
 * @brief Sentinel marking a fully-written ::ra8_crashlog_record_t.
 *
 * @details
 * Written LAST when a record is complete and cleared FIRST when it is
 * consumed, so neither a secondary fault mid-write nor random cold-boot
 * SRAM can be mistaken for a valid post-mortem. The value is distinct
 * from ::k_ra8_exc_magic_valid so the two fixed-SRAM records are never
 * confused by a debugger.
 *
 * @see ra8_crashlog_record_t
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_ra8_crashlog_magic_valid = 0x5AFEB007UL, /**< "SAFE-BOOT": record is valid. */
} ra8_crashlog_magic_t;

/**
 * @enum ra8_crashlog_limits_t
 * @brief Reset-loop threshold and the linker reservation size.
 *
 * @details
 * ::k_ra8_crashlog_loop_threshold is the number of recorded faults that
 * may accumulate before ra8_crashlog_safe_mode_requested() latches; the
 * app resets the count by reaching a known-good state and calling
 * ra8_crashlog_claim(). ::k_ra8_crashlog_reserve_bytes MUST equal the
 * `LENGTH(NOINIT)` region size in
 * `libs/ra8_board_ek_ra8d2/ld/linker_script.ld`; a compile-time assertion
 * in ra8_crashlog.c proves the record fits it.
 *
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_ra8_crashlog_loop_threshold = 3U,   /**< Safe mode when boot_loops exceeds this.   */
  k_ra8_crashlog_reserve_bytes  = 256U, /**< Bytes reserved for `.noinit` at SRAM top. */
} ra8_crashlog_limits_t;

/**
 * @struct ra8_crashlog_record_t
 * @brief The `.noinit` cross-reset post-mortem record.
 *
 * @details
 * Byte layout is deliberate: `magic` is the standalone integrity sentinel
 * and is NOT covered by `crc`; `crc` is a CRC-32 over every byte from
 * `boot_loops` to the end of the record, so the sentinel and the checksum
 * are two independent integrity signals. A record is trustworthy only
 * when `magic == k_ra8_crashlog_magic_valid` AND the recomputed CRC equals
 * `crc`.
 *
 * @invariant `magic == k_ra8_crashlog_magic_valid` only while `crc` matches
 *            the payload and `boot_loops`/`fault` are fully written.
 * @invariant `boot_loops >= 1` whenever `magic` is valid (a recorded fault
 *            always counts at least itself).
 *
 * @code
 * // Debugger / post-mortem usage:
 * //   (gdb) print/x g_ra8_ls_noinit_start
 * //   (gdb) print *(ra8_crashlog_record_t*)<that address>
 * // magic 0x5AFEB007 => trust boot_loops + fault.
 * @endcode
 *
 * @see ra8_crashlog_peek()
 * @see ra8_exception_last_t
 * @since 0.1.0
 */
typedef struct {
  uint32_t             magic;      /**< ::k_ra8_crashlog_magic_valid if valid (not CRC-covered). */
  uint32_t             crc;        /**< CRC-32 over `boot_loops`..end of record.                 */
  uint32_t             boot_loops; /**< Monotonic recorded-fault counter; cleared by a claim.    */
  ra8_exception_last_t fault;      /**< Embedded copy of the decoded fault / NMI snapshot.       */
} ra8_crashlog_record_t;

/**
 * @brief Arm the fault-persist hook so decoded faults are logged to `.noinit`.
 *
 * @details
 * Registers ra8_crashlog_record_fault() as the exception module's
 * post-decode persist sink (see ra8_exception_set_persist_hook()). After
 * this call, every fault/NMI that reaches ra8_exception_report() copies its
 * decoded snapshot into the cross-reset record and bumps the reset-loop
 * counter BEFORE the CPU halts. Must be called early each boot: the hook
 * pointer lives in `.bss` and is zeroed by every reset, so it has to be
 * re-armed before any code that might fault.
 *
 * @return Nothing.
 *
 * @pre Called from single-threaded boot context (before interrupts that
 *      could fault are enabled).
 * @pre The `.noinit` record is in writable, powered SRAM.
 * @post ra8_exception_report() will persist subsequent faults.
 * @post The existing record (from a prior boot) is left untouched for a
 *       following ra8_crashlog_peek().
 *
 * @note Not thread-safe; single-threaded boot use only.
 * @see ra8_crashlog_peek()
 * @see ra8_crashlog_record_fault()
 * @since 0.1.0
 */
void ra8_crashlog_install(void);

/**
 * @brief Persist a decoded fault snapshot into the cross-reset record.
 *
 * @details
 * The write path installed by ra8_crashlog_install(): reads the prior
 * `boot_loops` (only if the existing record still validates), invalidates
 * the record (`magic = 0`), copies @p decoded into the embedded snapshot,
 * increments the loop counter (saturating so it can never wrap back below
 * the threshold), stamps the CRC, and finally re-arms `magic` LAST so a
 * secondary fault mid-write leaves the record invalid rather than
 * half-written. Also directly callable by an application to record a
 * software-detected fatal condition (assertion, corruption, watchdog
 * arming) in the same field-readable format.
 *
 * @param[in] decoded Decoded fault snapshot to persist. Must not be
 *                    `nullptr`; typically `&g_ra8_exception_last`.
 *
 * @return Nothing.
 *
 * @pre @p decoded points at a populated ::ra8_exception_last_t.
 * @pre The `.noinit` record is in writable SRAM.
 * @post On non-`nullptr` input the record validates and `boot_loops` grew
 *       by one (or held at its saturation ceiling).
 * @post `magic == k_ra8_crashlog_magic_valid` once the write completes.
 *
 * @note Not thread-safe; single fault/boot context by construction.
 * @see ra8_crashlog_install()
 * @see ra8_crashlog_peek()
 * @since 0.1.0
 */
void ra8_crashlog_record_fault(const volatile ra8_exception_last_t* decoded);

/**
 * @brief Validate and copy out the last cross-reset record (non-destructive).
 *
 * @details
 * Recomputes the payload CRC and compares it plus the `magic` sentinel.
 * On a match, copies the whole record into @p out and returns `true`; the
 * stored record is left intact (call ra8_crashlog_claim() to consume it).
 * On any mismatch -- including the all-random SRAM of a cold power-on --
 * returns `false` and leaves @p out unmodified.
 *
 * @param[out] out Destination for the validated record. Must not be
 *                 `nullptr`. Untouched when the function returns `false`.
 *
 * @return Whether a valid record was found.
 * @retval true  A valid record existed and was copied to @p out.
 * @retval false No valid record (empty, corrupted, or `out == nullptr`).
 *
 * @pre @p out is either `nullptr` or points to writable storage.
 * @pre The `.noinit` record is in readable SRAM.
 * @post On `true`, `*out` holds the validated record; the store is intact.
 * @post On `false`, no caller state is modified.
 *
 * @note Not thread-safe.
 * @see ra8_crashlog_claim()
 * @see ra8_crashlog_safe_mode_requested()
 * @since 0.1.0
 */
bool ra8_crashlog_peek(ra8_crashlog_record_t* out);

/**
 * @brief Consume the record and reset the reset-loop guard (clean claim).
 *
 * @details
 * Marks the current record handled: clears `magic` (so a following
 * ra8_crashlog_peek() reports empty) and zeroes `boot_loops` (so the
 * reset-loop guard re-arms from a clean slate). Call it only after the
 * application has reached a known-good checkpoint -- claiming before the
 * risky work would defeat the loop guard. Idempotent.
 *
 * @return Nothing.
 *
 * @pre The `.noinit` record is in writable SRAM.
 * @pre The application has decided the prior record is handled.
 * @post A subsequent ra8_crashlog_peek() returns `false`.
 * @post `boot_loops` reads back zero; the guard is re-armed.
 *
 * @note Not thread-safe.
 * @see ra8_crashlog_peek()
 * @since 0.1.0
 */
void ra8_crashlog_claim(void);

/**
 * @brief Whether accumulated crashes crossed the reset-loop threshold.
 *
 * @details
 * Returns `true` only when a valid record exists AND its `boot_loops`
 * exceeds ::k_ra8_crashlog_loop_threshold -- i.e. the device has recorded
 * more than the allowed number of faults without a clean
 * ra8_crashlog_claim() in between. The application reads this early in boot
 * to divert into a minimal safe mode instead of re-running the code that
 * keeps faulting.
 *
 * @return Whether safe mode is requested.
 * @retval true  Valid record and `boot_loops > k_ra8_crashlog_loop_threshold`.
 * @retval false No valid record, or the count is within the threshold.
 *
 * @pre The `.noinit` record is in readable SRAM.
 * @pre Called from boot context to gate risky bring-up.
 * @post No state is modified (query only).
 * @post The record, if any, is left intact for ra8_crashlog_peek().
 *
 * @note Not thread-safe.
 * @see ra8_crashlog_claim()
 * @since 0.1.0
 */
bool ra8_crashlog_safe_mode_requested(void);

#ifdef __cplusplus
}
#endif
