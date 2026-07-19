/**
 * @file test_ra8_usb_host_bulk_cov.c
 * @brief Coverage unit tests for the USB host-mode bulk engine
 *        (libs/ra8_hal/src/ra8_usb_host_bulk.c).
 *
 * @details
 * Drives the polled host-mode bulk-transfer engine through its public
 * entry points (``ra8_usb_host_set_target``, ``ra8_usb_host_pipe_setup``,
 * ``ra8_usb_host_bulk_out`` / ``ra8_usb_host_bulk_in``, and
 * ``ra8_usb_host_line_state``) against the RAM-backed peripheral window
 * installed by ``ra8_sim_mmap``. Because the host simulator models the
 * USB register block as plain memory, the transfer state machine is
 * advanced deterministically by pre-seeding the BRDY/BEMP status words,
 * the CFIFOCTR.DTLN field, and the selected-pipe PIPEMAXP register
 * before each call, so the receive loop, the short-packet / overflow /
 * zero-length-packet drain paths, and the bounded-wait timeout leg are
 * all reached without any asynchronous injection (no SIGALRM).
 *
 * One source line is marked GCOVR_EXCL_LINE in the module because the
 * host cannot present the register transition that reaches it: the
 * STALL return in ``internal_host_wait_pipe`` (the SIE raises
 * PIPECTR.PID[1] asynchronously; both callers force PID=BUF before the
 * synchronous spin so the RAM sim never shows STALL). The FRDY-timeout
 * return in ``internal_host_bulk_rx_packet`` is reached by arming the
 * ra8_sim_mmio fault seam on CFIFOCTR, which ``internal_wait_frdy``
 * consults on every poll of its real bounded loop.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>

#include "ra8_err.h"
#include "ra8_sim_mmap.h"
#include "ra8_sim_mmio.h"
#include "ra8_usb.h"
#include "ra8_usb_internal.h"
#include "ra8_usb_regs.h"
#include "unity_minimal.h"

/**
 * @enum t_bulk_t
 * @brief Payload tail bytes and the received-count seed.
 */
typedef enum : uint16_t {
  k_t_payload_b2 = 0x30U,   /**< Byte 2 of the bulk-OUT payload.             */
  k_t_payload_b3 = 0x40U,   /**< Byte 3; distinct from b2 so a transposition
                                 in the FIFO write is visible.                 */
  k_t_got_unset  = 0xFFFFU, /**< Pre-set transferred-byte count; a transfer
                                 that fails must leave it.                      */
} t_bulk_t;

/**
 * @enum thb_const_t
 * @brief Named test vectors for the host bulk-engine coverage suite.
 *
 * @details Tests are exempt from the magic-number gate; these names keep
 * the intent of each seeded argument obvious at the call site.
 */
typedef enum : uint16_t {
  k_thb_pipe        = 1U,      /**< Valid data pipe (1..9).                  */
  k_thb_pipe_zero   = 0U,      /**< Rejected: pipe 0 is the DCP.             */
  k_thb_pipe_hi     = 99U,     /**< Rejected: pipe > k_ra8_usb_max_pipe_num. */
  k_thb_dev_addr    = 5U,      /**< Valid device address slot.               */
  k_thb_dev_addr_hi = 200U,    /**< Rejected: addr > k_ra8_usb_dev_addr_max. */
  k_thb_ep          = 1U,      /**< Valid endpoint number (1..15).           */
  k_thb_ep_zero     = 0U,      /**< Rejected: endpoint 0.                    */
  k_thb_ep_hi       = 99U,     /**< Rejected: ep > k_ra8_pipecfg_epnum_mask. */
  k_thb_mps         = 64U,     /**< Common FS bulk max packet size.          */
  k_thb_mps_zero    = 0U,      /**< Rejected: max packet 0.                  */
  k_thb_mps_hi      = 9999U,   /**< Rejected: mps > k_ra8_usb_pipemaxp_mxps. */
  k_thb_speed_bogus = 9U,      /**< Not FS, not HS.                          */
  k_thb_dtln_short  = 8U,      /**< Short packet (< mps) DTLN.               */
  k_thb_room_small  = 4U,      /**< Destination smaller than the packet.     */
  k_thb_len_ok      = 4U,      /**< In-range bulk-OUT length.                */
  k_thb_len_too_big = 2000U,   /**< Exceeds k_ra8_usb_pipe_max_packet.       */
  k_thb_lnst_j      = 1U,      /**< SYSSTS0.LNST J-state sample.             */
  k_thb_cfifo_seed  = 0xBBAAU, /**< Thb cfifo seed.                          */
} thb_const_t;

