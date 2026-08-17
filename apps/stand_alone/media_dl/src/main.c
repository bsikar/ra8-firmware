/**
 * @file main.c
 * @brief media_dl -- host CLI for the e-reader media downloader.
 *
 * @details
 * Native host binary that links the firmware error contract (`ra8_err_t`) and
 * drives the downloader logic that will later run on the RA8 (only the injected
 * libcurl backend is host-specific). Modes:
 *
 *  - **series** (`--config S.conf --series URL`): read a site descriptor, list
 * a series' chapters, and download them into a reader-openable file. Selection
 *    is by chapter identity, not list position: `--from CHAP` starts at the
 *    chapter NUMBERED CHAP, and `--update` fetches only chapters not already
 *    recorded complete in the per-series library state. Downloads resume after
 *    interruption and never re-fetch a page whose bytes are already held.
 *  - **library** (over `--out`): `--list` shows tracked series with coverage
 * and gaps, `--update-all` incrementally updates every tracked series,
 * `--remove` drops one series.
 *  - **pack** (`--pack DIR --format FMT`): package an existing image folder
 * with no network.
 *  - **page** (bare `URL`): fetch one page and download its `<img>` URLs.
 *
 * The tool identifies itself honestly, honours robots.txt by default, and
 * sanitises every untrusted name before it reaches the filesystem.
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "fw_if_fs_posix.h"
#include "mdl_app.h"
#include "mdl_cli_internal.h"
#include "mdl_compose_internal.h"
#include "mdl_host_credentials_internal.h"
#include "mdl_sanitize.h"
#include "mdl_stream_internal.h"
#include "ra8_attributes.h"
#include "ra8_io_stream_posix.h"

/** @brief Naturally aligned, caller-owned exporter workspace storage. */
typedef struct {
  alignas(max_align_t) uint8_t bytes[k_export_arena_bytes]; /**< Bounded scratch bytes. */
} export_arena_storage_t;

/** @brief Maximally aligned storage for one filesystem backend handle. */
typedef struct {
  alignas(max_align_t) uint8_t bytes[k_storage_work_bytes]; /**< Opaque backend state. */
} storage_workspace_t;

/** @brief Maximally aligned storage for one filesystem directory cursor. */
typedef struct {
  alignas(max_align_t) uint8_t bytes[k_mdl_storage_io_bytes]; /**< Opaque cursor state. */
} directory_workspace_t;

/** @brief Explicit host credential input capacities. */
typedef enum : size_t {
  k_cookie_input_capacity = 64U * 1024U,  /**< Maximum cookie-file bytes. */
  k_ca_pem_input_capacity = 512U * 1024U, /**< Maximum custom CA bytes.   */
} credential_input_capacity_t;

/** @brief Process-lifetime exporter workspace storage (zero heap). */
static export_arena_storage_t s_export_arena;
/** @brief Host-selected filesystem facade and adapter state. */
static fw_fs_t             s_fs;
static fw_fs_posix_state_t s_fs_posix = {.root_fd = -1};
/** @brief One file and one transaction workspace for single-threaded storage.
 */
static storage_workspace_t s_fs_file_work;
static storage_workspace_t s_fs_transaction_work;
/** @brief One bounded host directory-cursor workspace. */
static directory_workspace_t s_fs_directory_work;
/** @brief Caller-owned streaming buffer shared by serial storage operations. */
static uint8_t s_fs_io_buffer[k_mdl_storage_io_bytes];
/** @brief Canonical host config path retained through one command run. */
static char s_config_path[PATH_MAX];
/** @brief Canonical host library root retained through one command run. */
static char s_library_path[PATH_MAX];
/** @brief Canonical host cache root retained through one command run. */
static char s_cache_path[PATH_MAX];
/** @brief Canonical verify, pack, or descriptor path retained for one run. */
static char s_mode_path[PATH_MAX];
/** @brief Process-lifetime, caller-owned cookie input bytes. */
static uint8_t s_cookie_input[k_cookie_input_capacity];
/** @brief Process-lifetime, caller-owned custom CA PEM bytes. */
static uint8_t s_ca_pem_input[k_ca_pem_input_capacity];
/** @brief Process-lifetime bounded application state. */
static mdl_app_context_t s_app;
/** @brief Process stdout and stderr exposed through the portable byte-stream
 * facade. */
static ra8_io_stream_t s_output;
static ra8_io_stream_t s_diagnostic;
/** @brief Borrowed raw-descriptor backend state for process output streams. */
static ra8_io_stream_posix_state_t s_output_posix;
static ra8_io_stream_posix_state_t s_diagnostic_posix;

/**
 * @brief Bind process output descriptors and surface broken pipes as errors.
 * @return Canonical composition status.
 * @retval k_ra8_ok Both output streams are ready.
 * @retval k_ra8_err_comm_error The process signal disposition could not be
 * installed.
 * @retval other A raw-descriptor stream binding failed.
 * @pre Standard output and standard error descriptors are process-owned.
 * @pre No output operation has started.
 * @post Success binds borrowed descriptors for the process lifetime.
 * @post SIGPIPE is ignored so descriptor writes return EPIPE to the adapter.
 * @note Host composition only; no descriptor ownership is transferred.
 * @since 0.1.0

 * @details Uses process-lifetime caller-owned storage and injected I/O.
 *          Publishes no partial path or credential view after a failed validation.
 */
