/**
 * @file examples/ek_ra8d2/hw_validated/c6/c6_fw_version/src/c6_fwver_link.c
 * @brief The esp-hosted transaction pump: frame in, frame out, counted.
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * This is the application's own reduction of the vendored
 * ``spi_transaction_task`` down to what a bring-up needs. The vendored task
 * cannot be used here: it lives in ``transport_drv.c``, which is written
 * against ESP-IDF's Wi-Fi API and is deliberately not compiled into this tree
 * (see ``cmake/esp_hosted.cmake``). What it does on the wire, though, is
 * small and fully specified, and this file does exactly that and nothing
 * more:
 *
 *   1. wait for HANDSHAKE to go active -- the co-processor's "I am armed for
 *      the next transaction" signal;
 *   2. transmit either the caller's one payload, wrapped in a payload header
 *      with the checksum ``get_next_tx_buffer()`` computes, or an
 *      ``ESP_MAX_IF`` filler frame, which is what the reference host sends
 *      when it has nothing queued;
 *   3. clock 1600 bytes full duplex through ``_h_do_bus_transfer``;
 *   4. classify what came back, exactly as ``process_spi_rx_buf()`` does:
 *      zero length is filler, an offset that is not the header size or a
 *      length past ``MAX_PAYLOAD_SIZE`` is malformed, and everything else is
 *      checksum-verified before it is believed.
 *
 * Filler frames are counted rather than delivered. Their header is genuinely
 * not a header -- the co-processor stamps ``if_type = ESP_MAX_IF``,
 * ``if_num = 0x0F`` and leaves ``offset`` at zero -- which is why
 * ``c6_hosted_init`` first read one as "offset is not the payload-header
 * size". Judging a filler frame by the rules for a data frame is a false
 * negative, and this file does not repeat it.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>

#include "c6_fwver.h"
#include "esp_hosted_header.h"
#include "esp_hosted_interface.h"
#include "esp_hosted_os_abstraction.h"
#include "esp_hosted_transport.h"
#include "port_esp_hosted_host_config.h"
#include "port_esp_hosted_host_os.h"
#include "ra8_esp_hosted_port.h"
#include "transport_drv.h"

/**
 * @enum c6_fwver_proto_t
 * @brief Protocol constants the pump judges a received frame against.
 * @details Each is derived from a vendored header, so an upstream change to
 * the payload header or the transport buffer moves this file with it instead
 * of leaving a stale literal behind.
 * @invariant ::k_c6_fwver_hdr_bytes is the ``offset`` a well-formed data
 *            frame carries.
 * @invariant ::k_c6_fwver_max_payload bounds the length such a frame may
 *            advertise; anything larger cannot fit the transaction.
 * @par Example:
 * @code
 * if (rx.header.offset != (uint16_t)k_c6_fwver_hdr_bytes) { reject(); }
 * @endcode
 * @see esp_payload_header
 * @since 0.1.0
 */
typedef enum : uint16_t {
  k_c6_fwver_hdr_bytes = (uint16_t)sizeof(struct esp_payload_header),
  /**< Payload-header size; twelve bytes on this ABI. */
  k_c6_fwver_max_payload = (uint16_t)MAX_PAYLOAD_SIZE,
  /**< Largest payload a valid data frame may advertise. */
  k_c6_fwver_nibble_mask = 0x0FU,
  /**< Mask applied before a write to a four-bit header field. ``if_type``
       and ``if_num`` share one byte, and an unmasked assignment is a
       narrowing conversion the project's ``-Wconversion`` rejects. */
  k_c6_fwver_ifnum_shift = 4U,
  /**< Bit position of ``if_num`` within the header's first byte. Used to
       reconstruct how much a non-zero ``if_num`` contributes to the frame
       checksum, which is what identifies the co-processor defect below. */
} c6_fwver_proto_t;

/**
 * @var s_c6_fwver_tx
 * @brief Transmit buffer for the frame the pump is currently clocking.
 * @details A ::c6_fwver_frame_t, so the payload header is reached through a
 * union member rather than a cast. Aligned to ::k_c6_fwver_dma_align, the
 * alignment the transport expects of a transaction buffer.
 * @note Touched only by the worker thread inside ::c6_fwver_link_pump.
 * @warning Do not resize independently of ::s_c6_fwver_rx; both ends clock
 *          exactly ::k_c6_fwver_frame_bytes.
 * @since 0.1.0
 */
