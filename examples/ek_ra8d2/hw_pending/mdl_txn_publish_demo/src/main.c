/**
 * @file examples/ek_ra8d2/hw_pending/mdl_txn_publish_demo/src/main.c
 * @brief First consumer of mdl_storage_txn: the C6 media storage seam driven
 *        entirely by the portable fw_if_fs transaction port.
 *
 * @details
 * The C6 media coordinator publishes a downloaded object through five function
 * pointers (::ra8_mdl_storage_iface_t). `apps/shared_libs/mdl_storage_vfs`
 * satisfies that by re-implementing staged publication on `ra8_io_vfs`, which
 * is the second copy of a contract `libs/if` already owns (#762).
 * `mdl_storage_txn` fills the same five callbacks from one bound
 * ::fw_fs_transaction_port_t instead, so the coordinator inherits the port's
 * staging and publication rather than a second opinion about them.
 *
 * This app drives exactly the sequence the coordinator drives, with a RAM disk
 * standing in for the card so every leg is deterministic with no media
 * inserted and no C6 attached:
 *
 *   1. Stand up a RAM block device, format it FAT12, mount it, register it
 *      with `ra8_io_vfs` as `ram`, and bind it into `fw_fs_t`.
 *   2. Check the bound port really advertises transaction support and that
 *      its workspace fits this app's .bss ceiling.
 *   3. Publish through the seam only: `begin`, two `write` fragments,
 *      `validate` (which hands this app an open read-only handle on the
 *      stage), `commit`. Then read the destination back through the portable
 *      stream API and compare it byte for byte.
 *   4. Abort a second transfer part way and confirm the destination never
 *      appeared and the adapter went back to idle.
 *   5. Prove the policy field is real: a `create_new` transfer to the already
 *      published path is refused, and the published bytes are untouched.
 *
 * The digest argument is the transport's business and this app has no C6, so
 * it passes a fixed non-zero digest through the seam; the adapter carries it
 * to the validator untouched and never claims to have verified it.
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
#include "mdl_storage_txn.h"
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
 * @enum txn_const_t
 * @brief Console, RAM-disk, and workspace knobs (no magic numbers).
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_txn_uart_chan   = 8U,    /**< SCI8 J-Link OB console.                  */
  k_txn_disk_blocks = 512U,  /**< RAM-disk sectors; FAT12 fits comfortably. */
  k_txn_file_work   = 64U,   /**< Backend file-handle workspace bytes.     */
  k_txn_txn_work    = 2048U, /**< Backend transaction workspace bytes.     */
  k_txn_chunk       = 6U,    /**< Transfer fragment size, in bytes.        */
} txn_const_t;

/*
 * The two workspace ceilings come from `fw_if_fs`, not from this app: the
 * `caps` leg refuses to continue unless the bound port's advertised
 * `file_workspace_bytes` and `transaction_workspace_bytes` fit inside them, so
 * a backend that grows its state fails loudly here instead of overflowing a
 * buffer. The figures mirror the ones vfs_port_demo derived for the same
 * RAM/FAT12/VFS stack: a handle pointer for a file, two full-path scratch
 * buffers plus flags for a transaction.
 */

/** @brief Object the transfer publishes, in two coordinator fragments. */
static const uint8_t k_txn_payload[] = {
  0x52U, 0x42U, 0x4BU, 0x43U, 0x01U, 0x00U, 0xA5U, 0x5AU, 0x0FU, 0xF0U, 0x11U, 0x22U,
};

/** @brief Magic the stage validator insists on before publication. */
static const uint8_t k_txn_magic[] = {0x52U, 0x42U, 0x4BU, 0x43U};

/** @brief Digest the transport would have verified; passed through untouched. */
static const uint8_t k_txn_digest[k_ra8_mdl_sha256_bytes] = {
  0x11U, 0x22U, 0x33U, 0x44U, 0x55U, 0x66U, 0x77U, 0x88U,
};

/** @brief Portable paths; the mount name never appears in them. */
static const char* const k_txn_dir       = "/books";
static const char* const k_txn_published = "/books/a.rbk";
static const char* const k_txn_aborted   = "/books/b.rbk";

/** @brief Mount name registered with ra8_io_vfs. */
static const char* const k_txn_mount_name = "ram";

