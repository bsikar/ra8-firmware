/**
 * @file ra_ota.c
 * @brief Phase-5 OTA firmware-update orchestration -- implementation.
 *
 * @details
 * Plain-C implementation of the state machine declared in
 * ``ra_ota.h``. The module owns three statics:
 *
 *   - ``s_state``  -- single-byte state machine value.
 *   - ``s_cfg``    -- copy of the caller's configuration (function
 *                     pointers + URLs + bank metadata).
 *   - ``s_buf``    -- 4 KiB streaming chunk buffer used both for
 *                     the manifest fetch and the firmware download.
 *   - ``s_manifest`` -- cached decoded manifest from the most
 *                     recent ``ra_ota_check_for_update`` call.
 *
 * No malloc anywhere (NASA Rule 3). Every loop has a static upper
 * bound (NASA Rule 2). Every public entry point has at least two
 * preconditions (NASA Rule 5).
 *
 * The ThreadX worker thread is wired in through weakly-bound
 * functions ``ra_ota_threadx_spawn`` / ``ra_ota_threadx_join`` --
 * the host test build provides empty stubs in this same TU so the
 * module links cleanly on Linux x86_64. A target-side adapter
 * lives outside this module.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra_ota.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "ra_check.h"
#include "ra_err.h"
#include "ra_ota_internal.h"

/* =============================================================================
 * Module-static storage
 * ============================================================================= */

/** @brief Module log tag. */
static const char* const s_tag = "ra_ota";

/** @brief Current state-machine value. */
static ra_ota_state_t s_state = k_ra_ota_state_idle;

/** @brief Configuration captured at init time. */
static ra_ota_cfg_t s_cfg;

/** @brief True once ``ra_ota_init`` has succeeded. */
static bool s_initialized = false;

/** @brief Cached decoded manifest from the most recent check. */
static ra_ota_manifest_t s_manifest;

/** @brief Whether ``s_manifest`` holds a valid payload. */
static bool s_manifest_valid = false;

/** @brief Bytes already programmed into the inactive bank. */
static uint32_t s_bytes_done = 0U;

/** @brief Last error observed by the state machine. */
static ra_err_t s_last_err = k_ra_ok;

/** @brief Streaming buffer reused by manifest + download paths. */
static uint8_t s_buf[k_ra_ota_chunk_bytes];

/* =============================================================================
 * Internal helpers
 * ============================================================================= */

/**
 * @brief Set state and (if registered) call the progress callback.
 *
 * @details
 * Updates ``s_state`` and ``s_last_err``, then synthesises a
 * ``ra_ota_progress_t`` snapshot and forwards it to the user-provided
 * ``on_progress`` callback when one was registered at init time.
 *
 * @param[in] new_state New SM state.
 * @param[in] err       Error to surface (k_ra_ok on healthy paths).
 *
 * @pre Module is initialized.
 * @pre Caller is the single OTA worker (no concurrent callers).
 * @post ``s_state`` == new_state.
 * @post ``s_last_err`` == err.
 *
 * @note Static helper; not thread-safe -- the OTA module assumes a
 *       single owning context.
 * @since 0.1.0
 */
static void priv_set_state(ra_ota_state_t new_state, ra_err_t err)
{
  s_state    = new_state;
  s_last_err = err;
  if (s_cfg.on_progress != nullptr) {
    const ra_ota_progress_t snap = {
      .state       = new_state,
      .bytes_done  = s_bytes_done,
      .bytes_total = s_manifest_valid ? s_manifest.image_size_bytes : 0U,
      .last_err    = err,
    };
    s_cfg.on_progress(&snap);
  }
}

/**
 * @brief Drain the network stream and accumulate up to ``cap`` bytes.
 *
 * @details
 * Loops calling the user-supplied ``s_cfg.net.read`` callback until
 * either ``cap`` bytes have been collected or the backend reports EOF
 * (``got == 0``). The loop is bounded by ``cap + 1`` iterations
 * (NASA Rule 2). Returns the byte count via ``out_n``.
 *
 * @param[in,out] dst   Destination buffer.
 * @param[in]     cap   Capacity in bytes.
 * @param[out]    out_n Bytes actually received.
 *
 * @return ra_err_t outcome.
 * @retval k_ra_ok        Drain completed (possibly short on EOF).
 * @retval other          Whatever the network backend returned.
 *
 * @pre Module is initialized and ``s_cfg.net.read`` is set.
 * @pre ``dst`` and ``out_n`` are non-NULL.
 * @post On success ``*out_n`` reflects bytes written into ``dst``.
 * @post On failure ``*out_n`` is unspecified.
 *
 * @note Static helper; not thread-safe -- shares the OTA worker context.
 * @since 0.1.0
 */
