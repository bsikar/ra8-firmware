#
# port/lwip/CMakeLists.txt
#
# Defines the `lwip` static library: vendored lwIP core + APIs + netif
# helpers, compiled together with our ThreadX-backed sys_arch shim
# (`arch/sys_arch.c`) and our compiler glue (`arch/cc.h`,
# `lwipopts.h`).
#
# Source layout for this port (this directory):
#   port/lwip/
#     lwipopts.h           project-wide lwIP feature config
#     arch/cc.h            compiler glue (LWIP_PLATFORM_DIAG/ASSERT)
#     arch/sys_arch.h      ThreadX-backed typedefs (sys_sem_t, etc.)
#     arch/sys_arch.c      ThreadX-backed sys_* implementation
#     CMakeLists.txt       (this file)
#
# Vendored sources are pulled in via lwIP's own `src/Filelists.cmake`,
# which exports `lwipcore_SRCS`, `lwipcore4_SRCS`, `lwipapi_SRCS`,
# `lwipnetif_SRCS`, etc. We ingest the v4-only sets so the static lib
# matches the `LWIP_IPV6=0` toggle in lwipopts.h.
#
# Pulled in by `cmake/lwip.cmake` when `RA_USE_LWIP=ON`. Apps link
# against the `lwip` target; everything else (include dirs, the
# threadx dependency) flows through PUBLIC properties.
#
# Copyright (c) 2026 Brighton Sikarskie
# SPDX-License-Identifier: MIT
#

cmake_minimum_required(VERSION 3.20)

if(TARGET lwip)
    return()
endif()