static uint8_t s_disk[(size_t)k_txn_disk_blocks * (size_t)k_ra8_io_block_size_bytes];

/* Backend workspaces live in .bss: the transaction workspace alone is 2 KB and
 * this app's stack is 4 KB. */
static uint8_t s_file_work[k_txn_file_work];
static uint8_t s_txn_work[k_txn_txn_work];

static ra8_io_blockdev_t           s_blockdev;
static ra8_io_blockdev_ram_state_t s_ram_state;
static ra8_fs_backend_t            s_backend;
static ra8_fs_mount_t*             s_mount;

static fw_fs_t               s_fs;
static fw_fs_ra8_vfs_state_t s_adapter;

static mdl_storage_txn_t       s_storage;
static ra8_mdl_storage_iface_t s_iface;

static ra8_io_stream_t            s_uart;
static ra8_io_stream_uart_state_t s_uart_state;

/**
 * @brief Write a NUL-terminated string to the console stream.
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
                                           (uint32_t)k_txn_disk_blocks,
                                           false);
  if (err != k_ra8_ok) {
    return err;
  }

  err = ra8_io_blockdev_as_fs_backend(&s_blockdev, &s_backend);
  if (err != k_ra8_ok) {
    return err;
  }

  const ra8_fs_format_opts_t format
      = {.type = k_ra8_fs_type_fat12, .label = "MDLTXN", .sectors_per_cluster = 0U};

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

  err = ra8_io_vfs_mount(k_txn_mount_name, s_mount);
  if (err != k_ra8_ok) {
    return err;
  }

  const fw_fs_ra8_vfs_cfg_t cfg
      = {.mount_name = k_txn_mount_name, .mount = s_mount, .removable_media = false};

  err = fw_fs_ra8_vfs_init(&s_fs, &s_adapter, &cfg);
  if (err != k_ra8_ok) {
    return err;
  }

  return fw_fs_mkdir(&s_fs.names, k_txn_dir);
}

/**
 * @brief Check the bound port advertises transactions and fits the workspaces.
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok              Capability bit and workspace bounds agree.
 * @retval k_ra8_err_invalid_arg The port cannot stage, or asks for more room.
 * @since 0.1.0
 */
static ra8_err_t internal_check_caps(void)
{
  fw_fs_caps_t caps = {0};

  const ra8_err_t err = fw_fs_get_caps(&s_fs, &caps);
  if (err != k_ra8_ok) {
    return err;
  }

  const bool staged = (caps.flags & (uint32_t)k_fw_fs_cap_transactions) != 0U;
  const bool fits   = (caps.file_workspace_bytes <= (uint32_t)k_txn_file_work)
                    && (caps.transaction_workspace_bytes <= (uint32_t)k_txn_txn_work);

  return (staged && fits) ? k_ra8_ok : k_ra8_err_invalid_arg;
}

/**
 * @brief Bind the coordinator storage seam onto the port's transactions.
 * @param[in] policy Publication policy for the transfers that follow.
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok ::s_iface carries five bound callbacks.
 * @since 0.1.0
 */
static ra8_err_t internal_bind_seam(fw_fs_transaction_policy_t policy)
{
  const mdl_storage_txn_cfg_t cfg = {
    .port           = &s_fs.transactions,
    .policy         = policy,
    .workspace      = s_txn_work,
    .workspace_size = (uint32_t)sizeof(s_txn_work),
    .validate       = nullptr,
    .validate_ctx   = nullptr,
  };

  s_iface = (ra8_mdl_storage_iface_t){};
  return mdl_storage_txn_init(&s_storage, &cfg, &s_iface);
}

/**
 * @brief Inspect the open stage the way a `.rabook` reader would.
 * @param[in,out] ctx Unused validation context.
 * @param[in,out] staged Read-only handle on the complete stage.
 * @param[in] total_bytes Byte count the transfer reported.
 * @param[in] sha256 Digest the transfer reported.
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok              Magic, length, and digest all agree.
 * @retval k_ra8_err_invalid_arg The artifact is not what was transferred.
 * @note Runs before publication; it must not rename or delete the stage.
 * @since 0.1.0
 */