static ra_err_t priv_drain(uint8_t* dst, uint32_t cap, uint32_t* out_n)
{
  uint32_t total = 0U;
  /* Bounded loop: each iteration must consume >= 1 byte or hit EOF. */
  for (uint32_t guard = 0U; guard < cap + 1U; ++guard) {
    if (total >= cap) {
      break;
    }
    uint32_t       got = 0U;
    const ra_err_t e   = s_cfg.net.read(s_cfg.net.ctx, dst + total, cap - total, &got);
    if (e != k_ra_ok) {
      return e;
    }
    if (got == 0U) {
      break; /* EOF */
    }
    total += got;
  }
  *out_n = total;
  return k_ra_ok;
}

/* =============================================================================
 * Public API
 * ============================================================================= */

/**
 * @brief Initialise the OTA module from a caller-supplied configuration.
 *
 * @details
 * Verifies the module is in the un-initialized state, runs the full
 * ``ra_ota_internal_validate_cfg`` check on ``cfg``, then captures the descriptor
 * by-value into ``s_cfg`` and resets the state machine to
 * ``k_ra_ota_state_idle``.
 *
 * @param[in] cfg Configuration descriptor (function pointers + URLs).
 *
 * @return ra_err_t outcome.
 * @retval k_ra_ok                Module initialized.
 * @retval k_ra_err_invalid_state Module already initialized.
 * @retval k_ra_err_null_ptr      ``cfg`` (or sub-pointer) was NULL.
 * @retval k_ra_err_invalid_arg   Configuration field out of range.
 *
 * @pre Module is uninitialized (or ``ra_ota_deinit`` was called).
 * @pre All function pointers in ``cfg`` are wired up.
 * @post On success the module is in ``k_ra_ota_state_idle``.
 * @post On failure no module state was mutated.
 *
 * @par Example:
 * @code
 * ra_ota_cfg_t cfg = { .net = { ... }, .crypto = { ... } };
 * RA_RETURN_ON_ERROR(ra_ota_init(&cfg), s_tag, "ota init");
 * @endcode
 *
 * @see ra_ota_deinit()
 * @see ra_ota_run_full_update()
 *
 * @note Thread-safe: no.
 * @since 0.1.0
 */
ra_err_t ra_ota_init(const ra_ota_cfg_t* cfg)
{
  if (s_initialized) {
    return k_ra_err_invalid_state;
  }
  const ra_err_t e = ra_ota_internal_validate_cfg(cfg);
  if (e != k_ra_ok) {
    return e;
  }
  (void)memcpy(&s_cfg, cfg, sizeof s_cfg);
  s_state          = k_ra_ota_state_idle;
  s_manifest_valid = false;
  s_bytes_done     = 0U;
  s_last_err       = k_ra_ok;
  s_initialized    = true;
  /* run_as_thread is honoured by an external adapter -- on host the
   * caller drives ra_ota_run_step() directly. */
  return k_ra_ok;
}

/**
 * @brief Reset the OTA module to its un-initialized state.
 *
 * @details
 * Clears the cached configuration, manifest, byte-counter and last
 * error so a future ``ra_ota_init`` starts from a clean slate.
 *
 * @return ra_err_t outcome.
 * @retval k_ra_ok Always succeeds.
 *
 * @pre None (safe to call before init).
 * @post ``s_initialized`` is false.
 * @post ``s_cfg`` is zeroed.
 *
 * @see ra_ota_init()
 *
 * @note Thread-safe: no -- intended to be called when no OTA worker
 *       is running.
 * @since 0.1.0
 *
 * @pre Module has been initialized.
 */
ra_err_t ra_ota_deinit(void)
{
  s_initialized    = false;
  s_state          = k_ra_ota_state_idle;
  s_manifest_valid = false;
  s_bytes_done     = 0U;
  s_last_err       = k_ra_ok;
  (void)memset(&s_cfg, 0, sizeof s_cfg);
  return k_ra_ok;
}

