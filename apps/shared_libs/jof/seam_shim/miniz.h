/* translate-c seam shim: miniz.h replacement for header translation only.
 *
 * aro drops every declaration that follows a mid-file #pragma once, and
 * miniz.h (a concatenation of miniz's sources) carries five of them, so the
 * real header cannot feed the seam. The seam's only live use of miniz is the
 * tinfl handle type: jof_png_internal.h's state struct holds it by pointer,
 * and the ported Zig never dereferences it, so an opaque tag keeps the
 * layout exact. The mz_* typedefs match miniz's own definitions one for one.
 * The C builds never see this path (see build.zig). */
#ifndef SEAM_SHIM_MINIZ_H
#define SEAM_SHIM_MINIZ_H

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

struct tinfl_decompressor_tag;
typedef struct tinfl_decompressor_tag tinfl_decompressor;
struct tdefl_compressor;
typedef struct tdefl_compressor tdefl_compressor;

#endif
