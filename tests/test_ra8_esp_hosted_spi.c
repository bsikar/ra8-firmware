/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file test_ra8_esp_hosted_spi.c
 * @brief Unit tests for the esp-hosted full-duplex SPI transport slots
 *
 * @details
 * Host-only tests for ``port/esp-hosted/src/ra8_esp_hosted_spi.c``. The
 * transfer path is driven through the slice's two dependency-injection seams:
 * a recording ``ra8_io_spi_bus_iface`` stands in for the SCI Simple-SPI
 * backend, and a recording ``ra8_pin_interface_t`` stands in for the chip
 * select. Both append to one shared event log, which is what lets the tests
 * assert the *order* of "chip select low, frame, chip select high" rather
 * than merely that each happened.
 *
 * There is no mock of ``ra8_io_spi_bus_iface`` in ``tests/mocks/``; the
 * vtable is defined in ``libs/ra8_io/src/ra8_io_spi_bus_internal.h``, whose
 * own comment sanctions tests including it, so the mock is a local static
 * vtable in this translation unit.
 *
 * What we cover:
 *   - every rejection path of ``_h_do_bus_transfer``: null context, null
 *     transmit buffer, null receive buffer, zero length, a length past one
 *     frame, and no bound bus;
 *   - the success path, asserting the chip-select ordering and that the
 *     backend saw the caller's length and both buffers;
 *   - a backend error propagating as a failure with the chip select still
 *     released;
 *   - the open/close state machine and its argument rejections;
 *   - ``_h_bus_init`` before an open, and ``_h_bus_deinit`` with a foreign
 *     handle.
 */

#include <stdint.h>

#include "esp_hosted_os_abstraction.h"
#include "port_esp_hosted_host_os.h"
#include "port_esp_hosted_host_spi.h"
#include "ra8_board_ek_ra8d2_connectors.h"
#include "ra8_err.h"
#include "ra8_esp_hosted_pins.h"
#include "ra8_esp_hosted_spi_internal.h"
#include "ra8_fake_mmap.h"
#include "ra8_io_spi_bus.h"
#include "ra8_io_spi_bus_internal.h"
#include "ra8_pin_interface.h"
#include "ra8_pin_validator.h"
#include "ra8_port_constants.h"
#include "ra8_spi.h"
#include "transport_drv.h"
#include "unity_minimal.h"

/**
 * @enum spi_test_const_t
 * @brief Fixtures and bounds the SPI transport test cases need.
 */
typedef enum : uint32_t {
  k_spi_test_events_max  = 8U,         /**< Rows in the shared event log.     */
  k_spi_test_frame_bytes = 1600U,      /**< One esp-hosted transaction.       */
  k_spi_test_oversize    = 1601U,      /**< One byte past a legal frame.      */
  k_spi_test_short_len   = 12U,        /**< A legal, shorter transmit size.   */
  k_spi_test_pclk_hz     = 100000000U, /**< A plausible live PCLKA rate.      */
  k_spi_test_sck_hz      = 5000000U,   /**< The port's default bit rate.      */
  k_spi_test_bad_channel = 9U,         /**< Legal SCI index, wrong connector. */
  k_spi_test_tx_stamp    = 0xA5U,      /**< Byte pattern the test transmits.  */
  k_spi_test_rx_stamp    = 0x5AU,      /**< Byte pattern the backend replies. */
  k_spi_test_alien       = 0xDEADU,    /**< A handle the slice never issued.  */
} spi_test_const_t;

/**
 * @enum spi_test_event_t
 * @brief The things the two mocks record, in the order they happen.
 */
typedef enum : uint8_t {
  k_spi_test_event_none    = 0U, /**< Unused log row.                     */
  k_spi_test_event_cs_low  = 1U, /**< Chip select driven low (asserted).  */
  k_spi_test_event_cs_high = 2U, /**< Chip select driven high (released). */
  k_spi_test_event_xfer    = 3U, /**< A full-duplex frame was clocked.    */
  k_spi_test_event_cs_init = 4U, /**< Chip select configured as output.   */
} spi_test_event_t;