RA8_INTERNAL static ra8_err_t internal_output_init(void)
{
  struct sigaction action = {.sa_handler = SIG_IGN};
  if ((sigemptyset(&action.sa_mask) != 0) || (sigaction(SIGPIPE, &action, nullptr) != 0)) {
    return k_ra8_err_comm_error;
  }
  ra8_err_t err = ra8_io_stream_posix_init(&s_output, &s_output_posix, STDOUT_FILENO);
  if (err == k_ra8_ok) {
    err = ra8_io_stream_posix_init(&s_diagnostic, &s_diagnostic_posix, STDERR_FILENO);
  }
  return err;
}

/**
 * @brief Write one fixed process diagnostic through the injected sink.
 * @param[in] message NUL-terminated diagnostic including its newline.
 * @return Canonical stream status.
 * @pre @p message is non-NULL and NUL-terminated.
 * @pre ::internal_output_init completed successfully.
 * @post Success writes the complete diagnostic without C-runtime buffering.
 * @post Failure is returned without retrying at another destination.
 * @note Not thread-safe with concurrent writes to ::s_diagnostic.
 * @since 0.1.0

 * @details Uses process-lifetime caller-owned storage and injected I/O.
 *          Publishes no partial path or credential view after a failed validation.
 * @retval k_ra8_ok The operation completed successfully.
 * @retval other The originating validation, storage, stream, or network error.
 */
RA8_INTERNAL static ra8_err_t internal_diagnostic(const char* message)
{
  return ra8_io_stream_puts(&s_diagnostic, message);
}

/**
 * @brief Read one already-open credential file and reject concurrent mutation.
 * @param[in] fd Raw descriptor pinned to the selected regular file.
 * @param[in] before Metadata captured immediately after open.
 * @param[out] destination Caller-owned credential storage.
 * @param[in] length Exact expected file length.
 * @return Canonical read or mutation status.
 * @retval k_ra8_ok Exactly @p length stable bytes were read.
 * @retval k_ra8_err_access_denied A read/stat failed or the source mutated.
 * @pre @p fd is readable and @p destination has at least @p length bytes.
 * @post Success fills exactly @p length bytes and observes EOF at that extent.
 * @post Failure publishes no byte-view length to portable policy.
 * @note Host composition only; reads are positioned and bounded.
 * @since 0.1.0

 * @details Uses process-lifetime caller-owned storage and injected I/O.
 *          Publishes no partial path or credential view after a failed validation.
 * @pre Every required pointer is non-null and remains valid for the call.
 */
RA8_INTERNAL static ra8_err_t
internal_read_credential(int fd, const struct stat* before, uint8_t* destination, size_t length)
{
  size_t offset = 0U;
  while (offset < length) {
    ssize_t received = pread(fd, destination + offset, length - offset, (off_t)offset);
    if ((received < 0) && (errno == EINTR)) {
      continue;
    }
    if (received <= 0) {
      return k_ra8_err_access_denied;
    }
    offset += (size_t)received;
  }
  uint8_t extra    = 0U;
  ssize_t overflow = 0;
  do {
    overflow = pread(fd, &extra, 1U, (off_t)length);
  } while ((overflow < 0) && (errno == EINTR));
  struct stat after = {};
  if ((overflow != 0) || (fstat(fd, &after) != 0) ||
      !priv_mdl_host_credential_stat_unchanged(before, &after)) {
    return k_ra8_err_access_denied;
  }
  return k_ra8_ok;
}

/**
 * @brief Load one path argument into exact caller-supplied credential storage.
 * @param[in] path Host path accepted only at this composition boundary.
 * @param[out] destination Caller-owned credential byte storage.
 * @param[in] supplied Exact destination capacity in bytes.
 * @param[out] required Source size required for a complete snapshot.
 * @param[out] used Complete stable byte count on success.
 * @return Canonical path, type, capacity, read, or mutation status.
 * @retval k_ra8_ok A stable regular-file snapshot was retained.
 * @retval k_ra8_err_invalid_arg The source is not a regular file.
 * @retval k_ra8_err_invalid_size The source exceeds @p supplied.
 * @retval k_ra8_err_access_denied Open, stat, read, mutation, or close failed.
 * @pre All pointer arguments are non-NULL and @p path is NUL-terminated.
 * @post @p required reports the observed source size when representable.
 * @post Success sets @p used to @p required; failure leaves @p used zero.
 * @note `O_NOFOLLOW` rejects a symlink at the final component.
 * @since 0.1.0

 * @details Uses process-lifetime caller-owned storage and injected I/O.
 *          Publishes no partial path or credential view after a failed validation.
 * @pre Every required pointer is non-null and remains valid for the call.
 */
