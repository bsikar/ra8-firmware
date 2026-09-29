# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
#
# ra8_add_ns_image(): the Non-Secure half of a dual-image TrustZone product.
#
# ra8_add_app() scaffolds a single image well. The moment a product needs two,
# the author has had to hand-write the same ~108 lines in every app: the CMSE
# import-library handshake at both ends, the NS target's warning profile, the
# build ordering, and the objcopy-twice plus merge_ihex staple with its
# --remove-section=.option_setting* flag (issue #759 item 1).
#
# Two of those were not merely repetitive, they were wrong per app:
#
#   * The NS image is a raw add_executable, so it compiled WITHOUT
#     -Wall/-Wextra/-Werror unless the author remembered one extra line. A
#     discarded [[nodiscard]] ra8_err_t on half the product could not fail the
#     build. Here the profile is applied by construction.
#   * The import library is written by the Secure link as a side effect of
#     --out-implib and then named as a bare positional link argument on the NS
#     target, which CMake cannot see. add_dependencies orders the two targets
#     but declares no rule for the FILE, so a clean `ninja <app>_ns.elf` dies
#     with "missing and no known rule to make it" and only succeeds if the
#     Secure image happens to have been built first. Declaring it as a
#     BYPRODUCT of the Secure link gives the generator the rule it needs.
#
# Usage:
#
#   ra8_add_ns_image(
#     SECURE_TARGET ra8d2-ereader.elf
#     NAME          ra8d2-ereader_ns
#     LINKER        ${RA8_REPO_ROOT}/libs/ra8_board_ek_ra8d2/ld/ns_image_sram.ld
#     STACK_BYTES   2200
#     SOURCES       src/ns_main.c ...
#     INCLUDES      ...
#     DEFINES       ...
#     LINK_LIBS     threadx_ns
#     MERGED_HEX    ra8d2-ereader.hex
#   )
#
# An app that needs a layout this does not express keeps writing it by hand;
# the point is that doing so becomes a deliberate act with a reason.

include_guard(GLOBAL)

