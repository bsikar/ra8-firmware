/**
 * @file tests/test_ra8_canfd_frame_cov.c
 * @brief Coverage-gap tests for the receive path of ra8_canfd_frame.c
 *
 * @details
 * Drives the production ``ra8_canfd_receive`` path against a directly
 * staged register file. The host build backs the CANFD register block
 * with plain mmap'd RAM, so each test stages ``CFDRF[0]`` and
 * ``CFDRFSTS[0]`` with exactly the values the silicon RX engine would
 * have latched after a frame landed in RX FIFO 0, then asserts the
 * driver's real read-and-decode plus its register side effects. (The
 * old in-driver ``RA8_OFF_TARGET`` staging veneer this file used
 * to cover was deleted by the issue-238 seam migration: the driver now
 * compiles one register sequence on every build and the staging lives
 * here, in the test.)
 *
 * What this file adds over test_ra8_canfd.c's receive cases:
 *
 *  - The full 64-byte ``DF[]`` copy loop end to end (every payload
 *    byte asserted, not just the first few).
 *  - The ``CFDRFPCTR[0]`` pointer-advance ack write (0xFF) on a
 *    successful pop, and its absence on the empty-FIFO leg.
 *  - Both arms of every header-decode ternary across two frames
 *    (extended+FD+BRS versus standard+classic).
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>

#include "ra8_canfd.h"
#include "ra8_canfd_regs.h"
#include "ra8_err.h"
#include "ra8_fake_mmap.h"
#include "unity_minimal.h"

/**
 * @enum canfd_frame_cov_fixture_t
 * @brief The byte-level helpers.
 */
typedef enum : uint8_t {
  k_byte_mask = 0xFFU, /**< Truncates a generated or shifted value back into a byte. */
} canfd_frame_cov_fixture_t;

/* -------------------------------------------------------------------------
 * Local constants (tests are exempt from the magic-number gate but named
 * values document intent and keep clang-tidy -readability-magic-numbers
 * quiet on the host build).
 * -------------------------------------------------------------------------
 */

/**
 * @enum rx_channel_t
 * @brief Channel indices used throughout this test file.
 */
typedef enum : uint8_t {
  k_rx_ch0 = 0U, /**< Valid channel 0. */
  k_rx_ch1 = 1U, /**< Valid channel 1. */
} rx_channel_t;

/**
 * @enum rx_word_t
 * @brief Raw register words and lengths staged by the receive tests.
 */
typedef enum : uint32_t {
  k_rx_ext_id      = 0x1F0ABCDU,  /**< 29-bit extended ID payload bits.       */
  k_rx_std_id      = 0x7FFU,      /**< Max 11-bit standard ID (no IDE bit).   */
  k_rx_ptr_dlc15   = 0xF0000000U, /**< DLC=15 packed into PTR bits [31:28].   */
  k_rx_ptr_dlc8    = 0x80000000U, /**< DLC=8 packed into PTR bits [31:28].    */
  k_rx_fdsts_fdbrs = 0x00000006U, /**< FDF | BRS set (FD frame with BRS).     */
  k_rx_fdsts_none  = 0U,          /**< Classic CAN: no FD flags.              */
  k_rx_dlc15       = 15U,         /**< Decoded DLC for k_rx_ptr_dlc15.        */
  k_rx_dlc8        = 8U,          /**< Decoded DLC for k_rx_ptr_dlc8.         */
  k_rx_len_short   = 8U,          /**< Short payload staged on the ch1 frame. */
  k_rx_byte_seed   = 0xA5U,       /**< Seed byte for the payload pattern.     */
} rx_word_t;

/* -------------------------------------------------------------------------
 * Helpers
 * -------------------------------------------------------------------------
 */

/**
 * @brief Fill `buf[0..len-1]` with a deterministic byte sequence.
 *
 * @param[out] buf   Destination buffer.
 * @param[in]  len   Number of bytes to fill.
 * @pre  buf != nullptr.
 * @post buf[i] == (uint8_t)((k_rx_byte_seed + i) & 0xFFU) for all i < len.
 * @note Not thread-safe.
 * @since 0.1.0
 */
static void fill_pattern(uint8_t* buf, uint32_t len)
{
  for (uint32_t i = 0U; i < len; i++) {
    buf[i] = (uint8_t)(((uint32_t)k_rx_byte_seed + i) & k_byte_mask);
  }
}

