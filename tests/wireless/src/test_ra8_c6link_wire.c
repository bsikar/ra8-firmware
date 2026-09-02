/**
 * @file test_ra8_c6link_wire.c
 * @brief Unit tests for the three pure layers under `ra8_c6link` (#490).
 *
 * @details
 * The payload header, the TLV envelope and the decode arena are pure functions
 * over buffers: no transport, no co-processor, no timing. They are tested here
 * on their own so that a failure in `test_ra8_c6link.c` -- which drives the
 * whole facade against a co-processor model -- can never be ambiguous about
 * which layer broke.
 *
 * Every decision in these layers is exercised with N+1 vectors; the blocks
 * below name them.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>
#include <string.h>

#include "ra8_attributes.h"
#include "ra8_c6link.h"
#include "ra8_c6link_internal.h"
#include "ra8_err.h"
#include "unity_minimal.h"

/**
 * @enum t_wire_const_t
 * @brief Fixture sizes and offsets these tests work to.
 */
typedef enum : uint16_t {
  k_t_arena_bytes = 2048U, /**< Arena the fixture link is opened with.             */
  k_t_payload_len = 40U,   /**< A convenient payload length for a data frame.      */
  k_t_hdr_len_lo  = 2U,    /**< Offset of the header's length, low octet.          */
  k_t_hdr_off_lo  = 4U,    /**< Offset of the header's offset, low octet.          */
  k_t_hdr_csum_lo = 6U,    /**< Offset of the header's checksum, low octet.        */
  k_t_alloc_small = 5U,    /**< An unaligned allocation size.                      */
  k_t_alloc_pad   = 8U,    /**< What ::k_t_alloc_small rounds up to.               */
  k_t_proto_len   = 7U,    /**< A stand-in protobuf body length.                   */
  k_t_ifnum_one   = 0x10U, /**< Interface number one, in the header's high nibble. */
  k_t_ep_len      = 6U,    /**< Octets in `RPCRsp`.                                */
  k_t_seq_lo      = 8U,    /**< Offset of the header's sequence number, low.       */
  k_t_pkt_type    = 11U,   /**< Offset of the header's packet-type octet.          */
  k_t_odd_extra   = 5U,    /**< Makes the second arena's size non-aligned.         */
  k_t_odd_slack   = 8U,    /**< Shortfall that leaves room for the rounding.       */
  k_t_poison      = 0xAAU, /**< Byte a buffer is scrubbed with before a build.     */
  k_t_big_len_lo  = 0x40U, /**< Low octet of 1600, a length no frame can carry.    */
  k_t_big_len_hi  = 0x06U, /**< Its high octet.                                    */
  k_t_mac_first   = 1U,    /**< First octet of the address the copy helper reads.  */
} t_wire_const_t;

/** @brief Arena backing the fixture link. */
static uint8_t s_arena[(size_t)k_t_arena_bytes];

/**
 * @brief A second arena whose size is deliberately not a multiple of eight.
 * @details Exists so a request for every remaining octet can be made where
 * rounding it up to the allocator's alignment would run past the end -- the
 * one refusal a well-behaved bump allocator owes its caller and the one a
 * multiple-of-eight arena can never provoke.
 * @note Written only through ::priv_c6link_arena_alloc.
 * @warning Its size must stay non-aligned or the test it backs stops testing
 *          anything.
 * @since 0.1.0
 */
static uint8_t s_odd_arena[(size_t)k_t_arena_bytes + (size_t)k_t_odd_extra];

/** @brief The fixture link; only its arena and buffers are used here. */
static ra8_c6link_t s_link;

/** @brief One transaction buffer under test. */
static uint8_t s_frame[(size_t)k_ra8_c6link_frame_bytes];

/**
 * @brief Transport row that never moves bytes; these tests never pump.
 * @param[in] ctx Unused.
 * @param[in] tx Unused.
 * @param[out] rx Unused.
 * @param[in] len Unused.
 * @return Always `k_ra8_ok`. @details Implements the t no transfer fixture operation used only by this focused test executable. @retval k_ra8_ok The fixture operation completed successfully. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static ra8_err_t
internal_t_no_transfer(void* ctx, const uint8_t* tx, uint8_t* rx, uint16_t len)
{
  (void)ctx;
  (void)tx;
  (void)len;
  /* What a co-processor with nothing to say leaves behind. These tests never
     pump, but a transport row that does not write its receive buffer is not
     one. */
  priv_c6link_frame_filler(rx);
  return k_ra8_ok;
}

