//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Aggregator root for the Zig book archive (RA8FW-1242, RA8FW-1244,
//! RA8FW-1247, RA8FW-1248, RA8FW-1249): pulls in every ported book TU so its C ABI
//! exports land in libra8_book.a. Each
//! export is referenced by address, so none can be lazily dropped the way
//! the ra8_imgdec archive dropped its hook exports (Bug RA8FW-1243).

const wire = @import("book_stream_wire.zig");
const chunked = @import("book_chunked.zig");
const chunked_validate = @import("book_chunked_validate.zig");
const stream = @import("book_stream.zig");
const paged = @import("book_paged.zig");

comptime {
    _ = &wire.priv_book_stream_read;
    _ = &wire.priv_book_stream_read_validate_header;
    _ = &wire.priv_book_stream_string_ref;
    _ = &wire.priv_book_stream_nonempty_string_ref;
    _ = &wire.priv_book_stream_validate_string_envelope;
    _ = &chunked.book_chunked_open;
    _ = &chunked.book_chunked_read;
    _ = &chunked_validate.book_chunked_validate_strict;
    _ = &stream.book_validate_stream_strict;
    _ = &stream.priv_book_stream_validate_metadata;
    _ = &stream.priv_book_stream_validate_element;
    _ = &stream.priv_book_stream_validate_text;
    _ = &stream.priv_book_stream_validate_styles;
    _ = &paged.book_src_resident;
    _ = &paged.book_src_paged;
    _ = &paged.book_src_read;
    _ = &paged.book_src_image;
    _ = &paged.book_src_image_rect;
    _ = &paged.book_chapter_text_src;
    _ = &paged.book_src_prefetch_chapter;
}
