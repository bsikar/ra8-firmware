# i2c_loopback

RIIC controller self-test: brings up channel 1 and probes the on-board
PI4IOE5V6408 I/O expander (U15) at 7-bit address `0x43` once a second, toggling
LED1 on each ACK.

U15 sits on RIIC ch1 (P512 SCL1 / P511 SDA1), *not* on the I3C bus. An earlier
version of this app drove the I3C peripheral on channel 0 out to J27, where U15
is absent -- which is why it used to fail (#46). Bring-up now reuses the board
layer's validated U15 sequence
(`ra8_board_io_expander_apply_project_sw4_defaults`): bus recover, P109/P311
pull-ups, P512/P511 route plus NCODR, then init and scan.

U15 is on-board, so a bare EK-RA8D2 needs no jumpers.

`i3c_loopback` is the same idea on the I3C peripheral in I2C-compat mode.