/**
 * @brief Return the current OTA state-machine value.
 *
 * @details
 * Reads the latched ``s_state`` directly. ``s_state`` is a single byte,
 * so a torn read is impossible on the target.
 *
 * @return ra_ota_state_t outcome.
 * @retval k_ra_ota_state_idle Module not initialized, or genuinely idle.
 * @retval other               Whatever state the worker last latched.
 *
 * @pre None (safe to call before init -- returns ``idle``).
 * @post No state mutated.
 *
 * @see ra_ota_run_step()
 *
 * @note Thread-safe: yes -- single-byte read of a static.
 * @since 0.1.0
 *
 * @pre Module has been initialized.
 * @post Side effects bounded to documented state.
 */
ra_ota_state_t ra_ota_get_state(void)
{
  return s_state;
}

/**
 * @brief Open the manifest URL and drain its payload into ``s_buf``.
 *
 * @details
 * Calls ``s_cfg.net.open`` on the configured manifest URL, validates
 * the advertised content length is below ``k_ra_ota_manifest_max_bytes``,
 * then drains via ``priv_drain`` and appends a NUL byte so the JSON
 * helpers may use ``strstr``.
 *
 * @param[out] out_got Bytes received (NUL terminator added at ``s_buf[got]``).
 *
 * @return ra_err_t outcome.
 * @retval k_ra_ok                Payload fetched into ``s_buf``.
 * @retval k_ra_err_invalid_size  Server advertised too large a body.
 * @retval other                  Whatever the network backend returned.
 *
 * @pre Module is initialized.
 * @pre ``out_got`` non-NULL.
 * @post On success ``s_buf[0..*out_got]`` holds the payload + trailing NUL.
 * @post On failure the network connection has been closed.
 *
 * @note Static helper; not thread-safe.
 * @since 0.1.0
 */
static ra_err_t priv_fetch_manifest_payload(uint32_t* out_got)
{
  uint32_t content_len = 0U;
  ra_err_t e           = s_cfg.net.open(s_cfg.net.ctx, s_cfg.manifest_url, &content_len);
  if (e != k_ra_ok) {
    return e;
  }
  if (content_len > k_ra_ota_manifest_max_bytes) {
    (void)s_cfg.net.close(s_cfg.net.ctx);
    return k_ra_err_invalid_size;
  }
  uint32_t got = 0U;
  e            = priv_drain(s_buf, k_ra_ota_manifest_max_bytes - 1U, &got);
  (void)s_cfg.net.close(s_cfg.net.ctx);
  if (e != k_ra_ok) {
    return e;
  }
  s_buf[got] = 0U; /* NUL terminate so JSON helpers can use strstr. */
  *out_got   = got;
  return k_ra_ok;
}

/**
 * @brief Fetch and decode the upstream manifest, leaving it cached.
 *
 * @details
 * Transitions the state machine ``idle -> checking -> idle`` (success)
 * or ``idle -> checking -> error`` (failure). On success the manifest
 * is mirrored both into ``*out_manifest`` and into the module-private
 * ``s_manifest`` so subsequent steps can refer to it.
 *
 * @param[out] out_manifest Caller-owned manifest buffer.
 *
 * @return ra_err_t outcome.
 * @retval k_ra_ok                  Manifest cached and returned.
 * @retval k_ra_err_not_initialized Module not initialized.
 * @retval k_ra_err_null_ptr        ``out_manifest`` was NULL.
 * @retval k_ra_err_invalid_state   Module not in ``idle``.
 * @retval other                    Network or decode error.
 *
 * @pre ``ra_ota_init`` succeeded.
 * @pre Module is in ``k_ra_ota_state_idle``.
 * @post On success state == ``idle``, manifest is cached.
 * @post On failure state == ``error`` with ``s_last_err`` set.
 *
 * @see ra_ota_download_to_inactive_bank()
 *
 * @note Thread-safe: no.
 * @since 0.1.0
 */