RA8_INTERNAL static ra8_err_t internal_load_credential(const char* path,
                                                       uint8_t*    destination,
                                                       size_t      supplied,
                                                       size_t*     required,
                                                       size_t*     used)
{
  *required    = 0U;
  *used        = 0U;
  const int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
  if (fd < 0) {
    return k_ra8_err_access_denied;
  }
  struct stat before = {};
  ra8_err_t   error  = k_ra8_ok;
  if ((fstat(fd, &before) != 0) || (before.st_size < 0)) {
    error = k_ra8_err_access_denied;
  } else if (!S_ISREG(before.st_mode)) {
    error = k_ra8_err_invalid_arg;
  } else if ((uintmax_t)before.st_size > (uintmax_t)SIZE_MAX) {
    *required = SIZE_MAX;
    error     = k_ra8_err_invalid_size;
  } else {
    *required = (size_t)before.st_size;
    error = (*required > supplied) ? k_ra8_err_invalid_size
                                   : internal_read_credential(fd, &before, destination, *required);
  }
  if ((close(fd) != 0) && (error == k_ra8_ok)) {
    error = k_ra8_err_access_denied;
  }
  if (error == k_ra8_ok) {
    *used = *required;
  }
  return error;
}

/**
 * @brief Emit an exact required-versus-supplied credential capacity error.
 * @param[in,out] diagnostic Bound diagnostic byte stream.
 * @param[in] option NUL-terminated CLI option name.
 * @param[in] required Exact observed input bytes.
 * @param[in] supplied Exact caller-owned capacity bytes.
 * @return First stream error, or ::k_ra8_ok.
 * @since 0.1.0

 * @details Uses process-lifetime caller-owned storage and injected I/O.
 *          Publishes no partial path or credential view after a failed validation.
 * @retval k_ra8_ok The operation completed successfully.
 * @retval other The originating validation, storage, stream, or network error.
 * @pre Every required pointer is non-null and remains valid for the call.
 * @pre Lengths and capacities describe complete referenced objects without overflow.
 * @post Documented outputs and the return value describe the same outcome.
 * @post A rejected or failed operation is never reported as successful.
 * @note Thread safety follows ownership of the supplied context; no synchronization is added.
 */
RA8_INTERNAL static ra8_err_t internal_credential_capacity_error(ra8_io_stream_t* diagnostic,
                                                                 const char*      option,
                                                                 size_t           required,
                                                                 size_t           supplied)
{
  ra8_err_t error = priv_mdl_stream_text(k_ra8_ok, diagnostic, "media_dl: ");
  error           = priv_mdl_stream_text(error, diagnostic, option);
  error = priv_mdl_stream_text(error, diagnostic, " input exceeds credential capacity (required ");
  error = priv_mdl_stream_u64(error, diagnostic, required);
  error = priv_mdl_stream_text(error, diagnostic, ", supplied ");
  error = priv_mdl_stream_u64(error, diagnostic, supplied);
  return priv_mdl_stream_text(error, diagnostic, ")\n");
}

/**
 * @brief Snapshot optional host credential paths into portable byte policy.
 * @param[in] args Validated CLI arguments retaining host-only paths.
 * @param[in,out] policy Network policy receiving read-only byte views.
 * @param[in,out] diagnostic Bound diagnostic stream.
 * @return Canonical credential preparation status.
 * @pre All arguments are non-NULL and credential paths are NUL-terminated.
 * @post Success exposes no credential path beyond this composition function.
 * @post Failure leaves both policy byte views empty.
 * @since 0.1.0

 * @details Uses process-lifetime caller-owned storage and injected I/O.
 *          Publishes no partial path or credential view after a failed validation.
 * @retval k_ra8_ok The operation completed successfully.
 * @retval other The originating validation, storage, stream, or network error.
 * @pre Every required pointer is non-null and remains valid for the call.
 * @note Thread safety follows ownership of the supplied context; no synchronization is added.
 */
RA8_INTERNAL static ra8_err_t internal_prepare_credentials(const mdl_args_t* args,
                                                           mdl_net_policy_t* policy,
                                                           ra8_io_stream_t*  diagnostic)
{
  mdl_net_bytes_t cookies  = {};
  size_t          required = 0U;
  size_t          used     = 0U;
  policy->cookies          = (mdl_net_bytes_t){};
  policy->ca_pem           = (mdl_net_bytes_t){};
  if (args->cookie_file != nullptr) {
    ra8_err_t error = internal_load_credential(args->cookie_file,
                                               s_cookie_input,
                                               sizeof(s_cookie_input),
                                               &required,
                                               &used);
    if (error == k_ra8_err_invalid_size) {
      (void)internal_credential_capacity_error(diagnostic,
                                               "--cookie-file",
                                               required,
                                               sizeof(s_cookie_input));
    } else if (error != k_ra8_ok) {
      (void)ra8_io_stream_puts(diagnostic, "media_dl: could not load safe --cookie-file input\n");
    }
    if (error != k_ra8_ok) {
      return error;
    }
    cookies = (used == 0U) ? (mdl_net_bytes_t){}
                           : (mdl_net_bytes_t){.data = s_cookie_input, .length = used};
  }
  if (args->ca_file == nullptr) {
    policy->cookies = cookies;
    return k_ra8_ok;
  }
  const ra8_err_t error = internal_load_credential(args->ca_file,
                                                   s_ca_pem_input,
                                                   sizeof(s_ca_pem_input),
                                                   &required,
                                                   &used);
  if (error == k_ra8_err_invalid_size) {
    (void)
      internal_credential_capacity_error(diagnostic, "--ca-file", required, sizeof(s_ca_pem_input));
  } else if ((error != k_ra8_ok) || (used == 0U)) {
    (void)ra8_io_stream_puts(diagnostic,
                             "media_dl: could not load safe nonempty --ca-file input\n");
  }
  if ((error != k_ra8_ok) || (used == 0U)) {
    return (error == k_ra8_ok) ? k_ra8_err_invalid_arg : error;
  }
  policy->cookies = cookies;
  policy->ca_pem  = (mdl_net_bytes_t){.data = s_ca_pem_input, .length = used};
  return k_ra8_ok;
}

