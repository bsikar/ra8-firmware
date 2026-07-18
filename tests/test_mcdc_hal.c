/**
 * @file test_mcdc_hal.c
 * @brief MC/DC floor vectors for compound decisions in ra8_gfx / ra8_i2c /
 *        ra8_i2c_peripheral / ra8_jpeg_sw_decode (issue #190, theme T1-04).
 *
 * @details
 * Each test group closes a reachable MC/DC gap reported by the per-file floor
 * (scripts/utils/check_mcdc_floor.py) by driving the production decision to
 * full modified condition/decision coverage through the public API on the host
 * ra8_sim MMIO substrate. Every group is self-contained: it supplies the whole
 * N+1 vector set for its decision so the merged profile is complete regardless
 * of sibling test TUs.
 *
 * Covered decisions:
 *  - ra8_gfx_blit_gray8: `(src == nullptr) || (w <= 0) || (h <= 0)` and the
 *    RGB565 fast-path clip guard `(x0 < x1) && (y0 < y1)`.
 *  - internal_i2c_finish_tx: `(err != k_ra8_ok) || send_stop`.
 *  - ra8_i2c_internal_target_drain_rx: the final-byte drain guard
 *    `((icsr2 & rdrf) != 0) && (count < capacity)`.
 *  - dec_dispatch_marker: the unsupported-SOFn classifier
 *    `mk >= sof1 && mk <= sof_hi && mk != dht && mk != jpg`.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "ra8_err.h"
#include "ra8_gfx.h"
#include "ra8_i2c.h"
#include "ra8_i2c_internal.h"
#include "ra8_i2c_regs.h"
#include "ra8_jpeg_sw.h"
#include "ra8_mstp.h"
#include "ra8_sim_mmap.h"
#include "unity_minimal.h"

/* ===========================================================================
 * Group 1 -- ra8_gfx_blit_gray8 (libs/ra8_gfx/src/ra8_gfx_text.c)
 * ===========================================================================
 */

/** @brief Framebuffer + source geometry for the blit_gray8 vectors. */
typedef enum : int32_t {
  k_gfx_dim      = 64,  /**< Square RGB565 framebuffer edge in pixels. */
  k_gfx_src_dim  = 8,   /**< Gray source block edge in pixels.         */
  k_gfx_on       = 3,   /**< On-screen draw origin (x and y).          */
  k_gfx_off      = 100, /**< Origin fully past the right/bottom edge.  */
  k_gfx_bad_span = 0,   /**< Zero span -> w<=0 / h<=0 guard arm.       */
} gfx_geom_t;

/** @brief Non-background gray sample level for the blit source. */
typedef enum : uint8_t {
  k_gfx_gray = 0xD0U, /**< Bright gray sample (differs from background). */
} gfx_gray_t;

/** @brief Distinct 0x00RRGGBB colours (RGB565 lo!=hi so a blit shows). */
typedef enum : uint32_t {
  k_gfx_bg = 0x000000FFU, /**< Background: blue. */
} gfx_col_t;

/** @brief RGB565 framebuffer (2 bytes/pixel). */
static uint8_t s_gfx_fb[(size_t)k_gfx_dim * (size_t)k_gfx_dim * 2U];

/** @brief Gray source image (one byte per sample). */
static uint8_t s_gfx_gray[(size_t)k_gfx_src_dim * (size_t)k_gfx_src_dim];

/**
 * @test test_gfx_blit_gray8_arg_guard_mcdc
 *
 * @par MC/DC:
 * Decision: `if ((src == nullptr) || (w <= 0) || (h <= 0))` (3 conditions, OR;
 * libs/ra8_gfx/src/ra8_gfx_text.c@ra8_gfx_blit_gray8).
 * Vectors (N+1 = 4 for N=3):
 *  - V1: src=buf, w>0,  h>0  -> C1 F, C2 F, C3 F -> false (blit proceeds, ok).
 *  - V2: src=nullptr       -> C1 T             -> true  (invalid_arg).
 *  - V3: src=buf, w=0,  h>0  -> C1 F, C2 T       -> true  (invalid_arg).
 *  - V4: src=buf, w>0,  h=0  -> C1 F, C2 F, C3 T -> true  (invalid_arg).
 * V1 vs V2 flips only C1; V1 vs V3 flips only C2; V1 vs V4 flips only C3.
 * Each proves independent influence.
 *
 * @pre The gfx module is initialised as RGB565.
 * @post No pixel outside the on-screen block is altered by V1.
 * @since 0.1.0
 */