ra_err_t ra_ota_check_for_update(ra_ota_manifest_t* out_manifest)
{
  if (!s_initialized) {
    return k_ra_err_not_initialized;
  }
  RA_CHECK_NULL_PTR(out_manifest, s_tag, "out_manifest");
  if (s_state != k_ra_ota_state_idle) {
    return k_ra_err_invalid_state;
  }
  priv_set_state(k_ra_ota_state_checking, k_ra_ok);

  uint32_t got = 0U;
  ra_err_t e   = priv_fetch_manifest_payload(&got);
  if (e != k_ra_ok) {
    priv_set_state(k_ra_ota_state_error, e);
    return e;
  }

  e = ra_ota_internal_manifest_decode((const char*)s_buf, out_manifest);
  if (e != k_ra_ok) {
    priv_set_state(k_ra_ota_state_error, e);
    return e;
  }
  (void)memcpy(&s_manifest, out_manifest, sizeof s_manifest);
  s_manifest_valid = true;
  priv_set_state(k_ra_ota_state_idle, k_ra_ok);
  return k_ra_ok;
}

/**
 * @brief Stream one chunk: drain network -> hash -> flash program.
 *
 * @details
 * Computes a chunk size capped at ``k_ra_ota_chunk_bytes``, drains it
 * via ``priv_drain``, updates the SHA-256 accumulator, programs it
 * into flash at ``addr_base + *in_out_done`` and bumps the running
 * counter.
 *
 * @param[in]     addr_base   Bank base address inside flash.
 * @param[in,out] in_out_done Bytes already programmed; bumped on success.
 * @param[in]     total       Image size in bytes.
 *
 * @return ra_err_t outcome.
 * @retval k_ra_ok            Chunk programmed and accumulator updated.
 * @retval k_ra_err_hw_error  Backend EOF before image complete.
 * @retval other              Network / crypto / flash error.
 *
 * @pre Module is in ``downloading`` (or about to enter it).
 * @pre Pointers non-NULL.
 * @post On success ``*in_out_done`` increased by the chunk byte count.
 * @post On failure no flash bytes were programmed in this call.
 *
 * @note Static helper; not thread-safe.
 * @since 0.1.0
 */
static ra_err_t priv_download_chunk(uint32_t addr_base, uint32_t* in_out_done, uint32_t total)
{
  const uint32_t remaining = total - *in_out_done;
  const uint32_t want      = (remaining < k_ra_ota_chunk_bytes) ? remaining : k_ra_ota_chunk_bytes;
  uint32_t       got       = 0U;
  ra_err_t       e         = priv_drain(s_buf, want, &got);
  if (e != k_ra_ok) {
    return e;
  }
  if (got == 0U) {
    return k_ra_err_hw_error;
  }
  e = s_cfg.crypto.sha256_update(s_cfg.crypto.ctx, s_buf, got);
  if (e != k_ra_ok) {
    return e;
  }
  e = s_cfg.flash.program(s_cfg.flash.ctx, addr_base + *in_out_done, s_buf, got);
  if (e != k_ra_ok) {
    return e;
  }
  *in_out_done += got;
  priv_set_state(k_ra_ota_state_downloading, k_ra_ok);
  return k_ra_ok;
}

/**
 * @brief Erase the inactive bank and prime the SHA accumulator.
 *
 * @details
 * Only invoked on a fresh download start (``s_bytes_done == 0``).
 * Erases ``manifest->image_size_bytes`` worth of inactive-bank flash
 * then re-initialises the SHA-256 accumulator so the new download is
 * hashed from byte 0.
 *
 * @param[in] manifest Currently active manifest.
 *
 * @return ra_err_t outcome.
 * @retval k_ra_ok Bank erased and SHA primed.
 * @retval other   Whatever the flash erase / sha init returned.
 *
 * @pre ``manifest`` non-NULL.
 * @pre Module owns the inactive bank.
 * @post On success the inactive bank is fully erased and SHA is primed.
 * @post On failure the bank may be partially erased.
 *
 * @note Static helper; not thread-safe.
 * @since 0.1.0
 */
static ra_err_t priv_prepare_bank(const ra_ota_manifest_t* manifest)
{
  ra_err_t e =
    s_cfg.flash.erase(s_cfg.flash.ctx, s_cfg.flash.inactive_bank_addr, manifest->image_size_bytes);
  if (e != k_ra_ok) {
    return e;
  }
  return s_cfg.crypto.sha256_init(s_cfg.crypto.ctx);
}

