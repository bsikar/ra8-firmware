/**
 * @file test_ra8_fs_fat_guards_mcdc.c
 * @brief MC/DC vectors for the public `ra8_fs_*` entry-point argument guards.
 *
 * @details
 * Every public FAT entry point opens with a compound null / state guard. The
 * behavioural suite (`tests/test_ra8_fs_fat.c` and siblings) exercises the
 * happy paths; this file adds the dedicated N+1 independent-influence vector
 * sets the compound-decision ratchet (issue #426) requires for each guard:
 *
 *   - `ra8_fs_mount`   -- the backend/out-handle null guard and the
 *                         read/write/capacity function-pointer guard.
 *   - `ra8_fs_open`    -- the handle/path/out-file null guard.
 *   - `ra8_fs_read`    -- the pointer guard and the offset/length short-read guard.
 *   - `ra8_fs_write`   -- the pointer guard and the in-use/mode state guard.
 *   - `ra8_fs_tell` / `ra8_fs_size` -- their pointer guards.
 *   - `ra8_fs_listdir` / `ra8_fs_unlink` -- their pointer guards.
 *   - `ra8_fs_format`  -- the backend/opts null guard and the write/capacity
 *                         function-pointer guard.
 *
 * Each guard's FALSE (control) arm is observed by a real operation that returns
 * something other than the guard's error; each TRUE arm is driven by nulling
 * exactly one argument (or, for the write state guard, by a slot whose
 * in-use/mode fields are set directly). Citations name the enclosing function
 * as `libs/ra8_fs/src/<file>.c@<function>`.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ra8_err.h"
#include "ra8_fs.h"
#include "unity_minimal.h"

/**
 * @enum ra8_fs_guard_disk_t
 * @brief Synthetic-disk sizing and the data-file payload for the guard vectors.
 */
typedef enum : uint32_t {
  k_disk_block_size   = 512U,       /**< Bytes per logical block.                  */
  k_disk_blocks_fat16 = 8U * 1024U, /**< 4 MiB FAT16 card (matches siblings).      */
  k_data_len          = 64U,        /**< Bytes written into the probe data file.   */
  k_pattern_stride    = 17U,        /**< Payload generator stride, `i*17 + 3`.     */
  k_poison            = 123U,       /**< Sentinel proving an out-param is written. */
} ra8_fs_guard_disk_t;

/**
 * @enum ra8_fs_guard_bpb_t
 * @brief BPB byte offsets and FAT16 geometry for `build_fat16_volume`.
 */
typedef enum : uint32_t {
  k_bpb_off_bytes_per_sec = 11U,  /**< BPB_BytsPerSec.   */
  k_bpb_off_sec_per_clus  = 13U,  /**< BPB_SecPerClus.   */
  k_bpb_off_rsvd          = 14U,  /**< BPB_RsvdSecCnt.   */
  k_bpb_off_num_fats      = 16U,  /**< BPB_NumFATs.      */
  k_bpb_off_root_ents     = 17U,  /**< BPB_RootEntCnt.   */
  k_bpb_off_tot_sec16     = 19U,  /**< BPB_TotSec16.     */
  k_bpb_off_fatsz16       = 22U,  /**< BPB_FATSz16.      */
  k_bpb_off_sig_lo        = 510U, /**< 0x55 signature.   */
  k_bpb_off_sig_hi        = 511U, /**< 0xAA signature.   */
  k_fat16_rsvd            = 1U,   /**< Reserved sectors. */
  k_fat16_num_fats        = 2U,   /**< FAT copies.       */
  k_fat16_fatsz           = 16U,  /**< Sectors per FAT.  */
  k_fat16_root_ents       = 16U,  /**< Root entries.     */
} ra8_fs_guard_bpb_t;

/**
 * @enum ra8_fs_guard_sig_t
 * @brief The 0xAA55 boot-signature bytes.
 */
typedef enum : uint8_t {
  k_bpb_sig_lo = 0x55U, /**< Low signature byte.  */
  k_bpb_sig_hi = 0xAAU, /**< High signature byte. */
  k_byte_mask  = 0xFFU, /**< Low-byte mask.       */
} ra8_fs_guard_sig_t;

