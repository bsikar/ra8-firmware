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

set_source_files_properties("src/ra8_media_download.pb-c.c" PROPERTIES COMPILE_OPTIONS "-w")
target_compile_options(${COMPONENT_LIB} PRIVATE -std=gnu23)
