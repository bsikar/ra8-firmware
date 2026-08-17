/**
 * @file mdl_app_direct.c
 * @brief Direct artifact, page, pack, and run composition actions.
 * @details Composes validated direct-download and offline-pack modes over the
 *          shared storage, network, governor, and exporter dependencies.
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */
#include "mdl_app_internal.h"
#include "mdl_export_io_internal.h"
#include "mdl_fetch_body_internal.h"

/** @brief Transactional body sink for one directly downloaded artifact. */
typedef struct {
  mdl_export_output_t output;      /**< Structurally validated output stage. */
  mdl_storage_t*      storage;     /**< Exclusive portable storage binding.  */
  const char*         destination; /**< Canonical final artifact path.       */
  ra8_mdl_format_t    format;      /**< Exact canonical verifier selection.  */
} mdl_direct_artifact_sink_t;

/** @brief Append three borrowed direct-action fragments to one stream.
 * @details Uses caller-owned application and transaction state.
 *          Preserves the first network, storage, export, or presentation failure.
 * @param[in,out] stream Destination stream state.
 * @param[in] first First text fragment.
 * @param[in] second Second text fragment.
 * @param[in] third Third text fragment.
 * @return Operation status.
 * @retval k_ra8_ok The operation completed successfully.
 * @retval other The originating validation, storage, stream, or network error.
 * @pre Every required pointer is non-null and remains valid for the call.
 * @pre Lengths and capacities describe complete referenced objects without overflow.
 * @post Documented outputs and the return value describe the same outcome.
 * @post A rejected or failed operation is never reported as successful.
 * @note Thread safety follows ownership of the supplied context; no synchronization is added.
 * @since Version 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_direct_text3(ra8_io_stream_t* stream,
                                                    const char*      first,
                                                    const char*      second,
                                                    const char*      third)
{
  ra8_err_t error = priv_mdl_stream_text(k_ra8_ok, stream, first);
  error           = priv_mdl_stream_text(error, stream, second);
  return priv_mdl_stream_text(error, stream, third);
}

/** @brief Latch the first direct presenter failure for loop cancellation.
 * @details Uses caller-owned application and transaction state.
 *          Preserves the first network, storage, export, or presentation failure.
 * @param[in,out] error Error accumulator or error value.
 * @pre Every required pointer is non-null and remains valid for the call.
 * @pre Lengths and capacities describe complete referenced objects without overflow.
 * @post Documented outputs and the return value describe the same outcome.
 * @post A rejected or failed operation is never reported as successful.
 * @note Thread safety follows ownership of the supplied context; no synchronization is added.
 * @since Version 0.1.0
 */
RA8_INTERNAL static void internal_direct_latch(ra8_err_t error)
{
  if ((error != k_ra8_ok) && (priv_mdl_app_context()->io_error == k_ra8_ok)) {
    priv_mdl_app_context()->io_error = error;
  }
}

/** @brief Report one pack exporter failure with its exact hexadecimal status.
 * @details Uses caller-owned application and transaction state.
 *          Preserves the first network, storage, export, or presentation failure.
 * @param[in] dir Directory path or handle for the operation.
 * @param[in] ext Filename extension without implicit allocation.
 * @param[out] failure Receives the originating failure.
 * @return Operation status.
 * @retval k_ra8_ok The operation completed successfully.
 * @retval other The originating validation, storage, stream, or network error.
 * @pre Every required pointer is non-null and remains valid for the call.
 * @pre Lengths and capacities describe complete referenced objects without overflow.
 * @post Documented outputs and the return value describe the same outcome.
 * @post A rejected or failed operation is never reported as successful.
 * @note Thread safety follows ownership of the supplied context; no synchronization is added.
 * @since Version 0.1.0
 */
RA8_INTERNAL static ra8_err_t
internal_direct_pack_failure(const char* dir, const char* ext, ra8_err_t failure)
{
  ra8_io_stream_t* diagnostic = priv_mdl_app_context()->diagnostic;
  ra8_err_t        error = internal_direct_text3(diagnostic, "media_dl: pack '", dir, "' as .");
  error                  = priv_mdl_stream_text(error, diagnostic, ext);
  error                  = priv_mdl_stream_text(error, diagnostic, " FAILED (err 0x");
  error                  = priv_mdl_stream_hex(error, diagnostic, (uint32_t)failure);
  return priv_mdl_stream_text(error, diagnostic, ")\n");
}

