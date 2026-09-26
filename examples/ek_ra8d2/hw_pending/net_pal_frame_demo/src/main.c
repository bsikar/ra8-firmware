/**
 * @file examples/ek_ra8d2/hw_pending/net_pal_frame_demo/src/main.c
 * @brief First application consumer of ra8_net_pal: the stack-facing contract.
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * `libs/ra8_net_pal/` is the Ring-4 ethernet PAL: it owns the MAC address,
 * wraps the Ring-3 `ra8_eth` driver (ESWM module-stop gate, status fan-out),
 * and hands a network stack one send primitive, one receive primitive, a link
 * query and one event hook. Nothing in `apps/` or `examples/` ever included
 * `ra8_net_pal.h`. Its only in-tree callers were the TrustZone veneers in
 * `libs/ra8_nsc/src/ra8_nsc_eth.c` (themselves uncalled by any application)
 * and the host tests, and NetX Duo's driver deliberately bypasses the PAL and
 * talks to `ra8_eth_*` directly (#621). This app is the first consumer that
 * brings the PAL up on the board and drives the whole stack-facing surface.
 *
 * Legs:
 *
 *   1. `bind`: ::ra8_net_pal_init programmes the supplied MAC, the address
 *      reads back byte for byte, and the link reports down on a fresh PAL.
 *   2. `mac`: a second MAC replaces the first through
 *      ::ra8_net_pal_set_mac_addr, and both accessors refuse NULL.
 *   3. `ring`: three frames of different lengths (64, 128 and the 1518-byte
 *      `k_ra8_net_pal_frame_max` ceiling) go in and come back out in FIFO
 *      order, each byte-verified with its length preserved.
 *   4. `backpressure`: the ring depth is measured (send until the PAL reports
 *      ::k_ra8_err_no_mem, count the accepted frames) rather than restated
 *      from the PAL's private constant; refusal repeats while the ring is
 *      full, draining one slot makes room again, and an empty ring reports
 *      ::k_ra8_err_no_data.
 *   5. `guards`: zero length, one byte past the frame ceiling, NULL frame,
 *      NULL receive arguments, an undersized receive buffer and a NULL link
 *      pointer are each refused with the documented code, and the ring is
 *      then shown to be exactly as empty as it was on entry.
 *   6. `events`: an installed handler sees one `tx_done` per accepted send,
 *      and detaching it stops delivery.
 *   7. `unwind`: after ::ra8_net_pal_deinit every entry point reports
 *      ::k_ra8_err_invalid_state, including a second deinit.
 *
 * The PAL's ring is RAM-backed today (the GWCA descriptor engine lands with
 * the real media path), so the round trip is a genuine loopback of the
 * stack-facing API rather than wire traffic: no PHY, no cable and no link
 * partner are needed, and the run is observable on a stock EK-RA8D2 over the
 * SCI8 / J-Link OB VCOM console. `ra8_net_pal_init` does touch hardware --
 * it releases the ESWM module-stop gate through `ra8_eth_init` -- so the boot
 * path below brings up CGC and the MSTP controller first, as the PAL's
 * preconditions require.
 *
 * A good run prints one verdict per leg and a final `ALL PASS`.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>

#include "ra8_boot_entry.h"
#include "ra8_cgc.h"
#include "ra8_err.h"
#include "ra8_io_log.h"
#include "ra8_io_stream.h"
#include "ra8_io_stream_uart.h"
#include "ra8_log.h"
#include "ra8_mstp.h"
#include "ra8_net_pal.h"
#include "ra8_sci.h"

/**
 * @enum npf_const_t
 * @brief Console, frame-geometry and workload knobs (no magic numbers).
 *
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_npf_uart_chan   = 8U,    /**< SCI8 J-Link OB console.               */
  k_npf_ring_probe  = 64U,   /**< Cap on the ring-depth discovery loop. */
  k_npf_len_small   = 64U,   /**< Minimum untagged ethernet frame.      */
  k_npf_len_mid     = 128U,  /**< Second round-trip length.             */
  k_npf_len_zero    = 0U,    /**< Refused: a frame must carry bytes.    */
  k_npf_fill_a      = 0xA5U, /**< Fill byte for the first frame.        */
  k_npf_fill_b      = 0x5AU, /**< Fill byte for the second frame.       */
  k_npf_fill_c      = 0x3CU, /**< Fill byte for the max-length frame.   */
  k_npf_fill_probe  = 0x11U, /**< Fill byte for guard-leg probes.       */
  k_npf_seed_mul    = 31U,   /**< Per-index pattern multiplier.         */
  k_npf_expect_txok = 3U,    /**< Accepted sends in the event leg.      */
} npf_const_t;