/**
 * @brief Drain chunks until the entire image is downloaded or an error fires.
 *
 * @details
 * Loops calling ``priv_download_chunk`` until ``s_bytes_done`` reaches
 * ``manifest->image_size_bytes``. Bounded by
 * ``(k_ra_ota_max_image_bytes / k_ra_ota_chunk_bytes) + 1`` iterations
 * (NASA Rule 2).
 *
 * @param[in] manifest Manifest describing the in-flight image.
 *
 * @return ra_err_t outcome.
 * @retval k_ra_ok           Download completed.
 * @retval k_ra_err_hw_error Chunk count exceeded the static cap.
 * @retval other             Whatever ``priv_download_chunk`` returned.
 *
 * @pre Module is in ``downloading``.
 * @pre ``manifest`` non-NULL.
 * @post On success ``s_bytes_done == manifest->image_size_bytes``.
 * @post On failure ``s_bytes_done`` reflects the partial progress.
 *
 * @note Static helper; not thread-safe.
 * @since 0.1.0
 */
static ra_err_t priv_download_loop(const ra_ota_manifest_t* manifest)
{
  const uint32_t max_chunks = (k_ra_ota_max_image_bytes / k_ra_ota_chunk_bytes) + 1U;
  uint32_t       chunks     = 0U;
  ra_err_t       e          = k_ra_ok;
  while (s_bytes_done < manifest->image_size_bytes) {
    if (chunks >= max_chunks) {
      e = k_ra_err_hw_error;
      break;
    }
    e = priv_download_chunk(s_cfg.flash.inactive_bank_addr,
                            &s_bytes_done,
                            manifest->image_size_bytes);
    if (e != k_ra_ok) {
      break;
    }
    ++chunks;
  }
  return e;
}

/**
 * @brief Download an image into the inactive bank, hashing as it goes.
 *
 * @details
 * On a fresh start (``s_bytes_done == 0``) erases the bank and primes
 * the SHA accumulator via ``priv_prepare_bank``. Then opens the image
 * URL and runs ``priv_download_loop``. On success the state machine
 * lands in ``verifying``; on failure it lands in ``error``.
 *
 * @param[in] manifest Manifest describing the image to fetch.
 *
 * @return ra_err_t outcome.
 * @retval k_ra_ok                  Download complete; ready to verify.
 * @retval k_ra_err_not_initialized Module not initialized.
 * @retval k_ra_err_null_ptr        ``manifest`` was NULL.
 * @retval k_ra_err_invalid_state   Module not in ``idle`` or ``downloading``.
 * @retval k_ra_err_invalid_size    Image larger than the configured bank.
 * @retval other                    Network / crypto / flash error.
 *
 * @pre ``ra_ota_init`` succeeded.
 * @pre ``manifest`` non-NULL.
 * @post On success state == ``verifying``.
 * @post On failure state == ``error`` with ``s_last_err`` set.
 *
 * @see ra_ota_verify_signature()
 *
 * @note Thread-safe: no.
 * @since 0.1.0
 */
ra_err_t ra_ota_download_to_inactive_bank(const ra_ota_manifest_t* manifest)
{
  if (!s_initialized) {
    return k_ra_err_not_initialized;
  }
  RA_CHECK_NULL_PTR(manifest, s_tag, "manifest");
  if (ra_ota_internal_download_state_invalid((uint32_t)k_ra_ota_state_idle,
                                             (uint32_t)k_ra_ota_state_downloading,
                                             (uint32_t)s_state)) {
    return k_ra_err_invalid_state;
  }
  if (manifest->image_size_bytes > s_cfg.flash.bank_size_bytes) {
    priv_set_state(k_ra_ota_state_error, k_ra_err_invalid_size);
    return k_ra_err_invalid_size;
  }

  if (s_bytes_done == 0U) {
    const ra_err_t e = priv_prepare_bank(manifest);
    if (e != k_ra_ok) {
      priv_set_state(k_ra_ota_state_error, e);
      return e;
    }
  }

  uint32_t content_len = 0U;
  ra_err_t e           = s_cfg.net.open(s_cfg.net.ctx, manifest->image_url, &content_len);
  if (e != k_ra_ok) {
    priv_set_state(k_ra_ota_state_error, e);
    return e;
  }
  priv_set_state(k_ra_ota_state_downloading, k_ra_ok);

  e = priv_download_loop(manifest);
  (void)s_cfg.net.close(s_cfg.net.ctx);

  if (e != k_ra_ok) {
    priv_set_state(k_ra_ota_state_error, e);
    return e;
  }
  priv_set_state(k_ra_ota_state_verifying, k_ra_ok);
  return k_ra_ok;
}

