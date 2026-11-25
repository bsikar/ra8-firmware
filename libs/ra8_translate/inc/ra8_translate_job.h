/**
 * @file ra8_translate_job.h
 * @brief C23 ABI for resumable background chapter translation scheduling (`TRJC`).
 * @since 0.1.0
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ra8_err.h"
#include "ra8_translate.h"
#include "ra8_translate_overlay.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Wire constants, queue bounds, and workspace limits for `chapter_job`. */
typedef enum : uint32_t {
  k_ra8_translate_job_ckpt_magic         = 0x434A5254U, /**< Little-endian "TRJC".        */
  k_ra8_translate_job_ckpt_version       = 1U,          /**< Checkpoint format version.   */
  k_ra8_translate_job_ckpt_header_bytes  = 32U,         /**< Fixed TRJC wire header size. */
  k_ra8_translate_job_max_queue          = 16U,         /**< Queued chapter slots.        */
  k_ra8_translate_job_max_pages          = 64U,         /**< Max pages per chapter job.   */
  k_ra8_translate_job_max_bubbles        = 256U,        /**< Max bubbles per chapter job. */
  k_ra8_translate_job_max_page_bubbles   = 64U,         /**< Max bubbles per single page. */
  k_ra8_translate_job_max_line_breaks    = 2048U,       /**< Max fitted line breaks total.*/
  k_ra8_translate_job_max_text_pool      = 16384U,      /**< Max UTF-8 pool bytes.        */
  k_ra8_translate_job_max_batch          = 16U,         /**< Max bubbles per NMT batch.   */
  k_ra8_translate_job_default_batch      = 8U,          /**< Default NMT batch width.     */
  k_ra8_translate_job_default_batt_pct   = 25U,         /**< Default battery pause floor. */
  k_ra8_translate_job_max_glossary_bytes = 3072U,       /**< Staging cap for TGL1 table.  */
} ra8_translate_job_const_t;

/** @brief Source trigger that queued a chapter translation job. */
typedef enum : uint8_t {
  k_ra8_translate_job_trigger_mdl_download = 1U, /**< Auto-queued after MDL download completes. */
  k_ra8_translate_job_trigger_user_request = 2U, /**< Explicitly requested by reader UI/user.   */
} ra8_translate_job_trigger_t;

/** @brief High-level scheduler operational state returned in job status. */
typedef enum : uint8_t {
  k_ra8_translate_job_state_idle           = 0U, /**< Queue empty and no active chapter.       */
  k_ra8_translate_job_state_running        = 1U, /**< Active chapter advancing pipeline steps. */
  k_ra8_translate_job_state_yielded        = 2U, /**< Paused cleanly for active reader/panel.  */
  k_ra8_translate_job_state_paused_battery = 3U, /**< Paused below battery floor while on batt.*/
  k_ra8_translate_job_state_completed      = 4U, /**< Active chapter just committed TRN1.      */
  k_ra8_translate_job_state_failed         = 5U, /**< Active chapter aborted with error status.*/
} ra8_translate_job_state_t;

/** @brief Active chapter pipeline execution stage persisted inside checkpoints. */
typedef enum : uint8_t {
  k_ra8_translate_job_stage_none           = 0U, /**< No active chapter loaded.           */
  k_ra8_translate_job_stage_detect_ocr     = 1U, /**< Running per-page detector + OCR.    */
  k_ra8_translate_job_stage_lang_glossary  = 2U, /**< Resolving language and glossary.    */
  k_ra8_translate_job_stage_translate_fit  = 3U, /**< Batch translating + fitting layout. */
  k_ra8_translate_job_stage_commit_sidecar = 4U, /**< Writing CRC-stamped TRN1 output.    */
  k_ra8_translate_job_stage_done           = 5U, /**< Sidecar committed and finalized.    */
} ra8_translate_job_stage_t;

/** @brief Specific environmental reason holding execution paused. */
typedef enum : uint8_t {
  k_ra8_translate_job_pause_none          = 0U, /**< Eligible to advance pipeline steps.  */
  k_ra8_translate_job_pause_reader_active = 1U, /**< Yielded while reader interaction runs*/
  k_ra8_translate_job_pause_display_busy  = 2U, /**< Yielded while e-ink panel updates.   */
  k_ra8_translate_job_pause_low_battery   = 3U, /**< Paused below battery threshold.      */
} ra8_translate_job_pause_reason_t;

