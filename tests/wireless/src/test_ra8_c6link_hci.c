/**
 * @file test_ra8_c6link_hci.c
 * @brief Unit tests for the HCI channel on the C6 link (#493).
 *
 * @details
 * The channel is three pieces: the guard that decides which H4 packets it
 * carries, the split that moves the indicator octet into the frame header
 * before the checksum is taken, and the reassembly that restores it on the
 * receive side. Each is tested here against buffers, with a stub transport
 * that records what the pump clocked out rather than a co-processor model:
 * the point of these vectors is the wire shape, and the shape is decided
 * entirely on this side.
 *
 * The frame layout every assertion below reads is upstream's, not this
 * library's: `spi_drv.c` copies `payload[0]` into `payload_header
 * ->hci_pkt_type`, drops the declared length by one, copies from `payload[1]`,
 * and only then checksums header plus payload.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>
#include <string.h>

#include "esp_hosted_interface.h"
#include "ra8_attributes.h"
#include "ra8_c6link.h"
#include "ra8_c6link_hci.h"
#include "ra8_c6link_internal.h"
#include "ra8_err.h"
#include "unity_minimal.h"

/**
 * @enum t_hci_const_t
 * @brief Fixture sizes, header offsets and packet literals these tests use.
 */
typedef enum : uint16_t {
  k_t_arena_bytes  = 2048U, /**< Arena the fixture link is opened with.        */
  k_t_hdr_if       = 0U,    /**< Header offset: if_type / if_num nibbles.      */
  k_t_hdr_len_lo   = 2U,    /**< Header offset: declared length, low octet.    */
  k_t_hdr_off_lo   = 4U,    /**< Header offset: payload offset, low octet.     */
  k_t_hdr_csum_lo  = 6U,    /**< Header offset: checksum, low octet.           */
  k_t_hdr_pkt_type = 11U,   /**< Header offset: the trailing union octet.      */
  k_t_reset_len    = 4U,    /**< Octets in the HCI_Reset H4 packet.            */
  k_t_evt_len      = 7U,    /**< Octets in the Command Complete H4 packet.     */
  k_t_acl_len      = 9U,    /**< Octets in the ACL H4 packet.                  */
  k_t_scratch      = 32U,   /**< Reassembly destination, generously sized.     */
  k_t_poison       = 0xA5U, /**< Byte a destination is scrubbed with first.    */
  k_t_bad_pkt_type = 0x03U, /**< Synchronous data: a type this channel refuses. */
} t_hci_const_t;

/** @brief Arena backing the fixture link. */
static uint8_t s_arena[(size_t)k_t_arena_bytes];

/** @brief The fixture link. Static because it carries two 1600-byte frames. */
static ra8_c6link_t s_link;

/** @brief The first frame the stub transport clocked since a reset. */
static uint8_t s_clocked[(size_t)k_ra8_c6link_frame_bytes];

/** @brief Transactions the stub transport clocked since a reset. */
static uint16_t s_clocked_count;

/**
 * @brief Frames carrying `ESP_HCI_IF` the stub transport clocked.
 * @details A pump runs its whole transaction budget, so a staged payload is
 * followed by filler frames; counting the HCI ones separately is what proves
 * the packet was transmitted once rather than repeated.
 */
static uint16_t s_hci_frames;

/** @brief Packets the HCI sink received, most recent last. */
static uint8_t s_sunk[(size_t)k_t_scratch];

/** @brief Octets in the most recent sunk packet. */
static uint16_t s_sunk_len;

/** @brief Times the HCI sink was called since a reset. */
static uint16_t s_sunk_count;

/**
 * @brief Record one full-duplex transfer and hand back an idle frame.
 * @details The receive direction is deliberately empty: these vectors read
 *          what the host transmitted, and a synthesised response would only
 *          add a second thing that could fail.
 * @param[in] ctx Unused transport context.
 * @param[in] tx Frame the link sealed.
 * @param[out] rx Frame handed back to the link.
 * @param[in] len Octets in each direction.
 * @return Result code.
 * @retval k_ra8_ok Always; the stub cannot fail.
 * @pre @p tx and @p rx address @p len octets.
 * @pre The fixture cleared the capture first.
 * @post The first transmitted frame is in ::s_clocked and the counts advanced.
 * @post @p rx is an all-zero frame, which classifies as malformed, not data.
 * @note File-local test stub; no ownership escapes.
 * @since Version 0.1.0
 */
