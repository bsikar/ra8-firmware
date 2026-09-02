# eth_open_probe

The smallest program that reaches `ra8_eth_open()` on silicon: clocks, module
stop, time, console, `ra8_board_ethernet_init()`, then open on channel 1 -- the
EK-RA8D2 RJ45 is ETHA1. No RTOS, no IP stack, no packet pool, no wire peer. It
reaches a verdict in about a second and a debugger can watch the whole thing.

It is the bare-metal reproducer for the fault behind both #524 and #499.

## What it is guarding

`SRAMWTSC.WTEN` resets to 0, and nothing in this tree ever programmed it.
`ra8_cgc_init` takes ICLK to 250 MHz, and HUM Ch 58.3.7 "Wait State" p 3540
requires a wait cycle above half the rated maximum: *"when the wait is not
inserted, the operation is not guaranteed"*.

The way that failed was not a hang. It was **one bit dropped out of one value
read back from SRAM**, with the SRAM itself holding the correct word. Here the
value is `s_gwca_state.tx_chain`: the CPU loaded `0x200664B0` where memory held
`0x220664B0` -- address bit 25 clear, which is the bit separating on-chip SRAM
at `0x22000000` from the CPU0-DTCM window at `0x20000000` (HUM Ch 5 "Address
Space" pp 236-243). The pointer therefore landed in the reserved gap above the
64 KiB DTCM, and the first descriptor write BusFaulted.

It is not a stuck bit, and it is not the debugger's view being stale:
single-stepping that same `LDR` under J-Link produces the correct value every
time. That disagreement between the stepped read and the full-speed read is the
giveaway that the fault is a read not meeting timing, rather than a memory
holding the wrong thing.

## Why the build carries a `.bss` pad

`RA8_ETH_PROBE_PAD_BYTES` forces a pad into the plain `.bss` input section,
which the shared linker script emits ahead of every `-fdata-sections` `.bss.*`.
Growing it slides the HAL's whole GWCA block -- the state struct, both
descriptor chains and both buffer pools -- that far up SRAM and changes nothing
else about the program, which makes "where the Ethernet DMA structures live" a
single build-time variable. The default reproduces #524's reported layout byte
for byte, with `s_gwca_state` at `0x22060474` and `s_tx_chain` at `0x220664B0`.

Whether a given layout tripped the marginal read was perfectly repeatable but
not uniform across SRAM: a sweep in 32 KiB steps, power-cycled before every
measurement, failed only with the block sitting in the 64 KiB page at
`0x22060000`. That pattern is a property of a timing failure -- which addresses
and data patterns happen to be marginal -- and not a placement rule anyone can
look up, so treat the layout dependence as an observation, never a
specification.

**The authoritative regression guard is the host unit test**
`test_init_programs_sram_wait_state` in `tests/hal/src/test_ra8_cgc.c`, which asserts
that `ra8_cgc_init` programs `SRAMWTSC.WTEN` at all. This app is the on-silicon
corroboration, and it keeps that role only for as long as `.bss` does not shift
the GWCA block out of the marginal page.

Before the wait state landed, the run ended in the shared fault decoder's dump
instead of a verdict: BusFault, `BFSR.IMPRECISERR`, and `r0` holding the
bit-25-clear `0x200664B0`. `ra8_emulator` does not model SRAM access timing, so
an emulator run always passes -- as expected. The emulator can prove the open
path is functionally correct; only the bench can prove the memory system is.
