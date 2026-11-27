// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie
//! Banner formatter shared across host unit tests and bare-metal EK-RA8D2
//! firmware entry point (`target_os = "none"`).

use crate::vectors::PairResult;

const PASS_PREFIX: &[u8] = b"RA8_BOX_PAIR_RS PASS ";
const FAIL_PREFIX: &[u8] = b"RA8_BOX_PAIR_RS FAIL ";
const CRLF: &[u8] = b"\r\n";

fn push_bytes(dst: &mut [u8], pos: &mut usize, src: &[u8]) {
    for &byte in src {
        if *pos >= dst.len() {
            break;
        }
        dst[*pos] = byte;
        *pos += 1;
    }
}

fn push_u16(dst: &mut [u8], pos: &mut usize, mut value: u16) {
    let mut digits = [0_u8; 6];
    let mut count = 0_usize;
    loop {
        let rem = u8::try_from(value % 10).unwrap_or(0);
        digits[count] = b'0' + rem;
        value /= 10;
        count += 1;
        if value == 0 || count == digits.len() {
            break;
        }
    }
    while count > 0 && *pos < dst.len() {
        count -= 1;
        dst[*pos] = digits[count];
        *pos += 1;
    }
}

/// Format the deterministic `HIL_EXPECT` PASS or FAIL UART verdict line into `buf`.
#[must_use]
pub fn format_banner<'a>(buf: &'a mut [u8], result: &PairResult) -> &'a [u8] {
    let mut pos = 0_usize;
    if result.failed == 0 {
        push_bytes(buf, &mut pos, PASS_PREFIX);
        push_u16(buf, &mut pos, result.passed);
        push_bytes(buf, &mut pos, b"/");
        push_u16(buf, &mut pos, result.total);
    } else {
        let id = result.failed_id.unwrap_or("unknown");
        push_bytes(buf, &mut pos, FAIL_PREFIX);
        push_u16(buf, &mut pos, result.failed);
        push_bytes(buf, &mut pos, b"/");
        push_u16(buf, &mut pos, result.total);
        push_bytes(buf, &mut pos, b" ");
        push_bytes(buf, &mut pos, id.as_bytes());
    }
    push_bytes(buf, &mut pos, CRLF);
    &buf[..pos]
}

#[cfg(target_os = "none")]
mod target_entry {
    #![expect(
        unsafe_code,
        reason = "bare-metal reset entry point, linker symbol, and WFI halt instruction"
    )]

    use core::arch::asm;
    use core::panic::PanicInfo;
    use core::sync::atomic::{AtomicU32, Ordering};

    use crate::ffi::board;
    use crate::firmware::format_banner;
    use crate::vectors::{K_RA8_OK, run_all};

    const CONSOLE_BAUD: u32 = 115_200;
    const BANNER_PERIOD_MS: u32 = 1_000;
    const BANNER_BUF_BYTES: usize = 96;

    /// Observable pass latch for debugger / memprobe inspection.
    #[unsafe(no_mangle)]
    pub static g_ra8_box_pair_rs_pass: AtomicU32 = AtomicU32::new(0);

    fn panic_halt() -> ! {
        loop {
            unsafe {
                asm!("wfi", options(nomem, nostack));
            }
        }
    }

    #[unsafe(no_mangle)]
    pub extern "C" fn main() -> ! {
        if board::bringup(CONSOLE_BAUD) != K_RA8_OK {
            panic_halt();
        }
        board::enable_global_irqs();

        let result = run_all();
        let mut banner = [0_u8; BANNER_BUF_BYTES];
        let line = format_banner(&mut banner, &result);

        if result.failed != 0 {
            g_ra8_box_pair_rs_pass.store(0, Ordering::Release);
            let _ = board::uart_write(line);
            panic_halt();
        }

        g_ra8_box_pair_rs_pass.store(1, Ordering::Release);
        loop {
            let _ = board::uart_write(line);
            if board::led_toggle(board::LED1_ID) != K_RA8_OK {
                break;
            }
            board::delay_ms(BANNER_PERIOD_MS);
        }

        panic_halt();
    }

    #[panic_handler]
    fn panic(_info: &PanicInfo<'_>) -> ! {
        panic_halt();
    }
}
