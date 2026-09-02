# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie

idf_component_register(
  SRCS
  "src/mdl_service.c"
  "src/ra8_c6link_mdl_service.c"
  "src/ra8_media_download.pb-c.c"
  INCLUDE_DIRS
  "include"
  "../../../common/protobuf-c"
  REQUIRES
  esp_http_client
  mbedtls
  # The only upstream reference has a weak fallback in the same object as its
  # caller. Pull every component object in so the strong service hook is present
  # for the linker to select instead of silently accepting that fallback.
  WHOLE_ARCHIVE
)

# The generated protobuf-c translation unit carries NO warning suppression on
# this side. It used to take -Wno-cast-qual and -Wno-cast-align, mirroring the
# RA8-side treatment in cmake/ra8_app/sources.cmake. Those two classes really do
# fire on that file -- but only when they are ENABLED, and esp-idf never enables
# them: v5.5.4's default set (tools/cmake/build.cmake) is -Wall -Werror -Wextra
# plus six -Wno-error= relaxations, and neither -Wcast-qual nor -Wcast-align is
# in -Wall or -Wextra. coprocessor/esp32c6/sdkconfig.defaults adds no warning
# option either. Measured with the pinned riscv32-esp-elf-gcc 14.2.0 under that
# exact option set: the file compiles with zero diagnostics whether the two
# flags are present or absent, and forcing -Wcast-qual -Wcast-align on produces
# the 9 + 2 findings the RA8-side flags exist for. Both flags were therefore
# masking nothing on this target and are deleted.
target_compile_options(${COMPONENT_LIB} PRIVATE -std=gnu23)
