# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
#
# cmake/ra8_app/vendored.cmake -- per-TU compile options for the vendored SOUP decoders.
#
# SOURCED, NEVER EXECUTED on its own. cmake/ra8_add_app.cmake includes this
# file once and calls the macro below from inside ra8_add_app().
#
# It is a macro(), not a function(): CMake functions create a new variable
# scope, so every _ra8_* variable this sets would vanish on return. A macro is
# textual substitution and behaves exactly as the inline code it replaces --
# which is what makes this split behaviour-preserving rather than a rewrite.
#
# These run AFTER add_executable() because they are per-SOURCE properties on
# targets that must already exist. Each block narrows -Werror for exactly the
# style classes one vendored parser trips -- never a blanket -w, which is what
# these used to carry and which switched off the memory-safety classes too
# (issue #179).
macro(_ra8_app_vendored_flags)
  # Route stb_truetype's allocations through the heap-free arena (see above).
  # The vendored header trips only the style classes suppressed by the SOUP
  # -Wno set above (bad-function-cast, cast-qual, double-promotion,
  # unused-parameter); -Werror stays in force for the memory-safety classes.
  if(_ra8_stb_impl)
    set_property(
      SOURCE ${_ra8_stb_impl}
      APPEND
      PROPERTY COMPILE_OPTIONS
               ${_ra8_soup_wno_common}
               ${_ra8_soup_wno_c}
               -include
               "${RA8_REPO_ROOT}/libs/ra8_reflow/inc/ra8_stbtt_alloc.h"
               "-DSTBTT_malloc(x,u)=ra8_stbtt_malloc(x)"
               "-DSTBTT_free(x,u)=ra8_stbtt_free(x)"
    )
    # stb_truetype pulls in sqrt/floor/ceil from the math library.
    target_link_libraries(${_ra8_elf} PRIVATE m)
  endif()

  # Same treatment for the stb_image single-TU build: a vendored TU that trips
  # the SOUP style classes (cast-align, cast-qual, double-promotion,
  # duplicated-branches, missing-declarations/-prototypes) only. -Werror stays
  # in force for the memory-safety classes on this attacker-facing decoder.
  # The STBI_* allocator macros are defined inside stb_image_impl.c itself.
  if(_ra8_stb_img_impl)
    set_property(
      SOURCE ${_ra8_stb_img_impl}
      APPEND
      PROPERTY COMPILE_OPTIONS ${_ra8_soup_wno_common} ${_ra8_soup_wno_c}
    )
  endif()

  # The vendored miniz.c (C) / tinyxml2.cpp (C++) trip only the SOUP style
  # classes; suppress those with the narrow -Wno set so -Werror stays in force
  # for the memory-safety classes (the first-party ra8_epub .c / .cpp shim still
  # go through the full warning set). The C-only names would be a hard -Werror
  # in g++, so they are gated to the C TU via $<COMPILE_LANGUAGE:C>.
  # MINIZ_NO_STDIO/NO_TIME must be set for EVERY TU that includes miniz.h (the
  # vendored miniz.c *and* ra8_epub's TUs) so the header ABI matches -- miniz.c
  # references utime()/fopen() (absent on bare-metal newlib) only under the
  # default config, and a mismatched config between miniz.c and ra8_epub corrupts
  # the reader. ra8_epub uses only the in-memory reader, so the file path is
  # never needed.
  if(_ra8_epub_vendor)
    set_property(
      SOURCE ${_ra8_epub_vendor}
      APPEND
      PROPERTY COMPILE_OPTIONS ${_ra8_soup_wno_common}
               "$<$<COMPILE_LANGUAGE:C>:${_ra8_soup_wno_c}>"
    )
    target_compile_definitions(${_ra8_elf} PRIVATE MINIZ_NO_STDIO MINIZ_NO_TIME)
  endif()

  # miniz-only apps: same narrow vendored-TU warning suppression + config
  # defines, so miniz.c and every first-party TU that includes miniz.h share one
  # ABI. NO_ARCHIVE_APIS drops the malloc-using ZIP reader the firmware never
  # calls, leaving the heap-free tinfl DEFLATE core (driven from a static
  # decompressor). miniz.c is C, so it takes the C-only names too.
  if(_ra8_miniz_vendor)
    set_property(
      SOURCE ${_ra8_miniz_vendor}
      APPEND
      PROPERTY COMPILE_OPTIONS ${_ra8_soup_wno_common} ${_ra8_soup_wno_c}
    )
    target_compile_definitions(
      ${_ra8_elf} PRIVATE MINIZ_NO_STDIO MINIZ_NO_TIME MINIZ_NO_ARCHIVE_APIS
    )
  endif()

  # The vendored libwebp decoder (SOUP): a 60+ TU codec (VP8/VP8L + the
  # per-arch SIMD stubs) whose per-TU narrow -Wno tuning is not tractable, so
  # it takes a blanket -w + -fno-strict-aliasing (it type-puns through byte
  # buffers), matching tests/CMakeLists.txt. Its attacker-facing memory-safety
  # net is the ASan/UBSan libFuzzer harness (fuzz_ra8_webp), not -Werror.
  # -DRA8_WEBP_USE_ARENA activates the utils.c RA8 LOCAL PATCH that routes the
  # allocator through the heap-free ra8_webp bump arena (see docs/SOUP/libwebp.md).
  if(_ra8_webp_vendor)
    ra8_webp_apply_soup_flags(${_ra8_webp_vendor})
  endif()

  # The vendored xz-embedded decoder (SOUP): blanket -w + -fno-strict-aliasing
  # matching tests/CMakeLists.txt. Its attacker-facing memory-safety net is
  # the ASan/UBSan libFuzzer harness (fuzz_ra8_unarch_xz) plus the bounded
  # first-party wrapper (ra8_unarch_xz.c), which is held to the full warning
  # bar and charges every decode against the decompression-limits policy.
  if(_ra8_xz_vendor)
    set_source_files_properties(
      ${_ra8_xz_vendor} PROPERTIES COMPILE_OPTIONS "-w;-fno-strict-aliasing"
    )
  endif()
endmacro()
