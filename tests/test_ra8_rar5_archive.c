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

/** @brief RAR main-archive-header layout used by the fixture builder. */
typedef enum : uint8_t {
  k_av_off_high_pos_av = 7U, /**< HighPosAV + PosAV field offset.   */
  k_av_len_high_pos_av = 6U, /**< HighPosAV + PosAV combined width. */
} av_layout_t;

/**
 * @enum t_rar4_off_t
 * @brief Byte offsets of the RAR4 block-header fields the builder writes.
 *
 * @details
 * The archive-level tests craft RAR4 blocks by hand so a single field can be
 * made inconsistent. The layout is fixed: CRC, type, flags, size, then the
 * per-type fields at the offsets below.
 */
typedef enum : uint8_t {
  k_t_r4_off_head_size = 5U,  /**< HEAD_SIZE, 16-bit.               */
  k_t_r4_off_pack_size = 7U,  /**< PACK_SIZE, 32-bit.               */
  k_t_r4_off_unp_size  = 11U, /**< UNP_SIZE, 32-bit.                */
  k_t_r4_off_host_os   = 15U, /**< HOST_OS, 8-bit.                  */
  k_t_r4_off_ftime     = 20U, /**< FTIME, 32-bit MS-DOS timestamp.  */
  k_t_r4_off_unp_ver   = 24U, /**< UNP_VER, 8-bit.                  */
  k_t_r4_off_method    = 25U, /**< METHOD, 8-bit.                   */
  k_t_r4_off_name_size = 26U, /**< NAME_SIZE, 16-bit.               */
  k_t_r4_off_attr      = 28U, /**< ATTR, 32-bit.                    */
} t_rar4_off_t;

/**
 * @enum t_rar4_field_t
 * @brief RAR4 block types and field values the fixtures declare.
 */
typedef enum : uint16_t {
  k_t_r4_type_main    = 0x73U,   /**< Block type: main archive header.       */
  k_t_r4_type_file    = 0x74U,   /**< Block type: file header.               */
  k_t_r4_flag_long    = 0x8000U, /**< LONG_BLOCK: an ADD_SIZE field follows. */
  k_t_r4_unp_ver_20   = 20U,     /**< UNP_VER 20: RAR 2.0 stream format.     */
  k_t_r4_method_comp  = 0x33U,   /**< METHOD '3': compressed, which the
                                      stored-only extractor must refuse.     */
  k_t_r4_main_hdr_len = 13U,     /**< Total main-header length, bytes.       */
} t_rar4_field_t;

/**
 * @enum t_vint_t
 * @brief RAR5 header types and base-128 vint encoding parameters.
 */
typedef enum : uint8_t {
  k_t_r5_type_end       = 5U,    /**< RAR5 header type 5: end of archive.    */
  k_t_vint_payload_mask = 0x7FU, /**< Payload bits carried by one vint byte. */
  k_t_vint_shift        = 7U,    /**< Bits consumed per vint byte.           */
  k_t_vint_more_flag    = 0x80U, /**< Continuation bit: another byte follows. */
  k_t_le32_hi_shift     = 24U,   /**< Shift for the top byte of a 32-bit LE field. */
} t_vint_t;

/**
 * @enum t_fixture_t
 * @brief Scratch capacities the archive fixtures allocate.
 */
typedef enum : uint16_t {
  k_t_hdr_body_cap = 256U,  /**< RAR5 header-body scratch, bytes.      */
  k_t_name_arena   = 1024U, /**< Entry-name arena, bytes.              */
  k_t_extract_buf  = 64U,   /**< Extraction destination buffer, bytes. */
} t_fixture_t;

/* ---- CBR facade: compressed-page parity + RAR4 unsupported --------------- */

/** @brief Encode one RAR5 vint; return its byte length. */
static size_t av_vint(uint8_t* out, uint64_t v)
{
  size_t i = 0U;
  for (;;) {
    uint8_t b = (uint8_t)(v & k_t_vint_payload_mask);
    v >>= k_t_vint_shift;
    if (v != 0U) {
      out[i] = (uint8_t)(b | k_t_vint_more_flag);
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
  uint8_t        body[k_t_hdr_body_cap] = {};
  size_t         b                      = 0U;
  const bool     has_data               = (dlen > 0U);
  const uint64_t hflags                 = has_data ? 0x02U : 0x00U;
  const uint64_t cinfo                  = (uint64_t)method << 7U; /* method occupies bits 7..9 */
  b += av_vint(&body[b], 2U);                                     /* header type = file        */
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
  /* The RAR5 file-header name is a COUNTED field: the vint length written on
   * the line above is what the parser reads, and the name bytes that follow
   * carry no terminator. Copying strlen(name) + 1 here would push a stray NUL
   * into the next header field and corrupt the fixture, so the unterminated
   * copy is the format-correct one. */
  // NOLINTNEXTLINE(bugprone-not-null-terminated-result)
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
static char             s_fnames[k_t_name_arena];

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
  p += av5_simple(&s_farc[p], k_t_r5_type_end);
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
  p[3] = (uint8_t)(v >> k_t_le32_hi_shift);
}

/** @brief Encode a RAR4 file block with an arbitrary METHOD byte. */
static size_t
a4_file(uint8_t* out, const char* name, const uint8_t* data, size_t dlen, uint8_t method)
{
  const size_t nlen = strlen(name);
  const size_t head = 32U + nlen;
  a4_le16(&out[0], 0U);
  out[2] = k_t_r4_type_file;          /* type: file  */
  a4_le16(&out[3], k_t_r4_flag_long); /* flags: LONG */
  a4_le16(&out[k_t_r4_off_head_size], (uint16_t)head);
  a4_le32(&out[k_t_r4_off_pack_size], (uint32_t)dlen);
  a4_le32(&out[k_t_r4_off_unp_size], (uint32_t)dlen);
  out[k_t_r4_off_host_os] = 0U;
  a4_le32(&out[16], 0U);
  a4_le32(&out[k_t_r4_off_ftime], 0U);
  out[k_t_r4_off_unp_ver] = k_t_r4_unp_ver_20;
  out[k_t_r4_off_method]  = method; /* 0x30 = store; anything else = compressed */
  a4_le16(&out[k_t_r4_off_name_size], (uint16_t)nlen);
  a4_le32(&out[k_t_r4_off_attr], 0U);
  memcpy(&out[32], name, nlen);
  memcpy(&out[head], data, dlen);
  return head + dlen;
}

/** @brief Encode the RAR4 main block. */
static size_t a4_main(uint8_t* out)
{
  a4_le16(&out[0], 0U);
  out[2] = k_t_r4_type_main;
  a4_le16(&out[3], 0U);
  a4_le16(&out[k_t_r4_off_head_size], k_t_r4_main_hdr_len);
  memset(&out[k_av_off_high_pos_av], 0, k_av_len_high_pos_av);
  return k_t_r4_main_hdr_len;
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
               k_t_r4_method_comp); /* method 0x33 = compressed */
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
  static uint8_t s_obuf[k_t_extract_buf];
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
  static uint8_t s_obuf[k_t_extract_buf];
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
