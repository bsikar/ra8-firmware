/**
 * @file ra8_drw.h
 * @brief 2D Drawing Engine (DRW / D/AVE 2D) HAL driver -- public API
 * @ingroup grp_hal_display
 *
 * @par Tag
 * [Ring 3 / HAL] {World: NS}
 *
 * @details
 * Bare-metal driver for the RA8D2 2D drawing engine ("D/AVE 2D").
 * After the expansion the API now covers every register the
 * HUM Ch 62 documents, every operating mode (solid fill, textured
 * blit, RLE source, CLUT lookup, color key, alpha blend, anti-
 * aliased line / triangle, display list) and every IRQ source (enum
 * complete, dlist complete, bus error).
 *
 * - Lifecycle: ``ra8_drw_init``, ``ra8_drw_deinit``, power
 * transitions, MSTP gate.
 * - Diagnostics: ``ra8_drw_get_status``, ``ra8_drw_clear_status``,
 * ``ra8_drw_get_hwrevision``, ``ra8_drw_wait_idle``.
 * - IRQ: ``ra8_drw_attach_handler``, ``ra8_drw_dispatch``,
 * ``ra8_drw_set_irq_enables``.
 * - Drawing: solid fill, textured blit, anti-aliased line,
 * anti-aliased triangle, display list trigger.
 * - Surface: per-pixel + global alpha blending, color key,
 * pattern, all six limiters with band post-
 * process.
 * - Texture: base / pitch / U/V mask / clamp / filter,
 * CLUT load, RLE source, ACLUT44 / I8 / etc.
 * - Performance: both PERFCOUNTk + PERFTRIGGER selector.
 * - Cache: explicit FB and texture flush hooks.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "ra8_drw_regs.h"
#include "ra8_err.h"

/* =============================================================================
 * Configuration / runtime data structures
 * =============================================================================
 */

/**
 * @struct ra8_drw_config_t
 * @brief Minimal DRW configuration captured by ``ra8_drw_init``.
 *
 * @details
 * The config carries only the bits the driver needs to know up
 * front: framebuffer base address, pixel format, and pitch in
 * pixels (used to derive PITCH and SIZE for fill operations). The
 * cppcheck suppress-block is required because cppcheck cannot see
 * the test sources where the fields are read.
 *
 * @invariant ``framebuffer_addr`` is 4-byte aligned for ARGB8888.
 * @invariant ``pitch_px`` is equal to or larger than the framebuffer
 * width in pixels.
 *
 * @code
 * const ra8_drw_config_t cfg = {
 *.framebuffer_addr = (uintptr_t)g_layer1,
 *.pitch_px = 1024,
 *.format = k_ra8_drw_writefmt_argb8888,
 *.enable_caches = true,
 *.enable_buffered_writes = true,
 * };
 * if (ra8_drw_init(&cfg) != k_ra8_ok) {... }
 * @endcode
 */
typedef struct {
  uintptr_t             framebuffer_addr;       /**< FB base addr (target ARGB). */
  uint16_t              pitch_px;               /**< FB pitch in pixels.         */
  ra8_drw_writeformat_t format;                 /**< FB pixel format.            */
  bool                  enable_caches;          /**< Enable FB + texture caches. */
  bool                  enable_buffered_writes; /**< Set DBWER.BWE on init.      */
} ra8_drw_config_t;

/**
 * @struct ra8_drw_rect_t
 * @brief Rectangle parameters for ``ra8_drw_fill_rect``.
 *
 * @details
 * Coordinates are framebuffer pixels relative to the framebuffer
 * origin programmed at init time. ``color_argb8888`` is interpreted
 * as ``0xAARRGGBB`` and pushed into the COLOR1 register.
 */
typedef struct {
  int16_t  x;              /**< Top-left X in pixels.   */
  int16_t  y;              /**< Top-left Y in pixels.   */
  uint16_t width_px;       /**< Width in pixels.        */
  uint16_t height_px;      /**< Height in pixels.       */
  uint32_t color_argb8888; /**< 0xAARRGGBB fill colour. */
} ra8_drw_rect_t;

/**
 * @struct ra8_drw_blend_t
 * @brief Per-pixel + global alpha blending parameters.
 *
 * @details
 * Captured in CONTROL2.{BSF,BDF,BSI,BDI,BSFA,BDFA,BSIA,BDIA,USEACB}
 * exactly as documented by HUM Ch 62.2.2 p 3691-3692 and the
 * "Blending" subsection of Ch 62.4.7 p 3729. The standard "source
 * over" combination is BSF=1, BDF=1, BSFA=1, BDFA=1, BDIA=1, USEACB=1.
 */
