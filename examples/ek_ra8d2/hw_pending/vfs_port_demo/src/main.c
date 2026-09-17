/**
 * @file examples/ek_ra8d2/hw_pending/vfs_port_demo/src/main.c
 * @brief First consumer of if_ra8_vfs: the portable fw_if_fs facade on a volume.
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * `libs/if_ra8_vfs` is the composition-root adapter that binds one live named
 * VFS mount into the backend-neutral `fw_if_fs` facade, so portable code sees
 * `/books/a.bin` and never the mount name or the medium underneath. Only the
 * unit tests referenced it; nothing in `apps/` or `examples/` bound a volume
 * through it. This app does, end to end:
 *
 *   1. Stand up a RAM block device, format it FAT12, mount it, register it
 *      with `ra8_io_vfs` under the name `ram`, and bind it into `fw_fs_t`.
 *   2. Check the capabilities the adapter advertises. The header is explicit
 *      that this stack does not offer durable sync or atomic replacement, so
 *      the app asserts those bits are ABSENT and that namespace, stream, and
 *      transaction support are present.
 *   3. Round-trip a file through the portable API only: mkdir, write,
 *      read back, stat the size, and walk the directory cursor.
 *   4. Publish through a staged transaction with `create_new`, validating the
 *      stage before commit, then confirm a second `create_new` to the same
 *      destination is refused and leaves the published file untouched.
 *   5. Prove the lexical path guard rejects traversal (`/../escape`) before
 *      any backend sees the argument.
 *   6. Unwind cleanly: unlink, unmount the VFS name, unmount the volume.
 *
 * The medium is a RAM disk this file owns, so every leg is deterministic with
 * no card inserted. A board is needed only to confirm the console path.
 *
 * Observable over the SCI8 / J-Link OB VCOM console. A good run prints one
 * verdict per leg and a final `ALL PASS`.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>
#include <string.h>

#include "fw_if_fs.h"
#include "fw_if_fs_ra8_vfs.h"
#include "ra8_boot_entry.h"
#include "ra8_err.h"
#include "ra8_fs.h"
#include "ra8_io_blockdev.h"
#include "ra8_io_blockdev_ram.h"
#include "ra8_io_log.h"
#include "ra8_io_stream.h"
#include "ra8_io_stream_uart.h"
#include "ra8_io_vfs.h"
#include "ra8_log.h"
#include "ra8_sci.h"

/**
 * @enum vfs_const_t
 * @brief Console, RAM-disk, and workspace knobs (no magic numbers).
 *
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_vfs_uart_chan   = 8U,    /**< SCI8 J-Link OB console.                  */
  k_vfs_disk_blocks = 512U,  /**< RAM-disk sectors; FAT12 fits comfortably. */
  k_vfs_file_work   = 64U,   /**< Backend file-handle workspace bytes.     */
  k_vfs_txn_work    = 2048U, /**< Backend transaction workspace bytes.     */
  k_vfs_dir_work    = 512U,  /**< Backend directory-cursor workspace.      */
  k_vfs_list_cap    = 8U,    /**< Bounded directory-walk entry ceiling.    */
} vfs_const_t;

/*
 * Where the three workspace ceilings come from, since only the first two follow
 * from compile-time types:
 *
 *   k_vfs_file_work  `vfs_file_state_t` is one `ra8_fs_file_t*`, so the adapter
 *                    advertises 4 bytes on this target; 64 is slack.
 *   k_vfs_txn_work   `vfs_transaction_state_t` is two full-path scratch buffers
 *                    (`k_fw_fs_path_cap` 512 each) plus a handle, a policy byte
 *                    and two flags, about 1 KB; 2048 is slack.
 *   k_vfs_dir_work   NOT a compile-time number. `internal_capabilities()` adds
 *                    `sizeof(vfs_directory_state_t)`, alignment slack, and the
 *                    native cursor bytes the mounted format reports through
 *                    `ra8_io_vfs_dir_requirements()`, i.e. FAT12's own cursor
 *                    state on this volume, which no build-time constant fixes.
 *                    512 is this app's ceiling, not a measured figure: if the
 *                    volume asks for more, the `caps` leg FAILS on the bound
 *                    check below (before any cursor is opened) rather than
 *                    overflowing the buffer. Change the format or the backend
 *                    and this is the number to re-derive.
 */

