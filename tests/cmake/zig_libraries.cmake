# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
#
# Libraries whose implementation now lives in Zig behind their unchanged C
# header. Each one is built by `zig build` and linked into ra8_core_hal's
# consumers, so the existing C unit tests exercise the Zig object code without
# a single test edit.

include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/zig_library.cmake)

set(RA8_BOX_ZIG_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../libs/ra8_box")

ra8_add_zig_library(
  NAME
  ra8_box
  ZIG_ROOT
  ${RA8_BOX_ZIG_ROOT}
  LIBRARY_NAME
  ra8_box
)

# ra8_core_hal carries the Zig library's include directory so the C sources
# folded into it still see the unchanged public header.
target_link_libraries(ra8_core_hal PUBLIC ra8_zig::ra8_box)

# The link above does NOT reach the test executables: every host test consumes
# the core objects as $<TARGET_OBJECTS:ra8_core_hal>, which takes the compiled
# objects and drops the link interface, leaving the Zig ABI symbols undefined
# at link time. Attach the archives at directory scope instead, so every test
# target declared after this point links them. Each further C-to-Zig port adds
# its archive here alongside ra8_box.
link_libraries(ra8_zig::ra8_box)