/**
 * @brief Convert one CLI or output status to the process exit convention.
 * @param[in] err Canonical operation status.
 * @param[in] usage_error Whether invalid argument maps to usage exit 2.
 * @return Process exit status.
 * @retval 0 @p err is ::k_ra8_ok.
 * @retval 2 @p usage_error is true and @p err is invalid-argument.
 * @retval 1 Every output or operational failure.
 * @pre @p err is a canonical ::ra8_err_t value.
 * @pre The caller has already emitted any applicable diagnostic.
 * @post No stream or process state is modified.
 * @note Thread-safe: pure value mapping.
 * @since 0.1.0

 * @details Uses process-lifetime caller-owned storage and injected I/O.
 *          Publishes no partial path or credential view after a failed validation.
 * @post Documented outputs and the return value describe the same outcome.
 */
RA8_INTERNAL static int internal_exit_from_error(ra8_err_t err, bool usage_error)
{
  if (err == k_ra8_ok) {
    return 0;
  }
  return (usage_error && (err == k_ra8_err_invalid_arg)) ? 2 : 1;
}

/**
 * @brief Bind the host root through the POSIX composition adapter.
 * @details Performs storage init through the injected filesystem facade and
 * keeps source and transaction handles caller-owned on every return path.
 * @return Canonical storage-composition status.
 * @retval k_ra8_ok The POSIX facade and downloader binding are ready.
 * @retval other POSIX adapter or workspace binding initialization failed.
 * @pre Process-lifetime storage objects are not currently initialized.
 * @pre Static workspaces remain exclusively owned by the CLI thread.
 * @post Success leaves the shared storage binding ready until deinitialization.
 * @post Failure leaves no initialized POSIX adapter resource.
 * @note Host composition only; firmware supplies a VFS facade instead.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_storage_init(void)
{
  const fw_fs_posix_cfg_t cfg = {.root_path = "/", .removable_media = false};
  ra8_err_t               err = fw_fs_posix_init(&s_fs, &s_fs_posix, &cfg);
  if (err != k_ra8_ok) {
    return err;
  }
  err = mdl_storage_init(&s_app.storage,
                         &s_fs,
                         s_fs_file_work.bytes,
                         sizeof(s_fs_file_work.bytes),
                         s_fs_transaction_work.bytes,
                         sizeof(s_fs_transaction_work.bytes),
                         s_fs_io_buffer,
                         sizeof(s_fs_io_buffer));
  if (err == k_ra8_ok) {
    err = mdl_library_workspace_init(&s_app.library_workspace,
                                     s_fs_directory_work.bytes,
                                     (uint32_t)sizeof(s_fs_directory_work.bytes));
  }
  if (err != k_ra8_ok) {
    (void)fw_fs_posix_deinit(&s_fs_posix);
  }
  return err;
}

/**
 * @brief Resolve an optional host config argument into the root-bound
 * namespace.
 * @param[in,out] args Parsed command arguments whose config pointer may change.
 * @return Canonicalization status.
 * @retval k_ra8_ok No config was supplied or its canonical path was retained.
 * @retval k_ra8_err_not_found The host could not resolve the supplied path.
 * @pre @p args is non-NULL and its optional config string is NUL-terminated.
 * @pre The host composition binds portable storage at `/`.
 * @post Success with a config points `args->cfg` at process-lifetime storage.
 * @post No portable/domain reader observes a relative or symlinked host path.
 * @note Host composition only; device composition supplies canonical VFS paths.
 * @since 0.1.0

 * @details Uses process-lifetime caller-owned storage and injected I/O.
 *          Publishes no partial path or credential view after a failed validation.
 */
RA8_INTERNAL static ra8_err_t internal_resolve_config_path(mdl_args_t* args)
{
  if (args->cfg == nullptr) {
    return k_ra8_ok;
  }
  if (realpath(args->cfg, s_config_path) == nullptr) {
    return k_ra8_err_not_found;
  }
  if (strlen(s_config_path) >= (size_t)k_fw_fs_path_cap) {
    return k_ra8_err_invalid_size;
  }
  args->cfg = s_config_path;
  return k_ra8_ok;
}