typedef struct {
  bool    use_alpha_channel; /**< CONTROL2.USEACB                 */
  bool    src_factor;        /**< CONTROL2.BSF (use SRC alpha)    */
  bool    dst_factor;        /**< CONTROL2.BDF (use DST alpha)    */
  bool    src_invert;        /**< CONTROL2.BSI                    */
  bool    dst_invert;        /**< CONTROL2.BDI                    */
  bool    src_factor_alpha;  /**< CONTROL2.BSFA                   */
  bool    dst_factor_alpha;  /**< CONTROL2.BDFA                   */
  bool    src_invert_alpha;  /**< CONTROL2.BSIA                   */
  bool    dst_invert_alpha;  /**< CONTROL2.BDIA                   */
  bool    use_color2_dst;    /**< CONTROL2.BC2 (COLOR2 as dst)    */
  uint8_t global_alpha;      /**< COLOR1.A (0=fully transparent). */
} ra8_drw_blend_t;

/**
 * @struct ra8_drw_texture_t
 * @brief Texture parameters for ``ra8_drw_blit_textured_rect``.
 *
 * @details
 * Maps to TEXORIGIN, TEXPITCH, TEXMASK and the U/V limiter set
 * (LUSTART, LUXADD, LUYADD, LVSTART{I,F}, LVXADDI, LVYADDI,
 * LVYXADDF). HUM Ch 62.2.14.. 62.2.24 p 3700-3703.
 *
 * @invariant ``base_addr`` is at least 4-byte aligned.
 * @invariant ``pitch_px`` <= ``k_ra8_drw_max_texpitch_tx`` (HUM 62.2.15).
 */
typedef struct {
  uintptr_t            base_addr;        /**< TEXORIGIN -- texel base.      */
  uint16_t             pitch_px;         /**< TEXPITCH (texels per line).   */
  uint16_t             u_mask;           /**< TEXMASK[15:0] (U mask).       */
  uint16_t             v_mask;           /**< TEXMASK[31:16] (V mask).      */
  ra8_drw_readformat_t format;           /**< READFORMAT_H+L 4-bit value.   */
  bool                 clamp_x;          /**< CONTROL2.TEXTURECLAMPX.       */
  bool                 clamp_y;          /**< CONTROL2.TEXTURECLAMPY.       */
  bool                 filter_x;         /**< CONTROL2.TEXTUREFILTERX.      */
  bool                 filter_y;         /**< CONTROL2.TEXTUREFILTERY.      */
  bool                 enable_clut;      /**< CONTROL2.CLUTENABLE.          */
  bool                 clut_565;         /**< CONTROL2.CLUTFORMAT (RGB565). */
  bool                 enable_color_key; /**< CONTROL2.COLKEYENABLE.        */
  uint32_t             color_key_rgb;    /**< COLKEY value (0x00RRGGBB).    */
  bool                 enable_rle;       /**< CONTROL2.RLEENABLE.           */
  ra8_drw_rlepixel_t   rle_pixel_width;  /**< CONTROL2.RLEPIXELWIDTH.       */
  uint8_t              clut_offset;      /**< TEXCLOFFSET CLOFFSET[7:0].    */
} ra8_drw_texture_t;

/**
 * @struct ra8_drw_line_t
 * @brief Anti-aliased line parameters.
 *
 * @details
 * Two end-points and a stroke width. HUM Ch 62.4.4 "Lines" p 3725
 * documents the 6-limiter encoding the engine uses internally; this
 * driver pre-computes those limiters from (x0,y0,x1,y1,width).
 */
typedef struct {
  int16_t  x0;             /**< End-point 0 X in pixels.  */
  int16_t  y0;             /**< End-point 0 Y in pixels.  */
  int16_t  x1;             /**< End-point 1 X in pixels.  */
  int16_t  y1;             /**< End-point 1 Y in pixels.  */
  uint16_t width_px;       /**< Stroke width in pixels.   */
  uint32_t color_argb8888; /**< 0xAARRGGBB stroke colour. */
} ra8_drw_line_t;

/**
 * @struct ra8_drw_triangle_t
 * @brief Anti-aliased filled triangle parameters.
 *
 * @details
 * Three vertices in pixel space. HUM Ch 62.4.5 "Triangles" p 3726
 * uses LIM1..LIM3 with all-intersect to bound a filled triangle.
 */
