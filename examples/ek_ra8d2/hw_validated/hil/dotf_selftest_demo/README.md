# dotf_selftest_demo

Brings up the DOTF (Decryption On The Fly) block and runs its built-in AES
self-test on both channels without ever arming one (#127). LED1 toggles while
healthy and LED2 on a fault; `g_dotf_ok`, `g_dotf_init_err`, `g_dotf_reg00` and
the two post-trigger `REG00` snapshots (`g_dotf_st0_snap` / `g_dotf_st1_snap`)
mirror the result for a headless probe. No external hardware.

## Why it stops where it stops

The full flow to decrypt XiP code is init -> install key -> set IV -> set region
-> enable -> jump into XiP. This app runs only the init and the self-test, and
that boundary is a safety property rather than an unfinished feature:
`ra8_dotf_enable` arms the AES core over a live XiP window, and the very next
instruction fetch would come back as garbage and fault (HUM Ch 45 warning). It
also installs no key, stages no IV, programs no conversion region, and writes no
OTP, option-setting, security-attribution or flash byte.

Both channels stay in transparent bypass for the whole run, so there are **no
persistent side effects**. The shared module-stop bits are toggled, but that is
volatile clock state, not a fuse.

## What the headless verdict is worth

The emulator does not model the DOTF APB window, so off-target the self-test
register reads are not backed by an AES core at all: the calls return OK and the
demo reports healthy without any BIST having run. The headless verdict therefore
gates the driver call sequence and nothing more. The genuine AES self-test
result lives in the `REG00` snapshots read off silicon; off-target they read
back zero.

## Registers (HUM R01UH1065EJ0130 Ch 45 "DOTF")

- Block overview and the self-test feature: Ch 45.1 p 3048.
- `REG00` (AES core / control, +0x080 in the channel window) bit 20 is the
  self-test trigger and auto-clears on completion: Ch 45.3 "Register
  Descriptions" p 3049.
- Module-stop is shared with OSPI -- `MSTPB16` covers DOTF0 plus OSPI0, and
  `MSTPB17` covers DOTF1 plus OSPI1: Ch 45.6.1 "Module-stop Function" p 3050.