/**
 * @enum npf_mac_oui_t
 * @brief Shared OUI octets of both demo MAC addresses (no magic numbers).
 *
 * @details Bit 1 of the first octet is the locally-administered flag and bit 0
 *          is clear, so both addresses are valid unicast MACs no vendor owns.
 *
 * @since 0.1.0
 */
typedef enum : uint8_t {
  k_npf_mac_oui_0 = 0x02U, /**< Locally administered, unicast. */
  k_npf_mac_oui_1 = 0x00U, /**< Second OUI octet.              */
  k_npf_mac_oui_2 = 0x5EU, /**< Third OUI octet.               */
} npf_mac_oui_t;

/**
 * @enum npf_mac_nic_t
 * @brief NIC-specific octets that distinguish the two demo MAC addresses.
 *
 * @details Only these three octets differ between ::k_npf_mac_a and
 *          ::k_npf_mac_b, which is what makes the `mac` leg's replacement
 *          observable byte for byte.
 *
 * @since 0.1.0
 */
typedef enum : uint8_t {
  k_npf_mac_a_nic_0 = 0x10U, /**< First MAC, NIC octet 0.  */
  k_npf_mac_a_nic_1 = 0x20U, /**< First MAC, NIC octet 1.  */
  k_npf_mac_a_nic_2 = 0x30U, /**< First MAC, NIC octet 2.  */
  k_npf_mac_b_nic_0 = 0xAAU, /**< Second MAC, NIC octet 0. */
  k_npf_mac_b_nic_1 = 0xBBU, /**< Second MAC, NIC octet 1. */
  k_npf_mac_b_nic_2 = 0xCCU, /**< Second MAC, NIC octet 2. */
} npf_mac_nic_t;

/** @brief First MAC programmed at init (locally administered, unicast). */
static const ra8_net_pal_mac_t k_npf_mac_a = {
  .bytes = {k_npf_mac_oui_0, k_npf_mac_oui_1, k_npf_mac_oui_2, k_npf_mac_a_nic_0,
            k_npf_mac_a_nic_1, k_npf_mac_a_nic_2},
};

/** @brief Replacement MAC used by the `mac` leg. */
static const ra8_net_pal_mac_t k_npf_mac_b = {
  .bytes = {k_npf_mac_oui_0, k_npf_mac_oui_1, k_npf_mac_oui_2, k_npf_mac_b_nic_0,
            k_npf_mac_b_nic_1, k_npf_mac_b_nic_2},
};

static ra8_io_stream_t            s_uart;       /**< Console stream.       */
static ra8_io_stream_uart_state_t s_uart_state; /**< Console stream state. */

/** @brief Transmit scratch, sized to the PAL frame ceiling. */
static uint8_t s_tx[(uint16_t)k_ra8_net_pal_frame_max];
/** @brief Receive scratch, sized to the PAL frame ceiling. */
static uint8_t s_rx[(uint16_t)k_ra8_net_pal_frame_max];

/** @brief Event bits observed by ::internal_on_event, OR-accumulated. */
static volatile uint32_t s_event_mask;
/** @brief Number of event callbacks ::internal_on_event has seen. */
static volatile uint32_t s_event_count;
/** @brief Context pointer the PAL handed back on the last callback. */
static volatile void* s_event_ctx;

