/**
 * @file test_lx_fs_vfs_e2e.c
 * @brief End-to-end: RAM NOR -> LevelX -> ra8_fs -> VFS -> the reader surface.
 *
 * @details
 * Drives ONE unbroken chain from raw wear-levelled NOR up to the portable
 * filesystem surface the reader consumes:
 *
 *   lx_nor_fake_ram (RAM NOR fake, the xSPI stand-in)
 *     -> vendored LevelX NOR core (LX_STANDALONE_ENABLE, no ThreadX)
 *     -> port/levelx lx_fs_backend (block-device seam)
 *     -> first-party ra8_fs FAT12 (format + mount)
 *     -> ra8_io_vfs named mount ("flash")
 *     -> fw_if_fs_ra8_vfs adapter (composition root)
 *     -> fw_if_fs_* portable reader calls
 *
 * This is the whole path RA8FW-1008 asks for in one binary. The middle layers
 * each have their own focused tests (test_lx_fs_backend for LevelX -> ra8_fs,
 * test_fw_if_fs for the RAM-block-device -> ra8_fs -> VFS -> reader vectors);
 * neither connects the far ends, so a seam break between LevelX and the VFS
 * adapter would pass both while breaking the product. FileX is retired: the
 * first-party ra8_fs is the FAT layer, exactly as the HIL apps run it.
 *
 * The reader flow models the product: a /books directory, a book file written
 * and read back byte-exact, stat/listdir/cursor/space metadata, one staged
 * transaction published and verified, one aborted, and a clean teardown.
 *
 * @author Brighton Sikarskie
 * @date 2026-10-29
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "fw_if_fs.h"
#include "fw_if_fs_ra8_vfs.h"
#include "lx_api.h"
#include "lx_fs_backend.h"
#include "lx_nor_fake_ram.h"
#include "ra8_err.h"
#include "ra8_fs.h"
#include "ra8_io_vfs.h"
#include "unity_minimal.h"

/** @brief Fixed fixture constants (media geometry, payload sizes, chunking). */
typedef enum : uint32_t {
  k_e2e_sector_bytes    = 512U,   /**< Backend block size LevelX serves.     */
  k_e2e_book_bytes      = 8192U,  /**< Primary reader payload.               */
  k_e2e_stage_bytes     = 4096U,  /**< Staged-transaction payload.           */
  k_e2e_chunk           = 512U,   /**< Read/write chunk.                     */
  k_e2e_ws_bytes        = 16384U, /**< Generous per-consumer workspace.      */
  k_e2e_list_cap        = 8U,     /**< Captured directory names.             */
  k_e2e_name_cap        = 64U,    /**< Captured-name byte capacity.          */
} e2e_limits_t;

/** @brief Maximally aligned caller-owned consumer workspace. */
typedef union {
  max_align_t alignment;              /**< Force maximum C alignment. */
  uint8_t     bytes[k_e2e_ws_bytes];  /**< Opaque workspace storage.  */
} e2e_workspace_t;

/** @brief Expected staged bytes for the transaction validator. */
typedef struct {
  const uint8_t* bytes;   /**< Expected stage bytes.        */
  uint32_t       length;  /**< Expected stage byte count.   */
} e2e_expect_t;

/** @brief listdir capture. */
typedef struct {
  char     names[k_e2e_list_cap][k_e2e_name_cap]; /**< Captured leaves. */
  uint32_t count;                                  /**< Captured count.  */
} e2e_listing_t;

/** @brief LevelX control block under test. */
static LX_NOR_FLASH s_nor_flash;
/** @brief Bound block-device backend over ::s_nor_flash. */
static ra8_fs_backend_t s_backend;
/** @brief Live FAT mount over ::s_backend. */
static ra8_fs_mount_t* s_mount;
/** @brief Primary reader payload. */
static uint8_t s_book[k_e2e_book_bytes];
/** @brief Staged-transaction payload. */
static uint8_t s_stage[k_e2e_stage_bytes];
/** @brief Per-consumer workspaces (file, directory cursor, transaction). */
static e2e_workspace_t s_file_ws;
static e2e_workspace_t s_dir_ws;
static e2e_workspace_t s_txn_ws;

