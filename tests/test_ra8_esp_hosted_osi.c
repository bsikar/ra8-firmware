/**
 * @file test_ra8_esp_hosted_osi.c
 * @brief Unit tests for the esp-hosted OS-abstraction vtable and its slots.
 *
 * @par Tag
 * [Test / Host] {World: N/A}
 *
 * @details
 * Drives ``port/esp-hosted/src/ra8_esp_hosted_osi.c`` and
 * ``port/esp-hosted/src/ra8_esp_hosted_osi_absent.c``: the completeness scan,
 * both binders, the event-dispatch decision behind the two event rows, the
 * application-handler registration, the rows that belong to no slice, and all
 * sixteen rows of the transports this board does not carry.
 *
 * @par Why the absent rows are driven through the bound table
 * They are `static`, so a test cannot name them -- and should not, because
 * what the vendored core actually reaches is the function pointer the binder
 * installed. Calling through the table therefore tests the binding and the
 * body in one step: a binder that put ``_h_uart_read`` in the
 * ``_h_uart_write`` slot would pass a body-only test and fail this one.
 *
 * @par The two answers an absent row can give
 * ``RET_INVALID`` means the caller built a malformed request -- it would be
 * wrong on any board. ``RET_FAIL`` means the request was well-formed and this
 * board simply has no such transport. Every row is driven for both, because
 * collapsing them would leave a caller unable to tell its own bug from the
 * hardware, and a test that only checked "it fails" would not notice.
 *
 * @par One row name needs a terminology waiver, bound to a local alias
 * The SDIO interrupt-wait row carries a legacy role word in the name the
 * vendored header fixes, so every line naming it needs a per-line
 * ``LEGACY-OK`` waiver. Spelling it inside a ``TEST_ASSERT_EQ`` does not
 * survive: clang-format wraps the long call and carries the trailing comment
 * onto a different line from the token, after which the waiver no longer
 * applies to the offending line and the terminology gate fails. Binding the
 * row to a short local alias on one line -- with the waiver there -- keeps
 * the two together whatever the formatter does with the assertions.
 *
 * No hardware registers are touched; no ``ra8_sim_mmap`` window is required.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "esp_hosted_os_abstraction.h"
#include "esp_hosted_transport_config.h"
#include "port_esp_hosted_host_os.h"
#include "ra8_err.h"
#include "ra8_esp_hosted_osi_internal.h"
#include "ra8_esp_hosted_pins.h"
#include "ra8_esp_hosted_port.h"
#include "ra8_log.h"
#include "ra8_port_constants.h"
#include "unity_minimal.h"

/**
 * @enum t_osi_const_t
 * @brief Fixture sizes and fixed arguments this translation unit uses.
 *
 * @details
 * Named so the role of each value is readable where it is used -- a payload
 * length is not a register offset, even when both happen to be small.
 *
 * @invariant ::k_t_osi_base_cap exceeds the longest event base the tests post.
 *
 * @par Example:
 * @code
 * uint8_t payload[k_t_osi_payload_bytes] = {};
 * @endcode
 *
 * @see ra8_esp_hosted_osi_dispatch_event
 */
typedef enum : uint32_t {
  k_t_osi_payload_bytes = 4U,    /**< Bytes in the event / transfer fixtures. */
  k_t_osi_base_cap      = 32U,   /**< Capacity of the recorded event base.    */
  k_t_osi_reg           = 0x18U, /**< Arbitrary register offset for a row.    */
  k_t_osi_ticks         = 100U,  /**< Arbitrary tick budget for a wait row.   */
  k_t_osi_lines_single  = 1U,    /**< The one-line half-duplex width.         */
  k_t_osi_lines_dual    = 2U,    /**< The two-line half-duplex width.         */
  k_t_osi_lines_quad    = 4U,    /**< The four-line half-duplex width.        */
  k_t_osi_event_id      = 7U,    /**< Arbitrary event identifier.             */
  k_t_osi_spi_mode      = 3U,    /**< SPI mode the C6 image is built for.     */
  k_t_osi_printf_arg    = 9U,    /**< `%d` argument the log-shim row renders. */
  /** Byte written over a whole vtable so that every row reads as a
      non-null pointer, whatever the row count happens to be. */
  k_t_osi_all_bits = 0xFF,
} t_osi_const_t;

/**
 * @struct t_event_record
 * @brief What the test handler saw the last time it was called.
 *
 * @details
 * Records every argument rather than just a call count, because the point of
 * the dispatch tests is that the payload and namespace arrive intact -- a
 * handler that ran with the wrong base would still bump a counter.
 *
 * @invariant ``calls`` counts invocations since the last reset.
 * @invariant ``base`` is always NUL-terminated, including when the port
 *            substituted the empty string for a null namespace.
 *
 * @par Example:
 * @code
 * TEST_ASSERT_EQ(1, s_event.calls);
 * @endcode
 *
 * @see t_event_cb
 */
typedef struct t_event_record {
  uint32_t    calls;                          /**< Invocations since reset.   */
  void*       ctx;                            /**< Context handed back.       */
  char        base[(size_t)k_t_osi_base_cap]; /**< Namespace as delivered.    */
  int32_t     event_id;                       /**< Identifier as delivered.   */
  const void* data;                           /**< Payload pointer delivered. */
  size_t      data_len;                       /**< Payload length delivered.  */
} t_event_record_t;