/** @brief Ordered log of chip-select transitions and bus transfers. */
static uint8_t s_events[k_spi_test_events_max];
/** @brief Number of rows used in ::s_events. */
static uint32_t s_event_count;
/** @brief Frame length the recording backend was last handed. */
static uint32_t s_seen_len;
/** @brief Frame width the recording backend was last handed. */
static uint8_t s_seen_width;
/** @brief Error the recording backend returns from its next transfer. */
static ra8_err_t s_bus_result;
/** @brief Transmit buffer the recording backend was last handed. */
static const void* s_seen_tx;
/** @brief Transmit and receive frames the tests hand to the transfer slot. */
static uint8_t s_tx_frame[k_spi_test_frame_bytes];
/** @brief Receive frame the recording backend fills. */
static uint8_t s_rx_frame[k_spi_test_frame_bytes];

/** @brief Append one event to the shared ordered log. */
static void log_event(spi_test_event_t event)
{
  if (s_event_count < (uint32_t)k_spi_test_events_max) {
    s_events[s_event_count] = (uint8_t)event;
    s_event_count++;
  }
}

/** @brief Recording ``output_init`` row of the mock chip-select driver. */
static ra8_err_t mock_output_init(void* ctx, ra8_port_pin_t pin, ra8_level_t init_level)
{
  (void)ctx;
  (void)pin;
  (void)init_level;
  log_event(k_spi_test_event_cs_init);
  return k_ra8_ok;
}

/** @brief Recording ``write`` row of the mock chip-select driver. */
static ra8_err_t mock_write(void* ctx, ra8_port_pin_t pin, ra8_level_t level)
{
  (void)ctx;
  (void)pin;
  log_event((level == k_ra8_level_low) ? k_spi_test_event_cs_low : k_spi_test_event_cs_high);
  return k_ra8_ok;
}

/** @brief Unused ``read`` row; the transport never reads the chip select. */
static ra8_err_t mock_read(void* ctx, ra8_port_pin_t pin, ra8_level_t* out_level)
{
  (void)ctx;
  (void)pin;
  if (out_level != nullptr) {
    *out_level = k_ra8_level_high;
  }
  return k_ra8_ok;
}

/** @brief Unused ``toggle`` row; the transport never toggles the chip select. */
static ra8_err_t mock_toggle(void* ctx, ra8_port_pin_t pin)
{
  (void)ctx;
  (void)pin;
  return k_ra8_ok;
}

/** @brief The recording chip-select driver injected in place of the HAL's. */
static const ra8_pin_interface_t s_mock_pin_if = {
  .output_init = mock_output_init,
  .write       = mock_write,
  .read        = mock_read,
  .toggle      = mock_toggle,
  .ctx         = nullptr,
};

/** @brief Recording ``xfer8`` row; the transport only uses bulk transfers. */
static ra8_err_t mock_xfer8(void* ctx, uint8_t tx, uint8_t* rx)
{
  (void)ctx;
  (void)tx;
  if (rx != nullptr) {
    *rx = 0U;
  }
  return k_ra8_err_not_supported;
}

/** @brief Recording ``write_read`` row: the whole point of the mock backend. */
static ra8_err_t
mock_write_read(void* ctx, const void* tx, void* rx, uint32_t len, ra8_spi_bit_width_t width)
{
  (void)ctx;
  log_event(k_spi_test_event_xfer);
  s_seen_len   = len;
  s_seen_width = (uint8_t)width;
  s_seen_tx    = tx;
  if ((rx != nullptr) && (len > 0U)) {
    uint8_t* dst = (uint8_t*)rx;
    for (uint32_t i = 0U; i < len; i++) {
      dst[i] = (uint8_t)k_spi_test_rx_stamp;
    }
  }
  return s_bus_result;
}

/** @brief Recording ``set_clock`` row; unused by the transfer path. */
static ra8_err_t mock_set_clock(void* ctx, uint32_t baud_hz, uint32_t pclk_hz)
{
  (void)ctx;
  (void)baud_hz;
  (void)pclk_hz;
  return k_ra8_ok;
}

/** @brief The recording backend vtable standing in for SCI Simple-SPI. */
static const ra8_io_spi_bus_iface_t s_mock_iface = {
  .xfer8      = mock_xfer8,
  .write_read = mock_write_read,
  .set_clock  = mock_set_clock,
};

/** @brief The bus handle the tests inject through the transport's seam. */
static const ra8_io_spi_bus_t s_mock_bus = {
  .iface = &s_mock_iface,
  .ctx   = nullptr,
};