/**
 * @brief Write a NUL-terminated string to the console stream.
 *
 * @param[in] text Message to queue on SCI8.
 * @return void
 * @pre The console stream was initialised.
 * @post The text was queued on the console sink.
 * @note Errors are ignored: the console reports, it does not act.
 * @since 0.1.0
 */
static void internal_print(const char* text)
{
  (void)ra8_io_stream_puts(&s_uart, text);
}

/**
 * @brief Fill a buffer with a length-dependent, position-dependent pattern.
 *
 * @details Each byte mixes the fill seed with the index so a truncated or
 *          shifted copy cannot pass ::internal_match by accident.
 *
 * @param[out] dst  Destination buffer holding at least @p len bytes.
 * @param[in]  len  Bytes to write.
 * @param[in]  seed Per-frame fill byte.
 * @return void
 * @pre @p dst is non-NULL and covers @p len bytes.
 * @post `dst[0..len-1]` holds the derived pattern.
 * @since 0.1.0
 */
static void internal_pattern(uint8_t* dst, uint16_t len, uint8_t seed)
{
  for (uint16_t i = 0U; i < len; ++i) {
    dst[i] = (uint8_t)(seed ^ (uint8_t)((uint32_t)i * (uint32_t)k_npf_seed_mul));
  }
}

/**
 * @brief Compare two buffers byte for byte.
 *
 * @param[in] lhs First buffer.
 * @param[in] rhs Second buffer.
 * @param[in] len Bytes to compare.
 * @return bool True when every byte matches.
 * @retval true  The two buffers are identical over @p len bytes.
 * @retval false A byte differed.
 * @pre Both buffers cover @p len bytes.
 * @post Neither buffer is modified.
 * @since 0.1.0
 */
static bool internal_match(const uint8_t* lhs, const uint8_t* rhs, uint16_t len)
{
  for (uint16_t i = 0U; i < len; ++i) {
    if (lhs[i] != rhs[i]) {
      return false;
    }
  }
  return true;
}

/**
 * @brief PAL event handler: accumulate the mask and count the deliveries.
 *
 * @param[in] ctx        Context the app handed to ::ra8_net_pal_set_event_handler.
 * @param[in] event_mask OR of `k_ra8_net_pal_event_*` bits for this delivery.
 * @return void
 * @pre The PAL is initialised and this handler is installed.
 * @post ::s_event_mask gains @p event_mask and ::s_event_count advances by one.
 * @note Runs from the send path and from `ra8_eth` event context; it only
 *       touches its own counters, so it stays ISR-safe.
 * @since 0.1.0
 */
static void internal_on_event(void* ctx, uint32_t event_mask)
{
  s_event_ctx = ctx;
  s_event_mask |= event_mask;
  ++s_event_count;
}

/**
 * @brief Send one patterned frame and verify the PAL hands it straight back.
 *
 * @param[in] len  Frame length in bytes.
 * @param[in] seed Fill seed for ::internal_pattern.
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok              The frame returned with the same length + bytes.
 * @retval k_ra8_err_invalid_arg The length or the payload came back altered.
 * @retval (other)               The failing PAL call's code.
 * @pre The PAL is initialised and its ring has a free slot.
 * @post The ring is back to the depth it had on entry.
 * @since 0.1.0
 */
static ra8_err_t internal_round_trip(uint16_t len, uint8_t seed)
{
  internal_pattern(s_tx, len, seed);

  const ra8_err_t tx_err = ra8_net_pal_send_frame(s_tx, len);
  if (tx_err != k_ra8_ok) {
    return tx_err;
  }

  uint16_t        got    = (uint16_t)k_ra8_net_pal_frame_max;
  const ra8_err_t rx_err = ra8_net_pal_recv_frame(s_rx, &got);
  if (rx_err != k_ra8_ok) {
    return rx_err;
  }
  if (got != len) {
    return k_ra8_err_invalid_arg;
  }
  return internal_match(s_tx, s_rx, len) ? k_ra8_ok : k_ra8_err_invalid_arg;
}

