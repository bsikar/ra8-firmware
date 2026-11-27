# TMNT / TVOC / TSLT -- Bergamot Marian Translation Containers

**Magic:** `TMNT` (`0x544E4D54`), `TVOC` (`0x434F5654`), `TSLT` (`0x544C5354`) &nbsp;|&nbsp;
**Version:** `1` &nbsp;|&nbsp;
**Endianness:** Little-endian &nbsp;|&nbsp;
**Producer:** `tools/marian_conv` &nbsp;|&nbsp;
**Consumer:** `libs/ra8_translate` (`marian_container.zig`, `vocab.zig`, `marian.zig`, `marian_batch.zig`)

`TMNT`, `TVOC`, and `TSLT` are the three read-only binary containers that move Mozilla Bergamot
(Firefox Translations) neural machine translation models, SentencePiece vocabularies, and lexical
shortlists from the host converter toolchain onto Cortex-M85 XIP flash or SDRAM without dynamic
allocation or target-side model parsing.

> **One-sentence summary.** *`TVOC` stores a CRC-32-verified SentencePiece vocabulary and sorted
> prefix table; `TSLT` stores a CSR lexical shortlist seeded with `first_k` frequent targets; and
> `TMNT` binds affine `int8` weight matrices, `float32` biases/LayerNorm/sinusoidal positions, and
> an optional trailing `TSLT` payload behind separate metadata and weight-payload CRC-32 checks.*

---

## 1. Purpose, Lifecycle, and Producers

On host builds (`tools/marian_conv`), Bergamot upstream archives (`model.<pair>.intgemm.alphas.bin`,
`vocab.<pair>.spm`, `lex.50.50.<pair>.s2t.bin`, licensed `MPL-2.0`) are SHA-256-verified against
pinned Firefox Remote Settings manifests and compiled into deterministic host-independent binary
containers:

- **`TVOC` (`.tvoc`)** packs SentencePiece Unigram (or BPE) pieces, log-probability scores,
  piece kind tags, normalization flags, and a pre-sorted UTF-8 index permutation.
- **`TSLT` (`.tslt`)** packs Bergamot's `BinaryShortlist` (`first_k` common targets plus sparse
  source-token -> deduplicated target-ID candidate rows) as a `u32` CSR array.
- **`TMNT` (`.tmnt`)** packs ordered tensor descriptors (`[out, in]` `int8` `intgemm8` weights with
  `scale = 1.0 / quantMult`, `float32` vectors and optional sinusoidal position matrix) plus an
  optional embedded copy of `TSLT` at the tail of the payload.

On target (`libs/ra8_translate`), `ra8_translate_tok_open()` and `ra8_translate_marian_open()`
borrow caller-provided flash/XIP pointers directly with zero heap allocation. No descriptor table
resorting, quantization scaling discovery, or protobuf decoding runs on target.

@dot
digraph tmnt_pipeline {
  rankdir=LR;
  node [shape=box, style="rounded,filled", fontname="Helvetica", fontsize=10];

  subgraph cluster_host {
    label="Host offline conversion (tools/marian_conv)";
    style=dashed;
    bin [label="Bergamot .bin\n(intgemm8 + model.yml)", fillcolor="#eef3f8", color="#4a6b8a"];
    spm [label="SentencePiece .spm\n(ModelProto)", fillcolor="#eef3f8", color="#4a6b8a"];
    lex [label="Lexical .s2t.bin\n(BinaryShortlist)", fillcolor="#eef3f8", color="#4a6b8a"];
    conv [label="marian_conv convert", fillcolor="#fbf0d9", color="#b8913f"];
    bin -> conv;
    spm -> conv;
    lex -> conv;
  }

  tvoc [label="TVOC\n56B hdr + pieces + sorted + UTF-8 pool", fillcolor="#f4f8ee", color="#5b7a3d"];
  tmnt [label="TMNT (+ optional TSLT)\n84B hdr + descs + weights + TSLT", fillcolor="#f4f8ee", color="#5b7a3d"];
  rt [label="libs/ra8_translate\ntok_* + marian_* (zero alloc)", fillcolor="#f7efe8", color="#8c5a2b"];

  conv -> tvoc;
  conv -> tmnt;
  tvoc -> rt [label="encode / decode"];
  tmnt -> rt [label="step_logits / batch_decode"];
}
@enddot