typedef struct {
  int16_t  x0;             /**< Vertex 0 X.  */
  int16_t  y0;             /**< Vertex 0 Y.  */
  int16_t  x1;             /**< Vertex 1 X.  */
  int16_t  y1;             /**< Vertex 1 Y.  */
  int16_t  x2;             /**< Vertex 2 X.  */
  int16_t  y2;             /**< Vertex 2 Y.  */
  uint32_t color_argb8888; /**< Fill colour. */
} ra8_drw_triangle_t;

/**
 * @struct ra8_drw_gradient_t
 * @brief COLOR1 + COLOR2 two-stop gradient.
 *
 * @details
 * COLOR1 holds the source ("pixel") colour, COLOR2 holds the
 * destination ("background") colour for blend ops. Used as a simple
 * flat / 2-stop gradient via the blend unit. HUM Ch 62.2.7-8 p 3697.
 */
typedef struct {
  uint32_t color1_argb8888; /**< COLOR1 ARGB stop. */
  uint32_t color2_argb8888; /**< COLOR2 ARGB stop. */
} ra8_drw_gradient_t;

/**
 * @struct ra8_drw_dlist_t
 * @brief Builder state for composing a DRW display list in a caller buffer.
 *
 * @details
 * A display list lets the DRW clear and repaint a framebuffer end to end with
 * the CPU never touching it inside the loop, so there is no CPU/engine write
 * race to latch STATUS.BUSERRMFB -- the loop-stable path proven on silicon for
 * issue #247. ::ra8_drw_dlist_begin binds this builder to a 4-byte-aligned
 * word buffer in a region the DRW bus initiator can read (SRAM);
 * ::ra8_drw_dlist_add_fill appends primitives; ::ra8_drw_dlist_end terminates
 * the list; ::ra8_drw_dlist_run kicks it. The build calls never touch MMIO.
 *
 * @invariant ``count <= cap_words`` at all times; ``overflow`` latches true if
 * a request would have exceeded ``cap_words`` and no partial entry is written.
 *
 * @code
 * static uint32_t s_dlist[32];
 * ra8_drw_dlist_t dl;
 * ra8_drw_dlist_begin(&dl, s_dlist, 32U);
 * ra8_drw_dlist_add_fill(&dl, &clear_rect);
 * ra8_drw_dlist_add_fill(&dl, &box_rect);
 * ra8_drw_dlist_end(&dl);
 * ra8_drw_dlist_run(&dl);
 * @endcode
 *
 * @see ra8_drw_dlist_begin
 * @see ra8_drw_dlist_add_fill
 */
typedef struct {
  uint32_t* buf;        /**< Caller word buffer, 4-byte aligned, in SRAM. */
  uint32_t  cap_words;  /**< Buffer capacity in 32-bit words.             */
  uint32_t  count;      /**< Words emitted so far (<= cap_words).         */
  bool      overflow;   /**< A request exceeded the buffer capacity.      */
  bool      terminated; /**< ::ra8_drw_dlist_end has closed the list.     */
} ra8_drw_dlist_t;

/**
 * @typedef ra8_drw_event_fn_t
 * @brief DRW asynchronous event callback signature.
 *
 * @details
 * Fired from ``ra8_drw_dispatch`` after the DRW IRQ snapshots and
 * acknowledges the STATUS register. The ``status_mask`` argument
 * carries the raw STATUS value the dispatcher observed before
 * clearing it; consumers test bits with the ``k_ra8_drw_status_*``
 * masks from ``ra8_drw_regs.h``.
 */
typedef void (*ra8_drw_event_fn_t)(void* ctx, uint32_t status_mask);

/* =============================================================================
 * Lifecycle
 * =============================================================================
 */