alignas(k_c6_fwver_dma_align) static c6_fwver_frame_t s_c6_fwver_tx;

/**
 * @var s_c6_fwver_rx
 * @brief Receive buffer the co-processor's frame lands in.
 * @details Same type and alignment as ::s_c6_fwver_tx.
 * @note Touched only by the worker thread inside ::c6_fwver_link_pump.
 * @warning The checksum check zeroes the checksum field in place and restores
 *          it; do not read that field concurrently.
 * @since 0.1.0
 */
alignas(k_c6_fwver_dma_align) static c6_fwver_frame_t s_c6_fwver_rx;

/**
 * @brief Zero the whole transmit frame.
 * @return Nothing.
 * @pre ::s_c6_fwver_tx is ::k_c6_fwver_frame_bytes long.
 * @pre No transfer is in flight.
 * @post Every byte of ::s_c6_fwver_tx is zero.
 * @post No other module state is modified.
 * @note The loop is bounded by ::k_c6_fwver_frame_bytes (NASA Rule 2).
 * @since 0.1.0
 */
static void c6_fwver_tx_clear(void)
{
  for (uint16_t i = 0U; i < (uint16_t)k_c6_fwver_frame_bytes; i++) {
    s_c6_fwver_tx.bytes[i] = 0U;
  }
}

/**
 * @brief Stamp the transmit frame as the host's idle filler.
 * @return Nothing.
 * @pre ::s_c6_fwver_tx has been cleared.
 * @pre The co-processor expects a full transaction even with nothing to send.
 * @post Byte zero carries ``ESP_MAX_IF``; every other byte is zero.
 * @post The frame is exactly what the reference host sends when idle.
 * @note Mirrors the dummy-buffer branch of the vendored
 *       ``check_and_execute_spi_transaction()``, which sets only ``if_type``
 *       and leaves length, offset and checksum at zero.
 * @since 0.1.0
 */
static void c6_fwver_tx_filler(void)
{
  c6_fwver_tx_clear();
  s_c6_fwver_tx.header.if_type = (uint8_t)((uint8_t)ESP_MAX_IF & (uint8_t)k_c6_fwver_nibble_mask);
}

/**
 * @brief Wrap a payload in a payload header, checksum included.
 * @param[in] if_type Interface type for the frame.
 * @param[in] if_num Interface number for the frame.
 * @param[in] payload Payload bytes; must be non-null.
 * @param[in] len Payload length, at most ::k_c6_fwver_max_payload.
 * @return Nothing.
 * @pre @p len bytes are readable at @p payload.
 * @pre @p len is at most ::k_c6_fwver_max_payload.
 * @post ::s_c6_fwver_tx holds a well-formed frame whose checksum covers the
 *       header plus the payload.
 * @post Every byte past the payload is zero.
 * @note Field for field what ``get_next_tx_buffer()`` builds in the vendored
 *       ``spi_drv.c``: length, offset, interface, flags, then a checksum over
 *       header plus payload with the checksum field taken as zero.
 * @since 0.1.0
 */
static void c6_fwver_tx_frame(uint8_t if_type, uint8_t if_num, const uint8_t* payload, uint16_t len)
{
  c6_fwver_tx_clear();
  s_c6_fwver_tx.header.if_type = (uint8_t)(if_type & (uint8_t)k_c6_fwver_nibble_mask);
  s_c6_fwver_tx.header.if_num  = (uint8_t)(if_num & (uint8_t)k_c6_fwver_nibble_mask);
  s_c6_fwver_tx.header.len     = len;
  s_c6_fwver_tx.header.offset  = (uint16_t)k_c6_fwver_hdr_bytes;
  for (uint16_t i = 0U; i < len; i++) {
    s_c6_fwver_tx.bytes[(uint16_t)k_c6_fwver_hdr_bytes + i] = payload[i];
  }
  s_c6_fwver_tx.header.checksum =
    compute_checksum(s_c6_fwver_tx.bytes, (uint16_t)((uint16_t)k_c6_fwver_hdr_bytes + len));
}