RA8_INTERNAL static ra8_err_t
internal_t_transfer(void* ctx, const uint8_t* tx, uint8_t* rx, uint16_t len)
{
  (void)ctx;
  for (uint16_t i = 0U; i < len; i++) {
    if (s_clocked_count == 0U) {
      s_clocked[i] = tx[i];
    }
    rx[i] = 0U;
  }
  if ((len > 0U) && ((uint8_t)(tx[0] & 0x0FU) == (uint8_t)ESP_HCI_IF)) {
    s_hci_frames++;
  }
  s_clocked_count++;
  return k_ra8_ok;
}

/**
 * @brief Report HANDSHAKE as armed so a staged frame is always clocked.
 * @param[in] ctx Unused transport context.
 * @return Whether the co-processor is ready.
 * @retval true Always.
 * @pre None.
 * @pre None.
 * @post No state is modified.
 * @post No state is modified.
 * @note File-local test stub.
 * @since Version 0.1.0
 */
RA8_INTERNAL static bool internal_t_handshake(void* ctx)
{
  (void)ctx;
  return true;
}

/**
 * @brief Consume a delay without spending wall time.
 * @param[in] ctx Unused transport context.
 * @param[in] ms Requested delay, ignored.
 * @pre None.
 * @pre None.
 * @post No state is modified.
 * @post No state is modified.
 * @note File-local test stub.
 * @since Version 0.1.0
 */
RA8_INTERNAL static void internal_t_delay(void* ctx, uint16_t ms)
{
  (void)ctx;
  (void)ms;
}

/**
 * @brief Record one reassembled HCI packet delivered to the sink.
 * @param[in] ctx Unused sink context.
 * @param[in] packet Reassembled H4 packet.
 * @param[in] len Octets in @p packet.
 * @pre @p packet addresses @p len readable octets.
 * @pre @p len fits ::k_t_scratch.
 * @post ::s_sunk holds the packet and ::s_sunk_count advanced.
 * @post No other fixture state changes.
 * @note File-local test sink.
 * @since Version 0.1.0
 */
RA8_INTERNAL static void internal_t_sink(void* ctx, const uint8_t* packet, uint16_t len)
{
  (void)ctx;
  s_sunk_len = len;
  for (uint16_t i = 0U; (i < len) && (i < (uint16_t)k_t_scratch); i++) {
    s_sunk[i] = packet[i];
  }
  s_sunk_count++;
}

/**
 * @brief Open the fixture link over the stub transport.
 * @details Clears every capture so each block of vectors starts from a known
 *          state, and re-opens the link because the send guard is stateful.
 * @pre The link is closed, or has never been opened.
 * @pre No pump is running.
 * @post The link is open and every capture is empty.
 * @post The HCI sink is not yet attached.
 * @note File-local fixture helper.
 * @since Version 0.1.0
 */
RA8_INTERNAL static void internal_t_open(void)
{
  (void)memset(&s_link, 0, sizeof s_link);
  (void)memset(s_clocked, 0, sizeof s_clocked);
  (void)memset(s_sunk, 0, sizeof s_sunk);
  s_clocked_count = 0U;
  s_hci_frames    = 0U;
  s_sunk_len      = 0U;
  s_sunk_count    = 0U;

  const ra8_c6link_cfg_t cfg = {
    .transport   = {.ctx              = nullptr,
                    .transfer         = internal_t_transfer,
                    .handshake_active = internal_t_handshake,
                    .delay_ms         = internal_t_delay},
    .arena       = s_arena,
    .arena_bytes = (uint32_t)sizeof s_arena,
  };
  TEST_ASSERT_EQ(k_ra8_ok, ra8_c6link_open(&s_link, &cfg));
}