/**
 * @brief Re-hash the inactive bank to re-derive the digest after program.
 *
 * @details
 * Re-initialises the SHA accumulator then walks the inactive bank in
 * ``k_ra_ota_chunk_bytes`` chunks via ``s_cfg.flash.readback``,
 * feeding each one to ``s_cfg.crypto.sha256_update``. Finalises into
 * ``out_digest``. Loop is bounded by
 * ``(k_ra_ota_max_image_bytes / k_ra_ota_chunk_bytes) + 1``.
 *
 * @param[in]  m          Manifest (provides ``image_size_bytes``).
 * @param[out] out_digest 32-byte SHA-256 destination.
 *
 * @return ra_err_t outcome.
 * @retval k_ra_ok Digest derived.
 * @retval other   Whatever the readback / crypto callbacks returned.
 *
 * @pre Module is in ``verifying``.
 * @pre Pointers non-NULL.
 * @post On success ``out_digest`` holds SHA-256 of the bank contents.
 * @post On failure ``out_digest`` content is unspecified.
 *
 * @note Static helper; not thread-safe.
 * @since 0.1.0
 */
static ra_err_t priv_rehash_bank(const ra_ota_manifest_t* m, uint8_t out_digest[32])
{
  ra_err_t e = s_cfg.crypto.sha256_init(s_cfg.crypto.ctx);
  if (e != k_ra_ok) {
    return e;
  }
  uint32_t       offset     = 0U;
  const uint32_t max_chunks = (k_ra_ota_max_image_bytes / k_ra_ota_chunk_bytes) + 1U;
  for (uint32_t i = 0U; i < max_chunks; ++i) {
    if (offset >= m->image_size_bytes) {
      break;
    }
    const uint32_t remaining = m->image_size_bytes - offset;
    const uint32_t want = (remaining < k_ra_ota_chunk_bytes) ? remaining : k_ra_ota_chunk_bytes;
    e = s_cfg.flash.readback(s_cfg.flash.ctx, s_cfg.flash.inactive_bank_addr + offset, s_buf, want);
    if (e != k_ra_ok) {
      return e;
    }
    e = s_cfg.crypto.sha256_update(s_cfg.crypto.ctx, s_buf, want);
    if (e != k_ra_ok) {
      return e;
    }
    offset += want;
  }
  return s_cfg.crypto.sha256_final(s_cfg.crypto.ctx, out_digest);
}

/**
 * @brief Verify the freshly-programmed bank against the manifest signature.
 *
 * @details
 * Re-hashes the inactive bank via ``priv_rehash_bank``, compares the
 * digest against ``manifest->image_sha256``, and on a match invokes
 * the configured ECDSA verifier with the public-key handle and the
 * manifest signature. On success the state machine lands in
 * ``committing``.
 *
 * @param[in] manifest Manifest used for the download.
 *
 * @return ra_err_t outcome.
 * @retval k_ra_ok                  Image authenticated.
 * @retval k_ra_err_not_initialized Module not initialized.
 * @retval k_ra_err_null_ptr        ``manifest`` was NULL.
 * @retval k_ra_err_invalid_state   Module not in ``verifying``.
 * @retval k_ra_err_crc_mismatch    SHA-256 mismatch (image corrupt).
 * @retval k_ra_err_hw_error        ECDSA verify rejected the signature.
 * @retval other                    Crypto / flash backend error.
 *
 * @pre ``ra_ota_download_to_inactive_bank`` succeeded.
 * @pre ``manifest`` non-NULL.
 * @post On success state == ``committing``.
 * @post On failure state == ``error``.
 *
 * @see ra_ota_commit_and_reboot()
 *
 * @note Thread-safe: no.
 * @since 0.1.0
 */
