# i2c_loopback

IIC_B (I3C-in-I2C-mode) self-test smoke app for the EK-RA8D2.
Brings up `ra_iic_b` on channel 0 at 100 kHz Sm and probes the
on-board PI4IOE5V6408 I/O expander (U15) at 7-bit address `0x43`,
which is the only I2C peripheral guaranteed populated on a bare
EK-RA8D2 v1.

Banner on success:

```
iic_b: scan 0x43 ack=1
iic_b: scan 0x43 ack=1
...
```

LED1 toggles each scan; LED2 latches ON if the controller returns a
hard error (`hw_timeout`, `hw_error`).

## Why this is `hw_pending`

Both pin permutations (channel 0 + P512/P511 with the P109/P311
pullup-enable, and channel 0 + P400/P401 without it) produce
`iic_b: scan ERROR` on the live HIL board -- `ra_iic_b_scan`
times out waiting for `NTST.TDBEF0` after the START condition.
That points to a physical-board issue rather than a chip-config
issue:

- **EK-RA8D2 v1 Board UM Table 31** ties J27-1/J27-2 to
  **P400 (SCL0) / P401 (SDA0)** when **SW4-5 is ON (I3C mode)**
  and **P512 (SCL1) / P511 (SDA1)** when **SW4-5 is OFF
  (I2C mode)**.
- **Board UM Table 23 row 1** says the on-board SCL1/SDA1 pull-ups
  must be software-enabled via push-pull-HIGH on **P109 / P311**
  in I2C mode.
- The HIL test board's SW4-5 state is unknown to the firmware --
  if it is in a state where neither pin pair reaches U15 the bus
  floats and every scan times out.

The HAL fix that landed in the same commit
(`internal_iic_b_block_bringup` now also ungates **MSTPB9 IIC0**
alongside MSTPB4 I3C) is still required regardless of which pin
pair is used, so that part is correct and committed.

## How to graduate this back to `hw_validated/hil/`

1. With the EK-RA8D2 on the bench, run `i2cdetect` from a host
   wired to either pin pair (e.g. Pi I2C-1 SDA/SCL) and confirm
   U15 actually ACKs at `0x43` over the same physical lines the
   firmware drives.
2. If SW4-5 = OFF (I2C mode), confirm the firmware writes
   P109 / P311 HIGH and the on-board pull-ups visibly engage
   (a scope on SCL1 should idle at 3.3 V, not float).
3. If SW4-5 = ON (I3C mode), confirm `ra_iic_b_init` reaches U15
   via P400 / P401 with no software pull-up enable.
4. Once the working pin pair + switch position is identified, set
   it as the demo's pinout, drop the failed branch, and move the
   dir back under `examples/ek_ra8d2/hw_validated/hil/`.

Until that physical check happens this stays under `hw_pending/`.