/**
 * @brief Fill both payloads with distinct deterministic byte patterns.
 * @details Offsets stay position-sensitive so a short write or a lost sector
 *          cannot pass the read-back comparison by luck.
 */
static void internal_seed_payloads(void)
{
  for (uint32_t i = 0U; i < k_e2e_book_bytes; ++i) {
    s_book[i] = (uint8_t)((i * 31U) + 7U);
  }
  for (uint32_t i = 0U; i < k_e2e_stage_bytes; ++i) {
    s_stage[i] = (uint8_t)((i * 17U) + 91U);
  }
}

/**
 * @brief Bring the whole stack up from wiped NOR to a bound portable facade.
 * @details Wipe the fake NOR (it persists across close/open), lay a fresh
 *          LevelX partition, bind the block-device backend, format and mount
 *          FAT12 on it, register the named VFS mount, and bind the reader
 *          composition root. Caller tears down with ::internal_tear_down.
 * @param[out] out_fs Complete portable facade to bind.
 * @param[out] out_state Adapter state retained by @p out_fs.
 * @pre ::s_file_ws / ::s_dir_ws / ::s_txn_ws are free.
 * @post On ::k_ra8_ok every layer above NOR is live and the facade is bound.
 */
static void internal_bring_up(fw_fs_t* out_fs, fw_fs_ra8_vfs_state_t* out_state)
{
  internal_seed_payloads();
  TEST_ASSERT_EQ((UINT)LX_SUCCESS,
                 lx_nor_flash_format(&s_nor_flash, (CHAR*)"fake_nor", lx_nor_fake_ram_init, LX_NULL));
  TEST_ASSERT_EQ((UINT)LX_SUCCESS,
                 lx_nor_flash_open(&s_nor_flash, (CHAR*)"fake_nor", lx_nor_fake_ram_init));
  TEST_ASSERT_EQ(k_ra8_ok, lx_fs_backend_bind(&s_nor_flash, &s_backend));
  const ra8_fs_format_opts_t format = {.type                = k_ra8_fs_type_fat12,
                                       .label               = "E2E",
                                       .sectors_per_cluster = 0U};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_format(&s_backend, &format));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &s_mount));
  TEST_ASSERT_NOT_NULL(s_mount);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_vfs_init());
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_vfs_mount("flash", s_mount));
  *out_fs                     = (fw_fs_t){};
  *out_state                  = (fw_fs_ra8_vfs_state_t){};
  const fw_fs_ra8_vfs_cfg_t cfg = {.mount_name        = "flash",
                                   .mount             = s_mount,
                                   .removable_media   = false};
  TEST_ASSERT_EQ(k_ra8_ok, fw_fs_ra8_vfs_init(out_fs, out_state, &cfg));
  TEST_ASSERT(out_fs->caps.file_workspace_bytes <= sizeof(s_file_ws.bytes));
  TEST_ASSERT(out_fs->caps.directory_workspace_bytes <= sizeof(s_dir_ws.bytes));
  TEST_ASSERT(out_fs->caps.transaction_workspace_bytes <= sizeof(s_txn_ws.bytes));
}

/**
 * @brief Tear the stack down in reverse order.
 * @details Unregister the VFS mount, unmount FAT, close LevelX. The fake NOR
 *          backing persists; the next bring-up reformats it.
 * @pre ::internal_bring_up succeeded.
 * @post Every layer above the fake NOR is closed.
 */
static void internal_tear_down(void)
{
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_vfs_unmount("flash"));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(s_mount));
  s_mount = nullptr;
  TEST_ASSERT_EQ((UINT)LX_SUCCESS, lx_nor_flash_close(&s_nor_flash));
}

