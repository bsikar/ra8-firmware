# Unified Firmware Update Pipeline Contract

> **Status: DESIGN CONTRACT.** Specifies the declarative update-pipeline
> model uniting HTTPS OTA (`libs/ra8_ota`) and USB-DFU (`libs/ra8_dfu`,
> `libs/ra8_dfu_device`, `examples/ek_ra8d2/hw_validated/hil/dfu_bootloader`)
> over one verify/stage/commit/rollback core and the signed N-image bundle
> payload format, building on [c6_update_pipeline.md](c6_update_pipeline.md).

## 1. Purpose

Today the repository maintains two independently hand-wired update flows:

1. **OTA (`libs/ra8_ota`, `examples/ek_ra8d2/hw_pending/ota_ab_orchestration`):**
   A pull-driven Non-Secure cooperative state machine (`IDLE -> CHECKING ->
   DOWNLOADING -> VERIFYING -> COMMITTING -> DONE | ERROR`) with injected
   network, crypto, and flash interfaces and secure bank-swap veneers
   (`libs/ra8_nsc/src/ra8_nsc_ota.c`).
2. **DFU (`libs/ra8_dfu`, `libs/ra8_dfu_device`,
   `examples/ek_ra8d2/hw_validated/hil/dfu_bootloader`):**
   A push-driven Secure immutable 128 KiB bootloader hosting USBX DFU 1.1 on
   USB-FS/HS, 444 KiB A/B code-MRAM slots with header-last (`"RA8D"`) commits,
   dual-copy ping-pong trial `boot_state` + WDT0 reset guard, and copy-to-run
   RoT verification (`libs/ra8_rot_launch`, `libs/ra8_rot`).

The unified pipeline makes transport (OTA vs DFU) and storage activation
declarative choices rather than duplicated verification, staging, commit, and
rollback implementations.

## 2. Pipeline Declaration Model

An update pipeline flow is declared statically as a value of
`ra8_update_pipeline_decl_t` before running any stage:

```c
typedef enum : uint8_t {
  k_ra8_update_transport_ota = 0U, /* Pull stream over TLS / link (HTTPS/C6) */
  k_ra8_update_transport_dfu = 1U, /* Push block ingress over USB-FS / USB-HS */
  k_ra8_update_transport_mem = 2U, /* Pre-staged memory/blockdev input (host tests, recovery) */
} ra8_update_transport_t;

typedef enum : uint8_t {
  k_ra8_update_payload_bundle       = 0U, /* Signed N-image RA8U container */
  k_ra8_update_payload_manifest_raw = 1U, /* Legacy OTA JSON manifest + raw blob */
  k_ra8_update_payload_rot1_slot    = 2U, /* Legacy DFU raw body + ROT1 trailer */
} ra8_update_payload_t;

typedef enum : uint8_t {
  k_ra8_update_apply_ab_slots = 0U, /* Copy-to-run A/B MRAM slots + boot_state */
  k_ra8_update_apply_ab_banks = 1U, /* Dual-bank boot-area swap via NSC veneer */
  k_ra8_update_apply_staged   = 2U, /* Staging store feeding multi-chip appliers */
} ra8_update_apply_t;

typedef struct {
  ra8_update_transport_t transport;
  ra8_update_payload_t   payload;
  ra8_update_apply_t     apply_mode;
  uint8_t                trial_budget;      /* 0 => immediate commit; >0 => trial boots */
  bool                   enforce_rot;       /* Must be true in production builds */
  bool                   enforce_rollback;  /* Floor check before commit/launch */
  bool                   defer_floor_bump;  /* True => bump OTP floor on confirm only */
} ra8_update_pipeline_decl_t;
```

### Canonical Declarations

| Flow | `transport` | `payload` | `apply_mode` | `trial_budget` | `defer_floor_bump` |
|---|---|---|---|---|---|
| Production USB-DFU | `dfu` | `bundle` (compat: `rot1_slot`) | `ab_slots` | `3` | `true` |
| Production Wi-Fi/HTTPS OTA | `ota` | `bundle` (compat: `manifest_raw`) | `ab_banks` / `staged` | `3` (or `0` bank swap) | `true` |
| Single-chip OTA demo | `ota` | `manifest_raw` | `ab_banks` | `0` | `false` |

## 3. Shared Stage Machine (`STAGE -> VERIFY -> COMMIT -> CONFIRM | ROLLBACK`)