/** @brief Payload written through the portable stream API. */
static const uint8_t k_vfs_payload[] = {0x52U, 0x41U, 0x38U, 0x44U, 0x32U, 0x21U};

/** @brief Payload published through the staged transaction. */
static const uint8_t k_vfs_staged[] = {0xDEU, 0xADU, 0xBEU, 0xEFU};

/** @brief Portable paths; the mount name never appears in them. */
static const char* const k_vfs_dir       = "/books";
static const char* const k_vfs_file      = "/books/a.bin";
static const char* const k_vfs_leaf      = "a.bin";
static const char* const k_vfs_published = "/books/staged.bin";
static const char* const k_vfs_escape    = "/../escape";

/** @brief Mount name registered with ra8_io_vfs. */
static const char* const k_vfs_mount_name = "ram";

static uint8_t s_disk[(size_t)k_vfs_disk_blocks * (size_t)k_ra8_io_block_size_bytes];

/*
 * Backend workspaces live in .bss, not on the stack. `ra8_add_app(... STACK_BYTES
 * 4096)` gives this app 4 KB, and the transaction workspace alone is 2 KB on the
 * frame that then calls down through the facade into the FAT backend. The app is
 * single-threaded and the legs run one at a time, so file scope costs nothing
 * here: .bss is already dominated by the 256 KB RAM disk above.
 */
static uint8_t s_file_work[k_vfs_file_work];
static uint8_t s_dir_work[k_vfs_dir_work];
static uint8_t s_txn_work[k_vfs_txn_work];

static ra8_io_blockdev_t           s_blockdev;
static ra8_io_blockdev_ram_state_t s_ram_state;
static ra8_fs_backend_t            s_backend;
static ra8_fs_mount_t*             s_mount;

static fw_fs_t               s_fs;
static fw_fs_ra8_vfs_state_t s_adapter;

static ra8_io_stream_t            s_uart;
static ra8_io_stream_uart_state_t s_uart_state;

/**
 * @brief Write a NUL-terminated string to the console stream.
 *
 * @param[in] text Message to queue on SCI8.
 * @return void
 * @pre The console stream was initialised.
 * @post The text was queued on the console sink.
 * @note Errors are ignored: the console reports, it does not act.
 * @since 0.1.0
 */
static void internal_print(const char* text)
{
  (void)ra8_io_stream_puts(&s_uart, text);
}

/**
 * @brief Stand up RAM disk, FAT12 volume, VFS name, and portable binding.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok The facade in ::s_fs is bound to a live volume.
 * @post On success the volume is mounted and registered as `ram`.
 * @since 0.1.0
 */
static ra8_err_t internal_bind_volume(void)
{
  ra8_err_t err = ra8_io_blockdev_ram_init(&s_blockdev,
                                           &s_ram_state,
                                           s_disk,
                                           (uint32_t)k_vfs_disk_blocks,
                                           false);
  if (err != k_ra8_ok) {
    return err;
  }

  err = ra8_io_blockdev_as_fs_backend(&s_blockdev, &s_backend);
  if (err != k_ra8_ok) {
    return err;
  }

  const ra8_fs_format_opts_t format
      = {.type = k_ra8_fs_type_fat12, .label = "PORT", .sectors_per_cluster = 0U};

  err = ra8_fs_format(&s_backend, &format);
  if (err != k_ra8_ok) {
    return err;
  }

  err = ra8_fs_mount(&s_backend, &s_mount);
  if (err != k_ra8_ok) {
    return err;
  }

  err = ra8_io_vfs_init();
  if (err != k_ra8_ok) {
    return err;
  }

  err = ra8_io_vfs_mount(k_vfs_mount_name, s_mount);
  if (err != k_ra8_ok) {
    return err;
  }

  const fw_fs_ra8_vfs_cfg_t cfg
      = {.mount_name = k_vfs_mount_name, .mount = s_mount, .removable_media = false};

  return fw_fs_ra8_vfs_init(&s_fs, &s_adapter, &cfg);
}