/** @brief Return the vtable with the three transport rows bound. */
static hosted_osi_funcs_t bound_vtable(void)
{
  hosted_osi_funcs_t funcs = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_esp_hosted_spi_bind(&funcs));
  return funcs;
}

/** @brief Build a transfer context over the module's frame buffers. */
static struct hosted_transport_context_t make_ctx(uint32_t len)
{
  struct hosted_transport_context_t ctx = {};
  ctx.tx_buf                            = s_tx_frame;
  ctx.tx_buf_size                       = len;
  ctx.rx_buf                            = s_rx_frame;
  return ctx;
}

/** @brief Restore every module and fake fixture to a known state. */
static void reset_state(void)
{
  if (ra8_esp_hosted_spi_is_open()) {
    (void)ra8_esp_hosted_spi_close();
  }
  ra8_fake_mmap_reset();
  ra8_pin_validator_reset();
  for (uint32_t i = 0U; i < (uint32_t)k_spi_test_events_max; i++) {
    s_events[i] = (uint8_t)k_spi_test_event_none;
  }
  s_event_count = 0U;
  s_seen_len    = 0U;
  s_seen_width  = 0U;
  s_seen_tx     = nullptr;
  s_bus_result  = k_ra8_ok;
  for (uint32_t i = 0U; i < (uint32_t)k_spi_test_frame_bytes; i++) {
    s_tx_frame[i] = (uint8_t)k_spi_test_tx_stamp;
    s_rx_frame[i] = 0U;
  }
  ra8_esp_hosted_spi_set_pin_interface(&s_mock_pin_if);
  ra8_esp_hosted_spi_set_bus(&s_mock_bus);
}

/**
 * @par MC/DC:
 * (no compound decision in the code under test -- this case asserts that the
 * binder populates exactly the three transport rows and rejects a null
 * vtable)
 */
static void test_bind(void)
{
  TEST_BEGIN("esp_hosted spi: bind populates the three transport rows");
  reset_state();
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_esp_hosted_spi_bind(nullptr));

  hosted_osi_funcs_t funcs = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_esp_hosted_spi_bind(&funcs));
  TEST_ASSERT(funcs._h_bus_init != nullptr);
  TEST_ASSERT(funcs._h_bus_deinit != nullptr);
  TEST_ASSERT(funcs._h_do_bus_transfer != nullptr);
  /* Rows this slice does not own must be left for the other slices. */
  TEST_ASSERT(funcs._h_read_gpio == nullptr);
  TEST_END("esp_hosted spi: bind populates the three transport rows");
}

/**
 * @par MC/DC:
 * Decision A: `if ((ctx->tx_buf == nullptr) || (ctx->rx_buf == nullptr))` in
 * `port/esp-hosted/src/ra8_esp_hosted_spi.c@internal_do_bus_transfer`
 * (2 conditions).
 * - Vector A1: (F,F) both buffers set   -> accepted (the control vector)
 * - Vector A2: (T,F) transmit buffer null -> rejected (varies condition 1)
 * - Vector A3: (F,T) receive buffer null  -> rejected (varies condition 2)
 * A1+A2 prove `tx_buf` influences the outcome independently, A1+A3 do the
 * same for `rx_buf`: N+1 = 3 vectors for N=2 conditions.
 *
 * Decision B: `if ((ctx->tx_buf_size == 0U) || (ctx->tx_buf_size >
 * MAX_TRANSPORT_BUFFER_SIZE))` (2 conditions).
 * - Vector B1: (F,F) 1600 bytes  -> accepted (the control vector)
 * - Vector B2: (T,F) zero bytes  -> rejected (varies condition 1)
 * - Vector B3: (F,T) 1601 bytes  -> rejected (varies condition 2)
 * B1+B2 and B1+B3 prove each condition's independent influence: N+1 = 3.
 *
 * The null-context guard is a separate single-condition decision, covered by
 * the null call below plus every other vector in this case.
 */
