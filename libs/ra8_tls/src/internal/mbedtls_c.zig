//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The one place that reads the vendored Mbed TLS and PSA headers. Every
//! other file in the library sees the names below, never an `MBEDTLS_*`
//! macro, which is what keeps the rest of the port host-testable.
//!
//! `build.zig` supplies the include roots and the two config-file defines
//! that `cmake/mbedtls.cmake` already gives the C compiler.

pub const c = @cImport({
    @cInclude("mbedtls/error.h");
    @cInclude("mbedtls/ssl.h");
    @cInclude("mbedtls/x509_crt.h");
    @cInclude("psa/crypto.h");
});

pub const psa_success = c.PSA_SUCCESS;

pub const want_read = c.MBEDTLS_ERR_SSL_WANT_READ;
pub const want_write = c.MBEDTLS_ERR_SSL_WANT_WRITE;
pub const internal_error = c.MBEDTLS_ERR_SSL_INTERNAL_ERROR;
pub const peer_close_notify = c.MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY;

pub const is_client = c.MBEDTLS_SSL_IS_CLIENT;
pub const transport_stream = c.MBEDTLS_SSL_TRANSPORT_STREAM;
pub const preset_default = c.MBEDTLS_SSL_PRESET_DEFAULT;

pub const verify_none = c.MBEDTLS_SSL_VERIFY_NONE;
pub const verify_optional = c.MBEDTLS_SSL_VERIFY_OPTIONAL;
pub const verify_required = c.MBEDTLS_SSL_VERIFY_REQUIRED;