/**
 * @brief The indicator octet travels in the header, not the payload.
 * @details Pins every field of the transmitted frame for HCI_Reset
 *          (OGF 0x03 OCF 0x003, Bluetooth Core 5.3 Vol 4 Part E 7.3.2): the
 *          interface nibble, the declared length of one less than the packet,
 *          the payload's first octet being the opcode rather than the
 *          indicator, and the indicator sitting in the header's union field.
 * @pre The fixture link is closed.
 * @pre No pump is running.
 * @post The link is left open with nothing staged.
 * @post The capture holds exactly one clocked frame.
 * @note Bounded fixture state only.
 * @since Version 0.1.0
 */
RA8_INTERNAL static void internal_test_hci_send_shape(void)
{
  TEST_BEGIN("c6link hci send frame shape");
  internal_t_open();

  /* HCI_Reset: indicator 0x01, opcode 0x0C03 little-endian, no parameters. */
  const uint8_t reset[(size_t)k_t_reset_len] = {0x01U, 0x03U, 0x0CU, 0x00U};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_c6link_hci_send(&s_link, reset, (uint16_t)k_t_reset_len));
  /* The pump spends its whole transaction budget, so filler frames follow the
     staged one; exactly one of them carries HCI, and it is the first. */
  TEST_ASSERT(s_clocked_count >= 1U);
  TEST_ASSERT_EQ(1, s_hci_frames);

  TEST_ASSERT_EQ((uint8_t)ESP_HCI_IF, s_clocked[(size_t)k_t_hdr_if]);
  /* Declared length is the packet minus the octet the header absorbed. */
  TEST_ASSERT_EQ((uint16_t)k_t_reset_len - 1U, s_clocked[(size_t)k_t_hdr_len_lo]);
  TEST_ASSERT_EQ((uint8_t)k_ra8_c6link_header_bytes, s_clocked[(size_t)k_t_hdr_off_lo]);
  TEST_ASSERT_EQ(0x01U, s_clocked[(size_t)k_t_hdr_pkt_type]);
  /* Payload starts at the opcode, so the indicator is not duplicated. */
  TEST_ASSERT_EQ(0x03U, s_clocked[(size_t)k_ra8_c6link_header_bytes]);
  TEST_ASSERT_EQ(0x0CU, s_clocked[(size_t)k_ra8_c6link_header_bytes + 1U]);
  TEST_ASSERT_EQ(0x00U, s_clocked[(size_t)k_ra8_c6link_header_bytes + 2U]);
  /* Nothing past the payload, so the slave sees no stale tail. */
  TEST_ASSERT_EQ(0U, s_clocked[(size_t)k_ra8_c6link_header_bytes + 3U]);

  TEST_ASSERT_EQ(k_ra8_ok, ra8_c6link_close(&s_link));
  TEST_END("c6link hci send frame shape");
}

/**
 * @brief The packet-type octet is inside the checksummed span.
 * @details Reseals the captured frame twice through the internal sealer, once
 *          with the indicator and once with zero, and asserts the checksums
 *          differ. That is the fact the send contract rests on: an indicator
 *          patched into a sealed frame would fail the slave's verification.
 * @pre The fixture link is closed.
 * @pre No pump is running.
 * @post Only file-local buffers are written.
 * @post The link is left open with nothing staged.
 * @note Bounded fixture state only.
 * @since Version 0.1.0
 */
