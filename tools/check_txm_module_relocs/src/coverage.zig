//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Sites against records: is every data word that holds an address listed
//! in the module's rebase table, and does the table list nothing else?
//!
//! The relocations stay in a linked module whether or not anything rebases
//! them, so finding one is not the failure. Finding one the table does not
//! name is. So is the reverse: the start-up adds a load delta to whatever
//! the table names, and a record with no address behind it would corrupt
//! an ordinary word, as would a record listed twice.

const arm = @import("arm.zig");
const check = @import("check.zig");
const records = @import("records.zig");

pub const Error = records.Error;

/// True when the records of the module cover `site`: they name its exact
/// address, and it is a whole word, the only thing a delta can be added to.
pub fn covers(table: records.Records, site: check.Finding) bool {
    return arm.isWholeWord(site.kind) and table.has(site.address);
}

/// What is wrong with record `index`, if anything.
pub const Fault = enum { names_no_site, listed_twice };

pub fn fault(image: []const u8, table: records.Records, index: usize) Error!?Fault {
    const address = table.at(index);
    for (0..index) |earlier| {
        if (table.at(earlier) == address) return .listed_twice;
    }
    var walk = try check.Iterator.init(image);
    while (try walk.next()) |site| {
        if (site.address == address and arm.isWholeWord(site.kind)) return null;
    }
    return .names_no_site;
}