/**
 * @brief Check the adapter advertises what this stack can really honour.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok              Capability bits and workspace sizes agree.
 * @retval k_ra8_err_invalid_arg A bit or a workspace bound disagreed.
 * @note The header states FAT plus this stack offers neither durable sync nor
 *       atomic replacement, so both bits must be clear.
 * @since 0.1.0
 */
static ra8_err_t internal_check_caps(void)
{
  fw_fs_caps_t caps = {0};

  const ra8_err_t err = fw_fs_get_caps(&s_fs, &caps);
  if (err != k_ra8_ok) {
    return err;
  }

  const uint32_t required = (uint32_t)k_fw_fs_cap_namespace | (uint32_t)k_fw_fs_cap_stream
                            | (uint32_t)k_fw_fs_cap_transactions;
  const uint32_t refused = (uint32_t)k_fw_fs_cap_durable_file_sync
                           | (uint32_t)k_fw_fs_cap_durable_directory_sync
                           | (uint32_t)k_fw_fs_cap_atomic_replace
                           | (uint32_t)k_fw_fs_cap_symlinks;

  const bool honest = ((caps.flags & required) == required) && ((caps.flags & refused) == 0U);
  const bool fits   = (caps.file_workspace_bytes <= (uint32_t)k_vfs_file_work)
                    && (caps.transaction_workspace_bytes <= (uint32_t)k_vfs_txn_work)
                    && (caps.directory_workspace_bytes <= (uint32_t)k_vfs_dir_work);

  return (honest && fits) ? k_ra8_ok : k_ra8_err_invalid_arg;
}

/**
 * @brief Round-trip one file through the portable namespace and stream API.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok              Bytes written came back byte for byte.
 * @retval k_ra8_err_invalid_arg A count, a size, or the content disagreed.
 * @since 0.1.0
 */
static ra8_err_t internal_round_trip(void)
{
  uint8_t* const work = s_file_work;
  fw_fs_file_t   file = {0};

  (void)memset(work, 0, sizeof(s_file_work));

  ra8_err_t err = fw_fs_mkdir(&s_fs.names, k_vfs_dir);
  if (err != k_ra8_ok) {
    return err;
  }

  err = fw_fs_open(&s_fs.streams,
                   k_vfs_file,
                   k_fw_fs_open_write_truncate,
                   &file,
                   work,
                   (uint32_t)sizeof(s_file_work));
  if (err != k_ra8_ok) {
    return err;
  }

  uint32_t written = 0U;
  err              = fw_fs_write(&file, k_vfs_payload, (uint32_t)sizeof(k_vfs_payload), &written);
  const ra8_err_t closed = fw_fs_close(&file);
  if (err != k_ra8_ok) {
    return err;
  }
  if (closed != k_ra8_ok) {
    return closed;
  }
  if (written != (uint32_t)sizeof(k_vfs_payload)) {
    return k_ra8_err_invalid_arg;
  }

  err = fw_fs_open(&s_fs.streams,
                   k_vfs_file,
                   k_fw_fs_open_read,
                   &file,
                   work,
                   (uint32_t)sizeof(s_file_work));
  if (err != k_ra8_ok) {
    return err;
  }

  uint8_t  actual[sizeof(k_vfs_payload)] = {0};
  uint32_t got                           = 0U;
  err                      = fw_fs_read(&file, actual, (uint32_t)sizeof(actual), &got);
  const ra8_err_t reclosed = fw_fs_close(&file);
  if (err != k_ra8_ok) {
    return err;
  }
  if (reclosed != k_ra8_ok) {
    return reclosed;
  }

  if ((got != (uint32_t)sizeof(k_vfs_payload))
      || (memcmp(actual, k_vfs_payload, sizeof(k_vfs_payload)) != 0)) {
    return k_ra8_err_invalid_arg;
  }

  fw_fs_stat_t stat = {0};
  err               = fw_fs_stat(&s_fs.names, k_vfs_file, &stat);
  if (err != k_ra8_ok) {
    return err;
  }

  const bool sized = stat.exists && (stat.type == k_fw_fs_node_file)
                     && (stat.size_bytes == (uint64_t)sizeof(k_vfs_payload));

  return sized ? k_ra8_ok : k_ra8_err_invalid_arg;
}