static void test_transfer_rejections(void)
{
  TEST_BEGIN("esp_hosted spi: the transfer slot rejects every malformed frame");
  reset_state();
  const hosted_osi_funcs_t f = bound_vtable();

  TEST_ASSERT_EQ(RET_INVALID, f._h_do_bus_transfer(nullptr));

  struct hosted_transport_context_t ctx = make_ctx((uint32_t)k_spi_test_frame_bytes);
  ctx.tx_buf                            = nullptr; /* Vector A2 */
  TEST_ASSERT_EQ(RET_INVALID, f._h_do_bus_transfer(&ctx));

  ctx        = make_ctx((uint32_t)k_spi_test_frame_bytes);
  ctx.rx_buf = nullptr; /* Vector A3 */
  TEST_ASSERT_EQ(RET_INVALID, f._h_do_bus_transfer(&ctx));

  ctx = make_ctx(0U); /* Vector B2 */
  TEST_ASSERT_EQ(RET_INVALID, f._h_do_bus_transfer(&ctx));

  ctx = make_ctx((uint32_t)k_spi_test_oversize); /* Vector B3 */
  TEST_ASSERT_EQ(RET_INVALID, f._h_do_bus_transfer(&ctx));

  /* Not one rejection may reach the wire or move the chip select. */
  TEST_ASSERT_EQ(0U, s_event_count);
  TEST_END("esp_hosted spi: the transfer slot rejects every malformed frame");
}

/**
 * @par MC/DC:
 * Vectors A1 and B1 of the two decisions documented on
 * ``test_transfer_rejections``: both buffers set and a legal frame length,
 * which is the only combination that reaches the wire. This case also takes
 * the accepting arm of the single-condition "a bus is bound" test and of the
 * chip-select write test.
 */
static void test_transfer_success_and_ordering(void)
{
  TEST_BEGIN("esp_hosted spi: a good frame is clocked between chip-select edges");
  reset_state();
  const hosted_osi_funcs_t          f   = bound_vtable();
  struct hosted_transport_context_t ctx = make_ctx((uint32_t)k_spi_test_frame_bytes);

  TEST_ASSERT_EQ(RET_OK, f._h_do_bus_transfer(&ctx));

  /* The whole point: the chip select fell before the frame and rose after. */
  TEST_ASSERT_EQ(3U, s_event_count);
  TEST_ASSERT_EQ(k_spi_test_event_cs_low, s_events[0]);
  TEST_ASSERT_EQ(k_spi_test_event_xfer, s_events[1]);
  TEST_ASSERT_EQ(k_spi_test_event_cs_high, s_events[2]);

  /* The backend saw the caller's length, the caller's buffer, 8-bit frames. */
  TEST_ASSERT_EQ(k_spi_test_frame_bytes, s_seen_len);
  TEST_ASSERT_EQ(k_ra8_spi_width_8, s_seen_width);
  TEST_ASSERT((const void*)s_tx_frame == s_seen_tx);
  TEST_ASSERT_EQ(k_spi_test_rx_stamp, s_rx_frame[0]);
  TEST_ASSERT_EQ(k_spi_test_rx_stamp, s_rx_frame[k_spi_test_frame_bytes - 1U]);

  /* A shorter transmit size is honoured rather than rounded up to a frame. */
  reset_state();
  ctx = make_ctx((uint32_t)k_spi_test_short_len);
  TEST_ASSERT_EQ(RET_OK, f._h_do_bus_transfer(&ctx));
  TEST_ASSERT_EQ(k_spi_test_short_len, s_seen_len);
  TEST_END("esp_hosted spi: a good frame is clocked between chip-select edges");
}

/**
 * @par MC/DC:
 * Decision: `if (err != k_ra8_ok)` after the transfer (1 condition, 2
 * vectors). The success vector is in ``test_transfer_success_and_ordering``;
 * this case supplies the failure vector by arming the recording backend to
 * report an error, and asserts the chip select was released anyway.
 */
static void test_transfer_backend_error(void)
{
  TEST_BEGIN("esp_hosted spi: a backend error still releases the chip select");
  reset_state();
  const hosted_osi_funcs_t          f   = bound_vtable();
  struct hosted_transport_context_t ctx = make_ctx((uint32_t)k_spi_test_frame_bytes);

  s_bus_result = k_ra8_err_hw_timeout;
  TEST_ASSERT_EQ(RET_FAIL, f._h_do_bus_transfer(&ctx));
  TEST_ASSERT_EQ(3U, s_event_count);
  TEST_ASSERT_EQ(k_spi_test_event_cs_low, s_events[0]);
  TEST_ASSERT_EQ(k_spi_test_event_xfer, s_events[1]);
  /* A failed frame must never leave the co-processor selected. */
  TEST_ASSERT_EQ(k_spi_test_event_cs_high, s_events[2]);
  TEST_END("esp_hosted spi: a backend error still releases the chip select");
}