RA8_INTERNAL static void internal_test_hci_csum_covers_pkt_type(void)
{
  TEST_BEGIN("c6link hci packet type is checksummed");
  static uint8_t frame_typed[(size_t)k_ra8_c6link_frame_bytes];
  static uint8_t frame_zero[(size_t)k_ra8_c6link_frame_bytes];
  (void)memset(frame_typed, (int)k_t_poison, sizeof frame_typed);
  (void)memset(frame_zero, (int)k_t_poison, sizeof frame_zero);

  const uint16_t body = 3U;
  for (uint16_t i = 0U; i < body; i++) {
    frame_typed[(uint16_t)k_ra8_c6link_header_bytes + i] = (uint8_t)(i + 1U);
    frame_zero[(uint16_t)k_ra8_c6link_header_bytes + i]  = (uint8_t)(i + 1U);
  }
  priv_c6link_frame_seal_typed(frame_typed, (uint8_t)ESP_HCI_IF, 0U, body, 0x01U);
  priv_c6link_frame_seal_typed(frame_zero, (uint8_t)ESP_HCI_IF, 0U, body, 0x00U);

  TEST_ASSERT_EQ(0x01U, frame_typed[(size_t)k_t_hdr_pkt_type]);
  TEST_ASSERT_EQ(0x00U, frame_zero[(size_t)k_t_hdr_pkt_type]);
  TEST_ASSERT(frame_typed[(size_t)k_t_hdr_csum_lo] != frame_zero[(size_t)k_t_hdr_csum_lo]);
  /* The zero-typed seal is what the plain sealer produces, byte for byte. */
  static uint8_t frame_plain[(size_t)k_ra8_c6link_frame_bytes];
  (void)memset(frame_plain, (int)k_t_poison, sizeof frame_plain);
  for (uint16_t i = 0U; i < body; i++) {
    frame_plain[(uint16_t)k_ra8_c6link_header_bytes + i] = (uint8_t)(i + 1U);
  }
  priv_c6link_frame_seal(frame_plain, (uint8_t)ESP_HCI_IF, 0U, body);
  TEST_ASSERT_EQ(0, memcmp(frame_plain, frame_zero, (size_t)k_ra8_c6link_frame_bytes));
  TEST_END("c6link hci packet type is checksummed");
}

/**
 * @brief Every refusal the send guard owes its caller.
 * @details One vector per rejected condition: null link, null packet, a
 *          closed link, a packet below the H4 minimum, a packet above what one
 *          frame carries, a body too short for its own packet type, and a
 *          packet type this channel does not carry. Each must leave nothing
 *          staged and clock nothing.
 * @pre The fixture link is closed.
 * @pre No pump is running.
 * @post The link is left open with nothing staged.
 * @post No frame was clocked by any refused call.
 * @note Bounded fixture state only.
 * @since Version 0.1.0
 */
RA8_INTERNAL static void internal_test_hci_send_refusals(void)
{
  TEST_BEGIN("c6link hci send refusals");
  const uint8_t reset[(size_t)k_t_reset_len] = {0x01U, 0x03U, 0x0CU, 0x00U};

  /* Closed link, before any open. */
  (void)memset(&s_link, 0, sizeof s_link);
  TEST_ASSERT_EQ(k_ra8_err_not_initialized,
                 ra8_c6link_hci_send(&s_link, reset, (uint16_t)k_t_reset_len));

  internal_t_open();
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_c6link_hci_send(nullptr, reset, (uint16_t)k_t_reset_len));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_c6link_hci_send(&s_link, nullptr, (uint16_t)k_t_reset_len));
  TEST_ASSERT_EQ(k_ra8_err_invalid_size,
                 ra8_c6link_hci_send(&s_link,
                                     reset,
                                     (uint16_t)((uint16_t)k_ra8_c6link_hci_min_packet - 1U)));
  TEST_ASSERT_EQ(k_ra8_err_invalid_size,
                 ra8_c6link_hci_send(&s_link,
                                     reset,
                                     (uint16_t)((uint16_t)k_ra8_c6link_hci_max_packet + 1U)));

  /* A command needs three body octets; this offers two. */
  const uint8_t short_cmd[3] = {0x01U, 0x03U, 0x0CU};
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_c6link_hci_send(&s_link, short_cmd, 3U));

  /* Synchronous data: a real H4 indicator, but not one this channel carries. */
  const uint8_t sco[(size_t)k_t_reset_len] = {
    (uint8_t)k_t_bad_pkt_type, 0x01U, 0x00U, 0x00U};
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_c6link_hci_send(&s_link, sco, (uint16_t)k_t_reset_len));

  /* Every refusal returns before staging, so nothing was ever clocked. */
  TEST_ASSERT_EQ(0, s_clocked_count);
  TEST_ASSERT_EQ(0, s_hci_frames);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_c6link_close(&s_link));
  TEST_END("c6link hci send refusals");
}