/**
 * @brief Reset the simulated peripheral window before each test.
 *
 * @details Clears every mapped register region so a prior test's writes
 * cannot leak into the next; the host bulk engine needs no MSTP or
 * device-init bring-up because it only touches the USB register block.
 */
static void prep(void)
{
  ra8_sim_mmap_reset();
  ra8_sim_mmio_reset();
}

/** @brief Pipe-bit helper matching (1 << pipe_num) used by the module. */
static uint16_t thb_pipe_bit(uint8_t pipe_num)
{
  return (uint16_t)(1U << pipe_num);
}

/**
 * @test test_set_target_rejects_and_applies
 *
 * @par MC/DC:
 * (no compound decisions in this test -- ``ra8_usb_host_set_target``
 * uses only single-condition guards; each is exercised on its own,
 * no ``&&`` or ``||`` in the code under test that this case touches)
 */
static void test_set_target_rejects_and_applies(void)
{
  TEST_BEGIN("ra8_usb_host_set_target rejects bad speed / addr, applies DEVSEL");
  prep();

  /* Bogus speed -> internal_pick returns nullptr. */
  TEST_ASSERT_EQ(
    k_ra8_err_invalid_arg,
    ra8_usb_host_set_target((ra8_usb_speed_t)k_thb_speed_bogus, (uint8_t)k_thb_dev_addr));
  /* Device address past the DEVADDn range. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_usb_host_set_target(k_ra8_usb_speed_fs, (uint8_t)k_thb_dev_addr_hi));

  /* Happy path: DCPMAXP.DEVSEL loaded with the address, MXPS preserved. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_host_set_target(k_ra8_usb_speed_fs, (uint8_t)k_thb_dev_addr));
  volatile r_usb_regs_t* reg = ra8_usb_fs();
  TEST_ASSERT_EQ(
    k_thb_dev_addr,
    ((reg->DCPMAXP >> (uint16_t)k_ra8_usb_devsel_shift) & (uint16_t)k_ra8_usb_devsel_field_mask));

  TEST_END("ra8_usb_host_set_target rejects bad speed / addr, applies DEVSEL");
}

/**
 * @brief Thin wrapper over `ra8_usb_host_pipe_setup` (direction fixed OUT).
 * @param[in] speed Bus speed selector.
 * @param[in] pipe  Pipe number.
 * @param[in] dev   Device address.
 * @param[in] ep    Endpoint number.
 * @param[in] mps   Max packet size.
 * @return The `ra8_usb_host_pipe_setup` result code.
 * @pre The host controller mock is prepared.
 * @post No state beyond the pipe register bank is modified.
 * @note Not thread-safe; single-threaded host-test helper.
 * @since 0.1.0
 */
static ra8_err_t
thb_setup(ra8_usb_speed_t speed, uint8_t pipe, uint8_t dev, uint8_t ep, uint16_t mps)
{
  return ra8_usb_host_pipe_setup(speed, pipe, dev, ep, false, mps);
}

/**
 * @test test_pipe_setup_arg_validation
 *
 * @par MC/DC:
 * (no compound decisions in this test -- ``internal_host_pipe_args_ok``
 * splits every range check into its own single-condition ``if``; each
 * rejection is driven in isolation with the other arguments valid, so
 * there is no ``&&`` or ``||`` decision to vectorize)
 */
static void test_pipe_setup_arg_validation(void)
{
  TEST_BEGIN("ra8_usb_host_pipe_setup validates every argument field");
  prep();

  const uint8_t  pipe = (uint8_t)k_thb_pipe;
  const uint8_t  dev  = (uint8_t)k_thb_dev_addr;
  const uint8_t  ep   = (uint8_t)k_thb_ep;
  const uint16_t mps  = (uint16_t)k_thb_mps;

  /* Bogus speed -> internal_pick returns nullptr. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 thb_setup((ra8_usb_speed_t)k_thb_speed_bogus, pipe, dev, ep, mps));
  /* pipe_num == 0. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 thb_setup(k_ra8_usb_speed_fs, (uint8_t)k_thb_pipe_zero, dev, ep, mps));
  /* pipe_num > max. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 thb_setup(k_ra8_usb_speed_fs, (uint8_t)k_thb_pipe_hi, dev, ep, mps));
  /* dev_addr > max. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 thb_setup(k_ra8_usb_speed_fs, pipe, (uint8_t)k_thb_dev_addr_hi, ep, mps));
  /* ep_num == 0. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 thb_setup(k_ra8_usb_speed_fs, pipe, dev, (uint8_t)k_thb_ep_zero, mps));
  /* ep_num > mask. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 thb_setup(k_ra8_usb_speed_fs, pipe, dev, (uint8_t)k_thb_ep_hi, mps));
  /* max_packet == 0. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 thb_setup(k_ra8_usb_speed_fs, pipe, dev, ep, (uint16_t)k_thb_mps_zero));
  /* max_packet > mxps. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 thb_setup(k_ra8_usb_speed_fs, pipe, dev, ep, (uint16_t)k_thb_mps_hi));

  TEST_END("ra8_usb_host_pipe_setup validates every argument field");
}

/**
 * @test test_pipe_setup_happy_path
 *
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the configuration
 * body and its register writes; no ``&&`` or ``||`` in the code under
 * test that this case touches)
 */
static void test_pipe_setup_happy_path(void)
{
  TEST_BEGIN("ra8_usb_host_pipe_setup configures a bulk pipe and clears status");
  prep();

  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_usb_host_pipe_setup(k_ra8_usb_speed_fs,
                                         (uint8_t)k_thb_pipe,
                                         (uint8_t)k_thb_dev_addr,
                                         (uint8_t)k_thb_ep,
                                         true,
                                         (uint16_t)k_thb_mps));

  volatile r_usb_regs_t* reg = ra8_usb_fs();
  /* The pipe window is deselected (PIPESEL = 0) before returning. */
  TEST_ASSERT_EQ(0U, reg->PIPESEL);
  /* PIPECFG carries the endpoint number and the bulk type field. */
  TEST_ASSERT_EQ(k_thb_ep, (reg->PIPECFG & (uint16_t)k_ra8_pipecfg_epnum_mask));
  TEST_ASSERT((reg->PIPECFG & (uint16_t)k_ra8_pipecfg_type_mask) ==
              (uint16_t)k_ra8_pipecfg_type_bulk);
  /* PIPEMAXP encodes DEVSEL(dev_addr) in the high nibble and MPS low. */
  TEST_ASSERT_EQ(k_thb_mps, (reg->PIPEMAXP & (uint16_t)k_ra8_usb_pipemaxp_mxps));
  /* The pipe's status bits were acknowledged (cleared for pipe_num). */
  const uint16_t pipe_bit = thb_pipe_bit((uint8_t)k_thb_pipe);
  TEST_ASSERT_EQ(0U, (reg->BRDYSTS & pipe_bit));
  TEST_ASSERT_EQ(0U, (reg->BEMPSTS & pipe_bit));

  TEST_END("ra8_usb_host_pipe_setup configures a bulk pipe and clears status");
}

/**
 * @test test_bulk_out_arg_rejects
 *
 * @par MC/DC:
 * (no compound decisions in this test -- ``ra8_usb_host_bulk_out`` guards
 * are single-condition; the queue rejection is driven by an out-of-range
 * length, no ``&&`` or ``||`` in the code under test that this case
 * touches)
 */
static void test_bulk_out_arg_rejects(void)
{
  TEST_BEGIN("ra8_usb_host_bulk_out rejects bad pointer / speed / pipe / length");
  prep();

  uint8_t data[k_thb_len_ok] = {1U, 2U, 3U, 4U};

  /* Null payload. */
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_usb_host_bulk_out(k_ra8_usb_speed_fs,
                                       (uint8_t)k_thb_pipe,
                                       nullptr,
                                       (uint16_t)k_thb_len_ok));
  /* Bogus speed. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_usb_host_bulk_out((ra8_usb_speed_t)k_thb_speed_bogus,
                                       (uint8_t)k_thb_pipe,
                                       data,
                                       (uint16_t)k_thb_len_ok));
  /* pipe_num == 0. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_usb_host_bulk_out(k_ra8_usb_speed_fs,
                                       (uint8_t)k_thb_pipe_zero,
                                       data,
                                       (uint16_t)k_thb_len_ok));
  /* pipe_num > max. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_usb_host_bulk_out(k_ra8_usb_speed_fs,
                                       (uint8_t)k_thb_pipe_hi,
                                       data,
                                       (uint16_t)k_thb_len_ok));
  /* Valid guards but ra8_usb_queue_in rejects the oversize length: this
   * reaches the ``qerr != k_ra8_ok`` early return after BEMP is armed. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_usb_host_bulk_out(k_ra8_usb_speed_fs,
                                       (uint8_t)k_thb_pipe,
                                       data,
                                       (uint16_t)k_thb_len_too_big));

  TEST_END("ra8_usb_host_bulk_out rejects bad pointer / speed / pipe / length");
}

/**
 * @test test_bulk_out_wait_timeout
 *
 * @par MC/DC:
 * (no compound decisions in this test -- drives the bounded status wait
 * to its timeout; ``internal_host_wait_pipe`` uses separate
 * single-condition ``if`` checks, no ``&&`` or ``||`` in the code under
 * test that this case touches)
 */
static void test_bulk_out_wait_timeout(void)
{
  TEST_BEGIN("ra8_usb_host_bulk_out arms BEMP then times out with no BEMP event");
  prep();

  uint8_t data[k_thb_len_ok] = {0x10U,
                                0x20U,
                                k_t_payload_b2,
                                k_t_payload_b3};

  /* The sim cannot re-assert BEMPSTS after the engine clears it, so the
   * bounded spin runs to the poll limit and reports a hardware timeout. */
  TEST_ASSERT_EQ(
    k_ra8_err_hw_timeout,
    ra8_usb_host_bulk_out(k_ra8_usb_speed_fs, (uint8_t)k_thb_pipe, data, (uint16_t)k_thb_len_ok));

  /* BEMP was enabled for the pipe before the wait. */
  volatile r_usb_regs_t* reg      = ra8_usb_fs();
  const uint16_t         pipe_bit = thb_pipe_bit((uint8_t)k_thb_pipe);
  TEST_ASSERT((reg->BEMPENB & pipe_bit) != 0U);

  TEST_END("ra8_usb_host_bulk_out arms BEMP then times out with no BEMP event");
}

/**
 * @test test_bulk_in_arg_rejects
 *
 * @par MC/DC:
 * (no compound decisions in this test -- every ``ra8_usb_host_bulk_in``
 * guard is single-condition; the unconfigured-pipe path is reached via
 * PIPEMAXP == 0, no ``&&`` or ``||`` in the code under test that this
 * case touches)
 */
static void test_bulk_in_arg_rejects(void)
{
  TEST_BEGIN("ra8_usb_host_bulk_in rejects bad pointers / speed / pipe / unconfigured pipe");
  prep();

  uint8_t  buf[k_thb_mps] = {};
  uint16_t got            = 0U;

  /* Null destination buffer. */
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_usb_host_bulk_in(k_ra8_usb_speed_fs,
                                      (uint8_t)k_thb_pipe,
                                      nullptr,
                                      (uint16_t)k_thb_mps,
                                      &got));
  /* Null out-count. */
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_usb_host_bulk_in(k_ra8_usb_speed_fs,
                                      (uint8_t)k_thb_pipe,
                                      buf,
                                      (uint16_t)k_thb_mps,
                                      nullptr));
  /* Bogus speed. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_usb_host_bulk_in((ra8_usb_speed_t)k_thb_speed_bogus,
                                      (uint8_t)k_thb_pipe,
                                      buf,
                                      (uint16_t)k_thb_mps,
                                      &got));
  /* pipe_num == 0. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_usb_host_bulk_in(k_ra8_usb_speed_fs,
                                      (uint8_t)k_thb_pipe_zero,
                                      buf,
                                      (uint16_t)k_thb_mps,
                                      &got));
  /* pipe_num > max. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_usb_host_bulk_in(k_ra8_usb_speed_fs,
                                      (uint8_t)k_thb_pipe_hi,
                                      buf,
                                      (uint16_t)k_thb_mps,
                                      &got));
  /* PIPEMAXP reads back 0 (pipe never configured) -> invalid_state. */
  TEST_ASSERT_EQ(
    k_ra8_err_invalid_state,
    ra8_usb_host_bulk_in(k_ra8_usb_speed_fs, (uint8_t)k_thb_pipe, buf, (uint16_t)k_thb_mps, &got));

  TEST_END("ra8_usb_host_bulk_in rejects bad pointers / speed / pipe / unconfigured pipe");
}