/** @brief Memory-backed block device handed to ra8_fs. */
typedef struct {
  uint8_t* bytes;       /**< Flat sector store.         */
  uint32_t block_count; /**< Number of 512-byte blocks. */
} mem_disk_t;

static mem_disk_t s_disk = {};

static ra8_err_t mem_read(void* ctx, uint32_t lba, uint32_t count, uint8_t* buf)
{
  mem_disk_t* d = (mem_disk_t*)ctx;
  if (lba + count > d->block_count) {
    return k_ra8_err_out_of_range;
  }
  memcpy(buf,
         &d->bytes[(size_t)lba * (uint32_t)k_disk_block_size],
         (size_t)count * (uint32_t)k_disk_block_size);
  return k_ra8_ok;
}

static ra8_err_t mem_write(void* ctx, uint32_t lba, uint32_t count, const uint8_t* buf)
{
  mem_disk_t* d = (mem_disk_t*)ctx;
  if (lba + count > d->block_count) {
    return k_ra8_err_out_of_range;
  }
  memcpy(&d->bytes[(size_t)lba * (uint32_t)k_disk_block_size],
         buf,
         (size_t)count * (uint32_t)k_disk_block_size);
  return k_ra8_ok;
}

static ra8_err_t mem_capacity(void* ctx, uint32_t* block_count, uint32_t* block_size)
{
  mem_disk_t* d = (mem_disk_t*)ctx;
  *block_count  = d->block_count;
  *block_size   = (uint32_t)k_disk_block_size;
  return k_ra8_ok;
}

static const ra8_fs_backend_t s_backend = {
  .read_block   = mem_read,
  .write_block  = mem_write,
  .get_capacity = mem_capacity,
  .ctx          = &s_disk,
};

static void put16(uint8_t* p, uint32_t off, uint16_t v)
{
  p[off]     = (uint8_t)(v & (uint16_t)k_byte_mask);
  p[off + 1] = (uint8_t)((v >> 8) & (uint16_t)k_byte_mask);
}

static void free_volume(void)
{
  if (s_disk.bytes != nullptr) {
    free(s_disk.bytes);
    s_disk.bytes = nullptr;
  }
}

static void build_fat16_volume(void)
{
  free_volume();
  s_disk.block_count = (uint32_t)k_disk_blocks_fat16;
  s_disk.bytes = (uint8_t*)calloc(1, (size_t)s_disk.block_count * (uint32_t)k_disk_block_size);
  if (s_disk.bytes == nullptr) {
    TEST_FAIL_FMT("%s", "calloc failed");
  }
  uint8_t* bpb = &s_disk.bytes[0];
  put16(bpb, (uint32_t)k_bpb_off_bytes_per_sec, (uint16_t)k_disk_block_size);
  bpb[(uint32_t)k_bpb_off_sec_per_clus] = 1U;
  put16(bpb, (uint32_t)k_bpb_off_rsvd, (uint16_t)k_fat16_rsvd);
  bpb[(uint32_t)k_bpb_off_num_fats] = (uint8_t)k_fat16_num_fats;
  put16(bpb, (uint32_t)k_bpb_off_root_ents, (uint16_t)k_fat16_root_ents);
  put16(bpb, (uint32_t)k_bpb_off_tot_sec16, (uint16_t)k_disk_blocks_fat16);
  put16(bpb, (uint32_t)k_bpb_off_fatsz16, (uint16_t)k_fat16_fatsz);
  bpb[(uint32_t)k_bpb_off_sig_lo] = (uint8_t)k_bpb_sig_lo;
  bpb[(uint32_t)k_bpb_off_sig_hi] = (uint8_t)k_bpb_sig_hi;
}

/** @brief Fill @p buf with the deterministic probe payload. */
static void fill_payload(uint8_t* buf, uint32_t len)
{
  for (uint32_t i = 0U; i < len; i++) {
    buf[i] = (uint8_t)((i * (uint32_t)k_pattern_stride) + 3U);
  }
}

/**
 * @brief Build+mount a FAT16 volume holding one @p k_data_len byte "DATA.BIN".
 *
 * @param[out] out_h Receives the mounted volume handle.
 * @return The file reopened read-only, positioned at offset 0.
 */