/** @brief Abort a prior attempt and begin a fresh artifact transaction.
 * @details Uses caller-owned application and transaction state.
 *          Preserves the first network, storage, export, or presentation failure.
 * @param[in,out] context Caller-owned operation context.
 * @return Operation status.
 * @retval k_ra8_ok The operation completed successfully.
 * @retval other The originating validation, storage, stream, or network error.
 * @pre Every required pointer is non-null and remains valid for the call.
 * @pre Lengths and capacities describe complete referenced objects without overflow.
 * @post Documented outputs and the return value describe the same outcome.
 * @post A rejected or failed operation is never reported as successful.
 * @note Thread safety follows ownership of the supplied context; no synchronization is added.
 * @since Version 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_direct_artifact_reset(void* context)
{
  mdl_direct_artifact_sink_t* sink = (mdl_direct_artifact_sink_t*)context;
  if ((sink == nullptr) || (sink->storage == nullptr) || (sink->destination == nullptr)) {
    return k_ra8_err_invalid_arg;
  }
  if (sink->output.writer.transaction.active) {
    const ra8_err_t aborted = priv_mdl_export_output_abort(&sink->output);
    if (aborted != k_ra8_ok) {
      return aborted;
    }
  }
  return priv_mdl_export_output_begin(&sink->output,
                                      sink->storage,
                                      sink->destination,
                                      sink->format);
}

/** @brief Append one complete response chunk to the portable artifact stage.
 * @details Uses caller-owned application and transaction state.
 *          Preserves the first network, storage, export, or presentation failure.
 * @param[in,out] context Caller-owned operation context.
 * @param[in] bytes Readable byte span.
 * @param[in] length Byte length of the supplied span.
 * @param[out] out_written Receives the committed byte count.
 * @return Operation status.
 * @retval k_ra8_ok The operation completed successfully.
 * @retval other The originating validation, storage, stream, or network error.
 * @pre Every required pointer is non-null and remains valid for the call.
 * @pre Lengths and capacities describe complete referenced objects without overflow.
 * @post Documented outputs and the return value describe the same outcome.
 * @post A rejected or failed operation is never reported as successful.
 * @note Thread safety follows ownership of the supplied context; no synchronization is added.
 * @since Version 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_direct_artifact_write(void*          context,
                                                             const uint8_t* bytes,
                                                             uint32_t       length,
                                                             uint32_t*      out_written)
{
  mdl_direct_artifact_sink_t* sink = (mdl_direct_artifact_sink_t*)context;
  if ((sink == nullptr) || (out_written == nullptr) || ((bytes == nullptr) && (length != 0U))) {
    return k_ra8_err_invalid_arg;
  }
  *out_written        = 0U;
  const ra8_err_t err = priv_mdl_export_output_write(&sink->output, bytes, length);
  if (err == k_ra8_ok) {
    *out_written = length;
  }
  return err;
}

/**
 * @brief Return the larger of two unsigned values.
 * @details Performs the comparison without arithmetic or narrowing.
 * @param[in] a First value.
 * @param[in] b Second value.
 * @return The larger input value.
 * @retval a @p a is greater than @p b.
 * @retval b @p b is greater than or equal to @p a.
 * @pre Both inputs are valid uint32_t values.
 * @pre No ordering relationship is required.
 * @post The result equals one input.
 * @post Neither input nor shared state is modified.
 * @note Thread-safe and side-effect free.
 * @since 0.1.0
 */
RA8_INTERNAL static uint32_t internal_max_u32(uint32_t a, uint32_t b)
{
  return (a > b) ? a : b;
}

/**
 * @brief Remove stale page files whose extensions differ from verified bytes.
 * @details Enumerates the bounded supported image extensions, constructs each
 *          sibling leaf with checked formatting/joining, and unlinks every
 *          alternate while preserving the just-published true extension.
 * @param[in] out_dir Output directory containing page-mode files.
 * @param[in] page_no One-based page number used in the fixed leaf pattern.
 * @param[in] true_ext Verified extension that must remain present.
 * @pre @p out_dir and @p true_ext are non-NULL and NUL-terminated.
 * @pre @p true_ext names one supported image extension.
 * @post Every resolvable alternate page leaf has been unlinked or reported.
 * @post The leaf ending in @p true_ext is never removed.
 * @note Not safe for concurrent writers targeting the same page number.
 * @since 0.1.0
 */
RA8_INTERNAL static void
internal_remove_stale_page_variants(const char* out_dir, size_t page_no, const char* true_ext)
{
  static const char* const alternate_exts[] = {"jpg", "jpeg", "png", "webp", "gif", "bmp"};
  for (size_t ext_idx = 0U; ext_idx < (sizeof(alternate_exts) / sizeof(alternate_exts[0]));
       ++ext_idx) {
    if (strcmp(alternate_exts[ext_idx], true_ext) == 0) {
      continue;
    }
    char      alternate_leaf[k_leaf_name_bytes] = {};
    const int alternate_len                     = snprintf(alternate_leaf,
                                                           sizeof(alternate_leaf),
                                                           "page_%04zu.%s",
                                                           page_no,
                                                           alternate_exts[ext_idx]);
    char      alternate_path[k_file_path_bytes];
    if ((alternate_len <= 0) || ((size_t)alternate_len >= sizeof(alternate_leaf)) ||
        !mdl_path_join(out_dir, alternate_leaf, alternate_path, sizeof(alternate_path))) {
      internal_direct_latch(internal_direct_text3(priv_mdl_app_context()->diagnostic,
                                                  "media_dl: warning: cannot resolve stale page "
                                                  "variant '",
                                                  alternate_leaf,
                                                  "'\n"));
      if (priv_mdl_app_context()->io_error != k_ra8_ok) {
        return;
      }
      continue;
    }
    bool            removed = false;
    const ra8_err_t error   = priv_mdl_app_storage_unlink_regular(&priv_mdl_app_context()->storage,
                                                                  alternate_path,
                                                                  &removed);
    (void)removed;
    if (error != k_ra8_ok) {
      internal_direct_latch(internal_direct_text3(priv_mdl_app_context()->diagnostic,
                                                  "media_dl: warning: cannot safely remove stale "
                                                  "page variant '",
                                                  alternate_path,
                                                  "'\n"));
      if (priv_mdl_app_context()->io_error != k_ra8_ok) {
        return;
      }
    }
  }
}