/**
 * @var s_event
 * @brief Latest observation made by ::t_event_cb.
 * @details Written only by the test handler; read by the dispatch tests.
 * @note Cleared by ::t_event_reset before each observation.
 * @warning Not thread-safe; the host test driver is single-threaded.
 */
static t_event_record_t s_event;

/**
 * @brief Application event handler the dispatch tests register.
 *
 * @details
 * Copies every argument into ::s_event so the assertions can inspect them
 * after the call returns. The namespace is copied rather than aliased,
 * because the port may hand over a string literal it owns.
 *
 * @param[in] ctx Context the registration supplied.
 * @param[in] base Event namespace; never null by contract.
 * @param[in] event_id Identifier within the namespace.
 * @param[in] data Payload, or null when @p data_len is zero.
 * @param[in] data_len Payload length in bytes.
 *
 * @pre @p base is a non-null NUL-terminated string.
 * @pre ::t_event_reset has run since the last observation.
 * @post ::s_event holds every argument this call received.
 * @post The invocation count has grown by one.
 *
 * @note Not thread-safe; single-threaded host test driver.
 */
static void
t_event_cb(void* ctx, const char* base, int32_t event_id, const void* data, size_t data_len)
{
  s_event.calls++;
  s_event.ctx      = ctx;
  s_event.event_id = event_id;
  s_event.data     = data;
  s_event.data_len = data_len;
  s_event.base[0]  = '\0';
  if (base != nullptr) {
    (void)snprintf(s_event.base, sizeof s_event.base, "%s", base);
  }
}

/**
 * @brief Forget whatever the handler last observed.
 *
 * @details
 * Run before each dispatch so an assertion cannot pass on the residue of an
 * earlier call -- which is exactly how a "handler was not called" assertion
 * would silently become vacuous.
 *
 * @pre No dispatch is in flight.
 * @pre The handler is registered, or the reset is harmless.
 * @post The invocation count is zero.
 * @post The recorded namespace is the empty string.
 *
 * @note Not thread-safe; single-threaded host test driver.
 */
static void t_event_reset(void)
{
  s_event         = (t_event_record_t){};
  s_event.base[0] = '\0';
}

/**
 * @brief Swallow the log bytes the absent rows emit.
 *
 * @details
 * Every absent-transport row reports itself at error level, which is the
 * behaviour under test elsewhere; here it is only noise. Installing a
 * discarding sink also makes the logger's readiness deterministic on the
 * host, where the ITM stimulus port it would otherwise probe does not exist.
 *
 * @param[in] ctx Unused cookie.
 * @param[in] byte Unused log byte.
 *
 * @pre None.
 * @pre The logger has been initialised.
 * @post No state is modified.
 * @post The byte is discarded.
 *
 * @note Not thread-safe; single-threaded host test driver.
 */
static void t_log_sink(void* ctx, uint8_t byte)
{
  (void)ctx;
  (void)byte;
}

/**
 * @brief Drive a context-plus-buffer absent row through its four vectors.
 *
 * @details
 * The UART data rows share one argument shape and one three-condition
 * validation, so they share one driver. Passing the row as a function pointer
 * is the project's documented NASA Rule 9 deviation, and it is what lets both
 * rows be covered without two copies of the same four assertions drifting
 * apart.
 *
 * @param[in] row Row to drive; must be the bound table's entry, not a
 *                hand-named function.
 *
 * @return Nothing.
 *
 * @pre @p row is non-null, which the caller has already asserted.
 * @pre The row belongs to a transport this board does not carry.
 * @post Every assertion in the vector set has been made.
 * @post No board or module state is modified.
 *
 * @note Not thread-safe; single-threaded host test driver.
 */
static void t_drive_ctx_buffer_row(int (*row)(void*, uint8_t*, uint16_t))
{
  uint8_t ctx                                = 0U;
  uint8_t buf[(size_t)k_t_osi_payload_bytes] = {};

  TEST_ASSERT_EQ(RET_FAIL, row(&ctx, buf, (uint16_t)sizeof(buf)));
  TEST_ASSERT_EQ(RET_INVALID, row(nullptr, buf, (uint16_t)sizeof(buf)));
  TEST_ASSERT_EQ(RET_INVALID, row(&ctx, nullptr, (uint16_t)sizeof(buf)));
  TEST_ASSERT_EQ(RET_INVALID, row(&ctx, buf, 0U));
}

/**
 * @brief Drive an SDIO register or block row through its four vectors.
 *
 * @details
 * The same validation as ::t_drive_ctx_buffer_row, behind the wider SDIO
 * signature that also carries a register offset and a bus-lock request --
 * neither of which participates in the decision, which is itself worth
 * pinning: varying them must not change the answer.
 *
 * @param[in] row Row to drive; must be the bound table's entry.
 *
 * @return Nothing.
 *
 * @pre @p row is non-null, which the caller has already asserted.
 * @pre The row belongs to a transport this board does not carry.
 * @post Every assertion in the vector set has been made.
 * @post No board or module state is modified.
 *
 * @note Not thread-safe; single-threaded host test driver.
 */