function(ra8_add_ns_image)
  set(_opts)
  set(_one SECURE_TARGET NAME LINKER STACK_BYTES MERGED_HEX IMPLIB)
  set(_multi SOURCES INCLUDES DEFINES LINK_LIBS COMPILE_OPTIONS LINK_OPTIONS)
  cmake_parse_arguments(_NS "${_opts}" "${_one}" "${_multi}" ${ARGN})

  foreach(_req SECURE_TARGET NAME LINKER SOURCES)
    if(NOT _NS_${_req})
      message(FATAL_ERROR "ra8_add_ns_image(): ${_req} is required")
    endif()
  endforeach()
  if(NOT TARGET ${_NS_SECURE_TARGET})
    message(
      FATAL_ERROR
        "ra8_add_ns_image(): SECURE_TARGET ${_NS_SECURE_TARGET} does not exist "
        "yet. Call ra8_add_app() for the Secure image first."
    )
  endif()
  if(NOT EXISTS "${_NS_LINKER}")
    message(FATAL_ERROR "ra8_add_ns_image(): LINKER ${_NS_LINKER} does not exist")
  endif()

  set(_ns_elf ${_NS_NAME}.elf)
  if(NOT _NS_IMPLIB)
    set(_NS_IMPLIB ${CMAKE_CURRENT_BINARY_DIR}/${_NS_NAME}_cmse_import.o)
  endif()
  if(NOT _NS_STACK_BYTES)
    set(_NS_STACK_BYTES 2200)
  endif()

  # ---- Secure side of the handshake ---------------------------------------
  target_link_options(
    ${_NS_SECURE_TARGET} PRIVATE -Wl,--cmse-implib -Wl,--out-implib=${_NS_IMPLIB}
  )
  # The link writes the import library itself; this records that fact for the
  # generator so the NS link has a rule for the file rather than a bare
  # positional input that only resolves by luck of build order.
  add_custom_command(
    TARGET ${_NS_SECURE_TARGET}
    POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E true
    BYPRODUCTS ${_NS_IMPLIB}
    COMMENT "CMSE import library: ${_NS_IMPLIB}"
  )

  # ---- Non-Secure image ---------------------------------------------------
  # Every NS image carries the RoT header the Secure verifier looks for at
  # k_ra8_tz_ns_rot_header_offset. It is a C object rather than LONG() words in
  # the linker script (#759), so the magic and layout come from
  # ra8_ns_rot_header_t, and the link needs the translation unit plus the header
  # it reads those from. Added here, not asked of every caller: forgetting it is
  # an undefined g_ra8_ns_rot_header at link, which is loud but pointless.
  set(_ns_rot_dir ${RA8_REPO_ROOT}/libs/ra8_tz_secure_boot)
  add_executable(${_ns_elf} ${_NS_SOURCES} ${_ns_rot_dir}/ns/ra8_ns_rot_header.c)
  target_include_directories(${_ns_elf} PRIVATE ${_ns_rot_dir}/inc)
  if(_NS_DEFINES)
    target_compile_definitions(${_ns_elf} PRIVATE ${_NS_DEFINES})
  endif()
  if(_NS_INCLUDES)
    target_include_directories(${_ns_elf} PRIVATE ${_NS_INCLUDES})
  endif()
  target_compile_options(${_ns_elf} PRIVATE -fshort-enums -ffreestanding ${_NS_COMPILE_OPTIONS})
  if(_NS_LINK_LIBS)
    target_link_libraries(${_ns_elf} PRIVATE ${_NS_LINK_LIBS})
  endif()

  # The warning profile the raw add_executable does not get. This is the whole
  # reason the helper applies it rather than documenting it.
  ra8_target_enable_project_warnings(${_ns_elf} STACK_USAGE_BYTES ${_NS_STACK_BYTES})

  target_link_options(
    ${_ns_elf}
    PRIVATE
    -nostartfiles
    -T${_NS_LINKER}
    -Wl,--Map=${_NS_NAME}.map
    ${_NS_IMPLIB}
    ${_NS_LINK_OPTIONS}
  )
  set_target_properties(${_ns_elf} PROPERTIES LINK_DEPENDS "${_NS_LINKER};${_NS_IMPLIB}")
  add_dependencies(${_ns_elf} ${_NS_SECURE_TARGET})

  # ---- Staple -------------------------------------------------------------
  # --remove-section=.option_setting* on the Secure image only: the option
  # setting memory is programmed once from the Secure hex, and leaving it in
  # makes the merged record set overlap. This flag was folk knowledge repeated
  # in every dual-image app.
  if(_NS_MERGED_HEX)
    # merge_ihex is a first-party build tool whose implementation now lives in
    # Zig (epic #858); scripts/gen/merge_ihex.py is gone. ra8_use_merge_ihex()
    # builds it for the host and orders it ahead of this target.
    include("${RA8_REPO_ROOT}/cmake/ra8_zig_tool.cmake")
    ra8_use_merge_ihex(${_ns_elf})
    add_custom_command(
      TARGET ${_ns_elf}
      POST_BUILD
      COMMAND ${CMAKE_OBJCOPY} --remove-section=.option_setting* -O ihex
              $<TARGET_FILE:${_NS_SECURE_TARGET}> ${_NS_NAME}_secure_part.hex
      COMMAND ${CMAKE_OBJCOPY} -O ihex $<TARGET_FILE:${_ns_elf}> ${_NS_NAME}_part.hex
      COMMAND $<TARGET_FILE:ra8_zig::merge_ihex> ${_NS_NAME}_secure_part.hex
              ${_NS_NAME}_part.hex ${_NS_MERGED_HEX}
      BYPRODUCTS ${_NS_NAME}_secure_part.hex ${_NS_NAME}_part.hex ${_NS_MERGED_HEX}
      VERBATIM
      COMMENT "Merging Secure + Non-Secure images into ${_NS_MERGED_HEX}"
    )
  endif()
endfunction()