/**
 * @brief Reassembly restores exactly the packet the controller sent.
 * @details Takes a Command Complete for HCI_Reset apart the way the wire
 *          carries it -- indicator in the header, the rest as payload -- and
 *          asserts the reassembly is byte-identical to the original packet,
 *          for an event and for an ACL packet.
 * @pre No fixture state is required.
 * @pre No pump is running.
 * @post Only file-local buffers are written.
 * @post No link state is touched.
 * @note Pure-function vectors.
 * @since Version 0.1.0
 */
RA8_INTERNAL static void internal_test_hci_reassemble(void)
{
  TEST_BEGIN("c6link hci reassembly round trip");
  static uint8_t out[(size_t)k_t_scratch];

  /* Command Complete, one outstanding command, HCI_Reset, status success. */
  const uint8_t evt[(size_t)k_t_evt_len] = {
    0x04U, 0x0EU, 0x04U, 0x01U, 0x03U, 0x0CU, 0x00U};
  (void)memset(out, (int)k_t_poison, sizeof out);
  TEST_ASSERT_EQ((uint16_t)k_t_evt_len,
                 ra8_c6link_hci_reassemble(evt[0],
                                           &evt[1],
                                           (uint16_t)((uint16_t)k_t_evt_len - 1U),
                                           out,
                                           (uint16_t)k_t_scratch));
  TEST_ASSERT_EQ(0, memcmp(out, evt, (size_t)k_t_evt_len));
  TEST_ASSERT_EQ((uint8_t)k_t_poison, out[(size_t)k_t_evt_len]);

  const uint8_t acl[(size_t)k_t_acl_len] = {
    0x02U, 0x01U, 0x20U, 0x04U, 0x00U, 0xDEU, 0xADU, 0xBEU, 0xEFU};
  (void)memset(out, (int)k_t_poison, sizeof out);
  TEST_ASSERT_EQ((uint16_t)k_t_acl_len,
                 ra8_c6link_hci_reassemble(acl[0],
                                           &acl[1],
                                           (uint16_t)((uint16_t)k_t_acl_len - 1U),
                                           out,
                                           (uint16_t)k_t_scratch));
  TEST_ASSERT_EQ(0, memcmp(out, acl, (size_t)k_t_acl_len));
  TEST_END("c6link hci reassembly round trip");
}

/**
 * @brief Every refusal the reassembly owes its caller.
 * @details Null destination, an indicator this channel does not carry, a
 *          payload shorter than the packet type's own header, a null payload,
 *          and a destination one octet too small. Each must write nothing.
 * @pre No fixture state is required.
 * @pre No pump is running.
 * @post The destination is untouched by every refused call.
 * @post No link state is touched.
 * @note Pure-function vectors.
 * @since Version 0.1.0
 */
RA8_INTERNAL static void internal_test_hci_reassemble_refusals(void)
{
  TEST_BEGIN("c6link hci reassembly refusals");
  static uint8_t out[(size_t)k_t_scratch];
  const uint8_t  body[3] = {0x0EU, 0x04U, 0x01U};

  (void)memset(out, (int)k_t_poison, sizeof out);
  TEST_ASSERT_EQ(0, ra8_c6link_hci_reassemble(0x04U, body, 3U, nullptr, (uint16_t)k_t_scratch));
  TEST_ASSERT_EQ(
    0, ra8_c6link_hci_reassemble((uint8_t)k_t_bad_pkt_type, body, 3U, out, (uint16_t)k_t_scratch));
  /* An event needs two body octets; this offers one. */
  TEST_ASSERT_EQ(0, ra8_c6link_hci_reassemble(0x04U, body, 1U, out, (uint16_t)k_t_scratch));
  TEST_ASSERT_EQ(0, ra8_c6link_hci_reassemble(0x04U, nullptr, 3U, out, (uint16_t)k_t_scratch));
  /* Three body octets plus the indicator need four; offer three. */
  TEST_ASSERT_EQ(0, ra8_c6link_hci_reassemble(0x04U, body, 3U, out, 3U));
  TEST_ASSERT_EQ((uint8_t)k_t_poison, out[0]);
  TEST_END("c6link hci reassembly refusals");
}

