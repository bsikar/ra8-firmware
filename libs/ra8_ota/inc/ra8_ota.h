/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file ra8_ota.h
 * @brief Phase-5 OTA firmware-update orchestration for the RA8D2.
 * @ingroup grp_security
 *
 * @par Tag
 * [Ring 4 / Service] {World: NS}
 *
 * @details
 * The RA8D2 has 1 MiB of code-MRAM split into two equal-size banks
 * (HUM Ch 7.2 "BTFLG Boot-Area Swap" p 282..286 + HUM Ch 59.5
 * "MSUACR / MSUASMON" p 3582..3586). At any moment the part is
 * executing out of one bank ("active"); the other bank ("inactive")
 * is free to be erased and re-programmed. ``ra8_ota`` orchestrates
 * the safe-update flow on top of the existing ``ra8_flash`` driver:
 *
 * @par State machine (single thread, optionally a ThreadX worker):
 * @code
 *   IDLE -> CHECKING -> DOWNLOADING -> VERIFYING -> COMMITTING -> DONE
 *                |           |             |
 *                v           v             v
 *               ERROR <-----'-----<--------'
 * @endcode
 *
 * Each public entry point is one transition arrow. ``ra8_ota_run_step``
 * drives the machine cooperatively from a caller's main loop;
 * ``cfg.run_as_thread = true`` instead spawns a single static ThreadX
 * thread that calls ``ra8_ota_run_step`` until ``DONE`` / ``ERROR``.
 *
 * The HTTPS download and ECDSA verification are exposed through
 * function-pointer interfaces (``ra8_ota_net_iface_t``,
 * ``ra8_ota_crypto_iface_t``) so the module compiles independently of
 * ra8_tls / mbedtls / tf-psa-crypto and can be unit-tested with
 * mocks. This is the Dependency-Inversion deviation called out in
 * CLAUDE.md "NASA Rule 9 -- INTENTIONAL DEVIATION".
 *
 * @par Memory budget:
 * - 1 download chunk buffer of ``k_ra8_ota_chunk_bytes``
 *   (see ``ra8_ota_constants_t``).
 * - 1 manifest scratch buffer of ``k_ra8_ota_manifest_max_bytes``.
 * - 1 SHA-256 streaming context (caller-provided through
 *   ``ra8_ota_crypto_iface_t.sha256_ctx_size`` bytes).
 * - 1 static ThreadX thread (only if ``cfg.run_as_thread``).
 *
 * Every buffer is a file-static in ``ra8_ota.c``. There is zero
 * dynamic allocation anywhere in this module (NASA Rule 3).
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>
#include <stdint.h>

#include "ra8_err.h"

/**
 * @enum ra8_ota_constants_t
 * @brief Compile-time bounds for the OTA module.
 *
 * @details
 * Centralises every numeric limit so callers and unit tests can
 * reference them by name. All values are typed (NASA Rule 8 +
 * CLAUDE.md "C23 typed enums").
 */
typedef enum : uint32_t {
  k_ra8_ota_chunk_bytes         = 4096U,    /**< Download streaming chunk size in bytes. */
  k_ra8_ota_manifest_max_bytes  = 2048U,    /**< Largest accepted manifest payload.      */
  k_ra8_ota_sha256_bytes        = 32U,      /**< SHA-256 digest length.                  */
  k_ra8_ota_signature_max_bytes = 96U,      /**< ECDSA-P256 ASN.1 sig upper bound.       */
  k_ra8_ota_url_max_bytes       = 256U,     /**< NUL-terminated URL upper bound.         */
  k_ra8_ota_version_str_bytes   = 32U,      /**< NUL-terminated version string.          */
  k_ra8_ota_max_image_bytes     = 0x80000U, /**< 512 KiB upper bound per bank.           */
  k_ra8_ota_thread_stack_bytes  = 4096U,    /**< Static ThreadX worker stack.            */
} ra8_ota_constants_t;

