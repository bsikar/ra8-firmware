/*
 * stb_truetype_impl.c -- single-TU build of stb_truetype's implementation.
 *
 * stb is a header-only library; consumers usually paste
 *
 *     #define STB_TRUETYPE_IMPLEMENTATION
 *     #include "stb_truetype.h"
 *
 * into one source file and just include the header in others. Doing
 * the implementation include from inside libs/ra_epub/src/ would drag
 * stb's dense magic-number style and 4000-line functions through
 * clang-tidy on every CI run; instead we keep the include inside
 * libs/third_party/stb/, which scripts/clang_tidy.sh already excludes
 * via its third_party path filter.
 *
 * Public-domain (per stb's "unlicense"). See the header for the
 * upstream copyright notice.
 */

#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_assert(x) ((void)0)
#include "stb_truetype.h"