static void test_gfx_blit_gray8_arg_guard_mcdc(void)
{
  TEST_BEGIN("ra8_gfx_blit_gray8 MC/DC: (src==null)||(w<=0)||(h<=0)");
  TEST_ASSERT_EQ(
    k_ra8_ok,
    ra8_gfx_init(s_gfx_fb, (uint16_t)k_gfx_dim, (uint16_t)k_gfx_dim, k_ra8_gfx_format_rgb565));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_gfx_reset_clip());
  TEST_ASSERT_EQ(k_ra8_ok, ra8_gfx_clear((uint32_t)k_gfx_bg));
  (void)memset(s_gfx_gray, (int)k_gfx_gray, sizeof s_gfx_gray);

  /* V1: all conditions false -> the blit is accepted. */
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_gfx_blit_gray8(s_gfx_gray, k_gfx_src_dim, k_gfx_src_dim, k_gfx_on, k_gfx_on));
  /* V2: C1 true -- nullptr source. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_gfx_blit_gray8(nullptr, k_gfx_src_dim, k_gfx_src_dim, k_gfx_on, k_gfx_on));
  /* V3: C2 true -- zero width. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_gfx_blit_gray8(s_gfx_gray, k_gfx_bad_span, k_gfx_src_dim, k_gfx_on, k_gfx_on));
  /* V4: C3 true -- zero height. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_gfx_blit_gray8(s_gfx_gray, k_gfx_src_dim, k_gfx_bad_span, k_gfx_on, k_gfx_on));
  TEST_END("ra8_gfx_blit_gray8 MC/DC: (src==null)||(w<=0)||(h<=0)");
}

/**
 * @test test_gfx_blit_gray8_clip_mcdc
 *
 * @par MC/DC:
 * Decision: `if ((x0 < x1) && (y0 < y1))` (2 conditions, AND; the RGB565
 * fast-path visible-region guard, libs/ra8_gfx/src/ra8_gfx_text.c@ra8_gfx_blit_gray8).
 * `x0/x1/y0/y1` are the clip-resolved block edges.
 * Vectors (N+1 = 3 for N=2):
 *  - V1: on-screen block            -> C1 T, C2 T -> true  (pixel changes).
 *  - V2: block fully right of screen -> C1 F, C2 T -> false (no pixel changes).
 *  - V3: block fully below screen    -> C1 T, C2 F -> false (no pixel changes).
 * V1 vs V2 flips only C1 (x0<x1); V1 vs V3 flips only C2 (y0<y1).
 *
 * @pre The gfx module is initialised as RGB565 with a full-screen clip.
 * @post V2/V3 leave the framebuffer byte-for-byte unchanged.
 * @since 0.1.0
 */
static void test_gfx_blit_gray8_clip_mcdc(void)
{
  TEST_BEGIN("ra8_gfx_blit_gray8 MC/DC: (x0<x1) && (y0<y1)");
  TEST_ASSERT_EQ(
    k_ra8_ok,
    ra8_gfx_init(s_gfx_fb, (uint16_t)k_gfx_dim, (uint16_t)k_gfx_dim, k_ra8_gfx_format_rgb565));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_gfx_reset_clip());
  (void)memset(s_gfx_gray, (int)k_gfx_gray, sizeof s_gfx_gray);

  /* V1: on-screen block -> both conditions true -> the fast path draws. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_gfx_clear((uint32_t)k_gfx_bg));
  const uint8_t base_v1 =
    s_gfx_fb[(((size_t)k_gfx_on * (size_t)k_gfx_dim) + (size_t)k_gfx_on) * 2U];
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_gfx_blit_gray8(s_gfx_gray, k_gfx_src_dim, k_gfx_src_dim, k_gfx_on, k_gfx_on));
  TEST_ASSERT(s_gfx_fb[(((size_t)k_gfx_on * (size_t)k_gfx_dim) + (size_t)k_gfx_on) * 2U] !=
              base_v1);

  /* V2: block fully to the right (x0 >= x1) but vertically on-screen (y0 < y1). */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_gfx_clear((uint32_t)k_gfx_bg));
  const uint8_t base_v2 =
    s_gfx_fb[(((size_t)k_gfx_on * (size_t)k_gfx_dim) + (size_t)k_gfx_on) * 2U];
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_gfx_blit_gray8(s_gfx_gray, k_gfx_src_dim, k_gfx_src_dim, k_gfx_off, k_gfx_on));
  TEST_ASSERT_EQ(base_v2,
                 s_gfx_fb[(((size_t)k_gfx_on * (size_t)k_gfx_dim) + (size_t)k_gfx_on) * 2U]);

  /* V3: block fully below (y0 >= y1) but horizontally on-screen (x0 < x1). */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_gfx_clear((uint32_t)k_gfx_bg));
  const uint8_t base_v3 =
    s_gfx_fb[(((size_t)k_gfx_on * (size_t)k_gfx_dim) + (size_t)k_gfx_on) * 2U];
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_gfx_blit_gray8(s_gfx_gray, k_gfx_src_dim, k_gfx_src_dim, k_gfx_on, k_gfx_off));
  TEST_ASSERT_EQ(base_v3,
                 s_gfx_fb[(((size_t)k_gfx_on * (size_t)k_gfx_dim) + (size_t)k_gfx_on) * 2U]);
  TEST_END("ra8_gfx_blit_gray8 MC/DC: (x0<x1) && (y0<y1)");
}