static ra8_fs_file_t* mount_with_data_file(ra8_fs_mount_t** out_h)
{
  build_fat16_volume();
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));
  uint8_t payload[k_data_len] = {};
  fill_payload(payload, (uint32_t)k_data_len);
  ra8_fs_file_t* wf = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_open(h, "DATA.BIN", k_ra8_fs_mode_write, &wf));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_write(wf, payload, (uint32_t)k_data_len));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_close(wf));
  ra8_fs_file_t* rf = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_open(h, "DATA.BIN", k_ra8_fs_mode_read, &rf));
  *out_h = h;
  return rf;
}

/** @brief Directory-listing sink that counts entries (control-arm observation). */
static void count_cb(const char* name, uint8_t attr, uint32_t size, void* ctx)
{
  (void)name;
  (void)attr;
  (void)size;
  (*(uint32_t*)ctx)++;
}

/**
 * @test test_mcdc_mount_null_and_ops
 * @par MC/DC:
 * Two decisions in `libs/ra8_fs/src/ra8_fs_fat_mount.c@priv_mount_locked`.
 *
 * Guard `if (backend == nullptr || out_handle == nullptr)` (2 conditions):
 * - V1: backend=ok,   out=ok    -> C1=F, C2=F -> dec F (mounts -> ok).
 * - V2: backend=NULL, out=ok    -> C1=T short -> dec T -> null_ptr.
 * - V3: backend=ok,   out=NULL  -> C1=F, C2=T -> dec T -> null_ptr.
 *
 * Guard `if (read_block==NULL || write_block==NULL || get_capacity==NULL)`
 * (3 conditions):
 * - V4: all three set   -> F,F,F -> dec F (mounts -> ok, same as V1).
 * - V5: read_block=NULL  -> C1=T short          -> invalid_arg.
 * - V6: write_block=NULL -> C1=F,C2=T short      -> invalid_arg.
 * - V7: capacity=NULL    -> C1=F,C2=F,C3=T       -> invalid_arg.
 * V1|V4 vs each null vector isolates one condition. N+1 for the 2- and
 * 3-condition guards respectively.
 */
static void test_mcdc_mount_null_and_ops(void)
{
  TEST_BEGIN("ra8_fs MC/DC: ra8_fs_mount null + ops guards");
  build_fat16_volume();
  ra8_fs_mount_t* h = nullptr;

  /* Null guard. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h)); /* V1/V4 both-false. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_fs_mount(nullptr, &h));         /* V2 */
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_fs_mount(&s_backend, nullptr)); /* V3 */

  /* Function-pointer guard: null exactly one op each time. */
  ra8_fs_backend_t no_read = s_backend;
  no_read.read_block       = nullptr;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_fs_mount(&no_read, &h)); /* V5 */
  ra8_fs_backend_t no_write = s_backend;
  no_write.write_block      = nullptr;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_fs_mount(&no_write, &h)); /* V6 */
  ra8_fs_backend_t no_cap = s_backend;
  no_cap.get_capacity     = nullptr;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_fs_mount(&no_cap, &h)); /* V7 */

  free_volume();
  TEST_END("ra8_fs MC/DC: ra8_fs_mount null + ops guards");
}

/**
 * @test test_mcdc_open_null_guard
 * @par MC/DC:
 * Decision: `if (handle == nullptr || path == nullptr || out_file == nullptr)`
 * in `libs/ra8_fs/src/ra8_fs_fat_file.c@priv_open_locked` (3 conditions).
 * - V1: handle=ok, path=ok, out=ok -> F,F,F -> dec F (missing file -> not_found).
 * - V2: handle=NULL                -> C1=T short          -> null_ptr.
 * - V3: path=NULL   (handle ok)    -> C1=F,C2=T short      -> null_ptr.
 * - V4: out=NULL    (handle,path ok) -> C1=F,C2=F,C3=T     -> null_ptr.
 * V1 vs each null vector isolates one condition. N+1 = 4 for N=3.
 */