/** @brief Per-chapter status returned by ::ra8_translate_job_query_chapter. */
typedef enum : uint8_t {
  k_ra8_translate_job_chapter_none        = 0U, /**< Not queued or active.                 */
  k_ra8_translate_job_chapter_queued      = 1U, /**< Waiting in queue behind current work. */
  k_ra8_translate_job_chapter_translating = 2U, /**< Active chapter currently running.     */
  k_ra8_translate_job_chapter_paused      = 3U, /**< Active chapter yielded or batt-paused.*/
} ra8_translate_job_chapter_state_t;

static_assert(sizeof(ra8_translate_job_trigger_t) == 1U, "trigger enum width");
static_assert(sizeof(ra8_translate_job_state_t) == 1U, "state enum width");
static_assert(sizeof(ra8_translate_job_stage_t) == 1U, "stage enum width");
static_assert(sizeof(ra8_translate_job_pause_reason_t) == 1U, "pause enum width");
static_assert(sizeof(ra8_translate_job_chapter_state_t) == 1U, "chapter state enum width");

/**
 * @brief Descriptor identifying one chapter queued or executing translation.
 * @since 0.1.0
 */
typedef struct {
  ra8_translate_sidecar_key_t sidecar_key;   /**< Content SHA-256 and model/glossary versions. */
  uint32_t                    title_id;      /**< Library/series identifier.                   */
  uint32_t                    chapter_order; /**< Ordinal within series (lower runs first).    */
  uint16_t                    page_count;    /**< Total pages in chapter (`1..max_pages`).     */
  ra8_translate_lang_t        lang_hint;     /**< Per-title metadata language hint.            */
  uint8_t                     unread;        /**< `1` if unread (prioritized), `0` if read.    */
  ra8_translate_job_trigger_t trigger;       /**< Download-complete or explicit user trigger.  */
  uint8_t                     reserved0[3];  /**< Reserved alignment padding; must be zero.    */
} ra8_translate_job_chapter_t;

static_assert(sizeof(ra8_translate_job_chapter_t) == 68U, "ra8_translate_job_chapter_t size");
static_assert(alignof(ra8_translate_job_chapter_t) == 4U, "ra8_translate_job_chapter_t align");
static_assert(offsetof(ra8_translate_job_chapter_t, sidecar_key) == 0U, "chapter sidecar_key off");
static_assert(offsetof(ra8_translate_job_chapter_t, title_id) == 52U, "chapter title_id off");
static_assert(offsetof(ra8_translate_job_chapter_t, chapter_order) == 56U,
              "chapter chapter_order off");
static_assert(offsetof(ra8_translate_job_chapter_t, page_count) == 60U, "chapter page_count off");
static_assert(offsetof(ra8_translate_job_chapter_t, lang_hint) == 62U, "chapter lang_hint off");
static_assert(offsetof(ra8_translate_job_chapter_t, unread) == 63U, "chapter unread off");
static_assert(offsetof(ra8_translate_job_chapter_t, trigger) == 64U, "chapter trigger off");
static_assert(offsetof(ra8_translate_job_chapter_t, reserved0) == 65U, "chapter reserved0 off");

/**
 * @brief Scheduling, power-threshold, batching, and overlay-fit configuration.
 * @since 0.1.0
 */
typedef struct {
  uint8_t                       battery_min_pct;        /**< Pause below pct off charger.       */
  uint8_t                       battery_hysteresis_pct; /**< Extra battery recovery margin.     */
  uint8_t                       nmt_batch_max;          /**< Max bubbles per NMT batch (`0=8`). */
  uint8_t                       yield_to_reader;        /**< Non-zero yields to active reader.  */
  uint8_t                       yield_to_display;       /**< Non-zero yields while display busy.*/
  uint8_t                       reserved0[3];           /**< Reserved; must be `{0, 0, 0}`.     */
  ra8_translate_overlay_style_t overlay_style;          /**< Overlay style used during fitting. */
} ra8_translate_job_policy_t;

