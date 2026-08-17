# ESP32-C6 wireless co-processor firmware

The EK-RA8D2 gains Wi-Fi by pairing the RA8D2 host with an **ESP32-C6** running
Espressif's **esp-hosted-mcu** `network_adapter` application over a SPI
transport; the RA8 is the host that drives it. Bluetooth over the same link is
planned, not delivered -- the host `drivers/bt/` bridge is excluded from the
build and the BLE HCI seam is still on loopback (#493).

The C6 image is mixed: pinned Espressif esp-hosted-mcu SOUP plus the
first-party `ra8_mdl_service` component. `build.sh` applies one checked-in patch
to the pinned upstream commit to expose a bounded synchronous CustomRpc response
hook, stages the component from `port/esp32_c6`, and verifies the service symbol
reached the final ELF -- if the patch has drifted the build stops before
compiling, rather than producing an image quietly missing the hook. The RA8-side
host driver lives in `libs/third_party/esp-hosted/`, `port/esp-hosted/` and
`libs/ra8_c6link/`.

The media service is a pull-based HTTPS transfer: Start returns quickly, each
Next request supplies backpressure and acknowledges its offset, and redirects
are refused so an HTTPS request cannot be walked down to plaintext. The C6
counts every received byte and emits COMPLETE only after ESP-IDF confirms the
message body is complete and any advertised length matches that count. The RA8
remains the sole owner of SD paths, temporary files and the final rename.

- SOUP qualification: [`../../docs/SOUP/esp-hosted.md`](../../docs/SOUP/esp-hosted.md)
- Architecture: [`../../docs/design/c6_wireless_architecture.md`](../../docs/design/c6_wireless_architecture.md)

## The pin map lives in `pins.env`, not here

`pins.env` is the single source of truth for both ends of every wire -- the C6
GPIO numbers, the RA8D2 pins and Pmod1 / J26 holes they land on, the flash
parameters and the bench cabling -- and it is a plain `KEY=value` fragment so
`build.sh` and `flash.sh` can source it. `sdkconfig.defaults` restates the same
pin numbers in Kconfig syntax because that is the only form esp-idf reads;
`scripts/checks/check_c6_pin_config.py` diffs the two, in the `pre-commit-checks`
gate, in the git hook (pure text compare -- no esp-idf needed) and again on the
bench before every build. A third copy in this prose is exactly the drift that
checker exists to remove, which is why there is no pin table on this page.

`sdkconfig.defaults` is therefore derived but deliberately not generated: it is
kept byte-stable because the bench-proven image was built from exactly those
lines, and regenerating it would put an unproven file in the path of working
firmware. It is verified instead.

That verification is a gate rather than a convention because this particular
drift is undetectable downstream: the build succeeds, the image flashes, the
board boots, and the SPI link simply never comes up, because the two ends are
driving different pins. **To change a pin, edit `pins.env`, update
`sdkconfig.defaults` to match, and reflash the C6.** A board still holding the
previous image keeps the previous pins no matter what the repository says.

There is no 3V3 wire between the boards: the C6 is self-powered over its own
USB.

### Which config leaves actually take effect

Only the `CONFIG_ESP_SPI_HSPI_GPIO_*` SPI pin leaves are settable. The
similarly named `CONFIG_ESP_SPI_GPIO_*` symbols for the same four signals are
*derived* from those and are ignored if set directly. The data-ready and
handshake leaves, by contrast, are directly settable.

Worse, esp-hosted-mcu ships Kconfig **presets for Espressif's own dev boards**,
and selecting one **overrides the SPI pin leaves**. With any board but the
"none" selection, every pin the tree specifies is silently ignored and the C6
comes up on the preset's pins instead. That failure has the same expensive shape
as a pin typo, so the selection is recorded in `pins.env` too, and the checker
compares the enabled `CONFIG_ESP_HOST_DEV_BOARD_<X>` symbol's *suffix* rather
than merely asserting that some such symbol exists. Comparing the suffix is what
catches the worse of the two drifts -- the selection moving to a *different*
board, which still produces a working build driving the wrong pins.

## Bench: the SW4 DIP bank

**SW4-1 OFF, SW4-2 OFF, SW4-3 ON, SW4-4 OFF.** Four *independent* mechanical
switches, each owning a different thing:

- **SW4-1 / SW4-2** are Pmod1 Mode Select 1 and 2; OFF + OFF selects the SPI
  position (EK-RA8D2 v1 UM Table 18 p 26).
- **SW4-3** is the **Octo-SPI mux**. ON means "Octo-SPI Inactive", which is what
  makes the U6 / U9 bus switches connect the Pmod1 SPI pins to J26 at all (UM
  Table 3 p 16).
- **SW4-4** owns the **Arduino / mikroBUS connectors** and has nothing to do
  with the mux. SW4-3 ON together with SW4-4 ON is not a valid combination
  (#555).

Getting the bank wrong is invisible. With **SW4-3 OFF** the on-board Octo-SPI
flash keeps the Pmod1 SPI pins and the bus switches stay open, so J26 never
reaches the MCU while every part involved looks perfectly healthy: the host
clocks transactions into `0xff` and each app reports a protocol or timeout
failure of its own flavour. Nothing says "the bus is not connected", so it reads
as a firmware red and is not one (#555 tracks making the bench run detect and
abort on it).

**SW4-4 OFF** also takes the Arduino and mikroBUS connectors offline, so the
LSM6DSO IMU Click is unavailable while the C6 link runs. That is a real
trade-off of this board rather than an oversight, and it is why this tier cannot
share a bench pass with the rest of the HIL suite.

One outage on this rig was a **misread of this bank**, not a wiring fault. The
measurements taken while the nets were open at the mux are preserved in the
`c6_spi_probe` README as a worked example of how convincing a correct
measurement is when it hangs off a wrong premise. Check the switches
**electrically** before suspecting anything else.

## Building

The build needs esp-idf, and **the dev box does not have it** -- the Pi bench
host is the build machine. Its provisioning is declared, not hand-built, in
[`../../infra/ansible/roles/c6_toolchain/`](../../infra/ansible/roles/c6_toolchain/):
the IDF checkout, its pinned release, the apt prerequisites (`python3-venv` is
the one whose absence hard-fails IDF's installer) and a per-minor-version venv
marker that keeps a re-run from re-downloading gigabytes of toolchain. The role
also drops a profile script giving an interactive shell `get_idf` to source the
IDF export script.

**The esp-idf pin is not cosmetic: an earlier release does not build
esp-hosted-mcu at all** -- its component-manager pull of tf-psa-crypto fails to
compile p256-m. So `build.sh` asserts the exact pinned version, not merely the
series, before it does anything else, then verifies `sdkconfig.defaults` against
`pins.env`, fetches and patches the pinned upstream, stages the first-party
component, cleans, builds, and asserts that the strong media-service handler
(not the weak upstream fallback) and the component ABI marker both exist in the
resulting ELF. The fetched upstream clone is git-ignored: it is SOUP, fetched at
build time and never committed.

**There is no `menuconfig` step, and running one is a mistake.** The
configuration is `sdkconfig.defaults` plus the checker that verifies it against
`pins.env` -- two committed files and a gate, which is what makes a pin change
reviewable and a pin drift catchable. `menuconfig` writes a `sdkconfig` that is
not committed and that the next build deletes, so a change made that way
survives exactly until someone rebuilds, and until then the tree says one thing
while the flashed image does another.

### Reproducibility: recipe vs bits

The build deletes `dependencies.lock` and lets the esp-idf component manager
re-resolve on every run. That makes the *recipe* reproducible but not the
*bits*: the registry can hand back a different component version tomorrow from
an unchanged recipe. The generated lock cannot simply be committed, because it
records a **host-specific absolute path** for the local `cmd_system` component
taken from the building machine's own IDF checkout, which would be wrong on
every other host.

What is portable is the resolved **set**, so `components-lock.txt` records the
kind, name, version and registry hash of each component as resolved by the bench
build whose image is the one actually flashed and proven. The build normalises
the fresh lock to those same columns and diffs it, comparing the local component
on kind and version only and never on its path.

A difference there is not a warning to click past. The recipe is unchanged, so
it means the registry resolved something else: either re-pin deliberately --
updating the record **and** reflashing and re-qualifying the C6 -- or find out
why.

## Flashing

Flash over the **CH343 USB-UART bridge**. Do **not** write the image through the
C6 native USB-JTAG interface: it fails with `EPIPE` partway through the flash
write. Both interfaces enumerate as `/dev/ttyACM<n>` and the numbering changes
on a power cycle, so `flash.sh` resolves the bridge by device identity (see
`scripts/hil/lib/tty_resolve.sh`) and fails loudly if it cannot, rather than
writing to whatever holds a number. The flash size, mode and frequency come from
`pins.env` as well; a wrong flash size is the other common bring-up failure.

The Wi-Fi credentials the join app compiles in come from a git-ignored
`wifi.env` beside its template. The real passphrase lives in OpenBao under the
bench-network secret, never in this repository.

## Flashing is not evidence

The C6 is powered by its own USB, so it enumerates and looks alive whether or
not the harness and the SW4 mux are right -- which is exactly the trap the
outage above fell into. Prove the image on the bench instead, running the C6
tier in triage order: `c6_spi_probe` (the wire), `c6_hosted_init` (the port),
`c6_fw_version` (the protocol). See
[`../../examples/ek_ra8d2/hw_validated/c6/README.md`](../../examples/ek_ra8d2/hw_validated/c6/README.md).

`c6_fw_version` is the app that settles "did the right firmware land". It sends
a real esp-hosted RPC, decodes the response with the vendored protobuf codec,
and asserts the co-processor's reported version against the version macros of
the **vendored host driver** -- the same value `pins.env` pins and the pin
checker holds the two ends to. Bump the vendor pin without reflashing the C6, or
reflash without bumping the pin, and that app goes red on purpose instead of the
mismatch surfacing later as unexplained RPC timeouts.