static void test_mcdc_open_null_guard(void)
{
  TEST_BEGIN("ra8_fs MC/DC: ra8_fs_open null guard");
  build_fat16_volume();
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));
  ra8_fs_file_t* f = nullptr;

  /* Control: all non-null -> guard false; a missing file misses, not null_ptr. */
  TEST_ASSERT_EQ(k_ra8_err_not_found, ra8_fs_open(h, "NONE.BIN", k_ra8_fs_mode_read, &f));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_fs_open(nullptr, "A.BIN", k_ra8_fs_mode_read, &f));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_fs_open(h, nullptr, k_ra8_fs_mode_read, &f));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_fs_open(h, "A.BIN", k_ra8_fs_mode_read, nullptr));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  free_volume();
  TEST_END("ra8_fs MC/DC: ra8_fs_open null guard");
}

/**
 * @test test_mcdc_listdir_and_unlink_null_guards
 * @par MC/DC:
 * Guard `if (handle == nullptr || cb == nullptr || path == nullptr)` in
 * `libs/ra8_fs/src/ra8_fs_fat_dir.c@priv_listdir_locked` (3 conditions):
 * - V1: handle,cb,path all ok -> F,F,F -> dec F (lists root -> ok).
 * - V2: handle=NULL           -> C1=T short      -> null_ptr.
 * - V3: cb=NULL  (handle,path ok) -> C1=F,C2=T short -> null_ptr.
 * - V4: path=NULL (handle,cb ok)  -> C1=F,C2=F,C3=T  -> null_ptr.
 *
 * @par MC/DC:
 * Guard `if (handle == nullptr || path == nullptr)` in
 * `libs/ra8_fs/src/ra8_fs_fat_dir.c@priv_unlink_locked` (2 conditions):
 * - V5: handle=ok,   path=ok    -> F,F -> dec F (missing file -> not_found).
 * - V6: handle=NULL, path=ok    -> C1=T short -> null_ptr.
 * - V7: handle=ok,   path=NULL  -> C1=F,C2=T  -> null_ptr.
 */
static void test_mcdc_listdir_and_unlink_null_guards(void)
{
  TEST_BEGIN("ra8_fs MC/DC: ra8_fs_listdir + ra8_fs_unlink null guards");
  build_fat16_volume();
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));

  uint32_t seen = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_listdir(h, "/", count_cb, &seen));                 /* V1 */
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_fs_listdir(nullptr, "/", count_cb, &seen)); /* V2 */
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_fs_listdir(h, "/", nullptr, &seen));        /* V3 */
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_fs_listdir(h, nullptr, count_cb, &seen));   /* V4 */

  TEST_ASSERT_EQ(k_ra8_err_not_found, ra8_fs_unlink(h, "NONE.BIN"));   /* V5 */
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_fs_unlink(nullptr, "A.BIN")); /* V6 */
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_fs_unlink(h, nullptr));       /* V7 */

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  free_volume();
  TEST_END("ra8_fs MC/DC: ra8_fs_listdir + ra8_fs_unlink null guards");
}

/**
 * @test test_mcdc_read_guards
 * @par MC/DC:
 * Guard `if (file == nullptr || buf == nullptr || got_len == nullptr)` in
 * `libs/ra8_fs/src/ra8_fs_fat_fileio.c@priv_read_locked` (3 conditions):
 * - V1: file,buf,got all ok -> F,F,F -> dec F (reads bytes).
 * - V2: file=NULL           -> C1=T short      -> null_ptr.
 * - V3: buf=NULL  (file ok)  -> C1=F,C2=T short -> null_ptr.
 * - V4: got=NULL  (file,buf ok) -> C1=F,C2=F,C3=T -> null_ptr.
 *
 * Guard `if (file->offset >= file->size_bytes || max_len == 0U)` (2 conditions):
 * - V5: offset<size, max_len>0 -> F,F -> dec F (reads bytes; = V1).
 * - V6: offset==size (seek to EOF), max_len>0 -> C1=T short -> ok, got=0.
 * - V7: offset<size, max_len==0               -> C1=F,C2=T  -> ok, got=0.
 */
