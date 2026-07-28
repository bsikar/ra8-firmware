# touch_demo

Standalone **GoodIX GT911** capacitive-touch bring-up demo + **on-silicon HIL
gate** (`ra8_touch`, #122). Until now the GT911 driver was only exercised
*inside* `ereader_ui` (a `hw_validated/manual` app) -- there was no standalone
example and no CI gate for the touch driver itself.

## What it does

1. Brings up clocks / MSTP / the SCI8 J-Link OB console.
2. `ra8_touch_open()` -- the real driver: configures IIC_B channel 0, wakes the
   GT911 by reading its product-id string (`'9' '1' '1'`), and clears the
   status byte. A failure here prints `touch: FAIL open` and halts on a BKPT.
3. Polls `ra8_touch_read()` (statically bounded) for one touch frame and reports
   the first decoded contact on the console:

   ```
   touch: open=OK pts=1 x=250 y=250
   ```

The bring-up half (`touch: open=OK`) is the deterministic, **finger-free** part
of the gate -- it proves the real `ra8_touch_open -> IIC_B -> GT911` path reached
the product-id check. `hil.conf` asserts only that substring, so the gate is
stable whether or not a finger is present on the bench (no finger -> `pts=0`,
but `open=OK` still holds).

## Build + run

```
make touch_demo
scripts/hil/run_local.sh touch_demo      # flash + scrape the bring-up banner
```

## Result (validated 2026-06-19, ra8_emulator)

`ra8_emulator` models the GT911 on the modelled I2C bus and injects a tap with
`--click X Y`; the tap returns through the genuine `ra8_touch_read` decode (there
is no function-level stub), so the banner carries a **real decoded coordinate**.

```
$ ra8_emulator touch_demo.elf --click 250 250
[uart] SCI8: touch-demo: boot
[uart] SCI8: touch: open=OK pts=1 x=250 y=250
  I3C/I2C GT911 : 1 touch frame(s) drained via ra8_touch -> I3C
```

`scripts/emu/smoke.sh touch_demo` PASS -- the harness arms `--click 250
250` (1:1 on the default panel) and asserts the full
`touch: open=OK pts=1 x=250 y=250` banner, gating the GT911 path end to end
(`ra8_touch -> ra8_i3c -> GT911` decode). The driver is the same one
`ereader_ui` uses and `tests/test_ra8_touch.c` covers on the host.

## On real silicon

On the bench the GT911 lives on the ereader carrier's I2C0 bus. `open=OK`
confirms the IC answered its product-id probe; touch a real finger to the panel
and the banner reports that contact's panel-native coordinate. Without a finger
the bring-up gate (`open=OK`) still passes.
