/**
 * @file test_ra8_fmt.c
 * @brief Host C23 ABI acceptance tests for `libs/ra8_fmt`.
 *
 * @details
 * Exercises all six C ABI exports declared in `ra8_fmt_stream.h` against the
 * `libra8_fmt.a` static archive: structural JOF inspect, strict RBKC inspect,
 * convert requirements/stream, and verify requirements/stream, alongside static
 * layout checks on the shared contracts.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "ra8_err.h"
#include "ra8_fmt_stream.h"
#include "unity_minimal.h"

static_assert(sizeof(ra8_fmt_jof_convert_requirements_t) == 16U, "convert requirements size");
static_assert(offsetof(ra8_fmt_jof_verify_requirements_t, band_height) == 6U, "band_height offset");
static_assert(sizeof(ra8_fmt_jof_verify_requirements_t) == 32U, "verify requirements size");
static_assert(sizeof(ra8_fmt_sink_t) == sizeof(void*) * 2U, "sink size");
static_assert(sizeof(ra8_fmt_transaction_t) == sizeof(void*) * 2U, "transaction size");

typedef struct {
  const uint8_t* data;
  size_t         len;
} mem_src_t;

typedef struct {
  uint8_t buf[1024];
  size_t  len;
} mem_sink_t;

typedef struct {
  uint8_t stage[1024];
  size_t  stage_len;
  uint8_t committed_buf[1024];
  size_t  committed_len;
  bool    committed;
  bool    aborted;
} mem_tx_t;

typedef struct {
  uint8_t buf[1024];
  size_t  len;
  bool    sealed;
} mem_spool_t;

static ra8_err_t mem_read_at(void* ctx, uint64_t off, uint8_t* buf, size_t len, size_t* got)
{
  const mem_src_t* s = (const mem_src_t*)ctx;
  if ((s == nullptr) || (got == nullptr)) {
    return k_ra8_err_null_ptr;
  }
  if (off >= s->len) {
    *got = 0U;
    return k_ra8_ok;
  }
  size_t avail = s->len - (size_t)off;
  if (avail > len) {
    avail = len;
  }
  if (avail > 0U) {
    memcpy(buf, &s->data[(size_t)off], avail);
  }
  *got = avail;
  return k_ra8_ok;
}

static ra8_err_t mem_sink_write(void* ctx, const uint8_t* bytes, size_t len)
{
  mem_sink_t* s = (mem_sink_t*)ctx;
  if ((s->len + len) > sizeof(s->buf)) {
    return k_ra8_err_no_mem;
  }
  if (len > 0U) {
    memcpy(&s->buf[s->len], bytes, len);
    s->len += len;
  }
  return k_ra8_ok;
}

static ra8_err_t mem_tx_append(void* ctx, const uint8_t* bytes, size_t len)
{
  mem_tx_t* t = (mem_tx_t*)ctx;
  if ((t->stage_len + len) > sizeof(t->stage)) {
    return k_ra8_err_no_mem;
  }
  if (len > 0U) {
    memcpy(&t->stage[t->stage_len], bytes, len);
    t->stage_len += len;
  }
  return k_ra8_ok;
}

static ra8_err_t mem_tx_commit(void* ctx)
{
  mem_tx_t* t = (mem_tx_t*)ctx;
  memcpy(t->committed_buf, t->stage, t->stage_len);
  t->committed_len = t->stage_len;
  t->committed     = true;
  return k_ra8_ok;
}

static void mem_tx_abort(void* ctx)
{
  mem_tx_t* t  = (mem_tx_t*)ctx;
  t->stage_len = 0U;
  t->aborted   = true;
}

static const ra8_fmt_transaction_ops_t k_tx_ops = {
  .append = mem_tx_append,
  .commit = mem_tx_commit,
  .abort  = mem_tx_abort,
};

static ra8_err_t spool_read_at(void* ctx, uint64_t off, uint8_t* buf, size_t len, size_t* got)
{
  const mem_spool_t* sp = (const mem_spool_t*)ctx;
  if (off >= sp->len) {
    *got = 0U;
    return k_ra8_ok;
  }
  size_t avail = sp->len - (size_t)off;
  if (avail > len) {
    avail = len;
  }
  if (avail > 0U) {
    memcpy(buf, &sp->buf[(size_t)off], avail);
  }
  *got = avail;
  return k_ra8_ok;
}

static ra8_err_t spool_append(void* ctx, const uint8_t* bytes, size_t len)
{
  mem_spool_t* sp = (mem_spool_t*)ctx;
  if ((sp->len + len) > sizeof(sp->buf)) {
    return k_ra8_err_no_mem;
  }
  if (len > 0U) {
    memcpy(&sp->buf[sp->len], bytes, len);
    sp->len += len;
  }
  return k_ra8_ok;
}

static ra8_err_t spool_seal(void* ctx, uint64_t expected_size)
{
  mem_spool_t* sp = (mem_spool_t*)ctx;
  if (expected_size != sp->len) {
    return k_ra8_err_invalid_size;
  }
  sp->sealed = true;
  return k_ra8_ok;
}

static void test_null_guards(void)
{
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_fmt_jof_inspect_stream(nullptr, false, nullptr, nullptr));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_fmt_rabook_inspect_stream(nullptr, false, nullptr, nullptr));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_fmt_jof_convert_requirements(nullptr, nullptr));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_fmt_jof_convert_stream(nullptr, nullptr, nullptr, nullptr, nullptr, nullptr));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_fmt_jof_verify_requirements(nullptr, nullptr));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_fmt_jof_verify_stream(nullptr,
                                           nullptr,
                                           nullptr,
                                           nullptr,
                                           nullptr,
                                           nullptr,
                                           nullptr,
                                           nullptr,
                                           nullptr));
}

static void test_inspect_and_rabook(void)
{
  uint8_t                atlas[68] = {};
  mem_src_t              src_ctx   = {.data = atlas, .len = sizeof(atlas)};
  const ra8_fmt_source_t src = {.read_at = mem_read_at, .ctx = &src_ctx, .size = sizeof(atlas)};
  jof_audit_record_t     records[2]   = {};
  uint8_t                tile[16]     = {};
  uint8_t                scratch[16]  = {};
  ra8_fmt_jof_inspect_workspace_t jws = {
    .records     = records,
    .record_cap  = 2U,
    .tile        = tile,
    .tile_cap    = sizeof(tile),
    .scratch     = scratch,
    .scratch_cap = sizeof(scratch),
  };
  mem_sink_t           sink_ctx = {};
  const ra8_fmt_sink_t sink     = {.write = mem_sink_write, .ctx = &sink_ctx};

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fmt_jof_inspect_stream(&src, true, &jws, &sink));
  TEST_ASSERT_NOT_NULL(strstr((const char*)sink_ctx.buf, "verdict: VALID"));

  uint8_t                rbkc[80] = {'R', 'B', 'K', 'C'};
  mem_src_t              rsrc_ctx = {.data = rbkc, .len = sizeof(rbkc)};
  const ra8_fmt_source_t rsrc = {.read_at = mem_read_at, .ctx = &rsrc_ctx, .size = sizeof(rbkc)};
  uint64_t               table[4]        = {};
  uint8_t                comp[32]        = {};
  uint8_t                chunk[32]       = {};
  uint8_t                rscr[32]        = {};
  ra8_fmt_rabook_inspect_workspace_t rws = {
    .table          = table,
    .table_cap      = 4U,
    .compressed     = comp,
    .compressed_cap = sizeof(comp),
    .chunk          = chunk,
    .chunk_cap      = sizeof(chunk),
    .scratch        = rscr,
    .scratch_cap    = sizeof(rscr),
  };
  memset(&sink_ctx, 0, sizeof(sink_ctx));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fmt_rabook_inspect_stream(&rsrc, true, &rws, &sink));
  TEST_ASSERT_NOT_NULL(strstr((const char*)sink_ctx.buf, "verdict: VALID"));
}

static void test_convert_and_verify(void)
{
  const uint8_t png[33] = {
    0x89U, 'P', 'N', 'G', 0x0DU, 0x0AU, 0x1AU, 0x0AU, 0U, 0U, 0U, 13U, 'I', 'H', 'D', 'R', 0U,
    0U,    0U,  8U,  0U,  0U,    0U,    4U,    8U,    2U, 0U, 0U, 0U,  0U,  0U,  0U,  0U,
  };
  mem_src_t              src_ctx = {.data = png, .len = sizeof(png)};
  const ra8_fmt_source_t src     = {.read_at = mem_read_at, .ctx = &src_ctx, .size = sizeof(png)};
  ra8_fmt_jof_convert_requirements_t creq = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fmt_jof_convert_requirements(&src, &creq));
  TEST_ASSERT_EQ(8U, creq.width);
  TEST_ASSERT_EQ(4U, creq.height);

  uint8_t                         work[512] = {};
  ra8_fmt_jof_convert_workspace_t cws       = {.work = work, .work_cap = sizeof(work)};
  mem_tx_t                        tx_ctx    = {};
  ra8_fmt_transaction_t           tx        = {.ops = &k_tx_ops, .ctx = &tx_ctx};
  mem_sink_t                      sink_ctx  = {};
  const ra8_fmt_sink_t            sink      = {.write = mem_sink_write, .ctx = &sink_ctx};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fmt_jof_convert_stream(&src, &creq, &cws, &tx, &sink, "demo.jof"));
  TEST_ASSERT(tx_ctx.committed);
  TEST_ASSERT_NOT_NULL(strstr((const char*)sink_ctx.buf, "-> demo.jof"));

  ra8_fmt_jof_verify_requirements_t vreq = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fmt_jof_verify_requirements(&src, &vreq));
  TEST_ASSERT_EQ(3U, vreq.bpp);
  TEST_ASSERT_EQ(24U, vreq.row_bytes);

  mem_src_t                      src_ref_ctx = {.data = png, .len = sizeof(png)};
  mem_src_t                      src_got_ctx = {.data = png, .len = sizeof(png)};
  const ra8_fmt_source_t         src_ref     = {.read_at = mem_read_at,
                                                .ctx     = &src_ref_ctx,
                                                .size    = sizeof(png)};
  const ra8_fmt_source_t         src_got     = {.read_at = mem_read_at,
                                                .ctx     = &src_got_ctx,
                                                .size    = sizeof(png)};
  uint8_t                        vband[512]  = {};
  uint8_t                        vscr[512]   = {};
  uint8_t                        vrow[128]   = {};
  ra8_fmt_jof_verify_workspace_t vws         = {
    .work          = work,
    .work_cap      = sizeof(work),
    .band_tile     = vband,
    .band_tile_cap = sizeof(vband),
    .scratch       = vscr,
    .scratch_cap   = sizeof(vscr),
    .row           = vrow,
    .row_cap       = sizeof(vrow),
  };
  mem_spool_t           sp1_ctx  = {};
  mem_spool_t           sp2_ctx  = {};
  ra8_fmt_spool_t       sp1      = {.read_at = spool_read_at,
                                    .append  = spool_append,
                                    .seal    = spool_seal,
                                    .ctx     = &sp1_ctx};
  ra8_fmt_spool_t       sp2      = {.read_at = spool_read_at,
                                    .append  = spool_append,
                                    .seal    = spool_seal,
                                    .ctx     = &sp2_ctx};
  mem_tx_t              dump_ctx = {};
  ra8_fmt_transaction_t dump     = {.ops = &k_tx_ops, .ctx = &dump_ctx};
  memset(&sink_ctx, 0, sizeof(sink_ctx));
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_fmt_jof_verify_stream(&src_ref,
                                           &src_got,
                                           &vreq,
                                           &vws,
                                           &sp1,
                                           &sp2,
                                           &dump,
                                           "out.ppm",
                                           &sink));
  TEST_ASSERT(dump_ctx.committed);
  TEST_ASSERT_NOT_NULL(strstr((const char*)sink_ctx.buf, "ROUND-TRIP EXACT"));
}

int main(void)
{
  TEST_BEGIN("test_null_guards");
  test_null_guards();
  TEST_END("test_null_guards");

  TEST_BEGIN("test_inspect_and_rabook");
  test_inspect_and_rabook();
  TEST_END("test_inspect_and_rabook");

  TEST_BEGIN("test_convert_and_verify");
  test_convert_and_verify();
  TEST_END("test_convert_and_verify");
  return 0;
}
