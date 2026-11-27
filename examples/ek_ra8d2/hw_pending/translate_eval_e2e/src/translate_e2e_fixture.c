/**
 * @file translate_e2e_fixture.c
 * @brief Deterministic binary fixture and mini-TPAK builders for `translate_eval_e2e`.
 *
 * @details
 * Constructs validated `DetMap`, `TCHR`, `TVOC`, `TMNT`, and `TPAK` v1 binary
 * containers in caller-provided buffers with zero dynamic memory allocation.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>
#include <string.h>

#include "translate_e2e.h"

/** @brief Local constants for CRC-32 and binary container builders. */
typedef enum : uint32_t {
  k_fix_crc_init         = 0xFFFFFFFFU,
  k_fix_crc_poly         = 0xEDB88320U,
  k_fix_tvoc_magic       = 0x434F5654U,
  k_fix_tvoc_version     = 1U,
  k_fix_tvoc_header      = 56U,
  k_fix_tvoc_rec_bytes   = 12U,
  k_fix_tmnt_d_model     = 16U,
  k_fix_tmnt_d_ff        = 16U,
  k_fix_tmnt_max_seq     = 16U,
  k_fix_tmnt_slots       = 50U,
  k_fix_tmnt_dir_bytes   = 84U + (50U * 24U),
  k_fix_max_vocab_pieces = 48U,
  k_fix_max_tgt_tokens   = 16U,
  k_fix_max_glyphs       = 48U,
  k_fix_max_ctc_seq      = 96U,
  k_fix_max_piece_bytes  = 96U,
} te2e_fix_const_t;

typedef struct {
  char    text[k_fix_max_piece_bytes];
  size_t  len;
  float   score;
  uint8_t kind;
} te2e_vocab_piece_t;

typedef struct {
  uint8_t       dtype;
  uint32_t      rows;
  uint32_t      cols;
  float         scale;
  const int8_t* i8_vals;
  const float*  f32_vals;
} te2e_tensor_spec_t;

static const float  k_fix_det_scale = 0.013F;
static const int8_t k_fix_det_zp    = -128;
static const float  k_fix_ocr_scale = 0.030F;
static const float  k_fix_lm_scale  = 0.080F;

static void fix_write_u16(uint8_t* dst, size_t off, uint16_t val)
{
  dst[off + 0U] = (uint8_t)(val & 0xFFU);
  dst[off + 1U] = (uint8_t)((val >> 8U) & 0xFFU);
}

static void fix_write_u32(uint8_t* dst, size_t off, uint32_t val)
{
  dst[off + 0U] = (uint8_t)(val & 0xFFU);
  dst[off + 1U] = (uint8_t)((val >> 8U) & 0xFFU);
  dst[off + 2U] = (uint8_t)((val >> 16U) & 0xFFU);
  dst[off + 3U] = (uint8_t)((val >> 24U) & 0xFFU);
}

static void fix_write_f32(uint8_t* dst, size_t off, float val)
{
  uint32_t bits = 0U;
  memcpy(&bits, &val, sizeof(bits));
  fix_write_u32(dst, off, bits);
}

static int8_t fix_quantize_i8(float val, float scale, int8_t zp)
{
  const float   ratio   = val / scale;
  const int32_t rnd     = (ratio >= 0.0F) ? (int32_t)(ratio + 0.5F) : (int32_t)(ratio - 0.5F);
  int32_t       clamped = rnd + (int32_t)zp;
  if (clamped < -128) {
    clamped = -128;
  } else if (clamped > 127) {
    clamped = 127;
  }
  return (int8_t)clamped;
}

uint32_t translate_e2e_crc32(const uint8_t* bytes, size_t len)
{
  return translate_e2e_crc32_zero_win(bytes, len, 0U, 0U);
}

uint32_t
translate_e2e_crc32_zero_win(const uint8_t* bytes, size_t len, size_t zero_start, size_t zero_len)
{
  uint32_t     crc      = (uint32_t)k_fix_crc_init;
  const size_t zero_end = zero_start + zero_len;
  for (size_t i = 0U; i < len; ++i) {
    const uint8_t b = ((zero_len > 0U) && (i >= zero_start) && (i < zero_end)) ? 0U : bytes[i];
    crc ^= (uint32_t)b;
    for (uint32_t bit = 0U; bit < 8U; ++bit) {
      const uint32_t mask = ((crc & 1U) != 0U) ? (uint32_t)k_fix_crc_poly : 0U;
      crc                 = (crc >> 1U) ^ mask;
    }
  }
  return ~crc;
}

static size_t fix_utf8_char_len(uint8_t lead)
{
  if ((lead & 0x80U) == 0U) {
    return 1U;
  }
  if ((lead & 0xE0U) == 0xC0U) {
    return 2U;
  }
  if ((lead & 0xF0U) == 0xE0U) {
    return 3U;
  }
  return 4U;
}