---

## 2. Wire Framing and Endianness

All integer and floating-point fields across `TMNT`, `TVOC`, and `TSLT` are **little-endian**.
Checksum validation uses standard IEEE 802.3 **CRC-32** (polynomial `0xEDB88320`, matching `zlib.crc32`
in Python and `std.hash.Crc32` in Zig):

- **`TVOC` CRC-32:** Computed over the entire `total_bytes` image with the 4-byte `container_crc32`
  field at offset `0x0C` treated as zero.
- **`TMNT` Two-Stage CRC-32:**
  1. `payload_crc32` (`0x10`) is computed over `[weights_off .. total_bytes)` (which includes all
     tensor payloads, alignment padding inside the payload region, and the optional embedded `TSLT`).
  2. With `payload_crc32` already stamped at `0x10` and `meta_crc32` (`0x0C`) zeroed, `meta_crc32`
     is computed over `[0 .. 84 + tensor_count * 24)`.
  This lets fast startup paths call `ra8_translate_marian_open(..., k_ra8_translate_marian_open_meta_only)`
  in microseconds on XIP flash, or enable `k_ra8_translate_marian_open_verify_payload` to audit the
  full weight + shortlist image.

---

## 3. TMNT Header and Tensor Directory (`TMNT`)

### 3.1 Fixed 84-Byte Header

| Offset | Type | Field | Description |
|---:|---|---|---|
| `0x00` | `u32` | `magic` | `0x544E4D54` (bytes `"TMNT"`). |
| `0x04` | `u16` | `version` | Must equal `1`. |
| `0x06` | `u16` | `model_flags` | Bitmask of model architectural flags (table below). |
| `0x08` | `u32` | `total_bytes` | Exact container byte length (`>= weights_off`). |
| `0x0C` | `u32` | `meta_crc32` | IEEE CRC-32 over `[0 .. dir_end)` with this field zeroed. |
| `0x10` | `u32` | `payload_crc32` | IEEE CRC-32 over `[weights_off .. total_bytes)`. |
| `0x14` | `u8` | `nmt_model` | Language/model ID (`1=ko-en`, `2=ja-en`, `3=zh-en`). |
| `0x15` | `u8` | `num_heads` | Attention heads per block (`1..16`, `d_model % num_heads == 0`). |
| `0x16` | `u8` | `num_enc_layers` | Transformer encoder block count (`1..16`). |
| `0x17` | `u8` | `num_dec_layers` | Transformer decoder block count (`1..16`). |
| `0x18` | `u16` | `d_model` | Model hidden dimension (`1..2048`). |
| `0x1A` | `u16` | `d_ff` | Feed-forward inner dimension (`1..8192`). |
| `0x1C` | `u16` | `max_seq_len` | Maximum sequence length (`1..1024`). |
| `0x1E` | `u16` | `reserved_hdr` | Must be `0`. |
| `0x20` | `u32` | `vocab_size` | Vocabulary size (`<= 262144`). |
| `0x24` | `u32` | `unk_id` | `<unk>` token ID (`< vocab_size`). |
| `0x28` | `u32` | `bos_id` | Initial step-0 target ID (`< vocab_size`, `0` when `zero_bos` set). |
| `0x2C` | `u32` | `eos_id` | End-of-sequence token ID (`< vocab_size`). |
| `0x30` | `u32` | `pad_id` | Padding token ID (`< vocab_size`). |
| `0x34` | `f32` | `ln_eps` | LayerNorm epsilon (`1e-6` for Marian/Bergamot, finite `> 0`). |
| `0x38` | `u32` | `reserved_a` | Must be `0`. |
| `0x3C` | `u32` | `reserved_b` | Must be `0`. |
| `0x40` | `u32` | `shortlist_off` | Byte offset of embedded `TSLT`, or `0` if absent. |
| `0x44` | `u32` | `shortlist_bytes` | Byte length of embedded `TSLT`, or `0` if absent. |
| `0x48` | `u32` | `tensor_count` | Expected slot count (`8 + 16*num_enc_layers + dec_stride*num_dec_layers`). |
| `0x4C` | `u32` | `weights_off` | `align_up4(84 + tensor_count * 24)`. |
| `0x50` | `u32` | `weights_bytes` | Byte span from `weights_off` to start of `TSLT` (or `total_bytes`). |