/**
 * @par MC/DC:
 * Decision: `if (bus == nullptr)` in the transfer slot, and the matching
 * `if (s_injected_bus != nullptr)` / `s_bus.iface != nullptr` chain in the
 * bus selector (single-condition decisions, 2 vectors each). The bound
 * vectors are in the cases above; this case supplies the unbound ones by
 * clearing the injection while no open has run.
 */
static void test_transfer_without_a_bus(void)
{
  TEST_BEGIN("esp_hosted spi: no bound bus means refuse, not clock a dead channel");
  reset_state();
  const hosted_osi_funcs_t          f   = bound_vtable();
  struct hosted_transport_context_t ctx = make_ctx((uint32_t)k_spi_test_frame_bytes);

  ra8_esp_hosted_spi_set_bus(nullptr);
  TEST_ASSERT_EQ(RET_FAIL, f._h_do_bus_transfer(&ctx));
  TEST_ASSERT_EQ(0U, s_event_count);

  /* And the handle handed to the vendored driver is null for the same reason. */
  TEST_ASSERT(f._h_bus_init() == nullptr);

  ra8_esp_hosted_spi_set_bus(&s_mock_bus);
  TEST_ASSERT(f._h_bus_init() != nullptr);
  TEST_END("esp_hosted spi: no bound bus means refuse, not clock a dead channel");
}

/**
 * @par MC/DC:
 * Decision: `if (bus_handle == nullptr)` then `if (bus_handle != &s_bus)`
 * (two single-condition decisions, 2 vectors each). This case takes the null
 * handle, a foreign handle, and the genuine handle -- which, with no open
 * behind it, reaches the "was not open" arm of the close.
 */
static void test_bus_deinit_handle_checks(void)
{
  TEST_BEGIN("esp_hosted spi: bus deinit only accepts the handle it issued");
  reset_state();
  const hosted_osi_funcs_t f = bound_vtable();

  TEST_ASSERT_EQ(RET_INVALID, f._h_bus_deinit(nullptr));
  TEST_ASSERT_EQ(RET_INVALID, f._h_bus_deinit((void*)(uintptr_t)k_spi_test_alien));

  /* The real handle, but nothing is open behind it: a failure, not a lie. */
  void* handle = f._h_bus_init();
  TEST_ASSERT(handle != nullptr);
  TEST_ASSERT_EQ(RET_FAIL, f._h_bus_deinit(handle));
  TEST_END("esp_hosted spi: bus deinit only accepts the handle it issued");
}

/**
 * @par MC/DC:
 * Decision: `if ((pclk_hz == 0U) || (sck_hz == 0U) || (sci_channel !=
 * pmod1_channel))` in
 * `port/esp-hosted/src/ra8_esp_hosted_spi.c@ra8_esp_hosted_spi_open`
 * (3 conditions).
 * - Vector 1: (F,F,F) live clocks, Pmod1 channel -> accepted elsewhere in
 *   this file (the control vector, in ``test_open_close_cycle``)
 * - Vector 2: (T,F,F) pclk_hz zero               -> rejected
 * - Vector 3: (F,T,F) sck_hz zero                -> rejected
 * - Vector 4: (F,F,T) a different SCI channel    -> rejected
 * Pairing vector 1 with each of 2, 3 and 4 proves that condition's
 * independent influence: N+1 = 4 vectors for N=3 conditions.
 */
static void test_open_rejections(void)
{
  TEST_BEGIN("esp_hosted spi: open rejects dead clocks and the wrong connector");
  reset_state();
  const uint8_t channel = (uint8_t)k_ra8_board_pmod1_sci_channel;

  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_esp_hosted_spi_open(channel, 0U, (uint32_t)k_spi_test_sck_hz));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_esp_hosted_spi_open(channel, (uint32_t)k_spi_test_pclk_hz, 0U));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_esp_hosted_spi_open((uint8_t)k_spi_test_bad_channel,
                                         (uint32_t)k_spi_test_pclk_hz,
                                         (uint32_t)k_spi_test_sck_hz));
  TEST_ASSERT(!ra8_esp_hosted_spi_is_open());
  /* A rejected open must not have taken the chip select. */
  TEST_ASSERT_EQ(0U, s_event_count);
  TEST_ASSERT_EQ(k_ra8_err_not_initialized, ra8_esp_hosted_spi_close());
  TEST_END("esp_hosted spi: open rejects dead clocks and the wrong connector");
}