/**
 * @brief Power on the DRW block, install the supplied configuration,
 * and arm the IRQ control register.
 *
 * @details
 * Sequence performed:
 * 1. Ungate the DRW peripheral via ``ra8_mstp_enable(k_ra8_mstp_drw)``.
 * 2. Mask + clear all DRW IRQs (write IRQCTL = all_clr).
 * 3. Programme the framebuffer base address (ORIGIN), pitch
 * (PITCH), and format (CONTROL2).
 * 4. Enable framebuffer + texture caches if ``cfg->enable_caches``.
 * 5. Set DBWER.BWE if ``cfg->enable_buffered_writes``.
 *
 * @param[in] cfg Pointer to the configuration. Must not be ``nullptr``.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok DRW powered up + configured.
 * @retval k_ra8_err_null_ptr ``cfg`` was ``nullptr``.
 * @retval k_ra8_err_hw_init_failed MSTP enable failed.
 *
 * @pre ``cfg`` is non-null.
 * @pre Caller holds single-threaded init context.
 *
 * @post DRW is powered on, IRQs masked, ready for primitives.
 * @post ORIGIN, PITCH, and CONTROL2 reflect the supplied config.
 *
 * @note Not thread-safe; mask IRQs across the call.
 *
 * @see ra8_drw_deinit
 * @see ra8_drw_fill_rect
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_drw_init(const ra8_drw_config_t* cfg);

/**
 * @brief Tear down the DRW block: disable IRQs, drop the registered
 * callback, and request MSTP gate.
 *
 * @return ``ra8_err_t`` propagated from ``ra8_mstp_disable``.
 * @retval k_ra8_ok DRW powered down successfully.
 *
 * @pre Driver was previously initialized (otherwise the disable is
 * a no-op since ref count is already zero).
 * @pre Caller holds single-threaded teardown context.
 *
 * @post Registered callback (if any) has been cleared.
 * @post DRW peripheral is in MSTP-stop state.
 *
 * @note Not thread-safe.
 * @see ra8_drw_init
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_drw_deinit(void);

/* =============================================================================
 * Status / IRQ
 * =============================================================================
 */

/**
 * @brief Read the DRW STATUS register into ``*out_mask``.
 *
 * @param[out] out_mask Receives the raw 32-bit STATUS value. Must be
 * non-null. Bits decode via ``k_ra8_drw_status_*``.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok STATUS read into ``*out_mask``.
 * @retval k_ra8_err_null_ptr ``out_mask`` was ``nullptr``.
 *
 * @pre ``out_mask`` is non-null.
 * @pre Driver has been initialized (peripheral clock running).
 *
 * @post ``*out_mask`` reflects the latched STATUS bits at the time
 * of the read.
 * @post No side effects on the DRW block (read is non-destructive).
 *
 * @note Thread-safe with respect to other read-only callers.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_drw_get_status(uint32_t* out_mask);

/**
 * @brief Read the read-only HWREVISION register into ``*out``.
 *
 * @param[out] out Receives HWREVISION (HUM Ch 62.2.6 p 3696). Decode
 * via ``k_ra8_drw_hwrev_*`` masks.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok HWREVISION read.
 * @retval k_ra8_err_null_ptr ``out`` was nullptr.
 *
 * @pre Driver has been initialized.
 * @pre ``out`` non-null.
 * @post ``*out`` carries the alias-of-CONTROL2 HWREVISION value.
 * @post No DRW side effects.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_drw_get_hwrevision(uint32_t* out);

/**
 * @brief Acknowledge a subset of pending DRW interrupts via IRQCTL.
 *
 * @details
 * Writes the supplied mask to IRQCTL (write-1-to-clear semantics
 * for the ``*CLR`` bits). The mask should be one of the documented
 * ``k_ra8_drw_irqctl_*_clr`` enum values; arbitrary other bits in
 * the mask will set / clear the corresponding enable bits and the
 * caller must be prepared for that.
 *
 * @param[in] mask IRQCTL bits to write (W1C for ``ENUMIRQCLR``,
 * ``DLISTIRQCLR``, ``BUSIRQCLR``).
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok IRQCTL written.
 *
 * @pre Driver has been initialized.
 * @pre Caller's mask is constructed from documented bit enums.
 *
 * @post Bits selected by ``mask`` have been pushed to IRQCTL.
 * @post No state outside DRW is touched.
 *
 * @note Thread-safe; the write is a single 32-bit store.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_drw_clear_status(uint32_t mask);

/**
 * @brief Set the IRQCTL enable bits (ENUMIRQEN, DLISTIRQEN, BUSIRQEN).
 *
 * @details
 * Writes IRQCTL with the supplied combination of enable bits *plus*
 * an unconditional W1C of every pending bit, so the caller cannot
 * accidentally re-enable an interrupt while the corresponding flag
 * is still latched.
 *
 * @param[in] enable_mask Bitwise OR of any of:
 * ``k_ra8_drw_irqctl_enumirqen``,
 * ``k_ra8_drw_irqctl_dlistirqen``,
 * ``k_ra8_drw_irqctl_busirqen``.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok IRQCTL updated.
 *
 * @pre Driver has been initialized.
 * @pre Only enable bits set; W1C bits will be ORed in by this call.
 * @post IRQCTL.{ENUM,DLIST,BUS}IRQEN match ``enable_mask``.
 * @post All pending IRQ flags have been cleared.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_drw_set_irq_enables(uint32_t enable_mask);

/**
 * @brief Register a software callback for DRW IRQ events.
 *
 * @param[in] fn Callback function pointer (may be ``nullptr`` to
 * detach the previously registered callback).
 * @param[in] ctx Opaque caller context, passed back as the first
 * argument of ``fn``.
 *
 * @return ``ra8_err_t`` always ``k_ra8_ok``.
 *
 * @pre Caller owns ``ctx`` for the lifetime of the callback.
 * @pre Either ``fn == nullptr`` (detach) or ``fn`` is callable.
 *
 * @post Subsequent calls to ``ra8_drw_dispatch`` will invoke ``fn``.
 * @post Previous callback (if any) is replaced atomically with
 * respect to the dispatch path.
 *
 * @note Not interrupt-safe; install once at init.
 * @see ra8_drw_dispatch
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_drw_attach_handler(ra8_drw_event_fn_t fn, void* ctx);

/**
 * @brief Snapshot STATUS, ack pending DRW IRQs, and fire callback.
 *
 * @details
 * Intended to be called from the DRW ISR (or from a host test).
 * Reads STATUS, writes ``IRQCTL = all_clr`` to ack pending bits,
 * then invokes the registered callback (if any) with the snapshot.
 *
 * @pre Driver has been initialized.
 * @pre Called from IRQ context or with IRQs masked.
 *
 * @post All pending DRW IRQ flags acknowledged in IRQCTL.
 * @post Registered callback fired exactly once with the snapshot.
 *
 * @note Safe in IRQ context.
 * @see ra8_drw_attach_handler
 * @since 0.1.0
 */