/**
 * @brief Handshake row that is never consulted; these tests never pump.
 * @param[in] ctx Unused.
 * @return Always false. @details Implements the t no handshake fixture operation used only by this focused test executable. @retval true The named fixture condition holds. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static bool internal_t_no_handshake(void* ctx)
{
  (void)ctx;
  return false;
}

/**
 * @brief Delay row that is never consulted; these tests never pump.
 * @param[in] ctx Unused.
 * @param[in] ms Unused.
 * @return Nothing. @details Implements the t no delay fixture operation used only by this focused test executable. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_t_no_delay(void* ctx, uint16_t ms)
{
  (void)ctx;
  (void)ms;
}

/** @brief Open the fixture link with an inert transport and a live arena. @details Implements the t open fixture operation used only by this focused test executable. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_t_open(void)
{
  s_link                         = (ra8_c6link_t){};
  ra8_c6link_cfg_t cfg           = {};
  cfg.transport.transfer         = internal_t_no_transfer;
  cfg.transport.handshake_active = internal_t_no_handshake;
  cfg.transport.delay_ms         = internal_t_no_delay;
  cfg.arena                      = s_arena;
  cfg.arena_bytes                = (uint32_t)k_t_arena_bytes;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_c6link_open(&s_link, &cfg));
}

/**
 * @par MC/DC:
 * Decision: `(link == nullptr) || (link->arena == nullptr)` (2 conditions)
 * - Vector 1: link valid, arena set     -> false (control)
 * - Vector 2: link NULL, arena n/a      -> true  (varies link only)
 * - Vector 3: link valid, arena NULL    -> true  (varies arena only)
 * Vectors 1+2 prove link independently decides; 1+3 prove the same for arena.
 * N+1 = 3 vectors for N=2: minimal MC/DC.
 * Decisions: libs/ra8_c6link/src/ra8_c6link_arena.c@priv_c6link_arena_alloc @brief Verify arena guards behavior. @details Executes the arena guards scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_test_arena_guards(void)
{
  TEST_BEGIN("c6link arena guards");
  internal_t_open();
  TEST_ASSERT_NOT_NULL(priv_c6link_arena_alloc(&s_link, (size_t)k_t_alloc_small));
  TEST_ASSERT_NULL(priv_c6link_arena_alloc(nullptr, (size_t)k_t_alloc_small));

  ra8_c6link_t bare = {};
  TEST_ASSERT_NULL(priv_c6link_arena_alloc(&bare, (size_t)k_t_alloc_small));
  TEST_END("c6link arena guards");
}

/**
 * @par MC/DC:
 * (no compound decision under test -- an over-large request is refused by the
 * single `size > avail` guard, and the aligned bump is checked by inspection of
 * two consecutive allocations)
 * Decisions: libs/ra8_c6link/src/ra8_c6link_arena.c@priv_c6link_arena_bind @brief Verify arena bump behavior. @details Executes the arena bump scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_test_arena_bump(void)
{
  TEST_BEGIN("c6link arena bump and exhaustion");
  internal_t_open();
  uint8_t* first = (uint8_t*)priv_c6link_arena_alloc(&s_link, (size_t)k_t_alloc_small);
  TEST_ASSERT_NOT_NULL(first);
  uint8_t* second = (uint8_t*)priv_c6link_arena_alloc(&s_link, (size_t)k_t_alloc_small);
  TEST_ASSERT_NOT_NULL(second);
  /* Five bytes must round up to eight, so the blocks are eight apart. */
  TEST_ASSERT_EQ(k_t_alloc_pad, (second - first));

  TEST_ASSERT_NULL(priv_c6link_arena_alloc(&s_link, (size_t)k_t_arena_bytes));
  priv_c6link_arena_reset(&s_link);
  TEST_ASSERT_NOT_NULL(priv_c6link_arena_alloc(&s_link, (size_t)k_t_arena_bytes));

  /* An arena whose size is not a multiple of the alignment: a request for
     every remaining octet fits, but rounding it up does not. The allocator
     must refuse rather than hand out a block that ends past the arena. */
  ra8_c6link_t     odd           = {};
  ra8_c6link_cfg_t cfg           = {};
  cfg.transport.transfer         = internal_t_no_transfer;
  cfg.transport.handshake_active = internal_t_no_handshake;
  cfg.transport.delay_ms         = internal_t_no_delay;
  cfg.arena                      = s_odd_arena;
  cfg.arena_bytes                = (uint32_t)sizeof s_odd_arena;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_c6link_open(&odd, &cfg));
  TEST_ASSERT_NULL(priv_c6link_arena_alloc(&odd, sizeof s_odd_arena));
  TEST_ASSERT_NOT_NULL(priv_c6link_arena_alloc(&odd, sizeof s_odd_arena - k_t_odd_slack));

  /* Every helper tolerates a null link rather than faulting on it. */
  priv_c6link_arena_reset(nullptr);
  ProtobufCAllocator alloc = {};
  priv_c6link_arena_bind(nullptr, &s_link);
  priv_c6link_arena_bind(&alloc, nullptr);
  TEST_ASSERT_NULL(alloc.alloc);
  priv_c6link_arena_bind(&alloc, &s_link);
  TEST_ASSERT_NOT_NULL(alloc.alloc);
  TEST_END("c6link arena bump and exhaustion");
}

