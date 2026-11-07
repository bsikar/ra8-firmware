/* translate-c seam shim: miniz.h replacement for header translation only.
 *
 * aro drops every declaration that follows a mid-file #pragma once, and
 * miniz.h (a concatenation of miniz's sources) carries five of them, so the
 * real header cannot feed the seam. The tinfl surface the seam needs is
 * reproduced here one for one from the vendored header (flags, statuses,
 * the decompressor struct and the tinfl_decompress prototype): the ported
 * Zig carve sizes the struct exactly and calls tinfl_decompress, which
 * resolves against the vendored miniz at link time. The C builds never see
 * this path (see build.zig). */
#ifndef SEAM_SHIM_MINIZ_H
#define SEAM_SHIM_MINIZ_H

#include <stddef.h>
#include <stdint.h>

typedef uint8_t mz_uint8;
typedef int8_t mz_int8;
typedef uint16_t mz_uint16;
typedef int16_t mz_int16;
typedef uint32_t mz_uint32;
typedef int32_t mz_int32;
typedef uint64_t mz_uint64;
typedef int64_t mz_int64;
typedef unsigned int mz_uint;
typedef int mz_bool;
typedef unsigned long mz_ulong;

#define MZ_FALSE 0
#define MZ_TRUE 1

#if defined(_M_X64) || defined(_WIN64) || defined(__MINGW64__) || defined(_LP64) || defined(__LP64__) || defined(__ia64__) || defined(__x86_64__)
/* Mirrors the vendored miniz.h 64-bit register test verbatim, so the struct
 * layout (and its m_bit_buf width) matches the real one per build target. */
#define MINIZ_HAS_64BIT_REGISTERS 1
#else
#define MINIZ_HAS_64BIT_REGISTERS 0
#endif

/* Decompression flags used by tinfl_decompress(). */
enum
{
    TINFL_FLAG_PARSE_ZLIB_HEADER = 1,
    TINFL_FLAG_HAS_MORE_INPUT = 2,
    TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF = 4,
    TINFL_FLAG_COMPUTE_ADLER32 = 8
};

/* tinfl_status is a plain int here: the vendored enum is int-sized, so the
 * ABI matches the real tinfl_decompress the consumers link against. */
typedef mz_int32 tinfl_status;
#define TINFL_STATUS_FAILED_CANNOT_MAKE_PROGRESS (-4)
#define TINFL_STATUS_ADLER32_MISMATCH (-3)
#define TINFL_STATUS_CRC32_MISMATCH (-2)
#define TINFL_STATUS_FAILED (-1)
#define TINFL_STATUS_DONE 0
#define TINFL_STATUS_NEEDS_MORE_INPUT 1
#define TINFL_STATUS_HAS_MORE_OUTPUT 2

enum
{
    TINFL_MAX_HUFF_TABLES = 3,
    TINFL_MAX_HUFF_SYMBOLS_0 = 288,
    TINFL_MAX_HUFF_SYMBOLS_1 = 32,
    TINFL_MAX_HUFF_SYMBOLS_2 = 19,
    TINFL_FAST_LOOKUP_BITS = 10,
    TINFL_FAST_LOOKUP_SIZE = 1 << TINFL_FAST_LOOKUP_BITS
};

#if MINIZ_HAS_64BIT_REGISTERS
typedef mz_uint64 tinfl_bit_buf_t;
#define TINFL_BITBUF_SIZE (64)
#else
typedef mz_uint32 tinfl_bit_buf_t;
#define TINFL_BITBUF_SIZE (32)
#endif

struct tdefl_compressor;
typedef struct tdefl_compressor tdefl_compressor;

struct tinfl_decompressor_tag
{
    mz_uint32 m_state, m_num_bits, m_zhdr0, m_zhdr1, m_z_adler32, m_final, m_type, m_check_adler32, m_dist, m_counter, m_num_extra, m_table_sizes[TINFL_MAX_HUFF_TABLES];
    tinfl_bit_buf_t m_bit_buf;
    size_t m_dist_from_out_buf_start;
    mz_int16 m_look_up[TINFL_MAX_HUFF_TABLES][TINFL_FAST_LOOKUP_SIZE];
    mz_int16 m_tree_0[TINFL_MAX_HUFF_SYMBOLS_0 * 2];
    mz_int16 m_tree_1[TINFL_MAX_HUFF_SYMBOLS_1 * 2];
    mz_int16 m_tree_2[TINFL_MAX_HUFF_SYMBOLS_2 * 2];
    mz_uint8 m_code_size_0[TINFL_MAX_HUFF_SYMBOLS_0];
    mz_uint8 m_code_size_1[TINFL_MAX_HUFF_SYMBOLS_1];
    mz_uint8 m_code_size_2[TINFL_MAX_HUFF_SYMBOLS_2];
    mz_uint8 m_raw_header[4], m_len_codes[TINFL_MAX_HUFF_SYMBOLS_0 + TINFL_MAX_HUFF_SYMBOLS_1 + 137];
};
typedef struct tinfl_decompressor_tag tinfl_decompressor;

/* ABI-matched prototype: the real function returns the int-sized
 * tinfl_status enum. */
mz_int32 tinfl_decompress(tinfl_decompressor *r, const mz_uint8 *pIn_buf_next, size_t *pIn_buf_size, mz_uint8 *pOut_buf_start, mz_uint8 *pOut_buf_next, size_t *pOut_buf_size, const mz_uint32 decomp_flags);

#endif
