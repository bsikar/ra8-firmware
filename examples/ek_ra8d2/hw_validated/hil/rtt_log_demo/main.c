/**
 * @file examples/ek_ra8d2/hw_validated/hil/rtt_log_demo/main.c
 * @brief Minimal SEGGER RTT logging demo for EK-RA8D2
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * Allocates a SEGGER-compatible RTT control block in SRAM and writes
 * a counter line into the up-buffer once per second. The on-board
 * J-Link OB picks the block up by scanning RAM for the magic string
 * ``"SEGGER RTT"``; ``JLinkRTTViewer`` on the host then renders the
 * stream live with no UART pin needed.
 *
 * The control-block layout matches ``SEGGER_RTT.h`` (ring buffer with
 * read/write indices). Channel 0 is the standard "Terminal" up-buffer.
 * We keep a 1 KiB up-buffer and a 32 byte down-buffer (unused here).
 *
 * Sequence:
 *   1. ``ra8_cgc_init`` -- bring the chip up to the canonical clock tree.
 *   2. ``ra8_time_init`` -- 1 ms SysTick.
 *   3. Initialise the RTT control block in BSS (idempotent).
 *   4. Loop: ``rtt_write`` a counter line, toggle LED1, sleep 1 s.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "ra8_boot_entry.h"
#include "ra8_attributes.h"
#include "ra8_board_ek_ra8d2.h"
#include "ra8_cgc.h"
#include "ra8_err.h"
#include "ra8_isr.h"
#include "ra8_time.h"

/** @brief Demo tunables. */
typedef enum : uint32_t {
  k_rtt_demo_period_ms = 1000U, /**< Rtt demo period ms.       */
  k_rtt_up_buf_bytes   = 1024U, /**< Rtt up buffer bytes.      */
  k_rtt_down_buf_bytes = 32U,   /**< Rtt down buffer bytes.    */
  k_rtt_decimal_base   = 10U,   /**< Rtt decimal base.         */
  k_rtt_max_digits     = 10U,   /**< Rtt maximum digits.       */
  k_rtt_msg_buf_bytes  = 32U,   /**< Rtt message buffer bytes. */
} rtt_demo_const_t;

/**
 * @brief One direction of an RTT channel (matches SEGGER layout).
 */
typedef struct {
  const char* name;   /**< NUL-terminated channel name.  */
  uint8_t*    buf;    /**< Backing ring buffer.          */
  uint32_t    size;   /**< Bytes of @c buf.              */
  uint32_t    wr_off; /**< Write index (producer).       */
  uint32_t    rd_off; /**< Read index (consumer = host). */
  uint32_t    flags;  /**< 0 = no-block-skip, default.   */
} rtt_buf_t;

/**
 * @brief SEGGER RTT control block. Scanned by the J-Link OB via the
 *        leading ``id[]`` magic. Must live at a known SRAM region.
 */
typedef struct {
  char      id[16];   /**< "SEGGER RTT" + padding.       */
  uint32_t  max_up;   /**< Number of up channels.        */
  uint32_t  max_down; /**< Number of down channels.      */
  rtt_buf_t up[1];    /**< Up channel array (host RX).   */
  rtt_buf_t down[1];  /**< Down channel array (host TX). */
} rtt_cb_t;

/** @brief Up-buffer storage for channel 0. */
static uint8_t s_rtt_up_buf[k_rtt_up_buf_bytes];

/** @brief Down-buffer storage for channel 0. */
static uint8_t s_rtt_down_buf[k_rtt_down_buf_bytes];

/** @brief The control block itself. */
static rtt_cb_t s_rtt_cb;

