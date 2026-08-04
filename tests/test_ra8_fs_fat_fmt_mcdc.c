/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file test_ra8_fs_fat_fmt_mcdc.c
 * @brief MC/DC vectors for the FAT format-time and BPB-parse compound decisions.
 *
 * @details
 * Dedicated N+1 independent-influence vector sets (issue #426) for the
 * TU-internal helpers behind mount/format that are driven either directly (they
 * are declared in `ra8_fs_fat_internal.h`) or through the byte fields they read:
 *
 *   - `priv_parse_bpb_into_mount` (ra8_fs_fat_mount.c) -- the boot-signature
 *     guard and the bytes-per-sector / SPC / num-FATs sanity guard, driven by
 *     seeding the shared `s_scratch` boot-sector buffer.
 *   - `priv_fmt_choose_geometry` (ra8_fs_fat_fmt.c) -- the auto-sweep exhaustion
 *     guard, driven with a device too small to land any cluster band.
 *   - `priv_fmt_clear_region` (ra8_fs_fat_fmt.c) -- the erase-hook fast path,
 *     driven with a counting backend whose erase result and presence vary.
 *   - `priv_fmt_label_field` (ra8_fs_fat_fmt.c, TU-static) -- the volume-label
 *     copy/pad split, driven through a real `ra8_fs_format` with a short label.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ra8_err.h"
#include "ra8_fs.h"
#include "ra8_fs_fat_internal.h"
#include "unity_minimal.h"

/**
 * @enum ra8_fs_fmt_mcdc_t
 * @brief Disk sizing and the small out-of-band geometry for the format vectors.
 */
typedef enum : uint32_t {
  k_disk_block_size    = 512U,       /**< Bytes per logical block.                */
  k_disk_blocks_fat16  = 8U * 1024U, /**< 4 MiB FAT16 card (lands the band).      */
  k_tiny_total_sectors = 100U,       /**< Too small for any FAT band.             */
  k_tiny_overhead_secs = 1U,         /**< Reserved / root span for the tiny geom. */
  k_clear_lba          = 0U,         /**< Region base for the clear-region calls. */
  k_clear_count        = 1U,         /**< One sector cleared per call.            */
} ra8_fs_fmt_mcdc_t;

/**
 * @enum ra8_fs_fmt_bpb_t
 * @brief The valid boot-sector fields seeded into `s_scratch` before mutation.
 */
typedef enum : uint16_t {
  k_bad_bytes_per_sector = 1024U, /**< Any legal-looking but non-512 sector size. */
  k_fmt_byte_mask        = 0xFFU, /**< Low-byte mask.                             */
} ra8_fs_fmt_bpb_t;

/* --- priv_parse_bpb_into_mount ------------------------------------------- */

static void scratch_put16(uint32_t off, uint16_t v)
{
  s_scratch[off]     = (uint8_t)(v & (uint16_t)k_fmt_byte_mask);
  s_scratch[off + 1] = (uint8_t)((v >> 8) & (uint16_t)k_fmt_byte_mask);
}

/** @brief Seed `s_scratch` with a boot sector that passes both guards. */
static void seed_valid_bpb(void)
{
  memset(s_scratch, 0, sizeof s_scratch);
  s_scratch[(uint32_t)k_bpb_off_signature_lo] = (uint8_t)k_bpb_sig_lo;
  s_scratch[(uint32_t)k_bpb_off_signature_hi] = (uint8_t)k_bpb_sig_hi;
  scratch_put16((uint32_t)k_bpb_off_bytes_per_sec, (uint16_t)k_ra8_fs_bytes_per_sector);
  s_scratch[(uint32_t)k_bpb_off_sec_per_clus] = 1U;
  s_scratch[(uint32_t)k_bpb_off_num_fats]     = 2U;
}

/**
 * @test test_mcdc_parse_bpb_guards
 * @par MC/DC:
 * Two decisions in `libs/ra8_fs/src/ra8_fs_fat_mount.c@priv_parse_bpb_into_mount`,
 * driven by seeding the shared `s_scratch` boot sector.
 *
 * Signature guard `if (sig_lo != 0x55 || sig_hi != 0xAA)` (2 conditions):
 * - V1: sig_lo=0x55, sig_hi=0xAA -> F,F -> dec F (proceeds).
 * - V2: sig_lo!=0x55            -> C1=T short      -> validation_failed.
 * - V3: sig_lo=0x55, sig_hi!=0xAA -> C1=F,C2=T     -> validation_failed.
 *
 * Field guard `if (bytes_per_sector != 512 || sectors_per_cluster == 0 ||
 * num_fats == 0)` (3 conditions), reached only once the signature is valid:
 * - V4: 512, spc=1, fats=2 -> F,F,F -> dec F (returns ok).
 * - V5: bps!=512           -> C1=T short          -> validation_failed.
 * - V6: spc==0 (bps ok)    -> C1=F,C2=T short      -> validation_failed.
 * - V7: fats==0 (bps,spc ok) -> C1=F,C2=F,C3=T     -> validation_failed.
 */
static void test_mcdc_parse_bpb_guards(void)
{
  TEST_BEGIN("ra8_fs MC/DC: priv_parse_bpb_into_mount signature + field guards");
  ra8_fs_mount_t m = {};

  /* Signature guard. */
  seed_valid_bpb();
  TEST_ASSERT_EQ(k_ra8_ok, priv_parse_bpb_into_mount(&m)); /* V1 (and V4). */
  seed_valid_bpb();
  s_scratch[(uint32_t)k_bpb_off_signature_lo] = 0x00U;
  TEST_ASSERT_EQ(k_ra8_err_validation_failed, priv_parse_bpb_into_mount(&m)); /* V2 */
  seed_valid_bpb();
  s_scratch[(uint32_t)k_bpb_off_signature_hi] = 0x00U;
  TEST_ASSERT_EQ(k_ra8_err_validation_failed, priv_parse_bpb_into_mount(&m)); /* V3 */

  /* Field guard (signature kept valid). */
  seed_valid_bpb();
  scratch_put16((uint32_t)k_bpb_off_bytes_per_sec, (uint16_t)k_bad_bytes_per_sector);
  TEST_ASSERT_EQ(k_ra8_err_validation_failed, priv_parse_bpb_into_mount(&m)); /* V5 */
  seed_valid_bpb();
  s_scratch[(uint32_t)k_bpb_off_sec_per_clus] = 0U;
  TEST_ASSERT_EQ(k_ra8_err_validation_failed, priv_parse_bpb_into_mount(&m)); /* V6 */
  seed_valid_bpb();
  s_scratch[(uint32_t)k_bpb_off_num_fats] = 0U;
  TEST_ASSERT_EQ(k_ra8_err_validation_failed, priv_parse_bpb_into_mount(&m)); /* V7 */

  TEST_END("ra8_fs MC/DC: priv_parse_bpb_into_mount signature + field guards");
}

/* --- priv_fmt_choose_geometry -------------------------------------------- */

/**
 * @test test_mcdc_choose_geometry_sweep_exhaustion
 * @par MC/DC:
 * Decision: `if (!auto_mode || spc >= k_fmt_spc_max)` in
 * `libs/ra8_fs/src/ra8_fs_fat_fmt.c@priv_fmt_choose_geometry` (2 conditions),
 * the per-iteration "give up" test in the cluster-size sweep. A device too
 * small to reach the FAT16 lower band (< 4085 clusters at every SPC) never
 * satisfies the band, so the decision is reached on every iteration.
 * - Auto sweep (spc_hint=0): early iterations see `!auto_mode`=F and `spc<max`=F
 *   (C1=F, C2=F -> keep doubling), and the final iteration sees `spc>=max`
 *   (C1=F, C2=T -> give up) -> invalid_size. Covers the both-false control and
 *   the C2-true arm.
 * - Manual hint (spc_hint=1): `!auto_mode`=T on the first band miss short-circuits
 *   -> invalid_size. Covers the C1-true arm.
 * Auto vs manual isolates C1; the auto sweep's first vs last iteration isolates
 * C2. N+1 = 3 for N=2.
 */
static void test_mcdc_choose_geometry_sweep_exhaustion(void)
{
  TEST_BEGIN("ra8_fs MC/DC: priv_fmt_choose_geometry sweep exhaustion");
  ra8_fs_fmt_geom_t g = {};
  g.type              = k_ra8_fs_type_fat16;
  g.total_sectors     = (uint32_t)k_tiny_total_sectors;
  g.reserved_sectors  = (uint32_t)k_tiny_overhead_secs;
  g.root_sectors      = (uint32_t)k_tiny_overhead_secs;

  /* Auto: F,F on early sweeps, C2=T at the cap. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_size, priv_fmt_choose_geometry(&g, 0U));
  /* Manual: C1=T on the first band miss. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_size, priv_fmt_choose_geometry(&g, 1U));
  TEST_END("ra8_fs MC/DC: priv_fmt_choose_geometry sweep exhaustion");
}

/* --- priv_fmt_clear_region ----------------------------------------------- */

/** @brief Counting backend context for the clear-region vectors. */
typedef struct {
  uint32_t erase_calls; /**< Times the erase hook fired.          */
  uint32_t write_calls; /**< Times the zero-write fallback fired. */
  bool     erase_ok;    /**< What the erase hook returns.         */
} clr_ctx_t;

static ra8_err_t clr_erase(void* ctx, uint32_t lba, uint32_t count)
{
  (void)lba;
  (void)count;
  clr_ctx_t* c = (clr_ctx_t*)ctx;
  c->erase_calls++;
  return c->erase_ok ? k_ra8_ok : k_ra8_err_out_of_range;
}

static ra8_err_t clr_write(void* ctx, uint32_t lba, uint32_t count, const uint8_t* buf)
{
  (void)lba;
  (void)count;
  (void)buf;
  clr_ctx_t* c = (clr_ctx_t*)ctx;
  c->write_calls++;
  return k_ra8_ok;
}

/**
 * @test test_mcdc_clear_region_erase_hook
 * @par MC/DC:
 * Decision: `if ((erase_blocks != nullptr) && (erase_blocks(...) == k_ra8_ok))`
 * in `libs/ra8_fs/src/ra8_fs_fat_fmt.c@priv_fmt_clear_region` (2 conditions).
 * - V1 (T,T): erase hook present and returns ok -> take the fast path; the
 *   zero-write fallback never runs (write_calls == 0).
 * - V2 (C1=F): no erase hook -> short-circuit -> zero-write fallback runs.
 * - V3 (T,F): erase hook present but returns an error -> C2=F -> zero-write
 *   fallback runs.
 * V1 vs V2 isolates the hook presence; V1 vs V3 isolates the hook result.
 * N+1 = 3 for N=2.
 */
static void test_mcdc_clear_region_erase_hook(void)
{
  TEST_BEGIN("ra8_fs MC/DC: priv_fmt_clear_region erase-hook fast path");
  clr_ctx_t        c = {};
  ra8_fs_backend_t b = {};
  b.write_block      = clr_write;
  b.ctx              = &c;

  /* V1 (T,T): erase succeeds -> fast path, no zero-write. */
  c.erase_calls  = 0U;
  c.write_calls  = 0U;
  c.erase_ok     = true;
  b.erase_blocks = clr_erase;
  TEST_ASSERT_EQ(k_ra8_ok,
                 priv_fmt_clear_region(&b, (uint32_t)k_clear_lba, (uint32_t)k_clear_count));
  TEST_ASSERT_EQ(1U, c.erase_calls);
  TEST_ASSERT_EQ(0U, c.write_calls);

  /* V2 (C1=F): no erase hook -> zero-write fallback. */
  c.erase_calls  = 0U;
  c.write_calls  = 0U;
  b.erase_blocks = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok,
                 priv_fmt_clear_region(&b, (uint32_t)k_clear_lba, (uint32_t)k_clear_count));
  TEST_ASSERT_EQ(0U, c.erase_calls);
  TEST_ASSERT(c.write_calls >= 1U);

  /* V3 (T,F): erase present but fails -> zero-write fallback. */
  c.erase_calls  = 0U;
  c.write_calls  = 0U;
  c.erase_ok     = false;
  b.erase_blocks = clr_erase;
  TEST_ASSERT_EQ(k_ra8_ok,
                 priv_fmt_clear_region(&b, (uint32_t)k_clear_lba, (uint32_t)k_clear_count));
  TEST_ASSERT_EQ(1U, c.erase_calls);
  TEST_ASSERT(c.write_calls >= 1U);
  TEST_END("ra8_fs MC/DC: priv_fmt_clear_region erase-hook fast path");
}

/* --- priv_fmt_label_field (via ra8_fs_format) ---------------------------- */

/** @brief Memory-backed block device for the real-format label vectors. */
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

static void free_volume(void)
{
  if (s_disk.bytes != nullptr) {
    free(s_disk.bytes);
    s_disk.bytes = nullptr;
  }
}

static void alloc_card(uint32_t blocks)
{
  free_volume();
  s_disk.block_count = blocks;
  s_disk.bytes       = (uint8_t*)calloc(1, (size_t)blocks * (uint32_t)k_disk_block_size);
  if (s_disk.bytes == nullptr) {
    TEST_FAIL_FMT("%s", "calloc failed");
  }
}

/**
 * @test test_mcdc_label_field_copy_pad
 * @par MC/DC:
 * Decision: `if (!past_end && (label[i] == '\0'))` in
 * `libs/ra8_fs/src/ra8_fs_fat_fmt.c@priv_fmt_label_field` (2 conditions), driven
 * through a real FAT16 `ra8_fs_format` that lays the BS_VolLab field. `past_end`
 * latches once the label's NUL is seen, so one short label sweeps all arms:
 * - before the NUL (i=0,1 of "AB"): `!past_end`=T, `label[i]=='\0'`=F -> C1=T,C2=F.
 * - at the NUL (i=2 of "AB"):        `!past_end`=T, `label[i]=='\0'`=T -> C1=T,C2=T
 *   -> latches past_end.
 * - after the NUL (i=3..10):         `!past_end`=F -> short-circuit -> C1=F.
 * The i=0/1 vs i=2 pair isolates the terminator test (C2); the i=2 vs i=3 pair
 * isolates the latch (C1). A separate NULL-label format enters with past_end
 * already true, re-confirming the C1=F arm. Both formats succeed and mount.
 */
static void test_mcdc_label_field_copy_pad(void)
{
  TEST_BEGIN("ra8_fs MC/DC: priv_fmt_label_field copy/pad (!past_end && NUL)");
  ra8_fs_format_opts_t opts = {};
  opts.type                 = k_ra8_fs_type_fat16;

  /* Short label "AB": drives C1=T/C2=F, C1=T/C2=T, then C1=F. */
  alloc_card((uint32_t)k_disk_blocks_fat16);
  opts.label = "AB";
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_format(&s_backend, &opts));
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));
  TEST_ASSERT_EQ(k_ra8_fs_type_fat16, h->type);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  free_volume();

  /* NULL label: enters with past_end already true (C1=F from the first byte). */
  alloc_card((uint32_t)k_disk_blocks_fat16);
  opts.label = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_format(&s_backend, &opts));
  h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));
  TEST_ASSERT_EQ(k_ra8_fs_type_fat16, h->type);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  free_volume();
  TEST_END("ra8_fs MC/DC: priv_fmt_label_field copy/pad (!past_end && NUL)");
}

int32_t main(void)
{
  test_mcdc_parse_bpb_guards();
  test_mcdc_choose_geometry_sweep_exhaustion();
  test_mcdc_clear_region_erase_hook();
  test_mcdc_label_field_copy_pad();
  (void)fprintf(stderr, "[OK  ] test_ra8_fs_fat_fmt_mcdc.c\n");
  return 0;
}
