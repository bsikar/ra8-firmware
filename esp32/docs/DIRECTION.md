# ESP32-C6 direction: the fork in the road, researched

> **Status: DECISION DOCUMENT for owner review.** This addresses every concern
> raised about the `spike/esp32c6` direction, plus concerns not raised, each
> grounded in primary sources (fetched 2026-07-10; URLs inline and in the
> appendix). The companion design doc is `UPDATE_PIPELINE.md` (one update
> pipeline for OTA + USB). Nothing here is implemented; this is the map.

---

## 0. TL;DR -- the recommendations in one table

| Question | Recommendation | Section |
|---|---|---|
| idf.py/ESP-IDF vs our-own drivers? | Neither, at the app level: run **Espressif's esp-hosted-mcu co-processor firmware on the C6 as one pinned SOUP artifact** (a "smart NIC"), zero first-party code on the C6. All first-party code stays on the RA8, written our way. | 2 |
| Can ESP-IDF be "chopped up" like ThreadX/FileX? | Yes, and the coherent SOUP unit is the **whole co-processor firmware**. A-la-carte IDF components linked into our build is the one model that does NOT work well. | 3 |
| Can our existing libs run on the C6? | Moot under the recommendation (no C6 app). If a C6 app ever exists: `ra_core` is ISA-portable, `ra_hal` is not, the facade pattern ports. | 4 |
| Same RTOS on both chips? | Achieved in the sense that matters: **the RA8 stays 100% ThreadX and we own zero non-ThreadX code**; the C6's internal FreeRTOS is sealed inside the vendor appliance. A ThreadX-on-C6 port is newly possible but has no wireless prior art -- do not attempt. | 5 |
| Same pipeline for OTA and USB? | **Yes.** Transports deposit one signed bundle into RA8 staging; verify/apply/confirm are transport-blind. See `UPDATE_PIPELINE.md`. | 6 |
| Would a third-party wireless stack match Espressif's native one? | The question dissolves: **there is no third-party stack** -- everything wraps the same Espressif blobs, and the wrappers are strictly worse than native. | 1 |
| What can the ~20 old ESP32 boards do? | Prototype the entire companion-link architecture (esp-hosted co-processor, host driver, NetX glue, OTA relay) **now**. They cannot validate anything RISC-V, Wi-Fi 6, BLE 5.x, or C6-register related. | 7 |
| Buy list | 2-4x ESP32-C6-DevKitC-1-N8 (~$9, DigiKey, in stock), one sacrificial for eFuse/secure-boot rehearsal. | 7 |
| What SOUP do we keep vs. remove? | **Keep** NetX Duo (host IP stack over the C6's raw frames), **NimBLE + `ra_ble_host` + the `ra_ble.h` HCI seam** (the BLE host is the analog of NetX; C6 runs the controller), and wired Ethernet. **Removed** only the invented-hardware fiction: `ra8d2_ble_regs.h`, `ra_ble_patch`, the never-vendored `ble_patch` blob; `ra_ble.c` rewritten to an in-memory loopback transport. The RA8D2 has no BLE radio (datasheet, 0 mentions). | 9 |

---

## 1. The fact that reframes everything: the radio is undocumented and the stack is a blob

The owner's question was: "if we use a third-party library for Wi-Fi/Bluetooth
that we have verified, would it perform as well and have the same capabilities
as the ESP32's native stack?"

The research answer is that the premise does not exist:

- **The Wi-Fi MAC/PHY hardware does not appear in the 1,394-page C6 TRM.**
  The TRM we committed under `docs/reference/` documents every wired
  peripheral and zero radio registers. The esp32-open-mac project (the only
  serious attempt at an open driver) states it plainly: "Espressif did not
  document the Wi-Fi hardware peripherals in any of their public datasheets,
  so we had to reverse engineer" (https://github.com/esp32-open-mac/esp32-open-mac).
  After ~3 years of NLnet-funded work it supports open networks only, on the
  classic Xtensa ESP32 only (explicitly not C-series), still needs the blob to
  initialize the RF front end, and claims no production readiness.
- **Every "alternative" stack is the same blobs in a different wrapper.**
  Zephyr (`west blobs fetch hal_espressif`), NuttX (esp-hal-3rdparty), and
  Rust esp-radio all link Espressif's `libpp.a` / `libnet80211.a` /
  `libphy.a` / `libcoexist.a` / `libble_app.a`. There is no exception.
- **The wrappers are strictly worse than native.** Same silicon, same blobs:
  native ESP-IDF does 20 Mbps TCP / 30 Mbps UDP over the air; Espressif's own
  tuning got Zephyr to ~4/10 Mbps
  (https://developer.espressif.com/blog/2024/06/zephyr-max-wifi-throughput/);
  Zephyr's C6 driver hardcodes TWT off and had WPA3 parameter-forwarding
  broken as of March 2026 (zephyrproject-rtos/zephyr#105733); NuttX C6 numbers
  are ~3.6-9 Mbps with decay bugs.
- **Regulatory reality**: the ESP32-C6-WROOM-1 module's FCC modular grant
  (FCC ID 2AC7Z-ESPC6WROOM1) rests on KDB 996369's requirement that RF
  parameters are controlled by approved software -- which is Espressif's PHY
  blob. Running the standard blobs (under any OS) inherits the grant; shipping
  reverse-engineered RF code voids the modular-approval premise and buys a
  custom intentional-radiator certification campaign.
- What IS open and auditable: the WPA supplicant (BSD, hostap), the BLE HOST
  stacks (NimBLE, Apache-2.0), lwIP, OpenThread. The security-relevant layers
  above the radio can be vetted source; the radio itself cannot.
- Licensing is clean: the blobs are Apache-2.0 binaries (LICENSE verified in
  espressif/esp32-wifi-lib and esp-phy-lib), vendorable into this MIT
  monorepo as SOUP the same way Zephyr and esp-rs redistribute them.

**Consequence:** "our own drivers" is achievable for every wired C6 peripheral
(this spike proves it) and is *physically impossible* for the radio -- the one
peripheral the C6 exists to provide. Any strategy must make peace with
Espressif-blob SOUP somewhere; the strategies differ only in where the SOUP
boundary sits and how much of our own code sits on top of it.

---

## 2. The four strategies, scored

**A. From-scratch C6 firmware (this spike, extended).** Own drivers, own
build, TRM citations -- and no radio, ever (section 1). Viable only for a C6
used as a plain RISC-V MCU, which contradicts why the part is on the board.
*Verdict: keep as the bench bring-up / debug / recovery substrate; not the
product lane.*

**B. Hybrid: vendor register headers as SOUP + our drivers + blob wireless.**
Espressif's `components/soc/esp32c6` register headers are dual-licensed
`Apache-2.0 OR MIT` (SPDX verified per-file) and near-self-contained --
CMSIS-device-header style vendoring is precedented (esp-hal-3rdparty exists
precisely for this; NuttX pins it by SHA). But to get wireless you must also
carry the blobs plus their OS-adapter (128 function pointers, see section 5),
PHY calibration storage, and Espressif's clock/power HAL -- the exact bundle
Zephyr/NuttX vendor wholesale with Espressif employees maintaining the shims.
*Verdict: technically clean for wired peripherals, dominated by C for
wireless. Adopt its one good idea piecemeal: if strategy A code ever grows,
vendor Espressif's register headers instead of hand-transcribing the TRM.*

**C. Co-processor: stock Espressif firmware on the C6, zero first-party C6
code (RECOMMENDED).** Flash the C6 with esp-hosted-mcu co-processor firmware -- one
versioned, hash-pinned binary registered as SOUP exactly like ThreadX/FileX --
and treat the whole chip as a wireless appliance. This is Espressif's own
flagship architecture (the radio-less ESP32-P4's official companion story;
the P4-Function-EV board ships a C6 pre-flashed with exactly this), it is how
two shipping Renesas-RA-host products work (Arduino Portenta C33 = RA6M5 +
ESP32-C3 esp-hosted; Arduino UNO R4 WiFi = RA4M1 + ESP32-S3 AT-style), and it
keeps every line we compile under our rules and our build:

- Data plane is **raw 802.3 frames** -- so **NetX Duo** (already vendored)
  drives it through a first-party IP driver, and the RA8 owns TCP/TLS/HTTP.
  The C6 is invisible above L2.
- Transports: Standard SPI full-duplex measured at ~22 Mbps TCP (plenty; an
  e-reader's OTA and sync need single-digit Mbps), SDIO 4-bit at ~53 Mbps if
  ever needed (PCB-grade wiring required). UART works but wastes the part.
- **Host-pushed C6 update is a first-class API**
  (`esp_hosted_slave_ota_begin/write/end/activate` over the existing link)
  -- it slots directly into our update pipeline as the C6 apply step.
- BLE: the C6 runs only the controller; the **host BLE stack runs on the RA8**
  (NimBLE as SOUP, or HCI to anything), documented with examples.
- First-party surface we still write (all RA8-side, all our style): the
  ThreadX port of the hosted OS-abstraction vtable, the SPI transport driver +
  handshake GPIO ISRs, the NetX Duo IP driver, a thin control-API facade,
  the co-processor-OTA orchestrator, the recovery/provisioning path
  (esp-serial-flasher SOUP + EN/BOOT GPIO glue), power sequencing and link
  supervision. Research estimate: **6-9 person-weeks** to Wi-Fi + NetX + co-processor
  OTA over standard SPI.
- Honest costs: we would be writing the **first ThreadX/NetX port** of the
  2.x host driver (only an ESP-IDF/FreeRTOS port ships in-tree; the porting
  vtable exists and maps 1:1 onto ThreadX, but the porting guide is an open
  issue -- espressif/esp-hosted-mcu#46); the NetX attach point replaces
  esp_netif glue at a seam upstream does not document as supported; and
  host-driver/co-processor-firmware **version lock** is the #1 operational hazard in
  the field (pin the pair as one artifact, always).

**D. Full ESP-IDF application on the C6 (idf.py or its plain-CMake core).**
The only Espressif-tested path if substantial first-party logic must run ON
the C6 next to the radio. idf.py itself is a thin wrapper -- plain
cmake+ninja invocation is officially documented, and `idf_build_process()`
lets an external CMake project link IDF as libraries -- but you still carry
FreeRTOS (mandatory), Kconfig + Espressif's Python env, the IDF bootloader
and partition machinery, ~0.8 MB images, and a second toolchain discipline.
*Verdict: the fallback if C ever proves insufficient because real application
logic must live C6-side. Not first choice: today there is no such logic --
the C6's jobs (radio, OTA ingress) are all appliance jobs.*

**Why C wins for this product:** the companion IC exists to give the e-reader
wireless and OTA. Strategy C delivers both at vendor quality, adds zero
first-party code we cannot hold to our standards, makes the SOUP boundary a
documented wire protocol instead of a linker boundary, and preserves the FCC
modular grant. It is also the only strategy where "the driver layer must be
swappable" comes free: the hosted co-processor is literally swappable firmware, and
on the RA8 side the link driver sits behind our usual `ra_io_*`-style DIP
seam, so a future move to strategy D (or a different radio chip entirely)
replaces the appliance, not the application.

---

## 3. "Chop up the ESP32 dev cycle as libraries" -- the owner's framing, evaluated

The instinct is exactly right; the question is the cut point. Four consumption
models, from finest-grained to coarsest:

1. **IDF components a-la-carte into our Makefile** -- the one that does NOT
   work. Components are not standalone archives; they drag Kconfig/sdkconfig,
   the IDF Python environment, FreeRTOS, and inter-component dependency
   resolution. Nobody consumes IDF this way.
2. **Register headers only** (`Apache-2.0 OR MIT`, CMSIS-style) -- works,
   precedented, license-clean. Right-sized for strategy A/B wired drivers.
3. **`idf_build_process()` as a library platform** -- works, documented
   (examples/build_system/cmake/idf_as_lib), but imports the whole IDF world
   into the build; this is strategy D with better table manners.
4. **The whole co-processor firmware as ONE artifact** -- works today,
   Espressif-maintained, version-pinned, hash-verified, justified once in
   `docs/SOUP/`. This is the same shape as our ThreadX/NetX/FileX entries:
   the "library" interface is the hosted wire protocol, and the vetting
   surface is an interface contract rather than a million lines of source.

The RA8 precedent maps cleanly: we did not write our own RTOS or TCP/IP stack
there either -- we vendored MIT SOUP and put hand-written drivers and app code
around it. Strategy C is that same decision with the SOUP boundary drawn at
the chip's pins instead of the linker.

---

## 4. Reusing our existing libs across chips ("one cross-chip firmware")

Under strategy C the question mostly dissolves -- there is no C6 app to share
code with. The cross-chip reuse that DOES happen is on the RA8: the link
driver, NetX glue, OTA orchestrator are ordinary first-party `ra_*` code, and
the update-bundle format/verify logic is host-unit-testable pure C.

If a first-party C6 app ever exists (strategy B/D), measured reality from the
spike + research:

- `ra_core` (ra_err, ra_check, ra_log contracts) is ISA-agnostic C23; the
  spike already proved riscv64-elf-gcc 16 swallows our C23 idioms (typed
  enums, `nullptr`, `static_assert`) unmodified.
- `ra_hal` does not port (it IS the RA8 register map), but the architecture
  ports perfectly: the spike's `esp_hal.h` vtables are the same DIP shape as
  `ra_io_*`. A shared `io_*` interface header consumed by both trees is the
  "one firmware" north star's real substance -- worth doing only when a
  second consumer actually exists (the sim/companion-link protocol layer will
  be that consumer first).
- The spike's parallel `esp_err_t` universe is fine for a spike and wrong for
  a product; graduation would converge on one error-contract header. Deferred
  until there is C6-side first-party code at all.

---

## 5. RTOS: the "same RTOS on both chips" question

Researched facts:

- Official ThreadX RISC-V32 ports are **four months old** (v6.5.0_rel,
  2026-03) and target QEMU-virt with a standard PLIC/CLINT -- the C6 has
  Espressif's own interrupt matrix + PLIC flavor and SYSTIMER, so a port
  means hand-writing trap/tick/clock glue nobody has written. No ThreadX has
  ever run on any ESP32. No one has ever run the wireless blobs on ThreadX.
- The blobs are RTOS-agnostic in principle -- they call the OS through a
  **128-entry** function-pointer table (`wifi_osi_funcs_t`); NuttX's adapter
  is ~3,000 lines *plus* Espressif's vendored HAL underneath. Zephyr's is
  ~950 lines *plus* the same. Both are maintained by Espressif employees.
  A private ThreadX equivalent is multiple engineer-months and a permanent
  solo maintenance tail against blob/IDF drift.
- Under strategy C the uniformity goal is met where it counts: **every line
  of RTOS-adjacent code we own is ThreadX**, on the RA8. The C6's FreeRTOS
  is an implementation detail inside a sealed vendor appliance -- the same
  status as the RTOS inside any Wi-Fi module we might have bought.
- If shared application code across both chips is ever wanted (strategy D
  world), ThreadX ships an **official FreeRTOS compatibility layer**
  (utility/rtos_compatibility_layers/FreeRTOS) -- write shared code against
  the FreeRTOS API, run it natively on the C6 and via the layer on the RA8.
  Known limits documented (no tickless idle, a few unimplemented calls).

**Recommendation: do not port ThreadX to the C6.** Adopt C6-side FreeRTOS
only if strategy D ever activates, and register ESP-IDF (Apache-2.0 + MIT
FreeRTOS fork + blob submodules) as the SOUP unit at that point.

---

## 6. Updates: one pipeline, OTA and USB as transports

Full design in `UPDATE_PIPELINE.md`. The direct answers:

- **"Can you use the same pipeline for OTA and USB?" Yes.** Both deposit the
  same signed bundle into RA8-side staging (OSPI/SD); verify (RoT signature),
  apply (per-chip A/B), and confirm/rollback are identical and transport-blind.
- **"Download fully, then update without the cable"**: staging IS the design
  center -- the cable (or the Wi-Fi link) is only needed during STAGE.
- OTA ingress: the C6 (as hosted NIC) gives the RA8 a TCP/TLS socket via
  NetX; the RA8 downloads the bundle itself. The C6 never has authority over
  RA8 memory; each chip's own loader commits its own slots.
- C6's own update: esp-hosted's host-pushed co-processor OTA API, fed from the same
  staged bundle. One patch to own: the stock co-processor never self-confirms
  rollback (`esp_ota_mark_app_valid_cancel_rollback` absent) -- one-line
  co-processor rebuild, then re-pin the binary.
- Field recovery / factory provisioning: vendor **esp-serial-flasher**
  (Apache-2.0, portable C, C6 ROM-loader client, STM32/Zephyr precedent)
  instead of writing our own SLIP downloader -- research correction to the
  original spike plan. Requires the **C6 EN + BOOT pins wired to RA8 GPIOs**.
- The EK-RA8D2 two-USB self-test stands, with roles corrected (FS-as-host ->
  HS-as-device exercises the production ingest stack).

---

## 7. Hardware: what you own, what to buy, what transfers

**First: check the silkscreen on the ~20 modules.** "WROOM-1" is ambiguous:

- `ESP32-WROOM-32...` -> classic ESP32, Xtensa LX6, Wi-Fi 4, BT4.2. (Assumed
  below -- "older ESP32s" fits this.)
- `ESP32-S3-WROOM-1` -> S3, Xtensa LX7; adds USB-Serial-JTAG + BLE 5.0
  rehearsal value, still no Wi-Fi 6/802.15.4/RISC-V.
- `ESP32-C6-WROOM-1` -> these ARE the target module and the buy-list shrinks
  to carrier/devkit questions.

**What the classic boards CAN prototype now (all strategy-C groundwork):**

- esp-hosted-mcu explicitly supports **classic ESP32 as co-processor** (SDIO, SPI
  full-duplex, UART). The RA8-side ThreadX port of the host vtable, the SPI
  transport driver, the NetX Duo IP driver, version pinning, and the
  co-processor-OTA orchestration can all be developed against an old WROOM-32 wired
  to the EK-RA8D2, then re-pointed at a C6.
- The Wi-Fi OTA relay concept end-to-end (Wi-Fi 4 flavored), ESP-AT
  experiments, esptool workflow rehearsal.

**What they CANNOT validate:** anything RISC-V (the spike binary will not
execute -- different ISA), C6 registers/PCR clocking, Wi-Fi 6/TWT, BLE 5.x,
802.15.4, USB-Serial-JTAG, C6 secure-boot/eFuse choreography (different eFuse
map and scheme). Porting the spike itself to classic ESP32 would be a full
platform rewrite (Xtensa toolchain + windowed ABI, split IRAM/DRAM map, DPORT
clock gating) -- confirming the owner's suspicion: **our-style register
firmware is per-chip and per-ISA; only idf.py-style builds are
"device-agnostic", and only at the application layer.** (The esptool
*transport* and image *container* are cross-chip; chip id, addresses, and
ISA are not.)

**Buy:** 2-4x **ESP32-C6-DevKitC-1-N8** (~$9, DigiKey in stock July 2026;
DevKitM-1-N4 ~$8). One board is the sacrificial eFuse/secure-boot rehearsal
unit. Production path is the ESP32-C6-WROOM-1 module on our carrier -- same
firmware, and the module carries the FCC modular grant. Bench notes: the
devkit "LED" on GPIO8 is a WS2812 (the spike already documents this), and
the C6's native USB gives one-cable flash/console with no bridge.

---

## 8. Concerns the owner did not raise

1. **EN/BOOT wiring is a board-design commitment.** No EN+BOOT-to-RA8-GPIO
   traces = no field recovery, no factory provisioning, no unbrick. Must be
   locked into any carrier design first. (See UPDATE_PIPELINE.md.)
2. **eFuse burns are irreversible.** Secure Boot v2 (RSA-3072 or ECDSA-P256;
   ROM verifies the 2nd stage, 64 KB limit incl. our own bootloader if we
   ever ship one) and XTS-AES flash encryption are one-way fuses; a power cut
   during the first encryption pass corrupts flash. Rehearse with
   `CONFIG_EFUSE_VIRTUAL` (emulated fuses), then Development mode, and keep
   Release-mode burns for production units only. This repo has a bricking
   history (see the DLM recovery tooling) -- the C6 needs the same respect.
3. **Version lock is the #1 field hazard of the hosted architecture.** Host
   driver and co-processor firmware are protobuf-RPC-pinned; mismatch = boot loops /
   RPC timeouts (multiple 40+-comment upstream issues). Mitigation: the
   update bundle carries host firmware + C6 co-processor image as ONE artifact with
   a link-protocol version field, and CONFIRM requires the handshake.
4. **The inter-chip wire is plaintext.** esp-hosted claims no link encryption;
   our bundle signing covers integrity/authenticity of updates, but link
   traffic (e.g. downloaded book content) crosses the SPI wires in clear.
   On-board traces = acceptable for this product, but decide explicitly.
5. **Power sequencing and supervision.** The C6 rail must be up before hosted
   init (upstream crash reports otherwise); the RA8 needs a link watchdog +
   C6 reset authority (the EN pin again) as the escalation path.
6. **Defer 802.15.4/Thread.** RCP mode needs a dedicated UART (not muxed over
   the hosted link yet), a from-scratch OpenThread-on-ThreadX host port with
   zero public precedent, and Espressif recommends a second co-processor for
   production Wi-Fi+Thread coexistence. BLE, by contrast, is cheap: C6
   controller + NimBLE host on the RA8 over the muxed link (vHCI) or a
   dedicated H4 UART.
7. **ESP-AT is the wrong simpler-option for us.** Its C6 line is stalled at
   v4.1.1.0 (newer 5.0.x releases skip the C6), it terminates TCP/IP on the
   C6 (bypassing NetX and our TLS posture), has no host-push OTA, and its own
   docs concede low throughput/QoS. Mentioned for completeness; not proposed.
8. **Repo integration debts if esp32/ stays.** The format/tidy discovery
   scripts do not cover `esp32/` (only `check_comment_format.py` and
   `cite_check` see it today); there is no compile_commands for clangd (the
   editor shows false errors); the local hook's clang-format false-fails on
   Mac vs the pinned clang-format-22 (long-known divergence -- `make ci` on
   the dev box remains the truth). Wire `esp32/` into the gate matrix at
   graduation, or explicitly scope it out.
9. **Naming/monorepo.** The directory is chip-named (`esp32/`) in a repo that
   just went multi-chip (`ra8-firmware`, RA8D2+RA8P1). Under strategy C the
   RA8-side code lands in normal `libs/` homes (`ra_hosted_*` or
   `ra_net_pal`), and `esp32/` shrinks to: reference PDFs, the SOUP co-processor
   binary + justification, provisioning tools, and the bench spike. That is
   a comfortable monorepo shape; a separate repo buys nothing.
10. **SOUP + first-party inventory changes**: covered in full in section 9 --
    what stays (NetX Duo; NimBLE + `ra_ble_host` + the HCI seam; wired
    Ethernet), what the decision adds (esp-hosted host component + protobuf-c,
    the pinned C6 co-processor binary, esp-serial-flasher), and the removal executed:
    only the phantom on-chip BLE *controller* backend (`ra8d2_ble_regs.h`,
    `ra_ble_patch`, the never-vendored `ble_patch` blob) -- verified fiction,
    the RA8D2 has no radio. `ra_ble.c` was rewritten to a loopback transport;
    the BLE host + seam are the analog of NetX and are kept for the C6.
11. **C6 power lifecycle: deep-sleep by default (owner-directed, verified).**
    The RA8D2 is confirmed non-wireless (Renesas product page: "Wireless: No";
    datasheet: zero radio mentions), so all radio power is the C6's -- and it
    is large. C6 datasheet v1.5 numbers (3.3 V): Wi-Fi active ~252-354 mA TX /
    ~80 mA RX; light-sleep 180 uA; **deep-sleep 7 uA**. For scale the whole
    RA8D2 at 1 GHz draws ~183 mA, so an active C6 radio costs as much as the
    entire application processor, while a deep-sleeping C6 (7 uA) is ~40,000x
    lower and effectively free. Therefore, for the battery e-reader:
    - The C6 sits in **deep sleep the majority of its life**; the RA8 wakes it
      (via its power rail / EN GPIO) only for **download, sync, and OTA**.
    - Wireless is an **opt-in feature behind a user setting** (off by default).
    - **Idle auto-off**: if no data transfer has occurred in the last N
      minutes, the RA8 powers the C6 back down. Auto-off is **itself a
      setting** (enable/disable); if disabled, the user toggles Wi-Fi on/off
      manually.
    - Architectural consequences: reinforces the EN/BOOT-to-RA8-GPIO wiring
      (concern 1 -- the RA8 must power-cycle and wake the C6); the hosted-link
      driver must tolerate the C6 being off and re-init cleanly on wake
      (link-supervision state machine); the OTA/sync scheduler is what wakes
      the C6 on demand. esp-hosted supports host-driven co-processor power-down +
      re-init -- confirm the exact sequence during bring-up.

---

## 9. SOUP and first-party code impact: what stays, what goes, what's new

The co-processor decision reshapes the vendored-SOUP and first-party
inventory. Verified against the tree (the SBOM registry, `docs/SOUP/`, and the
network/BLE wiring) 2026-07-10.

### The trap to avoid: NetX Duo is NOT redundant

The tempting wrong move is "the C6 does networking, so delete NetX Duo." The
C6 does **L1/L2 only** -- radio + Wi-Fi MAC -- handing the RA8 raw 802.3
frames. **L3/L4 (IP/TCP/TLS/HTTP) stays on the RA8 = NetX Duo + mbedTLS.**
NetX is what turns the C6's frames into sockets; deleting it removes the
reason the frames are useful. This is exactly why esp-hosted was chosen over
ESP-AT (section 2): ESP-AT terminates TCP/IP on the C6 and *would* make NetX
redundant; we rejected it precisely to keep the stack, the TLS posture, and
control on the RA8. `libs/ra_net_pal` is already built for this -- its header
declares it "stack-agnostic," sitting between NetX and the `ra_eth` driver;
the C6 simply adds a PAL backend that sources frames from the hosted link
instead of `ra_eth`. NetX and the PAL stay; only the frame source beneath the
PAL changes.

### SOUP verdict

| SOUP component | Verdict | Why |
|---|---|---|
| netxduo | **Keep** | Your L3/L4 stack over the C6's raw frames. Load-bearing. |
| nimble | **Keep, repurpose** | BLE *host* stack -- moves from the RA8's on-chip controller to the C6's controller over HCI. Same pattern as NetX: keep the upper stack, swap the controller under it. |
| mbedtls, tf-psa-crypto | **Keep** | TLS for NetX; more relevant, not less. |
| threadx, filex, levelx, usbx | **Keep** | RA8 core (RTOS, FS, wear-level, USB). Unrelated to the C6. |
| miniz, stb, tinyxml2, litehtml | **Keep** | E-reader content rendering. |
| tflite-micro, flatbuffers, gemmlowp, ruy, vela | **Keep** | NPU/ML (RA8P1). Untouched. |
| r_sce_AMC | **Keep** | RSIP crypto coprocessor firmware. Unrelated. |
| **fsp_blobs/ble_patch** | **Remove (verified fiction)** | Blob for an on-chip BLE radio the RA8D2 does not have (datasheet: 0 mentions); never vendored. Part of the phantom-BLE-controller removal below. |

### New SOUP the decision adds

- **esp-hosted-mcu host component + the pinned C6 co-processor firmware binary** --
  the big one; its own `docs/SOUP/esp-hosted.md` justification, hash-pinned,
  host + co-processor versions locked as one artifact.
- **protobuf-c** -- esp-hosted's RPC wire encoding.
- **esp-serial-flasher** -- C6 recovery/provisioning (see `UPDATE_PIPELINE.md`).
- **NimBLE is NOT new** -- it is already vendored (`libs/third_party/nimble`,
  `docs/SOUP/nimble.md`); the decision repurposes it, it does not add it.

### BLE is the same architecture as Wi-Fi -- so most of it is KEPT

BLE over the C6 has the identical shape to the Wi-Fi story: the C6 is the
radio; the RA8 runs the upper stack. Only the split layer differs.

- **Wi-Fi** splits at the frame layer: C6 gives the RA8 raw 802.3 frames; the
  RA8 runs IP/TCP/TLS = **NetX Duo**. The seam is `ra_net_pal`.
- **BLE** splits at **HCI**: C6 runs the BLE *controller* (link layer, a blob
  on the C6); the RA8 runs the BLE *host* (L2CAP/ATT/GATT/security) over HCI =
  **NimBLE** (+ `ra_ble_host`). The seam is `ra_ble.h`.

So the BLE host is the analog of NetX, and deleting it would be the same trap
as deleting NetX. The code already had the right seam: `ra_ble.h` is a clean
HCI transport interface (`ra_ble_hci_send_command` / `_send_acl_data` /
`ra_ble_dispatch` + callbacks) -- the BLE equivalent of `ra_net_pal`. The
host talks HCI *through* it and never touches radio registers.

**KEPT (reusable for the C6, like NetX):** `ra_ble.h` (the HCI seam),
`libs/ra_ble_host/` (the BLE host stack), **NimBLE** (SOUP host), and all the
host + framing tests. For the C6 you write a new backend behind the seam that
carries HCI over the esp-hosted link; the host stack above is unchanged.

### Removal (executed 2026-07-10): only the phantom on-chip controller backend

**The RA8D2 has no on-chip Bluetooth radio.** Verified against the committed
datasheet (`docs/reference/ra8d2-datasheet.pdf`, R01DS0493EJ) and the Renesas
product page ("Wireless: No"): zero occurrences of Bluetooth/BLE/radio/wireless
in the datasheet (extraction validated). So the *implementation* of the HCI
seam that pretended to drive an on-chip controller was pure fiction and was
removed:

- `libs/ra_hal/inc/ra8d2_ble_regs.h` -- **invented register offsets** for a
  phantom block (its own header admitted the offsets were "placeholders").
  Same fiction pattern as the `ra_rsip` family. **Deleted.**
- `libs/ra_hal/src/ra_ble_patch.c` + `ra_ble_patch.h` -- a loader for an
  encrypted patch blob into the nonexistent radio. **Deleted.**
- `docs/SOUP/ble_patch_image.md` + the `fsp_blobs/ble_patch` SBOM entry --
  the blob was **never vendored** (SBOM recorded "Directory is absent on
  disk"). **Deleted.**
- `tests/test_ra_ble_patch.c` -- the patch-loader test. **Deleted.**

`libs/ra_hal/src/ra_ble.c` was **rewritten** from a register-poking phantom
backend into an **in-memory loopback HCI transport**: same H4 framing,
capture, and dispatch the host tests already drove, minus the invented
registers. It is the seam's placeholder backend until the C6 HCI backend is
written; `ra_ble.h`, `ra_ble_host`, NimBLE, and `test_ra_ble.c` (all its
convenience-wrapper + MC/DC coverage) stay green on it. Net removal: the
invented-hardware fiction only; the reusable BLE host + seam are preserved.

### Keep (owner decision, 2026-07-10): wired Ethernet stays

Reversed from an earlier "removal candidate" framing. The `ra_eth` family
(GWCA, COMA, RMAC, PHY, PTP/gPTP, the L3 switch; ~15 first-party files),
`ra_net_pal`'s `ra_eth` backing, and the board Ethernet glue are **kept**:

- It is real, working first-party code for a real on-chip peripheral (unlike
  the phantom BLE) -- substantial effort worth preserving.
- The repo is multi-chip/multi-product (RA8D2 + RA8P1, epic #220); wired
  Ethernet / PTP / TSN may serve a different product even though this
  wireless e-reader will not use the on-chip MAC.
- On "HW-marginal": that refers to issue **#21** -- the driver itself is
  correct and validated, but the *physical* RGMII link has marginal timing on
  a large-frame TX path (accepted-limitation, needs a logic analyzer to chase
  further). It is a wiring/timing limitation, not a code defect -- another
  reason not to throw the code away.
- Note the e-reader may still gain wired connectivity via a **USB-to-Ethernet
  dongle** -- but that path is USB host + CDC-ECM/RNDIS over **USBX + a NetX
  driver**, NOT the on-chip `ra_eth` MAC. It is a separate future consumer of
  NetX (one more reason NetX is load-bearing), and does not depend on `ra_eth`.

Net: the only actual removal is the phantom BLE controller (fiction); wired
Ethernet is retained. Whatever is cut must be cut **cleanly** (delete + update
every call site, per the zero-backward-compat policy), never left dormant.

---

## 10. What happens to this spike under the recommendation

Nothing here was wasted; the artifacts re-home:

- **Register map + TRM citations + committed manuals**: the permanent
  reference substrate for the recovery/provisioning path, bench debugging,
  and any future strategy-B/D revival. The [CONFIRM] discipline already
  caught two real errors (TXFIFO_CNT mask, IO_MUX base) before silicon.
- **RAM-app + Makefile + mkimage/flash tools**: the bench bring-up lane --
  first thing to run on a real C6 devkit to prove the ROM/console/pin
  assumptions before esp-hosted goes on. `esp_mkimage.py` also stays useful
  for understanding/verifying the image container our pipeline stages.
- **esp_hal.h DIP seam**: the pattern transfers to the RA8-side link driver
  facade (which is where the swappability requirement now actually bites).
- **update/README.md A/B design**: superseded on the C6 side by esp-hosted
  co-processor OTA (stock ota_0/ota_1 slots), but the bundle/manifest/rollback
  discipline moved up into `UPDATE_PIPELINE.md` intact.
- The from-scratch lane goes dormant, not deleted -- it is the escape hatch
  if Espressif's appliance ever fails the product.

---

## 11. Decision checklist for the owner

1. Adopt **strategy C** (hosted co-processor, C6 as SOUP appliance)? If no,
   which section-2 alternative and why?
2. Approve the buy: 2-4x C6-DevKitC-1-N8 (~$9 ea), one sacrificial.
3. Confirm what the ~20 old modules actually are (silkscreen: WROOM-32 vs
   S3-WROOM-1 vs C6-WROOM-1) -- it changes the prototyping lane, not the
   destination.
4. Green-light the WROOM-32 prototyping lane (RA8 ThreadX host port + NetX
   driver against an old board over SPI-FD) while C6 boards ship?
5. Transport commitment: standard SPI full-duplex (recommended; ~22 Mbps TCP,
   jumper-friendly) now; SDIO 4-bit only if a future need demands it (PCB).
6. BLE in scope now (NimBLE -- already vendored -- repointed onto the C6
   controller over HCI) or deferred? 802.15.4: proposed deferred indefinitely.
7. Security posture: accept plaintext inter-chip link? C6 secure boot +
   flash encryption at production only (with virtual-eFuse rehearsal)?
8. Update pipeline design in `UPDATE_PIPELINE.md`: approve bundle-of-both-
   chips + RA8 staging + esp-serial-flasher recovery (and the EN/BOOT PCB
   requirement)?
9. Gate wiring for `esp32/` (format/tidy/compile_commands) now or at
   graduation?
10. **BLE subsystem** -- DONE (2026-07-10, per your "purge fiction, keep
    host+seam" call). BLE is the same shape as Wi-Fi: C6 = controller, RA8 =
    host. Kept NimBLE + `ra_ble_host` + the `ra_ble.h` HCI seam (the BLE
    analog of NetX); removed only the phantom on-chip controller backend
    (invented `ra8d2_ble_regs.h`, `ra_ble_patch`, the never-vendored
    `ble_patch` blob); rewrote `ra_ble.c` to an in-memory loopback transport.
    Open follow-up when BLE is in scope: write the C6 HCI backend behind the
    seam (carry HCI over the esp-hosted link). (section 9)
11. **Wired Ethernet** -- decided KEEP per your call (real first-party code,
    reusable across products, HW-marginal = the #21 timing limitation not a
    code defect). No action; recorded for the record. (section 9)

---

## Appendix: primary sources

- ESP-Hosted-MCU: https://github.com/espressif/esp-hosted-mcu (transports +
  iperf table in README; docs/sdio.md; docs/bluetooth_design.md;
  docs/openthread_zigbee.md; host/esp_hosted_os_abstraction.h;
  host/api/include/esp_hosted_ota.h; examples/host_performs_slave_ota; <!-- LEGACY-OK: verbatim esp-hosted upstream repo paths -->
  issues #46, #25, #151, #204). Component registry:
  https://components.espressif.com/components/espressif/esp_hosted
- Espressif positioning: https://developer.espressif.com/blog/2025/09/esp-wifi-remote/ ;
  P4 companion statement: https://www.espressif.com/en/news/ESP32-P4
- Blobs + licenses: https://github.com/espressif/esp32-wifi-lib ;
  https://github.com/espressif/esp-phy-lib ; https://github.com/espressif/esp32c6-bt-lib ;
  https://github.com/espressif/esp-coex-lib
- Open-driver reality: https://github.com/esp32-open-mac/esp32-open-mac ;
  https://esp32-open-mac.be/ ; arXiv 2501.17684
- OS ports (same blobs): https://github.com/zephyrproject-rtos/hal_espressif ;
  https://github.com/espressif/esp-hal-3rdparty ;
  https://github.com/apache/nuttx (arch/risc-v/src/esp32c6/esp_wifi_adapter.c) ;
  https://lib.rs/crates/esp-wifi-sys
- Port performance/gaps: https://developer.espressif.com/blog/2024/06/zephyr-max-wifi-throughput/ ;
  zephyrproject-rtos/zephyr#105733 ; apache/nuttx#16915
- IDF consumption models: https://docs.espressif.com/projects/esp-idf/en/latest/esp32c6/api-guides/build-system.html
  ("Using ESP-IDF in Custom CMake Projects"; idf.py = CMake wrapper) ;
  https://github.com/espressif/esp-idf/tree/master/examples/build_system/cmake/idf_as_lib ; <!-- LEGACY-OK: verbatim upstream esp-idf URL (default branch) -->
  register headers SPDX `Apache-2.0 OR MIT` verified in
  components/soc/esp32c6/register/soc/uart_reg.h
- FreeRTOS coupling + osi table: esp_private/wifi_os_adapter.h (128 entries) ;
  https://esp32.com/viewtopic.php?t=6435 ; RF cal/NVS:
  https://docs.espressif.com/projects/esp-idf/en/latest/esp32/api-guides/RF_calibration.html
- ThreadX: https://github.com/eclipse-threadx/threadx (ports/risc-v32, v6.5.0
  release notes; utility/rtos_compatibility_layers/FreeRTOS)
- Secure boot / eFuse: https://docs.espressif.com/projects/esp-idf/en/latest/esp32c6/security/secure-boot-v2.html ;
  flash-encryption.html ; CONFIG_EFUSE_VIRTUAL in api-reference/system/efuse.html ;
  workflows: security/security-features-enablement-workflows.html
- Recovery flasher: https://github.com/espressif/esp-serial-flasher
- FCC: https://fccid.io/2AC7Z-ESPC6WROOM1 ; KDB 996369 D04
- Hardware matrix: esp32_datasheet_en.pdf, esp32-s3_datasheet_en.pdf,
  esp32-c6_datasheet_en.html (documentation.espressif.com) ; esptool:
  https://docs.espressif.com/projects/esptool/en/latest/ (auto-detect,
  load-ram, firmware-image-format) ; direct boot:
  https://github.com/espressif/esp32c3-direct-boot-example ; NuttX C6 "Simple
  Boot": https://nuttx.apache.org/docs/latest/platforms/risc-v/esp32c6/index.html
- Renesas-host precedent: https://docs.zephyrproject.org/latest/boards/arduino/portenta_c33/doc/index.html ;
  https://github.com/arduino/uno-r4-wifi-usb-bridge ; ESPHome co-processor-OTA:
  https://esphome.io/components/update/esp32_hosted/
- Buying: https://www.digikey.com/en/products/detail/espressif-systems/ESP32-C6-DEVKITC-1-N8/17728861 ;
  devkit guide (WS2812 GPIO8): https://docs.espressif.com/projects/esp-dev-kits/en/latest/esp32c6/esp32-c6-devkitc-1/user_guide.html