/**
 * @par MC/DC:
 * Decision: `(link == nullptr) || (pointer == nullptr) || (arena_last == 0)`
 * (3 conditions)
 * - Vector 1: link valid, pointer valid, a live newest block -> false (control)
 * - Vector 2: link NULL                                      -> true (varies link)
 * - Vector 3: pointer NULL                                   -> true (varies pointer)
 * - Vector 4: no newest block (freed twice)                  -> true (varies last)
 * Vector 1 paired with each of 2, 3 and 4 proves the corresponding condition
 * independently decides. N+1 = 4 vectors for N=3: minimal MC/DC.
 * Decisions: libs/ra8_c6link/src/ra8_c6link_arena.c@priv_c6link_arena_free @brief Verify arena free rollback behavior. @details Executes the arena free rollback scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_test_arena_free_rollback(void)
{
  TEST_BEGIN("c6link arena LIFO rollback");
  internal_t_open();
  uint8_t* first = (uint8_t*)priv_c6link_arena_alloc(&s_link, (size_t)k_t_alloc_small);
  TEST_ASSERT_NOT_NULL(first);
  uint8_t* second = (uint8_t*)priv_c6link_arena_alloc(&s_link, (size_t)k_t_alloc_small);
  TEST_ASSERT_NOT_NULL(second);

  /* Freeing the newest block returns its space, so the next allocation reuses
     the same address. */
  priv_c6link_arena_free(&s_link, second);
  uint8_t* again = (uint8_t*)priv_c6link_arena_alloc(&s_link, (size_t)k_t_alloc_small);
  TEST_ASSERT_EQ((intptr_t)second, (intptr_t)again);

  /* Every other spelling is a no-op that must not corrupt the offset. */
  priv_c6link_arena_free(nullptr, again);
  priv_c6link_arena_free(&s_link, nullptr);
  priv_c6link_arena_free(&s_link, again);
  priv_c6link_arena_free(&s_link, again);
  uint8_t* third = (uint8_t*)priv_c6link_arena_alloc(&s_link, (size_t)k_t_alloc_small);
  TEST_ASSERT_EQ((intptr_t)again, (intptr_t)third);

  /* Freeing a block that is not the newest one is retained until a reset. */
  priv_c6link_arena_free(&s_link, first);
  uint8_t* fourth = (uint8_t*)priv_c6link_arena_alloc(&s_link, (size_t)k_t_alloc_small);
  TEST_ASSERT(fourth != first);
  TEST_END("c6link arena LIFO rollback");
}

/**
 * @par MC/DC:
 * (no compound decision under test -- the filler frame is a fixed byte pattern
 * and is compared field by field) @brief Verify frame filler behavior. @details Executes the frame filler scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_test_frame_filler(void)
{
  TEST_BEGIN("c6link filler frame");
  (void)memset(s_frame, (int32_t)k_t_poison, sizeof s_frame);
  priv_c6link_frame_filler(s_frame);

  /* `ESP_MAX_IF` in the low nibble, everything else zero. */
  TEST_ASSERT_EQ(ESP_MAX_IF, s_frame[0]);
  for (uint16_t i = 1U; i < (uint16_t)k_ra8_c6link_frame_bytes; i++) {
    TEST_ASSERT_EQ(0, s_frame[i]);
  }

  /* A filler frame reads back as idle, not as a malformed data frame: its
     offset is legitimately zero, and judging it by data-frame rules is what
     once made a healthy link report a failure. */
  ra8_c6link_rx_view_t view = {};
  TEST_ASSERT_EQ(k_ra8_c6link_frame_idle, priv_c6link_frame_classify(s_frame, &view));
  TEST_END("c6link filler frame");
}