/* ===========================================================================
 * Group 2 -- internal_i2c_finish_tx (libs/ra8_hal/src/ra8_i2c.c)
 * ===========================================================================
 */

/** @brief I2C controller test constants. */
typedef enum : uint8_t {
  k_i2c_ch     = 0U,    /**< Channel under test.       */
  k_i2c_periph = 0x50U, /**< 7-bit peripheral address. */
} i2c_const_t;

/** @brief PCLKB used to program the bit rate. */
typedef enum : uint32_t {
  k_i2c_pclkb_hz = 50000000U, /**< 50 MHz PCLKB. */
} i2c_clk_t;

/** @brief Standard-mode controller configuration. */
static const ra8_i2c_cfg_t k_i2c_cfg = {
  .bus_hz   = (uint32_t)k_ra8_i2c_speed_standard,
  .pclkb_hz = (uint32_t)k_i2c_pclkb_hz,
};

/** @brief One-byte transmit payload. */
static const uint8_t s_i2c_payload[1] = {0xA5U};

/** @brief Pre-arm ICSR2.{TDRE,TEND,RDRF} so the wait loops fall through. */
static void i2c_prime_all(uint8_t channel)
{
  volatile r_i2c_regs_t* reg = ra8_i2c_regs(channel);
  reg->ICSR2 = (uint8_t)((uint8_t)k_ra8_i2c_msk_icsr2_tdre | (uint8_t)k_ra8_i2c_msk_icsr2_tend |
                         (uint8_t)k_ra8_i2c_msk_icsr2_rdrf);
}

/**
 * @test test_i2c_finish_tx_mcdc
 *
 * @par MC/DC:
 * Decision: `if ((err != k_ra8_ok) || send_stop)` (2 conditions, OR;
 * libs/ra8_hal/src/ra8_i2c.c@internal_i2c_finish_tx).
 * Vectors (N+1 = 3 for N=2):
 *  - V1: data phase ok, send_stop=false -> C1 F, C2 F -> false (bus held).
 *  - V2: data phase ok, send_stop=true  -> C1 F, C2 T -> true  (STOP issued).
 *  - V3: TEND times out (err!=ok), send_stop=false -> C1 T -> true (STOP).
 * V1 vs V2 flips only send_stop; V1 vs V3 flips only err. For V3 the address
 * and data phases succeed (TDRE primed) but the finish-phase TEND wait is
 * never satisfied, so `err` becomes hw_timeout at the decision -- the only
 * public-API path that reaches internal_i2c_finish_tx with err != k_ra8_ok
 * (an address-phase timeout early-returns before this function).
 *
 * @pre The RIIC channel is initialised on the ra8_sim MMIO window.
 * @post V1 records bus_held; V2/V3 issue STOP (ICCR2.SP set).
 * @since 0.1.0
 */