static_assert(sizeof(ra8_translate_job_policy_t) == 32U, "ra8_translate_job_policy_t size");
static_assert(alignof(ra8_translate_job_policy_t) == 4U, "ra8_translate_job_policy_t align");
static_assert(offsetof(ra8_translate_job_policy_t, battery_min_pct) == 0U,
              "policy battery_min_pct off");
static_assert(offsetof(ra8_translate_job_policy_t, battery_hysteresis_pct) == 1U,
              "policy battery_hysteresis_pct off");
static_assert(offsetof(ra8_translate_job_policy_t, nmt_batch_max) == 2U,
              "policy nmt_batch_max off");
static_assert(offsetof(ra8_translate_job_policy_t, yield_to_reader) == 3U,
              "policy yield_to_reader off");
static_assert(offsetof(ra8_translate_job_policy_t, yield_to_display) == 4U,
              "policy yield_to_display off");
static_assert(offsetof(ra8_translate_job_policy_t, reserved0) == 5U, "policy reserved0 off");
static_assert(offsetof(ra8_translate_job_policy_t, overlay_style) == 8U,
              "policy overlay_style off");

/**
 * @brief Live system power and contention snapshot sampled at each job step.
 * @since 0.1.0
 */
typedef struct {
  uint8_t battery_pct;   /**< Current battery state-of-charge in `[0, 100]`. */
  uint8_t charging;      /**< Non-zero when external charger is supplying power. */
  uint8_t reader_active; /**< Non-zero when foreground reader interaction owns CPU. */
  uint8_t display_busy;  /**< Non-zero while panel update or waveform refresh runs. */
} ra8_translate_job_env_t;

static_assert(sizeof(ra8_translate_job_env_t) == 4U, "ra8_translate_job_env_t size");
static_assert(alignof(ra8_translate_job_env_t) == 1U, "ra8_translate_job_env_t align");
static_assert(offsetof(ra8_translate_job_env_t, battery_pct) == 0U, "env battery_pct off");
static_assert(offsetof(ra8_translate_job_env_t, charging) == 1U, "env charging off");
static_assert(offsetof(ra8_translate_job_env_t, reader_active) == 2U, "env reader_active off");
static_assert(offsetof(ra8_translate_job_env_t, display_busy) == 3U, "env display_busy off");

/**
 * @brief One detected and OCR-recognized bubble returned by `ocr_page` hook.
 * @since 0.1.0
 */
typedef struct {
  uint16_t                    x;                  /**< Bubble left X in page pixels.    */
  uint16_t                    y;                  /**< Bubble top Y in page pixels.     */
  uint16_t                    w;                  /**< Bubble width (`> 0`).            */
  uint16_t                    h;                  /**< Bubble height (`> 0`).           */
  ra8_translate_shape_t       shape;              /**< Bubble mask shape enum.          */
  ra8_translate_orientation_t text_orientation;   /**< Horizontal vs vertical orientation.*/
  uint16_t                    corner_radius;      /**< Rounded-rect corner radius.      */
  uint16_t                    reading_order;      /**< Unique per-page reading index.   */
  uint16_t                    ocr_confidence_bps; /**< OCR confidence `[0..10000]` bps. */
  uint32_t                    source_off;         /**< Byte offset into `out_text_buf`. */
  uint32_t                    source_len;         /**< Byte length in `out_text_buf`.   */
} ra8_translate_job_ocr_bubble_t;

static_assert(sizeof(ra8_translate_job_ocr_bubble_t) == 24U, "ra8_translate_job_ocr_bubble_t size");
static_assert(alignof(ra8_translate_job_ocr_bubble_t) == 4U,
              "ra8_translate_job_ocr_bubble_t align");
static_assert(offsetof(ra8_translate_job_ocr_bubble_t, x) == 0U, "ocr_bubble x off");
static_assert(offsetof(ra8_translate_job_ocr_bubble_t, shape) == 8U, "ocr_bubble shape off");
static_assert(offsetof(ra8_translate_job_ocr_bubble_t, reading_order) == 12U,
              "ocr_bubble reading_order off");
static_assert(offsetof(ra8_translate_job_ocr_bubble_t, source_off) == 16U,
              "ocr_bubble source_off off");
static_assert(offsetof(ra8_translate_job_ocr_bubble_t, source_len) == 20U,
              "ocr_bubble source_len off");