static void test_mcdc_read_guards(void)
{
  TEST_BEGIN("ra8_fs MC/DC: ra8_fs_read pointer + short-read guards");
  ra8_fs_mount_t* h               = nullptr;
  ra8_fs_file_t*  f               = mount_with_data_file(&h);
  uint8_t         buf[k_data_len] = {};
  uint32_t        got             = 0U;

  /* V1/V5 both-false: a real read of the whole payload. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_seek(f, 0U));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_read(f, buf, (uint32_t)k_data_len, &got));
  TEST_ASSERT_EQ(k_data_len, got);

  /* Pointer-guard TRUE arms. */
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_fs_read(nullptr, buf, (uint32_t)k_data_len, &got));                   /* V2 */
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_fs_read(f, nullptr, (uint32_t)k_data_len, &got)); /* V3 */
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_fs_read(f, buf, (uint32_t)k_data_len, nullptr));  /* V4 */

  /* Short-read guard TRUE arms. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_seek(f, (uint32_t)k_data_len)); /* offset == size */
  got = k_poison;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_read(f, buf, (uint32_t)k_data_len, &got)); /* V6 */
  TEST_ASSERT_EQ(0U, got);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_seek(f, 0U));
  got = k_poison;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_read(f, buf, 0U, &got)); /* V7 max_len == 0 */
  TEST_ASSERT_EQ(0U, got);

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_close(f));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  free_volume();
  TEST_END("ra8_fs MC/DC: ra8_fs_read pointer + short-read guards");
}

/**
 * @test test_mcdc_write_guards
 * @par MC/DC:
 * Guard `if (file == nullptr || buf == nullptr)` in
 * `libs/ra8_fs/src/ra8_fs_fat_fileio.c@priv_write_locked` (2 conditions):
 * - V1: file=ok,   buf=ok    -> F,F -> dec F (writes bytes).
 * - V2: file=NULL, buf=ok    -> C1=T short -> null_ptr.
 * - V3: file=ok,   buf=NULL  -> C1=F,C2=T  -> null_ptr.
 *
 * Guard `if (file->in_use == 0U || file->mode == k_ra8_fs_mode_read)`
 * (2 conditions):
 * - V4: in_use=1, mode=write -> F,F -> dec F (writes bytes; = V1).
 * - V5: in_use=0, mode=write -> C1=T short -> invalid_state (slot with the
 *       in-use flag cleared but a non-read mode isolates the first condition).
 * - V6: in_use=1, mode=read  -> C1=F,C2=T  -> invalid_state (a read-mode handle).
 */
static void test_mcdc_write_guards(void)
{
  TEST_BEGIN("ra8_fs MC/DC: ra8_fs_write pointer + state guards");
  build_fat16_volume();
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));
  uint8_t payload[k_data_len] = {};
  fill_payload(payload, (uint32_t)k_data_len);

  /* V1/V4 both-false: a real write on an in-use write handle. */
  ra8_fs_file_t* wf = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_open(h, "WR.BIN", k_ra8_fs_mode_write, &wf));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_write(wf, payload, (uint32_t)k_data_len));

  /* Pointer-guard TRUE arms. */
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_fs_write(nullptr, payload, (uint32_t)k_data_len)); /* V2 */
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_fs_write(wf, nullptr, (uint32_t)k_data_len));      /* V3 */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_close(wf));

  /* V5: in_use==0 with mode=write -> first condition true in isolation. */
  ra8_fs_file_t stale = {};
  stale.in_use        = 0U;
  stale.mode          = k_ra8_fs_mode_write;
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, ra8_fs_write(&stale, payload, (uint32_t)k_data_len));

  /* V6: in_use==1 read-mode handle -> second condition true in isolation. */
  ra8_fs_file_t* rf = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_open(h, "WR.BIN", k_ra8_fs_mode_read, &rf));
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, ra8_fs_write(rf, payload, (uint32_t)k_data_len));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_close(rf));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  free_volume();
  TEST_END("ra8_fs MC/DC: ra8_fs_write pointer + state guards");
}

