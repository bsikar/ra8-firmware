// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

use firmware_pipeline_rust::{RustSummary, analyze};

#[test]
fn rejects_empty_image() {
    assert_eq!(analyze(&[]), None);
}

#[test]
fn analyzes_known_text() {
    assert_eq!(
        analyze(b"hello"),
        Some(RustSummary {
            byte_count: 5,
            zero_count: 0,
            erased_count: 0,
            fnv1a64: 0xa430_d846_80aa_bd0b,
        })
    );
}

#[test]
fn counts_boundary_values() {
    let result = analyze(&[0, 0xff, 7]).expect("non-empty input");
    assert_eq!(result.zero_count, 1);
    assert_eq!(result.erased_count, 1);
}