static uint32_t fix_find_or_add_glyph(uint8_t        glyphs[][8],
                                      size_t         glyph_lens[],
                                      uint32_t*      glyph_count,
                                      const uint8_t* g,
                                      size_t         glen)
{
  for (uint32_t i = 0U; i < *glyph_count; ++i) {
    if ((glyph_lens[i] == glen) && (memcmp(glyphs[i], g, glen) == 0)) {
      return i;
    }
  }
  if ((*glyph_count >= (uint32_t)k_fix_max_glyphs) || (glen > 8U)) {
    return UINT32_MAX;
  }
  const uint32_t id = *glyph_count;
  if (glen > 0U) {
    memcpy(glyphs[id], g, glen);
  }
  glyph_lens[id] = glen;
  *glyph_count   = id + 1U;
  return id;
}

static ra8_err_t fix_write_tchr_header(uint8_t*      tchr_buf,
                                       const uint8_t glyphs[][8],
                                       const size_t  glyph_lens[],
                                       uint32_t      class_count,
                                       size_t        pool_bytes,
                                       size_t*       out_tchr_len)
{
  const size_t offsets_bytes = ((size_t)class_count + 1U) * 4U;
  const size_t pool_off      = (size_t)k_ra8_translate_ctc_header_bytes + offsets_bytes;
  const size_t total         = pool_off + pool_bytes;
  if (total > (size_t)k_te2e_tchr_cap) {
    return k_ra8_err_invalid_size;
  }
  memset(tchr_buf, 0, total);
  size_t cursor = 0U;
  for (uint32_t gi = 0U; gi < class_count; ++gi) {
    if (glyph_lens[gi] > 0U) {
      memcpy(&tchr_buf[pool_off + cursor], glyphs[gi], glyph_lens[gi]);
    }
    cursor += glyph_lens[gi];
    fix_write_u32(tchr_buf,
                  (size_t)k_ra8_translate_ctc_header_bytes + (((size_t)gi + 1U) * 4U),
                  (uint32_t)cursor);
  }
  fix_write_u32(tchr_buf, 0U, (uint32_t)k_ra8_translate_ctc_magic);
  fix_write_u16(tchr_buf, 4U, (uint16_t)k_ra8_translate_ctc_version);
  fix_write_u32(tchr_buf, 8U, (uint32_t)total);
  tchr_buf[16U] = (uint8_t)k_ra8_translate_ocr_japanese;
  fix_write_u32(tchr_buf, 20U, class_count);
  fix_write_u32(tchr_buf, 28U, (uint32_t)k_ra8_translate_ctc_index_none);
  fix_write_u32(tchr_buf, 32U, (uint32_t)pool_bytes);
  fix_write_u32(tchr_buf, 12U, translate_e2e_crc32(tchr_buf, total));
  *out_tchr_len = total;
  return k_ra8_ok;
}

static ra8_err_t fix_build_ocr(const char* src_utf8, translate_e2e_bubble_pack_t* out_b)
{
  uint8_t      glyphs[k_fix_max_glyphs][8]  = {};
  size_t       glyph_lens[k_fix_max_glyphs] = {};
  uint32_t     class_count                  = 1U; /* Class 0 = CTC blank "". */
  uint32_t     seq[k_fix_max_ctc_seq]       = {};
  size_t       seq_len                      = 0U;
  const size_t slen                         = strlen(src_utf8);
  size_t       pos                          = 0U;
  uint32_t     prev_cls                     = UINT32_MAX;
  while (pos < slen) {
    const size_t   clen = fix_utf8_char_len((uint8_t)src_utf8[pos]);
    const uint32_t cls =
      fix_find_or_add_glyph(glyphs, glyph_lens, &class_count, (const uint8_t*)&src_utf8[pos], clen);
    if ((cls == UINT32_MAX) || (seq_len + 2U > (size_t)k_fix_max_ctc_seq)) {
      return k_ra8_err_invalid_size;
    }
    if ((seq_len > 0U) && (cls == prev_cls)) {
      seq[seq_len++] = 0U;
    }
    seq[seq_len++] = cls;
    prev_cls       = cls;
    pos += clen;
  }
  size_t pool_bytes = 0U;
  for (uint32_t gi = 0U; gi < class_count; ++gi) {
    pool_bytes += glyph_lens[gi];
  }
  ra8_err_t err = fix_write_tchr_header(out_b->tchr_buf,
                                        glyphs,
                                        glyph_lens,
                                        class_count,
                                        pool_bytes,
                                        &out_b->tchr_len);
  if (err != k_ra8_ok) {
    return err;
  }
  const size_t total_logits = seq_len * (size_t)class_count;
  if (total_logits > (size_t)k_te2e_logits_cap) {
    return k_ra8_err_invalid_size;
  }
  const int8_t lo_q = fix_quantize_i8(-1.0F, k_fix_ocr_scale, 0);
  const int8_t hi_q = fix_quantize_i8(0.504F, k_fix_ocr_scale, 0);
  for (size_t i = 0U; i < total_logits; ++i) {
    out_b->logits_i8[i] = lo_q;
  }
  for (size_t t = 0U; t < seq_len; ++t) {
    out_b->logits_i8[(t * (size_t)class_count) + seq[t]] = hi_q;
  }
  out_b->logits_len = total_logits;
  out_b->time_steps = (uint32_t)seq_len;
  out_b->ocr_scale  = k_fix_ocr_scale;
  out_b->ocr_zp     = 0;
  return k_ra8_ok;
}