/**
 * @brief Leg 1: init programmes the MAC and the link starts down.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok                The MAC read back and the link was down.
 * @retval k_ra8_err_invalid_state The link came up on a fresh PAL.
 * @retval k_ra8_err_invalid_arg   The MAC read back altered.
 * @retval (other)                 The failing PAL call's code.
 * @pre CGC and the MSTP controller are up; the PAL is not initialised.
 * @post On success the PAL is initialised and holds ::k_npf_mac_a.
 * @since 0.1.0
 */
static ra8_err_t internal_check_bind(void)
{
  const ra8_err_t init_err = ra8_net_pal_init(&k_npf_mac_a);
  if (init_err != k_ra8_ok) {
    return init_err;
  }

  ra8_net_pal_mac_t got     = {};
  const ra8_err_t   mac_err = ra8_net_pal_get_mac_addr(&got);
  if (mac_err != k_ra8_ok) {
    return mac_err;
  }
  if (!internal_match(got.bytes, k_npf_mac_a.bytes, (uint16_t)k_ra8_net_pal_mac_addr_len)) {
    return k_ra8_err_invalid_arg;
  }

  ra8_net_pal_link_state_t link     = k_ra8_net_pal_link_up;
  const ra8_err_t          link_err = ra8_net_pal_link_status(&link);
  if (link_err != k_ra8_ok) {
    return link_err;
  }
  return (link == k_ra8_net_pal_link_down) ? k_ra8_ok : k_ra8_err_invalid_state;
}

/**
 * @brief Leg 2: a second MAC replaces the first, and NULL is refused.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok              Replacement stored; both accessors refused NULL.
 * @retval k_ra8_err_invalid_arg The replacement did not take, or NULL was accepted.
 * @retval (other)               The failing PAL call's code.
 * @pre ::internal_check_bind succeeded.
 * @post The PAL holds ::k_npf_mac_b.
 * @since 0.1.0
 */
static ra8_err_t internal_check_mac(void)
{
  const ra8_err_t set_err = ra8_net_pal_set_mac_addr(&k_npf_mac_b);
  if (set_err != k_ra8_ok) {
    return set_err;
  }

  ra8_net_pal_mac_t got     = {};
  const ra8_err_t   get_err = ra8_net_pal_get_mac_addr(&got);
  if (get_err != k_ra8_ok) {
    return get_err;
  }
  if (!internal_match(got.bytes, k_npf_mac_b.bytes, (uint16_t)k_ra8_net_pal_mac_addr_len)) {
    return k_ra8_err_invalid_arg;
  }

  if (ra8_net_pal_set_mac_addr(nullptr) != k_ra8_err_null_ptr) {
    return k_ra8_err_invalid_arg;
  }
  if (ra8_net_pal_get_mac_addr(nullptr) != k_ra8_err_null_ptr) {
    return k_ra8_err_invalid_arg;
  }
  return k_ra8_ok;
}

/**
 * @brief Leg 3: three frame sizes round trip, including the 1518-byte ceiling.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok The three frames returned intact, FIFO ordered.
 * @retval (other)  The first failing round trip's code.
 * @pre The PAL is initialised and its ring is empty.
 * @post The ring is empty again.
 * @since 0.1.0
 */
static ra8_err_t internal_check_ring(void)
{
  const ra8_err_t small_err = internal_round_trip((uint16_t)k_npf_len_small, (uint8_t)k_npf_fill_a);
  if (small_err != k_ra8_ok) {
    return small_err;
  }
  const ra8_err_t mid_err = internal_round_trip((uint16_t)k_npf_len_mid, (uint8_t)k_npf_fill_b);
  if (mid_err != k_ra8_ok) {
    return mid_err;
  }
  return internal_round_trip((uint16_t)k_ra8_net_pal_frame_max, (uint8_t)k_npf_fill_c);
}