/**
 * @brief Stage one received frame into the RAM-backed RX FIFO 0 block.
 *
 * @details
 * Plays the silicon RX engine's role for the host build: stamps the
 * header words into ``CFDRF[0].ID/PTR/FDSTS``, copies @p len payload
 * bytes into ``CFDRF[0].DF[]``, and clears ``CFDRFSTS[0].RFEMP`` so the
 * production ``ra8_canfd_receive`` sees a frame ready. On silicon these
 * registers are read-only latches the FIFO engine fills; on the host
 * they are plain RAM, which is exactly why the staging belongs here in
 * the test instead of inside the driver.
 *
 * @param[in] reg   Channel register block; non-NULL.
 * @param[in] id    Raw ``CFDRF[0].ID`` word (IDE bit selects extended).
 * @param[in] ptr   Raw ``CFDRF[0].PTR`` word (DLC in bits [31:28]).
 * @param[in] fdsts Raw ``CFDRF[0].FDSTS`` word (FDF/BRS flags).
 * @param[in] data  Payload bytes to stage; may be NULL when len is 0.
 * @param[in] len   Payload byte count; at most k_ra8_canfd_data_bytes_max.
 * @pre  reg != nullptr and len <= k_ra8_canfd_data_bytes_max.
 * @post CFDRFSTS[0].RFEMP is clear; the frame is ready to pop.
 * @note Not thread-safe.
 * @since 0.1.0
 */
static void stage_rx_frame(volatile r_canfd_t* reg,
                           uint32_t            id,
                           uint32_t            ptr,
                           uint32_t            fdsts,
                           const uint8_t*      data,
                           uint32_t            len)
{
  reg->CFDRF[0].ID    = id;
  reg->CFDRF[0].PTR   = ptr;
  reg->CFDRF[0].FDSTS = fdsts;
  for (uint32_t b = 0U; b < len; b++) {
    reg->CFDRF[0].DF[b] = data[b];
  }
  reg->CFDRFSTS[0] &= ~(uint32_t)k_ra8_rfsts_bit_empty;
}

/* -------------------------------------------------------------------------
 * Tests
 * -------------------------------------------------------------------------
 */

/**
 * @brief Empty RX FIFO: receive reports no_data and does not pop.
 *
 * @details
 * Stages ``CFDRFSTS[0].RFEMP`` set (the reset-time silicon state) and
 * asserts the production path returns ``k_ra8_err_no_data`` WITHOUT
 * writing the ``CFDRFPCTR[0]`` pointer-advance ack -- popping an empty
 * FIFO would underflow the silicon FIFO pointer.
 *
 * @par MC/DC:
 * Decision: `(reg->CFDRFSTS[0] & k_ra8_rfsts_bit_empty) != 0U`
 * (single condition; no compound decision).
 * - Vector 1: RFEMP set   -> true  (this test: no_data, no pop).
 * - Vector 2: RFEMP clear -> false (the two receive tests below).
 * Single-condition decisions satisfy MC/DC trivially.
 *
 * @pre ra8_fake_mmap_reset() has been called.
 * @post CFDRFPCTR[0] is still 0 (no ack write happened).
 * @note Not thread-safe.
 * @since 0.1.0
 */
static void test_receive_empty_fifo_no_data(void)
{
  TEST_BEGIN("receive: RFEMP set returns no_data without a pop");
  ra8_fake_mmap_reset();

  volatile r_canfd_t* reg = ra8_canfd((uint8_t)k_rx_ch0);
  reg->CFDRFSTS[0]        = (uint32_t)k_ra8_rfsts_bit_empty;

  ra8_canfd_frame_t out = {};
  TEST_ASSERT_EQ(k_ra8_err_no_data, ra8_canfd_receive((uint8_t)k_rx_ch0, &out));

  /* No pointer-advance ack on the empty leg. */
  TEST_ASSERT_EQ(0U, reg->CFDRFPCTR[0]);

  TEST_END("receive: RFEMP set returns no_data without a pop");
}

/**
 * @brief Full 64-byte extended FD frame: decode + payload + ack write.
 *
 * @details
 * Stages an extended-ID (IDE set) FD frame with BRS and a full 64-byte
 * payload pattern, then asserts the production receive path:
 *
 *  - decodes is_extended=1 and masks the 29-bit ID;
 *  - extracts DLC=15 from PTR bits [31:28];
 *  - decodes is_fd=1 and is_brs=1 from FDSTS;
 *  - copies every one of the 64 ``DF[]`` bytes;
 *  - acks the pop by writing 0xFF to ``CFDRFPCTR[0]``.
 *
 * @par MC/DC:
 * Decisions in internal_decode_rx_header (each a single condition):
 * - `(id_word & k_ra8_canfd_id_ide) != 0U`   -> true here (extended).
 * - `(fdsts_word & k_ra8_canfd_fd_fdf) != 0U` -> true here (FD).
 * - `(fdsts_word & k_ra8_canfd_fd_brs) != 0U` -> true here (BRS).
 * The false arms run in test_receive_standard_classic_ch1.
 * Single-condition decisions satisfy MC/DC trivially.
 *
 * @pre ra8_fake_mmap_reset() has been called.
 * @post out mirrors the staged frame; CFDRFPCTR[0] == 0xFF.
 * @note Not thread-safe.
 * @since 0.1.0
 */