void ra8_drw_dispatch(void);

/**
 * @brief Poll STATUS until all "busy" bits clear or budget runs out.
 *
 * @details
 * Bounded wait (NASA Rule 2). Returns ``k_ra8_err_hw_timeout`` if the
 * engine remains busy after ``poll_budget`` reads.
 *
 * @param[in] poll_budget Maximum number of STATUS reads before timeout.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok Engine idle.
 * @retval k_ra8_err_hw_timeout Budget exhausted.
 *
 * @pre Driver has been initialized.
 * @pre ``poll_budget`` >= 1.
 * @post On success, all of BUSYENUM / BUSYWRITE / DLISTACTIVE are 0.
 * @post On timeout, no register state is changed.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_drw_wait_idle(uint32_t poll_budget);

/* =============================================================================
 * Power transition
 * =============================================================================
 */

/**
 * @brief Drop the DRW into MSTP-gated stop (clock removed).
 *
 * @return ``ra8_err_t`` propagated from ``ra8_mstp_disable``.
 * @retval k_ra8_ok DRW gated.
 *
 * @pre No DRW operation is in flight (caller drained the engine).
 * @pre Caller holds single-threaded power transition lock.
 *
 * @post DRW peripheral clock is disabled.
 * @post Registers retain their last-written value but cannot be
 * accessed without first calling ``ra8_drw_exit_stop``.
 *
 * @note Not thread-safe.
 * @see ra8_drw_exit_stop
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_drw_enter_stop(void);

/**
 * @brief Re-enable the DRW MSTP gate (clock restored).
 *
 * @return ``ra8_err_t`` propagated from ``ra8_mstp_enable``.
 * @retval k_ra8_ok DRW running.
 *
 * @pre Caller holds single-threaded power transition lock.
 * @pre Previous ``ra8_drw_enter_stop`` happened (or this is first
 * power-on; idempotent in that case).
 *
 * @post DRW peripheral clock is enabled.
 * @post Caller may now read STATUS or issue further primitives.
 *
 * @note Not thread-safe.
 * @see ra8_drw_enter_stop
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_drw_exit_stop(void);

/**
 * @brief Software reset: drain pipeline, ack IRQs, reset PERFCOUNTers.
 *
 * @return ``ra8_err_t`` always ``k_ra8_ok``.
 *
 * @pre Driver has been initialized.
 * @pre Caller is OK with discarding pending draws.
 * @post All IRQ flags ack'd; PERFCOUNT1/2 zeroed.
 * @post CACHECTL flush bits pulsed.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_drw_reset(void);

/* =============================================================================
 * Cache control
 * =============================================================================
 */