/**
 * @par MC/DC:
 * Decision: `(offset != header_bytes) || (len > max_payload)` (2 conditions)
 * - Vector 1: offset=12, len=40   -> false (control: a real data frame)
 * - Vector 2: offset=8,  len=40   -> true  (varies offset only)
 * - Vector 3: offset=12, len=1600 -> true  (varies len only)
 * Vectors 1+2 prove offset independently decides; 1+3 prove the same for len.
 * N+1 = 3 vectors for N=2: minimal MC/DC.
 * Decisions: libs/ra8_c6link/src/ra8_c6link_frame.c@priv_c6link_frame_classify
 * Decisions: libs/ra8_c6link/src/ra8_c6link_frame.c@priv_c6link_frame_seal @brief Verify frame classify behavior. @details Executes the frame classify scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_test_frame_classify(void)
{
  TEST_BEGIN("c6link frame classification");
  ra8_c6link_rx_view_t view = {};

  for (uint16_t i = 0U; i < (uint16_t)k_t_payload_len; i++) {
    s_frame[(uint16_t)k_ra8_c6link_header_bytes + i] = (uint8_t)(i + 1U);
  }
  priv_c6link_frame_seal(s_frame, (uint8_t)ESP_SERIAL_IF, 0U, (uint16_t)k_t_payload_len);
  TEST_ASSERT_EQ(k_ra8_c6link_frame_data, priv_c6link_frame_classify(s_frame, &view));
  TEST_ASSERT_EQ(k_ra8_c6link_header_bytes, view.offset);
  TEST_ASSERT_EQ(k_t_payload_len, view.len);
  TEST_ASSERT_EQ(ESP_SERIAL_IF, view.if_type);

  s_frame[k_t_hdr_off_lo] = 8U;
  TEST_ASSERT_EQ(k_ra8_c6link_frame_malformed, priv_c6link_frame_classify(s_frame, &view));
  s_frame[k_t_hdr_off_lo] = (uint8_t)k_ra8_c6link_header_bytes;

  s_frame[k_t_hdr_len_lo]      = (uint8_t)k_t_big_len_lo;
  s_frame[k_t_hdr_len_lo + 1U] = (uint8_t)k_t_big_len_hi; /* 0x0640 = 1600 > 1588 */
  TEST_ASSERT_EQ(k_ra8_c6link_frame_malformed, priv_c6link_frame_classify(s_frame, &view));

  /* A corrupted payload octet must be caught by the checksum, not ignored. */
  priv_c6link_frame_seal(s_frame, (uint8_t)ESP_SERIAL_IF, 0U, (uint16_t)k_t_payload_len);
  s_frame[(uint16_t)k_ra8_c6link_header_bytes]++;
  TEST_ASSERT_EQ(k_ra8_c6link_frame_bad_checksum, priv_c6link_frame_classify(s_frame, &view));

  /* Classification never modifies the buffer it judges. */
  const uint16_t stated = (uint16_t)((uint16_t)s_frame[k_t_hdr_csum_lo] |
                                     ((uint16_t)s_frame[k_t_hdr_csum_lo + 1U] << 8U));
  (void)priv_c6link_frame_classify(s_frame, &view);
  TEST_ASSERT_EQ(
    stated,
    ((uint16_t)s_frame[k_t_hdr_csum_lo] | ((uint16_t)s_frame[k_t_hdr_csum_lo + 1U] << 8U)));

  TEST_ASSERT_EQ(k_ra8_c6link_frame_malformed, priv_c6link_frame_classify(nullptr, &view));
  TEST_END("c6link frame classification");
}