static bool
fix_append_sp_escaped(char* dst, size_t cap, size_t* io_len, const char* src, size_t slen)
{
  static const char k_sp_space[] = "\xE2\x96\x81";
  size_t            pos          = *io_len;
  for (size_t i = 0U; i < slen; ++i) {
    if (src[i] == ' ') {
      if ((pos + 3U) >= cap) {
        return false;
      }
      memcpy(&dst[pos], k_sp_space, 3U);
      pos += 3U;
    } else {
      if ((pos + 1U) >= cap) {
        return false;
      }
      dst[pos++] = src[i];
    }
  }
  dst[pos] = '\0';
  *io_len  = pos;
  return true;
}

static uint32_t fix_add_piece(te2e_vocab_piece_t* pieces,
                              uint32_t*           count,
                              const char*         text,
                              size_t              len,
                              float               score,
                              uint8_t             kind)
{
  for (uint32_t i = 0U; i < *count; ++i) {
    if ((pieces[i].len == len) && (memcmp(pieces[i].text, text, len) == 0)) {
      return i;
    }
  }
  if ((*count >= (uint32_t)k_fix_max_vocab_pieces) || (len >= (size_t)k_fix_max_piece_bytes)) {
    return UINT32_MAX;
  }
  const uint32_t id = *count;
  memcpy(pieces[id].text, text, len);
  pieces[id].text[len] = '\0';
  pieces[id].len       = len;
  pieces[id].score     = score;
  pieces[id].kind      = kind;
  *count               = id + 1U;
  return id;
}

static uint32_t fix_add_prefixed_piece(te2e_vocab_piece_t* pieces,
                                       uint32_t*           count,
                                       const char*         raw,
                                       size_t              raw_len,
                                       float               score)
{
  char   buf[k_fix_max_piece_bytes] = "\xE2\x96\x81";
  size_t blen                       = 3U;
  if (!fix_append_sp_escaped(buf, sizeof(buf), &blen, raw, raw_len)) {
    return UINT32_MAX;
  }
  return fix_add_piece(pieces, count, buf, blen, score, (uint8_t)k_ra8_translate_tok_piece_normal);
}

static int
fix_piece_cmp(const te2e_vocab_piece_t* a, uint32_t ia, const te2e_vocab_piece_t* b, uint32_t ib)
{
  const size_t min_l = (a->len < b->len) ? a->len : b->len;
  const int    cmp   = memcmp(a->text, b->text, min_l);
  if (cmp != 0) {
    return cmp;
  }
  if (a->len != b->len) {
    return (a->len < b->len) ? -1 : 1;
  }
  return (ia < ib) ? -1 : 1;
}

static ra8_err_t
fix_serialize_tvoc(const te2e_vocab_piece_t* pieces, uint32_t count, uint8_t* out, size_t* out_len)
{
  const size_t table_off  = (size_t)k_fix_tvoc_header;
  const size_t sorted_off = table_off + ((size_t)count * (size_t)k_fix_tvoc_rec_bytes);
  const size_t pool_off   = sorted_off + ((size_t)count * 4U);
  size_t       pool_len   = 0U;
  for (uint32_t i = 0U; i < count; ++i) {
    pool_len += pieces[i].len;
  }
  const size_t total = pool_off + pool_len;
  if (total > (size_t)k_te2e_tvoc_cap) {
    return k_ra8_err_invalid_size;
  }
  memset(out, 0, total);
  float  min_score = -10.0F;
  bool   saw       = false;
  size_t cursor    = 0U;
  for (uint32_t i = 0U; i < count; ++i) {
    if ((pieces[i].kind == (uint8_t)k_ra8_translate_tok_piece_normal) &&
        (!saw || (pieces[i].score < min_score))) {
      min_score = pieces[i].score;
      saw       = true;
    }
    const size_t rec = table_off + ((size_t)i * (size_t)k_fix_tvoc_rec_bytes);
    fix_write_u32(out, rec + 0U, (uint32_t)cursor);
    fix_write_u16(out, rec + 4U, (uint16_t)pieces[i].len);
    out[rec + 6U] = pieces[i].kind;
    fix_write_f32(out, rec + 8U, pieces[i].score);
    memcpy(&out[pool_off + cursor], pieces[i].text, pieces[i].len);
    cursor += pieces[i].len;
  }
  uint32_t sorted[k_fix_max_vocab_pieces] = {};
  for (uint32_t i = 0U; i < count; ++i) {
    sorted[i] = i;
  }
  for (uint32_t i = 1U; i < count; ++i) {
    const uint32_t key = sorted[i];
    uint32_t       j   = i;
    while ((j > 0U) &&
           (fix_piece_cmp(&pieces[key], key, &pieces[sorted[j - 1U]], sorted[j - 1U]) < 0)) {
      sorted[j] = sorted[j - 1U];
      j--;
    }
    sorted[j] = key;
  }
  for (uint32_t i = 0U; i < count; ++i) {
    fix_write_u32(out, sorted_off + ((size_t)i * 4U), sorted[i]);
  }
  const uint8_t flags = (uint8_t)k_ra8_translate_tok_flag_add_dummy_prefix |
                        (uint8_t)k_ra8_translate_tok_flag_remove_extra_ws |
                        (uint8_t)k_ra8_translate_tok_flag_escape_ws;
  fix_write_u32(out, 0U, (uint32_t)k_fix_tvoc_magic);
  fix_write_u16(out, 4U, (uint16_t)k_fix_tvoc_version);
  out[6U] = (uint8_t)k_ra8_translate_tok_unigram;
  out[7U] = flags;
  fix_write_u32(out, 8U, (uint32_t)total);
  fix_write_u32(out, 16U, count);
  fix_write_u32(out, 24U, 1U);
  fix_write_u32(out, 28U, 2U);
  fix_write_u32(out, 32U, (uint32_t)k_ra8_translate_tok_id_none);
  fix_write_f32(out, 36U, min_score);
  fix_write_u32(out, 40U, (uint32_t)table_off);
  fix_write_u32(out, 44U, (uint32_t)sorted_off);
  fix_write_u32(out, 48U, (uint32_t)pool_off);
  fix_write_u32(out, 12U, translate_e2e_crc32(out, total));
  *out_len = total;
  return k_ra8_ok;
}