/**
 * @brief Leg 4: the ring saturates at its slot count and recovers on drain.
 *
 * @details Fills the ring with distinct patterns until the PAL refuses, which
 *          is how the depth is learned (the PAL keeps it private), checks the
 *          refusal repeats, drains slot 0 and verifies it is the frame that
 *          went in first, then re-sends into the freed slot and drains the
 *          rest until the ring reports empty.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok              Saturation, FIFO order and recovery all held.
 * @retval k_ra8_err_invalid_arg An over-full send was accepted, or FIFO order broke.
 * @retval (other)               The failing PAL call's code.
 * @pre The PAL is initialised and its ring is empty.
 * @post The ring is empty again.
 * @since 0.1.0
 */
static ra8_err_t internal_check_backpressure(void)
{
  uint16_t len = (uint16_t)k_ra8_net_pal_frame_max;
  if (ra8_net_pal_recv_frame(s_rx, &len) != k_ra8_err_no_data) {
    return k_ra8_err_invalid_arg;
  }

  /* The ring depth is private to ra8_net_pal.c and is not exported by
   * ra8_net_pal.h, so this leg MEASURES it instead of duplicating the
   * constant: send until the PAL refuses, and count what it accepted. A
   * hard-coded depth here would keep passing while quietly asserting the
   * wrong number if the PAL ever resized its ring. */
  uint32_t depth = 0U;
  for (uint32_t i = 0U; i < (uint32_t)k_npf_ring_probe; ++i) {
    internal_pattern(s_tx, (uint16_t)k_npf_len_small, (uint8_t)i);
    const ra8_err_t err = ra8_net_pal_send_frame(s_tx, (uint16_t)k_npf_len_small);
    if (err == k_ra8_err_no_mem) {
      break;
    }
    if (err != k_ra8_ok) {
      return err;
    }
    ++depth;
  }
  /* A ring that never refuses (or refuses the very first frame) would make
   * every claim below vacuous, so both ends are checked explicitly. */
  if ((depth == 0U) || (depth >= (uint32_t)k_npf_ring_probe)) {
    return k_ra8_err_invalid_arg;
  }

  /* Refusal must be repeatable while the ring stays full, not a one-off. */
  internal_pattern(s_tx, (uint16_t)k_npf_len_small, (uint8_t)k_npf_fill_probe);
  if (ra8_net_pal_send_frame(s_tx, (uint16_t)k_npf_len_small) != k_ra8_err_no_mem) {
    return k_ra8_err_invalid_arg;
  }

  /* Slot 0 must come back first: the ring is FIFO, not a stack. */
  len                    = (uint16_t)k_ra8_net_pal_frame_max;
  const ra8_err_t rx_err = ra8_net_pal_recv_frame(s_rx, &len);
  if (rx_err != k_ra8_ok) {
    return rx_err;
  }
  internal_pattern(s_tx, (uint16_t)k_npf_len_small, 0U);
  if ((len != (uint16_t)k_npf_len_small) ||
      !internal_match(s_tx, s_rx, (uint16_t)k_npf_len_small)) {
    return k_ra8_err_invalid_arg;
  }

  /* One slot is free again, so the previously refused send now fits. */
  internal_pattern(s_tx, (uint16_t)k_npf_len_small, (uint8_t)k_npf_fill_probe);
  const ra8_err_t retry_err = ra8_net_pal_send_frame(s_tx, (uint16_t)k_npf_len_small);
  if (retry_err != k_ra8_ok) {
    return retry_err;
  }

  /* Drain the measured depth exactly: one fewer than `depth` frames from the
   * fill loop are left, plus the frame the retry pushed. */
  for (uint32_t i = 0U; i < depth; ++i) {
    len                 = (uint16_t)k_ra8_net_pal_frame_max;
    const ra8_err_t err = ra8_net_pal_recv_frame(s_rx, &len);
    if (err != k_ra8_ok) {
      return err;
    }
  }
  len = (uint16_t)k_ra8_net_pal_frame_max;
  return (ra8_net_pal_recv_frame(s_rx, &len) == k_ra8_err_no_data) ? k_ra8_ok
                                                                  : k_ra8_err_invalid_arg;
}