/**
 * @enum ra8_ota_state_t
 * @brief Cooperative state-machine states.
 *
 * @details
 * Returned by ``ra8_ota_get_state``. Values are stable across calls so
 * callers may compare with ``==`` or use them as table indices.
 */
typedef enum : uint8_t {
  k_ra8_ota_state_idle        = 0U, /**< No update in progress.               */
  k_ra8_ota_state_checking    = 1U, /**< Manifest fetch in flight.            */
  k_ra8_ota_state_downloading = 2U, /**< Streaming firmware to inactive bank. */
  k_ra8_ota_state_verifying   = 3U, /**< SHA-256 + ECDSA verification.        */
  k_ra8_ota_state_committing  = 4U, /**< About to swap banks + reset.         */
  k_ra8_ota_state_done        = 5U, /**< Update applied (reset is imminent).  */
  k_ra8_ota_state_error       = 6U, /**< Last operation failed; see last err. */
  k_ra8_ota_state_count       = 7U, /**< Sentinel.                            */
} ra8_ota_state_t;

/**
 * @struct ra8_ota_manifest_t
 * @brief Decoded representation of the server manifest.
 *
 * @details
 * The manifest the server returns is JSON; ``ra8_ota`` decodes the
 * relevant fields here. The struct is also used as the input to
 * ``ra8_ota_download_to_inactive_bank`` and
 * ``ra8_ota_verify_signature``.
 *
 * @invariant ``image_size_bytes`` <= ``k_ra8_ota_max_image_bytes``.
 * @invariant ``signature_len`` <= ``k_ra8_ota_signature_max_bytes``.
 */
typedef struct {
  char     version[k_ra8_ota_version_str_bytes];     /**< Firmware version string.     */
  char     image_url[k_ra8_ota_url_max_bytes];       /**< HTTPS URL of the image blob. */
  uint32_t image_size_bytes;                         /**< Image size on the wire.      */
  uint8_t  image_sha256[k_ra8_ota_sha256_bytes];     /**< Expected digest.             */
  uint8_t  signature[k_ra8_ota_signature_max_bytes]; /**< ECDSA signature over digest. */
  uint16_t signature_len;                            /**< Bytes used in ``signature``. */
} ra8_ota_manifest_t;

/**
 * @struct ra8_ota_progress_t
 * @brief Snapshot delivered to the caller's progress callback.
 */
typedef struct {
  ra8_ota_state_t state;       /**< Current state machine value.          */
  uint32_t        bytes_done;  /**< Bytes successfully programmed so far. */
  uint32_t        bytes_total; /**< Manifest-declared size.               */
  ra8_err_t       last_err;    /**< Last error (k_ra8_ok if healthy).     */
} ra8_ota_progress_t;

/**
 * @brief Caller progress callback type.
 *
 * @param[in] p Snapshot of OTA progress (state, bytes, last err).
 *
 * @pre ``p`` non-NULL (guaranteed by the dispatcher).
 * @post Callback returns; the OTA module continues with the next
 *       state-machine step.
 *
 * @note Runs in the OTA worker context (or the caller's thread when
 *       cooperatively driven). Keep it short and non-blocking.
 */
typedef void (*ra8_ota_progress_cb_t)(const ra8_ota_progress_t* p);

/**
 * @struct ra8_ota_net_iface_t
 * @brief Injected HTTPS-download interface (Dependency Inversion).
 *
 * @details
 * The OTA module never links directly against ra8_tls / mbedtls. The
 * caller registers a vtable with ``open``, ``read``, ``close``. The
 * default wiring -- when ra8_tls eventually lands -- will be a thin
 * shim in ra8_tls that adapts ra8_tls handles to this struct.
 *
 * @invariant All three function pointers are non-NULL when this
 *            struct is passed to ``ra8_ota_init``.
 */