/**
 * @par MC/DC:
 * (no compound decision under test -- the envelope is written and read back,
 * and an insufficient capacity is refused by a single size guard)
 * Decisions: libs/ra8_c6link/src/ra8_c6link_tlv.c@priv_c6link_tlv_open @brief Verify tlv roundtrip behavior. @details Executes the tlv roundtrip scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_test_tlv_roundtrip(void)
{
  TEST_BEGIN("c6link TLV round trip");
  uint8_t  buf     = 0U;
  uint16_t body_at = 0U;
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 priv_c6link_tlv_open(nullptr, 0U, (uint16_t)k_t_proto_len, &body_at));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 priv_c6link_tlv_open(&buf, 0U, (uint16_t)k_t_proto_len, nullptr));

  uint8_t env[(size_t)k_ra8_c6link_max_payload] = {};
  TEST_ASSERT_EQ(k_ra8_err_invalid_size,
                 priv_c6link_tlv_open(env, 1U, (uint16_t)k_t_proto_len, &body_at));
  TEST_ASSERT_EQ(0, body_at);

  TEST_ASSERT_EQ(
    k_ra8_ok,
    priv_c6link_tlv_open(env, (uint16_t)sizeof env, (uint16_t)k_t_proto_len, &body_at));
  TEST_ASSERT_EQ(k_ra8_c6link_tlv_overhead, body_at);
  for (uint16_t i = 0U; i < (uint16_t)k_t_proto_len; i++) {
    env[body_at + i] = (uint8_t)(0x10U + i);
  }

  uint16_t       got = 0U;
  const uint8_t* body =
    priv_c6link_tlv_body(env, (uint16_t)(body_at + (uint16_t)k_t_proto_len), &got);
  TEST_ASSERT_NOT_NULL(body);
  TEST_ASSERT_EQ(k_t_proto_len, got);
  TEST_ASSERT_EQ(0, memcmp(body, &env[body_at], (size_t)k_t_proto_len));
  TEST_END("c6link TLV round trip");
}

/**
 * @par MC/DC:
 * Decision: `(len < overhead) || (payload[0] != epname_tag) ||
 *            (payload[data_tag] != data_tag_type)` (3 conditions)
 * - Vector 1: full length, both tags right -> false (control)
 * - Vector 2: length one octet short       -> true  (varies len only)
 * - Vector 3: first tag corrupted          -> true  (varies tag one only)
 * - Vector 4: second tag corrupted         -> true  (varies tag two only)
 * Vector 1 paired with each of 2, 3 and 4 proves the corresponding condition
 * independently decides. N+1 = 4 vectors for N=3: minimal MC/DC.
 * Decisions: libs/ra8_c6link/src/ra8_c6link_tlv.c@priv_c6link_tlv_body
 * Decisions: libs/ra8_c6link/src/ra8_c6link_tlv.c@internal_c6link_tlv_named @brief Verify tlv rejects behavior. @details Executes the tlv rejects scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_test_tlv_rejects(void)
{
  TEST_BEGIN("c6link TLV rejection");
  uint8_t  env[(size_t)k_ra8_c6link_max_payload] = {};
  uint16_t body_at                               = 0U;
  uint16_t got                                   = 0U;
  TEST_ASSERT_EQ(
    k_ra8_ok,
    priv_c6link_tlv_open(env, (uint16_t)sizeof env, (uint16_t)k_t_proto_len, &body_at));
  const uint16_t full = (uint16_t)(body_at + (uint16_t)k_t_proto_len);

  TEST_ASSERT_NULL(priv_c6link_tlv_body(nullptr, full, &got));
  TEST_ASSERT_NULL(priv_c6link_tlv_body(env, full, nullptr));
  TEST_ASSERT_NULL(
    priv_c6link_tlv_body(env, (uint16_t)((uint16_t)k_ra8_c6link_tlv_overhead - 1U), &got));

  env[k_ra8_c6link_tlv_type]++;
  TEST_ASSERT_NULL(priv_c6link_tlv_body(env, full, &got));
  env[k_ra8_c6link_tlv_type]--;

  const uint16_t data_tag =
    (uint16_t)((uint16_t)k_ra8_c6link_tlv_value + (uint16_t)k_ra8_c6link_tlv_ep_len);
  env[data_tag]++;
  TEST_ASSERT_NULL(priv_c6link_tlv_body(env, full, &got));
  env[data_tag]--;

  /* A declared endpoint-name length that is not the protocol's one. */
  env[k_ra8_c6link_tlv_len_lo]++;
  TEST_ASSERT_NULL(priv_c6link_tlv_body(env, full, &got));
  env[k_ra8_c6link_tlv_len_lo]--;

  /* A name that is neither `RPCRsp` nor `RPCEvt`. */
  env[k_ra8_c6link_tlv_value] = (uint8_t)'X';
  TEST_ASSERT_NULL(priv_c6link_tlv_body(env, full, &got));
  env[k_ra8_c6link_tlv_value] = (uint8_t)'R';

  /* A body that claims more octets than the frame carries, and one that
     claims none at all. */
  TEST_ASSERT_NULL(priv_c6link_tlv_body(env, (uint16_t)(full - 1U), &got));
  env[data_tag + (uint16_t)k_ra8_c6link_tlv_len_lo] = 0U;
  TEST_ASSERT_NULL(priv_c6link_tlv_body(env, full, &got));

  /* And the control: unmodified, it parses. */
  env[data_tag + (uint16_t)k_ra8_c6link_tlv_len_lo] = (uint8_t)k_t_proto_len;
  TEST_ASSERT_NOT_NULL(priv_c6link_tlv_body(env, full, &got));
  TEST_END("c6link TLV rejection");
}