static void t_drive_sdio_row(int (*row)(void*, uint32_t, uint8_t*, uint16_t, bool))
{
  uint8_t        ctx                                = 0U;
  uint8_t        buf[(size_t)k_t_osi_payload_bytes] = {};
  const uint32_t reg                                = (uint32_t)k_t_osi_reg;

  TEST_ASSERT_EQ(RET_FAIL, row(&ctx, reg, buf, (uint16_t)sizeof(buf), false));
  TEST_ASSERT_EQ(RET_INVALID, row(nullptr, reg, buf, (uint16_t)sizeof(buf), false));
  TEST_ASSERT_EQ(RET_INVALID, row(&ctx, reg, nullptr, (uint16_t)sizeof(buf), false));
  TEST_ASSERT_EQ(RET_INVALID, row(&ctx, reg, buf, 0U, false));

  /* The register offset and the lock request are carried, not consulted. */
  TEST_ASSERT_EQ(RET_FAIL, row(&ctx, 0U, buf, (uint16_t)sizeof(buf), true));
}

/**
 * @test test_is_complete
 *
 * @brief The completeness scan rejects a null table and any null row.
 *
 * @details
 * The scan walks the table as a flat array of function pointers rather than
 * naming sixty-odd fields, so a row added upstream is covered the moment it
 * exists. That is the property this test protects: filling every byte with a
 * non-zero pattern must satisfy it without the test knowing how many rows
 * there are, and clearing any single row must break it.
 *
 * @par MC/DC:
 * Decision A: `if (table == nullptr) { return false; }` (1 condition, 2 vectors)
 * - Vector A1: table=null   -> true;  false is returned before any scan
 * - Vector A2: table=valid  -> false; the scan runs
 *
 * Decision B: `if (view.rows[i] == nullptr) { return false; }` inside the scan
 * (1 condition, 2 vectors)
 * - Vector B1: every row non-null -> false for all rows; true is returned
 * - Vector B2: exactly one row cleared -> true at that row; false is returned
 * Vector B2 is driven twice, clearing an early row and then a late one, so a
 * scan that only inspected a prefix of the table could not pass.
 *
 * @pre None.
 * @post No module state is modified.
 * @note Not thread-safe; single-threaded test context.
 * @since 0.1.0
 */
static void test_is_complete(void)
{
  TEST_BEGIN("osi completeness scan");
  hosted_osi_funcs_t table = {};

  TEST_ASSERT(!ra8_esp_hosted_osi_is_complete(nullptr));
  TEST_ASSERT(!ra8_esp_hosted_osi_is_complete(&table));

  /* Every byte set: no row can be null, whatever the row count is. */
  (void)memset(&table, (int)k_t_osi_all_bits, sizeof table);
  TEST_ASSERT(ra8_esp_hosted_osi_is_complete(&table));

  /* Exactly one early row cleared. */
  table._h_memcpy = nullptr;
  TEST_ASSERT(!ra8_esp_hosted_osi_is_complete(&table));

  /* ...and exactly one late row, so a scan that stopped early could not pass. */
  (void)memset(&table, (int)k_t_osi_all_bits, sizeof table);
  table._h_event_post = nullptr;
  TEST_ASSERT(!ra8_esp_hosted_osi_is_complete(&table));
  TEST_END("osi completeness scan");
}

/**
 * @test test_bind_absent_fills_exactly_its_rows
 *
 * @brief The absent binder fills its sixteen rows and touches nothing else.
 *
 * @details
 * Both halves matter. Filling the sixteen is what keeps the vendored core
 * from dereferencing a null row; touching nothing else is what lets the four
 * binders be called in any order without one clobbering another's work.
 *
 * @par MC/DC:
 * Decision: `if (out == nullptr) { return; }` (1 condition, 2 vectors)
 * - Vector 1: out=null   -> true;  the call is a no-op and does not fault
 * - Vector 2: out=valid  -> false; all sixteen rows are written
 *
 * @pre None.
 * @post No module state is modified.
 * @note Not thread-safe; single-threaded test context.
 * @since 0.1.0
 */
static void test_bind_absent_fills_exactly_its_rows(void)
{
  TEST_BEGIN("osi absent binder");
  hosted_osi_funcs_t table = {};

  /* A null table is refused rather than dereferenced. */
  ra8_esp_hosted_osi_bind_absent(nullptr);

  ra8_esp_hosted_osi_bind_absent(&table);

  int (*const wait_intr)(void*, uint32_t) =
    table._h_sdio_wait_slave_intr; /* LEGACY-OK: vendored vtable field name */

  TEST_ASSERT_NOT_NULL((const void*)table._h_sdio_card_init);
  TEST_ASSERT_NOT_NULL((const void*)table._h_sdio_card_deinit);
  TEST_ASSERT_NOT_NULL((const void*)table._h_sdio_read_reg);
  TEST_ASSERT_NOT_NULL((const void*)table._h_sdio_write_reg);
  TEST_ASSERT_NOT_NULL((const void*)table._h_sdio_read_block);
  TEST_ASSERT_NOT_NULL((const void*)table._h_sdio_write_block);
  TEST_ASSERT_NOT_NULL((const void*)wait_intr);

  TEST_ASSERT_NOT_NULL((const void*)table._h_spi_hd_read_reg);
  TEST_ASSERT_NOT_NULL((const void*)table._h_spi_hd_write_reg);
  TEST_ASSERT_NOT_NULL((const void*)table._h_spi_hd_read_dma);
  TEST_ASSERT_NOT_NULL((const void*)table._h_spi_hd_write_dma);
  TEST_ASSERT_NOT_NULL((const void*)table._h_spi_hd_set_data_lines);
  TEST_ASSERT_NOT_NULL((const void*)table._h_spi_hd_send_cmd9);

  TEST_ASSERT_NOT_NULL((const void*)table._h_uart_read);
  TEST_ASSERT_NOT_NULL((const void*)table._h_uart_write);
  TEST_ASSERT_NOT_NULL((const void*)table._h_uart_flush_input);

  /* Rows belonging to the other three slices are untouched, so the table as
     a whole is still incomplete. */
  TEST_ASSERT_NULL((const void*)table._h_memcpy);
  TEST_ASSERT_NULL((const void*)table._h_malloc);
  TEST_ASSERT_NULL((const void*)table._h_thread_create);
  TEST_ASSERT_NULL((const void*)table._h_read_gpio);
  TEST_ASSERT_NULL((const void*)table._h_do_bus_transfer);
  TEST_ASSERT_NULL((const void*)table._h_event_post);
  TEST_ASSERT(!ra8_esp_hosted_osi_is_complete(&table));
  TEST_END("osi absent binder");
}