typedef struct {
  /**
   * @brief Begin a streaming GET against ``url``.
   * @return ``k_ra8_ok`` if a download session has been opened and
   *         subsequent ``read`` calls are valid.
   */
  ra8_err_t (*open)(void* ctx, const char* url, uint32_t* out_content_len);

  /**
   * @brief Read up to ``cap`` bytes from the open session.
   * @param[out] out_len Bytes actually read (0 == EOF).
   */
  ra8_err_t (*read)(void* ctx, uint8_t* dst, uint32_t cap, uint32_t* out_len);

  /**
   * @brief Tear the streaming GET down.
   */
  ra8_err_t (*close)(void* ctx);

  /** @brief Opaque context passed back to every callback. */
  void* ctx;
} ra8_ota_net_iface_t;

/**
 * @struct ra8_ota_crypto_iface_t
 * @brief Injected hash + ECDSA verification interface.
 *
 * @details
 * Same Dependency-Inversion pattern as ``ra8_ota_net_iface_t``. A
 * concrete implementation lives outside this module (mbedtls or
 * tf-psa-crypto adapter); unit tests pass a mock that scripts
 * happy-path / mismatch behaviour.
 */
typedef struct {
  /** @brief Begin a SHA-256 streaming hash. */
  ra8_err_t (*sha256_init)(void* ctx);
  /** @brief Feed bytes to the running SHA-256 hash. */
  ra8_err_t (*sha256_update)(void* ctx, const uint8_t* data, uint32_t len);
  /** @brief Finalise and write the 32-byte digest to ``out``. */
  ra8_err_t (*sha256_final)(void* ctx, uint8_t out[k_ra8_ota_sha256_bytes]);

  /**
   * @brief Verify ``sig`` is a valid ECDSA signature over ``digest``
   *        using the public key referenced by ``pubkey_handle``.
   */
  ra8_err_t (*ecdsa_verify)(void*          ctx,
                            uint32_t       pubkey_handle,
                            const uint8_t  digest[k_ra8_ota_sha256_bytes],
                            const uint8_t* sig,
                            uint32_t       sig_len);

  /** @brief Opaque context passed back to every callback. */
  void* ctx;
} ra8_ota_crypto_iface_t;

/**
 * @struct ra8_ota_flash_iface_t
 * @brief Injected flash backend (so tests do not need real MRAM).
 *
 * @details
 * The default production wiring is a 4-line shim that forwards to
 * ``ra8_flash_erase`` / ``ra8_flash_write`` /
 * ``ra8_flash_set_startup_area`` and lives outside this module so
 * libs/ra8_ota does not transitively pull in libs/ra8_hal at link
 * time. Unit tests register an in-RAM mock backend.
 */
typedef struct {
  /** @brief Erase ``len`` bytes starting at ``addr`` in the inactive bank. */
  ra8_err_t (*erase)(void* ctx, uint32_t addr, uint32_t len);
  /** @brief Program ``len`` bytes at ``addr`` (must be 32-byte aligned). */
  ra8_err_t (*program)(void* ctx, uint32_t addr, const uint8_t* src, uint32_t len);
  /** @brief Pick the bank to boot from at the next reset. */
  ra8_err_t (*set_startup)(void* ctx, uint8_t which_bank, bool persistent);
  /** @brief Read back ``len`` bytes (used by verification re-hash). */
  ra8_err_t (*readback)(void* ctx, uint32_t addr, uint8_t* dst, uint32_t len);

  /** @brief Base address of the inactive bank in the MRAM window. */
  uint32_t inactive_bank_addr;
  /** @brief Size of one bank in bytes. */
  uint32_t bank_size_bytes;
  /** @brief Bank index ``set_startup`` should select on commit. */
  uint8_t inactive_bank_index;

  /** @brief Opaque context passed back to every callback. */
  void* ctx;
} ra8_ota_flash_iface_t;

/**
 * @struct ra8_ota_cfg_t
 * @brief Initialisation descriptor for ``ra8_ota_init``.
 *
 * @details
 * Aggregates every dependency the OTA module needs at run-time:
 * the manifest URL, the public-key handle, the three injected
 * interfaces, and the optional ThreadX worker selection.
 */
