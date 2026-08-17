# cac_accuracy_demo

Uses the Clock Frequency Accuracy Measurement Circuit to count edges of the
24 MHz main oscillator inside one window of the LOCO reference divided by 32,
and flags a frequency error if the crystal is off. LED1 toggles on a healthy
measurement and LED2 on a timeout or error; `g_cac_count`, `g_cac_ok`,
`g_cac_status` and `g_cac_heartbeat` mirror the result for a headless probe.

The allowed window is the expected count (24 MHz over the 1024 Hz reference,
about 23437) plus or minus roughly 6 percent -- wide enough that a healthy
crystal never trips `FERRF`, narrow enough that a stopped or grossly detuned
oscillator always does. The LOCO must be running: it is the reference, and if it
is stopped the measurement simply times out rather than reporting an error about
the crystal. No external hardware.

The `ra8_cac` HAL takes only the count limits and leaves CACR1 / CACR2 at zero,
which is a degenerate main-against-main pairing. This demo writes those two
registers directly to select the real main-against-LOCO pair; folding
clock-source selection into `ra8_cac_init` would remove the need.

Configuration, HUM R01UH1065EJ0130 Ch 10 "CAC":

- CAULVR / CALLVR carry the window (Ch 10.2.6 / 10.2.7 p 425).
- CACR1 = `0x00` -- target is the main oscillator (FMCS = 000), no division,
  rising edge (Ch 10.2.2 "CACR1" p 421).
- CACR2 = `0x09` -- internal reference (RPS = 1), LOCO (RSCS = 100) divided by
  32 (RCDS = 00), no digital filter (Ch 10.2.3 "CACR2" p 422).
- A measurement sets `CACR0.CFME` and polls `CASTR.MENDF` (Ch 10.2.1 p 421,
  Ch 10.2.5 p 424); the count is read from CACNTBR.

The emulator models the edge counter by loading CACNTBR with the midpoint of
whatever window the firmware programmed, so it is in band by construction. That
proves the start / poll / read-back sequence and nothing whatever about the real
clock; only silicon proves the edge count.