/**
 * @brief Byte offset and length span inside a flat batch text buffer.
 * @since 0.1.0
 */
typedef struct {
  uint32_t off; /**< Byte offset inside concatenated batch buffer. */
  uint32_t len; /**< Byte length of item UTF-8 slice.              */
} ra8_translate_job_span_t;

static_assert(sizeof(ra8_translate_job_span_t) == 8U, "ra8_translate_job_span_t size");
static_assert(alignof(ra8_translate_job_span_t) == 4U, "ra8_translate_job_span_t align");
static_assert(offsetof(ra8_translate_job_span_t, off) == 0U, "span off offset");
static_assert(offsetof(ra8_translate_job_span_t, len) == 4U, "span len offset");

/**
 * @brief Observable scheduler snapshot populated by step and status queries.
 * @since 0.1.0
 */
typedef struct {
  uint32_t                         active_title_id;       /**< Active title ID (`0` if none). */
  uint32_t                         active_chapter_order;  /**< Active chapter ordinal.        */
  uint32_t                         checkpoint_seq;        /**< Monotonic saved ckpt counter.  */
  uint16_t                         queued_count;          /**< Pending chapters in queue.     */
  uint16_t                         page_count;            /**< Active chapter total pages.    */
  uint16_t                         pages_ocr_done;        /**< Pages OCR-completed so far.    */
  uint16_t                         pages_translated_done; /**< Pages NMT+fitted so far.       */
  uint16_t                         last_error;            /**< Last failure `ra8_err_t` code. */
  ra8_translate_job_state_t        state;                 /**< Current scheduler state.       */
  ra8_translate_job_stage_t        stage;                 /**< Active chapter pipeline stage. */
  ra8_translate_job_pause_reason_t pause_reason;          /**< Why execution is paused.       */
  ra8_translate_lang_t             resolved_lang;         /**< Resolved source language.      */
  uint8_t                          has_active;            /**< Non-zero if active slot valid. */
  uint8_t                          reserved0[5];          /**< Zero padding bytes.            */
} ra8_translate_job_status_t;

static_assert(sizeof(ra8_translate_job_status_t) == 32U, "ra8_translate_job_status_t size");
static_assert(alignof(ra8_translate_job_status_t) == 4U, "ra8_translate_job_status_t align");
static_assert(offsetof(ra8_translate_job_status_t, active_title_id) == 0U,
              "status active_title_id off");
static_assert(offsetof(ra8_translate_job_status_t, active_chapter_order) == 4U,
              "status active_chapter_order off");
static_assert(offsetof(ra8_translate_job_status_t, checkpoint_seq) == 8U,
              "status checkpoint_seq off");
static_assert(offsetof(ra8_translate_job_status_t, queued_count) == 12U, "status queued_count off");
static_assert(offsetof(ra8_translate_job_status_t, last_error) == 20U, "status last_error off");
static_assert(offsetof(ra8_translate_job_status_t, state) == 22U, "status state off");
static_assert(offsetof(ra8_translate_job_status_t, has_active) == 26U, "status has_active off");

/**
 * @brief Platform, model, persistence, and font metric hooks injected by host/RTOS.
 * @since 0.1.0
 */