typedef struct {
  /** @brief HTTPS URL of the manifest JSON. NUL-terminated. */
  char manifest_url[k_ra8_ota_url_max_bytes];

  /** @brief Opaque handle to the trusted ECDSA public key. */
  uint32_t pubkey_handle;

  /** @brief Progress callback (NULL == disabled). */
  ra8_ota_progress_cb_t on_progress;

  /** @brief Network HTTPS interface (must be fully populated). */
  ra8_ota_net_iface_t net;

  /** @brief Crypto interface (must be fully populated). */
  ra8_ota_crypto_iface_t crypto;

  /** @brief Flash backend (must be fully populated). */
  ra8_ota_flash_iface_t flash;

  /** @brief true => spawn the static ThreadX worker thread. */
  bool run_as_thread;
} ra8_ota_cfg_t;

/**
 * @brief Initialise the OTA module.
 *
 * @details
 * Validates every field of ``cfg``, copies it into module-static
 * storage, optionally spawns the ThreadX worker, and parks the
 * state machine in ``k_ra8_ota_state_idle``.
 *
 * @param[in] cfg Non-NULL configuration descriptor.
 *
 * @return ``ra8_err_t`` outcome.
 * @retval k_ra8_ok               Module ready.
 * @retval k_ra8_err_null_ptr     ``cfg`` was NULL or any required
 *                               function pointer was NULL.
 * @retval k_ra8_err_invalid_arg  URL string was empty or not
 *                               NUL-terminated; bank size 0; etc.
 * @retval k_ra8_err_invalid_state Already initialized.
 *
 * @pre ``cfg`` non-NULL.
 * @pre Every interface in ``cfg`` has all its function pointers set.
 * @post Module is in ``k_ra8_ota_state_idle``.
 * @post Subsequent ``ra8_ota_*`` calls are valid.
 *
 * @note Thread-safe: no, single-threaded init only.
 * @see ra8_ota_deinit
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_ota_init(const ra8_ota_cfg_t* cfg);

/**
 * @brief Tear the OTA module down (mostly for tests / re-init).
 *
 * @details
 * Aborts an in-progress download, signals the worker thread to exit,
 * and clears the module-static state. Safe to call from idle.
 *
 * @return ``ra8_err_t`` outcome.
 * @retval k_ra8_ok Module deinitialized.
 *
 * @pre None.
 * @post Module is back in the uninitialized state.
 * @post Calling any other ra8_ota_* API now returns
 *       ``k_ra8_err_not_initialized``.
 *
 * @note Thread-safe: no.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_ota_deinit(void);

/**
 * @brief Fetch and decode the manifest JSON over HTTPS.
 *
 * @details
 * Drives ``cfg.net.open / read / close`` against the manifest URL,
 * runs a small JSON parser to extract version, image URL, image
 * size, expected SHA-256 and ECDSA signature, and returns the
 * decoded struct in ``out_manifest``. The bytes of the JSON itself
 * are dropped after decoding -- only the fixed-size struct is kept.
 *
 * @param[out] out_manifest Non-NULL destination.
 *
 * @return ``ra8_err_t`` outcome.
 * @retval k_ra8_ok                  Manifest decoded.
 * @retval k_ra8_err_null_ptr        ``out_manifest`` was NULL.
 * @retval k_ra8_err_not_initialized ``ra8_ota_init`` not called.
 * @retval k_ra8_err_invalid_state   Module not in ``idle``.
 * @retval k_ra8_err_hw_error        Network backend reported an error.
 * @retval k_ra8_err_invalid_size    Manifest exceeded
 *                                  ``k_ra8_ota_manifest_max_bytes``.
 * @retval k_ra8_err_invalid_arg     JSON malformed / missing field.
 *
 * @pre ``ra8_ota_init`` has been called.
 * @pre ``out_manifest`` non-NULL.
 * @post On success, every field in ``*out_manifest`` is populated.
 * @post Module returns to ``k_ra8_ota_state_idle`` on either path.
 *
 * @note Thread-safe: no.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_ota_check_for_update(ra8_ota_manifest_t* out_manifest);

/**
 * @brief Stream the firmware blob into the inactive MRAM bank.
 *
 * @details
 * Walks the image URL one ``k_ra8_ota_chunk_bytes`` chunk at a
 * time. Each chunk is:
 *   1. Fed to the running SHA-256 context.
 *   2. Programmed into the inactive bank via
 *      ``cfg.flash.erase`` (first chunk only) and
 *      ``cfg.flash.program``.
 *   3. Reported to ``cfg.on_progress`` if registered.
 *
 * Writes are page-aligned (32-byte MRAM page). The function tracks
 * a high-water byte offset so it can resume cleanly if the caller
 * re-invokes it after a partial download.
 *
 * @param[in] manifest Non-NULL, validated manifest.
 *
 * @return ``ra8_err_t`` outcome.
 * @retval k_ra8_ok                  All bytes programmed.
 * @retval k_ra8_err_null_ptr        ``manifest`` was NULL.
 * @retval k_ra8_err_not_initialized Module not initialized.
 * @retval k_ra8_err_invalid_state   Wrong state for download.
 * @retval k_ra8_err_invalid_size    image_size > bank size.
 * @retval k_ra8_err_hw_error        Flash or network backend failed.
 *
 * @pre ``manifest`` non-NULL with valid fields.
 * @pre ``ra8_ota_init`` has been called.
 * @post On success, the inactive bank holds the new image.
 * @post On any error path, the inactive bank is left in an
 *       indeterminate state and must be re-erased before retrying.
 *
 * @note Thread-safe: no.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_ota_download_to_inactive_bank(const ra8_ota_manifest_t* manifest);

/**
 * @brief Verify SHA-256 + ECDSA over the freshly programmed bank.
 *
 * @details
 * Re-reads the inactive bank chunk-by-chunk, computes the SHA-256, and compares
 * it against ``manifest->image_sha256``. On a match it binds the manifest
 * metadata (``version`` / ``image_url`` / ``image_size_bytes``) into the signed
 * material -- the ECDSA verify runs over ``SHA-256`` of the version, URL, size and
 * image digest concatenated, not over the bare image digest
 * -- so a MITM that alters the declared version (an anti-rollback bypass),
 * redirects the URL, or changes the size cannot present a valid signature
 * (T5-05). Then it dispatches the crypto interface's ``ecdsa_verify`` over that
 * bound digest and ``manifest->signature``. All checks must pass.
 *
 * @param[in] manifest Non-NULL, validated manifest.
 *
 * @return ``ra8_err_t`` outcome.
 * @retval k_ra8_ok                  Verification passed.
 * @retval k_ra8_err_null_ptr        ``manifest`` was NULL.
 * @retval k_ra8_err_not_initialized Module not initialized.
 * @retval k_ra8_err_invalid_state   Wrong state for verification.
 * @retval k_ra8_err_crc_mismatch    SHA-256 digest mismatch.
 * @retval k_ra8_err_hw_error        ECDSA verification rejected the
 *                                  signature (bad sig or tampered metadata).
 *
 * @pre ``manifest`` non-NULL.
 * @pre Inactive bank holds the freshly downloaded image.
 * @post On success, the module advances to
 *       ``k_ra8_ota_state_committing``.
 * @post On failure, the module is in ``k_ra8_ota_state_error``.
 *
 * @note Thread-safe: no.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_ota_verify_signature(const ra8_ota_manifest_t* manifest);

/**
 * @brief Persist the bank swap and reboot.
 *
 * @details
 * Calls ``cfg.flash.set_startup`` with the inactive-bank index and
 * ``persistent = true`` (so BTFLG sticks across reset), sets the
 * state to ``k_ra8_ota_state_done``, and -- in production -- triggers
 * ``NVIC_SystemReset``. In the host test build the system-reset
 * call is forwarded to a weak ``ra8_ota_system_reset_hook`` symbol so
 * the test process does not actually exit.
 *
 * @return ``ra8_err_t`` outcome (caller usually never sees the
 *         success path on hardware -- the part is already resetting).
 * @retval k_ra8_ok                  Boot bank swapped (host build only).
 * @retval k_ra8_err_not_initialized Module not initialized.
 * @retval k_ra8_err_invalid_state   Verification has not passed.
 * @retval k_ra8_err_hw_error        ``set_startup`` failed.
 *
 * @pre Verification has passed (state == ``committing``).
 * @post On hardware, the part resets and boots from the new bank.
 *
 * @note Thread-safe: no.
 * @warning Calling this without prior verification will boot
 *          unverified code on the next reset.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_ota_commit_and_reboot(void);

/**
 * @brief Return the current state-machine value.
 *
 * @details
 * Direct read of the latched state byte. ``s_state`` is a single
 * ``uint8_t`` so a torn read is impossible on the target.
 *
 * @return One of ``ra8_ota_state_t``; ``k_ra8_ota_state_idle`` if the
 *         module has not been initialized.
 * @retval k_ra8_ota_state_idle        Uninitialized, idle, or already done.
 * @retval k_ra8_ota_state_checking    Manifest fetch in progress.
 * @retval k_ra8_ota_state_downloading Image download in progress.
 * @retval k_ra8_ota_state_verifying   Verifying signature.
 * @retval k_ra8_ota_state_committing  Committing bank swap.
 * @retval k_ra8_ota_state_done        Update complete.
 * @retval k_ra8_ota_state_error       Last step failed.
 *
 * @pre None.
 * @post No state change.
 *
 * @note Thread-safe: read of a single ``uint8_t``.
 * @since 0.1.0
 *
 * @pre Module has been initialized.
 * @post Side effects bounded to documented state.
 */
