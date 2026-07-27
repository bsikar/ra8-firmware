/**
 * @file examples/ek_ra8d2/hw_pending/c6_hosted_init/src/c6_hosted_frame.c
 * @brief The single esp-hosted transaction, its decode and its verdict.
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * Builds one valid zero-length esp-hosted frame, clocks it through the
 * OS-abstraction vtable's ``_h_do_bus_transfer``, decodes whatever came
 * back and prints a single PASS/FAIL verdict line.
 *
 * @par PASS criteria, in the order they are tested
 *   1. ``_h_do_bus_transfer`` returned ``RET_OK``.
 *   2. The received buffer is neither uniformly 0x00 nor uniformly 0xFF.
 *   3. ``offset`` equals ``sizeof(struct esp_payload_header)``.
 *   4. ``len`` is within ``MAX_PAYLOAD_SIZE``.
 *   5. The received checksum equals the recomputed one.
 *
 * The undriven-bus test comes before the header tests deliberately. The C6
 * holds its controller-in line with an internal pull-down, so a connected
 * co-processor that never saw a valid transaction reads all-zero, while an
 * unterminated RA8 input floats to all-ones; the two are different faults,
 * and reporting either as a checksum mismatch would point at the wrong
 * problem entirely. See the app README for what each verdict implicates.
 *
 * Every protocol size is read from the vendored headers rather than
 * restated, so an upstream change moves this file with it.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>

#include "c6_hosted.h"
#include "esp_hosted_header.h"
#include "esp_hosted_interface.h"
#include "esp_hosted_os_abstraction.h"
#include "esp_hosted_transport.h"
#include "port_esp_hosted_host_os.h"
#include "port_esp_hosted_host_spi.h"
#include "transport_drv.h"

/**
 * @enum c6_hosted_proto_t
 * @brief Protocol sizes the verdict is judged against.
 * @details Each value is derived from a vendored header, so an upstream
 * change to the transport buffer or the payload header moves these with it
 * instead of leaving a stale literal behind.
 * @invariant ::k_c6_hosted_hdr_bytes is the offset a well-formed frame
 *            carries, which is exactly what the PASS test compares against.
 * @invariant ::k_c6_hosted_max_payload bounds the length a valid frame may
 *            advertise; anything larger cannot fit the transaction.
 * @par Example:
 * @code
 * if (frame->header.len > (uint16_t)k_c6_hosted_max_payload) { reject(); }
 * @endcode
 * @see esp_payload_header
 * @since 0.1.0
 */
typedef enum : uint16_t {
  k_c6_hosted_hdr_bytes = (uint16_t)sizeof(struct esp_payload_header),
  /**< Payload-header size; twelve bytes on this ABI. */
  k_c6_hosted_max_payload = (uint16_t)MAX_PAYLOAD_SIZE,
  /**< Largest payload a valid frame may advertise. */
  k_c6_hosted_byte_ones = 0xFFU,
  /**< The all-ones byte an unterminated RA8 input reads. */
} c6_hosted_proto_t;

/**
 * @enum c6_hosted_fill_t
 * @brief Coarse classification of the whole received buffer.
 * @details A buffer that is uniformly 0x00 or uniformly 0xFF did not come
 * from a peripheral, so it is reported as an undriven bus rather than as
 * whatever header-shaped nonsense its first twelve bytes happen to form.
 * @invariant Exactly one value describes a given buffer.
 * @par Example:
 * @code
 * if (c6_hosted_rx_fill() != k_c6_hosted_fill_mixed) { report_undriven(); }
 * @endcode
 * @see c6_hosted_verdict_t
 * @since 0.1.0
 */
typedef enum : uint8_t {
  k_c6_hosted_fill_mixed = 0U, /**< The buffer holds a mix of byte values. */
  k_c6_hosted_fill_zero  = 1U, /**< Every byte is 0x00.                    */
  k_c6_hosted_fill_ones  = 2U, /**< Every byte is 0xFF.                    */
} c6_hosted_fill_t;

/**
 * @enum c6_hosted_verdict_t
 * @brief Outcome of the single transaction, in the order it is tested.
 * @details One enumerator per failing check, so the printed reason names
 * the first thing that was actually wrong instead of a generic failure.
 * @invariant ::k_c6_hosted_verdict_pass is the only value that prints PASS.
 * @par Example:
 * @code
 * c6_hosted_puts(c6_hosted_verdict_text(verdict));
 * @endcode
 * @see c6_hosted_fill_t
 * @since 0.1.0
 */
