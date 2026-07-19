/**
 * @file test_ra8_rar5_archive.c
 * @brief CBR facade tests: RAR5 compressed-page parity + RAR4 unsupported.
 *
 * @details
 * Split out of test_ra8_rar5.c to keep each test translation unit under the
 * repository file-size cap. This sibling owns the archive-level tests: a
 * hand-assembled RAR5 archive whose compressed page decodes via the
 * ra8_comic facade to the same bytes as the equivalent STORE page (the
 * CBZ-parity acceptance for #235), a RAR4 archive whose compressed member
 * stays unsupported, and the ra8_rar_extract dispatch guards. The shared
 * RAR5 writer fixture is tests/support/rar5_enc_fixture.h. Tests are
 * magic-number exempt, so byte offsets and bit widths appear as literals.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "comic_fixture.h"
#include "ra8_comic.h"
#include "ra8_err.h"
#include "ra8_rar.h"
#include "ra8_rar5.h"
#include "ra8_rar5_internal.h"
#include "support/rar5_enc_fixture.h"
#include "unity_minimal.h"

/**
 * @enum rar5_archive_uint8_const_t
 * @brief Named uint8_t constants used by this file.
 *
 * @details
 * Every literal this translation unit needs, named so the
 * value's role is visible at the point of use (CLAUDE.md
 * "No Magic Numbers").
 */
typedef enum : uint8_t {
  k_rar5_archive_a4_file_33   = 0x33U,
  k_rar5_archive_a4_le16_13   = 13U,
  k_rar5_archive_a4_le16_26   = 26,
  k_rar5_archive_a4_le16_5    = 5,
  k_rar5_archive_a4_le32_11   = 11,
  k_rar5_archive_a4_le32_20   = 20,
  k_rar5_archive_a4_le32_28   = 28,
  k_rar5_archive_a4_le32_7    = 7,
  k_rar5_archive_av5_simple_5 = 5U,
  k_rar5_archive_b_80         = 0x80U,
  k_rar5_archive_out_20       = 20U,
  k_rar5_archive_out_73       = 0x73U,
  k_rar5_archive_out_74       = 0x74U,
  k_rar5_archive_v_24         = 24,
  k_rar5_archive_v_7f         = 0x7FU,
  k_rar5_archive_val_15       = 15,
  k_rar5_archive_val_25       = 25,
  k_rar5_archive_val_64       = 64,
  k_rar5_archive_val_7        = 7U,
} rar5_archive_uint8_const_t;

/**
 * @enum rar5_archive_uint16_const_t
 * @brief Named uint16_t constants used by this file.
 *
 * @details
 * Every literal this translation unit needs, named so the
 * value's role is visible at the point of use (CLAUDE.md
 * "No Magic Numbers").
 */
typedef enum : uint16_t {
  k_rar5_archive_a4_le16_8000 = 0x8000U,
  k_rar5_archive_val_1024     = 1024,
  k_rar5_archive_val_256      = 256,
} rar5_archive_uint16_const_t;

/* ---- CBR facade: compressed-page parity + RAR4 unsupported --------------- */

/** @brief Encode one RAR5 vint; return its byte length. */
static size_t av_vint(uint8_t* out, uint64_t v)
{
  size_t i = 0U;
  for (;;) {
    uint8_t b = (uint8_t)(v & k_rar5_archive_v_7f);
    v >>= k_rar5_archive_val_7;
    if (v != 0U) {
      out[i] = (uint8_t)(b | k_rar5_archive_b_80);
      i++;
    } else {
      out[i] = b;
      i++;
      break;
    }
  }
  return i;
}