/**
 * @par MC/DC:
 * Vector 1 of the decision documented on ``test_open_rejections``: live
 * clocks on the Pmod1 channel, the only combination that opens. This case
 * also takes both arms of the single-condition `if (s_open)` guard at the top
 * of the open and of the matching `if (!s_open)` guard in the close.
 */
static void test_open_close_cycle(void)
{
  TEST_BEGIN("esp_hosted spi: open, refuse to re-open, close, refuse to re-close");
  reset_state();
  const uint8_t channel = (uint8_t)k_ra8_board_pmod1_sci_channel;

  TEST_ASSERT_EQ(
    k_ra8_ok,
    ra8_esp_hosted_spi_open(channel, (uint32_t)k_spi_test_pclk_hz, (uint32_t)k_spi_test_sck_hz));
  TEST_ASSERT(ra8_esp_hosted_spi_is_open());
  /* The chip select was taken as an output before any frame could move. */
  TEST_ASSERT_EQ(1U, s_event_count);
  TEST_ASSERT_EQ(k_spi_test_event_cs_init, s_events[0]);
  TEST_ASSERT(ra8_pin_validator_is_claimed((ra8_port_pin_t)k_ra8_board_pmod1_spi_sck));

  TEST_ASSERT_EQ(
    k_ra8_err_invalid_state,
    ra8_esp_hosted_spi_open(channel, (uint32_t)k_spi_test_pclk_hz, (uint32_t)k_spi_test_sck_hz));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_esp_hosted_spi_close());
  TEST_ASSERT(!ra8_esp_hosted_spi_is_open());
  TEST_ASSERT(!ra8_pin_validator_is_claimed((ra8_port_pin_t)k_ra8_board_pmod1_spi_sck));
  TEST_ASSERT(!ra8_pin_validator_is_claimed((ra8_port_pin_t)k_ra8_esp_hosted_pin_chip_select));
  TEST_ASSERT_EQ(k_ra8_err_not_initialized, ra8_esp_hosted_spi_close());
  TEST_END("esp_hosted spi: open, refuse to re-open, close, refuse to re-close");
}

/**
 * @par MC/DC:
 * (no compound decision -- this case takes both arms of the chip-select
 * seam's "something injected / nothing injected" test)
 */
static void test_pin_interface_seam(void)
{
  TEST_BEGIN("esp_hosted spi: the chip-select seam falls back on the HAL");
  reset_state();
  const hosted_osi_funcs_t          f   = bound_vtable();
  struct hosted_transport_context_t ctx = make_ctx((uint32_t)k_spi_test_short_len);

  /* With the production driver restored the mock records nothing, and the
   * transfer still completes because the chip-select pin is claimable. */
  ra8_esp_hosted_spi_set_pin_interface(nullptr);
  TEST_ASSERT_EQ(RET_OK, f._h_do_bus_transfer(&ctx));
  TEST_ASSERT_EQ(1U, s_event_count);
  TEST_ASSERT_EQ(k_spi_test_event_xfer, s_events[0]);

  ra8_esp_hosted_spi_set_pin_interface(&s_mock_pin_if);
  TEST_END("esp_hosted spi: the chip-select seam falls back on the HAL");
}

/** @brief Every case in this translation unit, in execution order. */
static void (*const s_test_roster[])(void) = {
  test_bind,
  test_transfer_rejections,
  test_transfer_success_and_ordering,
  test_transfer_backend_error,
  test_transfer_without_a_bus,
  test_bus_deinit_handle_checks,
  test_open_rejections,
  test_open_close_cycle,
  test_pin_interface_seam,
};

int32_t main(void)
{
  for (size_t i = 0U; i < (sizeof s_test_roster / sizeof s_test_roster[0]); ++i) {
    s_test_roster[i]();
  }
  (void)fprintf(stderr, "[OK ] test_ra8_esp_hosted_spi.c\n");
  return 0;
}