/**
 * @brief Write ::s_book to /books/demo.cbz through the reader stream surface.
 * @details One helper shared by several sections so each section stays about
 *          its own assertion, not about bring-up plumbing.
 * @pre The facade is bound and /books exists.
 * @post /books/demo.cbz holds exactly ::s_book.
 */
static void internal_write_book(fw_fs_t* fs)
{
  fw_fs_file_t file = {};
  TEST_ASSERT_EQ(k_ra8_ok,
                 fw_fs_open(&fs->streams, "/books/demo.cbz", k_fw_fs_open_create_new, &file,
                            s_file_ws.bytes, sizeof(s_file_ws.bytes)));
  for (uint32_t off = 0U; off < k_e2e_book_bytes; off += k_e2e_chunk) {
    uint32_t written = 0U;
    TEST_ASSERT_EQ(k_ra8_ok,
                   fw_fs_write(&file, &s_book[off], k_e2e_chunk, &written));
    TEST_ASSERT_EQ(k_e2e_chunk, written);
  }
  uint64_t size = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, fw_fs_file_size(&file, &size));
  TEST_ASSERT_EQ((uint64_t)k_e2e_book_bytes, size);
  TEST_ASSERT_EQ(k_ra8_ok, fw_fs_close(&file));
}

/**
 * @brief Validator for the staged transaction: read the stage and compare.
 * @param[in] ctx ::e2e_expect_t carrying the expected bytes and length.
 * @param[in] staged Read-only view of the staged artifact.
 * @return ::k_ra8_ok only when the stage is byte-exact.
 */
static ra8_err_t internal_validate_stage(void* ctx, fw_fs_file_t* staged)
{
  const e2e_expect_t* want = (const e2e_expect_t*)ctx;
  uint8_t             buf[k_e2e_chunk];
  uint32_t            off = 0U;
  for (;;) {
    uint32_t got = 0U;
    if (fw_fs_read(staged, buf, sizeof(buf), &got) != k_ra8_ok) {
      return k_ra8_err_invalid_arg;
    }
    if (got == 0U) {
      break;
    }
    if ((off + got) > want->length) {
      return k_ra8_err_invalid_arg;
    }
    if (memcmp(buf, &want->bytes[off], got) != 0) {
      return k_ra8_err_invalid_arg;
    }
    off += got;
  }
  return (off == want->length) ? k_ra8_ok : k_ra8_err_invalid_arg;
}

/**
 * @brief Capture listdir leaves into ::e2e_listing_t.
 */
static ra8_err_t internal_capture_entry(void* ctx, const fw_fs_dirent_t* entry, bool* out_continue)
{
  e2e_listing_t* listing = (e2e_listing_t*)ctx;
  if ((listing->count < k_e2e_list_cap) && (entry->name_bytes < k_e2e_name_cap)) {
    (void)memcpy(listing->names[listing->count], entry->name, (size_t)entry->name_bytes);
    listing->names[listing->count][entry->name_bytes] = '\0';
    listing->count += 1U;
  }
  *out_continue = true;
  return k_ra8_ok;
}

/** @brief A captured listing contains the given leaf name. */
static bool internal_listing_has(const e2e_listing_t* listing, const char* name)
{
  for (uint32_t i = 0U; i < listing->count; ++i) {
    if (strcmp(listing->names[i], name) == 0) {
      return true;
    }
  }
  return false;
}

/**
 * @brief The full stack comes up: capacity is sane and FAT12 mounts on NOR.
 *
 * @details Proves the composition LevelX -> lx_fs_backend -> ra8_fs accepts a
 *          real format and mount, with a usable non-zero FAT-usable window at
 *          the fixed 512-byte block size. Without this section a lower-layer
 *          regression would surface as confusing failures deep in the reader
 *          sections instead of at the seam.
 */