/**
 * @test test_absent_sdio_rows
 *
 * @brief Every SDIO row separates a malformed request from an absent bus.
 *
 * @details
 * There is no SDIO controller wired to the co-processor, so none of these
 * rows can move a byte -- but a caller that passed a null buffer has made a
 * different mistake, and gets a different answer. Both are asserted for all
 * seven rows.
 *
 * @par MC/DC:
 * Decision A: `if ((ctx == nullptr) || (data == nullptr) || (size == 0U))`
 * in each of the four register / block rows -- one copy per row, in
 * `port/esp-hosted/src/ra8_esp_hosted_osi_absent.c@internal_sdio_read_reg`,
 * `port/esp-hosted/src/ra8_esp_hosted_osi_absent.c@internal_sdio_write_reg`,
 * `port/esp-hosted/src/ra8_esp_hosted_osi_absent.c@internal_sdio_read_block`
 * and
 * `port/esp-hosted/src/ra8_esp_hosted_osi_absent.c@internal_sdio_write_block`
 * (3 conditions, 4 vectors each)
 * - Vector A1: ctx=valid, data=valid, size=4 -> false, false, false -> false
 *   (control: the request is well-formed, so the absence is reported)
 * - Vector A2: ctx=null,  data=valid, size=4 -> true                 -> true
 *   (varies the context only)
 * - Vector A3: ctx=valid, data=null,  size=4 -> false, true          -> true
 *   (varies the buffer only)
 * - Vector A4: ctx=valid, data=valid, size=0 -> false, false, true   -> true
 *   (varies the length only)
 * A1 pairs with each of A2, A3 and A4 to prove that condition independently
 * affects the outcome. N+1 = 4 vectors for N=3: minimal MC/DC, driven four
 * times over -- once per row -- through ::t_drive_sdio_row.
 *
 * Decision B: `if (ctx == nullptr)` in the card init, card deinit and
 * interrupt-wait rows (1 condition, 2 vectors each)
 * - Vector B1: ctx=valid -> false; the absence is reported
 * - Vector B2: ctx=null  -> true;  the request is refused as malformed
 *
 * @pre The table has been filled by the absent binder.
 * @post No board or module state is modified.
 * @note Not thread-safe; single-threaded test context.
 * @since 0.1.0
 */
static void test_absent_sdio_rows(void)
{
  TEST_BEGIN("osi absent sdio rows");
  hosted_osi_funcs_t table = {};
  uint8_t            ctx   = 0U;

  ra8_esp_hosted_osi_bind_absent(&table);

  int (*const wait_intr)(void*, uint32_t) =
    table._h_sdio_wait_slave_intr; /* LEGACY-OK: vendored vtable field name */

  TEST_ASSERT_EQ(RET_FAIL, table._h_sdio_card_init(&ctx, true));
  TEST_ASSERT_EQ(RET_FAIL, table._h_sdio_card_init(&ctx, false));
  TEST_ASSERT_EQ(RET_INVALID, table._h_sdio_card_init(nullptr, true));

  TEST_ASSERT_EQ(RET_FAIL, table._h_sdio_card_deinit(&ctx));
  TEST_ASSERT_EQ(RET_INVALID, table._h_sdio_card_deinit(nullptr));

  t_drive_sdio_row(table._h_sdio_read_reg);
  t_drive_sdio_row(table._h_sdio_write_reg);
  t_drive_sdio_row(table._h_sdio_read_block);
  t_drive_sdio_row(table._h_sdio_write_block);

  TEST_ASSERT_EQ(RET_FAIL, wait_intr(&ctx, (uint32_t)k_t_osi_ticks));
  TEST_ASSERT_EQ(RET_INVALID, wait_intr(nullptr, (uint32_t)k_t_osi_ticks));
  /* A zero tick budget is a legal request, not a malformed one; the row must
     not block on it either way. */
  TEST_ASSERT_EQ(RET_FAIL, wait_intr(&ctx, 0U));
  TEST_END("osi absent sdio rows");
}