/**
 * @brief Validate and publish one staged page-mode image.
 * @details Performs publish page image under the injected network, governor,
 * and storage contracts; dependency failures are propagated before incomplete
 * bytes are published.
 * @param[in,out] body Complete transaction-owned response body.
 * @param[in] out_dir Canonical output directory.
 * @param[in] idx Zero-based page image index.
 * @return Failure contribution for the page tally.
 * @retval 0 Supported bytes were atomically published.
 * @retval 1 Signature, path, or publication validation failed.
 * @pre @p body and @p out_dir are non-NULL and @p body owns the response stage.
 * @post Success leaves only the verified true-extension sibling.
 * @post Failure removes the holding file whenever it still exists.
 * @note Not safe for concurrent writers targeting the same page index.
 * @since 0.1.0

 * @pre Every required pointer is non-null and remains valid for the call.
 */
RA8_INTERNAL static size_t
internal_publish_page_image(mdl_fetch_body_t* body, const char* out_dir, size_t idx)
{
  ra8_err_t error = priv_mdl_fetch_body_prepare(body);
  if (error != k_ra8_ok) {
    (void)priv_mdl_fetch_body_abort(body);
    ra8_err_t output_error =
      priv_mdl_stream_text(k_ra8_ok, priv_mdl_app_context()->diagnostic, "  page ");
    output_error = priv_mdl_stream_u64(output_error, priv_mdl_app_context()->diagnostic, idx + 1U);
    output_error =
      priv_mdl_stream_text(output_error, priv_mdl_app_context()->diagnostic, " FAILED ");
    output_error = priv_mdl_stream_text(output_error,
                                        priv_mdl_app_context()->diagnostic,
                                        priv_mdl_app_context()->images.urls[idx]);
    output_error = priv_mdl_stream_text(output_error,
                                        priv_mdl_app_context()->diagnostic,
                                        " -- unsupported image signature\n");
    internal_direct_latch(output_error);
    return 1U;
  }
  const char* const dot = strrchr(body->actual_abs, '.');
  if ((dot == nullptr) || (dot[1] == '\0')) {
    (void)priv_mdl_fetch_body_abort(body);
    return 1U;
  }
  char      true_ext[k_ext_bytes];
  const int extension = snprintf(true_ext, sizeof(true_ext), "%s", dot + 1);
  if ((extension <= 0) || ((size_t)extension >= sizeof(true_ext))) {
    (void)priv_mdl_fetch_body_abort(body);
    return 1U;
  }
  error = priv_mdl_fetch_body_commit(body);
  if (error != k_ra8_ok) {
    ra8_err_t output_error =
      priv_mdl_stream_text(k_ra8_ok, priv_mdl_app_context()->diagnostic, "  page ");
    output_error = priv_mdl_stream_u64(output_error, priv_mdl_app_context()->diagnostic, idx + 1U);
    output_error = priv_mdl_stream_text(output_error,
                                        priv_mdl_app_context()->diagnostic,
                                        " FAILED -- atomic publication failed\n");
    internal_direct_latch(output_error);
    return 1U;
  }
  internal_remove_stale_page_variants(out_dir, idx + 1U, true_ext);
  return (priv_mdl_app_context()->io_error == k_ra8_ok) ? 0U : 1U;
}

/**
 * @brief Gate, space, and download one page-mode image.
 * @details Enforces session policy and crawl delay, downloads to a holding
 *          leaf, requires a supported byte signature, then durably publishes
 *          `page_NNNN.<true-ext>`. URL suffixes and Content-Type cannot
 * override the bytes, and no partial/unknown file is presented as an image.
 * @param[in] url Referrer page URL.
 * @param[in] out_dir Output directory.
 * @param[in] dmin Minimum request delay in milliseconds.
 * @param[in] dmax Maximum request delay in milliseconds.
 * @param[in] timeout Per-request timeout in milliseconds.
 * @param[in,out] pol Initialized politeness state.
 * @param[in] idx Index into ::priv_mdl_app_context()->images.
 * @return Failure contribution for the page tally.
 * @retval 0 The image downloaded successfully.
 * @retval 1 Policy refusal or network/file failure occurred.
 * @pre @p url, @p out_dir, and @p pol are non-NULL.
 * @pre @p idx is smaller than `priv_mdl_app_context()->images.count`.
 * @post Success leaves one image file in @p out_dir.
 * @post Failure is reported and contributes exactly one failure.
 * @note Not thread-safe because it reads the shared session/image table.
 * @since 0.1.0
 */
