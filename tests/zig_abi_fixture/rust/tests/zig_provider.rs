// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie
//! Cross-language acceptance vectors for Rust consuming the Zig C ABI.

use ra8_zig_abi_fixture::{AbiResult, Config, Fixture, apply};

#[test]
fn rust_consumes_the_zig_provider_through_the_c_abi() {
    assert_eq!(size_of::<Config>(), 8);
    assert_eq!(align_of::<Config>(), 4);
    assert_eq!(std::mem::offset_of!(Config, value), 0);
    assert_eq!(std::mem::offset_of!(Config, factor), 4);
    assert_eq!(std::mem::offset_of!(Config, enabled), 6);
    assert_eq!(std::mem::offset_of!(Config, reserved0), 7);

    let config = Config {
        value: 7,
        factor: 3,
        enabled: 1,
        reserved0: 0,
    };
    assert_eq!(apply(&config), Ok(21));
    assert_eq!(
        apply(&Config {
            enabled: 2,
            ..config
        }),
        Err(AbiResult::InvalidArgument)
    );

    assert!(matches!(
        Fixture::create_with_forced_allocation_failure_for_test(),
        Err(AbiResult::NoMemory)
    ));
    let mut fixture = Fixture::create().expect("bounded Zig handle");
    assert!(matches!(Fixture::create(), Err(AbiResult::NoMemory)));

    let input = [1_u8, 2, 3, 4];
    let mut short = [0xA5_u8; 3];
    assert_eq!(
        fixture.copy_into(&input, &mut short),
        Err(AbiResult::Length)
    );
    assert_eq!(short, [0xA5; 3]);
    let mut output = [0xA5_u8; 8];
    assert_eq!(fixture.copy_into(&input, &mut output), Ok(input.len()));
    assert_eq!(&output[..input.len()], &input);
    assert_eq!(&output[input.len()..], &[0xA5; 4]);

    assert!(matches!(
        fixture.owned_bytes_with_forced_allocation_failure_for_test(&input),
        Err(AbiResult::NoMemory)
    ));
    {
        let mut owned = fixture.owned_bytes(&input).expect("Zig-owned result");
        assert_eq!(owned.as_slice(), input);
        owned.release().expect("explicit Zig-owned result release");
        assert_eq!(owned.release(), Err(AbiResult::State));
    }
    {
        let owned = fixture
            .owned_bytes(&input)
            .expect("implicitly released result");
        assert_eq!(owned.as_slice(), input);
    }
    let mut reacquired = fixture.owned_bytes(&input).expect("pool restored by Drop");
    reacquired.release().expect("release reacquired bytes");
    drop(reacquired);
    fixture.close().expect("explicit Zig handle release");
    assert_eq!(fixture.close(), Err(AbiResult::State));

    let implicit_fixture = Fixture::create().expect("implicitly released handle");
    drop(implicit_fixture);
    let mut reacquired_fixture = Fixture::create().expect("handle pool restored by Drop");
    reacquired_fixture
        .close()
        .expect("release reacquired handle");
    assert_concurrent_serialization();
}

fn assert_concurrent_serialization() {
    use std::sync::{Arc, Barrier, mpsc};

    let start = Arc::new(Barrier::new(3));
    let finish = Arc::new(Barrier::new(2));
    let (result_tx, result_rx) = mpsc::channel();
    let workers = (0..2)
        .map(|_| {
            let start = Arc::clone(&start);
            let finish = Arc::clone(&finish);
            let result_tx = result_tx.clone();
            std::thread::spawn(move || {
                start.wait();
                let outcome = Fixture::create();
                finish.wait();
                result_tx
                    .send(outcome.is_ok())
                    .expect("report concurrent result");
            })
        })
        .collect::<Vec<_>>();
    start.wait();
    drop(result_tx);
    let successes = result_rx.into_iter().filter(|success| *success).count();
    for worker in workers {
        worker.join().expect("concurrent ABI worker");
    }
    assert_eq!(successes, 1);
}