/**
 * @test test_absent_spi_hd_rows
 *
 * @brief Every half-duplex SPI row reports the transport as absent.
 *
 * @details
 * Half-duplex SPI is a different transport from the full-duplex link this
 * board uses, not a mode of it: it needs its own register protocol and a
 * co-processor image built to serve it, and the C6 image here is built for
 * the full-duplex one. The command row takes no arguments at all, so it has
 * nothing to validate and reports the absence directly -- which is asserted
 * rather than skipped, because a row that returned success would let the core
 * believe it had finished a read it never started.
 *
 * @par MC/DC:
 * The three decisions below are one per row family of
 * ``ra8_esp_hosted_osi_absent.c``: the burst rows carry Decision B, in
 * `port/esp-hosted/src/ra8_esp_hosted_osi_absent.c@internal_spi_hd_read_dma`
 * and
 * `port/esp-hosted/src/ra8_esp_hosted_osi_absent.c@internal_spi_hd_write_dma`,
 * and the width row carries Decision C, in
 * `port/esp-hosted/src/ra8_esp_hosted_osi_absent.c@internal_spi_hd_set_data_lines`.
 * Decision A: `if (data == nullptr)` in the two register rows
 * (1 condition, 2 vectors each)
 * - Vector A1: data=valid -> false; the absence is reported
 * - Vector A2: data=null  -> true;  the request is refused as malformed
 *
 * Decision B: `if ((data == nullptr) || (size == 0U))` in the two burst rows
 * (2 conditions, 3 vectors each)
 * - Vector B1: data=valid, size=4 -> false, false -> false (control)
 * - Vector B2: data=null,  size=4 -> true         -> true  (varies the buffer)
 * - Vector B3: data=valid, size=0 -> false, true  -> true  (varies the length)
 * B1+B2 prove the buffer condition independently affects the outcome; B1+B3
 * prove the same for the length. N+1 = 3 vectors for N=2: minimal MC/DC.
 *
 * Decision C: the legal-width test
 * `if ((data_lines != 1) && (data_lines != 2) && (data_lines != 4))`
 * (3 conditions, 4 vectors). The transport defines exactly three widths, so
 * anything else is a malformed request rather than an unsupported one.
 * - Vector C1: data_lines=0 -> true, true, true -> true (control: every
 *   comparison holds, so the width is rejected as malformed)
 * - Vector C2: data_lines=1 -> false            -> false (varies the
 *   one-line comparison; the absence is reported instead)
 * - Vector C3: data_lines=2 -> true, false      -> false (varies the
 *   two-line comparison)
 * - Vector C4: data_lines=4 -> true, true, false -> false (varies the
 *   four-line comparison)
 * C1 pairs with each of C2, C3 and C4 to prove that comparison independently
 * affects the outcome, since in a conjunction the only way to isolate one
 * term is to hold every other term true. N+1 = 4 vectors for N=3: minimal
 * MC/DC.
 *
 * @pre The table has been filled by the absent binder.
 * @post No board or module state is modified.
 * @note Not thread-safe; single-threaded test context.
 * @since 0.1.0
 */
static void test_absent_spi_hd_rows(void)
{
  TEST_BEGIN("osi absent half-duplex spi rows");
  hosted_osi_funcs_t table                              = {};
  uint32_t           word                               = 0U;
  uint8_t            buf[(size_t)k_t_osi_payload_bytes] = {};
  const uint32_t     reg                                = (uint32_t)k_t_osi_reg;

  ra8_esp_hosted_osi_bind_absent(&table);

  TEST_ASSERT_EQ(RET_FAIL, table._h_spi_hd_read_reg(reg, &word, 0, false));
  TEST_ASSERT_EQ(RET_INVALID, table._h_spi_hd_read_reg(reg, nullptr, 0, false));
  TEST_ASSERT_EQ(RET_FAIL, table._h_spi_hd_read_reg(reg, &word, 1, true));

  TEST_ASSERT_EQ(RET_FAIL, table._h_spi_hd_write_reg(reg, &word, false));
  TEST_ASSERT_EQ(RET_INVALID, table._h_spi_hd_write_reg(reg, nullptr, false));

  TEST_ASSERT_EQ(RET_FAIL, table._h_spi_hd_read_dma(buf, (uint16_t)sizeof(buf), false));
  TEST_ASSERT_EQ(RET_INVALID, table._h_spi_hd_read_dma(nullptr, (uint16_t)sizeof(buf), false));
  TEST_ASSERT_EQ(RET_INVALID, table._h_spi_hd_read_dma(buf, 0U, false));

  TEST_ASSERT_EQ(RET_FAIL, table._h_spi_hd_write_dma(buf, (uint16_t)sizeof(buf), true));
  TEST_ASSERT_EQ(RET_INVALID, table._h_spi_hd_write_dma(nullptr, (uint16_t)sizeof(buf), true));
  TEST_ASSERT_EQ(RET_INVALID, table._h_spi_hd_write_dma(buf, 0U, true));

  TEST_ASSERT_EQ(RET_INVALID, table._h_spi_hd_set_data_lines(0U));
  TEST_ASSERT_EQ(RET_FAIL, table._h_spi_hd_set_data_lines((uint32_t)k_t_osi_lines_single));
  TEST_ASSERT_EQ(RET_FAIL, table._h_spi_hd_set_data_lines((uint32_t)k_t_osi_lines_dual));
  TEST_ASSERT_EQ(RET_FAIL, table._h_spi_hd_set_data_lines((uint32_t)k_t_osi_lines_quad));

  TEST_ASSERT_EQ(RET_FAIL, table._h_spi_hd_send_cmd9());
  TEST_END("osi absent half-duplex spi rows");
}