RA8_INTERNAL static size_t internal_download_page_image(const char*       url,
                                                        const char*       out_dir,
                                                        uint32_t          dmin,
                                                        uint32_t          dmax,
                                                        uint32_t          timeout,
                                                        mdl_politeness_t* pol,
                                                        size_t            idx)
{
  uint32_t crawl = 0U;
  if (!mdl_session_url_allowed(&priv_mdl_app_context()->session,
                               priv_mdl_app_context()->images.urls[idx],
                               &crawl)) {
    return 1U;
  }
  (void)mdl_politeness_wait(pol, internal_max_u32(dmin, crawl), internal_max_u32(dmax, crawl));
  char      holding_leaf[k_leaf_name_bytes];
  const int holding_len =
    snprintf(holding_leaf, sizeof(holding_leaf), "page_%04zu.download", idx + 1U);
  char holding[k_file_path_bytes];
  if ((holding_len < 0) || ((size_t)holding_len >= sizeof(holding_leaf)) ||
      !mdl_path_join(out_dir, holding_leaf, holding, sizeof(holding))) {
    return 1U;
  }
  const mdl_net_req_t ir   = {.user_agent = priv_mdl_app_context()->session.user_agent,
                              .referer    = url,
                              .timeout_ms = timeout};
  size_t              got  = 0U;
  mdl_net_resp_t      resp = {};
  mdl_fetch_body_t    body = {};
  ra8_err_t           rc =
    priv_mdl_fetch_body_init_image(&body, &priv_mdl_app_context()->storage, holding, holding_leaf);
  mdl_net_body_sink_t sink = priv_mdl_fetch_body_sink(&body);
  if (rc == k_ra8_ok) {
    rc = mdl_net_get_body(priv_mdl_app_context()->session.net,
                          priv_mdl_app_context()->images.urls[idx],
                          &ir,
                          &sink,
                          &got,
                          &resp);
  }
  if (rc != k_ra8_ok) {
    (void)priv_mdl_fetch_body_abort(&body);
    char reason[k_mdl_reason_max];
    priv_mdl_fetch_reason(rc, resp.status, reason, sizeof(reason));
    ra8_err_t output_error =
      priv_mdl_stream_text(k_ra8_ok, priv_mdl_app_context()->diagnostic, "  page ");
    output_error = priv_mdl_stream_u64(output_error, priv_mdl_app_context()->diagnostic, idx + 1U);
    output_error =
      priv_mdl_stream_text(output_error, priv_mdl_app_context()->diagnostic, " FAILED ");
    output_error = priv_mdl_stream_text(output_error,
                                        priv_mdl_app_context()->diagnostic,
                                        priv_mdl_app_context()->images.urls[idx]);
    output_error = priv_mdl_stream_text(output_error, priv_mdl_app_context()->diagnostic, " -- ");
    output_error = priv_mdl_stream_text(output_error, priv_mdl_app_context()->diagnostic, reason);
    output_error = priv_mdl_stream_text(output_error, priv_mdl_app_context()->diagnostic, "\n");
    internal_direct_latch(output_error);
    return 1U;
  }
  return internal_publish_page_image(&body, out_dir, idx);
}

/**
 * @brief Download the extracted page images into an output directory.
 * @details Initializes deterministic delay state, applies the configured page
 *          limit, downloads each row, and prints the final tally.
 * @param[in] url Referrer page URL.
 * @param[in] out_dir Output directory.
 * @param[in] max_imgs Maximum images, or zero for every extracted row.
 * @param[in] seed Politeness jitter seed.
 * @param[in] timeout Per-request timeout in milliseconds.
 * @param[in] polite Whether conservative delay bounds apply.
 * @return Number of refused or failed images.
 * @retval 0 Every attempted image succeeded.
 * @retval positive One or more attempted images failed.
 * @pre @p url and @p out_dir are non-NULL.
 * @pre ::priv_mdl_app_context()->session and ::priv_mdl_app_context()->images
 * are prepared.
 * @post At most @p max_imgs rows are attempted when it is nonzero.
 * @post A complete success/failure tally is printed.
 * @note Not thread-safe because it uses shared image/session state.
 * @since 0.1.0
 */
RA8_INTERNAL static size_t internal_download_page_images(const char* url,
                                                         const char* out_dir,
                                                         uint32_t    max_imgs,
                                                         uint64_t    seed,
                                                         uint32_t    timeout,
                                                         bool        polite)
{
  mdl_politeness_t pol;
  mdl_politeness_init(&pol, seed);
  const uint32_t dmin  = polite ? (uint32_t)k_polite_img_min_ms : (uint32_t)k_page_img_delay_min;
  const uint32_t dmax  = polite ? (uint32_t)k_polite_img_max_ms : (uint32_t)k_page_img_delay_max;
  const size_t   limit = (max_imgs == 0U) ? priv_mdl_app_context()->images.count : (size_t)max_imgs;
  size_t         fail  = 0U;
  for (size_t i = 0U; (i < priv_mdl_app_context()->images.count) && (i < limit); ++i) {
    fail += internal_download_page_image(url, out_dir, dmin, dmax, timeout, &pol, i);
    if (priv_mdl_app_context()->io_error != k_ra8_ok) {
      return fail;
    }
  }
  ra8_err_t output_error = priv_mdl_stream_text(k_ra8_ok, priv_mdl_app_context()->output, "done: ");
  output_error           = priv_mdl_stream_u64(
    output_error,
    priv_mdl_app_context()->output,
    (priv_mdl_app_context()->images.count < limit ? priv_mdl_app_context()->images.count : limit) -
      fail);
  output_error = priv_mdl_stream_text(output_error, priv_mdl_app_context()->output, " ok, ");
  output_error = priv_mdl_stream_u64(output_error, priv_mdl_app_context()->output, fail);
  output_error =
    priv_mdl_stream_text(output_error, priv_mdl_app_context()->output, " failed, into ");
  output_error = priv_mdl_stream_text(output_error, priv_mdl_app_context()->output, out_dir);
  output_error = priv_mdl_stream_text(output_error, priv_mdl_app_context()->output, "/\n");
  internal_direct_latch(output_error);
  return fail;
}