if(NOT DEFINED RA_REPO_ROOT)
    get_filename_component(RA_REPO_ROOT
                           "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
endif()

set(LWIP_DIR "${RA_REPO_ROOT}/libs/third_party/lwip")
if(NOT EXISTS "${LWIP_DIR}/src/include/lwip/opt.h")
    message(FATAL_ERROR
        "lwIP vendor tree not found at ${LWIP_DIR} -- did the submodule "
        "fail to populate?")
endif()

# NOTE: lwIP's own `src/Filelists.cmake` is intentionally NOT included --
# it pulls in additional add_library() targets, runs find_package(Doxygen),
# and references `doc/doxygen/*.cmake.in` which is stripped from this
# vendored tree. Enumerating the sources here keeps the build narrow and
# matches the v4-only / no-apps / no-PPP toggles in lwipopts.h.
#
# The lists below mirror `lwipcore_SRCS`, `lwipcore4_SRCS`, `lwipapi_SRCS`
# and `lwipnetif_SRCS` from the upstream Filelists.cmake (lwIP v2.2.0).
# When updating the vendored lwIP, re-sync these against
# `libs/third_party/lwip/src/Filelists.cmake`.

set(_RA_LWIP_CORE_SRCS
    ${LWIP_DIR}/src/core/init.c
    ${LWIP_DIR}/src/core/def.c
    ${LWIP_DIR}/src/core/dns.c
    ${LWIP_DIR}/src/core/inet_chksum.c
    ${LWIP_DIR}/src/core/ip.c
    ${LWIP_DIR}/src/core/mem.c
    ${LWIP_DIR}/src/core/memp.c
    ${LWIP_DIR}/src/core/netif.c
    ${LWIP_DIR}/src/core/pbuf.c
    ${LWIP_DIR}/src/core/raw.c
    ${LWIP_DIR}/src/core/stats.c
    ${LWIP_DIR}/src/core/sys.c
    ${LWIP_DIR}/src/core/altcp.c
    ${LWIP_DIR}/src/core/altcp_alloc.c
    ${LWIP_DIR}/src/core/altcp_tcp.c
    ${LWIP_DIR}/src/core/tcp.c
    ${LWIP_DIR}/src/core/tcp_in.c
    ${LWIP_DIR}/src/core/tcp_out.c
    ${LWIP_DIR}/src/core/timeouts.c
    ${LWIP_DIR}/src/core/udp.c
)

set(_RA_LWIP_CORE4_SRCS
    ${LWIP_DIR}/src/core/ipv4/acd.c
    ${LWIP_DIR}/src/core/ipv4/autoip.c
    ${LWIP_DIR}/src/core/ipv4/dhcp.c
    ${LWIP_DIR}/src/core/ipv4/etharp.c
    ${LWIP_DIR}/src/core/ipv4/icmp.c
    ${LWIP_DIR}/src/core/ipv4/igmp.c
    ${LWIP_DIR}/src/core/ipv4/ip4_frag.c
    ${LWIP_DIR}/src/core/ipv4/ip4.c
    ${LWIP_DIR}/src/core/ipv4/ip4_addr.c
)

set(_RA_LWIP_API_SRCS
    ${LWIP_DIR}/src/api/api_lib.c
    ${LWIP_DIR}/src/api/api_msg.c
    ${LWIP_DIR}/src/api/err.c
    ${LWIP_DIR}/src/api/if_api.c
    ${LWIP_DIR}/src/api/netbuf.c
    ${LWIP_DIR}/src/api/netdb.c
    ${LWIP_DIR}/src/api/netifapi.c
    ${LWIP_DIR}/src/api/sockets.c
    ${LWIP_DIR}/src/api/tcpip.c
)

set(_RA_LWIP_NETIF_SRCS
    ${LWIP_DIR}/src/netif/ethernet.c
    ${LWIP_DIR}/src/netif/bridgeif.c
    ${LWIP_DIR}/src/netif/bridgeif_fdb.c
)

set(_RA_LWIP_VENDOR_SRCS
    ${_RA_LWIP_CORE_SRCS}
    ${_RA_LWIP_CORE4_SRCS}
    ${_RA_LWIP_API_SRCS}
    ${_RA_LWIP_NETIF_SRCS}
)

set(_RA_LWIP_SOURCES
    ${_RA_LWIP_VENDOR_SRCS}
    ${CMAKE_CURRENT_SOURCE_DIR}/arch/sys_arch.c
)

add_library(lwip STATIC ${_RA_LWIP_SOURCES})

target_include_directories(lwip SYSTEM PUBLIC
    ${LWIP_DIR}/src/include
)

# Project-side headers (lwipopts.h + arch/) take priority so cc.h /
# sys_arch.h / lwipopts.h come from this directory.
target_include_directories(lwip PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)

# `arch/sys_arch.c` includes "ra_log.h"; surface ra_core's headers so
# the lwip target builds standalone (without the consuming app having
# to splice ra_core into the lwip target's command line).
target_include_directories(lwip PRIVATE
    ${RA_REPO_ROOT}/libs/ra_core/inc
)

# ThreadX is a hard dependency: sys_arch.c uses tx_* APIs. We re-use
# the same `threadx` target that cmake/threadx.cmake builds.
target_link_libraries(lwip PUBLIC threadx)

# Vendor sources (the ~80 .c files under libs/third_party/lwip/src)
# do not compile under -Wall -Wextra -Werror. Quiet them entirely;
# our shim files compile under the project warning policy below.
set_source_files_properties(
    ${_RA_LWIP_VENDOR_SRCS}
    PROPERTIES COMPILE_FLAGS "-w"
)

# The shim itself is allowed to relax -Wunused-parameter only -- lwIP
# typedefs prevent us from naming every formal in some callbacks.
set_source_files_properties(
    ${CMAKE_CURRENT_SOURCE_DIR}/arch/sys_arch.c
    PROPERTIES COMPILE_FLAGS "-Wno-error=unused-parameter"
)

message(STATUS "lwIP: target configured (${LWIP_DIR})")
message(STATUS "lwIP: lwipopts.h    = ${CMAKE_CURRENT_SOURCE_DIR}/lwipopts.h")
message(STATUS "lwIP: sys_arch impl = ${CMAKE_CURRENT_SOURCE_DIR}/arch/sys_arch.c")
