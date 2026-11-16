//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Aggregator root for the Zig book archive (RA8FW-1242, RA8FW-1244): pulls
//! in every ported book TU so its C ABI exports land in libra8_book.a. Each
//! export is referenced by address, so none can be lazily dropped the way
//! the ra8_imgdec archive dropped its hook exports (Bug RA8FW-1243).

const wire = @import("book_stream_wire.zig");
const chunked = @import("book_chunked.zig");

comptime {
    _ = &wire.priv_book_stream_read;
    _ = &wire.priv_book_stream_read_validate_header;
    _ = &wire.priv_book_stream_string_ref;
    _ = &wire.priv_book_stream_nonempty_string_ref;
    _ = &wire.priv_book_stream_validate_string_envelope;
    _ = &chunked.book_chunked_open;
    _ = &chunked.book_chunked_read;
}