/**
 * @brief Pulse FB and/or texture cache flush bits in CACHECTL.
 *
 * @param[in] flush_fb True to set CFLUSHFX (framebuffer flush).
 * @param[in] flush_texture True to set CFLUSHTX (texture flush).
 *
 * @return ``ra8_err_t`` always ``k_ra8_ok``.
 *
 * @pre Driver has been initialized.
 * @pre At least one of the booleans is true (no-op accepted but
 * logged).
 * @post Selected cache(s) are flushed; enable bits are preserved.
 * @post No effect on geometry / blend state.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_drw_cache_flush(bool flush_fb, bool flush_texture);

/* =============================================================================
 * Surface / blend / colour
 * =============================================================================
 */

/**
 * @brief Programme the COLOR1 and COLOR2 registers from a 2-stop gradient.
 *
 * @param[in] grad Pointer to the gradient. Must not be ``nullptr``.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok COLOR1/COLOR2 updated.
 * @retval k_ra8_err_null_ptr ``grad`` was nullptr.
 *
 * @pre ``grad`` is non-null.
 * @pre Driver has been initialized.
 * @post COLOR1 = grad->color1_argb8888.
 * @post COLOR2 = grad->color2_argb8888.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_drw_set_gradient(const ra8_drw_gradient_t* grad);

/**
 * @brief Set the per-byte PATTERN bitmap (HUM Ch 62.2.9 p 3698).
 *
 * @param[in] pattern_byte Pattern bitmap; only the low 8 bits used.
 *
 * @return ``ra8_err_t`` always ``k_ra8_ok``.
 *
 * @pre Driver has been initialized.
 * @pre Pattern source has been enabled via ``ra8_drw_set_pattern_enable``.
 * @post PATTERN[7:0] = pattern_byte; upper bits zeroed (HUM W=0).
 * @post No effect on geometry / blend state.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_drw_set_pattern(uint8_t pattern_byte);

/**
 * @brief Toggle CONTROL2.PATTERNENABLE / PATTERNSOURCEL5.
 *
 * @param[in] enable True to set CONTROL2.PATTERNENABLE.
 * @param[in] source_from_l5 True to set CONTROL2.PATTERNSOURCEL5.
 *
 * @return ``ra8_err_t`` always ``k_ra8_ok``.
 *
 * @pre Driver has been initialized.
 * @pre PATTERN already programmed if ``enable`` is true.
 * @post CONTROL2 reflects the requested pattern bits; other CONTROL2
 * fields preserved.
 * @post No effect on COLOR1/COLOR2.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_drw_set_pattern_enable(bool enable, bool source_from_l5);

/**
 * @brief Configure the alpha-blend unit using ``ra8_drw_blend_t`` fields.
 *
 * @param[in] blend Blend parameters. Must not be ``nullptr``.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok CONTROL2 + COLOR1.A updated.
 * @retval k_ra8_err_null_ptr ``blend`` was nullptr.
 *
 * @pre ``blend`` is non-null.
 * @pre Driver has been initialized.
 * @post CONTROL2.{BSF,BDF,BSI,BDI,BSFA,BDFA,BSIA,BDIA,BC2,USEACB}
 * reflect ``*blend``; other CONTROL2 bits preserved.
 * @post COLOR1 alpha byte set to ``blend->global_alpha`` (other COLOR1
 * channels are NOT touched -- caller controls the RGB triple via
 * ``ra8_drw_set_gradient`` first).
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_drw_set_blend(const ra8_drw_blend_t* blend);

/**
 * @brief Programme the COLKEY register and toggle COLKEYENABLE.
 *
 * @param[in] key_rgb 0x00RRGGBB colour key value (alpha bits ignored).
 * @param[in] enable True to set CONTROL2.COLKEYENABLE.
 *
 * @return ``ra8_err_t`` always ``k_ra8_ok``.
 *
 * @pre Driver has been initialized.
 * @pre Caller has chosen a key colour that does not occur in the source
 * data when ``enable`` is true.
 * @post COLKEY = key_rgb (alpha cleared per HUM W=0 rule).
 * @post CONTROL2.COLKEYENABLE matches ``enable``.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_drw_set_color_key(uint32_t key_rgb, bool enable);

/* =============================================================================
 * Texture
 * =============================================================================
 */