static void test_stack_brings_up_fat_on_nor(void)
{
  fw_fs_t               fs    = {};
  fw_fs_ra8_vfs_state_t state = {};
  TEST_BEGIN("lx_fs_vfs_e2e: FAT12 mounts over LevelX NOR");
  internal_bring_up(&fs, &state);
  uint64_t block_count = 0U;
  uint32_t block_size  = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, s_backend.get_capacity(s_backend.ctx, &block_count, &block_size));
  TEST_ASSERT(block_count > 0U);
  TEST_ASSERT_EQ(k_e2e_sector_bytes, block_size);
  fw_fs_stat_t stat = {};
  TEST_ASSERT_EQ(k_ra8_ok, fw_fs_stat(&fs.names, "/books", &stat));
  TEST_ASSERT(!stat.exists);
  internal_tear_down();
  TEST_END("lx_fs_vfs_e2e: FAT12 mounts over LevelX NOR");
}

/**
 * @brief A book written through the reader surface reads back byte-exact.
 *
 * @details The product path: create /books, create the book exclusively,
 *          write it in 512-byte chunks, size-check through the open handle,
 *          close, reopen read-only, and stream the whole file back comparing
 *          every byte. Closes with the exclusive-creation guard: a second
 *          create on the existing leaf must report ::k_ra8_err_exists.
 */
static void test_reader_writes_and_reads_back(void)
{
  fw_fs_t               fs    = {};
  fw_fs_ra8_vfs_state_t state = {};
  TEST_BEGIN("lx_fs_vfs_e2e: reader write/read roundtrip");
  internal_bring_up(&fs, &state);
  TEST_ASSERT_EQ(k_ra8_ok, fw_fs_mkdir(&fs.names, "/books"));
  internal_write_book(&fs);
  fw_fs_file_t file  = {};
  TEST_ASSERT_EQ(k_ra8_ok,
                 fw_fs_open(&fs.streams, "/books/demo.cbz", k_fw_fs_open_read, &file,
                            s_file_ws.bytes, sizeof(s_file_ws.bytes)));
  uint8_t  buf[k_e2e_chunk];
  uint32_t total = 0U;
  for (;;) {
    uint32_t got = 0U;
    TEST_ASSERT_EQ(k_ra8_ok, fw_fs_read(&file, buf, sizeof(buf), &got));
    if (got == 0U) {
      break;
    }
    TEST_ASSERT((total + got) <= k_e2e_book_bytes);
    TEST_ASSERT(memcmp(buf, &s_book[total], got) == 0);
    total += got;
  }
  TEST_ASSERT_EQ(k_e2e_book_bytes, total);
  uint64_t tell = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, fw_fs_tell(&file, &tell));
  TEST_ASSERT_EQ((uint64_t)k_e2e_book_bytes, tell);
  TEST_ASSERT_EQ(k_ra8_ok, fw_fs_close(&file));
  fw_fs_file_t again = {};
  TEST_ASSERT_EQ(k_ra8_err_exists,
                 fw_fs_open(&fs.streams, "/books/demo.cbz", k_fw_fs_open_create_new, &again,
                            s_file_ws.bytes, sizeof(s_file_ws.bytes)));
  internal_tear_down();
  TEST_END("lx_fs_vfs_e2e: reader write/read roundtrip");
}

/**
 * @brief Reader metadata: stat, listdir, directory cursor, and space query.
 *
 * @details Everything a bookshelf view needs before opening anything: the
 *          file stats as an existing regular file of the right size, listdir
 *          on /books reports the book by leaf name, a directory cursor on /
 *          reports /books as a directory, and the volume reports used and free
 *          space consistent with a written file.
 */