/** @brief Encode a RAR5 file block (STORE if @p method 0, else compressed). */
static size_t av5_file(uint8_t*       out,
                       const char*    name,
                       const uint8_t* data,
                       size_t         dlen,
                       uint64_t       unp,
                       uint32_t       method)
{
  uint8_t        body[k_rar5_archive_val_256] = {};
  size_t         b                            = 0U;
  const bool     has_data                     = (dlen > 0U);
  const uint64_t hflags                       = has_data ? 0x02U : 0x00U;
  const uint64_t cinfo = (uint64_t)method << 7U; /* method occupies bits 7..9 */
  b += av_vint(&body[b], 2U);                    /* header type = file        */
  b += av_vint(&body[b], hflags);
  if (has_data) {
    b += av_vint(&body[b], (uint64_t)dlen);
  }
  b += av_vint(&body[b], 0U);  /* file flags    */
  b += av_vint(&body[b], unp); /* unpacked size */
  b += av_vint(&body[b], 0U);  /* attributes    */
  b += av_vint(&body[b], cinfo);
  b += av_vint(&body[b], 0U); /* host os */
  const size_t nlen = strlen(name);
  b += av_vint(&body[b], (uint64_t)nlen);
  memcpy(&body[b], name, nlen);
  b += nlen;
  size_t p = 4U; /* header crc (unverified) */
  p += av_vint(&out[p], (uint64_t)b);
  memcpy(&out[p], body, b);
  p += b;
  if (has_data) {
    memcpy(&out[p], data, dlen);
    p += dlen;
  }
  return p;
}

/** @brief Encode a RAR5 main/end block (@p htype 1 or 5). */
static size_t av5_simple(uint8_t* out, uint64_t htype)
{
  uint8_t body[8] = {};
  size_t  b       = 0U;
  b += av_vint(&body[b], htype);
  b += av_vint(&body[b], 0U);
  b += av_vint(&body[b], 0U);
  size_t p = 4U;
  p += av_vint(&out[p], (uint64_t)b);
  memcpy(&out[p], body, b);
  p += b;
  return p;
}

/** @brief Comic page/name/archive buffers for the facade tests. */
static uint8_t          s_farc[k_arc_cap];
static size_t           s_farc_len;
static ra8_comic_page_t s_fpages[16];
static char             s_fnames[k_rar5_archive_val_1024];

/** @brief Read callback over ::s_farc. */
static size_t farc_read(void* ctx, uint64_t off, void* dst, size_t len)
{
  (void)ctx;
  buf_src_t s = {.data = s_farc, .len = s_farc_len};
  return buf_read(&s, off, dst, len);
}

/**
 * @test test_cbr_compressed_parity
 * @brief A `.cbr` whose page is RAR5-compressed opens by magic and decodes to the
 *        exact bytes of the equivalent STORE page (the #235 CBZ-parity acceptance).
 *
 * @par MC/DC:
 * Decision libs/ra8_comic/src/ra8_comic_cbr.c@s_add_member:
 * `is_store || is_rar5` (2 conditions)
 * - STORE page on RAR5:      is_store=1              -> extractable (control)
 * - compressed page on RAR5: is_store=0, is_rar5=1   -> extractable (varies is_rar5)
 * The RAR4-compressed case (both false -> not extractable) is covered by
 * test_cbr_rar4_compressed_unsupported; together they give the N+1 vectors.
 */
