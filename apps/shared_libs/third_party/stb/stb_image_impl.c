/*
 * stb_image_impl.c -- single-TU build of stb_image's implementation.
 *
 * stb is a header-only library; consumers usually paste
 *
 *     #define STB_IMAGE_IMPLEMENTATION
 *     #include "stb_image.h"
 *
 * into one source file and just include the header in others. Doing
 * the implementation include from inside apps/shared_libs/reflow/src/ would drag
 * stb's dense magic-number style and multi-thousand-line functions
 * through clang-tidy on every CI run; instead we keep the include here
 * under apps/shared_libs/third_party/stb/, which the source-quality gates
 * exclude via their third_party path filter.
 *
 * Configuration:
 *   - Only the four raster formats EPUB covers + figures actually use
 *     (JPEG, PNG, GIF, BMP) are compiled in; the rest are excluded to
 *     keep the firmware image small.
 *   - No stdio (decode from memory only); no HDR / linear float path.
 *   - STBI_MALLOC / STBI_FREE / STBI_REALLOC_SIZED are redirected to the
 *     heap-free bump arena in libs/ra_reflow/ (see ra8_img_arena.h), so the
 *     decoder never reaches libc malloc (NASA P10 Rule 3).
 *
 * Public-domain (per stb's "unlicense"). See the header for the
 * upstream copyright notice.
 */

#define STBI_ONLY_JPEG
#define STBI_ONLY_PNG
#define STBI_ONLY_GIF
#define STBI_ONLY_BMP
#define STBI_NO_STDIO
#define STBI_NO_LINEAR
#define STBI_NO_HDR
#define STBI_ASSERT(x) ((void)0)

/*
 * Reject attacker-controlled images that declare an absurd width or height
 * before the decoder multiplies them out. stb defaults STBI_MAX_DIMENSIONS to
 * 1<<24 (16M px per axis); a malicious EPUB cover/figure claiming, e.g.,
 * 65536x65536 makes w*h*channels overflow 32 bits to a small value, so the
 * ra8_img_arena allocation succeeds small while the decode then writes the full
 * (non-overflowed) pixel count -> heap overflow -> Non-secure code execution.
 * 8192 per axis is far above any real e-reader raster (the 1024x600 panel and
 * the bounded bump arena reject anything large on actual size anyway) yet keeps
 * w*h*4 <= 2^28, well clear of the 32-bit overflow the cap exists to prevent.
 */
#define STBI_MAX_DIMENSIONS 8192

/*
 * Single-threaded bare-metal firmware: stb_image otherwise makes its global
 * failure-reason + flag state `_Thread_local`, which the compiler lowers to
 * emulated TLS (__emutls_get_address). There is no TLS runtime on this target,
 * so reaching one of those globals during a decode HardFaults. Force plain
 * `static` globals instead -- correct because decoding is never concurrent here.
 */
#define STBI_NO_THREAD_LOCALS

#define STBI_MALLOC(sz)                     ra8_img_arena_malloc(sz)
#define STBI_FREE(p)                        ra8_img_arena_free(p)
#define STBI_REALLOC_SIZED(p, oldsz, newsz) ra8_img_arena_realloc_sized((p), (oldsz), (newsz))

#define STB_IMAGE_IMPLEMENTATION
#include "ra8_img_arena.h"
#include "stb_image.h"