static void test_reader_metadata_listing_and_space(void)
{
  fw_fs_t               fs    = {};
  fw_fs_ra8_vfs_state_t state = {};
  TEST_BEGIN("lx_fs_vfs_e2e: stat, listdir, cursor, space");
  internal_bring_up(&fs, &state);
  TEST_ASSERT_EQ(k_ra8_ok, fw_fs_mkdir(&fs.names, "/books"));
  internal_write_book(&fs);
  fw_fs_stat_t stat = {};
  TEST_ASSERT_EQ(k_ra8_ok, fw_fs_stat(&fs.names, "/books/demo.cbz", &stat));
  TEST_ASSERT(stat.exists);
  TEST_ASSERT_EQ(k_fw_fs_node_file, stat.type);
  TEST_ASSERT_EQ((uint64_t)k_e2e_book_bytes, stat.size_bytes);
  e2e_listing_t listing = {.count = 0U};
  uint32_t      count   = 0U;
  bool          done    = false;
  TEST_ASSERT_EQ(k_ra8_ok,
                 fw_fs_listdir(&fs.names, "/books", k_e2e_list_cap, internal_capture_entry,
                               &listing, &count, &done));
  TEST_ASSERT_EQ(1U, count);
  TEST_ASSERT(done);
  TEST_ASSERT(internal_listing_has(&listing, "demo.cbz"));
  fw_fs_dir_t          dir  = {};
  fw_fs_dirent_value_t ent  = {};
  bool                 have = false;
  TEST_ASSERT_EQ(k_ra8_ok,
                 fw_fs_dir_open(&fs.names, "/", &dir, s_dir_ws.bytes, sizeof(s_dir_ws.bytes)));
  bool saw_books = false;
  for (;;) {
    TEST_ASSERT_EQ(k_ra8_ok, fw_fs_dir_next(&dir, &ent, &have));
    if (!have) {
      break;
    }
    if ((strcmp(ent.name, "books") == 0) && (ent.type == k_fw_fs_node_directory)) {
      saw_books = true;
    }
  }
  TEST_ASSERT_EQ(k_ra8_ok, fw_fs_dir_close(&dir));
  TEST_ASSERT(saw_books);
  fw_fs_space_t space = {};
  TEST_ASSERT_EQ(k_ra8_ok, fw_fs_space(&fs.names, &space));
  TEST_ASSERT(space.total_bytes > 0U);
  TEST_ASSERT(space.used_bytes > 0U);
  TEST_ASSERT(space.free_bytes < space.total_bytes);
  internal_tear_down();
  TEST_END("lx_fs_vfs_e2e: stat, listdir, cursor, space");
}

/**
 * @brief A staged transaction publishes a verified book onto the volume.
 *
 * @details The reader's safe-update path end to end over wear-levelled NOR:
 *          begin a create-new stage for /books/demo2.cbz, append the payload,
 *          validate the stage byte-exact through the read-only view, commit,
 *          and read the published file back through the normal reader open.
 *          A second, unvalidated stage is aborted and must leave no artifact.
 */