/**
 * @brief Resolve a library root, including one absent final directory.
 * @details Existing roots pass through `realpath`; for an absent root only its
 *          existing canonical parent is resolved and one safe leaf is joined.
 * @param[in] input Existing canonical path or path with one absent final leaf.
 * @param[out] destination Receives the canonical absolute path.
 * @param[in] capacity Writable bytes at @p destination.
 * @return Canonical host-composition status.
 * @retval k_ra8_ok The root is canonical and retained for the command lifetime.
 * @retval k_ra8_err_not_found The path or its immediate parent cannot resolve.
 * @retval k_ra8_err_invalid_arg The absent final component is unsafe.
 * @pre @p input and @p destination are non-NULL and @p input is NUL-terminated.
 * @pre The host storage composition is bound to `/`.
 * @post Success NUL-terminates @p destination within @p capacity.
 * @post Domain library code never receives a relative or unresolved host path.
 * @note Host composition only; firmware supplies canonical VFS paths directly.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t
internal_resolve_one_leaf(const char* input, char* destination, size_t capacity)
{
  if (realpath(input, destination) != nullptr) {
    if ((strlen(destination) >= capacity) || (strlen(destination) >= (size_t)k_fw_fs_path_cap)) {
      return k_ra8_err_invalid_size;
    }
    return k_ra8_ok;
  }
  const char* slash = strrchr(input, '/');
  const char* leaf  = (slash == nullptr) ? input : slash + 1;
  if ((leaf[0] == '\0') || (strcmp(leaf, ".") == 0) || (strcmp(leaf, "..") == 0)) {
    return k_ra8_err_invalid_arg;
  }
  char parent[PATH_MAX];
  if (slash == nullptr) {
    (void)memcpy(parent, ".", 2U);
  } else if (slash == input) {
    (void)memcpy(parent, "/", 2U);
  } else {
    const size_t parent_bytes = (size_t)(slash - input);
    if (parent_bytes >= sizeof(parent)) {
      return k_ra8_err_invalid_size;
    }
    memcpy(parent, input, parent_bytes);
    parent[parent_bytes] = '\0';
  }
  char canonical_parent[PATH_MAX];
  if (realpath(parent, canonical_parent) == nullptr) {
    return k_ra8_err_not_found;
  }
  if (!mdl_path_join(canonical_parent, leaf, destination, capacity)) {
    return k_ra8_err_invalid_arg;
  }
  return k_ra8_ok;
}

/**
 * @brief Canonicalize the command output root with one absent leaf allowed.
 * @details Resolves the existing directory or its canonical parent, then
 *          redirects the parsed output view to process-lifetime storage.
 * @param[in,out] args Parsed arguments whose output pointer is replaced.
 * @return Canonical host-composition status.
 * @retval k_ra8_ok The output root is canonical and portable-sized.
 * @retval other The path is missing, unsafe, or too large.
 * @pre @p args and `args->out` are non-NULL and NUL-terminated.
 * @pre Host path composition is bound for the command lifetime.
 * @post Success points `args->out` at process-lifetime storage.
 * @post Failure leaves `args->out` unchanged.
 * @note Host composition only; portable runners receive canonical paths.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_resolve_output_path(mdl_args_t* args)
{
  const ra8_err_t error =
    internal_resolve_one_leaf(args->out, s_library_path, sizeof(s_library_path));
  if (error != k_ra8_ok) {
    return error;
  }
  args->out = s_library_path;
  return k_ra8_ok;
}

/**
 * @brief Resolve or derive the persistent per-host cache root.
 * @details Uses the explicit cache path when supplied; otherwise joins the
 *          fixed `.mdl_cache` leaf beneath the canonical output root.
 * @param[in,out] args Parsed arguments receiving the canonical cache path.
 * @return Canonical host-path status.
 * @retval k_ra8_ok The cache root is canonical and portable-sized.
 * @retval other The supplied or derived path is unsafe or too large.
 * @pre @p args is non-NULL and its output root is canonical.
 * @pre A supplied cache path has at most one absent final component.
 * @post Success points `cache_dir` at process-lifetime storage.
 * @post The default is exactly `OUT/.mdl_cache`.
 * @note Directory creation occurs later through the portable storage facade.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_resolve_cache_path(mdl_args_t* args)
{
  ra8_err_t error = k_ra8_ok;
  if (args->cache_dir == nullptr) {
    if (!mdl_path_join(args->out, ".mdl_cache", s_cache_path, sizeof(s_cache_path))) {
      return k_ra8_err_invalid_size;
    }
  } else {
    error = internal_resolve_one_leaf(args->cache_dir, s_cache_path, sizeof(s_cache_path));
  }
  if (error == k_ra8_ok) {
    args->cache_dir = s_cache_path;
  }
  return error;
}

/**
 * @brief True when a validated mode consumes the canonical output root.
 * @param[in] mode Validated command mode.
 * @return Whether `args->out` crosses into portable application code.
 * @retval true The mode consumes the output root.
 * @retval false The mode has no output-root dependency.
 * @pre @p mode was produced by ::mdl_cli_validate.
 * @pre The caller has applied the default output string.
 * @post No argument or process state is modified.
 * @note Thread-safe: pure value classification.
 * @since 0.1.0

 * @details Uses process-lifetime caller-owned storage and injected I/O.
 *          Publishes no partial path or credential view after a failed validation.
 * @post Documented outputs and the return value describe the same outcome.
 */
