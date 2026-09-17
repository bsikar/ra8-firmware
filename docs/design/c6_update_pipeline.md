# One update pipeline: OTA and USB are transports, not designs

> **Status: DESIGN OF RECORD.** One pipeline serves both OTA and USB updates:
> each transport deposits the same signed bundle into RA8-side staging, and
> verify/apply/confirm are transport-blind.

## The requirements this is built to

1. The C6 exists (in large part) to give the product **over-the-air updates**.
2. The RA8 already has a **USB HS** update path (`ra8_dfu` + the production
   `dfu_bootloader`, A/B slots, header-last commit, RoT-enforced boot).
3. OTA and USB updates must **behave the same**.
4. An update must be **fully downloaded to local storage first**, then applied
   -- so the USB cable does not have to stay plugged in across the apply, and
   a dropped Wi-Fi link mid-download can never half-apply anything.

## Design: one pipeline, N transports

```
 TRANSPORTS (dumb byte producers)        PIPELINE (one implementation)
 --------------------------------        ------------------------------------
 USB HS DFU   host -> RA8                      +-> STAGE   whole bundle lands in
 Wi-Fi OTA    cloud -> C6 -> link -> RA8       |           RA8 staging store
 Bench serial dev host -> RA8 (or C6)  --------+-> VERIFY  RoT signature + per-image
                                               |           SHA-256 + version policy
                                               +-> APPLY   per-chip applier (A/B)
                                               +-> CONFIRM health flags or rollback
```

The pipeline stages are the product; a transport's only job is to deposit an
identical **update bundle** into the staging store. Nothing downstream of
STAGE knows or cares which transport ran. That is the same DIP discipline as
the driver seams: `apply` depends on the bundle contract, never on USB or
Wi-Fi.

### The update bundle

One artifact updates the whole product (both chips), so image pairing can
never drift:

```
 bundle
   manifest        version, per-image table (target chip, size, SHA-256),
                   companion-link protocol version, signature over all of it
   ra8 image       existing RA8 image format (dfu_bootloader consumes it)
   c6 image        Espressif app-image container (the C6 ROM / 2nd-stage
                   loader requires this format)
```

- The manifest is signed with the existing **RoT signing key** flow; the RA8
  verifies before anything is applied anywhere. The C6 additionally verifies
  its own image's SHA-256 (and, once C6 secure boot is decided, its own
  signature) so a compromised RA8 cannot silently feed it garbage.
- A bundle may carry one image (C6-only fix, RA8-only fix) or both; the
  manifest table says which. Cross-chip compatibility is enforced by the
  companion-link protocol version field: the RA8 refuses to commit a bundle
  that would leave the two sides speaking different protocol versions.

### Staging lives on the RA8

The RA8 owns the Octo-SPI flash (plus SD); the C6 has neither the storage nor
the authority. Every transport stages into the same RA8-side staging area:

- **USB HS**: host pushes the bundle over the existing `ra8_dfu` ingest; bytes
  go to staging instead of directly to a slot. Cable can be pulled the moment
  the transfer completes; VERIFY/APPLY/CONFIRM run from staging (requirement 4).
- **Wi-Fi OTA**: the C6 downloads the bundle (it is the only chip with a
  radio) and streams it over the companion link into the same RA8 staging
  area. The C6 does not buffer the whole bundle -- it has neither the SRAM nor
  any requirement to; it is a pipe with flow control.
- **Bench serial**: an esptool / J-Link flow remains the dev-only shortcut
  that bypasses the pipeline entirely (writing slots directly). Never a
  production path.

### Apply is per-chip, authority is fixed

- **RA8 image**: applied by the existing `dfu_bootloader` machinery (A/B
  slots, header-last commit, copy-to-run, RoT check at boot). Unchanged.
- **C6 image**: the RA8 streams the staged C6 image over the companion link;
  the C6-side receiver writes its **inactive** OTA slot, verifies, marks
  pending, reboots into it, and must set its health flag within the
  boot-attempt budget or its loader rolls back.