/**
 * @brief Leg 5: malformed send and receive arguments are refused.
 *
 * @details Each probe is checked against the exact documented code, and the
 *          leg then drains the ring to prove no refused call moved a slot.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok              Every probe reported its code; ring untouched.
 * @retval k_ra8_err_invalid_arg A probe was accepted, answered differently, or
 *                               left a frame in the ring.
 * @pre The PAL is initialised and its ring is empty.
 * @post The ring is still empty.
 * @since 0.1.0
 */
static ra8_err_t internal_check_guards(void)
{
  internal_pattern(s_tx, (uint16_t)k_npf_len_small, (uint8_t)k_npf_fill_probe);

  if (ra8_net_pal_send_frame(nullptr, (uint16_t)k_npf_len_small) != k_ra8_err_null_ptr) {
    return k_ra8_err_invalid_arg;
  }
  if (ra8_net_pal_send_frame(s_tx, (uint16_t)k_npf_len_zero) != k_ra8_err_invalid_arg) {
    return k_ra8_err_invalid_arg;
  }
  if (ra8_net_pal_send_frame(s_tx, (uint16_t)((uint32_t)k_ra8_net_pal_frame_max + 1U)) !=
      k_ra8_err_invalid_arg) {
    return k_ra8_err_invalid_arg;
  }

  uint16_t len = (uint16_t)k_ra8_net_pal_frame_max;
  if (ra8_net_pal_recv_frame(nullptr, &len) != k_ra8_err_null_ptr) {
    return k_ra8_err_invalid_arg;
  }
  if (ra8_net_pal_recv_frame(s_rx, nullptr) != k_ra8_err_null_ptr) {
    return k_ra8_err_invalid_arg;
  }

  /* A buffer that cannot hold a maximum-length frame is refused outright,
   * rather than the PAL truncating a frame into it. */
  uint16_t small_cap = (uint16_t)((uint32_t)k_ra8_net_pal_frame_max - 1U);
  if (ra8_net_pal_recv_frame(s_rx, &small_cap) != k_ra8_err_invalid_arg) {
    return k_ra8_err_invalid_arg;
  }

  if (ra8_net_pal_link_status(nullptr) != k_ra8_err_null_ptr) {
    return k_ra8_err_invalid_arg;
  }

  /* The leg enters with an empty ring, so a ring still reporting no_data is
   * what proves no refused probe pushed or popped a slot. Checked rather than
   * asserted in prose. */
  len = (uint16_t)k_ra8_net_pal_frame_max;
  return (ra8_net_pal_recv_frame(s_rx, &len) == k_ra8_err_no_data) ? k_ra8_ok
                                                                  : k_ra8_err_invalid_arg;
}

/**
 * @brief Leg 6: the event handler sees one `tx_done` per accepted send.
 *
 * @details Installs the handler, sends ::k_npf_expect_txok frames, checks the
 *          count and the mask, then detaches and shows a further send is
 *          silent. Refused sends must not raise an event.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok              Deliveries matched the accepted-send count.
 * @retval k_ra8_err_invalid_arg A delivery was missing, extra, or misreported.
 * @retval (other)               The failing PAL call's code.
 * @pre The PAL is initialised and its ring is empty.
 * @post No handler is installed and the ring is empty.
 * @since 0.1.0
 */