### 3.2 `model_flags` Bitmask

| Bit | Name | Meaning |
|---:|---|---|
| `0x0001` | `flag_pre_norm` | Pre-LayerNorm residual blocks (`"n"` in Marian `transformer-preprocess`). |
| `0x0002` | `flag_scale_embed` | Multiply token embedding by `sqrt(d_model)` before adding position. |
| `0x0004` | `flag_tied_embed` | Output LM head matrix (`slot 2`) shares `embed_w` (`slot 0`) payload/scale. |
| `0x0008` | `flag_decoder_ssru` | Decoder blocks use 21-slot SSRU recurrent sublayers (`dec-cell: ssru`). |
| `0x0010` | `flag_zero_bos` | Step 0 decoder input embedding starts at zero vector instead of `embed(bos_id)`. |

Bits above `0x001F` must be zero.

### 3.3 24-Byte Tensor Descriptor & Slot Order

Each of the `tensor_count` descriptors at `0x54 + slot * 24` has fixed layout:

| Offset | Type | Field | Description |
|---:|---|---|---|
| `0x00` | `u32` | `offset` | Container-relative offset in `[weights_off .. weights_off + weights_bytes)` (`% 4 == 0`). |
| `0x04` | `u32` | `byte_size` | Exact element payload size (`rows * cols` for `i8`, `rows * cols * 4` for `f32`). |
| `0x08` | `u32` | `rows` | First matrix dimension (or element count for 1D vector). |
| `0x0C` | `u32` | `cols` | Second matrix dimension (`1` for 1D vector). |
| `0x10` | `f32` | `scale` | Dequantization multiplier (`1.0 / quantMult` for `intgemm8`, finite `> 0`). |
| `0x14` | `u8` | `dtype` | `0 = none` (optional omitted), `1 = i8`, `2 = f32`. |
| `0x15` | `i8` | `zero_point` | Affine `int8` zero point (`0` for symmetric Bergamot `intgemm8`). |
| `0x16` | `u8[2]` | `reserved` | Must be `[0, 0]`. |

All 2D weight matrices are stored **already transposed in `[out, in]` row-major order** so `y[o] = sum_k x[k] * w_i8[o, k]` streams sequentially with unit stride across SIMD/Helium lanes.

- **Global Slots (`0..7`):**
  `0: embed_w (i8 [vocab, d_model])`, `1: pos_embed (opt f32 [max_seq_len, d_model])`,
  `2: lm_head_w (i8 [vocab, d_model], alias to slot 0 when tied)`, `3: lm_head_b (opt f32 [vocab])`,
  `4: enc_final_ln_scale (opt f32 [d_model])`, `5: enc_final_ln_bias (opt f32 [d_model])`,
  `6: dec_final_ln_scale (opt f32 [d_model])`, `7: dec_final_ln_bias (opt f32 [d_model])`.
  When `pos_embed` (`slot 1`) is provided (as in `tools/marian_conv`), Marian's exact sinusoidal table
  (`log(10000) / (d_model/2 - 1)`) is precomputed into `slot 1` at conversion time.
- **Encoder Block (`16` slots per layer):**
  Self-attention `(Wq, bq, Wk, bk, Wv, bv, Wo, bo, Wo_ln_scale, Wo_ln_bias)` followed by FFN
  `(W1, b1, W2, b2, ffn_ln_scale, ffn_ln_bias)`. Biases may be `dtype=none`; weights/LN scales/LN biases are required.
- **Decoder Block (`26` slots standard vs `21` slots SSRU):**
  - When `flag_decoder_ssru == 0`: self-attn (`10`) + cross-attn (`10`) + FFN (`6`) = `26` slots/layer.
  - When `flag_decoder_ssru != 0`: SSRU cell `(rnn_W [d,d], rnn_Wf [d,d], opt rnn_bf [d], rnn_ffn_ln_scale [d], rnn_ffn_ln_bias [d])` (`5`) + cross-attn (`10`) + FFN (`6`) = `21` slots/layer.
    The SSRU recurrence computes `f = sigmoid(Wf * x + bf)`, `c_t = f * c_{t-1} + (1 - f) * (W * x)`,
    `y = x + (W * x - c_t)` matching `marian::rnn::SimplerNN` (`ssru`).