/**
 * @test test_absent_uart_rows
 *
 * @brief Every UART row reports that no serial link to the C6 exists.
 *
 * @details
 * The Pmod1 connector does have a UART position, but this board selects the
 * SPI one and the co-processor image serves no UART transport in any case.
 * Reporting success from the write row would be the worst possible answer:
 * the core would believe a frame had reached the wire and would wait for a
 * reply that can never arrive.
 *
 * @par MC/DC:
 * Decision A: `if ((ctx == nullptr) || (data == nullptr) || (size == 0U))` in
 * the read and write rows,
 * `port/esp-hosted/src/ra8_esp_hosted_osi_absent.c@internal_uart_read` and
 * `port/esp-hosted/src/ra8_esp_hosted_osi_absent.c@internal_uart_write`
 * (3 conditions, 4 vectors each)
 * - Vector A1: ctx=valid, data=valid, size=4 -> false, false, false -> false
 *   (control: the absence is reported)
 * - Vector A2: ctx=null,  data=valid, size=4 -> true                 -> true
 * - Vector A3: ctx=valid, data=null,  size=4 -> false, true          -> true
 * - Vector A4: ctx=valid, data=valid, size=0 -> false, false, true   -> true
 * A1 pairs with each of A2, A3, A4 to prove independent influence. N+1 = 4
 * vectors for N=3: minimal MC/DC, driven through ::t_drive_ctx_buffer_row.
 *
 * Decision B: `if (ctx == nullptr)` in the flush row (1 condition, 2 vectors)
 * - Vector B1: ctx=valid -> false; the absence is reported
 * - Vector B2: ctx=null  -> true;  the request is refused as malformed
 *
 * @pre The table has been filled by the absent binder.
 * @post No board or module state is modified.
 * @note Not thread-safe; single-threaded test context.
 * @since 0.1.0
 */
static void test_absent_uart_rows(void)
{
  TEST_BEGIN("osi absent uart rows");
  hosted_osi_funcs_t table = {};
  uint8_t            ctx   = 0U;

  ra8_esp_hosted_osi_bind_absent(&table);

  t_drive_ctx_buffer_row(table._h_uart_read);
  t_drive_ctx_buffer_row(table._h_uart_write);

  TEST_ASSERT_EQ(RET_FAIL, table._h_uart_flush_input(&ctx));
  TEST_ASSERT_EQ(RET_INVALID, table._h_uart_flush_input(nullptr));
  TEST_END("osi absent uart rows");
}

/**
 * @test test_set_event_cb
 *
 * @brief Registration refuses a context with no handler to hand it to.
 *
 * @details
 * Handing over a context without a handler is always a caller mistake: the
 * context would be stored and never used, and the caller would believe it had
 * registered. Removing a handler (both arguments null) is a legitimate
 * operation and is accepted.
 *
 * @par MC/DC:
 * Decision: `if ((cb == nullptr) && (ctx != nullptr))` in
 * `port/esp-hosted/src/ra8_esp_hosted_osi.c@ra8_esp_hosted_port_set_event_cb`
 * (2 conditions, 3 vectors)
 * - Vector 1: cb=valid, ctx=valid -> false        -> false (control: accepted)
 * - Vector 2: cb=null,  ctx=valid -> true, true   -> true  (rejected)
 * - Vector 3: cb=null,  ctx=null  -> true, false  -> false (accepted; the
 *   handler is removed)
 * Vectors 1+2 prove the handler condition independently affects the outcome
 * (the context condition is true in both); vectors 2+3 prove the same for the
 * context condition (the handler condition is true in both). N+1 = 3 vectors
 * for N=2: minimal MC/DC.
 *
 * @pre No dispatch is in flight.
 * @post No handler is registered when this test returns.
 * @note Not thread-safe; single-threaded test context.
 * @since 0.1.0
 */
static void test_set_event_cb(void)
{
  TEST_BEGIN("osi event handler registration");
  uint8_t app_state = 0U;

  TEST_ASSERT_EQ(k_ra8_ok, ra8_esp_hosted_port_set_event_cb(t_event_cb, &app_state));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_esp_hosted_port_set_event_cb(nullptr, &app_state));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_esp_hosted_port_set_event_cb(nullptr, nullptr));

  /* The rejected call must not have taken effect: with the handler removed by
     the third call, a post is reported unconsumed. */
  TEST_ASSERT_EQ(RET_FAIL, ra8_esp_hosted_osi_dispatch_event("B", 1, nullptr, 0U));

  /* A handler with no context is also legitimate. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_esp_hosted_port_set_event_cb(t_event_cb, nullptr));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_esp_hosted_port_set_event_cb(nullptr, nullptr));
  TEST_END("osi event handler registration");
}

/**
 * @test test_dispatch_event
 *
 * @brief Dispatch distinguishes no handler, a bad payload and a delivery.
 *
 * @details
 * The three answers are what let the vendored core tell "nobody is listening"
 * from "delivered", which it uses to decide whether a dropped event matters.
 * The null-namespace substitution is asserted because the handler's contract
 * promises a non-null string, and a handler that trusted that promise would
 * fault on a null.
 *
 * @par MC/DC:
 * All three decisions below belong to
 * `port/esp-hosted/src/ra8_esp_hosted_osi.c@ra8_esp_hosted_osi_dispatch_event`
 * and are taken in the order it evaluates them.
 * Decision A: `if (s_ra8_esp_hosted_event_cb == nullptr) { return RET_FAIL; }`
 * (1 condition, 2 vectors)
 * - Vector A1: no handler registered -> true;  RET_FAIL, nothing runs
 * - Vector A2: handler registered    -> false; dispatch continues
 *
 * Decision B: `if ((data == nullptr) && (data_len != 0U)) { return RET_INVALID; }`
 * (2 conditions, 3 vectors)
 * - Vector B1: data=valid, len=4 -> false        -> false (control: delivered)
 * - Vector B2: data=null,  len=4 -> true, true   -> true  (refused; the
 *   pointer and length disagree)
 * - Vector B3: data=null,  len=0 -> true, false  -> false (delivered; an
 *   empty payload is legitimate)
 * Vectors B1+B2 prove the pointer condition independently affects the outcome
 * (the length condition is true in both); vectors B2+B3 prove the same for
 * the length condition (the pointer condition is true in both). N+1 = 3
 * vectors for N=2: minimal MC/DC.
 *
 * Decision C: `(base == nullptr) ? "" : base` (1 condition, 2 vectors)
 * - Vector C1: base="TEST_EVENT" -> false; the namespace arrives intact
 * - Vector C2: base=null         -> true;  the handler receives ""
 *
 * @pre No handler is registered on entry.
 * @post No handler is registered when this test returns.
 * @note Not thread-safe; single-threaded test context.
 * @since 0.1.0
 */
