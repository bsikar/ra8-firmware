/**
 * @file test_ra8_io_fsfmt_cov.c
 * @brief Validation and capability-consistency vectors for filesystem formats.
 *
 * @details
 * Builds fixed fake backend operation tables to cover required callbacks,
 * mutually dependent capability flags, duplicate registration, capacity
 * limits, and lookup behavior. The vectors verify that inconsistent filesystem
 * implementations are rejected before they enter the global format registry.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>

#include "ra8_err.h"
#include "ra8_fs.h"
#include "ra8_io_fsfmt.h"
#include "unity_minimal.h"

static bool cov_probe(const ra8_fs_backend_t* backend)
{
  return backend->ctx != nullptr;
}

static ra8_err_t cov_mount(const ra8_fs_backend_t* backend, void** out_mount)
{
  *out_mount = backend->ctx;
  return k_ra8_ok;
}

static ra8_err_t cov_unmount(void* mount_ctx)
{
  (void)mount_ctx;
  return k_ra8_ok;
}

static ra8_err_t cov_open(void* mount_ctx, const char* path, ra8_fs_mode_t mode, void** out_file)
{
  (void)path;
  (void)mode;
  *out_file = mount_ctx;
  return k_ra8_ok;
}

static ra8_err_t cov_close(void* file_ctx)
{
  (void)file_ctx;
  return k_ra8_ok;
}

static ra8_err_t cov_read(void* file_ctx, void* buf, uint32_t bytes, uint32_t* out_read)
{
  (void)file_ctx;
  (void)buf;
  (void)bytes;
  *out_read = 0U;
  return k_ra8_ok;
}

static ra8_err_t cov_write(void* file_ctx, const void* buf, uint32_t bytes)
{
  (void)file_ctx;
  (void)buf;
  (void)bytes;
  return k_ra8_ok;
}

static ra8_err_t cov_seek(void* file_ctx, uint64_t offset)
{
  (void)file_ctx;
  (void)offset;
  return k_ra8_ok;
}

static ra8_err_t cov_tell(const void* file_ctx, uint64_t* out_offset)
{
  (void)file_ctx;
  *out_offset = 0U;
  return k_ra8_ok;
}

static ra8_err_t cov_size(const void* file_ctx, uint64_t* out_size)
{
  (void)file_ctx;
  *out_size = 0U;
  return k_ra8_ok;
}

static ra8_err_t cov_sync(void* file_ctx)
{
  (void)file_ctx;
  return k_ra8_ok;
}

static ra8_err_t cov_stat(void* mount_ctx, const char* path, ra8_fs_stat_t* out)
{
  (void)mount_ctx;
  (void)path;
  *out = (ra8_fs_stat_t){};
  return k_ra8_ok;
}

static ra8_err_t
cov_listdir(void* mount_ctx, const char* path, ra8_fs_listdir_cb_t cb, void* cb_ctx)
{
  (void)mount_ctx;
  (void)path;
  (void)cb;
  (void)cb_ctx;
  return k_ra8_ok;
}

static ra8_err_t cov_path(void* mount_ctx, const char* path)
{
  (void)mount_ctx;
  (void)path;
  return k_ra8_ok;
}

static ra8_err_t cov_rename(void* mount_ctx, const char* old_path, const char* new_path)
{
  (void)mount_ctx;
  (void)old_path;
  (void)new_path;
  return k_ra8_ok;
}

static ra8_err_t cov_space(void* mount_ctx, ra8_fs_space_t* out)
{
  (void)mount_ctx;
  *out = (ra8_fs_space_t){};
  return k_ra8_ok;
}

static const ra8_io_fsfmt_ops_t k_valid_ops = {
  .probe      = cov_probe,
  .mount      = cov_mount,
  .unmount    = cov_unmount,
  .open       = cov_open,
  .close      = cov_close,
  .read       = cov_read,
  .write      = cov_write,
  .seek       = cov_seek,
  .tell       = cov_tell,
  .size       = cov_size,
  .sync       = cov_sync,
  .stat       = cov_stat,
  .listdir    = cov_listdir,
  .unlink     = cov_path,
  .rename     = cov_rename,
  .mkdir      = cov_path,
  .rmdir      = cov_path,
  .free_space = cov_space,
};

static const ra8_io_fsfmt_t k_valid_format = {
  .name = "cov",
  .caps = {},
  .ops  = &k_valid_ops,
};

/** @brief Every mandatory descriptor member is rejected independently. */
static void test_required_ops(void)
{
  TEST_BEGIN("fsfmt required ops validation");
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_fsfmt_init());
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_io_fsfmt_register(nullptr));
  ra8_io_fsfmt_t fmt = k_valid_format;
  fmt.name           = nullptr;
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_io_fsfmt_register(&fmt));
  fmt     = k_valid_format;
  fmt.ops = nullptr;
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_io_fsfmt_register(&fmt));

  ra8_io_fsfmt_ops_t ops = k_valid_ops;