static float fix_walsh_sign(uint32_t row, uint32_t col)
{
  uint32_t bits = row & col;
  uint32_t pop  = 0U;
  while (bits != 0U) {
    pop += (bits & 1U);
    bits >>= 1U;
  }
  return ((pop & 1U) == 0U) ? 1.0F : -1.0F;
}

static void fix_init_tmnt_slots(te2e_tensor_spec_t* specs,
                                uint32_t            vocab_size,
                                const int8_t*       embed_i8,
                                const float*        pos_f32,
                                const int8_t*       lm_w_i8,
                                const float*        lm_b_f32,
                                const int8_t*       zero_i8,
                                const float*        one_ln,
                                const float*        zero_ln)
{
  memset(specs, 0, sizeof(te2e_tensor_spec_t) * (size_t)k_fix_tmnt_slots);
  specs[0] = (te2e_tensor_spec_t){.dtype   = k_ra8_translate_marian_dtype_i8,
                                  .rows    = vocab_size,
                                  .cols    = k_fix_tmnt_d_model,
                                  .scale   = 0.05F,
                                  .i8_vals = embed_i8};
  specs[1] = (te2e_tensor_spec_t){.dtype    = k_ra8_translate_marian_dtype_f32,
                                  .rows     = k_fix_tmnt_max_seq,
                                  .cols     = k_fix_tmnt_d_model,
                                  .scale    = 1.0F,
                                  .f32_vals = pos_f32};
  specs[2] = (te2e_tensor_spec_t){.dtype   = k_ra8_translate_marian_dtype_i8,
                                  .rows    = vocab_size,
                                  .cols    = k_fix_tmnt_d_model,
                                  .scale   = k_fix_lm_scale,
                                  .i8_vals = lm_w_i8};
  specs[3] = (te2e_tensor_spec_t){.dtype    = k_ra8_translate_marian_dtype_f32,
                                  .rows     = vocab_size,
                                  .cols     = 1U,
                                  .scale    = 1.0F,
                                  .f32_vals = lm_b_f32};
  for (uint32_t p = 0U; p < 4U; ++p) {
    specs[8U + (p * 2U)] = (te2e_tensor_spec_t){.dtype   = k_ra8_translate_marian_dtype_i8,
                                                .rows    = k_fix_tmnt_d_model,
                                                .cols    = k_fix_tmnt_d_model,
                                                .scale   = 0.02F,
                                                .i8_vals = zero_i8};
  }
  specs[16] = (te2e_tensor_spec_t){.dtype    = k_ra8_translate_marian_dtype_f32,
                                   .rows     = k_fix_tmnt_d_model,
                                   .cols     = 1U,
                                   .scale    = 1.0F,
                                   .f32_vals = one_ln};
  specs[17] = (te2e_tensor_spec_t){.dtype    = k_ra8_translate_marian_dtype_f32,
                                   .rows     = k_fix_tmnt_d_model,
                                   .cols     = 1U,
                                   .scale    = 1.0F,
                                   .f32_vals = zero_ln};
  specs[18] = (te2e_tensor_spec_t){.dtype   = k_ra8_translate_marian_dtype_i8,
                                   .rows    = k_fix_tmnt_d_ff,
                                   .cols    = k_fix_tmnt_d_model,
                                   .scale   = 0.02F,
                                   .i8_vals = zero_i8};
  specs[20] = (te2e_tensor_spec_t){.dtype   = k_ra8_translate_marian_dtype_i8,
                                   .rows    = k_fix_tmnt_d_model,
                                   .cols    = k_fix_tmnt_d_ff,
                                   .scale   = 0.02F,
                                   .i8_vals = zero_i8};
  specs[22] = specs[16];
  specs[23] = specs[17];
  for (uint32_t blk = 0U; blk < 2U; ++blk) {
    const uint32_t base = 24U + (blk * 10U);
    for (uint32_t p = 0U; p < 4U; ++p) {
      specs[base + (p * 2U)] = specs[8];
    }
    specs[base + 8U] = specs[16];
    specs[base + 9U] = specs[17];
  }
  specs[44] = specs[18];
  specs[46] = specs[20];
  specs[48] = specs[16];
  specs[49] = specs[17];
}