typedef enum : uint8_t {
  k_c6_hosted_verdict_pass     = 0U, /**< Every check passed.                                */
  k_c6_hosted_verdict_transfer = 1U, /**< The vtable transfer did not return RET_OK.         */
  k_c6_hosted_verdict_zero     = 2U, /**< Receive buffer all-zero: bus not driven.           */
  k_c6_hosted_verdict_ones     = 3U, /**< Receive buffer all-ones: bus not driven.           */
  k_c6_hosted_verdict_offset   = 4U, /**< Header offset is not the payload-header size.      */
  k_c6_hosted_verdict_length   = 5U, /**< Advertised length exceeds MAX_PAYLOAD_SIZE.        */
  k_c6_hosted_verdict_csum     = 6U, /**< Received checksum differs from the recomputed one. */
} c6_hosted_verdict_t;

/**
 * @var s_c6_hosted_tx
 * @brief Transmit buffer holding the single idle frame this app sends.
 * @details A ::c6_hosted_frame_t, so the payload header is reached through a
 * union member rather than a cast. Aligned to ::k_c6_hosted_dma_align, the
 * alignment the transport expects of a transaction buffer. Zero as ``.bss``,
 * so the bytes after the header are already the padding a valid frame wants.
 * @note Touched only by the worker thread and the transfer it starts.
 * @warning Do not resize independently of ::s_c6_hosted_rx; both ends clock
 *          exactly ::k_c6_hosted_frame_bytes.
 * @since 0.1.0
 */
alignas(k_c6_hosted_dma_align) static c6_hosted_frame_t s_c6_hosted_tx;

/**
 * @var s_c6_hosted_rx
 * @brief Receive buffer the co-processor's frame lands in.
 * @details Same type and alignment as ::s_c6_hosted_tx, and left zero until
 * the transfer runs, so an all-zero buffer afterwards genuinely means
 * nothing was clocked in.
 * @note Touched only by the worker thread and the transfer it starts.
 * @warning ::c6_hosted_rx_checksum zeroes the checksum field in place and
 *          restores it; do not read that field concurrently.
 * @since 0.1.0
 */
alignas(k_c6_hosted_dma_align) static c6_hosted_frame_t s_c6_hosted_rx;

/**
 * @brief Build the idle frame this application transmits.
 * @return Nothing.
 * @pre ::s_c6_hosted_tx is at least ::k_c6_hosted_hdr_bytes long.
 * @pre The payload area is already zero, which ``.bss`` guarantees.
 * @post ::s_c6_hosted_tx holds a well-formed zero-length frame addressed to
 *       ``ESP_MAX_IF``, the interface the transport treats as no-payload.
 * @post The checksum covers exactly the header, since the length is zero.
 * @note Mirrors the filler frame the vendored ``spi_drv.c`` sends when it
 *       has nothing queued, so the co-processor sees nothing unusual.
 * @since 0.1.0
 */
static void c6_hosted_build_idle_frame(void)
{
  s_c6_hosted_tx.header.if_type      = (uint8_t)ESP_MAX_IF;
  s_c6_hosted_tx.header.if_num       = 0U;
  s_c6_hosted_tx.header.flags        = 0U;
  s_c6_hosted_tx.header.len          = 0U;
  s_c6_hosted_tx.header.offset       = (uint16_t)k_c6_hosted_hdr_bytes;
  s_c6_hosted_tx.header.checksum     = 0U;
  s_c6_hosted_tx.header.seq_num      = 0U;
  s_c6_hosted_tx.header.throttle_cmd = 0U;
  s_c6_hosted_tx.header.reserved2    = 0U;
  s_c6_hosted_tx.header.reserved3    = 0U;
  s_c6_hosted_tx.header.checksum =
    compute_checksum(s_c6_hosted_tx.bytes, (uint16_t)k_c6_hosted_hdr_bytes);
}

/**
 * @brief Classify the received buffer as all-zero, all-ones or mixed.
 * @return The classification of ::s_c6_hosted_rx.
 * @retval k_c6_hosted_fill_zero  Every byte is 0x00.
 * @retval k_c6_hosted_fill_ones  Every byte is 0xFF.
 * @retval k_c6_hosted_fill_mixed Anything else.
 * @pre The transfer has completed, so the buffer is stable.
 * @pre ::s_c6_hosted_rx is ::k_c6_hosted_frame_bytes long.
 * @post No buffer byte is modified.
 * @post Exactly one classification is returned.
 * @note Scans the whole transaction, not just the header: a header that
 *       happens to look plausible inside an otherwise dead buffer is still
 *       a dead buffer. The scan is bounded by ::k_c6_hosted_frame_bytes.
 * @since 0.1.0
 */