```text
  [Transport Ingress: OTA pull / DFU push / Staged]
                         |
                         v
  1. PREFLIGHT   Validate declaration, open inactive slot / staging window,
                 record baseline active slot & rollback version.
                         |
                         v
  2. STAGE       Stream 32-byte-aligned blocks/chunks into inactive storage.
                 Compute/update SHA-256; never touch active metadata on error.
                         |
                 [transfer complete]
                         v
  3. VERIFY      (DEFAULT-DENY, authenticity before freshness):
                 a) Screen container header + descriptors (bounds, alignment).
                 b) Read back staged payload; verify per-image SHA-256 digests.
                 c) Bind metadata + digest and check ECDSA-P256 against RoT key.
                 d) Check monotonic floor: img_version >= stored_min
                    (verify-only during trial; never advance OTP before confirm).
                 e) Enforce companion protocol compatibility (if multi-image).
                         |
          +--------------+--------------+
          | (verify fails)              | (verify passes)
          v                             v
  4. ROLLBACK (abort)          4. COMMIT (atomic activation)
     Discard/invalidate           - ab_slots: write header-last ("RA8D") +
     staged bytes; active           boot_state(trial_slot, tries=budget,
     slot/bank untouched.           confirmed=false, seq).
                                  - ab_banks: call ra8_nsc_ota_commit(inactive).
                                  - multi-chip: apply C6 slot first, wait ready.
                                        |
                                        v
                               5. CONFIRM / TRIAL REVERT
                                  - Boot payload under WDT0 trial timer (if
                                    trial_budget > 0).
                                  - Self-test pass -> ra8_dfu_confirm(); next
                                    confirmed boot (or pre-DFU gate) advances
                                    Extra-MRAM anti-rollback floor.
                                  - Fault / hang -> WDT0 resets -> decrement
                                    tries -> tries==0 reverts to prior slot.
```

### Shared Core Seams (Dependency Inversion)

All hardware and protocol dependencies stay behind narrow injected vtables so
both transports share the exact same stage logic and host unit tests run
without silicon hardware:

1. **Ingress (`stage` producer):** Supplies pulled chunks (`read`) or pushed
   blocks (`on_block` / `on_finish`).
2. **Staging / Slot Store (`storage`):** 32-byte-aligned `prepare`, `write`,
   `readback`, `commit_slot` / `set_startup`, `boot_state_read/write`, keeping
   MRAM programming loops in SRAM (`.sram_text`).
3. **Crypto & Anti-Rollback (`verify`):** Streaming SHA-256, ECDSA-P256 verify
   against the provisioned root key, constant-time digest compare, and split
   anti-rollback (`verify_no_commit` on trial, `verify` on confirmed).
4. **Confirm / Revert Hook:** Trial watchdog arming, `ra8_dfu_confirm()`, and
   rollback status notification.

## 4. Trust Boundaries

| Boundary | Execution Context | Authority & Isolation Rule |
|---|---|---|
| DFU Bootloader & Root of Trust | `{World: S}`, immutable 128 KiB MRAM (`0x0200_0000`) | Owns code-MRAM write windows (`MRCPC1`), `boot_state`, Extra-MRAM anti-rollback counter (`0x02E0_7600`), and copy-to-run branch (`0x2202_0000`). Only `dfu_bootloader` / `ra8_rot_launch` invoke `ra8_dfu_launch*`. |
| OTA Orchestration | `{World: NS}` (`libs/ra8_ota/inc/ra8_ota.h`) | Downloads, parses, and stages strictly into NS/staging memory. Cannot flip hardware boot configuration directly; must call `{World: NSC}` veneers `ra8_nsc_ota_commit` / `ra8_nsc_flash_bank_config` (`libs/ra8_nsc/src/ra8_nsc_ota.c`), which validate scalars before invoking `libs/ra8_secure_app`. |
| Deferred OTP Floor | `{World: S}` (`libs/ra8_rot/src/antirollback_abi.zig`) | Unconfirmed trials call non-committing floor checks (`ra8_rot_antirollback_verify_no_commit`) so WDT0 reverts remain valid; only confirmed launches (`ra8_dfu_launch`) or confirmed pre-DFU commits (`blc_commit_confirmed_slot_floor`) advance `0x02E0_7600`. |
| Companion (ESP32-C6) | Off-chip MCU across companion link | Neither chip writes the other's flash directly; RA8 verifies the outer bundle signature while the C6 bootloader/OTA agent independently checks SHA-256 and its own rollback counter before marking its slot valid. |