static void test_i2c_finish_tx_mcdc(void)
{
  TEST_BEGIN("i2c MC/DC: internal_i2c_finish_tx (err!=ok) || send_stop");

  /* V1: everything primed, send_stop=false -> both conditions false. */
  ra8_sim_mmap_reset();
  (void)ra8_mstp_init();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_i2c_init((uint8_t)k_i2c_ch, &k_i2c_cfg));
  i2c_prime_all((uint8_t)k_i2c_ch);
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_i2c_write((uint8_t)k_i2c_ch,
                               (uint8_t)k_i2c_periph,
                               s_i2c_payload,
                               sizeof s_i2c_payload,
                               /*send_stop=*/false));

  /* V2: everything primed, send_stop=true -> C2 true -> STOP. */
  ra8_sim_mmap_reset();
  (void)ra8_mstp_init();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_i2c_init((uint8_t)k_i2c_ch, &k_i2c_cfg));
  i2c_prime_all((uint8_t)k_i2c_ch);
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_i2c_write((uint8_t)k_i2c_ch,
                               (uint8_t)k_i2c_periph,
                               s_i2c_payload,
                               sizeof s_i2c_payload,
                               /*send_stop=*/true));
  volatile const r_i2c_regs_t* reg2 = ra8_i2c_regs((uint8_t)k_i2c_ch);
  TEST_ASSERT((reg2->ICCR2 & (uint8_t)k_ra8_i2c_msk_iccr2_sp) != 0U);

  /* V3: prime only TDRE so the address + data phases succeed but the TEND
   * wait in finish_tx times out -> err != k_ra8_ok at the decision. */
  ra8_sim_mmap_reset();
  (void)ra8_mstp_init();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_i2c_init((uint8_t)k_i2c_ch, &k_i2c_cfg));
  volatile r_i2c_regs_t* reg3 = ra8_i2c_regs((uint8_t)k_i2c_ch);
  reg3->ICSR2                 = (uint8_t)k_ra8_i2c_msk_icsr2_tdre;
  TEST_ASSERT_EQ(k_ra8_err_hw_timeout,
                 ra8_i2c_write((uint8_t)k_i2c_ch,
                               (uint8_t)k_i2c_periph,
                               s_i2c_payload,
                               sizeof s_i2c_payload,
                               /*send_stop=*/false));
  TEST_ASSERT((reg3->ICCR2 & (uint8_t)k_ra8_i2c_msk_iccr2_sp) != 0U);
  TEST_END("i2c MC/DC: internal_i2c_finish_tx (err!=ok) || send_stop");
}

/* ===========================================================================
 * Group 3 -- ra8_i2c_internal_target_drain_rx (libs/ra8_hal/src/ra8_i2c_peripheral.c)
 * ===========================================================================
 */

/** @brief RIIC peripheral (target) test constants. */
typedef enum : uint8_t {
  k_tgt_ch   = 0U,    /**< Target channel under test. */
  k_tgt_addr = 0x42U, /**< 7-bit own address.         */
  k_tgt_byte = 0xABU, /**< Received payload byte.     */
} tgt_const_t;

/** @brief Receive buffer sizing for the drain vectors. */
typedef enum : uint32_t {
  k_tgt_cap     = 3U, /**< Small capacity (fill-to-capacity vector). */
  k_tgt_cap_big = 8U, /**< Large capacity (STOP-terminated vectors). */
  k_tgt_buf     = 8U, /**< Stack buffer length.                      */
} tgt_size_t;

/** @brief Build a slot-0 own-address peripheral configuration. */
static ra8_i2c_peripheral_cfg_t tgt_make_cfg(void)
{
  const ra8_i2c_peripheral_cfg_t cfg = {
    .own_addr_7b   = (uint8_t)k_tgt_addr,
    .slot          = k_ra8_i2c_peripheral_slot_0,
    .general_call  = false,
    .clock_stretch = false,
    .irq_enable    = false,
  };
  return cfg;
}

/** @brief Reset the sim and disarm the target on the test channel. */
static void tgt_prep(void)
{
  ra8_sim_mmap_reset();
  (void)ra8_i2c_peripheral_deinit((uint8_t)k_tgt_ch);
}

