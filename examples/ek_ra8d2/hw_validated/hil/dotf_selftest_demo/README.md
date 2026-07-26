# dotf_selftest_demo

DOTF (**Decryption On The Fly**) bring-up + AES built-in **self-test** for the
bare EK-RA8D2 EVM. Exercises the safe, key-free half of the `ra8_dotf` driver
(#127).

## What it does

Brings up SCI8 + LEDs, powers on the DOTF block, and runs its AES self-test
without ever arming a channel:

1. `ra8_dotf_init` -- clock-ungate both DOTF channels (`MSTPB16` / `MSTPB17`,
   shared with OSPI0 / OSPI1) and reset each `REG00` to its bypass value.
   Neither channel is armed; the AES core stays in transparent pass-through.
2. `ra8_dotf_run_self_test` on channels 0 and 1 -- set `REG00` bit 20 (the
   built-in AES self-test trigger), poll a bounded number of times for it to
   clear, and snapshot `REG00`.
3. `ra8_dotf_get_status` -- read `REG00` back for the bench probe.

Each second: `dotf: ch0/1 init=ok selftest=run ok=Y`. LED1 toggles while
healthy; LED2 on a fault. `g_dotf_ok` / `g_dotf_init_err` / `g_dotf_reg00` /
`g_dotf_st0_snap` / `g_dotf_st1_snap` / `g_dotf_heartbeat` mirror the result
for headless probing.

No external hardware required.

### What this demo deliberately does NOT do (safety)

The full DOTF flow to actually decrypt XiP code is
`init -> install_key -> set_iv -> set_region -> enable -> jump into XiP`. This
demo stops after `init` + `self_test` + `get_status`. It **never**:

- installs a key (that needs an `ra8_rsip`-wrapped key handle), stages an IV,
  or programs a conversion region;
- calls `ra8_dotf_enable` -- arming the AES core over a live XiP window would
  fault the next instruction fetch (HUM Ch 45 warning);
- writes any OTP, option-setting, security-attribution, or flash byte.

Both channels stay in transparent bypass for the whole run, so the demo has
**no persistent side effects** -- it is a read-mostly bring-up probe. (The
shared `MSTPB16/17` are toggled, but that is volatile module-clock state, not
a persistent fuse.)

## Why this is in hw_pending

`tools/ra8_emulator` does **not** model the DOTF APB window (`0x4026_8800` /
`0x4026_8900`), so the self-test register reads are not backed by a real AES
core -- the bring-up + self-test calls return `k_ra8_ok` and the demo reports
`ok=Y`, but the genuine AES BIST pass/fail result and the in-place decryption
path only exist on silicon. The headless verdict therefore gates only on the
driver calls succeeding; the opaque `REG00` snapshots are reported via
`g_dotf_st0_snap` / `g_dotf_st1_snap` for on-silicon probing. The real AES
self-test result can only be confirmed on the bench, so the app stays in
`hw_pending/`.

## Registers (HUM R01UH1065EJ0130 Rev.1.30, Ch 45 "DOTF")

- DOTF block overview + AES self-test feature: HUM Ch 45.1 p 3048
  ("Supports self-test function").
- `REG00` (AES core / control, +0x080 in the channel window) bit 20 is the
  self-test trigger; decoded by `ra8_dotf_run_self_test` / `ra8_dotf_get_status`
  (HUM Ch 45.3 "Register Descriptions" p 3049).
- Shared module-stop with OSPI: HUM Ch 45.6.1 "Module-stop Function" p 3050
  (`MSTPB16` = DOTF0+OSPI0, `MSTPB17` = DOTF1+OSPI1).

## On-silicon bench plan

1. `make dotf_selftest_demo`, then flash the EK-RA8D2.
2. Confirm the bring-up: `dotf: ch0/1 init=ok selftest=run ok=Y` once a second
   (`g_dotf_ok == 1`, `g_dotf_init_err == 0`).
3. **AES self-test (the real acceptance, needs silicon):** capture the post-
   trigger `REG00` snapshots (`g_dotf_st0_snap` / `g_dotf_st1_snap`) and
   confirm the AES BIST completion encoding documented for the silicon
   (the bit-20 auto-clear + the pass/fail field). On the emulator these read
   back `0`; on hardware they carry the real BIST result.
4. Once the on-silicon self-test result is confirmed, move the app to
   `hw_validated/hil/`.

Build / flash:

```
make dotf_selftest_demo
make -C examples/ek_ra8d2/hw_pending/dotf_selftest_demo flash
```