static ra8_err_t internal_validate_artifact(void*         ctx,
                                            fw_fs_file_t* staged,
                                            uint64_t      total_bytes,
                                            const uint8_t sha256[k_ra8_mdl_sha256_bytes])
{
  (void)ctx;

  uint8_t  actual[sizeof(k_txn_magic)] = {0};
  uint32_t got                         = 0U;

  const ra8_err_t err = fw_fs_read(staged, actual, (uint32_t)sizeof(actual), &got);
  if (err != k_ra8_ok) {
    return err;
  }

  const bool magic = (got == (uint32_t)sizeof(k_txn_magic))
                     && (memcmp(actual, k_txn_magic, sizeof(k_txn_magic)) == 0);
  const bool length = total_bytes == (uint64_t)sizeof(k_txn_payload);
  const bool digest = memcmp(sha256, k_txn_digest, sizeof(k_txn_digest)) == 0;

  return (magic && length && digest) ? k_ra8_ok : k_ra8_err_invalid_arg;
}

/**
 * @brief Drive one whole transfer through the five coordinator callbacks.
 * @param[in] destination Portable destination path.
 * @param[in] commit Whether to publish or abort after validation.
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok The requested outcome was reached through the seam only.
 * @since 0.1.0
 */
static ra8_err_t internal_run_transfer(const char* destination, bool commit)
{
  ra8_err_t err = s_iface.begin(s_iface.ctx, destination);
  if (err != k_ra8_ok) {
    return err;
  }

  uint32_t offset = 0U;
  while (offset < (uint32_t)sizeof(k_txn_payload)) {
    uint32_t remaining = (uint32_t)sizeof(k_txn_payload) - offset;
    if (remaining > (uint32_t)k_txn_chunk) {
      remaining = (uint32_t)k_txn_chunk;
    }

    uint16_t written = 0U;
    err = s_iface.write(s_iface.ctx, &k_txn_payload[offset], (uint16_t)remaining, &written);
    if ((err != k_ra8_ok) || (written != (uint16_t)remaining)) {
      (void)s_iface.abort(s_iface.ctx);
      return (err != k_ra8_ok) ? err : k_ra8_err_invalid_arg;
    }
    offset += remaining;
  }

  if (!commit) {
    return s_iface.abort(s_iface.ctx);
  }

  err = s_iface.validate(s_iface.ctx, (uint64_t)sizeof(k_txn_payload), k_txn_digest);
  if (err != k_ra8_ok) {
    (void)s_iface.abort(s_iface.ctx);
    return err;
  }

  err = s_iface.commit(s_iface.ctx);
  if (err != k_ra8_ok) {
    (void)s_iface.abort(s_iface.ctx);
    return err;
  }
  return k_ra8_ok;
}

/**
 * @brief Read the published destination back through the portable stream API.
 * @param[in] path Portable destination path.
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok              The bytes match what the transfer wrote.
 * @retval k_ra8_err_invalid_arg A count or a byte disagreed.
 * @since 0.1.0
 */
static ra8_err_t internal_read_back(const char* path)
{
  fw_fs_file_t file                          = {0};
  uint8_t      actual[sizeof(k_txn_payload)] = {0};
  uint32_t     got                           = 0U;

  ra8_err_t err = fw_fs_open(&s_fs.streams,
                             path,
                             k_fw_fs_open_read,
                             &file,
                             s_file_work,
                             (uint32_t)sizeof(s_file_work));
  if (err != k_ra8_ok) {
    return err;
  }

  err                    = fw_fs_read(&file, actual, (uint32_t)sizeof(actual), &got);
  const ra8_err_t closed = fw_fs_close(&file);
  if (err != k_ra8_ok) {
    return err;
  }
  if (closed != k_ra8_ok) {
    return closed;
  }

  const bool exact = (got == (uint32_t)sizeof(k_txn_payload))
                     && (memcmp(actual, k_txn_payload, sizeof(k_txn_payload)) == 0);

  return exact ? k_ra8_ok : k_ra8_err_invalid_arg;
}

/**
 * @brief Publish one object through the seam and verify it landed.
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok The destination holds exactly the transferred bytes.
 * @since 0.1.0
 */