typedef struct {
  void*                     user;  /**< Opaque caller context passed to hooks.         */
  const ra8_widget_paint_t* paint; /**< Widget text-measure hooks for layout fitting.  */
  ra8_err_t (*ocr_page)(void*                              user,
                        const ra8_translate_job_chapter_t* chapter,
                        uint16_t                           page_index,
                        uint16_t                           max_bubbles,
                        uint16_t*                          out_page_w,
                        uint16_t*                          out_page_h,
                        ra8_translate_job_ocr_bubble_t*    out_bubbles,
                        uint16_t*                          out_bubble_count,
                        uint8_t*                           out_text_buf,
                        size_t                             text_cap,
                        size_t* out_text_len); /**< Detect + OCR one page. */
  ra8_err_t (*translate_term)(void*                user,
                              ra8_translate_lang_t source_lang,
                              const uint8_t*       src_utf8,
                              size_t               src_len,
                              uint8_t*             out_eng_utf8,
                              size_t               out_eng_cap,
                              size_t*              out_eng_len); /**< Optional candidate NMT. */
  ra8_err_t (*translate_batch)(void*                           user,
                               ra8_translate_nmt_model_t       nmt_model,
                               const uint8_t*                  src_buf,
                               size_t                          src_bytes,
                               const ra8_translate_job_span_t* src_spans,
                               uint16_t                        item_count,
                               uint8_t*                        out_eng_buf,
                               size_t                          out_eng_cap,
                               ra8_translate_job_span_t* out_eng_spans); /**< Batched NMT decode. */
  ra8_err_t (*load_glossary)(void*    user,
                             uint32_t title_id,
                             uint8_t* out_tgl_buf,
                             size_t   out_tgl_cap,
                             size_t*  out_tgl_len); /**< Optional persisted TGL1 loader. */
  ra8_err_t (*save_checkpoint)(void*          user,
                               const uint8_t* ckpt_bytes,
                               size_t         ckpt_len); /**< Optional per-page NV checkpoint. */
  ra8_err_t (*commit_chapter)(void*                              user,
                              const ra8_translate_job_chapter_t* chapter,
                              const uint8_t*                     trn1_bytes,
                              size_t                             trn1_len,
                              const uint8_t*                     tgl1_bytes,
                              size_t tgl1_len); /**< Sidecar + glossary write. */
} ra8_translate_job_hooks_t;

static_assert(sizeof(ra8_translate_job_hooks_t) == sizeof(void*) * 8U,
              "ra8_translate_job_hooks_t size");
static_assert(alignof(ra8_translate_job_hooks_t) == alignof(void*),
              "ra8_translate_job_hooks_t align");

/**
 * @brief Query the caller workspace byte size required by ::ra8_translate_job_init.
 *
 * @details Calling context: `task-safe-reentrant`. Includes alignment margin so
 * any byte array of size `*out_bytes` aligns safely without trap on ARM.
 *
 * @param[out] out_bytes Non-null out-pointer receiving workspace byte count.
 * @return ::k_ra8_ok on success, or `k_ra8_err_null_ptr` if @p out_bytes is NULL.
 * @since 0.1.0
 */
ra8_err_t ra8_translate_job_work_bytes(size_t* out_bytes);

/**
 * @brief Initialize a caller-owned chapter job workspace with scheduling policy.
 *
 * @details Calling context: `task-safe-reentrant`. Borrows @p policy and binds
 * caller workspace @p work_buf with zero dynamic memory allocation.
 *
 * @param[out] work_buf Non-null workspace buffer of at least `ra8_translate_job_work_bytes()`.
 * @param[in] work_cap Byte capacity of @p work_buf.
 * @param[in] policy Non-null scheduler configuration.
 * @return ::k_ra8_ok on success, or explicit error status without side effects.
 * @since 0.1.0
 */
ra8_err_t ra8_translate_job_init(uint8_t*                          work_buf,
                                 size_t                            work_cap,
                                 const ra8_translate_job_policy_t* policy);

/**
 * @brief Enqueue a chapter from MDL download completion or explicit user request.
 *
 * @details Calling context: `task-only-non-reentrant`. Coalesces duplicates by
 * `(title_id, chapter_order)` and prioritizes unread chapters with lowest
 * `chapter_order` first once any running chapter finishes.
 *
 * @param[in,out] work_buf Initialized job workspace.
 * @param[in] work_cap Byte capacity of @p work_buf.
 * @param[in] chapter Non-null chapter descriptor.
 * @return ::k_ra8_ok on queue admission or coalesce, or explicit error status.
 * @since 0.1.0
 */
ra8_err_t ra8_translate_job_enqueue(uint8_t*                           work_buf,
                                    size_t                             work_cap,
                                    const ra8_translate_job_chapter_t* chapter);

/**
 * @brief Cancel a queued or active chapter matching `(title_id, chapter_order)`.
 *
 * @details Calling context: `task-only-non-reentrant`. If the cancelled chapter
 * is active and @p hooks supplies `save_checkpoint`, clears persisted NV state
 * via `(NULL, 0)` / `(empty, 0)`. Returns `k_ra8_err_not_found` if absent.
 *
 * @param[in,out] work_buf Initialized job workspace.
 * @param[in] work_cap Byte capacity of @p work_buf.
 * @param[in] title_id Title identifier.
 * @param[in] chapter_order Chapter ordinal.
 * @param[in] hooks Optional hooks used to clear active NV checkpoint (`may be NULL`).
 * @return ::k_ra8_ok on removal, or `k_ra8_err_not_found` / validation error.
 * @since 0.1.0
 */