static c6_hosted_fill_t c6_hosted_rx_fill(void)
{
  bool all_zero = true;
  bool all_ones = true;
  for (uint32_t i = 0U; i < (uint32_t)k_c6_hosted_frame_bytes; i++) {
    if (s_c6_hosted_rx.bytes[i] != 0U) {
      all_zero = false;
    }
    if (s_c6_hosted_rx.bytes[i] != (uint8_t)k_c6_hosted_byte_ones) {
      all_ones = false;
    }
  }
  if (all_zero) {
    return k_c6_hosted_fill_zero;
  }
  if (all_ones) {
    return k_c6_hosted_fill_ones;
  }
  return k_c6_hosted_fill_mixed;
}

/**
 * @brief Recompute the checksum of the received frame.
 * @return The checksum computed over the received frame with its checksum
 *         field taken as zero.
 * @pre The transfer has completed and ::s_c6_hosted_rx is stable.
 * @pre No other context is reading the header concurrently.
 * @post The checksum field holds exactly the value it held on entry.
 * @post No other buffer byte is modified.
 * @note The span covers header plus payload only when the header is
 *       self-consistent; a garbage offset or length would otherwise run
 *       past the buffer, so those cases are summed over the header alone
 *       and fail on their own grounds instead.
 * @since 0.1.0
 */
static uint16_t c6_hosted_rx_checksum(void)
{
  uint16_t span = (uint16_t)k_c6_hosted_hdr_bytes;
  if ((s_c6_hosted_rx.header.offset == (uint16_t)k_c6_hosted_hdr_bytes) &&
      (s_c6_hosted_rx.header.len <= (uint16_t)k_c6_hosted_max_payload)) {
    span = (uint16_t)(span + s_c6_hosted_rx.header.len);
  }
  const uint16_t saved           = s_c6_hosted_rx.header.checksum;
  s_c6_hosted_rx.header.checksum = 0U;
  const uint16_t calc            = compute_checksum(s_c6_hosted_rx.bytes, span);
  s_c6_hosted_rx.header.checksum = saved;
  return calc;
}

/**
 * @brief Decide the verdict for the completed transaction.
 * @param[in] rc   Value ``_h_do_bus_transfer`` returned.
 * @param[in] calc Checksum recomputed by ::c6_hosted_rx_checksum.
 * @return The first failing check, or ::k_c6_hosted_verdict_pass.
 * @retval k_c6_hosted_verdict_transfer The transfer itself failed.
 * @retval k_c6_hosted_verdict_zero     The bus was never driven (all-zero).
 * @retval k_c6_hosted_verdict_ones     The bus was never driven (all-ones).
 * @retval k_c6_hosted_verdict_offset   Offset is not the header size.
 * @retval k_c6_hosted_verdict_length   Length exceeds ``MAX_PAYLOAD_SIZE``.
 * @retval k_c6_hosted_verdict_csum     Checksum mismatch.
 * @retval k_c6_hosted_verdict_pass     Every check passed.
 * @pre The transfer has completed and ::s_c6_hosted_rx is stable.
 * @pre @p calc came from ::c6_hosted_rx_checksum for this same buffer.
 * @post No application state is modified.
 * @post Exactly one verdict is returned.
 * @note The undriven-bus tests come first deliberately: a missing wire and
 *       a mis-clocked link are different faults and must not be reported as
 *       one another.
 * @since 0.1.0
 */
static c6_hosted_verdict_t c6_hosted_classify(int32_t rc, uint16_t calc)
{
  const c6_hosted_fill_t fill = c6_hosted_rx_fill();

  if (rc != (int32_t)RET_OK) {
    return k_c6_hosted_verdict_transfer;
  }
  if (fill == k_c6_hosted_fill_zero) {
    return k_c6_hosted_verdict_zero;
  }
  if (fill == k_c6_hosted_fill_ones) {
    return k_c6_hosted_verdict_ones;
  }
  if (s_c6_hosted_rx.header.offset != (uint16_t)k_c6_hosted_hdr_bytes) {
    return k_c6_hosted_verdict_offset;
  }
  if (s_c6_hosted_rx.header.len > (uint16_t)k_c6_hosted_max_payload) {
    return k_c6_hosted_verdict_length;
  }
  if (s_c6_hosted_rx.header.checksum != calc) {
    return k_c6_hosted_verdict_csum;
  }
  return k_c6_hosted_verdict_pass;
}