/**
 * @brief Create and canonicalize one direct-action output directory.
 * @details Coordinates prepare output dir with fixed application workspaces and
 * propagates validation, storage, or network failure to the selected command
 * runner.
 * @param[in] out_dir Requested output directory.
 * @param[out] out_abs Canonical absolute output path.
 * @return Whether @p out_abs names a real directory.
 * @retval true The directory exists and was canonicalized.
 * @retval false Creation, resolution, or type validation failed.
 * @pre @p out_dir and @p out_abs are non-NULL.
 * @pre @p out_abs has capacity `PATH_MAX`.
 * @post Success leaves a real directory at @p out_abs.
 * @post Failure emits one diagnostic and callers perform no network work.
 * @note Not safe for concurrent replacement of @p out_dir.
 * @since 0.1.0
 */
RA8_INTERNAL static bool internal_prepare_output_dir(const char* out_dir, char* out_abs)
{
  if (priv_mdl_app_storage_ensure_directory(&priv_mdl_app_context()->storage, out_dir) !=
      k_ra8_ok) {
    internal_direct_latch(internal_direct_text3(priv_mdl_app_context()->diagnostic,
                                                "media_dl: cannot create output directory '",
                                                out_dir,
                                                "'\n"));
    return false;
  }
  const int copied = snprintf(out_abs, (size_t)k_fw_fs_path_cap, "%s", out_dir);
  if ((copied <= 0) || ((size_t)copied >= (size_t)k_fw_fs_path_cap)) {
    internal_direct_latch(internal_direct_text3(priv_mdl_app_context()->diagnostic,
                                                "media_dl: output path is not a directory: '",
                                                out_dir,
                                                "'\n"));
    return false;
  }
  return true;
}

/**
 * @brief Prepare the exact final path for one direct artifact.
 * @details Coordinates prepare artifact path with fixed application workspaces
 * and propagates validation, storage, or network failure to the selected
 * command runner.
 * @param[in] out_dir Requested output directory.
 * @param[in] leaf Validated artifact leaf name.
 * @param[out] final_path Complete canonical publication path.
 * @return Whether the output directory and final path are safe.
 * @retval true @p final_path is ready for atomic staging.
 * @retval false Creation, canonicalization, or joining failed.
 * @pre All pointer arguments are non-NULL and @p final_path has `PATH_MAX`
 * bytes.
 * @pre @p leaf is the bounded final URL segment.
 * @post Success confines @p final_path beneath a canonical directory.
 * @post Failure emits the same stage-specific diagnostic as direct mode.
 * @note Not safe for concurrent replacement of @p out_dir.
 * @since 0.1.0
 */
RA8_INTERNAL static bool
internal_prepare_artifact_path(const char* out_dir, const char* leaf, char* final_path)
{
  if (priv_mdl_app_storage_ensure_directory(&priv_mdl_app_context()->storage, out_dir) !=
      k_ra8_ok) {
    internal_direct_latch(internal_direct_text3(priv_mdl_app_context()->diagnostic,
                                                "media_dl: cannot create output directory '",
                                                out_dir,
                                                "'\n"));
    return false;
  }
  if (!mdl_path_join(out_dir, leaf, final_path, (size_t)k_fw_fs_path_cap)) {
    internal_direct_latch(internal_direct_text3(priv_mdl_app_context()->diagnostic,
                                                "media_dl: unsafe or unresolved output directory '",
                                                out_dir,
                                                "'\n"));
    return false;
  }
  return true;
}

/**
 * @brief Finalize or abort one direct artifact transfer.
 * @param[in,out] state Active artifact sink state.
 * @param[in] error Transfer or publication result.
 * @param[in] status HTTP status associated with @p error.
 * @return Whether the artifact was published.
 * @retval true Publication completed successfully.
 * @retval false The transfer failed and any transaction was aborted.
 * @pre @p state is non-NULL and initialized for the attempted transfer.
 * @post Failure leaves no active transaction.
 * @post A structural rejection retains its established diagnostic wording.
 * @note Not thread-safe because diagnostics use the shared application context.
 * @since 0.1.0

 * @details Uses caller-owned application and transaction state.
 *          Preserves the first network, storage, export, or presentation failure.
 * @pre Every required pointer is non-null and remains valid for the call.
 */
RA8_INTERNAL static bool
internal_finish_artifact_fetch(mdl_direct_artifact_sink_t* state, ra8_err_t error, long status)
{
  if (error == k_ra8_ok) {
    return true;
  }
  if (state->output.writer.transaction.active) {
    const ra8_err_t aborted = priv_mdl_export_output_abort(&state->output);
    if (aborted != k_ra8_ok) {
      error = aborted;
    }
  }
  if (error == k_ra8_err_validation_failed) {
    internal_direct_latch(priv_mdl_stream_text(k_ra8_ok,
                                               priv_mdl_app_context()->diagnostic,
                                               "media_dl: downloaded artifact failed structural "
                                               "validation\n"));
    return false;
  }
  char reason[k_mdl_reason_max];
  priv_mdl_fetch_reason(error, status, reason, sizeof(reason));
  internal_direct_latch(internal_direct_text3(priv_mdl_app_context()->diagnostic,
                                              "media_dl: artifact fetch failed -- ",
                                              reason,
                                              "\n"));
  return false;
}

