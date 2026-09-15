// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

use firmware_report_provider::{ReportSummary, summarize};

#[test]
fn summarizes_empty_image() {
    assert_eq!(
        summarize(&[]),
        ReportSummary {
            byte_count: 0,
            zero_count: 0,
            erased_count: 0,
            fnv1a64: 0xcbf2_9ce4_8422_2325,
        }
    );
}

#[test]
fn summarizes_boundary_byte_values() {
    let summary = summarize(&[0, 1, 0xff, 0, 0xff]);
    assert_eq!(summary.byte_count, 5);
    assert_eq!(summary.zero_count, 2);
    assert_eq!(summary.erased_count, 2);
    assert_eq!(summary.fnv1a64, 0xf8c7_51d3_78ed_45b6);
}

#[test]
fn summarizes_known_text() {
    assert_eq!(summarize(b"hello").fnv1a64, 0xa430_d846_80aa_bd0b);
}