RA8_INTERNAL static bool internal_uses_output_path(mdl_cli_mode_t mode)
{
  return (mode == k_mdl_cli_mode_series) || (mode == k_mdl_cli_mode_search) ||
         (mode == k_mdl_cli_mode_browse) || (mode == k_mdl_cli_mode_list) ||
         (mode == k_mdl_cli_mode_update_all) || (mode == k_mdl_cli_mode_remove) ||
         (mode == k_mdl_cli_mode_artifact) || (mode == k_mdl_cli_mode_page);
}

/**
 * @brief Classify modes that perform persistent document caching.
 * @details Restricts cache-path setup to modes that may retrieve series,
 *          chapter, or cover documents through the cache coordinator.
 * @param[in] mode Validated CLI mode.
 * @return Whether the mode may fetch a series or chapter index.
 * @retval true The selected mode may read or publish cache state.
 * @retval false The selected mode does not consume a cache path.
 * @pre @p mode is a valid enum value.
 * @pre CLI mode selection has completed.
 * @post No process or argument state is modified.
 * @post True is limited to series/discovery/update modes.
 * @note List/remove operations do not need the cache path.
 * @since 0.1.0
 */
RA8_INTERNAL static bool internal_uses_cache_path(mdl_cli_mode_t mode)
{
  return (mode == k_mdl_cli_mode_series) || (mode == k_mdl_cli_mode_search) ||
         (mode == k_mdl_cli_mode_browse) || (mode == k_mdl_cli_mode_update_all);
}

/**
 * @brief Canonicalize the existing pack input directory.
 * @param[in,out] args Parsed arguments whose pack pointer is replaced.
 * @return Canonical host-composition status.
 * @retval k_ra8_ok The selected input is canonical and portable-sized.
 * @retval k_ra8_err_not_found The selected directory cannot be resolved.
 * @retval k_ra8_err_invalid_size The canonical path exceeds the facade cap.
 * @pre @p args is non-null and `args->pack` is NUL-terminated.
 * @pre Pack mode was selected by ::mdl_cli_validate.
 * @post Success points `args->pack` at process-lifetime storage.
 * @post Failure leaves the parsed pointer unchanged.
 * @note Host composition only; portable runners receive canonical paths.
 * @since 0.1.0

 * @details Uses process-lifetime caller-owned storage and injected I/O.
 *          Publishes no partial path or credential view after a failed validation.
 */
RA8_INTERNAL static ra8_err_t internal_resolve_pack_path(mdl_args_t* args)
{
  if (realpath(args->pack, s_mode_path) == nullptr) {
    return k_ra8_err_not_found;
  }
  if (strlen(s_mode_path) >= (size_t)k_fw_fs_path_cap) {
    return k_ra8_err_invalid_size;
  }
  args->pack = s_mode_path;
  return k_ra8_ok;
}

/**
 * @brief Canonicalize a verify target while preserving absent-leaf diagnostics.
 * @details Reuses output-root resolution so one missing final component becomes
 *          a canonical portable path; the verifier then reports the specific
 *          unreadable-directory failure instead of preparation masking it.
 * @param[in,out] args Parsed arguments whose selected verify path is replaced.
 * @return Canonical host-composition status.
 * @retval k_ra8_ok The verify target is canonical and portable-sized.
 * @retval other The target or its immediate parent cannot be resolved safely.
 * @pre @p args is non-null and its selected verify path is NUL-terminated.
 * @pre Verify mode was selected by ::mdl_cli_validate.
 * @post Success updates `verify_dir` when explicit, otherwise `out`.
 * @post Failure restores the original `out` pointer.
 * @note Host composition only; no filesystem mutation occurs.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_resolve_verify_path(mdl_args_t* args)
{
  const char* const saved_out = args->out;
  if (args->verify_dir != nullptr) {
    args->out = args->verify_dir;
  }
  const ra8_err_t error = internal_resolve_output_path(args);
  if (args->verify_dir != nullptr) {
    if (error == k_ra8_ok) {
      args->verify_dir = args->out;
    }
    args->out = saved_out;
  }
  return error;
}

/**
 * @brief Select the canonical init-site descriptor directory.
 * @details Uses a real `sites` child only when `lstat` proves the child is a
 *          directory rather than a symlink; otherwise it selects the current
 *          canonical directory. Unexpected inspection failures stop the run.
 * @param[in,out] args Parsed arguments whose output field receives the
 * directory.
 * @return Canonical host-composition status.
 * @retval k_ra8_ok A safe portable-sized descriptor directory was selected.
 * @retval k_ra8_err_not_found The current directory could not be resolved.
 * @retval k_ra8_err_access_denied The candidate child could not be inspected.
 * @retval k_ra8_err_invalid_size The selected namespace exceeds the path cap.
 * @pre @p args is non-null and init-site mode was validated.
 * @pre The process current directory remains stable during preparation.
 * @post Success points `args->out` at process-lifetime canonical storage.
 * @post Failure publishes no descriptor path to application code.
 * @note Host composition only; the app itself has no current-directory policy.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_resolve_descriptor_path(mdl_args_t* args)
{
  if (realpath(".", s_mode_path) == nullptr) {
    return k_ra8_err_not_found;
  }
  if (strlen(s_mode_path) >= (size_t)k_fw_fs_path_cap) {
    return k_ra8_err_invalid_size;
  }
  char sites[k_fw_fs_path_cap];
  if (!mdl_path_join(s_mode_path, "sites", sites, sizeof(sites))) {
    return k_ra8_err_invalid_size;
  }
  struct stat status = {};
  if (lstat(sites, &status) == 0) {
    if (S_ISDIR(status.st_mode) && !S_ISLNK(status.st_mode)) {
      (void)memcpy(s_mode_path, sites, strlen(sites) + 1U);
    }
  } else if (errno != ENOENT) {
    return k_ra8_err_access_denied;
  }
  args->out = s_mode_path;
  return k_ra8_ok;
}

/**
 * @brief Apply host defaults and canonicalize paths for the selected mode.
 * @param[in,out] args Validated parsed arguments.
 * @param[in] mode Validated CLI mode.
 * @param[in,out] diagnostic Bound stream receiving preparation diagnostics.
 * @return Process-style preparation status.
 * @retval 0 Defaults and required host paths are ready.
 * @retval 1 A required config or library path could not be resolved.
 * @pre @p args and @p diagnostic are non-NULL and @p mode came from
 *      ::mdl_cli_validate.
 * @pre Optional argument strings remain NUL-terminated.
 * @post Success supplies default output/image-attribute strings when absent.
 * @post Every domain-facing filesystem path is canonical and portable-sized.
 * @note The ignore-robots warning is emitted exactly once here.
 * @since 0.1.0

 * @details Uses process-lifetime caller-owned storage and injected I/O.
 *          Publishes no partial path or credential view after a failed validation.
 */