/**
 * @test test_i2c_target_drain_final_byte_mcdc
 *
 * @par MC/DC:
 * Decision: `if (((icsr2 & rdrf) != 0) && (count < capacity))` (2 conditions,
 * AND; the final-pending-byte drain guard,
 * libs/ra8_hal/src/ra8_i2c_peripheral.c@ra8_i2c_internal_target_drain_rx). The
 * guard is exercised by calling the promoted drain helper DIRECTLY: the ra8_sim
 * MMIO window is side-effect-free, so the public ra8_i2c_peripheral_receive path
 * cannot present RDRF set at its address-phase wait and clear at this guard,
 * which makes the C1=F arm unreachable through receive() alone.
 * Vectors (N+1 = 3 for N=2):
 *  - V1: ICSR2=RDRF|STOP, room      -> C1 T, C2 T -> true  (drains 1 byte).
 *  - V2: ICSR2=STOP (RDRF clear)    -> C1 F        -> false (no drain, got=0).
 *  - V3: ICSR2=RDRF, fills capacity -> C1 T, C2 F -> false (got=capacity).
 * V1 vs V2 flips only C1 (rdrf pending); V1 vs V3 flips only C2 (count<cap).
 *
 * @pre The target register block is bound on the ra8_sim MMIO window.
 * @post *out_count matches the drained byte count for each vector.
 * @since 0.1.0
 */
static void test_i2c_target_drain_final_byte_mcdc(void)
{
  TEST_BEGIN("i2c-target MC/DC: drain (rdrf) && (count<capacity)");
  uint8_t                        buf[k_tgt_buf] = {};
  uint32_t                       got            = 0U;
  const ra8_i2c_peripheral_cfg_t cfg            = tgt_make_cfg();

  /* V1: RDRF|STOP with room -> drains one final pending byte (C1 T, C2 T). */
  tgt_prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_i2c_peripheral_init((uint8_t)k_tgt_ch, &cfg));
  volatile r_i2c_regs_t* reg1 = ra8_i2c_regs((uint8_t)k_tgt_ch);
  reg1->ICSR2 = (uint8_t)((uint8_t)k_ra8_i2c_msk_icsr2_rdrf | (uint8_t)k_ra8_i2c_msk_icsr2_stop);
  reg1->ICDRR = (uint8_t)k_tgt_byte;
  got         = 0U;
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_i2c_internal_target_drain_rx(reg1, buf, (uint32_t)k_tgt_cap_big, &got));
  TEST_ASSERT_EQ(1, got);
  TEST_ASSERT_EQ(k_tgt_byte, buf[0]);

  /* V2: STOP with RDRF clear -> the drain guard's C1 is false -> no byte. */
  tgt_prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_i2c_peripheral_init((uint8_t)k_tgt_ch, &cfg));
  volatile r_i2c_regs_t* reg2 = ra8_i2c_regs((uint8_t)k_tgt_ch);
  reg2->ICSR2                 = (uint8_t)k_ra8_i2c_msk_icsr2_stop;
  got                         = 0U;
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_i2c_internal_target_drain_rx(reg2, buf, (uint32_t)k_tgt_cap_big, &got));
  TEST_ASSERT_EQ(0, got);

  /* V3: RDRF without STOP fills to capacity -> C2 false at the exit check. */
  tgt_prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_i2c_peripheral_init((uint8_t)k_tgt_ch, &cfg));
  volatile r_i2c_regs_t* reg3 = ra8_i2c_regs((uint8_t)k_tgt_ch);
  reg3->ICSR2                 = (uint8_t)k_ra8_i2c_msk_icsr2_rdrf;
  reg3->ICDRR                 = (uint8_t)k_tgt_byte;
  got                         = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_i2c_internal_target_drain_rx(reg3, buf, (uint32_t)k_tgt_cap, &got));
  TEST_ASSERT_EQ(k_tgt_cap, got);
  TEST_END("i2c-target MC/DC: drain (rdrf) && (count<capacity)");
}

/* ===========================================================================
 * Group 4 -- dec_dispatch_marker unsupported-SOFn (libs/ra8_hal/src/ra8_jpeg_sw_decode.c)
 * ===========================================================================
 */

/** @brief JPEG decode scratch: output buffer + dimension sinks. */
typedef enum : uint32_t {
  k_jpeg_out_len = 64U, /**< Output byte budget (unused: every stream errors). */
} jpeg_out_t;

/** @brief Marker low bytes selecting each MC/DC condition arm. */
typedef enum : uint8_t {
  k_jm_sof2 = 0xC2U, /**< SOF2 progressive: all four conditions true. */
  k_jm_lo   = 0xBFU, /**< 0xFFBF (< SOF1): C1 false.                  */
  k_jm_hi   = 0xD0U, /**< 0xFFD0 (> SOF_HI, RST0): C2 false.          */
  k_jm_dht  = 0xC4U, /**< 0xFFC4 (DHT): C3 false.                     */
  k_jm_jpg  = 0xC8U, /**< 0xFFC8 (JPG): C4 false.                     */
} jpeg_marker_lo_t;