/**
 * @test test_bulk_in_short_packet
 *
 * @par MC/DC:
 * (no compound decisions in this test -- the receive loop and its drain
 * helper use single-condition ``if`` checks; the short-packet exit is
 * driven by DTLN < mps, no ``&&`` or ``||`` in the code under test that
 * this case touches)
 */
static void test_bulk_in_short_packet(void)
{
  TEST_BEGIN("ra8_usb_host_bulk_in consumes one short packet and ends the transfer");
  prep();

  volatile r_usb_regs_t* reg      = ra8_usb_fs();
  const uint16_t         pipe_bit = thb_pipe_bit((uint8_t)k_thb_pipe);
  reg->PIPEMAXP                   = (uint16_t)k_thb_mps;        /* mps = 64          */
  reg->BRDYSTS                    = pipe_bit;                   /* pending BRDY edge */
  reg->CFIFOCTR                   = (uint16_t)k_thb_dtln_short; /* DTLN = 8 (< mps)  */
  reg->CFIFO                      = (uint16_t)k_thb_cfifo_seed;

  uint8_t  buf[k_thb_mps] = {};
  uint16_t got            = 0U;
  TEST_ASSERT_EQ(
    k_ra8_ok,
    ra8_usb_host_bulk_in(k_ra8_usb_speed_fs, (uint8_t)k_thb_pipe, buf, (uint16_t)k_thb_mps, &got));
  TEST_ASSERT_EQ(k_thb_dtln_short, got);
  /* The engine acknowledged the BRDY edge for the pipe (cleared it). */
  TEST_ASSERT_EQ(0U, (reg->BRDYSTS & pipe_bit));

  TEST_END("ra8_usb_host_bulk_in consumes one short packet and ends the transfer");
}