static void test_cbr_compressed_parity(void)
{
  TEST_BEGIN("comic cbr: RAR5-compressed page == STORE page (parity)");
  /* One PNG page; the STORE and compressed archives must decode to it identically. */
  static uint8_t s_png[k_cf_png_max];
  const size_t   plen = cf_make_png(7U, 5U, 9U, s_png, sizeof(s_png));
  TEST_ASSERT(plen > 0U);
  static uint8_t s_packed[k_pk_cap];
  memset(s_packed, 0, sizeof(s_packed));
  const size_t pklen = enc_all_literal(s_png, plen, s_packed, sizeof(s_packed));

  static const uint8_t k_sig[8] = {0x52U, 0x61U, 0x72U, 0x21U, 0x1AU, 0x07U, 0x01U, 0x00U};
  size_t               p        = 0U;
  memcpy(&s_farc[p], k_sig, sizeof(k_sig));
  p += sizeof(k_sig);
  p += av5_simple(&s_farc[p], 1U);
  p += av5_file(&s_farc[p], "page.png", s_packed, pklen, (uint64_t)plen, 1U); /* compressed */
  p += av5_simple(&s_farc[p], k_rar5_archive_av5_simple_5);
  s_farc_len = p;

  ra8_comic_t c = {};
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_comic_open(&c,
                                farc_read,
                                nullptr,
                                (uint64_t)s_farc_len,
                                s_fpages,
                                (uint32_t)(sizeof(s_fpages) / sizeof(s_fpages[0])),
                                s_fnames,
                                (uint32_t)sizeof(s_fnames)));
  TEST_ASSERT_EQ(k_ra8_comic_kind_cbr, ra8_comic_kind(&c));
  TEST_ASSERT_EQ(1U, ra8_comic_page_count(&c));
  uint16_t nl  = 0U;
  uint64_t raw = 0U;
  uint8_t  ex  = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_comic_page_info(&c, 0U, nullptr, 0U, &nl, &raw, &ex));
  TEST_ASSERT_EQ(1U, ex); /* compressed RAR5 page is extractable */
  TEST_ASSERT_EQ(plen, raw);

  static uint8_t s_obuf[k_cf_png_max];
  memset(s_obuf, 0, sizeof(s_obuf));
  size_t got = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_comic_page_read(&c, 0U, s_obuf, sizeof(s_obuf), &got));
  TEST_ASSERT_EQ(plen, got);
  TEST_ASSERT_EQ(0, memcmp(s_obuf, s_png, plen)); /* byte-parity with the STORE page */
  TEST_ASSERT(cf_decode_ok(s_obuf, got, 7, 5));   /* and decodes to real pixels      */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_comic_close(&c));
  TEST_END("comic cbr: RAR5-compressed page == STORE page (parity)");
}

/* ---- RAR4 compressed member: unsupported (extractable == 0) -------------- */

/** @brief Write a little-endian uint16. */
static void a4_le16(uint8_t* p, uint16_t v)
{
  p[0] = (uint8_t)v;
  p[1] = (uint8_t)(v >> 8);
}

/** @brief Write a little-endian uint32. */
static void a4_le32(uint8_t* p, uint32_t v)
{
  p[0] = (uint8_t)v;
  p[1] = (uint8_t)(v >> 8);
  p[2] = (uint8_t)(v >> 16);
  p[3] = (uint8_t)(v >> k_rar5_archive_v_24);
}

/** @brief Encode a RAR4 file block with an arbitrary METHOD byte. */
static size_t
a4_file(uint8_t* out, const char* name, const uint8_t* data, size_t dlen, uint8_t method)
{
  const size_t nlen = strlen(name);
  const size_t head = 32U + nlen;
  a4_le16(&out[0], 0U);
  out[2] = k_rar5_archive_out_74;                /* type: file  */
  a4_le16(&out[3], k_rar5_archive_a4_le16_8000); /* flags: LONG */
  a4_le16(&out[k_rar5_archive_a4_le16_5], (uint16_t)head);
  a4_le32(&out[k_rar5_archive_a4_le32_7], (uint32_t)dlen);
  a4_le32(&out[k_rar5_archive_a4_le32_11], (uint32_t)dlen);
  out[k_rar5_archive_val_15] = 0U;
  a4_le32(&out[16], 0U);
  a4_le32(&out[k_rar5_archive_a4_le32_20], 0U);
  out[k_rar5_archive_v_24]   = k_rar5_archive_out_20;
  out[k_rar5_archive_val_25] = method; /* 0x30 = store; anything else = compressed */
  a4_le16(&out[k_rar5_archive_a4_le16_26], (uint16_t)nlen);
  a4_le32(&out[k_rar5_archive_a4_le32_28], 0U);
  memcpy(&out[32], name, nlen);
  memcpy(&out[head], data, dlen);
  return head + dlen;
}