## 5. Signed N-Image Bundle Payload (`"RA8U"`)

To eliminate format drift between OTA manifests and DFU slot binaries, both
transports carry the same self-describing signed container:

```text
  Offset                    Size     Section
  ------------------------  -------  -------------------------------------------
  0x0000                    32 B     ra8_bundle_hdr_t ("RA8U" header)
  0x0020                    N * 64 B ra8_bundle_image_desc_t[N] (descriptors)
  0x0020 + N * 64           64 B     ECDSA-P256 raw signature (r || s)
  payloads_offset (32B aln) variable N payloads, each at 32-byte aligned offset
```

Every section offset (`0x0020`, `0x0020 + N * 64`, `payloads_offset`, and each
`desc[i].offset`) is a multiple of 32 bytes so staged writes map directly onto
RA8D2 MRAM page boundaries.

### 5.1 Bundle Header (`32` bytes, little-endian)

| Offset | Type | Field | Contract |
|---|---|---|---|
| `0x00` | `u32` | `magic` | `"RA8U"` (`0x52413855`) |
| `0x04` | `u16` | `format_version` | `1` (must match exact major format version) |
| `0x06` | `u16` | `header_bytes` | `32` |
| `0x08` | `u32` | `bundle_version` | Monotonic security counter checked against anti-rollback floor |
| `0x0C` | `u16` | `image_count` | `1 .. 4` (`0` and `> max` rejected) |
| `0x0E` | `u16` | `flags` | Reserved, must be `0` |
| `0x10` | `u32` | `total_bytes` | Exact container byte length; equals max aligned image end |
| `0x14` | `u32` | `payloads_offset` | Must equal `32 + image_count * 64 + 64` (32-byte aligned) |
| `0x18` | `u32` | `sig_len` | Active signature length (`64` on target; `<= 64` on host) |
| `0x1C` | `u32` | `reserved` | Must be `0` |

### 5.2 Image Descriptor (`64` bytes each, little-endian)

| Offset | Type | Field | Contract |
|---|---|---|---|
| `0x00` | `u32` | `target` | Target processor ID (`1 = RA8_APP`, `2 = ESP32_C6`, `3 = DISPLAY`) |
| `0x04` | `u16` | `format` | Payload type (`0 = raw`, `1 = ROT1_IMAGE`, `2 = C6_APP_IMAGE`) |
| `0x06` | `u16` | `flags` | Per-image policy flags (reserved `0` unless set by spec) |
| `0x08` | `u32` | `image_version` | Component version (`<= bundle_version` or independent component ver) |
| `0x0C` | `u32` | `compat_proto_ver` | Required companion-link protocol version (`0` if N/A) |
| `0x10` | `u32` | `offset` | Byte offset from container start; `>= payloads_offset`, `% 32 == 0` |
| `0x14` | `u32` | `size` | Unpadded payload size in bytes (`> 0`, non-overlapping, in bounds) |
| `0x18` | `u32` | `reserved0` | Must be `0` |
| `0x1C` | `u32` | `reserved1` | Must be `0` |
| `0x20` | `u8[32]` | `sha256` | `SHA-256(bundle[offset .. offset + size])` |

### 5.3 Authentication & Compatibility Rules

1. **Single RoT signature covers full manifest:** ECDSA-P256 signs
   `SHA-256(header[0..32] || desc[0 .. N*64])`, pinning `bundle_version`, all
   `image_count` targets, protocol compatibility versions, sizes, offsets, and
   every image digest before any payload bytes are activated.
2. **Per-image integrity:** Re-hashing `bundle[desc[i].offset .. +desc[i].size]`
   from staging/flash must match `desc[i].sha256` in constant time (`ra8_ct_equal`).
3. **Bounds & non-overlap:** Descriptors must stay strictly within
   `[payloads_offset, total_bytes)`, aligned to 32 bytes, ordered with zero
   overlap (`desc[i].offset >= prev_aligned_end`).
4. **Single-image RA8 compatibility:** A RA8-only bundle has `image_count = 1`,
   `target = RA8_APP`, and `offset`/`size` covering either a raw linked body or a
   `ROT1`-suffixed image ([the ROT1 format specification](../formats/ROT1.md)); unwrapping extracts the
   exact RA8 slot image for `dfu_bootloader`.

