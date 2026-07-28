# poeg_safe_shutoff

Demonstrates the RA8D2 **POEG** (Port Output Enable for GPT) safe-shutoff path:
kill a running GPT PWM output on a fault trigger, then clear the fault and
re-enable the output. This is the mechanism a motor-control H-bridge relies on
so an over-current trip forces the PWM pins high-impedance in a single cycle,
with no CPU in the loop.

## What it does

Every second the demo runs one full cycle and prints the result on the J-Link OB
CDC console:

1. **RUN** -- GPT0 counts in saw-wave PWM (`ra8_gpt_init`, 50% duty). `POEGG.ST`
   (the output-disable STATE flag) is clear, so the outputs are driven.
2. **SHUTOFF** -- assert the software output-disable request
   (`ra8_poeg_trigger_stop`, `POEGG.SSF = 1`). The block latches `POEGG.ST = 1`
   and the GPT outputs go **high-impedance**. Verified by reading ST back.
3. **RE-ENABLE** -- clear the request (`ra8_poeg_clear_status` of SSF). With no
   request left, `POEGG.ST` returns to 0 and the outputs are driven again.
   Verified by reading ST back.

The GPT *counter* keeps advancing across the shutoff (POEG gates the output
pins, not the counter), so the demo also samples `GTCNT` before and after the
hold to prove the timer is really running.

`LED1` toggles on a healthy cycle, `LED2` on a fault. Success banner:

```
poeg: pwm=run shutoff=highZ reenable=on ok=Y
```

## Trigger sources

POEG can request the output-disable from four sources (HUM Ch 21): the external
`GTETRG` pin (over-current comparator), a GPT output-level short, an oscillation
stop, and the software `SSF` bit. This demo arms the external-pin path
(`enable_pin = true`) so a comparator wired to the POEG pin would trip the
shutoff on silicon, and drives the deterministic **software** trigger to make
the cycle observable headlessly. On a real board you would additionally route
the GPT output-disable request of GPT0 to this POEG group.

## Board_sim gate

`tools/ra8_emulator` models the POEG register file in `board_periph_poeg.c`: a
`POEGG.SSF` write latches the derived `POEGG.ST` output-disable STATE flag, and
clearing every request flag returns ST to 0 -- exactly the STATE latch the
hardware produces. So `scripts/emu/eil_all.sh` / `scripts/emu/smoke.sh` see the
same `assert -> high-Z -> clear -> re-enable` transition and assert the `ok=Y`
banner with no board attached. The `hil.conf` `uart_scrape` gate keys on it.

## Build

```
make poeg_safe_shutoff
```

Bare EK-RA8D2; no shields or external transceivers.
