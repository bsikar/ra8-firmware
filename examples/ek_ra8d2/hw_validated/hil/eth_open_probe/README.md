# eth_open_probe

The smallest program that reaches `ra8_eth_open()` on silicon, and the
bare-metal reproducer for the fault that turned out to be behind both
tracker issue #524 and issue #499.

```
ra8_cgc_init + ra8_mstp_init + ra8_time_init + SCI8 console
ra8_board_ethernet_init()
ra8_eth_open(&cfg)          /* channel 1 -- the EK-RA8D2 RJ45 is ETHA1 */
```

No RTOS, no IP stack, no packet pool, no wire peer. It reaches the verdict
line in about a second, and a debugger can watch the whole thing.

## What it is guarding

`SRAMWTSC.WTEN` resets to 0, and nothing in this tree ever programmed it.
`ra8_cgc_init` takes ICLK to 250 MHz, and HUM Ch 58.3.7 "Wait State" p 3540
requires a wait cycle above half the rated maximum: *"when the wait is not
inserted, the operation is not guaranteed"*.

The way that failed was not a hang. It was **one bit dropped out of one value
read back from SRAM**, with the SRAM itself holding the correct word. In this
app the value is `s_gwca_state.tx_chain`; the CPU loaded `0x200664B0` where
memory held `0x220664B0` -- address bit 25 clear, which is the bit that
separates on-chip SRAM at `0x22000000` from the CPU0-DTCM window at
`0x20000000` (HUM Ch 5 "Address Space" pp 236-243). The pointer therefore
landed in the reserved gap above the 64 KiB DTCM, and the first descriptor
write BusFaulted.

It is not a stuck bit and it is not the debugger's view being stale: single-
stepping that same `LDR` under J-Link produces the correct value every time,
which is the giveaway that the fault is a read that does not meet timing at
full speed rather than a memory that holds the wrong thing.

## Why the build carries a `.bss` pad

`RA8_ETH_PROBE_PAD_BYTES` (see `CMakeLists.txt`) forces a pad into the plain
`.bss` input section, which the shared linker script emits ahead of every
`-fdata-sections` `.bss.*`. Growing it slides the HAL's whole GWCA block --
`s_gwca_state`, both descriptor chains, both buffer pools -- that far up SRAM
and changes nothing else about the program. That makes "where the Ethernet
DMA structures live" a single build-time variable.

The default, 393168, reproduces #524's reported layout byte for byte:
`s_gwca_state` at `0x22060474`, `s_tx_pool_storage` at `0x220604B0`,
`s_tx_chain` at `0x220664B0`.

Whether a given layout tripped the marginal read was perfectly repeatable but
not uniform across SRAM. A 16-point sweep in 32 KiB steps, power-cycled before
every measurement, failed only with the block in the 64 KiB page at
`0x22060000`:

| `s_gwca_state` | result | | `s_gwca_state` | result |
|---|---|---|---|---|
| `0x220004b4` .. `0x220584b4` (12 points) | PASS | | `0x220604b4` | **FAIL** |
| `0x220704b4`, `0x220784b4` | PASS | | `0x220684b4` | **FAIL** |

That pattern is a property of a timing failure -- which addresses and data
patterns happen to be marginal -- not a placement rule anyone can look up, so
treat the layout dependence as an observation, not a specification. **The
authoritative regression guard is the host unit test**
`test_init_programs_sram_wait_state` in `tests/test_ra8_cgc.c`, which asserts
that `ra8_cgc_init` programs `SRAMWTSC.WTEN` at all. This app is the on-silicon
corroboration, and it only keeps that role for as long as `.bss` does not shift
the GWCA block out of the marginal page.

## Running it

```sh
make eth_open_probe                                # build
bash scripts/hil/all.sh --only eth_open_probe      # flash + verify on the bench
make emu-eth_open_probe                            # same ELF, headless emulator
```

Expected transcript:

```
eth-open-probe: boot
eth-open-probe: cpuclk0=0x3B9ACA00 bss_pad=0x22000030 pad_bytes=0x0005FFD0
eth-open-probe: board_eth rc=0
eth-open-probe: calling ra8_eth_open ch=1
eth-open-probe: eth_open rc=0
eth-open-probe: PASS
```

Before the wait state landed, the last two lines were replaced by the shared
fault decoder's dump -- `exception=5` (BusFault), `cfsr =1024`
(BFSR.IMPRECISERR), `r0 =537289904` = `0x200664B0`.

`ra8_emulator` does not model the SRAM access timing, so an EIL run always
reaches PASS. That is expected: the emulator can prove the open path is
functionally correct, and only the bench can prove the memory system is.