/**
 * @brief Fetch one policy-approved artifact into an atomic holding path.
 * @details Performs fetch artifact under the injected network, governor, and
 * storage contracts; dependency failures are propagated before incomplete bytes
 * are published.
 * @param[in] url Absolute artifact URL.
 * @param[in] final_path Complete final publication path.
 * @param[in] format Exact structural verifier selection.
 * @param[in] timeout Per-request timeout in milliseconds.
 * @param[in] opts Validated network and identity policy.
 * @param[out] got Number of downloaded bytes on success.
 * @return Whether a complete staged artifact is ready for validation.
 * @retval true Download completed and network resources were released.
 * @retval false Policy, staging, initialization, or transfer failed.
 * @pre All pointer arguments are non-NULL and output capacities are `PATH_MAX`.
 * @pre @p final_path remains confined beneath a canonical output directory.
 * @post Network resources are destroyed on every initialized path.
 * @post Failure removes any holding file created for this attempt.
 * @note Not thread-safe because it replaces the shared network session.
 * @since 0.1.0
 */
RA8_INTERNAL static bool internal_fetch_artifact(const char*           url,
                                                 const char*           final_path,
                                                 ra8_mdl_format_t      format,
                                                 uint32_t              timeout,
                                                 const mdl_run_opts_t* opts,
                                                 size_t*               got)
{
  mdl_net_iface_t net = {};
  if (mdl_net_provider_open(opts->net, &opts->policy, &net) != k_ra8_ok) {
    internal_direct_latch(priv_mdl_stream_text(k_ra8_ok,
                                               priv_mdl_app_context()->diagnostic,
                                               "media_dl: network init failed\n"));
    return false;
  }
  char ua[k_mdl_ua_max];
  if (priv_mdl_app_start_session(&net, opts, nullptr, ua, sizeof(ua)) != k_ra8_ok) {
    mdl_net_destroy(&net);
    return false;
  }
  if (!mdl_session_url_allowed(&priv_mdl_app_context()->session, url, nullptr)) {
    mdl_net_destroy(&net);
    return false;
  }
  const mdl_net_req_t        req   = {.user_agent = priv_mdl_app_context()->session.user_agent,
                                      .referer    = nullptr,
                                      .timeout_ms = timeout};
  mdl_net_resp_t             resp  = {};
  mdl_direct_artifact_sink_t state = {.storage     = &priv_mdl_app_context()->storage,
                                      .destination = final_path,
                                      .format      = format};
  mdl_net_body_sink_t        sink  = {.reset = internal_direct_artifact_reset,
                                      .write = internal_direct_artifact_write,
                                      .ctx   = &state};
  ra8_err_t                  rc    = mdl_net_get_body(&net, url, &req, &sink, got, &resp);
  if (rc == k_ra8_ok) {
    bool published = false;
    rc =
      priv_mdl_export_output_commit(&state.output, &priv_mdl_app_context()->export_ws, &published);
    if ((rc == k_ra8_ok) && !published) {
      rc = k_ra8_err_invalid_state;
    }
  }
  mdl_net_destroy(&net);
  return internal_finish_artifact_fetch(&state, rc, resp.status);
}

int mdl_app_run_artifact(const char*           url,
                         const char*           out_dir,
                         uint32_t              timeout,
                         const mdl_run_opts_t* opts)
{
  char leaf[k_leaf_name_bytes];
  mdl_urlname_last_segment(url, leaf, sizeof(leaf));
  ra8_mdl_format_t format = k_ra8_mdl_format_invalid;
  if ((mdl_format_from_path(leaf, &format) != k_ra8_ok) || !mdl_format_is_verifiable(format)) {
    internal_direct_latch(internal_direct_text3(priv_mdl_app_context()->diagnostic,
                                                "media_dl: direct artifact '",
                                                leaf,
                                                "' has no supported structural validator "
                                                "(cbz|cbt|cbt.gz|epub|jof|rabook)\n"));
    return 1;
  }
  char final_path[k_fw_fs_path_cap];
  if (!internal_prepare_artifact_path(out_dir, leaf, final_path)) {
    return 1;
  }
  size_t got = 0U;
  if (!internal_fetch_artifact(url, final_path, format, timeout, opts, &got)) {
    return 1;
  }
  mdl_verify_report_t report    = {};
  const ra8_err_t     verify_rc = mdl_verify_file(&priv_mdl_app_context()->storage,
                                                  format,
                                                  final_path,
                                                  &priv_mdl_app_context()->export_ws,
                                                  &report);
  if (verify_rc != k_ra8_ok) {
    ra8_err_t output_error = internal_direct_text3(priv_mdl_app_context()->output,
                                                   "downloaded and verified ",
                                                   final_path,
                                                   " (");
    output_error           = priv_mdl_stream_u64(output_error, priv_mdl_app_context()->output, got);
    output_error = priv_mdl_stream_text(output_error, priv_mdl_app_context()->output, " bytes)\n");
    return (output_error == k_ra8_ok) ? 0 : 1;
  }
  ra8_err_t output_error = internal_direct_text3(priv_mdl_app_context()->output,
                                                 "downloaded and verified ",
                                                 final_path,
                                                 " (");
  output_error           = priv_mdl_stream_u64(output_error, priv_mdl_app_context()->output, got);
  output_error = priv_mdl_stream_text(output_error, priv_mdl_app_context()->output, " bytes, ");
  output_error =
    priv_mdl_stream_u64(output_error, priv_mdl_app_context()->output, report.page_count);
  output_error = priv_mdl_stream_text(output_error, priv_mdl_app_context()->output, " page(s))\n");
  return (output_error == k_ra8_ok) ? 0 : 1;
}