static ra8_err_t fix_serialize_tmnt(const te2e_tensor_spec_t* specs,
                                    uint32_t                  vocab_size,
                                    uint8_t*                  out,
                                    size_t*                   out_len)
{
  const size_t weights_at = ((size_t)k_fix_tmnt_dir_bytes + 3U) & ~(size_t)3U;
  size_t       total      = weights_at;
  for (uint32_t s = 0U; s < (uint32_t)k_fix_tmnt_slots; ++s) {
    if (specs[s].dtype == (uint8_t)k_ra8_translate_marian_dtype_none) {
      continue;
    }
    total           = (total + 3U) & ~(size_t)3U;
    const size_t eb = (specs[s].dtype == (uint8_t)k_ra8_translate_marian_dtype_i8) ? 1U : 4U;
    total += (size_t)specs[s].rows * (size_t)specs[s].cols * eb;
  }
  total = (total + 3U) & ~(size_t)3U;
  if (total > (size_t)k_te2e_tmnt_cap) {
    return k_ra8_err_invalid_size;
  }
  memset(out, 0, total);
  size_t cursor = weights_at;
  for (uint32_t s = 0U; s < (uint32_t)k_fix_tmnt_slots; ++s) {
    const size_t doff = (size_t)k_ra8_translate_marian_header_bytes +
                        ((size_t)s * (size_t)k_ra8_translate_marian_tensor_desc_bytes);
    if (specs[s].dtype == (uint8_t)k_ra8_translate_marian_dtype_none) {
      fix_write_f32(out, doff + 16U, 1.0F);
      continue;
    }
    cursor              = (cursor + 3U) & ~(size_t)3U;
    const size_t elems  = (size_t)specs[s].rows * (size_t)specs[s].cols;
    const size_t eb     = (specs[s].dtype == (uint8_t)k_ra8_translate_marian_dtype_i8) ? 1U : 4U;
    const size_t nbytes = elems * eb;
    if (specs[s].dtype == (uint8_t)k_ra8_translate_marian_dtype_i8) {
      memcpy(&out[cursor], specs[s].i8_vals, elems);
    } else {
      for (size_t k = 0U; k < elems; ++k) {
        fix_write_f32(out, cursor + (k * 4U), specs[s].f32_vals[k]);
      }
    }
    fix_write_u32(out, doff + 0U, (uint32_t)cursor);
    fix_write_u32(out, doff + 4U, (uint32_t)nbytes);
    fix_write_u32(out, doff + 8U, specs[s].rows);
    fix_write_u32(out, doff + 12U, specs[s].cols);
    fix_write_f32(out, doff + 16U, specs[s].scale);
    out[doff + 20U] = specs[s].dtype;
    cursor += nbytes;
  }
  fix_write_u32(out, 0U, (uint32_t)k_ra8_translate_marian_magic);
  fix_write_u16(out, 4U, (uint16_t)k_ra8_translate_marian_version);
  fix_write_u16(out, 6U, (uint16_t)k_ra8_translate_marian_flag_pre_norm);
  fix_write_u32(out, 8U, (uint32_t)total);
  out[20U] = (uint8_t)k_ra8_translate_nmt_ja_en;
  out[21U] = 2U;
  out[22U] = 1U;
  out[23U] = 1U;
  fix_write_u16(out, 24U, (uint16_t)k_fix_tmnt_d_model);
  fix_write_u16(out, 26U, (uint16_t)k_fix_tmnt_d_ff);
  fix_write_u16(out, 28U, (uint16_t)k_fix_tmnt_max_seq);
  fix_write_u32(out, 32U, vocab_size);
  fix_write_u32(out, 40U, 1U);
  fix_write_u32(out, 44U, 2U);
  fix_write_u32(out, 48U, 3U);
  fix_write_f32(out, 52U, 1e-5F);
  fix_write_u32(out, 72U, (uint32_t)k_fix_tmnt_slots);
  fix_write_u32(out, 76U, (uint32_t)weights_at);
  fix_write_u32(out, 80U, (uint32_t)(total - weights_at));
  fix_write_u32(out, 16U, translate_e2e_crc32(&out[weights_at], total - weights_at));
  fix_write_u32(out, 12U, translate_e2e_crc32_zero_win(out, (size_t)k_fix_tmnt_dir_bytes, 12U, 4U));
  *out_len = total;
  return k_ra8_ok;
}