/**
 * @par MC/DC:
 * Decision: `(src == nullptr) || (src->data == nullptr) || (src->len != 6)`
 * (3 conditions)
 * - Vector 1: field present, data set, length 6 -> false (control)
 * - Vector 2: field absent                      -> true (varies src only)
 * - Vector 3: data pointer null                 -> true (varies data only)
 * - Vector 4: length 5                          -> true (varies len only)
 * Vector 1 paired with each of 2, 3 and 4 proves the corresponding condition
 * independently decides. N+1 = 4 vectors for N=3: minimal MC/DC.
 * Decisions: libs/ra8_c6link/src/ra8_c6link.c@priv_c6link_copy_mac
 * Decisions: libs/ra8_c6link/src/ra8_c6link.c@priv_c6link_copy_str @brief Verify copy helpers behavior. @details Executes the copy helpers scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_test_copy_helpers(void)
{
  TEST_BEGIN("c6link binary-field copies");
  uint8_t raw[(size_t)k_ra8_c6link_mac_bytes] = {};
  for (uint8_t i = 0U; i < (uint8_t)k_ra8_c6link_mac_bytes; i++) {
    raw[i] = (uint8_t)((uint8_t)k_t_mac_first + i);
  }
  ProtobufCBinaryData field = {.len = (size_t)k_ra8_c6link_mac_bytes, .data = raw};
  ra8_c6link_mac_t    mac   = {};

  TEST_ASSERT(priv_c6link_copy_mac(&mac, &field));
  TEST_ASSERT_EQ(0, memcmp(mac.octet, raw, sizeof raw));
  TEST_ASSERT(!priv_c6link_copy_mac(&mac, nullptr));
  TEST_ASSERT(!priv_c6link_copy_mac(nullptr, &field));

  field.data = nullptr;
  TEST_ASSERT(!priv_c6link_copy_mac(&mac, &field));
  field.data = raw;
  field.len  = (size_t)k_ra8_c6link_mac_bytes - 1U;
  TEST_ASSERT(!priv_c6link_copy_mac(&mac, &field));
  /* A rejected field leaves the destination cleared, never half-written. */
  TEST_ASSERT_EQ(0, mac.octet[0]);

  /* Strings are truncated to the destination and always terminated, however
     long the co-processor's field was and whether or not it terminated it. */
  char    text[8] = {};
  uint8_t letters[16];
  for (uint8_t i = 0U; i < (uint8_t)sizeof letters; i++) {
    letters[i] = (uint8_t)('a' + i);
  }
  ProtobufCBinaryData wide = {.len = sizeof letters, .data = letters};
  TEST_ASSERT_EQ(sizeof text - 1, priv_c6link_copy_str(text, (uint8_t)sizeof text, &wide));
  TEST_ASSERT_EQ(0, text[sizeof text - 1U]);
  TEST_ASSERT_EQ(0, priv_c6link_copy_str(text, (uint8_t)sizeof text, nullptr));
  TEST_ASSERT_EQ(0, priv_c6link_copy_str(nullptr, (uint8_t)sizeof text, &wide));
  TEST_ASSERT_EQ(0, priv_c6link_copy_str(text, 0U, &wide));
  TEST_END("c6link binary-field copies");
}

/**
 * @par MC/DC:
 * (no compound decision under test -- the octets the host puts on the wire are
 * compared against literals taken from the protocol, not against the code that
 * produced them) @brief Verify wire literals behavior. @details Executes the wire literals scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_test_wire_literals(void)
{
  TEST_BEGIN("c6link wire octets against protocol literals");

  /* The payload header, octet by octet: interface nibbles, flags, length,
     offset, checksum, sequence number, packet type. A round trip through this
     library's own writer and reader would agree with itself even if both were
     wrong, so the bytes are checked against the protocol here. */
  (void)memset(s_frame, 0, sizeof s_frame);
  for (uint16_t i = 0U; i < (uint16_t)k_t_payload_len; i++) {
    s_frame[(uint16_t)k_ra8_c6link_header_bytes + i] = 1U;
  }
  priv_c6link_frame_seal(s_frame, (uint8_t)ESP_SERIAL_IF, 1U, (uint16_t)k_t_payload_len);
  /* `if_type` in the low nibble, `if_num` in the high one. */
  const int32_t if_byte = (int32_t)ESP_SERIAL_IF | (int32_t)k_t_ifnum_one;
  TEST_ASSERT_EQ(if_byte, s_frame[0]);
  TEST_ASSERT_EQ(0, s_frame[1]); /* flags */
  TEST_ASSERT_EQ(k_t_payload_len, s_frame[k_t_hdr_len_lo]);
  TEST_ASSERT_EQ(0, s_frame[k_t_hdr_len_lo + 1U]);
  TEST_ASSERT_EQ(k_ra8_c6link_header_bytes, s_frame[k_t_hdr_off_lo]);
  TEST_ASSERT_EQ(0, s_frame[k_t_hdr_off_lo + 1U]);
  /* The checksum is the plain 16-bit sum of every octet with the field taken
     as zero: the interface byte, the length field's low octet, the offset
     field's low octet, and forty payload octets of one. */
  const uint16_t expect =
    (uint16_t)((uint16_t)if_byte + (uint16_t)k_t_payload_len + (uint16_t)k_ra8_c6link_header_bytes +
               (uint16_t)k_t_payload_len);
  TEST_ASSERT_EQ((expect & 0xFFU), s_frame[k_t_hdr_csum_lo]);
  TEST_ASSERT_EQ((expect >> 8U), s_frame[k_t_hdr_csum_lo + 1U]);
  TEST_ASSERT_EQ(0, s_frame[k_t_seq_lo]);      /* seq_num low  */
  TEST_ASSERT_EQ(0, s_frame[k_t_seq_lo + 1U]); /* seq_num high */
  TEST_ASSERT_EQ(0, s_frame[k_t_pkt_type]);    /* packet type  */

  /* The envelope: tag 0x01, length 6 little-endian, "RPCRsp", tag 0x02, the
     protobuf length little-endian. */
  uint8_t  env[(size_t)k_ra8_c6link_max_payload] = {};
  uint16_t body_at                               = 0U;
  TEST_ASSERT_EQ(
    k_ra8_ok,
    priv_c6link_tlv_open(env, (uint16_t)sizeof env, (uint16_t)k_t_proto_len, &body_at));
  const uint16_t data_at = (uint16_t)((uint16_t)k_ra8_c6link_tlv_value + (uint16_t)k_t_ep_len);
  TEST_ASSERT_EQ(k_ra8_c6link_tlv_t_epname, env[0]);
  TEST_ASSERT_EQ(k_t_ep_len, env[1]);
  TEST_ASSERT_EQ(0, env[2]);
  TEST_ASSERT_EQ(0, memcmp(&env[k_ra8_c6link_tlv_value], "RPCRsp", (size_t)k_t_ep_len));
  TEST_ASSERT_EQ(k_ra8_c6link_tlv_t_data, env[data_at]);
  TEST_ASSERT_EQ(k_t_proto_len, env[data_at + 1U]);
  TEST_ASSERT_EQ(0, env[data_at + 2U]);
  TEST_ASSERT_EQ(k_ra8_c6link_tlv_overhead, body_at);
  TEST_END("c6link wire octets against protocol literals");
}