/**
 * @brief Walk the directory cursor and find the file just written.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok              The leaf was delivered exactly once.
 * @retval k_ra8_err_invalid_arg The cursor missed it or repeated it.
 * @since 0.1.0
 */
static ra8_err_t internal_walk_dir(void)
{
  uint8_t* const work   = s_dir_work;
  fw_fs_dir_t    cursor = {0};

  (void)memset(work, 0, sizeof(s_dir_work));

  ra8_err_t err
      = fw_fs_dir_open(&s_fs.names, k_vfs_dir, &cursor, work, (uint32_t)sizeof(s_dir_work));
  if (err != k_ra8_ok) {
    return err;
  }

  uint32_t seen    = 0U;
  uint32_t matched = 0U;

  for (; seen < (uint32_t)k_vfs_list_cap; ++seen) {
    fw_fs_dirent_value_t entry   = {0};
    bool                 present = false;

    err = fw_fs_dir_next(&cursor, &entry, &present);
    if ((err != k_ra8_ok) || !present) {
      break;
    }

    if (strcmp(entry.name, k_vfs_leaf) == 0) {
      matched += 1U;
    }
  }

  const ra8_err_t closed = fw_fs_dir_close(&cursor);
  if (err != k_ra8_ok) {
    return err;
  }
  if (closed != k_ra8_ok) {
    return closed;
  }

  return (matched == 1U) ? k_ra8_ok : k_ra8_err_invalid_arg;
}

/**
 * @brief Validator hook: confirm the stage holds exactly the staged bytes.
 *
 * @param[in]     ctx    Unused caller cookie.
 * @param[in]     staged Read-only handle on the staging artifact.
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok              The stage matched byte for byte.
 * @retval k_ra8_err_invalid_arg The stage held something else.
 * @since 0.1.0
 */
static ra8_err_t internal_validate_stage(void* ctx, fw_fs_file_t* staged)
{
  (void)ctx;

  uint8_t  actual[sizeof(k_vfs_staged)] = {0};
  uint32_t got                          = 0U;

  const ra8_err_t err = fw_fs_read(staged, actual, (uint32_t)sizeof(actual), &got);
  if (err != k_ra8_ok) {
    return err;
  }

  const bool exact = (got == (uint32_t)sizeof(k_vfs_staged))
                     && (memcmp(actual, k_vfs_staged, sizeof(k_vfs_staged)) == 0);

  return exact ? k_ra8_ok : k_ra8_err_invalid_arg;
}

/**
 * @brief Publish through a staged transaction, then prove the collision guard.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok              Published once; the second attempt was refused.
 * @retval k_ra8_err_invalid_arg Publication or the refusal misbehaved.
 * @since 0.1.0
 */
static ra8_err_t internal_publish_staged(void)
{
  uint8_t* const      work = s_txn_work;
  fw_fs_transaction_t txn  = {0};

  (void)memset(work, 0, sizeof(s_txn_work));

  ra8_err_t err = fw_fs_transaction_begin(&s_fs.transactions,
                                          k_vfs_published,
                                          k_fw_fs_txn_create_new,
                                          &txn,
                                          work,
                                          (uint32_t)sizeof(s_txn_work));
  if (err != k_ra8_ok) {
    return err;
  }

  uint32_t written = 0U;
  err = fw_fs_transaction_write(&txn, k_vfs_staged, (uint32_t)sizeof(k_vfs_staged), &written);
  if ((err != k_ra8_ok) || (written != (uint32_t)sizeof(k_vfs_staged))) {
    (void)fw_fs_transaction_abort(&txn);
    return (err != k_ra8_ok) ? err : k_ra8_err_invalid_arg;
  }

  err = fw_fs_transaction_validate(&txn, internal_validate_stage, nullptr);
  if (err != k_ra8_ok) {
    (void)fw_fs_transaction_abort(&txn);
    return err;
  }

  bool published = false;
  err            = fw_fs_transaction_commit(&txn, &published);
  if (err != k_ra8_ok) {
    (void)fw_fs_transaction_abort(&txn);
    return err;
  }
  if (!published) {
    return k_ra8_err_invalid_arg;
  }

  /* create_new must refuse a destination that now exists. */
  fw_fs_transaction_t again = {0};
  const ra8_err_t     denied
      = fw_fs_transaction_begin(&s_fs.transactions,
                                k_vfs_published,
                                k_fw_fs_txn_create_new,
                                &again,
                                work,
                                (uint32_t)sizeof(s_txn_work));
  if (denied == k_ra8_ok) {
    (void)fw_fs_transaction_abort(&again);
    return k_ra8_err_invalid_arg;
  }

  fw_fs_stat_t stat = {0};
  err               = fw_fs_stat(&s_fs.names, k_vfs_published, &stat);
  if (err != k_ra8_ok) {
    return err;
  }

  const bool intact = stat.exists && (stat.size_bytes == (uint64_t)sizeof(k_vfs_staged));

  return intact ? k_ra8_ok : k_ra8_err_invalid_arg;
}