static ra8_err_t
fix_build_nmt(const char* src_for_nmt, const char* eng_utf8, translate_e2e_bubble_pack_t* out_b)
{
  te2e_vocab_piece_t pieces[k_fix_max_vocab_pieces] = {};
  uint32_t           vcount                         = 0U;
  (void)
    fix_add_piece(pieces, &vcount, "<unk>", 5U, 0.0F, (uint8_t)k_ra8_translate_tok_piece_unknown);
  (void)fix_add_piece(pieces, &vcount, "<s>", 3U, 0.0F, (uint8_t)k_ra8_translate_tok_piece_control);
  (void)
    fix_add_piece(pieces, &vcount, "</s>", 4U, 0.0F, (uint8_t)k_ra8_translate_tok_piece_control);
  (void)fix_add_piece(pieces,
                      &vcount,
                      "\xE2\x96\x81",
                      3U,
                      -2.0F,
                      (uint8_t)k_ra8_translate_tok_piece_normal);
  if (fix_add_prefixed_piece(pieces, &vcount, src_for_nmt, strlen(src_for_nmt), -0.1F) ==
      UINT32_MAX) {
    return k_ra8_err_invalid_size;
  }
  uint32_t     tgt_ids[k_fix_max_tgt_tokens] = {};
  size_t       tgt_n                         = 0U;
  const size_t elen                          = strlen(eng_utf8);
  size_t       p                             = 0U;
  while (p < elen) {
    while ((p < elen) && (eng_utf8[p] == ' ')) {
      p++;
    }
    const size_t start = p;
    while ((p < elen) && (eng_utf8[p] != ' ')) {
      p++;
    }
    if ((p > start) && (tgt_n + 1U < (size_t)k_fix_max_tgt_tokens)) {
      const uint32_t tid =
        fix_add_prefixed_piece(pieces, &vcount, &eng_utf8[start], p - start, -0.5F);
      if (tid == UINT32_MAX) {
        return k_ra8_err_invalid_size;
      }
      tgt_ids[tgt_n++] = tid;
    }
  }
  tgt_ids[tgt_n++] = 2U;
  ra8_err_t err    = fix_serialize_tvoc(pieces, vcount, out_b->tvoc_buf, &out_b->tvoc_len);
  if (err != k_ra8_ok) {
    return err;
  }
  static int8_t s_embed[k_fix_max_vocab_pieces * k_fix_tmnt_d_model];
  static int8_t s_lm_w_i8[k_fix_max_vocab_pieces * k_fix_tmnt_d_model];
  static float  s_lm_w_f32[k_fix_max_vocab_pieces * k_fix_tmnt_d_model];
  static float  s_lm_b[k_fix_max_vocab_pieces];
  static float  s_pos[k_fix_tmnt_max_seq * k_fix_tmnt_d_model];
  static int8_t s_zero_i8[k_fix_tmnt_d_model * k_fix_tmnt_d_model];
  static float  s_one_ln[k_fix_tmnt_d_model];
  static float  s_zero_ln[k_fix_tmnt_d_model];
  memset(s_embed, 0, sizeof(s_embed));
  memset(s_lm_w_f32, 0, sizeof(s_lm_w_f32));
  memset(s_zero_i8, 0, sizeof(s_zero_i8));
  for (uint32_t i = 0U; i < vcount; ++i) {
    s_lm_b[i] = -2.0F;
  }
  for (uint32_t pos = 0U; pos < (uint32_t)k_fix_tmnt_max_seq; ++pos) {
    for (uint32_t col = 0U; col < (uint32_t)k_fix_tmnt_d_model; ++col) {
      s_pos[(pos * k_fix_tmnt_d_model) + col] =
        (pos < 15U) ? (0.25F * fix_walsh_sign(pos + 1U, col)) : 0.0F;
    }
  }
  for (uint32_t col = 0U; col < (uint32_t)k_fix_tmnt_d_model; ++col) {
    s_one_ln[col]  = 1.0F;
    s_zero_ln[col] = 0.0F;
  }
  for (size_t step = 0U; step < tgt_n; ++step) {
    const uint32_t tok = tgt_ids[step];
    for (uint32_t col = 0U; col < (uint32_t)k_fix_tmnt_d_model; ++col) {
      s_lm_w_f32[(tok * k_fix_tmnt_d_model) + col] +=
        0.25F * fix_walsh_sign((uint32_t)step + 1U, col);
    }
    s_lm_b[tok] = 0.0F;
  }
  for (size_t i = 0U; i < ((size_t)vcount * (size_t)k_fix_tmnt_d_model); ++i) {
    s_lm_w_i8[i] = fix_quantize_i8(s_lm_w_f32[i], k_fix_lm_scale, 0);
  }
  te2e_tensor_spec_t specs[k_fix_tmnt_slots];
  fix_init_tmnt_slots(specs,
                      vcount,
                      s_embed,
                      s_pos,
                      s_lm_w_i8,
                      s_lm_b,
                      s_zero_i8,
                      s_one_ln,
                      s_zero_ln);
  return fix_serialize_tmnt(specs, vcount, out_b->tmnt_buf, &out_b->tmnt_len);
}