- The C6 **never writes RA8 memory**, and the RA8 never writes C6 flash
  directly -- each chip's own loader is the only thing that commits its own
  slots. No cross-brick authority exists in either direction.

### Confirm, health, and coordinated rollback

Bundle-level success is orchestrated by the RA8 (it has the storage, the RoT,
and the display for user-facing progress):

1. Apply C6 image first (if present), wait for the C6 to reboot and report
   healthy over the link.
2. Apply RA8 image (if present); the RA8's own boot-attempt/rollback discipline
   covers it.
3. Only when every image in the manifest reports healthy does the RA8 mark the
   bundle committed and delete it from staging. Any failure = both chips roll
   back to their last-known-good slots (the C6 rolls back autonomously via its
   attempt counter even if the RA8 dies mid-orchestration).

Ordering note: C6-first is deliberate. A new RA8 firmware may require a new
link protocol; the C6 slot layout keeps the old image intact until CONFIRM, so
a C6-first apply that then fails the RA8 apply still rolls the C6 back.

### Field recovery: the C6 ROM-loader client runs ON the RA8

A C6 whose flash is fully bricked (both slots dead, loader dead) must be
re-flashable in the field by the RA8 from a staged known-good image -- the
same anti-brick discipline `scripts/hil/dlm_reset_local.sh` and
`just hil::reflash <app>` give the RA8 on the bench. Rather than write our own SLIP
downloader, **Espressif already ships this as a portable C library** --
`esp-serial-flasher` (Apache-2.0, actively maintained, ESP32-C6 supported over
UART, with existing host ports for STM32/Zephyr/RP2040/Linux;
https://github.com/espressif/esp-serial-flasher). Vendor it as SOUP like
ThreadX/NetX and write only the first-party RA8 port glue: the UART transport
plus GPIO control of the C6's EN (reset) and BOOT strapping pins.

**PCB requirement this creates: the C6's EN and BOOT pins must be wired to
RA8 GPIOs.** Without those two traces there is no field-recovery path and no
factory-provisioning path; this is the single most important companion-IC
board-design decision and must be locked before any carrier is laid out.

## The two-USB self-test, sharpened

The EK-RA8D2's two USB peripherals prove the pipeline on one board, but the
loop must exercise the **production roles**:

- Production ingress is the RA8's USB HS as a **device** (host pushes to us).
- Therefore the self-test should run **USB FS as host, USB HS as device**
  (FS-host enumerates the HS port at full speed; slower, same code path), so
  the device-mode ingest stack -- the thing production uses -- is what gets
  tested. An HS-sender / FS-receiver framing would instead exercise HS in
  host mode, which production never uses.
- The looped bundle then drives STAGE -> VERIFY -> APPLY(C6 over the link once
  a C6 is wired, or a modelled link endpoint before then) -> CONFIRM.

## The dependency order, and why it is the point

The bundle format and its RoT verification come first: they are pure logic,
host-unit-testable and MC/DC-able, and need no new hardware. The staging store
sits behind the existing `ra8_io_blockdev_t` facade, the USB HS ingest is a
rewire of `ra8_dfu` onto staging rather than onto a slot, and the companion
link supplies a framed, versioned channel for the C6 receiver.

The C6 apply step is the *last* thing to land and the only thing a change of
strategy touches. Under the adopted co-processor architecture (see
`c6_wireless_architecture.md`) it is esp-hosted's first-class host-pushed
co-processor OTA -- `esp_hosted_slave_ota_begin/write/end/activate` RPCs over
the existing link -- feeding the C6's stock A/B slots from the same staged
bundle. One known gap to own: the stock co-processor firmware never calls
`esp_ota_mark_app_valid_cancel_rollback()`, so enabling ESP-IDF's boot-rollback
config requires a one-line co-processor patch (rebuild once, pin the binary).

That is the whole argument for making OTA-vs-USB a transport detail: the
bundle, the staging, the RA8-side code and the self-test are invariant under
any change to how a C6 image is applied.