/** @brief Output buffer for the decode calls. */
static uint8_t s_jpeg_out[k_jpeg_out_len];

/** @brief Run ra8_jpeg_sw_decode on `FF D8 FF <m> 00 00 00 00`, returning err. */
static ra8_err_t jpeg_decode_marker(uint8_t marker_lo)
{
  const uint8_t stream[8] = {0xFFU, 0xD8U, 0xFFU, marker_lo, 0x00U, 0x00U, 0x00U, 0x00U};
  uint16_t      w         = 0U;
  uint16_t      h         = 0U;
  return ra8_jpeg_sw_decode(stream,
                            (uint32_t)sizeof stream,
                            s_jpeg_out,
                            (uint32_t)k_jpeg_out_len,
                            &w,
                            &h);
}

/**
 * @test test_jpeg_unsupported_sofn_mcdc
 *
 * @par MC/DC:
 * Decision: `if (mk >= sof1 && mk <= sof_hi && mk != dht && mk != jpg)`
 * (4 conditions, AND; the unsupported-SOFn classifier,
 * libs/ra8_hal/src/ra8_jpeg_sw_decode.c@dec_dispatch_marker). `mk = 0xFF00 | m`
 * is read from the marker byte of a `FF D8 FF <m>` stream.
 * Vectors (N+1 = 5 for N=4):
 *  - V1: mk=0xFFC2 (SOF2)  -> C1 T, C2 T, C3 T, C4 T -> true  (not_supported).
 *  - V2: mk=0xFFBF         -> C1 F                    -> false (other error).
 *  - V3: mk=0xFFD0 (RST0)  -> C1 T, C2 F              -> false (other error).
 *  - V4: mk=0xFFC4 (DHT)   -> C1 T, C2 T, C3 F        -> false (other error).
 *  - V5: mk=0xFFC8 (JPG)   -> C1 T, C2 T, C3 T, C4 F  -> false (other error).
 * V1 pairs with V2/V3/V4/V5 to flip exactly one condition each, proving the
 * independent influence of C1/C2/C3/C4 respectively. Only V1 yields
 * not_supported; V2..V5 are incomplete streams that fail with a different
 * status after the decision has been evaluated false.
 *
 * @pre The stream begins with a valid SOI so the marker loop is entered.
 * @post V1 returns k_ra8_err_not_supported; V2..V5 return non-ok, non-not_supported.
 * @since 0.1.0
 */
static void test_jpeg_unsupported_sofn_mcdc(void)
{
  TEST_BEGIN("jpeg MC/DC: unsupported-SOFn 4-condition classifier");
  /* V1: the all-true arm returns k_ra8_err_not_supported. */
  TEST_ASSERT_EQ(k_ra8_err_not_supported, jpeg_decode_marker((uint8_t)k_jm_sof2));
  /* V2..V5: each flips one condition false; the decision is evaluated but the
   * stream is invalid, so decode fails with a status other than
   * not_supported. */
  TEST_ASSERT(jpeg_decode_marker((uint8_t)k_jm_lo) != k_ra8_ok);
  TEST_ASSERT(jpeg_decode_marker((uint8_t)k_jm_lo) != k_ra8_err_not_supported);
  TEST_ASSERT(jpeg_decode_marker((uint8_t)k_jm_hi) != k_ra8_ok);
  TEST_ASSERT(jpeg_decode_marker((uint8_t)k_jm_hi) != k_ra8_err_not_supported);
  TEST_ASSERT(jpeg_decode_marker((uint8_t)k_jm_dht) != k_ra8_err_not_supported);
  TEST_ASSERT(jpeg_decode_marker((uint8_t)k_jm_jpg) != k_ra8_err_not_supported);
  TEST_END("jpeg MC/DC: unsupported-SOFn 4-condition classifier");
}

/**
 * @brief Test entry point.
 * @return 0 on success; unity macros call exit(1) on the first failure.
 * @since 0.1.0
 */
int32_t main(void)
{
  test_gfx_blit_gray8_arg_guard_mcdc();
  test_gfx_blit_gray8_clip_mcdc();
  test_i2c_finish_tx_mcdc();
  test_i2c_target_drain_final_byte_mcdc();
  test_jpeg_unsupported_sofn_mcdc();
  (void)fprintf(stderr, "[OK ] test_mcdc_hal.c\n");
  return 0;
}