/**
 * @par MC/DC:
 * Decision: `(tx == nullptr) || (len > k_ra8_c6link_max_payload)` (2 conditions,
 * ::priv_c6link_frame_seal)
 * - Vector 1: tx valid, len 40           -> false (control: the frame is built)
 * - Vector 2: tx NULL, len 40            -> true  (varies tx only)
 * - Vector 3: tx valid, len max + 1      -> true  (varies len only)
 * Decision: `(rx == nullptr) || (view == nullptr)` (2 conditions,
 * ::priv_c6link_frame_classify)
 * - Vector 4: rx valid, view valid       -> false (control)
 * - Vector 5: rx NULL, view valid        -> true  (varies rx only)
 * - Vector 6: rx valid, view NULL        -> true  (varies view only)
 * Decision: `(got != RPCRsp[i]) && (got != RPCEvt[i])` (2 conditions,
 * ::internal_c6link_tlv_named)
 * - Vector 7: octet is the RPCRsp one    -> false (varies the first test)
 * - Vector 8: octet is the RPCEvt one    -> false (varies the second test)
 * - Vector 9: octet is neither           -> true  (control: both differ)
 * Each control paired with each varied vector proves that condition
 * independently decides. N+1 vectors per decision: minimal MC/DC.
 * Each control paired with each varied vector proves that condition
 * independently decides. N+1 vectors per decision: minimal MC/DC.
 * Decisions: libs/ra8_c6link/src/ra8_c6link_frame.c@priv_c6link_frame_seal
 * Decisions: libs/ra8_c6link/src/ra8_c6link_frame.c@priv_c6link_frame_classify
 * Decisions: libs/ra8_c6link/src/ra8_c6link_tlv.c@internal_c6link_tlv_named
 * Decisions: libs/ra8_c6link/src/ra8_c6link.c@priv_c6link_copy_str @brief Verify mcdc wire guards behavior. @details Executes the mcdc wire guards scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_test_mcdc_wire_guards(void)
{
  TEST_BEGIN("c6link wire guard vectors");
  ra8_c6link_rx_view_t view = {};

  /* frame_seal: neither guard, then each on its own. A refused call leaves the
     buffer alone, which is what distinguishes "refused" from "built wrong". */
  (void)memset(s_frame, 0, sizeof s_frame);
  priv_c6link_frame_seal(s_frame, (uint8_t)ESP_SERIAL_IF, 0U, (uint16_t)k_t_payload_len);
  TEST_ASSERT_EQ(ESP_SERIAL_IF, s_frame[0]);
  priv_c6link_frame_seal(nullptr, (uint8_t)ESP_SERIAL_IF, 0U, (uint16_t)k_t_payload_len);
  (void)memset(s_frame, 0, sizeof s_frame);
  priv_c6link_frame_seal(s_frame,
                         (uint8_t)ESP_SERIAL_IF,
                         0U,
                         (uint16_t)((uint16_t)k_ra8_c6link_max_payload + 1U));
  TEST_ASSERT_EQ(0, s_frame[0]);
  priv_c6link_frame_filler(nullptr);

  /* frame_classify: neither guard, then each on its own. */
  priv_c6link_frame_seal(s_frame, (uint8_t)ESP_SERIAL_IF, 0U, (uint16_t)k_t_payload_len);
  TEST_ASSERT_EQ(k_ra8_c6link_frame_data, priv_c6link_frame_classify(s_frame, &view));
  TEST_ASSERT_EQ(k_ra8_c6link_frame_malformed, priv_c6link_frame_classify(nullptr, &view));
  TEST_ASSERT_EQ(k_ra8_c6link_frame_malformed, priv_c6link_frame_classify(s_frame, nullptr));

  /* tlv_named: an envelope addressed to the EVENT endpoint. Upstream sends
     unsolicited events on `RPCEvt` and responses on `RPCRsp`, and both must
     parse -- so the name test has to accept either, one octet at a time. */
  uint8_t  env[(size_t)k_ra8_c6link_max_payload] = {};
  uint16_t body_at                               = 0U;
  uint16_t got                                   = 0U;
  TEST_ASSERT_EQ(
    k_ra8_ok,
    priv_c6link_tlv_open(env, (uint16_t)sizeof env, (uint16_t)k_t_proto_len, &body_at));
  const uint16_t full = (uint16_t)(body_at + (uint16_t)k_t_proto_len);
  for (uint16_t i = 0U; i < (uint16_t)k_t_ep_len; i++) {
    env[(uint16_t)k_ra8_c6link_tlv_value + i] = (uint8_t)"RPCEvt"[i];
  }
  TEST_ASSERT_NOT_NULL(priv_c6link_tlv_body(env, full, &got));
  TEST_ASSERT_EQ(k_t_proto_len, got);

  /* copy_str: a field that is present but carries no data pointer. */
  ProtobufCBinaryData hollow  = {.len = (size_t)k_t_ep_len, .data = nullptr};
  char                text[8] = {};
  TEST_ASSERT_EQ(0, priv_c6link_copy_str(text, (uint8_t)sizeof text, &hollow));
  TEST_ASSERT_EQ(0, text[0]);
  TEST_END("c6link wire guard vectors");
}

