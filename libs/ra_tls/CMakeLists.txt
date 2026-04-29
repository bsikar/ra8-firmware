#
# libs/ra_tls -- minimal TLS 1.2 client on top of ra_sce + ra_net.
#
# This library is intentionally bounded scope -- one cipher suite
# (TLS_RSA_WITH_AES_128_GCM_SHA256, RFC 5246 sec 9 mandatory-to-
# implement) and no X.509 chain validation beyond a server-cert
# SHA-256 pinning hash. Real production TLS clients should depend
# on a vetted stack (mbedTLS, wolfSSL, BearSSL) -- this exists to
# give the firmware a starting point without dragging in ~50 KLOC.
#
# Copyright (c) 2026 Brighton Sikarskie
# SPDX-License-Identifier: MIT
#

# Intentionally empty -- consumers (tests, examples) reach into
# inc/ and src/ directly, mirroring libs/ra_net. When an app starts
# to depend on ra_tls, wire up an OBJECT or INTERFACE library here.