---

## 4. TVOC SentencePiece Vocabulary Container (`TVOC`)

### 4.1 Fixed 56-Byte Header

| Offset | Type | Field | Description |
|---:|---|---|---|
| `0x00` | `u32` | `magic` | `0x434F5654` (bytes `"TVOC"`). |
| `0x04` | `u16` | `version` | Must equal `1`. |
| `0x06` | `u8` | `model_type` | `0 = unigram`, `1 = bpe`. |
| `0x07` | `u8` | `flags` | Bitmask: `0x01=add_dummy_prefix`, `0x02=remove_extra_ws`, `0x04=escape_ws`, `0x08=byte_fallback`, `0x10=norm_fullwidth`. |
| `0x08` | `u32` | `total_bytes` | Exact container byte length. |
| `0x0C` | `u32` | `container_crc32` | IEEE CRC-32 over entire image with `[0x0C..0x10)` zeroed. |
| `0x10` | `u32` | `vocab_size` | Number of pieces (`1..262144`). |
| `0x14` | `u32` | `unk_id` | Unknown token ID (`< vocab_size`, kind must be `1 = unknown`). |
| `0x18` | `u32` | `bos_id` | BOS token ID or `0xFFFFFFFF` (`none_id`). |
| `0x1C` | `u32` | `eos_id` | EOS token ID or `0xFFFFFFFF` (`none_id`). |
| `0x20` | `u32` | `pad_id` | PAD token ID or `0xFFFFFFFF` (`none_id`). |
| `0x24` | `f32` | `min_normal_score` | Minimum finite score across `kind == normal` entries. |
| `0x28` | `u32` | `piece_table_off` | Must equal `56`. |
| `0x2C` | `u32` | `sorted_index_off` | Must equal `56 + vocab_size * 12`. |
| `0x30` | `u32` | `pool_off` | Must equal `sorted_index_off + vocab_size * 4`. |
| `0x34` | `u32` | `reserved` | Must equal `0`. |

### 4.2 Piece Records & Sorted Index

Each 12-byte piece record at `piece_table_off + id * 12` holds `<text_off:u32, text_len:u16, kind:u8, reserved:u8, score:f32>`.
`TVOC` `kind` values map from SentencePiece `ModelProto::SentencePiece::Type` as:
`0=normal (SPM 1)`, `1=unknown (SPM 2)`, `2=control (SPM 3)`, `3=user_defined (SPM 4)`, `4=byte (SPM 6)`, `5=unused (SPM 5)`.
The sorted index table at `sorted_index_off` holds `vocab_size` `u32` token IDs sorted strictly in ascending `(utf8_bytes, token_id)` order so `libs/ra8_translate` binary-searches UTF-8 spans in `O(log V)` without extra RAM.

---

## 5. TSLT Lexical Shortlist Container (`TSLT`)

| Offset | Type | Field | Description |
|---:|---|---|---|
| `0x00` | `u32` | `magic` | `0x544C5354` (bytes `"TSLT"`). |
| `0x04` | `u16` | `version` | Must equal `1`. |
| `0x06` | `u16` | `first_k` | Common-target IDs `[0 .. first_k)` seeded into every shortlist (`<= vocab_size`). |
| `0x08` | `u32` | `vocab_size` | Must equal `TMNT.vocab_size` when embedded. |
| `0x0C` | `u32` | `total_entries` | Total target candidate IDs after CSR offsets. |
| `0x10` | `u32[V+1]` | `row_offsets` | Non-decreasing CSR row offsets (`offsets[0] == 0`, `offsets[V] == total_entries`). |
| `...` | `u32[E]` | `target_ids` | Deduplicated target vocabulary IDs (`< vocab_size`). |

Total `TSLT` length is strictly `16 + (vocab_size + 1 + total_entries) * 4` bytes. When `TMNT.shortlist_bytes > 0`, `TMNT.shortlist_off` points immediately after `weights_off + weights_bytes` and is covered by `TMNT.payload_crc32`.

---

## 6. Validation Rules and Fail-Closed Behavior

`marian_container.zig` and `vocab.zig` reject malformed containers before reading weights or returning success:

1. Exact header magic, version `1`, reserved zeros, and `total_bytes == buffer.len`.
2. Non-zero finite `ln_eps` and descriptor scales, `offset % 4 == 0`, and non-overlapping payload bounds inside `[weights_off .. weights_off + weights_bytes)`.
3. Expected slot count matching `num_enc_layers`, `num_dec_layers`, and `flag_decoder_ssru`.
4. `TSLT` `first_k <= vocab_size`, monotonic `row_offsets`, and all `target_ids < vocab_size`.
5. `TVOC` valid UTF-8 on all non-unused pieces, uppercase `<0xHH>` syntax on `byte` pieces, `unk_id` piece kind `unknown`, and strict ascending `(utf8, id)` order on `sorted_index`.
6. CRC-32 match (`TVOC` unconditionally; `TMNT` `meta_crc32` always, `payload_crc32` when `k_ra8_translate_marian_open_verify_payload` is set).

---

## 7. Worked Example (Real `ko-en` Bergamot v2.1)

Converting the pinned Mozilla Bergamot v2.1 `ko-en` (`base-memory`, `MPL-2.0`) model with
`marian_conv convert --pair ko-en` produces three deterministic outputs:

- `koen.tmnt`: `52,683,772` bytes (`sha256: 7d786ad4f6104ddb8dcf47d46d5b5f52b91f747d4d2182c1413bb8ca950e980a`)
- `koen.tvoc`: `1,619,689` bytes (`sha256: ce935c631412dcb92a51edfff6507a52ae962a5f5da2aaa35e17200db9debf07`)
- `koen.tslt`: `8,361,480` bytes (`sha256: 6e5e0e83affb924b2ecedb03bbd59032026305c3707e37eab129d46a0dd5a85c`)

Hexdump of the serialized headers:

```text
koen.tmnt (84-byte header):
  0000: 54 4d 4e 54 01 00 1e 00 fc e3 23 03 51 89 8f 60   ; magic="TMNT", ver=1, flags=0x001e, total=52683772, meta_crc=0x608f8951
  0010: 99 20 89 3d 01 08 06 04 80 01 00 06 00 01 00 00   ; payload_crc=0x3d892099, model=1(ko-en), heads=8, enc=6, dec=4, d=384, ff=1536, max_seq=256
  0020: 00 fa 00 00 01 00 00 00 00 00 00 00 00 00 00 00   ; vocab=64000, unk=1, bos=0, eos=0
  0030: 00 00 00 00 bd 37 86 35 00 00 00 00 00 00 00 00   ; pad=0, ln_eps=1e-6 (0x358637bd)
  0040: f4 4d a4 02 08 96 7f 00 bc 00 00 00 f4 11 00 00   ; sl_off=44322292, sl_bytes=8361480, tensors=188 (8+6*16+4*21), weights_off=4596
  0050: 00 3c a4 02                                       ; weights_bytes=44317696

koen.tvoc (56-byte header):
  0000: 54 56 4f 43 01 00 00 1f e9 b6 18 00 d8 36 74 8e   ; magic="TVOC", ver=1, unigram=0, flags=0x1f, total=1619689, crc=0x8e7436d8
  0010: 00 fa 00 00 01 00 00 00 ff ff ff ff 00 00 00 00   ; vocab=64000, unk=1, bos=none, eos=0
  0020: ff ff ff ff 8c 23 9b c1 38 00 00 00 38 b8 0b 00   ; pad=none, min_score, piece_off=56, sorted_off=768056
  0030: 38 a0 0f 00 00 00 00 00                           ; pool_off=1024056, reserved=0

koen.tslt (header + start of row_offsets):
  0000: 54 53 4c 54 01 00 32 00 00 fa 00 00 7d eb 1e 00   ; magic="TSLT", ver=1, first_k=50, vocab=64000, total_entries=2026365
```

---

## 8. Versioning and Compatibility

`version` stays `1` across all three containers (`TMNT`, `TVOC`, `TSLT`) while adding backward-compatible
capability flags (`flag_decoder_ssru`, `flag_zero_bos`, and `TSLT.first_k`, which defaults cleanly to
`0` on legacy zero-initialized headers). Any change to header byte length, tensor descriptor size, or
slot semantics requires incrementing `version` to `2`.