/** @brief Encode the RAR4 main block. */
static size_t a4_main(uint8_t* out)
{
  a4_le16(&out[0], 0U);
  out[2] = k_rar5_archive_out_73;
  a4_le16(&out[3], 0U);
  a4_le16(&out[k_rar5_archive_a4_le16_5], k_rar5_archive_a4_le16_13);
  memset(&out[7], 0, 6U);
  return k_rar5_archive_a4_le16_13;
}

/**
 * @test test_cbr_rar4_compressed_unsupported
 * @brief A RAR4-compressed page is indexed but non-extractable; page_read reports
 *        it unsupported (the RAR4 codec is out of scope).
 *
 * @par MC/DC:
 * Completes the libs/ra8_comic/src/ra8_comic_cbr.c@s_add_member `is_store || is_rar5`
 * vector set from test_cbr_compressed_parity:
 * - RAR4 compressed page: is_store=0, is_rar5=0 -> not extractable (both false).
 */
static void test_cbr_rar4_compressed_unsupported(void)
{
  TEST_BEGIN("comic cbr: RAR4-compressed page unsupported");
  static const uint8_t k_sig[7]  = {0x52U, 0x61U, 0x72U, 0x21U, 0x1AU, 0x07U, 0x00U};
  static const uint8_t k_data[8] = {1U, 2U, 3U, 4U, 5U, 6U, 7U, 8U};
  size_t               p         = 0U;
  memcpy(&s_farc[p], k_sig, sizeof(k_sig));
  p += sizeof(k_sig);
  p += a4_main(&s_farc[p]);
  p += a4_file(&s_farc[p],
               "page.png",
               k_data,
               sizeof(k_data),
               k_rar5_archive_a4_file_33); /* method 0x33 = compressed */
  s_farc_len = p;

  ra8_comic_t c = {};
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_comic_open(&c,
                                farc_read,
                                nullptr,
                                (uint64_t)s_farc_len,
                                s_fpages,
                                (uint32_t)(sizeof(s_fpages) / sizeof(s_fpages[0])),
                                s_fnames,
                                (uint32_t)sizeof(s_fnames)));
  TEST_ASSERT_EQ(k_ra8_comic_kind_cbr, ra8_comic_kind(&c));
  TEST_ASSERT_EQ(1U, ra8_comic_page_count(&c));
  uint16_t nl  = 0U;
  uint64_t raw = 0U;
  uint8_t  ex  = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_comic_page_info(&c, 0U, nullptr, 0U, &nl, &raw, &ex));
  TEST_ASSERT_EQ(0U, ex); /* RAR4-compressed: not extractable */
  static uint8_t s_obuf[k_rar5_archive_val_64];
  size_t         got = 0U;
  TEST_ASSERT_EQ(k_ra8_err_not_supported,
                 ra8_comic_page_read(&c, 0U, s_obuf, sizeof(s_obuf), &got));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_comic_close(&c));
  TEST_END("comic cbr: RAR4-compressed page unsupported");
}

/** @brief Unopened-archive and NULL-argument guard legs of the extract dispatch. */
static void extract_dispatch_guards(const ra8_rar_t*       rar,
                                    const ra8_rar_entry_t* store,
                                    uint8_t*               obuf,
                                    size_t                 obuf_len)
{
  size_t got = 0U;

  /* Unopened archive -> invalid_state. */
  ra8_rar_t none = *rar;
  none.version   = k_ra8_rar_ver_none;
  TEST_ASSERT_EQ(k_ra8_err_invalid_state,
                 ra8_rar_extract(&none, store, obuf, obuf_len, &s_state, &got));

  /* NULL argument guards. */
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_rar_extract(nullptr, store, obuf, obuf_len, &s_state, &got));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_rar_extract(rar, nullptr, obuf, obuf_len, &s_state, &got));
}