#define EXPECT_REQUIRED_NULL(member_)                                                              \
  do {                                                                                             \
    ops         = k_valid_ops;                                                                     \
    ops.member_ = nullptr;                                                                         \
    fmt         = k_valid_format;                                                                  \
    fmt.ops     = &ops;                                                                            \
    TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_io_fsfmt_register(&fmt));                               \
  } while (false)
  EXPECT_REQUIRED_NULL(probe);
  EXPECT_REQUIRED_NULL(mount);
  EXPECT_REQUIRED_NULL(unmount);
  EXPECT_REQUIRED_NULL(open);
  EXPECT_REQUIRED_NULL(close);
  EXPECT_REQUIRED_NULL(read);
  EXPECT_REQUIRED_NULL(seek);
  EXPECT_REQUIRED_NULL(tell);
  EXPECT_REQUIRED_NULL(size);
  EXPECT_REQUIRED_NULL(stat);
  EXPECT_REQUIRED_NULL(listdir);
#undef EXPECT_REQUIRED_NULL
  TEST_END("fsfmt required ops validation");
}

/** @brief Every capability-to-operation invariant is checked independently. */
static void test_capability_consistency(void)
{
  TEST_BEGIN("fsfmt capability consistency");
  ra8_io_fsfmt_t     fmt = k_valid_format;
  ra8_io_fsfmt_ops_t ops = k_valid_ops;
#define EXPECT_CAP_MISMATCH(cap_, member_)                                                         \
  do {                                                                                             \
    TEST_ASSERT_EQ(k_ra8_ok, ra8_io_fsfmt_init());                                                 \
    ops           = k_valid_ops;                                                                   \
    ops.member_   = nullptr;                                                                       \
    fmt           = k_valid_format;                                                                \
    fmt.caps.cap_ = true;                                                                          \
    fmt.ops       = &ops;                                                                          \
    TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_io_fsfmt_register(&fmt));                            \
  } while (false)
  EXPECT_CAP_MISMATCH(supports_streaming_write, write);
  EXPECT_CAP_MISMATCH(supports_mkdir, mkdir);
  EXPECT_CAP_MISMATCH(supports_rmdir, rmdir);
  EXPECT_CAP_MISMATCH(supports_free_space, free_space);
  EXPECT_CAP_MISMATCH(supports_sync, sync);
  EXPECT_CAP_MISMATCH(atomic_rename, rename);
#undef EXPECT_CAP_MISMATCH

  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_fsfmt_init());
  fmt                   = k_valid_format;
  fmt.caps.durable_sync = true;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_io_fsfmt_register(&fmt));
  TEST_END("fsfmt capability consistency");
}

/** @brief Registry, builtin lookup, probe-success and probe-miss branches. */
static void test_registry_boundaries(void)
{
  TEST_BEGIN("fsfmt registry boundaries");
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_fsfmt_init());
  for (uint32_t i = 2U; i < (uint32_t)k_ra8_io_fsfmt_max; ++i) {
    TEST_ASSERT_EQ(k_ra8_ok, ra8_io_fsfmt_register(&k_valid_format));
  }
  TEST_ASSERT_EQ(k_ra8_err_no_mem, ra8_io_fsfmt_register(&k_valid_format));

  const ra8_io_fsfmt_t* out = nullptr;
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_io_fsfmt_get_builtin(k_ra8_fs_type_fat16, nullptr));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_fsfmt_get_builtin(k_ra8_fs_type_fat12, &out));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_fsfmt_get_builtin(k_ra8_fs_type_fat16, &out));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_fsfmt_get_builtin(k_ra8_fs_type_fat32, &out));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_fsfmt_get_builtin(k_ra8_fs_type_exfat, &out));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_io_fsfmt_get_builtin(k_ra8_fs_type_unknown, &out));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_fsfmt_init());
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_fsfmt_register(&k_valid_format));
  ra8_fs_backend_t backend = {};
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_io_fsfmt_probe(nullptr, &out));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_io_fsfmt_probe(&backend, nullptr));
  TEST_ASSERT_EQ(k_ra8_err_not_found, ra8_io_fsfmt_probe(&backend, &out));
  uint8_t marker = 1U;
  backend.ctx    = &marker;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_fsfmt_probe(&backend, &out));
  TEST_ASSERT(out == &k_valid_format);
  TEST_END("fsfmt registry boundaries");
}

int32_t main(void)
{
  test_required_ops();
  test_capability_consistency();
  test_registry_boundaries();
  (void)fprintf(stderr, "[OK  ] test_ra8_io_fsfmt_cov.c\n");
  return 0;
}
