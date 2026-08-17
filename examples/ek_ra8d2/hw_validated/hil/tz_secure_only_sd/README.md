# tz_secure_only_sd

SPI-mode SD round trip: bring the card up, mount its FAT volume through
`ra8_fs`, write a pseudo-random payload, read it back, compare byte for byte.

Needs a Digilent PMOD MicroSD (part 410-380) in Pmod2 (J25) with a FAT16 or
FAT32 formatted card already in it. The demo never formats, and the HIL bench
does not normally carry a card, so this usually cannot run unattended.

| EK-RA8D2 pin | PMOD MicroSD signal | Net (UM Table 19 p 27) |
|--------------|---------------------|------------------------|
| Pmod2.1 P604 | CS                  | SSLB0 (GPIO output)    |
| Pmod2.2 P603 | COPI                | MOSIB (UM legacy name) |
| Pmod2.3 P602 | CIPO                | MISOB (UM legacy name) |
| Pmod2.4 P601 | SCK                 | RSPCKB                 |
| Pmod2.5 GND  | GND                 | GND                    |
| Pmod2.6 3V3  | 3V3                 | 3V3                    |

Chip select is claimed as a plain GPIO rather than the SCI's hardware CS, which
pulses per byte; SD SPI mode needs CS held asserted across a whole multi-byte
command frame (SD PHY spec v9 section 7.2.4). The bus opens at 400 kHz for card
identification -- the spec ceiling for that phase -- and escalates once the card
answers.