static ra8_err_t internal_check_events(void)
{
  s_event_mask  = 0U;
  s_event_count = 0U;
  s_event_ctx   = nullptr;

  const ra8_err_t attach_err = ra8_net_pal_set_event_handler(internal_on_event, &s_uart);
  if (attach_err != k_ra8_ok) {
    return attach_err;
  }

  internal_pattern(s_tx, (uint16_t)k_npf_len_small, (uint8_t)k_npf_fill_a);
  for (uint32_t i = 0U; i < (uint32_t)k_npf_expect_txok; ++i) {
    const ra8_err_t err = ra8_net_pal_send_frame(s_tx, (uint16_t)k_npf_len_small);
    if (err != k_ra8_ok) {
      return err;
    }
  }

  /* A refused send must not look like a completed transmit. */
  if (ra8_net_pal_send_frame(s_tx, (uint16_t)k_npf_len_zero) != k_ra8_err_invalid_arg) {
    return k_ra8_err_invalid_arg;
  }

  if (s_event_count != (uint32_t)k_npf_expect_txok) {
    return k_ra8_err_invalid_arg;
  }
  if ((s_event_mask & (uint32_t)k_ra8_net_pal_event_tx_done) == 0U) {
    return k_ra8_err_invalid_arg;
  }
  if (s_event_ctx != (volatile void*)&s_uart) {
    return k_ra8_err_invalid_arg;
  }

  const ra8_err_t detach_err = ra8_net_pal_set_event_handler(nullptr, nullptr);
  if (detach_err != k_ra8_ok) {
    return detach_err;
  }

  const uint32_t before = s_event_count;
  uint16_t       len    = (uint16_t)k_ra8_net_pal_frame_max;
  const ra8_err_t rx_err = ra8_net_pal_recv_frame(s_rx, &len);
  if (rx_err != k_ra8_ok) {
    return rx_err;
  }
  const ra8_err_t tx_err = ra8_net_pal_send_frame(s_tx, (uint16_t)k_npf_len_small);
  if (tx_err != k_ra8_ok) {
    return tx_err;
  }
  if (s_event_count != before) {
    return k_ra8_err_invalid_arg;
  }

  for (uint32_t i = 0U; i < (uint32_t)k_npf_expect_txok; ++i) {
    len                 = (uint16_t)k_ra8_net_pal_frame_max;
    const ra8_err_t err = ra8_net_pal_recv_frame(s_rx, &len);
    if (err != k_ra8_ok) {
      return err;
    }
  }
  return k_ra8_ok;
}

/**
 * @brief Leg 7: after deinit the whole surface reports invalid state.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok              Deinit released the PAL and every call refused.
 * @retval k_ra8_err_invalid_arg A call still worked on a released PAL.
 * @retval (other)               The failing deinit code.
 * @pre The PAL is initialised.
 * @post The PAL is released and the ESWM gate is dropped.
 * @since 0.1.0
 */
static ra8_err_t internal_check_unwind(void)
{
  const ra8_err_t deinit_err = ra8_net_pal_deinit();
  if (deinit_err != k_ra8_ok) {
    return deinit_err;
  }

  ra8_net_pal_mac_t        mac  = {};
  ra8_net_pal_link_state_t link = k_ra8_net_pal_link_down;
  uint16_t                 len  = (uint16_t)k_ra8_net_pal_frame_max;

  internal_pattern(s_tx, (uint16_t)k_npf_len_small, (uint8_t)k_npf_fill_probe);

  if (ra8_net_pal_send_frame(s_tx, (uint16_t)k_npf_len_small) != k_ra8_err_invalid_state) {
    return k_ra8_err_invalid_arg;
  }
  if (ra8_net_pal_recv_frame(s_rx, &len) != k_ra8_err_invalid_state) {
    return k_ra8_err_invalid_arg;
  }
  if (ra8_net_pal_set_mac_addr(&k_npf_mac_a) != k_ra8_err_invalid_state) {
    return k_ra8_err_invalid_arg;
  }
  if (ra8_net_pal_get_mac_addr(&mac) != k_ra8_err_invalid_state) {
    return k_ra8_err_invalid_arg;
  }
  if (ra8_net_pal_link_status(&link) != k_ra8_err_invalid_state) {
    return k_ra8_err_invalid_arg;
  }
  if (ra8_net_pal_set_event_handler(internal_on_event, nullptr) != k_ra8_err_invalid_state) {
    return k_ra8_err_invalid_arg;
  }
  return (ra8_net_pal_deinit() == k_ra8_err_invalid_state) ? k_ra8_ok : k_ra8_err_invalid_arg;
}