ra8_err_t ra8_translate_job_cancel(uint8_t*                         work_buf,
                                   size_t                           work_cap,
                                   uint32_t                         title_id,
                                   uint32_t                         chapter_order,
                                   const ra8_translate_job_hooks_t* hooks);

/**
 * @brief Restore an interrupted chapter job from a validated `TRJC` checkpoint.
 *
 * @details Calling context: `task-only-non-reentrant`. Validates magic, version,
 * CRC-32, `TGL1` glossary, page/bubble/break bounds, and UTF-8 strings before
 * modifying @p work_buf. Preserves queued chapters already registered in @p work_buf.
 *
 * @param[in,out] work_buf Initialized job workspace.
 * @param[in] work_cap Byte capacity of @p work_buf.
 * @param[in] ckpt_bytes Persisted `TRJC` checkpoint bytes.
 * @param[in] ckpt_len Byte length of @p ckpt_bytes.
 * @return ::k_ra8_ok on restore, or `k_ra8_err_crc_mismatch` / validation error.
 * @since 0.1.0
 */
ra8_err_t ra8_translate_job_restore_checkpoint(uint8_t*       work_buf,
                                               size_t         work_cap,
                                               const uint8_t* ckpt_bytes,
                                               size_t         ckpt_len);

/**
 * @brief Execute at most one bounded scheduling or per-page pipeline increment.
 *
 * @details Calling context: `task-only-non-reentrant`. Enforces reader/display
 * yield and battery policy first, activates the highest-priority unread chapter
 * when idle, runs one stage increment (one OCR page, language/glossary pass, one
 * translated+fitted page batch, or final `TRN1` commit), and emits an NV
 * checkpoint per page.
 *
 * @param[in,out] work_buf Initialized job workspace.
 * @param[in] work_cap Byte capacity of @p work_buf.
 * @param[in] env Non-null current power and display/reader contention sample.
 * @param[in] hooks Non-null injected pipeline callbacks and `paint` handle.
 * @param[out] out_status Non-null status struct receiving updated state.
 * @return ::k_ra8_ok on clean step or pause, or explicit error status.
 * @since 0.1.0
 */
ra8_err_t ra8_translate_job_step(uint8_t*                         work_buf,
                                 size_t                           work_cap,
                                 const ra8_translate_job_env_t*   env,
                                 const ra8_translate_job_hooks_t* hooks,
                                 ra8_translate_job_status_t*      out_status);

/**
 * @brief Read current scheduler state without mutating active or queued work.
 *
 * @details Calling context: `task-safe-reentrant`.
 *
 * @param[in] work_buf Initialized job workspace.
 * @param[in] work_cap Byte capacity of @p work_buf.
 * @param[out] out_status Non-null caller storage receiving scheduler snapshot.
 * @return ::k_ra8_ok on success, or explicit error status.
 * @since 0.1.0
 */
ra8_err_t ra8_translate_job_status(const uint8_t*              work_buf,
                                   size_t                      work_cap,
                                   ra8_translate_job_status_t* out_status);

/**
 * @brief Query whether `(title_id, chapter_order)` is queued or actively translating.
 *
 * @details Calling context: `task-safe-reentrant`. Intended for reader shelf/chapter
 * UI markers without interrupting background execution.
 *
 * @param[in] work_buf Initialized job workspace.
 * @param[in] work_cap Byte capacity of @p work_buf.
 * @param[in] title_id Title identifier.
 * @param[in] chapter_order Chapter ordinal.
 * @param[out] out_state Non-null storage receiving ::ra8_translate_job_chapter_state_t.
 * @return ::k_ra8_ok on success, or explicit error status.
 * @since 0.1.0
 */
ra8_err_t ra8_translate_job_query_chapter(const uint8_t*                     work_buf,
                                          size_t                             work_cap,
                                          uint32_t                           title_id,
                                          uint32_t                           chapter_order,
                                          ra8_translate_job_chapter_state_t* out_state);

#ifdef __cplusplus
}
#endif