static void test_reader_transaction_publishes(void)
{
  fw_fs_t               fs    = {};
  fw_fs_ra8_vfs_state_t state = {};
  TEST_BEGIN("lx_fs_vfs_e2e: staged transaction publishes");
  internal_bring_up(&fs, &state);
  TEST_ASSERT_EQ(k_ra8_ok, fw_fs_mkdir(&fs.names, "/books"));
  fw_fs_transaction_t txn       = {};
  const e2e_expect_t  expect    = {.bytes = s_stage, .length = k_e2e_stage_bytes};
  TEST_ASSERT_EQ(k_ra8_ok,
                 fw_fs_transaction_begin(&fs.transactions, "/books/demo2.cbz",
                                         k_fw_fs_txn_create_new, &txn, s_txn_ws.bytes,
                                         sizeof(s_txn_ws.bytes)));
  TEST_ASSERT(txn.active);
  for (uint32_t off = 0U; off < k_e2e_stage_bytes; off += k_e2e_chunk) {
    uint32_t written = 0U;
    TEST_ASSERT_EQ(k_ra8_ok,
                   fw_fs_transaction_write(&txn, &s_stage[off], k_e2e_chunk, &written));
    TEST_ASSERT_EQ(k_e2e_chunk, written);
  }
  TEST_ASSERT_EQ(k_ra8_ok, fw_fs_transaction_validate(&txn, internal_validate_stage, &expect));
  bool published = false;
  TEST_ASSERT_EQ(k_ra8_ok, fw_fs_transaction_commit(&txn, &published));
  TEST_ASSERT(published);
  fw_fs_file_t file = {};
  TEST_ASSERT_EQ(k_ra8_ok,
                 fw_fs_open(&fs.streams, "/books/demo2.cbz", k_fw_fs_open_read, &file,
                            s_file_ws.bytes, sizeof(s_file_ws.bytes)));
  uint8_t  buf[k_e2e_chunk];
  uint32_t total = 0U;
  for (;;) {
    uint32_t got = 0U;
    TEST_ASSERT_EQ(k_ra8_ok, fw_fs_read(&file, buf, sizeof(buf), &got));
    if (got == 0U) {
      break;
    }
    TEST_ASSERT((total + got) <= k_e2e_stage_bytes);
    TEST_ASSERT(memcmp(buf, &s_stage[total], got) == 0);
    total += got;
  }
  TEST_ASSERT_EQ(k_e2e_stage_bytes, total);
  TEST_ASSERT_EQ(k_ra8_ok, fw_fs_close(&file));
  fw_fs_transaction_t dropped = {};
  TEST_ASSERT_EQ(k_ra8_ok,
                 fw_fs_transaction_begin(&fs.transactions, "/books/demo3.cbz",
                                         k_fw_fs_txn_create_new, &dropped, s_txn_ws.bytes,
                                         sizeof(s_txn_ws.bytes)));
  TEST_ASSERT_EQ(k_ra8_ok, fw_fs_transaction_abort(&dropped));
  fw_fs_stat_t stat = {};
  TEST_ASSERT_EQ(k_ra8_ok, fw_fs_stat(&fs.names, "/books/demo3.cbz", &stat));
  TEST_ASSERT(!stat.exists);
  internal_tear_down();
  TEST_END("lx_fs_vfs_e2e: staged transaction publishes");
}

/**
 * @brief Reader cleanup: unlink the books, remove the shelf, unmount cleanly.
 *
 * @details The delete path over the same stack: unlink both book files, stat
 *          confirms the misses, rmdir the empty /books, stat confirms the
 *          directory is gone, and the full teardown succeeds.
 */
static void test_reader_cleanup_unmounts_cleanly(void)
{
  fw_fs_t               fs    = {};
  fw_fs_ra8_vfs_state_t state = {};
  TEST_BEGIN("lx_fs_vfs_e2e: unlink, rmdir, clean unmount");
  internal_bring_up(&fs, &state);
  TEST_ASSERT_EQ(k_ra8_ok, fw_fs_mkdir(&fs.names, "/books"));
  internal_write_book(&fs);
  TEST_ASSERT_EQ(k_ra8_ok, fw_fs_unlink(&fs.names, "/books/demo.cbz"));
  fw_fs_stat_t stat = {};
  TEST_ASSERT_EQ(k_ra8_ok, fw_fs_stat(&fs.names, "/books/demo.cbz", &stat));
  TEST_ASSERT(!stat.exists);
  TEST_ASSERT_EQ(k_ra8_ok, fw_fs_rmdir(&fs.names, "/books"));
  TEST_ASSERT_EQ(k_ra8_ok, fw_fs_stat(&fs.names, "/books", &stat));
  TEST_ASSERT(!stat.exists);
  internal_tear_down();
  TEST_END("lx_fs_vfs_e2e: unlink, rmdir, clean unmount");
}

/**
 * @brief Run every end-to-end section in order.
 * @return 0 when all sections pass.
 */
int main(void)
{
  test_stack_brings_up_fat_on_nor();
  test_reader_writes_and_reads_back();
  test_reader_metadata_listing_and_space();
  test_reader_transaction_publishes();
  test_reader_cleanup_unmounts_cleanly();
  return 0;
}