static void fix_fill_det_rect(int8_t*  map,
                              uint16_t mw,
                              uint16_t mh,
                              uint16_t rx,
                              uint16_t ry,
                              uint16_t rw,
                              uint16_t rh,
                              float    prob)
{
  const int8_t   qv    = fix_quantize_i8(prob, k_fix_det_scale, k_fix_det_zp);
  const uint16_t x_end = (uint16_t)((((uint32_t)rx + rw) < mw) ? (rx + rw) : mw);
  const uint16_t y_end = (uint16_t)((((uint32_t)ry + rh) < mh) ? (ry + rh) : mh);
  for (uint16_t y = ry; y < y_end; ++y) {
    for (uint16_t x = rx; x < x_end; ++x) {
      map[((size_t)y * (size_t)mw) + (size_t)x] = qv;
    }
  }
}

static void fix_init_det_page(translate_e2e_page_pack_t* pg,
                              uint16_t                   mw,
                              uint16_t                   mh,
                              uint16_t                   pw,
                              uint16_t                   ph,
                              uint8_t                    first,
                              uint8_t                    bcnt)
{
  pg->page_w       = pw;
  pg->page_h       = ph;
  pg->map_w        = mw;
  pg->map_h        = mh;
  pg->det_elems    = (size_t)mw * (size_t)mh;
  pg->det_scale    = k_fix_det_scale;
  pg->det_zp       = k_fix_det_zp;
  pg->first_bubble = first;
  pg->bubble_count = bcnt;
  const int8_t bg  = fix_quantize_i8(0.01F, k_fix_det_scale, k_fix_det_zp);
  memset(pg->det_map, bg, pg->det_elems);
}

ra8_err_t translate_e2e_populate_chapter_fixtures(translate_e2e_arena_t* arena)
{
  if (arena == nullptr) {
    return k_ra8_err_null_ptr;
  }
  static const char* const k_src[k_te2e_fixture_bubbles] = {
    "\xE3\x82\xBF\xE3\x83\x8A\xE3\x82\xAB\xE3\x81\x95\xE3\x82\x93 \xE5\xBE\x85\xE3\x81\xA3\xE3\x81\xA6!",
    "\xE8\xB5\xB0\xE3\x82\x8D\xE3\x81\x86!",
    "\xE3\x82\xBF\xE3\x83\x8A\xE3\x82\xAB\xE3\x81\x95\xE3\x82\x93",
  };
  static const char* const k_masked[k_te2e_fixture_bubbles] = {
    "__G00__ \xE5\xBE\x85\xE3\x81\xA3\xE3\x81\xA6!",
    "\xE8\xB5\xB0\xE3\x82\x8D\xE3\x81\x86!",
    "__G00__",
  };
  static const char* const k_eng[k_te2e_fixture_bubbles] = {
    "__G00__, wait for me!",
    "Run now!",
    "__G00__!",
  };
  fix_init_det_page(&arena->pages[0], 64U, 96U, 640U, 960U, 0U, 2U);
  fix_fill_det_rect(arena->pages[0].det_map, 64U, 96U, 44U, 8U, 6U, 24U, 0.92F);
  fix_fill_det_rect(arena->pages[0].det_map, 64U, 96U, 51U, 10U, 2U, 12U, 0.89F);
  fix_fill_det_rect(arena->pages[0].det_map, 64U, 96U, 8U, 48U, 18U, 14U, 0.88F);
  fix_init_det_page(&arena->pages[1], 64U, 96U, 640U, 960U, 2U, 1U);
  fix_fill_det_rect(arena->pages[1].det_map, 64U, 96U, 20U, 20U, 20U, 12U, 0.91F);
  for (uint32_t i = 0U; i < (uint32_t)k_te2e_fixture_bubbles; ++i) {
    arena->bubbles[i].src_utf8        = k_src[i];
    arena->bubbles[i].masked_src_utf8 = k_masked[i];
    arena->bubbles[i].eng_utf8        = k_eng[i];
    ra8_err_t err                     = fix_build_ocr(k_src[i], &arena->bubbles[i]);
    if (err != k_ra8_ok) {
      return err;
    }
    err = fix_build_nmt(k_masked[i], k_eng[i], &arena->bubbles[i]);
    if (err != k_ra8_ok) {
      return err;
    }
  }
  return k_ra8_ok;
}