/**
 * @brief Wait, bounded, for the co-processor to arm HANDSHAKE.
 * @return ``true`` when HANDSHAKE read active within the budget.
 * @retval true The co-processor is ready for a transaction.
 * @retval false ::k_c6_fwver_hs_wait_ms elapsed with the line inactive.
 * @pre The port is up, so ``g_h.funcs`` is populated.
 * @pre HANDSHAKE is configured as an input.
 * @post No module state is modified.
 * @post At most ::k_c6_fwver_hs_wait_ms elapsed.
 * @note The loop is bounded by ::k_c6_fwver_hs_wait_ms (NASA Rule 2). The
 *       line is read through the vtable, not the GPIO HAL, so the pump sees
 *       exactly what the vendored driver would see.
 * @since 0.1.0
 */
static bool c6_fwver_wait_handshake(void)
{
  for (uint16_t waited = 0U; waited < (uint16_t)k_c6_fwver_hs_wait_ms;
       waited          = (uint16_t)(waited + (uint16_t)k_c6_fwver_hs_poll_ms)) {
    const int32_t level =
      (int32_t)g_h.funcs->_h_read_gpio(H_GPIO_HANDSHAKE_Port, (uint32_t)H_GPIO_HANDSHAKE_Pin);
    if (level == (int32_t)H_HS_VAL_ACTIVE) {
      return true;
    }
    (void)g_h.funcs->_h_msleep((unsigned int)k_c6_fwver_hs_poll_ms);
  }
  return false;
}

/**
 * @brief Recompute the received frame's checksum over header plus payload.
 * @param[in] span Bytes the checksum is defined over.
 * @return The checksum computed with the checksum field taken as zero.
 * @pre The transfer has completed and ::s_c6_fwver_rx is stable.
 * @pre @p span is at most ::k_c6_fwver_frame_bytes.
 * @post The checksum field holds exactly the value it held on entry.
 * @post No other buffer byte is modified.
 * @note The field is zeroed in place rather than copied out, which is what
 *       the vendored ``process_spi_rx_buf()`` does; restoring it afterwards
 *       keeps the printed value honest.
 * @since 0.1.0
 */
static uint16_t c6_fwver_rx_checksum(uint16_t span)
{
  const uint16_t saved          = s_c6_fwver_rx.header.checksum;
  s_c6_fwver_rx.header.checksum = 0U;
  const uint16_t calc           = compute_checksum(s_c6_fwver_rx.bytes, span);
  s_c6_fwver_rx.header.checksum = saved;
  return calc;
}

/**
 * @brief Print every header field of a frame the pump is about to drop.
 * @param[in] why Short reason, printed verbatim; must be non-null.
 * @param[in] calc Recomputed checksum, or zero when it was not computed.
 * @return Nothing.
 * @pre The transfer has completed and ::s_c6_fwver_rx is stable.
 * @pre The console is up.
 * @post One line naming every header field was emitted.
 * @post No module state is modified.
 * @note A dropped frame is the most informative thing on a bring-up bench,
 *       and a counter alone cannot say which frame it was. The call sites are
 *       inside a bounded loop, so this cannot flood the console.
 * @since 0.1.0
 */
static void c6_fwver_print_reject(const char* why, uint16_t calc)
{
  c6_fwver_puts("c6_fwver: dropped ");
  c6_fwver_puts(why);
  c6_fwver_puts(" if_type=");
  c6_fwver_put_u32((uint32_t)s_c6_fwver_rx.header.if_type);
  c6_fwver_puts(" if_num=");
  c6_fwver_put_u32((uint32_t)s_c6_fwver_rx.header.if_num);
  c6_fwver_puts(" flags=0x");
  c6_fwver_put_hex((uint32_t)s_c6_fwver_rx.header.flags, (uint8_t)k_c6_fwver_hex_byte);
  c6_fwver_puts(" len=");
  c6_fwver_put_u32((uint32_t)s_c6_fwver_rx.header.len);
  c6_fwver_puts(" offset=");
  c6_fwver_put_u32((uint32_t)s_c6_fwver_rx.header.offset);
  c6_fwver_puts(" seq=");
  c6_fwver_put_u32((uint32_t)s_c6_fwver_rx.header.seq_num);
  c6_fwver_puts(" pkt_type=0x");
  c6_fwver_put_hex((uint32_t)s_c6_fwver_rx.header.reserved3, (uint8_t)k_c6_fwver_hex_byte);
  c6_fwver_puts(" csum=0x");
  c6_fwver_put_hex((uint32_t)s_c6_fwver_rx.header.checksum, (uint8_t)k_c6_fwver_hex_word);
  c6_fwver_puts(" calc=0x");
  c6_fwver_put_hex((uint32_t)calc, (uint8_t)k_c6_fwver_hex_word);
  c6_fwver_puts(" raw=");
  for (uint16_t i = 0U; i < (uint16_t)k_c6_fwver_hdr_bytes; i++) {
    c6_fwver_put_hex((uint32_t)s_c6_fwver_rx.bytes[i], (uint8_t)k_c6_fwver_hex_byte);
  }
  c6_fwver_puts("\r\n");
}