static void test_receive_full_payload_ext_fd(void)
{
  TEST_BEGIN("receive: 64-byte extended FD frame decodes + acks");
  ra8_fake_mmap_reset();

  uint8_t data[k_ra8_canfd_data_bytes_max];
  fill_pattern(data, (uint32_t)k_ra8_canfd_data_bytes_max);

  volatile r_canfd_t* reg = ra8_canfd((uint8_t)k_rx_ch0);
  reg->CFDRFSTS[0]        = (uint32_t)k_ra8_rfsts_bit_empty;
  stage_rx_frame(reg,
                 (uint32_t)k_rx_ext_id | (uint32_t)k_ra8_canfd_id_ide,
                 (uint32_t)k_rx_ptr_dlc15,
                 (uint32_t)k_rx_fdsts_fdbrs,
                 data,
                 (uint32_t)k_ra8_canfd_data_bytes_max);

  ra8_canfd_frame_t out = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_canfd_receive((uint8_t)k_rx_ch0, &out));

  TEST_ASSERT_EQ(1, out.is_extended);
  TEST_ASSERT_EQ(k_rx_ext_id, out.id);
  TEST_ASSERT_EQ(k_rx_dlc15, out.dlc);
  TEST_ASSERT_EQ(1, out.is_fd);
  TEST_ASSERT_EQ(1, out.is_brs);

  /* Every byte of the 64-byte data field must have been copied. */
  for (uint32_t b = 0U; b < (uint32_t)k_ra8_canfd_data_bytes_max; b++) {
    TEST_ASSERT_EQ(data[b], out.data[b]);
  }

  /* The pop must be acked with the dummy 0xFF pointer-advance write. */
  TEST_ASSERT_EQ(k_ra8_rfpctr_value_ack, reg->CFDRFPCTR[0]);

  TEST_END("receive: 64-byte extended FD frame decodes + acks");
}

/**
 * @brief Standard-ID classic frame on channel 1: false decode arms.
 *
 * @details
 * Stages an 11-bit standard-ID classic-CAN frame (no IDE, no FDF, no
 * BRS) with DLC=8 on channel 1 and asserts the production receive path
 * decodes the standard-ID mask, DLC extraction, and the cleared FD
 * flags -- the false arm of every header-decode ternary -- and that the
 * short payload bytes land unmodified.
 *
 * @par MC/DC:
 * Decisions in internal_decode_rx_header (each a single condition):
 * - `(id_word & k_ra8_canfd_id_ide) != 0U`    -> false here (standard).
 * - `(fdsts_word & k_ra8_canfd_fd_fdf) != 0U`  -> false here (classic).
 * - `(fdsts_word & k_ra8_canfd_fd_brs) != 0U`  -> false here (no BRS).
 * The true arms run in test_receive_full_payload_ext_fd.
 * Single-condition decisions satisfy MC/DC trivially.
 *
 * @pre ra8_fake_mmap_reset() has been called.
 * @post out.id == k_rx_std_id, out.is_extended == 0, out.dlc == 8.
 * @note Not thread-safe.
 * @since 0.1.0
 */
static void test_receive_standard_classic_ch1(void)
{
  TEST_BEGIN("receive: standard classic frame on channel 1");
  ra8_fake_mmap_reset();

  uint8_t data[k_rx_len_short];
  fill_pattern(data, (uint32_t)k_rx_len_short);

  volatile r_canfd_t* reg = ra8_canfd((uint8_t)k_rx_ch1);
  reg->CFDRFSTS[0]        = (uint32_t)k_ra8_rfsts_bit_empty;
  stage_rx_frame(reg,
                 (uint32_t)k_rx_std_id,
                 (uint32_t)k_rx_ptr_dlc8,
                 (uint32_t)k_rx_fdsts_none,
                 data,
                 (uint32_t)k_rx_len_short);

  /* Staging must have cleared RFEMP so the pop can proceed. */
  TEST_ASSERT_EQ(0U, reg->CFDRFSTS[0] & (uint32_t)k_ra8_rfsts_bit_empty);

  ra8_canfd_frame_t out = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_canfd_receive((uint8_t)k_rx_ch1, &out));

  TEST_ASSERT_EQ(k_rx_std_id, out.id);
  TEST_ASSERT_EQ(0, out.is_extended);
  TEST_ASSERT_EQ(k_rx_dlc8, out.dlc);
  TEST_ASSERT_EQ(0, out.is_fd);
  TEST_ASSERT_EQ(0, out.is_brs);
  TEST_ASSERT_EQ(data[0], out.data[0]);
  TEST_ASSERT_EQ(data[k_rx_len_short - 1U], out.data[k_rx_len_short - 1U]);

  TEST_ASSERT_EQ(k_ra8_rfpctr_value_ack, reg->CFDRFPCTR[0]);

  TEST_END("receive: standard classic frame on channel 1");
}

/* -------------------------------------------------------------------------
 * Entry point
 * -------------------------------------------------------------------------
 */

/**
 * @brief Test executable entry point.
 * @return 0 on success; calls exit(1) on first assertion failure.
 * @since 0.1.0
 */
int32_t main(void)
{
  test_receive_empty_fifo_no_data();
  test_receive_full_payload_ext_fd();
  test_receive_standard_classic_ch1();
  (void)fprintf(stderr, "[OK ] test_ra8_canfd_frame_cov.c\n");
  return 0;
}
