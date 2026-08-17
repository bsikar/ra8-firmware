# lpm_wake_matrix_demo

Walks every WUPEN0 / WUPEN1 wake-source enable bit the LPM HAL exposes and
confirms each reads back as written, then disarms everything and confirms both
registers read zero. It is the smoke test for `ra8_lpm_arm_wupen0_bits` /
`ra8_lpm_arm_wupen1_bits`, their clear counterparts, and the
`ra8_lpm_get_exit_cause` packed snapshot. `g_lpm_wake_matrix_armed` advances
through the walk for a finer-grained SWD trace.

WUPEN0 covers the internal-peripheral sources IWDT, PVD1, PVD2, VBATT and the
RTC alarm and periodic events; WUPEN1 covers COMPHS0, SOSC, the three ULPT0
sources and I3C0.

**It deliberately never enters standby.** Most WUPEN sources need the underlying
peripheral armed and wired to external hardware -- an attached USB device, a
driven IRQ pin, an armed timer -- which a bare EK-RA8D2 cannot synthesise
without a shield. The claim here is narrower and fully checkable: the WUPEN
registers are reachable and hold what you write to them.

No external hardware required.