/**
 * @brief A received HCI frame reaches the attached sink, reassembled.
 * @details Builds a real `ESP_HCI_IF` frame with the internal sealer, drops it
 *          into the link's receive transaction, and drives the dispatch the
 *          pump uses. Asserts the sink saw the whole H4 packet with its
 *          indicator restored, that the counter advanced, and that an
 *          unattached link counts the frame instead of calling through a null.
 * @pre The fixture link is closed.
 * @pre No pump is running.
 * @post The link is left open with nothing staged.
 * @post The sink capture holds the delivered packet.
 * @note Bounded fixture state only.
 * @since Version 0.1.0
 */
RA8_INTERNAL static void internal_test_hci_receive_dispatch(void)
{
  TEST_BEGIN("c6link hci receive dispatch");
  internal_t_open();

  const uint8_t evt[(size_t)k_t_evt_len] = {
    0x04U, 0x0EU, 0x04U, 0x01U, 0x03U, 0x0CU, 0x00U};
  const uint16_t body = (uint16_t)((uint16_t)k_t_evt_len - 1U);
  for (uint16_t i = 0U; i < body; i++) {
    s_link.rx[(uint16_t)k_ra8_c6link_header_bytes + i] = evt[i + 1U];
  }
  priv_c6link_frame_seal_typed(s_link.rx, (uint8_t)ESP_HCI_IF, 0U, body, evt[0]);

  ra8_c6link_rx_view_t           view = {};
  const ra8_c6link_frame_class_t cls  = priv_c6link_frame_classify(s_link.rx, &view);
  TEST_ASSERT_EQ(k_ra8_c6link_frame_data, cls);
  TEST_ASSERT_EQ((uint8_t)ESP_HCI_IF, view.if_type);
  TEST_ASSERT_EQ(evt[0], view.pkt_type);

  ra8_c6link_stats_t stats = {};
  s_link.stats             = &stats;

  /* No sink yet: counted, not delivered, and no null call. */
  TEST_ASSERT(!priv_c6link_dispatch(&s_link, &view));
  TEST_ASSERT_EQ(1, stats.hci_in);
  TEST_ASSERT_EQ(0, s_sunk_count);

  TEST_ASSERT_EQ(k_ra8_ok, ra8_c6link_hci_attach(&s_link, internal_t_sink, nullptr));
  TEST_ASSERT(!priv_c6link_dispatch(&s_link, &view));
  TEST_ASSERT_EQ(2, stats.hci_in);
  TEST_ASSERT_EQ(1, s_sunk_count);
  TEST_ASSERT_EQ((uint16_t)k_t_evt_len, s_sunk_len);
  TEST_ASSERT_EQ(0, memcmp(s_sunk, evt, (size_t)k_t_evt_len));
  TEST_ASSERT_EQ(0, stats.hci_dropped);

  s_link.stats = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_c6link_close(&s_link));
  TEST_END("c6link hci receive dispatch");
}

/**
 * @brief A frame the channel cannot reassemble is dropped and counted.
 * @details Seals a frame whose header claims synchronous data, which is a real
 *          H4 indicator the C6's BLE-only controller never sends. The sink
 *          must not be called and both counters must reflect it.
 * @pre The fixture link is closed.
 * @pre No pump is running.
 * @post The link is left open with nothing staged.
 * @post The sink capture is unchanged by the refused frame.
 * @note Bounded fixture state only.
 * @since Version 0.1.0
 */