/**
 * @brief Confirm the lexical guard refuses traversal before the backend.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok              The escape path was refused.
 * @retval k_ra8_err_invalid_arg It was accepted or refused for a stat miss.
 * @since 0.1.0
 */
static ra8_err_t internal_check_guard(void)
{
  fw_fs_stat_t stat = {0};

  const ra8_err_t err = fw_fs_stat(&s_fs.names, k_vfs_escape, &stat);

  return (err == k_ra8_err_access_denied) ? k_ra8_ok : k_ra8_err_invalid_arg;
}

/**
 * @brief Remove what the demo created and release the volume.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok Everything unwound cleanly.
 * @post The VFS name is released and the volume is unmounted.
 * @since 0.1.0
 */
static ra8_err_t internal_unwind(void)
{
  ra8_err_t err = fw_fs_unlink(&s_fs.names, k_vfs_file);
  if (err != k_ra8_ok) {
    return err;
  }

  err = fw_fs_unlink(&s_fs.names, k_vfs_published);
  if (err != k_ra8_ok) {
    return err;
  }

  err = ra8_io_vfs_unmount(k_vfs_mount_name);
  if (err != k_ra8_ok) {
    return err;
  }

  err     = ra8_fs_unmount(s_mount);
  s_mount = nullptr;
  return err;
}

/**
 * @brief Report one leg's verdict on the console.
 *
 * @param[in]     label Leg name, printed verbatim.
 * @param[in]     err   Leg result.
 * @param[in,out] pass  Cleared when @p err is not ::k_ra8_ok.
 * @return void
 * @post One verdict line is queued on the console.
 * @since 0.1.0
 */
static void internal_verdict(const char* label, ra8_err_t err, bool* pass)
{
  internal_print("vfs_port_demo: ");
  internal_print(label);

  if (err == k_ra8_ok) {
    internal_print(" PASS\r\n");
    return;
  }

  internal_print(" FAIL\r\n");
  if (pass != nullptr) {
    *pass = false;
  }
}

/**
 * @brief Entry point: bind the volume and walk the portable API.
 *
 * @return void
 * @pre SystemInit configured VTOR / FPU / priority grouping.
 * @post A verdict per leg and a final summary are queued on SCI8.
 * @post Control parks in an infinite loop; the function never returns.
 * @note Single-threaded; the RAM disk lives in .bss, not on the stack.
 * @since 0.1.0
 */
void main(void)
{
  ra8_log_init();
  (void)ra8_io_stream_uart_init(&s_uart, &s_uart_state, (uint8_t)k_vfs_uart_chan);
  (void)ra8_io_log_attach(&s_uart);
  internal_print("vfs_port_demo: boot\r\n");

  bool pass = true;

  const ra8_err_t bound = internal_bind_volume();
  internal_verdict("bind", bound, &pass);

  if (bound == k_ra8_ok) {
    internal_verdict("caps", internal_check_caps(), &pass);
    internal_verdict("round-trip", internal_round_trip(), &pass);
    internal_verdict("listing", internal_walk_dir(), &pass);
    internal_verdict("transaction", internal_publish_staged(), &pass);
    internal_verdict("path-guard", internal_check_guard(), &pass);
    internal_verdict("unwind", internal_unwind(), &pass);
  }

  internal_print(pass ? "vfs_port_demo: ALL PASS\r\n" : "vfs_port_demo: ALL FAIL\r\n");

  (void)ra8_sci_flush((uint8_t)k_vfs_uart_chan);
  while (true) {
  }
}