/**
 * @brief Decide whether a checksum failure is the known ``if_num`` defect.
 *
 * @details
 * The ESP32-C6 running esp-hosted-mcu 2.12.11 transmits its bootup
 * ``ESP_PRIV_IF`` INIT event with a non-zero ``if_num`` in the header's first
 * byte, but computes the frame checksum with that nibble still zero. The
 * shortfall is therefore exactly ``if_num << 4``, and a host that tests for
 * that can say so by name instead of reporting an unexplained mismatch.
 *
 * This DIAGNOSES; it does not excuse. The frame is still dropped, because the
 * bytes on the wire do not match the integrity check that accompanies them
 * and a host that decoded them anyway would be trusting an unverified header.
 *
 * @param[in] calc Checksum recomputed over the frame as received.
 * @param[in] stated Checksum the co-processor put in the header.
 * @return ``true`` when the shortfall is exactly the ``if_num`` contribution.
 * @retval true The mismatch is fully explained by the defect above.
 * @retval false The mismatch is something else, and stays unexplained.
 * @pre The transfer has completed and ::s_c6_fwver_rx is stable.
 * @pre @p calc and @p stated genuinely disagree.
 * @post No module state is modified.
 * @post No frame is accepted as a result of this call.
 * @note The arithmetic is deliberately 16-bit and wrapping, matching the
 *       accumulator in the vendored ``compute_checksum()``.
 * @since 0.1.0
 */
static bool c6_fwver_ifnum_defect(uint16_t calc, uint16_t stated)
{
  const uint16_t contribution =
    (uint16_t)((uint16_t)s_c6_fwver_rx.header.if_num << (uint16_t)k_c6_fwver_ifnum_shift);
  return (contribution != 0U) && ((uint16_t)(calc - stated) == contribution);
}

/**
 * @brief Classify the received frame and deliver it if it is real.
 * @param[in] sink Receiver for a well-formed data frame, or null.
 * @param[in,out] stats Counters to update; must be non-null.
 * @return ``true`` when the sink asked the pump to stop.
 * @retval true The sink accepted the frame and wants no more transactions.
 * @retval false Keep pumping.
 * @pre The transfer has completed and ::s_c6_fwver_rx is stable.
 * @pre @p stats is the caller's live counter block.
 * @post Exactly one counter in @p stats was incremented.
 * @post The sink saw the frame only if it verified.
 * @note Ordering matches the vendored ``process_spi_rx_buf()``: zero length
 *       first (filler), then the header sanity tests, then the checksum.
 * @since 0.1.0
 */
static bool c6_fwver_rx_dispatch(c6_fwver_sink_t sink, c6_fwver_pump_stats_t* stats)
{
  const uint16_t len    = s_c6_fwver_rx.header.len;
  const uint16_t offset = s_c6_fwver_rx.header.offset;

  if (len == 0U) {
    stats->idle++;
    return false;
  }
  if ((offset != (uint16_t)k_c6_fwver_hdr_bytes) || (len > (uint16_t)k_c6_fwver_max_payload)) {
    stats->malformed++;
    c6_fwver_print_reject("malformed", 0U);
    return false;
  }
  const uint16_t calc = c6_fwver_rx_checksum((uint16_t)(offset + len));
  if (calc != s_c6_fwver_rx.header.checksum) {
    stats->bad_checksum++;
    c6_fwver_print_reject("bad-checksum", calc);
    if (c6_fwver_ifnum_defect(calc, s_c6_fwver_rx.header.checksum)) {
      stats->ifnum_defect++;
      c6_fwver_puts("c6_fwver:   ^ explained: the co-processor checksummed this frame with "
                    "if_num=0 and then transmitted a non-zero if_num. A conformant host "
                    "drops it -- upstream's own process_spi_rx_buf() would too.\r\n");
    }
    return false;
  }

  stats->frames++;
  if (sink == nullptr) {
    return false;
  }
  return sink(s_c6_fwver_rx.header.if_type,
              s_c6_fwver_rx.header.if_num,
              &s_c6_fwver_rx.bytes[offset],
              len);
}