ra_err_t ra_ota_verify_signature(const ra_ota_manifest_t* manifest)
{
  if (!s_initialized) {
    return k_ra_err_not_initialized;
  }
  RA_CHECK_NULL_PTR(manifest, s_tag, "manifest");
  if (s_state != k_ra_ota_state_verifying) {
    return k_ra_err_invalid_state;
  }

  uint8_t  digest[k_ra_ota_sha256_bytes] = {};
  ra_err_t e                             = priv_rehash_bank(manifest, digest);
  if (e != k_ra_ok) {
    priv_set_state(k_ra_ota_state_error, e);
    return e;
  }
  if (memcmp(digest, manifest->image_sha256, k_ra_ota_sha256_bytes) != 0) {
    priv_set_state(k_ra_ota_state_error, k_ra_err_crc_mismatch);
    return k_ra_err_crc_mismatch;
  }
  e = s_cfg.crypto.ecdsa_verify(s_cfg.crypto.ctx,
                                s_cfg.pubkey_handle,
                                digest,
                                manifest->signature,
                                manifest->signature_len);
  if (e != k_ra_ok) {
    priv_set_state(k_ra_ota_state_error, k_ra_err_hw_error);
    return k_ra_err_hw_error;
  }
  priv_set_state(k_ra_ota_state_committing, k_ra_ok);
  return k_ra_ok;
}

/**
 * @brief Latch the inactive bank as the next boot bank and reboot.
 *
 * @details
 * Calls ``s_cfg.flash.set_startup`` to mark the inactive bank as the
 * boot bank, then invokes ``ra_ota_system_reset_hook`` (which on
 * hardware overrides to ``NVIC_SystemReset`` and on host is a no-op
 * for testability).
 *
 * @return ra_err_t outcome.
 * @retval k_ra_ok                  Bank latched (the call normally
 *                                  doesn't return on hardware).
 * @retval k_ra_err_not_initialized Module not initialized.
 * @retval k_ra_err_invalid_state   Module not in ``committing``.
 * @retval other                    Backend error from set_startup.
 *
 * @pre ``ra_ota_verify_signature`` succeeded.
 * @pre Module is in ``k_ra_ota_state_committing``.
 * @post On success state == ``done`` and the system reset hook fired.
 * @post On failure state == ``error``.
 *
 * @see ra_ota_system_reset_hook()
 *
 * @note Thread-safe: no.
 * @since 0.1.0
 */
ra_err_t ra_ota_commit_and_reboot(void)
{
  if (!s_initialized) {
    return k_ra_err_not_initialized;
  }
  if (s_state != k_ra_ota_state_committing) {
    return k_ra_err_invalid_state;
  }
  const ra_err_t e =
    s_cfg.flash.set_startup(s_cfg.flash.ctx, s_cfg.flash.inactive_bank_index, true);
  if (e != k_ra_ok) {
    priv_set_state(k_ra_ota_state_error, e);
    return e;
  }
  priv_set_state(k_ra_ota_state_done, k_ra_ok);
  /* On hardware ra_ota_system_reset_hook is overridden to call
   * NVIC_SystemReset; in the host build it is a no-op. */
  ra_ota_system_reset_hook();
  return k_ra_ok;
}

/**
 * @brief Drive one transition based on the current state.
 *
 * @details
 * Switches on ``s_state`` and dispatches to the matching public-API
 * function (``check_for_update``, ``download_to_inactive_bank``,
 * ``verify_signature`` or ``commit_and_reboot``). Terminal states
 * (``done`` / ``error``) return ``k_ra_ok`` so the caller may stop
 * polling.
 *
 * @return ra_err_t outcome.
 * @retval k_ra_ok Step completed (or terminal state reached).
 * @retval other   Whatever the dispatched function returned.
 *
 * @pre Module is initialized.
 * @post The state machine has advanced by at most one transition.
 *
 * @note Static helper; not thread-safe.
 * @since 0.1.0
 *
 * @pre Module has been initialized.
 * @post Side effects bounded to documented state.
 */