/**
 * @brief Fetch one page and populate the shared extracted-image list.
 * @param[in] url Policy-approved absolute page URL.
 * @param[in] attr Image attribute selector.
 * @param[in] timeout Per-request timeout in milliseconds.
 * @return Whether at least one supported image URL was extracted.
 * @retval true The shared image list contains one or more URLs.
 * @retval false Fetching or extraction failed and was diagnosed.
 * @pre The shared session is initialized and @p url was policy-approved.
 * @post Success replaces the shared image list with this page's matches.
 * @post Failure never reports a usable empty list.
 * @note Not thread-safe because it uses shared page and image buffers.
 * @since 0.1.0

 * @details Uses caller-owned application and transaction state.
 *          Preserves the first network, storage, export, or presentation failure.
 * @pre Every required pointer is non-null and remains valid for the call.
 */
RA8_INTERNAL static bool
internal_extract_page_images(const char* url, const char* attr, uint32_t timeout)
{
  const mdl_net_req_t req   = {.user_agent = priv_mdl_app_context()->session.user_agent,
                               .referer    = nullptr,
                               .timeout_ms = timeout};
  size_t              len   = 0U;
  mdl_net_resp_t      resp  = {};
  ra8_err_t           error = mdl_net_get_buf(priv_mdl_app_context()->session.net,
                                              url,
                                              &req,
                                              priv_mdl_app_context()->page,
                                              sizeof(priv_mdl_app_context()->page),
                                              &len,
                                              &resp);
  if (error != k_ra8_ok) {
    char reason[k_mdl_reason_max];
    priv_mdl_fetch_reason(error, resp.status, reason, sizeof(reason));
    internal_direct_latch(internal_direct_text3(priv_mdl_app_context()->diagnostic,
                                                "media_dl: fetch failed -- ",
                                                reason,
                                                "\n"));
    return false;
  }
  error = mdl_extract_images(priv_mdl_app_context()->page,
                             len,
                             url,
                             attr,
                             nullptr,
                             &priv_mdl_app_context()->images);
  if ((error == k_ra8_ok) && (priv_mdl_app_context()->images.count > 0U)) {
    return true;
  }
  internal_direct_latch(priv_mdl_stream_text(k_ra8_ok,
                                             priv_mdl_app_context()->diagnostic,
                                             "media_dl: page contains no supported image URLs\n"));
  return false;
}

int mdl_app_run_page(const char*           url,
                     const char*           out_dir,
                     const char*           attr,
                     uint32_t              max_imgs,
                     uint64_t              seed,
                     uint32_t              timeout,
                     const mdl_run_opts_t* opts)
{
  char out_abs[k_fw_fs_path_cap];
  if (!internal_prepare_output_dir(out_dir, out_abs)) {
    return 1;
  }
  mdl_net_iface_t net = {};
  if (mdl_net_provider_open(opts->net, &opts->policy, &net) != k_ra8_ok) {
    internal_direct_latch(priv_mdl_stream_text(k_ra8_ok,
                                               priv_mdl_app_context()->diagnostic,
                                               "media_dl: network init failed\n"));
    return 1;
  }
  char ua[k_mdl_ua_max];
  if (priv_mdl_app_start_session(&net, opts, nullptr, ua, sizeof(ua)) != k_ra8_ok) {
    mdl_net_destroy(&net);
    return 1;
  }

  if (!mdl_session_url_allowed(&priv_mdl_app_context()->session, url, nullptr)) {
    mdl_net_destroy(&net);
    return 1;
  }
  if (!internal_extract_page_images(url, attr, timeout)) {
    mdl_net_destroy(&net);
    return 1;
  }
  ra8_err_t output_error = priv_mdl_stream_text(k_ra8_ok, priv_mdl_app_context()->output, "found ");
  output_error           = priv_mdl_stream_u64(output_error,
                                               priv_mdl_app_context()->output,
                                               priv_mdl_app_context()->images.count);
  output_error = priv_mdl_stream_text(output_error, priv_mdl_app_context()->output, " image(s)\n");
  if (output_error != k_ra8_ok) {
    mdl_net_destroy(&net);
    return 1;
  }

  const size_t fail =
    internal_download_page_images(url, out_abs, max_imgs, seed, timeout, opts->polite);
  mdl_net_destroy(&net);
  return ((fail == 0U) && (priv_mdl_app_context()->io_error == k_ra8_ok)) ? 0 : 1;
}