/**
 * @test test_bulk_in_overflow_clamps
 *
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the copy-clamp and
 * remainder-drop drain branches, each a single-condition ``if``; no
 * ``&&`` or ``||`` in the code under test that this case touches)
 */
static void test_bulk_in_overflow_clamps(void)
{
  TEST_BEGIN("ra8_usb_host_bulk_in clamps a packet larger than the destination");
  prep();

  volatile r_usb_regs_t* reg      = ra8_usb_fs();
  const uint16_t         pipe_bit = thb_pipe_bit((uint8_t)k_thb_pipe);
  reg->PIPEMAXP                   = (uint16_t)k_thb_mps;
  reg->BRDYSTS                    = pipe_bit;
  reg->CFIFOCTR                   = (uint16_t)k_thb_dtln_short; /* device sent 8 bytes */
  reg->CFIFO                      = (uint16_t)k_thb_cfifo_seed;

  uint8_t  buf[k_thb_room_small] = {}; /* room for only 4 */
  uint16_t got                   = 0U;
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_usb_host_bulk_in(k_ra8_usb_speed_fs,
                                      (uint8_t)k_thb_pipe,
                                      buf,
                                      (uint16_t)k_thb_room_small,
                                      &got));
  /* Only room-many bytes are copied; the remainder is dropped with BCLR. */
  TEST_ASSERT_EQ(k_thb_room_small, got);

  TEST_END("ra8_usb_host_bulk_in clamps a packet larger than the destination");
}