/**
 * @brief Park the core after an unrecoverable RTT demo failure.
 * @details Repeatedly executes WFI while preserving the ring and clock state.
 * @pre Called only from a fatal boot or terminal foreground path.
 * @pre The caller does not require recovery without reset.
 * @post The core stays in WFI until external intervention.
 * @post No RTT producer index changes after entry.
 * @note Not thread-safe; this is the terminal single-threaded path.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_panic_halt(void)
{
  while (1) {
    __asm__ volatile("wfi");
  }
}

/**
 * @brief Initialise the RTT control block in place.
 *
 * @details
 * The id is written byte-by-byte (rather than as a string literal) so
 * a search for ``"SEGGER RTT"`` in the firmware image only matches the
 * runtime control block, not a stale literal pool entry.
 *
 * @pre Called once before the first @ref internal_write.
 * @pre The file-scope control block and buffers occupy writable SRAM.
 * @post @c s_rtt_cb.id begins with ``S E G G E R sp R T T``.
 * @post Both channel descriptors use empty, bounded caller-owned rings.
 * @note Not thread-safe; initialization replaces every descriptor field.
 *
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_init(void)
{
  static const char k_magic[] = {'S', 'E', 'G', 'G', 'E', 'R', ' ', 'R', 'T', 'T', 0};
  for (size_t i = 0U; i < sizeof k_magic; i++) {
    s_rtt_cb.id[i] = k_magic[i];
  }
  s_rtt_cb.max_up         = 1U;
  s_rtt_cb.max_down       = 1U;
  s_rtt_cb.up[0].name     = "Terminal";
  s_rtt_cb.up[0].buf      = s_rtt_up_buf;
  s_rtt_cb.up[0].size     = (uint32_t)k_rtt_up_buf_bytes;
  s_rtt_cb.up[0].wr_off   = 0U;
  s_rtt_cb.up[0].rd_off   = 0U;
  s_rtt_cb.up[0].flags    = 0U;
  s_rtt_cb.down[0].name   = "Terminal";
  s_rtt_cb.down[0].buf    = s_rtt_down_buf;
  s_rtt_cb.down[0].size   = (uint32_t)k_rtt_down_buf_bytes;
  s_rtt_cb.down[0].wr_off = 0U;
  s_rtt_cb.down[0].rd_off = 0U;
  s_rtt_cb.down[0].flags  = 0U;
}

/**
 * @brief Push @c len bytes into channel 0's up-buffer (non-blocking).
 *
 * @par MC/DC:
 * Compound decision: ``next == rd || size == 0``. Two atomic
 * conditions x N+1 = 3 vectors; the host test exercises full / empty
 * / mid-buffer wrap independently.
 *
 * @param[in] data Bytes to enqueue.
 * @param[in] len  Byte count; data fitting beyond head is dropped.
 *
 * @return RTT producer status.
 * @retval k_ra8_ok            Bytes were appended (or dropped silently).
 * @retval k_ra8_err_null_ptr  data was NULL.
 *
 * @pre ``internal_init`` configured channel zero and its backing buffer.
 * @pre The host owns only ``rd_off`` while this producer owns ``wr_off``.
 * @post Successfully enqueued bytes precede the published write offset.
 * @post The producer never advances into the consumer's current read slot.
 * @note Single-producer only; not thread-safe across firmware writers.
 *
 * @since 0.1.0
 */
[[nodiscard]] RA8_INTERNAL static ra8_err_t internal_write(const uint8_t* data, uint32_t len)
{
  if (data == nullptr) {
    return k_ra8_err_null_ptr;
  }
  rtt_buf_t* up = &s_rtt_cb.up[0];
  for (uint32_t i = 0U; i < len; i++) {
    uint32_t next = up->wr_off + 1U;
    if (next == up->size) {
      next = 0U;
    }
    if (next == up->rd_off) {
      break; /* host has not drained -- drop the tail. */
    }
    up->buf[up->wr_off] = data[i];
    up->wr_off          = next;
  }
  return k_ra8_ok;
}