RA8_INTERNAL static int
internal_prepare_args(mdl_args_t* args, mdl_cli_mode_t mode, ra8_io_stream_t* diagnostic)
{
  if (args->out == nullptr) {
    args->out = "downloads";
  }
  if (args->attr == nullptr) {
    args->attr = "data-src";
  }
  if (internal_resolve_config_path(args) != k_ra8_ok) {
    (void)ra8_io_stream_puts(diagnostic, "media_dl: could not resolve config path\n");
    return 1;
  }
  ra8_err_t path_error = k_ra8_ok;
  if (internal_uses_output_path(mode)) {
    path_error = internal_resolve_output_path(args);
  } else if (mode == k_mdl_cli_mode_verify) {
    path_error = internal_resolve_verify_path(args);
  } else if (mode == k_mdl_cli_mode_pack) {
    path_error = internal_resolve_pack_path(args);
  } else if (mode == k_mdl_cli_mode_init_site) {
    path_error = internal_resolve_descriptor_path(args);
  }
  if (path_error != k_ra8_ok) {
    (void)ra8_io_stream_puts(diagnostic, "media_dl: could not resolve portable command path\n");
    return 1;
  }
  if (internal_uses_cache_path(mode) && (internal_resolve_cache_path(args) != k_ra8_ok)) {
    (void)ra8_io_stream_puts(diagnostic, "media_dl: could not resolve persistent cache path\n");
    return 1;
  }
  if (args->ignore_robots) {
    (void)ra8_io_stream_puts(diagnostic,
                             "media_dl: WARNING: --ignore-robots set; "
                             "robots.txt will NOT be honoured\n");
  }
  return 0;
}

/**
 * @brief Validate output format and attach bounded credential byte views.
 * @param[in] args Validated parsed command arguments.
 * @param[in,out] opts Mutable run policy receiving credential views.
 * @param[out] format Validated output format.
 * @return Process-style preparation status.
 * @retval 0 Format and credential policy are ready.
 * @retval 1 Credential preparation failed.
 * @retval 2 The requested format is invalid.
 * @pre All arguments are non-NULL and process diagnostics are bound.
 * @post Success leaves @p opts and @p format ready for the complete run.
 * @post Failure performs no network or portable storage operation.
 * @since 0.1.0

 * @details Uses process-lifetime caller-owned storage and injected I/O.
 *          Publishes no partial path or credential view after a failed validation.
 * @pre Every required pointer is non-null and remains valid for the call.
 * @note Thread safety follows ownership of the supplied context; no synchronization is added.
 */
RA8_INTERNAL static int
internal_prepare_run_policy(const mdl_args_t* args, mdl_run_opts_t* opts, ra8_mdl_format_t* format)
{
  *format = mdl_format_from_str(args->format);
  if (*format == k_ra8_mdl_format_invalid) {
    const char* const parts[] = {"media_dl: bad --format '",
                                 args->format,
                                 "' (loose|cbz|cbt|cbt.gz|epub|jof|rabook)\n"};
    const ra8_err_t   error =
      priv_mdl_cli_reject_parts(&s_diagnostic, parts, sizeof(parts) / sizeof(parts[0]));
    return internal_exit_from_error(error, true);
  }
  return (internal_prepare_credentials(args, &opts->policy, &s_diagnostic) == k_ra8_ok) ? 0 : 1;
}