/**
 * @brief Map a verdict onto the text printed after PASS or FAIL.
 * @param[in] verdict Verdict to describe.
 * @return A static NUL-terminated reason string; never null.
 * @retval "link up" The verdict is ::k_c6_hosted_verdict_pass.
 * @pre @p verdict came from ::c6_hosted_classify.
 * @pre The caller prints the string rather than storing it.
 * @post No application state is modified.
 * @post The returned pointer stays valid for the life of the program.
 * @note An unrecognised verdict yields a distinct string rather than null,
 *       so a future enumerator cannot crash the report.
 * @since 0.1.0
 */
static const char* c6_hosted_verdict_text(c6_hosted_verdict_t verdict)
{
  switch (verdict) {
    case k_c6_hosted_verdict_pass:
      return "link up";
    case k_c6_hosted_verdict_transfer:
      return "bus transfer did not return RET_OK";
    case k_c6_hosted_verdict_zero:
      return "receive buffer all-zero -- co-processor did not drive the bus";
    case k_c6_hosted_verdict_ones:
      return "receive buffer all-ones -- co-processor did not drive the bus";
    case k_c6_hosted_verdict_offset:
      return "header offset is not the payload-header size";
    case k_c6_hosted_verdict_length:
      return "advertised length exceeds MAX_PAYLOAD_SIZE";
    case k_c6_hosted_verdict_csum:
      return "checksum mismatch";
    default:
      return "unrecognised verdict";
  }
}

/**
 * @brief Print every field of the received payload header.
 * @param[in] calc Checksum recomputed by ::c6_hosted_rx_checksum.
 * @return Nothing.
 * @pre The transfer has completed and ::s_c6_hosted_rx is stable.
 * @pre The console is up.
 * @post One line carrying all header fields and both checksums was emitted.
 * @post No application state is modified.
 * @note Printed for every outcome: on a dead bus the field values are
 *       themselves the evidence.
 * @since 0.1.0
 */
static void c6_hosted_print_rx_header(uint16_t calc)
{
  c6_hosted_puts("c6_hosted_init: rx if_type=");
  c6_hosted_put_u32((uint32_t)s_c6_hosted_rx.header.if_type);
  c6_hosted_puts(" if_num=");
  c6_hosted_put_u32((uint32_t)s_c6_hosted_rx.header.if_num);
  c6_hosted_puts(" flags=0x");
  c6_hosted_put_hex((uint32_t)s_c6_hosted_rx.header.flags, (uint8_t)k_c6_hosted_hex_byte);
  c6_hosted_puts(" len=");
  c6_hosted_put_u32((uint32_t)s_c6_hosted_rx.header.len);
  c6_hosted_puts(" offset=");
  c6_hosted_put_u32((uint32_t)s_c6_hosted_rx.header.offset);
  c6_hosted_puts(" seq_num=");
  c6_hosted_put_u32((uint32_t)s_c6_hosted_rx.header.seq_num);
  c6_hosted_puts(" csum=0x");
  c6_hosted_put_hex((uint32_t)s_c6_hosted_rx.header.checksum, (uint8_t)k_c6_hosted_hex_csum);
  c6_hosted_puts(" calc=0x");
  c6_hosted_put_hex((uint32_t)calc, (uint8_t)k_c6_hosted_hex_csum);
  c6_hosted_puts("\r\n");
}

void c6_hosted_run_transaction(void)
{
  struct hosted_transport_context_t ctx = {};

  c6_hosted_build_idle_frame();
  ctx.tx_buf      = s_c6_hosted_tx.bytes;
  ctx.tx_buf_size = (uint32_t)k_c6_hosted_frame_bytes;
  ctx.rx_buf      = s_c6_hosted_rx.bytes;

  const int32_t             rc      = (int32_t)g_h.funcs->_h_do_bus_transfer(&ctx);
  const uint16_t            calc    = c6_hosted_rx_checksum();
  const c6_hosted_verdict_t verdict = c6_hosted_classify(rc, calc);

  c6_hosted_puts("c6_hosted_init: transfer rc=");
  c6_hosted_put_i32(rc);
  c6_hosted_puts(" frame_bytes=");
  c6_hosted_put_u32((uint32_t)k_c6_hosted_frame_bytes);
  c6_hosted_puts("\r\n");
  c6_hosted_print_rx_header(calc);
  c6_hosted_puts((verdict == k_c6_hosted_verdict_pass) ? "c6_hosted_init: PASS "
                                                       : "c6_hosted_init: FAIL ");
  c6_hosted_puts(c6_hosted_verdict_text(verdict));
  c6_hosted_puts("\r\n");
}