/**
 * @test test_bulk_in_zero_length_packet
 *
 * @par MC/DC:
 * (no compound decisions in this test -- drives the zero-length-packet
 * release branch, a single-condition ``if (dtln == 0U)``; no ``&&`` or
 * ``||`` in the code under test that this case touches)
 */
static void test_bulk_in_zero_length_packet(void)
{
  TEST_BEGIN("ra8_usb_host_bulk_in releases a zero-length packet and ends");
  prep();

  volatile r_usb_regs_t* reg      = ra8_usb_fs();
  const uint16_t         pipe_bit = thb_pipe_bit((uint8_t)k_thb_pipe);
  reg->PIPEMAXP                   = (uint16_t)k_thb_mps;
  reg->BRDYSTS                    = pipe_bit;
  reg->CFIFOCTR                   = 0U; /* DTLN = 0 -> ZLP */

  uint8_t  buf[k_thb_mps] = {};
  uint16_t got            = 0U;
  TEST_ASSERT_EQ(
    k_ra8_ok,
    ra8_usb_host_bulk_in(k_ra8_usb_speed_fs, (uint8_t)k_thb_pipe, buf, (uint16_t)k_thb_mps, &got));
  TEST_ASSERT_EQ(0U, got);
  /* The buffer was released via BCLR. */
  TEST_ASSERT((reg->CFIFOCTR & (uint16_t)k_ra8_fifoctr_bclr) != 0U);

  TEST_END("ra8_usb_host_bulk_in releases a zero-length packet and ends");
}