/**
 * @brief Report one leg's verdict on the console.
 *
 * @param[in]     label Leg name, printed verbatim.
 * @param[in]     err   Leg result.
 * @param[in,out] pass  Cleared when @p err is not ::k_ra8_ok.
 * @return void
 * @post One verdict line is queued on the console.
 * @since 0.1.0
 */
static void internal_verdict(const char* label, ra8_err_t err, bool* pass)
{
  internal_print("net_pal_frame_demo: ");
  internal_print(label);

  if (err == k_ra8_ok) {
    internal_print(" PASS\r\n");
    return;
  }

  internal_print(" FAIL\r\n");
  if (pass != nullptr) {
    *pass = false;
  }
}

/**
 * @brief Bring up the clocks and the module-stop controller the PAL needs.
 *
 * @details ::ra8_net_pal_init calls `ra8_eth_init`, which releases the ESWM
 *          module-stop gate, so the MSTP controller must be initialised first
 *          (the PAL documents exactly this precondition).
 *
 * @return ra8_err_t Error code from the first failing bring-up call.
 * @retval k_ra8_ok CGC and MSTP are up.
 * @retval (other)  The failing call's code.
 * @pre Running after Reset_Handler with `.data` / `.bss` initialised.
 * @post On success the ESWM gate can be released by the PAL.
 * @note Not thread-safe; boot context only.
 * @since 0.1.0
 */
static ra8_err_t internal_setup(void)
{
  const ra8_err_t cgc_err = ra8_cgc_init();
  if (cgc_err != k_ra8_ok) {
    return cgc_err;
  }
  return ra8_mstp_init();
}

/**
 * @brief Entry point: drive the ethernet PAL's stack-facing contract.
 *
 * @return void
 * @pre SystemInit configured VTOR / FPU / priority grouping.
 * @post A verdict per leg and a final summary are queued on SCI8.
 * @post The PAL is released and control parks in an infinite loop.
 * @note Single-threaded; no wire traffic and no PHY access is attempted.
 * @since 0.1.0
 */
void main(void)
{
  ra8_log_init();
  (void)ra8_io_stream_uart_init(&s_uart, &s_uart_state, (uint8_t)k_npf_uart_chan);
  (void)ra8_io_log_attach(&s_uart);
  internal_print("net_pal_frame_demo: boot\r\n");

  bool pass = true;

  const ra8_err_t setup_err = internal_setup();
  internal_verdict("setup", setup_err, &pass);

  if (setup_err == k_ra8_ok) {
    const ra8_err_t bind_err = internal_check_bind();
    internal_verdict("bind", bind_err, &pass);

    if (bind_err == k_ra8_ok) {
      internal_verdict("mac", internal_check_mac(), &pass);
      internal_verdict("ring", internal_check_ring(), &pass);
      internal_verdict("backpressure", internal_check_backpressure(), &pass);
      internal_verdict("guards", internal_check_guards(), &pass);
      internal_verdict("events", internal_check_events(), &pass);
      internal_verdict("unwind", internal_check_unwind(), &pass);
    }
  }

  internal_print(pass ? "net_pal_frame_demo: ALL PASS\r\n" : "net_pal_frame_demo: ALL FAIL\r\n");

  (void)ra8_sci_flush((uint8_t)k_npf_uart_chan);
  while (true) {
  }
}
