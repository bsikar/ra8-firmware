/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file port/esp-hosted/src/ra8_esp_hosted_osi_internal.h
 * @brief Library-private surface of the esp-hosted OS-abstraction vtable.
 *
 * @par Tag
 * [Ring 4 / PORT] {World: NS}
 *
 * @details
 * The vtable is assembled from four slices -- RTOS, GPIO, SPI and the
 * rows that belong to no slice -- each of which keeps its implementations
 * `static` and exposes exactly one binder. This header declares the
 * binders that live in ``ra8_esp_hosted_osi.c`` and
 * ``ra8_esp_hosted_osi_absent.c``, plus the two decisions promoted out of
 * `static` so host tests can drive them without a live co-processor.
 *
 * Nothing outside ``port/esp-hosted/`` and ``tests/`` may include this.
 *
 * @since 0.1.0
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_hosted_os_abstraction.h"
#include "ra8_attributes.h"
#include "ra8_err.h"

/**
 * @brief Fill every row of the OS-abstraction vtable.
 *
 * @details
 * Zeroes the table, runs the RTOS, GPIO, SPI and absent-transport
 * binders, installs the rows this file owns, and then verifies that no
 * row was left null. The verification is the point: a slice that gains a
 * row and forgets to bind it becomes a test failure here rather than a
 * null dereference somewhere inside the vendored core.
 *
 * @param[out] out Table to populate. Must be non-null. Entirely
 *                 overwritten, so a partially populated table cannot
 *                 survive a second call.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok Every row is populated.
 * @retval k_ra8_err_null_ptr ``out`` was null.
 * @retval k_ra8_err_invalid_state A row was left null by its binder.
 *
 * @pre ``out`` points at writable storage for one table.
 * @pre The RTOS slice has been initialised, since some rows capture its
 *      state at bind time.
 * @post On success every row is non-null.
 * @post On failure the table is left populated as far as it got, which is
 *       what the caller needs to identify the missing row in a debugger.
 *
 * @note Not thread-safe; run once from the port's bring-up path.
 *
 * @par MC/DC:
 * Promoted from `static` so a test can bind into a local table and assert
 * completeness without initialising the board. Production callers reach it
 * through ``ra8_esp_hosted_port_init``.
 *
 * @par Example:
 * @code
 * hosted_osi_funcs_t table = {};
 * TEST_ASSERT_EQ(k_ra8_ok, ra8_esp_hosted_osi_bind_all(&table));
 * @endcode
 *
 * @see ra8_esp_hosted_osi_is_complete
 * @since 0.1.0
 */
RA8_PRIV
[[nodiscard]] ra8_err_t ra8_esp_hosted_osi_bind_all(hosted_osi_funcs_t* out);

/**
 * @brief Fill the rows belonging to transports this board does not carry.
 *
 * @details
 * The SDIO, half-duplex SPI and UART rows. Each reports the transport as
 * absent and names the row that was called; none pretends to move data.
 * See ``ra8_esp_hosted_osi_absent.c`` for why that is the truthful answer
 * on this board rather than a placeholder.
 *
 * @param[out] out Table whose absent-transport rows are filled. Must be
 *                 non-null. Only those rows are written.
 *
 * @return Nothing.
 *
 * @pre ``out`` points at writable storage for one table.
 * @pre The caller zeroed the table, or accepts that other rows survive.
 * @post Every SDIO, half-duplex SPI and UART row is non-null.
 * @post No other row is modified.
 *
 * @note Not thread-safe; run once from ``ra8_esp_hosted_osi_bind_all``.
 *
 * @par MC/DC:
 * Promoted from `static` so a test can assert the rows are filled without
 * building the whole table. Tests and ``ra8_esp_hosted_osi_bind_all``
 * only.
 *
 * @par Example:
 * @code
 * hosted_osi_funcs_t table = {};
 * ra8_esp_hosted_osi_bind_absent(&table);
 * TEST_ASSERT_NOT_NULL((void*)table._h_sdio_card_init);
 * @endcode
 *
 * @see ra8_esp_hosted_osi_bind_all
 * @since 0.1.0
 */
RA8_PRIV
void ra8_esp_hosted_osi_bind_absent(hosted_osi_funcs_t* out);

/**
 * @brief Report whether every row of a table is populated.
 *
 * @details
 * Walks the table as an array of function pointers rather than naming
 * sixty-odd fields, so a row added to the vendored structure is covered
 * the moment it exists. That is deliberate: a hand-written field list
 * would silently stop covering new rows, which is the exact failure this
 * check exists to prevent.
 *
 * @param[in] table Table to inspect. Must be non-null.
 *
 * @return Whether the table has no null row.
 * @retval true Every row is populated.
 * @retval false ``table`` was null, or at least one row is null.
 *
 * @pre ``table`` points at one fully-sized table.
 * @pre The table's rows are all function pointers of the same width,
 *      which the accompanying static assertion checks.
 * @post No state is modified.
 * @post The answer depends only on the table's contents.
 *
 * @note Reentrant; a bounded read-only scan.
 *
 * @par MC/DC:
 * Promoted from `static` so the null-table guard and the null-row
 * detection can be driven independently. Tests and
 * ``ra8_esp_hosted_osi_bind_all`` only.
 *
 * @par Example:
 * @code
 * TEST_ASSERT(!ra8_esp_hosted_osi_is_complete(&(hosted_osi_funcs_t){}));
 * @endcode
 *
 * @see ra8_esp_hosted_osi_bind_all
 * @since 0.1.0
 */
RA8_PRIV
[[nodiscard]] bool ra8_esp_hosted_osi_is_complete(const hosted_osi_funcs_t* table);

/**
 * @brief Deliver one posted event to the registered application handler.
 *
 * @details
 * The single decision behind both event rows of the vtable, so the
 * generic and Wi-Fi paths cannot drift apart. Reports whether the event
 * was consumed, which is what lets the core distinguish "nobody is
 * listening" from "delivered".
 *
 * @param[in] base Event namespace; a null pointer is delivered as an
 *                 empty string rather than dereferenced by the handler.
 * @param[in] event_id Identifier within the namespace.
 * @param[in] data Payload, or null when ``data_len`` is zero.
 * @param[in] data_len Payload length in bytes.
 *
 * @return Whether the event was consumed.
 * @retval RET_OK The handler ran.
 * @retval RET_FAIL No handler is registered.
 * @retval RET_INVALID The payload pointer and length disagree.
 *
 * @pre A handler has been registered, or the caller tolerates a refusal.
 * @pre The caller holds no lock the handler also takes.
 * @post No port state is modified.
 * @post The handler has run exactly once, or not at all.
 *
 * @note Runs on the posting thread.
 *
 * @par MC/DC:
 * Promoted from `static` so the "no handler" and "payload disagrees"
 * decisions can be driven without a live link. Production callers are the
 * two event rows; tests may call it directly.
 *
 * @par Example:
 * @code
 * TEST_ASSERT_EQ(RET_FAIL, ra8_esp_hosted_osi_dispatch_event("B", 1, nullptr, 0U));
 * @endcode
 *
 * @see ra8_esp_hosted_port_set_event_cb
 * @since 0.1.0
 */
RA8_PRIV
int ra8_esp_hosted_osi_dispatch_event(const char* base,
                                      int32_t     event_id,
                                      const void* data,
                                      size_t      data_len);