/**
 * @test test_bulk_in_wait_timeout
 *
 * @par MC/DC:
 * (no compound decisions in this test -- with no pending BRDY the drain
 * helper's bounded wait times out and the loop's single-condition error
 * check ends the transfer; no ``&&`` or ``||`` in the code under test
 * that this case touches)
 */
static void test_bulk_in_wait_timeout(void)
{
  TEST_BEGIN("ra8_usb_host_bulk_in times out when no packet ever arrives");
  prep();

  volatile r_usb_regs_t* reg = ra8_usb_fs();
  reg->PIPEMAXP              = (uint16_t)k_thb_mps;
  reg->BRDYSTS               = 0U; /* no pending BRDY, PIPECTR stays BUF */

  uint8_t  buf[k_thb_mps] = {};
  uint16_t got            = 0U;
  TEST_ASSERT_EQ(
    k_ra8_err_hw_timeout,
    ra8_usb_host_bulk_in(k_ra8_usb_speed_fs, (uint8_t)k_thb_pipe, buf, (uint16_t)k_thb_mps, &got));
  TEST_ASSERT_EQ(0U, got);

  TEST_END("ra8_usb_host_bulk_in times out when no packet ever arrives");
}

/**
 * @test test_bulk_in_fills_buffer
 *
 * @par MC/DC:
 * (no compound decisions in this test -- ends the transfer on the
 * byte-count branch ``if (rx >= max_len)`` with a full packet, a
 * single-condition ``if``; no ``&&`` or ``||`` in the code under test
 * that this case touches)
 */
static void test_bulk_in_fills_buffer(void)
{
  TEST_BEGIN("ra8_usb_host_bulk_in ends when the destination buffer fills");
  prep();

  volatile r_usb_regs_t* reg      = ra8_usb_fs();
  const uint16_t         pipe_bit = thb_pipe_bit((uint8_t)k_thb_pipe);
  /* mps == max_len == 4: a full (non-short) packet exactly fills buf, so
   * the loop ends on the rx >= max_len branch rather than on dtln < mps. */
  reg->PIPEMAXP = (uint16_t)k_thb_room_small;
  reg->BRDYSTS  = pipe_bit;
  reg->CFIFOCTR = (uint16_t)k_thb_room_small; /* DTLN = 4 == mps */
  reg->CFIFO    = (uint16_t)k_thb_cfifo_seed;

  uint8_t  buf[k_thb_room_small] = {};
  uint16_t got                   = 0U;
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_usb_host_bulk_in(k_ra8_usb_speed_fs,
                                      (uint8_t)k_thb_pipe,
                                      buf,
                                      (uint16_t)k_thb_room_small,
                                      &got));
  TEST_ASSERT_EQ(k_thb_room_small, got);

  TEST_END("ra8_usb_host_bulk_in ends when the destination buffer fills");
}

