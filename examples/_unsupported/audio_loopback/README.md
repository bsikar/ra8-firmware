# audio_loopback

I2S audio playback smoke test for the EK-RA8D2 (DA7212 CODEC). Brings
the chip up via `ra_cgc_init`, opens SCI8 for diagnostic logs, and
hands the entire SSIE0 + DA7212 bring-up to the
`ra_board_ek_ra8d2` BSP via `ra_board_audio_init(48000, 16, 2)`.
Uses `ra_board_ek_ra8d2` BSP for CODEC pin routing (P403/P404/P405/
P406/PD06 + I2C SDA1/SCL1) per EK-RA8D2 v1 UM Table 32 ("Audio CODEC
Port Pin Assignments") p 38, and forwards stereo PCM blocks via
`ra_board_audio_play_sample_block`.

The current main loop feeds a static silence buffer (zero-amplitude
PCM); the file name keeps the historical "loopback" label from the
earlier hand-rolled SSIE driver smoke test, but the BSP-driven app is
playback-only. Replace `k_audio_loopback_silence` with a sine LUT or
an iso-OUT USB feed to drive an audible signal.

## Status: untested on hardware

Build is green and the BSP CODEC bring-up matches UM Table 32. The
audio path itself has not been verified end-to-end on real silicon at
authoring time.

## Where to plug in

The EK-RA8D2 v1 board carries the on-board DA7212 CODEC (UM Section
6.6, U14). Headphones / line-out on the AUDIO OUT 3.5 mm jack should
hear whatever PCM block the firmware pushes to
`ra_board_audio_play_sample_block`.

J41 jumpers must be populated to select the CODEC over the camera
connector (P405 / P406 are shared between SSIE0 SDIN/SDOUT and the
parallel-camera D2 / D3 lines per UM Table 32 + UM Table 35 p 48).

## Build + flash

From the repo root:

```sh
make audio_loopback                       # cross-compile
make -C examples/audio_loopback flash     # flash via on-board J-Link OB
```

Or standalone:

```sh
cd examples/audio_loopback/
make
make flash
make clean
```

## What the firmware does

1. `ra_cgc_init()` brings up XTAL + PLL1 (CPUCLK0 = 1 GHz, PCLKA =
   125 MHz).
2. `ra_pfs_route_peripheral()` routes `PD02 / PD03` to SCI8 for the
   J-Link OB CDC log channel.
3. `ra_sci_init(8, 115200 8N1)` opens the J-Link diagnostic stream.
4. `ra_board_audio_init(48000, 16, 2)` -- BSP routes the seven CODEC
   pins per UM Table 32 p 38 and brings SSIE0 up in I2S controller
   mode.
5. Loop:
   - `ra_board_audio_play_sample_block(silence, 128)` -- push one
     stereo block (64 frames * 2 ch) per iteration.
   - Every 1000 blocks, print `audio: <N> blocks played\r\n` on SCI8
     and toggle LED1.

## Connecting a terminal to SCI8

```sh
picocom -b 115200 /dev/cu.usbmodem...
```

You should see one line every ~1.3 s when 48 kHz audio is flowing.

## Debugging

```sh
make -C examples/audio_loopback ozone
make -C examples/audio_loopback debug
```

Useful SWD probes (HUM Ch 46 register window):

```
mem32 0x4005D000 1   # SSIE0 SSICR
mem32 0x4005D004 1   # SSIE0 SSISR  -- check ROIRQ/RUIRQ/TOIRQ/TUIRQ
mem32 0x4005D008 1   # SSIE0 SSIFCR -- TIE/RIE/AUCKE/BSW
mem32 0x4005D00C 1   # SSIE0 SSIFSR -- RDC/TDC
mem32 0x4005D014 1   # SSIE0 SSIOFR -- OMOD/LRCONT
mem32 0x4005D018 1   # SSIE0 SSISCR -- TDES/RDFS thresholds
```

Validated 2026-05-02 against EK-RA8D2 v1 User's Manual (R20UT5523EG0101
Rev 1.01) Table 32 "Audio CODEC Port Pin Assignments" p 38 + Section
6.6, and HUM (R01UH1065EJ0130) Ch 46 "SSIE".