/**
 * @brief Programme TEXORIGIN, TEXPITCH, TEXMASK, U/V limiters and the
 * CONTROL2 texture / CLUT / RLE / colour-key bits.
 *
 * @param[in] tex Texture descriptor. Must not be ``nullptr``.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok Texture programmed.
 * @retval k_ra8_err_null_ptr ``tex`` was nullptr.
 * @retval k_ra8_err_invalid_arg pitch_px > k_ra8_drw_max_texpitch_tx.
 *
 * @pre ``tex`` non-null and points at validated descriptor.
 * @pre Driver has been initialized.
 * @post TEXORIGIN, TEXPITCH, TEXMASK, U/V limiters, COLKEY (when
 * enabled) all reflect ``*tex``.
 * @post CONTROL2.{TEXTUREENABLE,TEXTURECLAMPX,TEXTURECLAMPY,
 * TEXTUREFILTERX,TEXTUREFILTERY,READFORMAT,RLEENABLE,
 * CLUTENABLE,CLUTFORMAT,COLKEYENABLE,RLEPIXELWIDTH} updated.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_drw_set_texture(const ra8_drw_texture_t* tex);

/**
 * @brief Disable the texture unit (clear CONTROL2.TEXTUREENABLE / RLE / CLUT).
 *
 * @return ``ra8_err_t`` always ``k_ra8_ok``.
 *
 * @pre Driver has been initialized.
 * @pre Caller drained any in-flight textured blit.
 * @post CONTROL2.{TEXTUREENABLE,RLEENABLE,CLUTENABLE,COLKEYENABLE} = 0.
 * @post Other CONTROL2 fields preserved.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_drw_clear_texture(void);

/**
 * @brief Load a CLUT entry pair (TEXCLADDR, TEXCLDATA, TEXCLOFFSET).
 *
 * @details
 * Walks the supplied ``entries`` array, writing TEXCLADDR with the
 * starting CLUT index then streaming each ARGB8888 entry into
 * TEXCLDATA. The DRW auto-increments CLADDR after each TEXCLDATA
 * write (HUM Ch 62.5.4 p 3717).
 *
 * @param[in] start_index Index of the first CLUT slot to write [0..255].
 * @param[in] entries ARGB8888 entries to load (length = ``count``).
 * @param[in] count Number of entries to write [1..256].
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok CLUT loaded.
 * @retval k_ra8_err_null_ptr ``entries`` was nullptr.
 * @retval k_ra8_err_invalid_arg start_index + count > 256, or count = 0.
 *
 * @pre ``entries`` non-null.
 * @pre ``count`` in [1, 256] and ``start_index + count <= 256``.
 * @post All requested CLUT slots have been written.
 * @post TEXCLADDR points one past the last written index (auto-inc).
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t
ra8_drw_load_clut(uint8_t start_index, const uint32_t* entries, uint32_t count);

/* =============================================================================
 * Drawing primitives
 * =============================================================================
 */

/**
 * @brief Issue a solid-fill rectangle blit into the framebuffer.
 *
 * @details
 * Programmes the DRW limiters to bound a rectangle, sets COLOR1 to
 * the requested ARGB colour, then writes CONTROL with the four
 * limiter-enable bits set to kick the engine.
 *
 * @param[in] rect Rectangle parameters. Must not be ``nullptr``.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok Blit issued (engine may still be busy).
 * @retval k_ra8_err_null_ptr ``rect`` was ``nullptr``.
 * @retval k_ra8_err_invalid_arg width/height was 0 or > 1024.
 *
 * @pre ``rect`` is non-null.
 * @pre Driver is initialized; framebuffer ORIGIN is valid memory.
 * @post DRW engine has accepted the rectangle; STATUS may be busy.
 * @post Caller may follow with ``ra8_drw_get_status`` to wait on
 * ``k_ra8_drw_status_busyenum`` to fall to 0.
 *
 * @note Not thread-safe with respect to other DRW operations.
 *
 * @see ra8_drw_get_status
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_drw_fill_rect(const ra8_drw_rect_t* rect);

/**
 * @brief Blit a textured rectangle (texture must be programmed first).
 *
 * @details
 * The caller programmes the texture state via ``ra8_drw_set_texture``
 * (which also enables CONTROL2.TEXTUREENABLE). This function then
 * writes the rectangle limiter set + SIZE and starts the engine via
 * a write to CONTROL.
 *
 * @param[in] rect Destination rectangle. Must not be ``nullptr``.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok Blit issued.
 * @retval k_ra8_err_null_ptr ``rect`` was nullptr.
 * @retval k_ra8_err_invalid_arg width/height out of range.
 *
 * @pre ``rect`` non-null and validated.
 * @pre Texture state is programmed (CONTROL2.TEXTUREENABLE = 1).
 * @post DRW engine has accepted the textured rectangle.
 * @post CACHECTL pulsed to flush stale FB lines before consumption.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_drw_blit_textured_rect(const ra8_drw_rect_t* rect);

/**
 * @brief Issue an anti-aliased line stroke between two points.
 *
 * @details
 * Pre-computes the four perpendicular limiters bounding the stroke,
 * programmes COLOR1, then enables LIM1..LIM4 with the standard
 * intersect topology. HUM Ch 62.4.4 "Lines" p 3725.
 *
 * @param[in] line Line parameters. Must not be ``nullptr``.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok Line issued.
 * @retval k_ra8_err_null_ptr ``line`` was nullptr.
 * @retval k_ra8_err_invalid_arg width_px == 0 or > 1024.
 *
 * @pre ``line`` non-null.
 * @pre Driver is initialized.
 * @post DRW engine has accepted the line; STATUS may be busy.
 * @post COLOR1 holds line->color_argb8888.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_drw_draw_line(const ra8_drw_line_t* line);

/**
 * @brief Issue an anti-aliased filled triangle.
 *
 * @details
 * Computes the three edge limiters and writes CONTROL with
 * LIM1..LIM3 enabled. HUM Ch 62.4.5 "Triangles" p 3726.
 *
 * @param[in] tri Triangle parameters. Must not be ``nullptr``.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok Triangle issued.
 * @retval k_ra8_err_null_ptr ``tri`` was nullptr.
 *
 * @pre ``tri`` non-null.
 * @pre Driver is initialized.
 * @post DRW engine has accepted the triangle.
 * @post COLOR1 holds tri->color_argb8888.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_drw_draw_triangle(const ra8_drw_triangle_t* tri);

/* =============================================================================
 * Display list mode
 * =============================================================================
 */

