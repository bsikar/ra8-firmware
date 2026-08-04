/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file ra8_crashlog_internal.h
 * @brief Cross-TU surface for ra8_crashlog (host-test white-box access only).
 * @ingroup grp_core
 *
 * @details
 * Not part of the public API. On the host unit-test build
 * (`RA8_OFF_TARGET`) the `.noinit` record is an ordinary zero-init
 * static rather than a warm-reset-surviving SRAM region, so the tests
 * cannot reproduce a cold-boot "random SRAM" or a corrupted checksum by
 * resetting the board. Instead these helpers expose the raw record so a
 * test can plant a specific `magic` / `crc` byte pattern and drive the
 * two-condition validation decision (the magic sentinel plus the payload
 * CRC) through both of its conditions independently -- the MC/DC obligation
 * for `ra8_crashlog_is_valid`. See CLAUDE.md "Test access to internal
 * symbols (MC/DC scope)".
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#ifdef RA8_OFF_TARGET

#include "ra8_attributes.h"
#include "ra8_crashlog.h"

/**
 * @brief Raw pointer to the one `.noinit` record (host tests only).
 *
 * @details
 * Promoted access to the TU-private record instance so a white-box test
 * can corrupt `magic` or `crc` in isolation and assert that
 * ra8_crashlog_peek() rejects the record. Never linked into a firmware
 * build (guarded by `RA8_OFF_TARGET`).
 *
 * @par MC/DC:
 * Backs the vectors for `libs/ra8_core/src/ra8_crashlog.c@ra8_crashlog_is_valid`:
 * planting `magic`/`crc` values here lets a test hold one condition fixed
 * while varying the other, proving each independently affects the outcome.
 *
 * @return Address of the module's `volatile` record instance (never `nullptr`).
 * @retval non-null Always: the record is a statically-allocated object.
 *
 * @pre Host unit-test build (`RA8_OFF_TARGET`).
 * @pre The caller is a test under `tests/`.
 * @post No state is modified by the accessor itself.
 * @post The returned pointer stays valid for the process lifetime.
 *
 * @note Not thread-safe; single-threaded test context only.
 * @since 0.1.0
 */
RA8_TEST_HELPER volatile ra8_crashlog_record_t* ra8_crashlog_test_record(void);

/**
 * @brief Zero the record to model a blank / fresh-SRAM start (host tests).
 *
 * @details
 * Resets every byte of the record to zero so each test case begins from a
 * deterministic "no prior crash" state (the process-global static would
 * otherwise carry state between test functions). A zeroed record has
 * `magic == 0`, so ra8_crashlog_peek() reports empty.
 *
 * @return Nothing.
 *
 * @pre Host unit-test build (`RA8_OFF_TARGET`).
 * @pre The caller is a test under `tests/`.
 * @post The record reads as all-zero; ra8_crashlog_peek() returns `false`.
 * @post `boot_loops` reads back zero.
 *
 * @note Not thread-safe; single-threaded test context only.
 * @since 0.1.0
 */
RA8_TEST_HELPER void ra8_crashlog_test_wipe(void);

#endif /* RA8_OFF_TARGET */

#ifdef __cplusplus
}
#endif