RA8_INTERNAL static void internal_test_hci_receive_dropped(void)
{
  TEST_BEGIN("c6link hci receive drop");
  internal_t_open();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_c6link_hci_attach(&s_link, internal_t_sink, nullptr));

  const uint16_t body = 3U;
  for (uint16_t i = 0U; i < body; i++) {
    s_link.rx[(uint16_t)k_ra8_c6link_header_bytes + i] = (uint8_t)(i + 1U);
  }
  priv_c6link_frame_seal_typed(
    s_link.rx, (uint8_t)ESP_HCI_IF, 0U, body, (uint8_t)k_t_bad_pkt_type);

  ra8_c6link_rx_view_t view = {};
  TEST_ASSERT_EQ(k_ra8_c6link_frame_data, priv_c6link_frame_classify(s_link.rx, &view));

  ra8_c6link_stats_t stats = {};
  s_link.stats             = &stats;
  TEST_ASSERT(!priv_c6link_dispatch(&s_link, &view));
  TEST_ASSERT_EQ(1, stats.hci_in);
  TEST_ASSERT_EQ(1, stats.hci_dropped);
  TEST_ASSERT_EQ(0, s_sunk_count);

  s_link.stats = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_c6link_close(&s_link));
  TEST_END("c6link hci receive drop");
}

/**
 * @brief Attach refuses what it cannot record, and detaching is allowed.
 * @details Null link and closed link are the two refusals; a null callback on
 *          an open link is a detach, not an error, because a caller shutting
 *          its host stack down needs a way to stop delivery without closing
 *          the whole link.
 * @pre The fixture link is closed.
 * @pre No pump is running.
 * @post The link is left open with no sink attached.
 * @post No frame is clocked.
 * @note Bounded fixture state only.
 * @since Version 0.1.0
 */
RA8_INTERNAL static void internal_test_hci_attach_guards(void)
{
  TEST_BEGIN("c6link hci attach guards");
  (void)memset(&s_link, 0, sizeof s_link);
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_c6link_hci_attach(nullptr, internal_t_sink, nullptr));
  TEST_ASSERT_EQ(k_ra8_err_not_initialized,
                 ra8_c6link_hci_attach(&s_link, internal_t_sink, nullptr));

  internal_t_open();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_c6link_hci_attach(&s_link, internal_t_sink, nullptr));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_c6link_hci_attach(&s_link, nullptr, nullptr));
  TEST_ASSERT_EQ(0, s_clocked_count);
  TEST_ASSERT_EQ(0, s_hci_frames);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_c6link_close(&s_link));
  TEST_END("c6link hci attach guards");
}

/**
 * @brief The channel's own geometry agrees with the frame's.
 * @details One octet of every H4 packet rides in the header, so the channel's
 *          maximum packet must be exactly one more than the frame's maximum
 *          payload. Pinned because the two constants live in different
 *          headers and a future frame-size change would otherwise drift them.
 * @pre No fixture state is required.
 * @pre No pump is running.
 * @post No state is modified.
 * @post No link state is touched.
 * @note Compile-time facts asserted at run time for the report.
 * @since Version 0.1.0
 */
RA8_INTERNAL static void internal_test_hci_geometry(void)
{
  TEST_BEGIN("c6link hci geometry");
  TEST_ASSERT_EQ((uint16_t)k_ra8_c6link_max_payload + 1U,
                 (uint16_t)k_ra8_c6link_hci_max_packet);
  TEST_ASSERT_EQ(3U, (uint16_t)k_ra8_c6link_hci_min_packet);
  TEST_ASSERT_EQ(0x01U, (uint8_t)k_ra8_c6link_hci_cmd);
  TEST_ASSERT_EQ(0x02U, (uint8_t)k_ra8_c6link_hci_acl);
  TEST_ASSERT_EQ(0x04U, (uint8_t)k_ra8_c6link_hci_evt);
  /* The header octet the indicator lands in is the last of the twelve. */
  TEST_ASSERT_EQ((uint16_t)k_ra8_c6link_header_bytes - 1U, (uint16_t)k_t_hdr_pkt_type);
  TEST_END("c6link hci geometry");
}

int main(void)
{
  internal_test_hci_geometry();
  internal_test_hci_send_shape();
  internal_test_hci_csum_covers_pkt_type();
  internal_test_hci_send_refusals();
  internal_test_hci_reassemble();
  internal_test_hci_reassemble_refusals();
  internal_test_hci_receive_dispatch();
  internal_test_hci_receive_dropped();
  internal_test_hci_attach_guards();
  return 0;
}