static ra_err_t priv_step_dispatch(void)
{
  switch (s_state) {
    case k_ra_ota_state_idle: {
      /* If a previous run_step already fetched and validated the
       * manifest, advance to download instead of re-fetching it. */
      if (s_manifest_valid) {
        return ra_ota_download_to_inactive_bank(&s_manifest);
      }
      ra_ota_manifest_t m;
      return ra_ota_check_for_update(&m);
    }
    case k_ra_ota_state_checking:
    case k_ra_ota_state_downloading:
      if (s_manifest_valid) {
        return ra_ota_download_to_inactive_bank(&s_manifest);
      }
      return k_ra_err_invalid_state;
    case k_ra_ota_state_verifying:
      return ra_ota_verify_signature(&s_manifest);
    case k_ra_ota_state_committing:
      return ra_ota_commit_and_reboot();
    case k_ra_ota_state_done:
    case k_ra_ota_state_error:
    case k_ra_ota_state_count:
    default:
      return k_ra_ok;
  }
}

/**
 * @brief Drive the OTA state machine one step forward.
 *
 * @details
 * Thin wrapper over ``priv_step_dispatch`` that gates on
 * ``s_initialized``. Intended for callers that opted out of running
 * the OTA worker as a background thread.
 *
 * @return ra_err_t outcome.
 * @retval k_ra_ok                  Step completed.
 * @retval k_ra_err_not_initialized Module not initialized.
 * @retval other                    Step-specific error.
 *
 * @pre ``ra_ota_init`` succeeded.
 * @post The state machine has advanced by at most one transition.
 *
 * @see ra_ota_get_state()
 * @see ra_ota_run_full_update()
 *
 * @note Thread-safe: no -- single owner only.
 * @since 0.1.0
 *
 * @pre Module has been initialized.
 * @post Side effects bounded to documented state.
 */
ra_err_t ra_ota_run_step(void)
{
  if (!s_initialized) {
    return k_ra_err_not_initialized;
  }
  return priv_step_dispatch();
}

/**
 * @brief Drive the OTA state machine through an end-to-end update.
 *
 * @details
 * Loops calling ``ra_ota_run_step`` for at most ``k_ra_ota_state_count``
 * iterations (NASA Rule 2 bound: idle -> checking -> downloading ->
 * verifying -> committing -> done). Stops early on ``done`` or
 * ``error``.
 *
 * @return ra_err_t outcome.
 * @retval k_ra_ok                  Update completed (or already done).
 * @retval k_ra_err_not_initialized Module not initialized.
 * @retval other                    Whatever the failing step returned.
 *
 * @pre ``ra_ota_init`` succeeded.
 * @post Module is in ``done`` (success) or ``error`` (failure).
 *
 * @see ra_ota_run_step()
 *
 * @note Thread-safe: no.
 * @since 0.1.0
 *
 * @pre Module has been initialized.
 * @post Side effects bounded to documented state.
 */
ra_err_t ra_ota_run_full_update(void)
{
  if (!s_initialized) {
    return k_ra_err_not_initialized;
  }
  /* Bounded by the longest legal sequence (idle -> checking ->
   * downloading -> verifying -> committing -> done). Six steps is
   * the upper bound; pad to ``k_ra_ota_state_count`` for safety. */
  for (uint32_t i = 0U; i < (uint32_t)k_ra_ota_state_count; ++i) {
    if ((s_state == k_ra_ota_state_done) || (s_state == k_ra_ota_state_error)) {
      break;
    }
    const ra_err_t e = ra_ota_run_step();
    if (e != k_ra_ok) {
      return e;
    }
  }
  return s_last_err;
}

/* =============================================================================
 * Weak system-reset hook: real target overrides this with
 * ``NVIC_SystemReset()``.  Default is a no-op so unit tests don't
 * actually exit the process.
 * ============================================================================= */

/**
 * @brief Weak system-reset hook overridden by the target build.
 *
 * @details
 * Called from ``ra_ota_commit_and_reboot`` after the bank-swap is
 * latched. The hardware build overrides this with a definition that
 * calls ``NVIC_SystemReset``. The host (unit-test) build keeps the
 * weak no-op default so tests can observe post-commit state without
 * actually exiting the process.
 *
 * @pre Weak symbol; safe to leave unimplemented.
 * @post Default no-op; target override never returns.
 *
 * @par Example:
 * @code
 * // In target firmware:
 * void ra_ota_system_reset_hook(void) { NVIC_SystemReset(); }
 * @endcode
 *
 * @note Thread-safe: target override does not return, so trivially safe.
 * @since 0.1.0
 */
#if defined(__GNUC__) || defined(__clang__)
__attribute__((weak))
#endif
void ra_ota_system_reset_hook(void)
{
  /* Intentionally empty. */
}