/**
 * @brief Format ``"rtt_log_demo: NNN\r\n"`` into @p buf.
 * @details Copies the fixed prefix, converts the counter through a bounded
 *          reverse digit buffer, and appends CRLF without a NUL terminator.
 *
 * @param[in]  counter Number to render in base-10 ASCII.
 * @param[out] buf     Output buffer (>= k_rtt_msg_buf_bytes).
 * @return Number of bytes written.
 * @retval 17..26 Complete line length, depending on counter width.
 * @pre ``buf`` points to at least ``k_rtt_msg_buf_bytes`` writable bytes.
 * @pre ``counter`` is a complete 32-bit sample.
 * @post The returned prefix of ``buf`` contains one CRLF-terminated line.
 * @post No byte beyond the returned count is modified.
 * @note Reentrant when callers provide distinct output buffers.
 * @since 0.1.0
 */
RA8_INTERNAL static uint8_t internal_format_line(uint32_t counter, char* buf)
{
  static const char prefix[] = "rtt_log_demo: ";
  uint8_t           n        = 0U;
  for (size_t k = 0U; k < (sizeof prefix - 1U); k++) {
    buf[n++] = prefix[k];
  }
  char     dig[k_rtt_max_digits];
  uint8_t  d   = 0U;
  uint32_t tmp = counter;
  do {
    dig[d++] = (char)('0' + (uint8_t)(tmp % (uint32_t)k_rtt_decimal_base));
    tmp /= (uint32_t)k_rtt_decimal_base;
  } while (tmp != 0U && d < (uint8_t)k_rtt_max_digits);
  while (d > 0U) {
    buf[n++] = dig[--d];
  }
  buf[n++] = '\r';
  buf[n++] = '\n';
  return n;
}

/**
 * @brief Bring CGC + SysTick + LED1 + RTT control block up.
 * @details Initializes clocks, the delay service, LED1, and the in-SRAM RTT
 *          descriptor in dependency order, halting on a failed HAL operation.
 * @pre Reset startup initialized static storage and the vector table.
 * @pre Called once before global interrupt enable.
 * @post On return, channel zero is ready for non-blocking firmware writes.
 * @post LED1 and the millisecond delay service are ready.
 * @note Not thread-safe; it owns the demo's global initialization.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_setup_or_halt(void)
{
  uint32_t cpuclk0_hz = 0U;
  if (ra8_cgc_init() != k_ra8_ok) {
    internal_panic_halt();
  }
  if (ra8_cgc_get_clock_hz(k_ra8_clock_id_cpuclk0, &cpuclk0_hz) != k_ra8_ok) {
    internal_panic_halt();
  }
  if (ra8_time_init(cpuclk0_hz) != k_ra8_ok) {
    internal_panic_halt();
  }
  if (ra8_board_led_init(k_ra8_board_led1) != k_ra8_ok) {
    internal_panic_halt();
  }
  internal_init();
}

/**
 * @brief Publish a monotonically increasing RTT line once per second.
 * @details Initializes channel zero, formats and enqueues each counter line,
 *          toggles LED1, and delays before incrementing the next sample.
 * @pre Reset startup and SystemInit completed successfully.
 * @pre J-Link may update only the consumer offset in the shared descriptor.
 * @post Each successful iteration advances the counter and toggles LED1.
 * @post Any producer or LED error leads to the terminal panic helper.
 * @note Does not return during normal operation.
 * @since 0.1.0
 */
void main(void)
{
  internal_setup_or_halt();
  ra8_isr_globals_enable();

  uint32_t counter = 0U;
  while (1) {
    char          buf[k_rtt_msg_buf_bytes];
    const uint8_t n = internal_format_line(counter, buf);
    if (internal_write((const uint8_t*)buf, (uint32_t)n) != k_ra8_ok) {
      break;
    }
    if (ra8_board_led_toggle(k_ra8_board_led1) != k_ra8_ok) {
      break;
    }
    counter++;
    ra8_delay_ms(k_rtt_demo_period_ms);
  }
  internal_panic_halt();
}