## 6. Current Flows Annotated Against This Contract

| Dimension | OTA (`libs/ra8_ota` + `ota_ab_orchestration`) | DFU (`libs/ra8_dfu` + `libs/ra8_dfu_device` + `dfu_bootloader`) | Unified Pipeline Target |
|---|---|---|---|
| **Agrees: Staging storage** | Writes inactive MRAM bank in 32-byte aligned blocks (`ra8_flash`) while active firmware runs undisturbed. | Writes inactive 444 KiB MRAM slot in 32-byte pages (`ra8_dfu_program_image`) while immutable bootloader/active slot stays untouched. | Shared 32-byte-aligned staging/slot store interface. |
| **Agrees: DI & pure core** | Pure Zig `libs/ra8_ota/src/internal/root.zig` decoupled from `net`/`crypto`/`flash` vtables (`ra8_ota_abi.zig`). | Pure Zig cores in `libs/ra8_dfu/src/internal/`, `libs/ra8_dfu_device/src/internal/device.zig`, `libs/ra8_rot/src/internal/`. | Shared `STAGE -> VERIFY -> COMMIT -> CONFIRM` pure Zig state/step core + thin transport adapters. |
| **Agrees: Crypto authority** | Computes SHA-256 + binds version/URL/size + runs ECDSA check before `COMMITTING`; constant-time compare (`ra8_ct_equal`). | Computes SHA-256 + binds `img_version` + runs ECDSA-P256 (`ra8_rot_verify_image`) before copy-to-run branch. | Shared verification stage verifying `"RA8U"` signature + per-image SHA-256 + anti-rollback before activation. |
| **Agrees: Safe abort path** | Bad digest/sig moves to `k_ra8_ota_state_error`, skips `set_startup`, preserves Bank A. | Corrupt download leaves `"RA8D"` header unwritten; bad signature/version returns to DFU or falls back on trial exhaustion. | Verification failure never activates staged slot/bank. |
| **Diverges: Ingress** | Cooperative pull from HTTPS/RAM (`ra8_ota_net_iface_t`, JSON manifest + raw stream). | USB-FS/HS DFU 1.1 push callbacks (`onWrite` / `onNotify` / worker loop). | Both act solely as ingress feeders into the shared `STAGE` sink. |
| **Diverges: Payload format** | 2 KiB JSON manifest struct (`ra8_ota_manifest_t`) + separate binary blob URL. | Unwrapped body bytes + `"RA8D"` 32-byte trailer at slot top + optional 116-byte `ROT1` trailer after body. | Signed N-image `"RA8U"` bundle consumed identically by OTA and DFU (`compat` wrappers kept for legacy tests). |
| **Diverges: World & commit** | Executes in `{World: NS}` (`ota_ab_orchestration`), commits swap via `{World: NSC}` `ra8_nsc_ota_commit`. | Executes in `{World: S}` (`dfu_bootloader`), commits `"RA8D"` header + ping-pong `boot_state` + copy-to-run (`0x2202_0000`). | Declared `apply_mode` preserves each trust boundary without changing verification logic. |
| **Diverges: Trial/Rollback** | Binary pass/fail on verify (`set_startup` not called on fail; no post-boot WDT0 trial countdown yet). | Multi-boot trial budget (`tries_left = 3`), WDT0 expiry reset, `ra8_dfu_confirm()`, deferred OTP floor commit (`ra8_dfu_launch_trial`). | `trial_budget` + `defer_floor_bump` unified under shared trial/confirm/rollback policy. |

## 7. Convergence Plan

1. **Shared Seam Extraction:** Introduce a pure Zig/C core encapsulating
   pipeline declarations and `STAGE -> VERIFY -> COMMIT -> ROLLBACK` transitions
   over storage, crypto, and clock/rollback interfaces.
2. **N-Image Bundle Core:** Implement `"RA8U"` header/descriptor parsing,
   32-byte alignment and non-overlap bounds validation, metadata-bound ECDSA-P256
   verification, and per-image SHA-256 verification with host unit tests.
3. **OTA & DFU Convergence:** Re-drive `libs/ra8_ota` and
   `libs/ra8_dfu`/`dfu_bootloader` onto the shared seam and `"RA8U"` bundle
   acceptor while keeping every existing public C ABI and security/rollback
   guarantee intact.