/**
 * @brief Trigger execution of a display list.
 *
 * @details
 * Writes DLISTSTART with the supplied address. The DRW pulls one
 * 32-bit word at a time from RAM and decodes it as (register index,
 * value) pairs until it hits a terminator. HUM Ch 62.6 "Display List
 * Mode" p 3724-3725.
 *
 * @param[in] dlist_addr Pointer to the first display-list word in
 * memory; must be 4-byte aligned and reside in
 * a region the DRW bus initiator can read.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok Display list trigger written.
 * @retval k_ra8_err_invalid_arg Address null or unaligned.
 *
 * @pre ``dlist_addr`` non-null and 4-byte aligned.
 * @pre Driver is initialized; CACHECTL flushed if RAM was just written.
 * @post DLISTSTART = dlist_addr; engine begins fetching.
 * @post On completion the DLISTIRQ flag will be raised in STATUS.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_drw_run_dlist(const uint32_t* dlist_addr);

/* =============================================================================
 * Performance counters
 * =============================================================================
 */

/**
 * @brief Programme PERFTRIGGER for the two performance counters and
 * reset both PERFCOUNTk registers.
 *
 * @param[in] event_ctr1 Trigger event for PERFCOUNT1.
 * @param[in] event_ctr2 Trigger event for PERFCOUNT2.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok Counters armed.
 * @retval k_ra8_err_invalid_arg Either event > 0x1F.
 *
 * @pre Driver has been initialized.
 * @pre Each event in [0..0x1F]; "Setting prohibited" values rejected
 * where they fall outside the documented set.
 * @post PERFTRIGGER carries both selectors; PERFCOUNT1 = PERFCOUNT2 = 0.
 * @post Counters begin incrementing on the next clock matching the
 * selected event.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_drw_perf_arm(ra8_drw_perftrigger_t event_ctr1,
                                         ra8_drw_perftrigger_t event_ctr2);

/**
 * @brief Read one PERFCOUNTk register.
 *
 * @param[in] id Counter identifier.
 * @param[out] out Receives the 32-bit count.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok Count read.
 * @retval k_ra8_err_null_ptr ``out`` was nullptr.
 * @retval k_ra8_err_invalid_arg ``id`` outside enum.
 *
 * @pre ``out`` non-null.
 * @pre Driver has been initialized.
 * @post ``*out`` holds the snapshot.
 * @post No DRW side effects.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_drw_perf_read(ra8_drw_perfcounter_id_t id, uint32_t* out);

/**
 * @brief Reset one PERFCOUNTk register to zero.
 *
 * @param[in] id Counter identifier.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok Counter cleared.
 * @retval k_ra8_err_invalid_arg ``id`` outside enum.
 *
 * @pre Driver has been initialized.
 * @pre ``id`` is one of ``k_ra8_drw_perfctr_*``.
 * @post PERFCOUNTid = 0.
 * @post No effect on the other counter.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_drw_perf_reset(ra8_drw_perfcounter_id_t id);

#ifdef __cplusplus
}
#endif
