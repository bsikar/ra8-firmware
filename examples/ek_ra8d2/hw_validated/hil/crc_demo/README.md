# crc_demo

Cross-checks the on-chip CRC engine against a bit-serial software reference over
the same buffer, once a second, and reports whether they agree. LED1 toggles on
a match and LED2 on a mismatch, which should never happen.

The point is the comparison: a hardware CRC that silently disagrees with the
standard polynomial is the kind of defect that only shows up when someone else
tries to verify your data. Needs no external hardware.