/**
 * @brief Bootstrap process I/O and workspace state, then parse arguments.
 * @details Initializes the output stream and composition-root context,
 *          binds the fixed export arena, and parses @p argv into @p a.
 * @param[in] argc Argument count.
 * @param[in] argv Argument vector.
 * @param[out] a Receives the parsed (not yet validated) command arguments.
 * @return Process-style bootstrap status.
 * @retval 0 Output, context, and argument parsing are ready.
 * @retval 1 Output stream initialization failed.
 * @pre @p argv is non-NULL and holds @p argc entries.
 * @pre @p a addresses writable storage for one complete `mdl_args_t`.
 * @post On success @p a holds the raw parsed arguments.
 * @post A failed output-stream init binds no context and no export arena.
 * @note Not thread-safe; initializes process-lifetime shared state.
 * @since 0.1.0
 */
RA8_INTERNAL static int internal_main_init(int argc, char** argv, mdl_args_t* a)
{
  if (internal_output_init() != k_ra8_ok) {
    return 1;
  }
  s_app.output     = &s_output;
  s_app.diagnostic = &s_diagnostic;
  s_app.io_error   = k_ra8_ok;
  mdl_export_workspace_init(&s_app.export_ws, s_export_arena.bytes, sizeof(s_export_arena.bytes));
  mdl_app_bind(&s_app);
  mdl_cli_parse(argc, argv, a);
  return 0;
}

/**
 * @brief Build the series-run plan, then dispatch and shut down cleanly.
 * @details Binds the portable filesystem, dispatches to the selected mode
 *          handler, and always attempts filesystem shutdown before returning.
 * @param[in] a Validated parsed command arguments.
 * @param[in] mode Selected run mode.
 * @param[in] format Validated output format.
 * @param[in] opts Prepared run policy.
 * @param[in] nums Parsed numeric options.
 * @return Process-style run status.
 * @retval 0 The dispatched mode completed successfully.
 * @retval 1 Filesystem binding or shutdown failed.
 * @retval other The dispatched mode's own failure status.
 * @pre All pointers are non-NULL.
 * @pre @p opts already carries the credential views the run policy prepared.
 * @post Filesystem shutdown is attempted exactly once regardless of outcome.
 * @post No mode handler runs when the portable filesystem binding fails.
 * @note Not thread-safe; binds and releases process-lifetime storage.
 * @since 0.1.0
 */
RA8_INTERNAL static int internal_main_run(const mdl_args_t*     a,
                                          mdl_cli_mode_t        mode,
                                          ra8_mdl_format_t      format,
                                          const mdl_run_opts_t* opts,
                                          const mdl_nums_t*     nums)
{
  const mdl_series_run_t run = priv_mdl_compose_build_run(a, format, opts, nums);
  if (internal_storage_init() != k_ra8_ok) {
    (void)internal_diagnostic("media_dl: could not initialize portable filesystem binding\n");
    return 1;
  }
  const int result = priv_mdl_compose_dispatch(a, mode, format, opts, nums, &run);
  if (fw_fs_posix_deinit(&s_fs_posix) != k_ra8_ok) {
    (void)internal_diagnostic("media_dl: filesystem shutdown failed\n");
    return 1;
  }
  return result;
}

/**
 * @brief Program entry point: parse the command line and select a run mode.
 * @details Parses and validates the arguments, then hands off to
 * ::priv_mdl_compose_dispatch, which selects a library command
 * (`--list`/`--remove`/`--update-all`), search/browse discovery
 * (`--search`/`--browse`), pack mode (`--pack`), series mode (`--config` +
 * `--series`), or single-page mode (a bare URL), in that precedence.
 * @param[in] argc Argument count.
 * @param[in] argv Argument vector.
 * @return 0 on success, 1 on a download/export failure, 2 on a usage error.
 * @since 0.1.0
 */
int main(int argc, char** argv)
{
  mdl_args_t a = {};
  if (internal_main_init(argc, argv, &a) != 0) {
    return 1;
  }
  mdl_cli_mode_t mode    = k_mdl_cli_mode_invalid;
  ra8_err_t      cli_err = mdl_cli_validate(&a, &s_diagnostic, &mode);
  if (cli_err != k_ra8_ok) {
    if ((cli_err == k_ra8_err_invalid_arg) && (mdl_cli_usage(&s_diagnostic, argv[0]) != k_ra8_ok)) {
      return 1;
    }
    return internal_exit_from_error(cli_err, true);
  }
  if (mode == k_mdl_cli_mode_help) {
    return internal_exit_from_error(mdl_cli_usage(&s_diagnostic, argv[0]), false);
  }
  if (mode == k_mdl_cli_mode_version) {
    return internal_exit_from_error(ra8_io_stream_puts(&s_output, "media_dl 0.1.0\n"), false);
  }
  if (internal_prepare_args(&a, mode, &s_diagnostic) != 0) {
    return 1;
  }

  mdl_nums_t nums = {};
  cli_err         = mdl_cli_parse_nums(&a, &s_diagnostic, &nums);
  if (cli_err != k_ra8_ok) {
    return internal_exit_from_error(cli_err, true);
  }
  mdl_run_opts_t opts = mdl_cli_run_opts(&a);
  /* The one place this build form's transport is chosen. */
  opts.net                       = priv_mdl_compose_net_provider();
  ra8_mdl_format_t format        = k_ra8_mdl_format_invalid;
  const int        policy_status = internal_prepare_run_policy(&a, &opts, &format);
  if (policy_status != 0) {
    return policy_status;
  }

  return internal_main_run(&a, mode, format, &opts, &nums);
}