/**
 * @test test_line_state
 *
 * @par MC/DC:
 * (no compound decisions in this test -- ``ra8_usb_host_line_state`` has a
 * single nullptr guard and a pure read; no ``&&`` or ``||`` in the code
 * under test that this case touches)
 */
static void test_line_state(void)
{
  TEST_BEGIN("ra8_usb_host_line_state returns 0 for bad speed, LNST otherwise");
  prep();

  /* Bogus speed -> 0. */
  TEST_ASSERT_EQ(0U, ra8_usb_host_line_state((ra8_usb_speed_t)k_thb_speed_bogus));

  /* Valid speed: reads SYSSTS0.LNST[1:0]. */
  volatile r_usb_regs_t* reg = ra8_usb_fs();
  reg->SYSSTS0               = (uint16_t)k_thb_lnst_j;
  TEST_ASSERT_EQ(k_thb_lnst_j, ra8_usb_host_line_state(k_ra8_usb_speed_fs));

  TEST_END("ra8_usb_host_line_state returns 0 for bad speed, LNST otherwise");
}

/**
 * @test test_bulk_in_frdy_timeout
 *
 * @par MC/DC:
 * (no compound decisions in this test -- the FRDY poll in
 * ``internal_wait_frdy`` is a single-condition loop exit; the armed
 * fail drives its timeout leg through ``internal_host_bulk_rx_packet``)
 *
 * @details Pre-seeds a pending BRDY edge so the pipe wait passes, then
 * arms the ra8_sim_mmio seam on CFIFOCTR to fail so the packet drain's
 * FRDY wait runs to its budget. ``ra8_usb_host_bulk_in`` must surface
 * ``k_ra8_err_hw_timeout`` with a zero partial count and park the pipe
 * NAK -- the leg that was host-dead while ``internal_wait_frdy``
 * short-circuited under RA8_SIMULATOR_MODE.
 */
static void test_bulk_in_frdy_timeout(void)
{
  TEST_BEGIN("ra8_usb_host_bulk_in surfaces the rx-packet FRDY timeout");
  prep();

  volatile r_usb_regs_t* reg      = ra8_usb_fs();
  const uint16_t         pipe_bit = thb_pipe_bit((uint8_t)k_thb_pipe);
  reg->PIPEMAXP                   = (uint16_t)k_thb_mps; /* mps = 64          */
  reg->BRDYSTS                    = pipe_bit;            /* pending BRDY edge */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_sim_mmio_fail_wait((const volatile void*)&reg->CFIFOCTR));

  uint8_t  buf[k_thb_mps] = {};
  uint16_t got            = k_t_got_unset;
  TEST_ASSERT_EQ(
    k_ra8_err_hw_timeout,
    ra8_usb_host_bulk_in(k_ra8_usb_speed_fs, (uint8_t)k_thb_pipe, buf, (uint16_t)k_thb_mps, &got));
  /* The failing packet contributed no bytes to the partial count. */
  TEST_ASSERT_EQ(0U, got);

  TEST_END("ra8_usb_host_bulk_in surfaces the rx-packet FRDY timeout");
}

int32_t main(void)
{
  test_set_target_rejects_and_applies();
  test_pipe_setup_arg_validation();
  test_pipe_setup_happy_path();
  test_bulk_out_arg_rejects();
  test_bulk_out_wait_timeout();
  test_bulk_in_arg_rejects();
  test_bulk_in_short_packet();
  test_bulk_in_overflow_clamps();
  test_bulk_in_zero_length_packet();
  test_bulk_in_wait_timeout();
  test_bulk_in_frdy_timeout();
  test_bulk_in_fills_buffer();
  test_line_state();
  (void)fprintf(stderr, "[OK ] test_ra8_usb_host_bulk_cov.c\n");
  return 0;
}