/** @brief Package a directory-output format and report its wildcard destination.
 * @details Uses caller-owned application and transaction state.
 *          Preserves the first network, storage, export, or presentation failure.
 * @param[in] dir Directory path or handle for the operation.
 * @param[in] ext Filename extension without implicit allocation.
 * @param[in] format Requested output format.
 * @return Process-style packaging and presentation status.
 * @retval 0 Packaging and result presentation completed.
 * @retval 1 Export, path construction, or stream output failed.
 * @pre Every required pointer is non-null and remains valid for the call.
 * @pre Lengths and capacities describe complete referenced objects without overflow.
 * @post Documented outputs and the return value describe the same outcome.
 * @post A rejected or failed operation is never reported as successful.
 * @note Thread safety follows ownership of the supplied context; no synchronization is added.
 * @since Version 0.1.0
 */
RA8_INTERNAL static int
internal_pack_directory_output(const char* dir, const char* ext, ra8_mdl_format_t format)
{
  const ra8_err_t error = mdl_export_chapter_ws(&priv_mdl_app_context()->storage,
                                                format,
                                                dir,
                                                dir,
                                                &priv_mdl_app_context()->export_ws);
  if (error != k_ra8_ok) {
    (void)internal_direct_pack_failure(dir, ext, error);
    return 1;
  }
  ra8_err_t output_error =
    internal_direct_text3(priv_mdl_app_context()->output, "packed ", dir, " -> ");
  output_error = priv_mdl_stream_text(output_error, priv_mdl_app_context()->output, dir);
  output_error = priv_mdl_stream_text(output_error, priv_mdl_app_context()->output, "/*.");
  output_error = priv_mdl_stream_text(output_error, priv_mdl_app_context()->output, ext);
  output_error = priv_mdl_stream_text(output_error, priv_mdl_app_context()->output, "\n");
  return (output_error == k_ra8_ok) ? 0 : 1;
}

/** @brief Package a file-output format beside its source directory.
 * @details Uses caller-owned application and transaction state.
 *          Preserves the first network, storage, export, or presentation failure.
 * @param[in] dir Directory path or handle for the operation.
 * @param[in] ext Filename extension without implicit allocation.
 * @param[in] format Requested output format.
 * @return Process-style packaging and presentation status.
 * @retval 0 Packaging and result presentation completed.
 * @retval 1 Export, path construction, or stream output failed.
 * @pre Every required pointer is non-null and remains valid for the call.
 * @pre Lengths and capacities describe complete referenced objects without overflow.
 * @post Documented outputs and the return value describe the same outcome.
 * @post A rejected or failed operation is never reported as successful.
 * @note Thread safety follows ownership of the supplied context; no synchronization is added.
 * @since Version 0.1.0
 */
RA8_INTERNAL static int
internal_pack_file_output(const char* dir, const char* ext, ra8_mdl_format_t format)
{
  char      out[k_fw_fs_path_cap];
  const int length = snprintf(out, sizeof(out), "%s.%s", dir, ext);
  if ((length < 0) || ((size_t)length >= sizeof(out))) {
    (void)internal_direct_text3(priv_mdl_app_context()->diagnostic,
                                "media_dl: output path for '",
                                dir,
                                "' is too long\n");
    return 1;
  }
  const ra8_err_t error = mdl_export_chapter_ws(&priv_mdl_app_context()->storage,
                                                format,
                                                dir,
                                                out,
                                                &priv_mdl_app_context()->export_ws);
  if (error != k_ra8_ok) {
    (void)internal_direct_pack_failure(dir, ext, error);
    return 1;
  }
  ra8_err_t output_error =
    internal_direct_text3(priv_mdl_app_context()->output, "packed ", dir, " -> ");
  output_error = priv_mdl_stream_text(output_error, priv_mdl_app_context()->output, out);
  output_error = priv_mdl_stream_text(output_error, priv_mdl_app_context()->output, "\n");
  return (output_error == k_ra8_ok) ? 0 : 1;
}

int mdl_app_run_pack(const char* dir, ra8_mdl_format_t format)
{
  if ((format == k_ra8_mdl_format_loose) || (format == k_ra8_mdl_format_invalid)) {
    const ra8_err_t output_error = priv_mdl_stream_text(k_ra8_ok,
                                                        priv_mdl_app_context()->diagnostic,
                                                        "media_dl: --pack needs a --format "
                                                        "(cbz|cbt|cbt.gz|epub|jof|rabook)\n");
    return (output_error == k_ra8_ok) ? 2 : 1;
  }
  fw_fs_stat_t source = {};
  if ((dir == nullptr) || (dir[0] != '/') ||
      (fw_fs_stat(&priv_mdl_app_context()->storage.fs->names, dir, &source) != k_ra8_ok) ||
      !source.exists || (source.type != k_fw_fs_node_directory)) {
    (void)internal_direct_text3(priv_mdl_app_context()->diagnostic,
                                "media_dl: cannot resolve '",
                                dir,
                                "'\n");
    return 1;
  }
  const char* ext = mdl_format_ext(format);
  if (mdl_format_is_dir_output(format)) {
    /* JOF writes per-page `.jof` siblings into the packed directory itself;
     * report that directory rather than a container file it never creates. */
    return internal_pack_directory_output(dir, ext, format);
  }
  return internal_pack_file_output(dir, ext, format);
}