/**
 * @test test_rar_extract_dispatch
 * @brief `ra8_rar_extract` routes STORE by copy, decodes RAR5-compressed, and
 *        rejects non-files, directories, RAR4-compressed, and a missing scratch.
 *
 * @par MC/DC:
 * Decision libs/ra8_comic/src/ra8_rar.c@ra8_rar_extract:
 * `ent->is_file == 0 || ent->is_dir != 0` (2 conditions)
 * - is_file=1, is_dir=0 -> false (control: a real file proceeds)
 * - is_file=0, is_dir=0 -> true  (varies is_file: non-file rejected)
 * - is_file=1, is_dir=1 -> true  (varies is_dir: directory rejected)
 * The first two prove is_file independently rejects; the first and third prove the
 * same for is_dir. N+1 = 3 vectors for N=2 conditions.
 */
static void test_rar_extract_dispatch(void)
{
  TEST_BEGIN("rar: extract dispatch (store / compressed / guards)");
  static const uint8_t k_bytes[16] =
    {10U, 11U, 12U, 13U, 14U, 15U, 16U, 17U, 18U, 19U, 20U, 21U, 22U, 23U, 24U, 25U};
  buf_src_t      src = {.data = k_bytes, .len = sizeof(k_bytes)};
  ra8_rar_t      rar = {.read      = buf_read,
                        .ctx       = &src,
                        .size      = (uint64_t)sizeof(k_bytes),
                        .first_off = 0U,
                        .version   = k_ra8_rar_ver_5};
  static uint8_t s_obuf[k_rar5_archive_val_64];
  size_t         got = 0U;

  /* STORE member -> copy (control: is_file true, is_dir false). */
  ra8_rar_entry_t store = {.is_file   = 1U,
                           .is_dir    = 0U,
                           .method    = (uint8_t)k_ra8_rar_method_store,
                           .data_off  = 0U,
                           .pack_size = 8U,
                           .unp_size  = 8U};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_rar_extract(&rar, &store, s_obuf, sizeof(s_obuf), &s_state, &got));
  TEST_ASSERT_EQ(8U, got);
  TEST_ASSERT_EQ(0, memcmp(s_obuf, k_bytes, 8U));

  /* Non-file entry -> unsupported (varies is_file). */
  ra8_rar_entry_t nonfile = store;
  nonfile.is_file         = 0U;
  TEST_ASSERT_EQ(k_ra8_err_not_supported,
                 ra8_rar_extract(&rar, &nonfile, s_obuf, sizeof(s_obuf), &s_state, &got));

  /* Directory entry -> unsupported (varies is_dir). */
  ra8_rar_entry_t dir = store;
  dir.is_dir          = 1U;
  TEST_ASSERT_EQ(k_ra8_err_not_supported,
                 ra8_rar_extract(&rar, &dir, s_obuf, sizeof(s_obuf), &s_state, &got));

  /* Compressed member with NULL scratch -> null_ptr. */
  ra8_rar_entry_t comp = store;
  comp.method          = (uint8_t)k_ra8_rar_method_compressed;
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_rar_extract(&rar, &comp, s_obuf, sizeof(s_obuf), nullptr, &got));

  /* RAR4-compressed member -> unsupported (legacy codec out of scope). */
  ra8_rar_t r4 = rar;
  r4.version   = k_ra8_rar_ver_4;
  TEST_ASSERT_EQ(k_ra8_err_not_supported,
                 ra8_rar_extract(&r4, &comp, s_obuf, sizeof(s_obuf), &s_state, &got));

  extract_dispatch_guards(&rar, &store, s_obuf, sizeof(s_obuf));
  TEST_END("rar: extract dispatch (store / compressed / guards)");
}

int32_t main(void)
{
  test_rar_extract_dispatch();
  test_cbr_compressed_parity();
  test_cbr_rar4_compressed_unsupported();
  (void)fprintf(stderr, "[OK  ] test_ra8_rar5_archive.c\n");
  return 0;
}
