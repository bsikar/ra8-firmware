/*
 * stb_truetype_impl.c -- single-TU build of stb_truetype's implementation.
 *
 * stb is a header-only library; consumers usually paste
 *
 *     #define STB_TRUETYPE_IMPLEMENTATION
 *     #include "stb_truetype.h"
 *
 * into one source file and just include the header in others. Doing
 * the implementation include from inside apps/shared_libs/epub/src/ would drag
 * stb's dense magic-number style and 4000-line functions through
 * clang-tidy on every CI run; instead we keep the include inside
 * apps/shared_libs/third_party/stb/, which the source-quality gates exclude
 * via its third_party path filter.
 *
 * Public-domain (per stb's "unlicense"). See the header for the
 * upstream copyright notice.
 */

#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_assert(x) ((void)0)

/*
 * The RA8D2 Cortex-M85 FPU is single-precision only (fpv5-sp-d16), so stb's
 * default double-precision math hooks (sqrt/pow/fmod/cos/...) are emulated in
 * software -- a board_sim profile of EPUB glyph rasterisation showed
 * __ieee754_sqrt alone at ~45% of the wall time, with __adddf3/__cmpdf2 close
 * behind. Point the hooks at the single-precision (float) variants so the curve
 * flattening + edge rasteriser run on the hardware FPU. The geometry is computed
 * in float throughout stb's rasteriser, so this only removes the float->double->
 * float round trips; antialiased glyph coverage is unchanged.
 */
#include <math.h>
#define STBTT_ifloor(x)  ((int)floorf((x)))
#define STBTT_iceil(x)   ((int)ceilf((x)))
#define STBTT_sqrt(x)    (sqrtf((x)))
#define STBTT_pow(x, y)  (powf((x), (y)))
#define STBTT_fmod(x, y) (fmodf((x), (y)))
#define STBTT_cos(x)     (cosf((x)))
#define STBTT_acos(x)    (acosf((x)))
#define STBTT_fabs(x)    (fabsf((x)))

#include "stb_truetype.h"