/**
 * @test test_mcdc_tell_and_size_null_guards
 * @par MC/DC:
 * Guard `if (file == nullptr || out_offset == nullptr)` in
 * `libs/ra8_fs/src/ra8_fs_fat_fileio.c@priv_tell_locked` (2 conditions):
 * - V1: file=ok,   out=ok    -> F,F -> dec F (returns offset).
 * - V2: file=NULL, out=ok    -> C1=T short -> null_ptr.
 * - V3: file=ok,   out=NULL  -> C1=F,C2=T  -> null_ptr.
 *
 * @par MC/DC:
 * Guard `if (file == nullptr || out_bytes == nullptr)` in
 * `libs/ra8_fs/src/ra8_fs_fat_fileio.c@priv_size_locked` (2 conditions), same shape:
 * - V4: file=ok, out=ok -> F,F; V5: file=NULL -> C1=T; V6: out=NULL -> C1=F,C2=T.
 */
static void test_mcdc_tell_and_size_null_guards(void)
{
  TEST_BEGIN("ra8_fs MC/DC: ra8_fs_tell + ra8_fs_size null guards");
  ra8_fs_mount_t* h   = nullptr;
  ra8_fs_file_t*  f   = mount_with_data_file(&h);
  uint32_t        pos = k_poison;
  uint32_t        sz  = 0U;

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_tell(f, &pos)); /* V1 */
  TEST_ASSERT_EQ(0U, pos);
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_fs_tell(nullptr, &pos)); /* V2 */
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_fs_tell(f, nullptr));    /* V3 */

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_size(f, &sz)); /* V4 */
  TEST_ASSERT_EQ(k_data_len, sz);
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_fs_size(nullptr, &sz)); /* V5 */
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_fs_size(f, nullptr));   /* V6 */

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_close(f));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  free_volume();
  TEST_END("ra8_fs MC/DC: ra8_fs_tell + ra8_fs_size null guards");
}

/**
 * @test test_mcdc_format_null_and_ops
 * @par MC/DC:
 * Guard `if (backend == nullptr || opts == nullptr)` in
 * `libs/ra8_fs/src/ra8_fs_fat_mount.c@priv_format_locked` (2 conditions):
 * - V1: backend=ok,   opts=ok    -> F,F -> dec F (formats -> ok).
 * - V2: backend=NULL, opts=ok    -> C1=T short -> null_ptr.
 * - V3: backend=ok,   opts=NULL  -> C1=F,C2=T  -> null_ptr.
 *
 * Guard `if (write_block == nullptr || get_capacity == nullptr)` (2 conditions):
 * - V4: both set        -> F,F -> dec F (formats -> ok; = V1).
 * - V5: write_block=NULL -> C1=T short -> invalid_arg.
 * - V6: get_capacity=NULL -> C1=F,C2=T -> invalid_arg.
 * These two guards precede the type/block-size guards already covered in
 * test_ra8_fs_fat_mcdc.c, completing MC/DC for ra8_fs_format.
 */
static void test_mcdc_format_null_and_ops(void)
{
  TEST_BEGIN("ra8_fs MC/DC: ra8_fs_format null + ops guards");
  build_fat16_volume();
  ra8_fs_format_opts_t opts = {};
  opts.type                 = k_ra8_fs_type_fat16;

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_format(&s_backend, &opts));             /* V1/V4 */
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_fs_format(nullptr, &opts));      /* V2    */
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_fs_format(&s_backend, nullptr)); /* V3    */

  ra8_fs_backend_t no_write = s_backend;
  no_write.write_block      = nullptr;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_fs_format(&no_write, &opts)); /* V5 */
  ra8_fs_backend_t no_cap = s_backend;
  no_cap.get_capacity     = nullptr; /* write_block stays set -> isolates C2 */
  /* V6 */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_fs_format(&no_cap, &opts));

  free_volume();
  TEST_END("ra8_fs MC/DC: ra8_fs_format null + ops guards");
}

int32_t main(void)
{
  test_mcdc_mount_null_and_ops();
  test_mcdc_open_null_guard();
  test_mcdc_listdir_and_unlink_null_guards();
  test_mcdc_read_guards();
  test_mcdc_write_guards();
  test_mcdc_tell_and_size_null_guards();
  test_mcdc_format_null_and_ops();
  (void)fprintf(stderr, "[OK  ] test_ra8_fs_fat_guards_mcdc.c\n");
  return 0;
}