static void test_dispatch_event(void)
{
  TEST_BEGIN("osi event dispatch");
  uint8_t       app_state                              = 0U;
  const uint8_t payload[(size_t)k_t_osi_payload_bytes] = {1U, 2U, 3U, 4U};
  const int32_t event_id                               = (int32_t)k_t_osi_event_id;

  /* No handler: reported unconsumed rather than dropped quietly. */
  t_event_reset();
  TEST_ASSERT_EQ(
    RET_FAIL,
    ra8_esp_hosted_osi_dispatch_event("TEST_EVENT", event_id, payload, sizeof payload));
  TEST_ASSERT_EQ(0, s_event.calls);

  TEST_ASSERT_EQ(k_ra8_ok, ra8_esp_hosted_port_set_event_cb(t_event_cb, &app_state));

  /* A consistent payload is delivered whole. */
  t_event_reset();
  TEST_ASSERT_EQ(
    RET_OK,
    ra8_esp_hosted_osi_dispatch_event("TEST_EVENT", event_id, payload, sizeof payload));
  TEST_ASSERT_EQ(1, s_event.calls);
  TEST_ASSERT(strcmp("TEST_EVENT", s_event.base) == 0);
  TEST_ASSERT_EQ(event_id, s_event.event_id);
  TEST_ASSERT(s_event.data == (const void*)payload);
  TEST_ASSERT_EQ(sizeof payload, s_event.data_len);
  TEST_ASSERT(s_event.ctx == (void*)&app_state);

  /* A null pointer with a non-zero length is a contradiction, and is refused
     without the handler ever seeing it. */
  t_event_reset();
  TEST_ASSERT_EQ(
    RET_INVALID,
    ra8_esp_hosted_osi_dispatch_event("TEST_EVENT", event_id, nullptr, sizeof payload));
  TEST_ASSERT_EQ(0, s_event.calls);

  /* A null pointer with a zero length is an empty payload, which is fine. */
  t_event_reset();
  TEST_ASSERT_EQ(RET_OK, ra8_esp_hosted_osi_dispatch_event("TEST_EVENT", event_id, nullptr, 0U));
  TEST_ASSERT_EQ(1, s_event.calls);
  TEST_ASSERT_NULL(s_event.data);
  TEST_ASSERT_EQ(0, s_event.data_len);

  /* A null namespace reaches the handler as an empty string, never as null. */
  t_event_reset();
  TEST_ASSERT_EQ(RET_OK, ra8_esp_hosted_osi_dispatch_event(nullptr, event_id, nullptr, 0U));
  TEST_ASSERT_EQ(1, s_event.calls);
  TEST_ASSERT(strcmp("", s_event.base) == 0);

  TEST_ASSERT_EQ(k_ra8_ok, ra8_esp_hosted_port_set_event_cb(nullptr, nullptr));
  TEST_END("osi event dispatch");
}

/**
 * @test test_bind_all_and_non_slice_rows
 *
 * @brief The full binder leaves no null row, and its own rows behave.
 *
 * @details
 * ``ra8_esp_hosted_osi_bind_all`` is the function the port's bring-up path
 * calls, and its contract is that a slice which gains a row and forgets to
 * bind it becomes a failure here rather than a null dereference inside the
 * vendored core. The rows the file owns itself -- the two event posts, the
 * log shim, the init hook, the restart hook and the two power-save hooks --
 * are then driven through the bound table.
 *
 * The restart and power-save rows deliberately decline. Resetting the board
 * on the transport's authority would cost a reader its page for a link that
 * carries connectivity only, and host power save is disabled in this build,
 * so a row that claimed to have configured a wake source would be configuring
 * one that could never fire.
 *
 * @par MC/DC:
 * Decision A: `RA8_CHECK_NULL_PTR(out, ...)` in the binder (1 condition, 2 vectors)
 * - Vector A1: out=null  -> true;  the null-pointer error is returned
 * - Vector A2: out=valid -> false; binding proceeds
 *
 * Decision B: `if (!ra8_esp_hosted_osi_is_complete(out))` (1 condition, 2 vectors)
 * - Vector B1: every slice bound its rows -> false; success is returned
 * - Vector B2: the true arm is not reachable from a test without breaking a
 *   sibling binder, which no test may do. The equivalent scan failure is
 *   driven directly in ::test_is_complete, where a table with one null row is
 *   fed to the same function.
 *
 * Decision C: the event rows' shared dispatch decision -- both the generic and
 * the Wi-Fi row are driven with no handler registered and then with one,
 * proving they share one decision rather than each carrying a copy that could
 * drift.
 *
 * @pre No handler is registered on entry.
 * @post No handler is registered when this test returns.
 * @note Not thread-safe; single-threaded test context.
 * @since 0.1.0
 */