/**
 * @par MC/DC:
 * Decision: `(out == nullptr) || (cap < k_ra8_c6link_caps_bytes)` (2 conditions,
 * ::priv_c6link_caps)
 * - Vector 1: out valid, cap exact     -> false (control: both false)
 * - Vector 2: out NULL,  cap exact     -> true  (varies out only)
 * - Vector 3: out valid, cap one short -> true  (varies cap only)
 * Each control paired with each varied vector proves that condition
 * independently decides. N+1 = 3 vectors for N=2 conditions: minimal MC/DC.
 * Decisions: libs/ra8_c6link/src/ra8_c6link_frame.c@priv_c6link_caps @brief Verify mcdc caps guard behavior. @details Executes the mcdc caps guard scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_test_mcdc_caps_guard(void)
{
  TEST_BEGIN("c6link caps guard vectors");
  /* Neither guard, then each on its own. A refused build returns zero octets
     written, which is what the caller checks -- so the refusal is observable
     without inspecting the buffer. */
  (void)memset(s_frame, 0, sizeof s_frame);
  TEST_ASSERT_EQ(k_ra8_c6link_caps_bytes,
                 priv_c6link_caps(s_frame, (uint8_t)k_ra8_c6link_caps_bytes));
  TEST_ASSERT_EQ(ESP_PRIV_EVENT_INIT, s_frame[0]);
  TEST_ASSERT_EQ(0, priv_c6link_caps(nullptr, (uint8_t)k_ra8_c6link_caps_bytes));
  TEST_ASSERT_EQ(0, priv_c6link_caps(s_frame, (uint8_t)((uint8_t)k_ra8_c6link_caps_bytes - 1U)));
  TEST_END("c6link caps guard vectors");
}

int main(void)
{
  internal_test_arena_guards();
  internal_test_arena_bump();
  internal_test_arena_free_rollback();
  internal_test_frame_filler();
  internal_test_frame_classify();
  internal_test_tlv_roundtrip();
  internal_test_tlv_rejects();
  internal_test_wire_literals();
  internal_test_copy_helpers();
  internal_test_mcdc_wire_guards();
  internal_test_mcdc_caps_guard();
  return 0;
}