/**
 * @brief Reject a pump request whose arguments cannot be honoured.
 * @param[in] payload Payload the caller passed, possibly null.
 * @param[in] payload_len Length the caller passed.
 * @param[in] max_transfers Transaction budget the caller passed.
 * @param[in] stats Counter block the caller passed, possibly null.
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok Every argument is usable.
 * @retval k_ra8_err_null_ptr @p stats was null, or a length came with no
 *         buffer.
 * @retval k_ra8_err_invalid_arg @p max_transfers was zero.
 * @retval k_ra8_err_invalid_size @p payload_len exceeds ::k_c6_fwver_tx_max.
 * @retval k_ra8_err_not_initialized The esp-hosted port is not up.
 * @pre The caller has not yet touched the bus.
 * @pre The port's readiness flag reflects a completed init.
 * @post No module state is modified.
 * @post Exactly one code is returned, naming the first failing check.
 * @note Split out of ::c6_fwver_link_pump so that function stays inside the
 *       NASA Rule 4 length budget.
 * @since 0.1.0
 */
static ra8_err_t c6_fwver_pump_check(const uint8_t*               payload,
                                     uint16_t                     payload_len,
                                     uint16_t                     max_transfers,
                                     const c6_fwver_pump_stats_t* stats)
{
  if ((stats == nullptr) || ((payload_len != 0U) && (payload == nullptr))) {
    return k_ra8_err_null_ptr;
  }
  if (max_transfers == 0U) {
    return k_ra8_err_invalid_arg;
  }
  if (payload_len > (uint16_t)k_c6_fwver_tx_max) {
    return k_ra8_err_invalid_size;
  }
  if (!ra8_esp_hosted_port_is_ready()) {
    return k_ra8_err_not_initialized;
  }
  return k_ra8_ok;
}

ra8_err_t c6_fwver_link_pump(uint8_t                if_type,
                             uint8_t                if_num,
                             const uint8_t*         payload,
                             uint16_t               payload_len,
                             uint16_t               max_transfers,
                             c6_fwver_sink_t        sink,
                             c6_fwver_pump_stats_t* stats)
{
  const ra8_err_t bad = c6_fwver_pump_check(payload, payload_len, max_transfers, stats);
  if (bad != k_ra8_ok) {
    return bad;
  }

  *stats                                       = (c6_fwver_pump_stats_t){};
  bool                              tx_pending = (payload_len != 0U);
  struct hosted_transport_context_t ctx        = {};

  uint16_t consecutive_timeouts = 0U;

  for (uint16_t i = 0U; i < max_transfers; i++) {
    if (!c6_fwver_wait_handshake()) {
      stats->hs_timeouts++;
      consecutive_timeouts++;
      if (consecutive_timeouts >= (uint16_t)k_c6_fwver_hs_giveup) {
        break;
      }
      continue;
    }
    consecutive_timeouts = 0U;
    if (tx_pending) {
      c6_fwver_tx_frame(if_type, if_num, payload, payload_len);
      tx_pending = false;
    } else {
      c6_fwver_tx_filler();
    }

    ctx.tx_buf      = s_c6_fwver_tx.bytes;
    ctx.tx_buf_size = (uint32_t)k_c6_fwver_frame_bytes;
    ctx.rx_buf      = s_c6_fwver_rx.bytes;
    if ((int32_t)g_h.funcs->_h_do_bus_transfer(&ctx) != (int32_t)RET_OK) {
      stats->bus_error = true;
      return k_ra8_err_spi_error;
    }
    stats->transfers++;

    if (c6_fwver_rx_dispatch(sink, stats)) {
      stats->sink_stopped = true;
      return k_ra8_ok;
    }
    (void)g_h.funcs->_h_msleep((unsigned int)k_c6_fwver_gap_ms);
  }

  return (stats->transfers == 0U) ? k_ra8_err_hw_timeout : k_ra8_ok;
}