ra8_err_t translate_e2e_build_mini_tpak(translate_e2e_page_pack_t*   pg_scratch,
                                        translate_e2e_bubble_pack_t* bp_scratch,
                                        uint8_t*                     out_buf,
                                        size_t                       out_cap,
                                        size_t*                      out_len)
{
  if ((pg_scratch == nullptr) || (bp_scratch == nullptr) || (out_buf == nullptr) ||
      (out_len == nullptr)) {
    return k_ra8_err_null_ptr;
  }
  static const char k_pid[] = "ja_mini_01";
  static const char k_src[] =
    "\xE4\xBB\x8A\xE6\x97\xA5\xE3\x81\xAF\xE9\x9B\xA8\xE3\x81\xA7\xE3\x81\x99";
  static const char k_eng[] = "Today it is raining.";
  memset(pg_scratch, 0, sizeof(*pg_scratch));
  memset(bp_scratch, 0, sizeof(*bp_scratch));
  fix_init_det_page(pg_scratch, 64U, 96U, 640U, 960U, 0U, 1U);
  fix_fill_det_rect(pg_scratch->det_map, 64U, 96U, 44U, 8U, 6U, 24U, 0.92F);
  fix_fill_det_rect(pg_scratch->det_map, 64U, 96U, 51U, 10U, 2U, 12U, 0.89F);
  ra8_err_t err = fix_build_ocr(k_src, bp_scratch);
  if (err != k_ra8_ok) {
    return err;
  }
  err = fix_build_nmt(k_src, k_eng, bp_scratch);
  if (err != k_ra8_ok) {
    return err;
  }
  const size_t poff     = (size_t)k_te2e_tpak_header_bytes;
  const size_t boff     = poff + (size_t)k_te2e_tpak_page_bytes;
  size_t       cursor   = (boff + (size_t)k_te2e_tpak_bubble_bytes + 3U) & ~(size_t)3U;
  const size_t det_off  = cursor;
  cursor                = (cursor + pg_scratch->det_elems + 3U) & ~(size_t)3U;
  const size_t tchr_off = cursor;
  cursor                = (cursor + bp_scratch->tchr_len + 3U) & ~(size_t)3U;
  const size_t log_off  = cursor;
  cursor                = (cursor + bp_scratch->logits_len + 3U) & ~(size_t)3U;
  const size_t tvoc_off = cursor;
  cursor                = (cursor + bp_scratch->tvoc_len + 3U) & ~(size_t)3U;
  const size_t tmnt_off = cursor;
  cursor                = (cursor + bp_scratch->tmnt_len + 3U) & ~(size_t)3U;
  if (cursor > out_cap) {
    return k_ra8_err_invalid_size;
  }
  memset(out_buf, 0, cursor);
  fix_write_u32(out_buf, 0U, (uint32_t)k_te2e_tpak_magic);
  fix_write_u32(out_buf, 4U, (uint32_t)k_te2e_tpak_version);
  fix_write_u32(out_buf, 8U, 1U);
  fix_write_u32(out_buf, 12U, (uint32_t)cursor);
  fix_write_u32(out_buf, poff + 0U, (uint32_t)(sizeof(k_pid) - 1U));
  memcpy(&out_buf[poff + 4U], k_pid, sizeof(k_pid) - 1U);
  fix_write_u16(out_buf, poff + 36U, pg_scratch->page_w);
  fix_write_u16(out_buf, poff + 38U, pg_scratch->page_h);
  fix_write_u16(out_buf, poff + 40U, pg_scratch->map_w);
  fix_write_u16(out_buf, poff + 42U, pg_scratch->map_h);
  out_buf[poff + 44U] = (uint8_t)k_ra8_translate_lang_japanese;
  out_buf[poff + 45U] = (uint8_t)k_ra8_translate_det_order_manga_rtl;
  out_buf[poff + 46U] = (uint8_t)pg_scratch->det_zp;
  fix_write_u16(out_buf, poff + 48U, 1U);
  fix_write_u32(out_buf, poff + 52U, (uint32_t)det_off);
  fix_write_f32(out_buf, poff + 56U, pg_scratch->det_scale);
  fix_write_u32(out_buf, boff + 0U, (uint32_t)tchr_off);
  fix_write_u32(out_buf, boff + 4U, (uint32_t)bp_scratch->tchr_len);
  fix_write_u32(out_buf, boff + 8U, (uint32_t)log_off);
  fix_write_u32(out_buf, boff + 12U, (uint32_t)bp_scratch->logits_len);
  fix_write_u32(out_buf, boff + 16U, bp_scratch->time_steps);
  fix_write_f32(out_buf, boff + 20U, bp_scratch->ocr_scale);
  fix_write_u32(out_buf, boff + 24U, (uint32_t)tvoc_off);
  fix_write_u32(out_buf, boff + 28U, (uint32_t)bp_scratch->tvoc_len);
  fix_write_u32(out_buf, boff + 32U, (uint32_t)tmnt_off);
  fix_write_u32(out_buf, boff + 36U, (uint32_t)bp_scratch->tmnt_len);
  out_buf[boff + 40U] = (uint8_t)bp_scratch->ocr_zp;
  out_buf[boff + 41U] = (uint8_t)k_ra8_translate_ocr_japanese;
  memcpy(&out_buf[det_off], pg_scratch->det_map, pg_scratch->det_elems);
  memcpy(&out_buf[tchr_off], bp_scratch->tchr_buf, bp_scratch->tchr_len);
  memcpy(&out_buf[log_off], bp_scratch->logits_i8, bp_scratch->logits_len);
  memcpy(&out_buf[tvoc_off], bp_scratch->tvoc_buf, bp_scratch->tvoc_len);
  memcpy(&out_buf[tmnt_off], bp_scratch->tmnt_buf, bp_scratch->tmnt_len);
  fix_write_u32(out_buf, 16U, translate_e2e_crc32_zero_win(out_buf, cursor, 16U, 4U));
  *out_len = cursor;
  return k_ra8_ok;
}