ra8_ota_state_t ra8_ota_get_state(void);

/**
 * @brief Drive the OTA state machine one step forward.
 *
 * @details
 * For callers that opted out of ``cfg.run_as_thread``: invoke this
 * from a main loop. Each call advances at most one transition. The
 * caller may inspect ``ra8_ota_get_state`` between invocations.
 *
 * @return ``k_ra8_ok`` on a successful step (including no-op when
 *         already idle/done) or a state-specific error code.
 * @retval k_ra8_ok                  Step completed.
 * @retval k_ra8_err_not_initialized Module not initialized.
 *
 * @pre ``ra8_ota_init`` has been called.
 * @post The state machine is in the next state, ``done``, or ``error``.
 *
 * @note Thread-safe: no (the worker thread, if any, owns the SM).
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_ota_run_step(void);

/**
 * @brief Kick off an end-to-end update from idle.
 *
 * @details
 * Sequences ``check_for_update`` -> ``download_to_inactive_bank`` ->
 * ``verify_signature`` -> ``commit_and_reboot`` against the cached
 * manifest. Used by the ThreadX worker thread; equally available
 * to single-threaded callers that prefer one call over driving the
 * state machine themselves.
 *
 * @return ``ra8_err_t`` outcome (the success path on hardware
 *         normally never returns -- the part resets first).
 *
 * @pre ``ra8_ota_init`` has been called.
 * @post Module is in ``done`` (success) or ``error`` (failure).
 *
 * @note Thread-safe: no.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_ota_run_full_update(void);

/**
 * @brief Test/host hook for ``commit_and_reboot``.
 *
 * @details
 * Called instead of ``NVIC_SystemReset`` when the implementation
 * detects it is running outside the ARM target (``RA8_OFF_TARGET``
 * defined). Default implementation is a no-op. Tests override to
 * count invocations.
 *
 * @pre None (safe to call any time after init).
 * @pre Caller has already latched the new boot bank.
 * @post Default implementation: no state mutation.
 * @post Target override: function does not return -- the part resets.
 *
 * @note Weak symbol; safe to leave unimplemented.
 * @since 0.1.0
 */
void ra8_ota_system_reset_hook(void);

#ifdef __cplusplus
}
#endif