static void test_bind_all_and_non_slice_rows(void)
{
  TEST_BEGIN("osi full binder and non-slice rows");
  hosted_osi_funcs_t table                                  = {};
  uint8_t            app_state                              = 0U;
  uint8_t            payload[(size_t)k_t_osi_payload_bytes] = {};

  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_esp_hosted_osi_bind_all(nullptr));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_esp_hosted_osi_bind_all(&table));
  TEST_ASSERT(ra8_esp_hosted_osi_is_complete(&table));

  /* With no handler registered, both event rows report the post unconsumed. */
  TEST_ASSERT_EQ(RET_FAIL, table._h_event_post("TEST_EVENT", 1, payload, sizeof payload, 0U));
  TEST_ASSERT_EQ(RET_FAIL, table._h_event_wifi_post(1, payload, sizeof payload, 0U));

  /* With one registered, the Wi-Fi row supplies its own fixed namespace. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_esp_hosted_port_set_event_cb(t_event_cb, &app_state));
  t_event_reset();
  TEST_ASSERT_EQ(RET_OK, table._h_event_wifi_post(2, payload, sizeof payload, 0U));
  TEST_ASSERT_EQ(1, s_event.calls);
  TEST_ASSERT(strcmp("WIFI_EVENT", s_event.base) == 0);

  t_event_reset();
  TEST_ASSERT_EQ(RET_OK, table._h_event_post("OTHER", 3, payload, sizeof payload, 0U));
  TEST_ASSERT(strcmp("OTHER", s_event.base) == 0);
  TEST_ASSERT_EQ(3, s_event.event_id);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_esp_hosted_port_set_event_cb(nullptr, nullptr));

  /* The log shim formats and filters exactly as the direct path does. */
  table._h_printf(1, "H_t", "row %d", (int)k_t_osi_printf_arg);

  /* The init hook runs before the port is initialised, which it reports
     rather than proceeding on. */
  table._h_hosted_init_hook();

  /* Policy refusals, asserted so a change of mind has to be deliberate. */
  TEST_ASSERT_EQ(RET_FAIL, table._h_restart_host());
  TEST_ASSERT_EQ(RET_FAIL, table._h_config_host_power_save_hal_impl(1U, nullptr, 2U, 1));
  TEST_ASSERT_EQ(RET_FAIL, table._h_start_host_power_save_hal_impl(1U));
  TEST_END("osi full binder and non-slice rows");
}

/**
 * @test test_default_spi_config
 *
 * @brief The published SPI configuration is derived from the port's pin map.
 *
 * @details
 * The vendored transport reads its pins from this structure while the port
 * drives them from ``ra8_esp_hosted_pins.h``. If the two ever disagreed, the
 * driver would wait on a handshake nobody was toggling -- a failure that
 * looks like dead hardware. The assertions therefore compare the published
 * configuration against the pin map rather than against literals, so the two
 * cannot drift.
 *
 * @par MC/DC:
 * The function under test is branch-free: it copies fields and returns. There
 * is no decision to cover, so this test asserts the derivation instead, which
 * is the property that can actually break.
 *
 * @pre None.
 * @post No module state is modified.
 * @note Not thread-safe; single-threaded test context.
 * @since 0.1.0
 */
static void test_default_spi_config(void)
{
  TEST_BEGIN("osi default spi configuration");
  const struct esp_hosted_spi_config cfg = esp_hosted_get_default_spi_config();

  TEST_ASSERT_EQ(RA8_PIN_PIN(k_ra8_esp_hosted_pin_chip_select), cfg.pin_cs.pin);
  TEST_ASSERT_EQ(RA8_PIN_PIN(k_ra8_esp_hosted_pin_copi), cfg.pin_mosi.pin);
  TEST_ASSERT_EQ(RA8_PIN_PIN(k_ra8_esp_hosted_pin_cipo), cfg.pin_miso.pin);
  TEST_ASSERT_EQ(RA8_PIN_PIN(k_ra8_esp_hosted_pin_sck), cfg.pin_sclk.pin);

  /* All four bus signals live on one port, which is what makes them a single
     Pmod position rather than four unrelated nets. */
  TEST_ASSERT(cfg.pin_cs.port == cfg.pin_sclk.port);
  TEST_ASSERT(cfg.pin_mosi.port == cfg.pin_miso.port);

  /* The co-processor image is built for SPI mode 3; a mismatch here would
     sample every bit on the wrong clock edge. */
  TEST_ASSERT_EQ(k_t_osi_spi_mode, cfg.mode);
  TEST_ASSERT(cfg.clk_mhz > 0U);
  TEST_ASSERT(cfg.tx_queue_size > 0U);
  TEST_ASSERT_EQ(cfg.tx_queue_size, cfg.rx_queue_size);
  TEST_END("osi default spi configuration");
}

int32_t main(void)
{
  ra8_log_init();
  ra8_log_set_byte_sink(t_log_sink, nullptr);

  test_is_complete();
  test_bind_absent_fills_exactly_its_rows();
  test_absent_sdio_rows();
  test_absent_spi_hd_rows();
  test_absent_uart_rows();
  test_set_event_cb();
  test_dispatch_event();
  test_bind_all_and_non_slice_rows();
  test_default_spi_config();

  ra8_log_set_byte_sink(nullptr, nullptr);
  (void)fprintf(stderr, "[OK  ] test_ra8_esp_hosted_osi.c\n");
  return 0;
}