static ra8_err_t internal_publish(void)
{
  ra8_err_t err = internal_bind_seam(k_fw_fs_txn_create_new);
  if (err != k_ra8_ok) {
    return err;
  }

  s_storage.validate     = internal_validate_artifact;
  s_storage.validate_ctx = nullptr;

  err = internal_run_transfer(k_txn_published, true);
  if (err != k_ra8_ok) {
    return err;
  }

  mdl_storage_txn_state_t state = k_mdl_storage_txn_idle;
  err                           = mdl_storage_txn_state(&s_storage, &state);
  if (err != k_ra8_ok) {
    return err;
  }
  if (state != k_mdl_storage_txn_committed) {
    return k_ra8_err_invalid_state;
  }

  return internal_read_back(k_txn_published);
}

/**
 * @brief Abort a transfer part way and confirm nothing was published.
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok              The destination never appeared.
 * @retval k_ra8_err_invalid_arg A stage or a destination survived the abort.
 * @since 0.1.0
 */
static ra8_err_t internal_abort_leaves_nothing(void)
{
  ra8_err_t err = internal_run_transfer(k_txn_aborted, false);
  if (err != k_ra8_ok) {
    return err;
  }

  mdl_storage_txn_state_t state = k_mdl_storage_txn_committed;
  err                           = mdl_storage_txn_state(&s_storage, &state);
  if (err != k_ra8_ok) {
    return err;
  }
  if (state != k_mdl_storage_txn_idle) {
    return k_ra8_err_invalid_state;
  }

  fw_fs_stat_t stat = {0};
  err               = fw_fs_stat(&s_fs.names, k_txn_aborted, &stat);
  if (err != k_ra8_ok) {
    return err;
  }

  return stat.exists ? k_ra8_err_invalid_arg : k_ra8_ok;
}

/**
 * @brief Confirm `create_new` refuses a destination that already exists.
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok              The second transfer was refused, bytes intact.
 * @retval k_ra8_err_invalid_arg It was accepted, or the object changed.
 * @since 0.1.0
 */
static ra8_err_t internal_collision_refused(void)
{
  const ra8_err_t denied = internal_run_transfer(k_txn_published, true);
  if (denied == k_ra8_ok) {
    return k_ra8_err_invalid_arg;
  }

  return internal_read_back(k_txn_published);
}

/**
 * @brief Remove what the demo created and release the volume.
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok Everything unwound cleanly.
 * @post The VFS name is released and the volume is unmounted.
 * @since 0.1.0
 */
static ra8_err_t internal_unwind(void)
{
  ra8_err_t err = fw_fs_unlink(&s_fs.names, k_txn_published);
  if (err != k_ra8_ok) {
    return err;
  }

  err = ra8_io_vfs_unmount(k_txn_mount_name);
  if (err != k_ra8_ok) {
    return err;
  }

  err     = ra8_fs_unmount(s_mount);
  s_mount = nullptr;
  return err;
}

/**
 * @brief Report one leg's verdict on the console.
 * @param[in]     label Leg name, printed verbatim.
 * @param[in]     err   Leg result.
 * @param[in,out] pass  Cleared when @p err is not ::k_ra8_ok.
 * @return void
 * @post One verdict line is queued on the console.
 * @since 0.1.0
 */
static void internal_verdict(const char* label, ra8_err_t err, bool* pass)
{
  internal_print("mdl_txn_publish_demo: ");
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
 * @brief Entry point: bind the volume and drive the storage seam.
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
  (void)ra8_io_stream_uart_init(&s_uart, &s_uart_state, (uint8_t)k_txn_uart_chan);
  (void)ra8_io_log_attach(&s_uart);
  internal_print("mdl_txn_publish_demo: boot\r\n");

  bool pass = true;

  const ra8_err_t bound = internal_bind_volume();
  internal_verdict("bind", bound, &pass);

  if (bound == k_ra8_ok) {
    internal_verdict("caps", internal_check_caps(), &pass);
    internal_verdict("publish", internal_publish(), &pass);
    internal_verdict("abort", internal_abort_leaves_nothing(), &pass);
    internal_verdict("collision", internal_collision_refused(), &pass);
    internal_verdict("unwind", internal_unwind(), &pass);
  }

  internal_print(pass ? "mdl_txn_publish_demo: ALL PASS\r\n"
                      : "mdl_txn_publish_demo: ALL FAIL\r\n");

  (void)ra8_sci_flush((uint8_t)k_txn_uart_chan);
  while (true) {
  }
}
