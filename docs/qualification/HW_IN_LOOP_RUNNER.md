# Hardware-in-the-Loop CI Runner Setup (EK-RA8D2) -- DEPRECATED

**Status: DEPRECATED (2026-05-03).** This document describes a
self-hosted GitHub Actions runner that the project no longer pursues.
The current and **permanent** HIL posture is the developer-laptop
pre-push workflow documented in
[`../HIL_DEVELOPER_WORKFLOW.md`](../HIL_DEVELOPER_WORKFLOW.md).

A self-hosted runner is **out of scope** per
[`../CERTIFICATION_SCOPE.md`](../CERTIFICATION_SCOPE.md) (MIT-licensed
personal project; leased runner farms are not pursued). The remainder
of this document is preserved for historical reference only -- do not
follow these steps.

----

This document is the operator's manual for standing up a self-hosted
GitHub Actions runner that flashes every PR onto a real EK-RA8D2 board
and reports the result back to the PR. The runner does not exist yet --
this guide is for the person bringing one up for the first time.

The companion workflow is
[`.github/workflows/hardware-smoke.yml`](../../.github/workflows/hardware-smoke.yml),
which targets `runs-on: [self-hosted, ek-ra8d2]`.

## 1. Hardware required

| Item                              | Notes                                                                                    |
|-----------------------------------|------------------------------------------------------------------------------------------|
| Host machine                      | macOS Mac mini (M1 or newer) OR Linux x86_64 box. 8 GB RAM, 64 GB free disk minimum.     |
| EK-RA8D2 evaluation kit           | Renesas part `968-K7EKA8D2S01001BE`. One board per runner.                               |
| USB-C cable to **J10**            | Carries SWD + serial via the on-board J-Link OB. This is the cable the runner flashes.   |
| USB-C cable to **J11**            | High-speed USB device port -- needed for `usb_*` apps to enumerate to the host.          |
| USB-C cable to **J12**            | Full-speed USB device port -- needed for the second USB-device app slot.                 |
| Ethernet cable from **J7** to LAN | Required for any `net_*` app that calls out via DHCP.                                    |
| (Optional) USB-controllable PDU   | A `uhubctl`-compatible hub on J10 lets the runner power-cycle the board if JLink hangs.  |

Plug J10 first; the J-Link OB enumerates as a SEGGER device and is
what `JLinkExe` talks to. J11/J12/J7 are only required for the apps
under `examples/ek_ra8d2/usb_*` and `examples/ek_ra8d2/net_*`; if you
only care about the GPIO/UART tier you can omit them.

## 2. Software required

Install on the host before registering the runner:

| Tool                      | Why                                                                                  |
|---------------------------|--------------------------------------------------------------------------------------|
| `arm-none-eabi-gcc` 13+   | Cross-compiles every app and provides `arm-none-eabi-addr2line`.                     |
| `cmake` 3.20+, `ninja`    | Build system.                                                                        |
| `JLinkExe` (SEGGER pack)  | Drives the on-board J-Link OB. Install from <https://www.segger.com/downloads/jlink>.|
| `git`, `bash`, `python3`  | Runner agent + harness helpers.                                                      |
| GitHub Actions runner     | The agent itself. Downloaded per-repo from `Settings -> Actions -> Runners -> New`.  |
| (Optional) `uhubctl`      | For the power-cycle troubleshooting recipe below.                                    |

Confirm the toolchain is on PATH for the *runner's user account* (not
just your interactive shell):

```sh
which arm-none-eabi-gcc
which JLinkExe
which cmake
```

If `JLinkExe` is not on PATH, add it. The SEGGER macOS installer drops
it under `/Applications/SEGGER/JLink/`; the Linux `.deb` puts it in
`/opt/SEGGER/JLink/`.

### Sudo passwordless (optional, for power-cycle recovery)

If you wired a USB PDU and want the runner to recover from a hung
J-Link by toggling power, add a NOPASSWD sudoers rule scoped to just
`uhubctl`:

```
ci ALL=(root) NOPASSWD: /usr/sbin/uhubctl
```

The smoke harness itself does not require sudo; this is purely for
the recovery hook described in section 5.

## 3. Step-by-step setup

### 3.1 Create a dedicated user

Do **not** run the runner as your own login account.

```sh
# Linux
sudo useradd -m -s /bin/bash ci
sudo usermod -aG dialout,plugdev ci   # USB + serial access

# macOS (System Settings -> Users & Groups -> Add User -> "Standard")
# Then grant USB access by booting once with the J-Link cable plugged
# in so macOS records the device under the new account.
```

### 3.2 Download and register the runner

From the GitHub repo: `Settings -> Actions -> Runners -> New self-hosted runner`.
Pick the OS (macOS / Linux x64) and follow the displayed `curl` +
`./config.sh` commands as the `ci` user. When `config.sh` prompts for
labels, **add `ek-ra8d2`** in addition to the defaults:

```
Enter any additional labels (ex. label-1,label-2): ek-ra8d2
```

The workflow keys off that label (`runs-on: [self-hosted, ek-ra8d2]`),
so a typo here means PRs will queue forever.

### 3.3 Verify JLinkExe sees the board

As the `ci` user, with J10 plugged in:

```sh
JLinkExe -device R7KA8D2KF_CPU0 -if SWD -speed 4000 -autoconnect 1 -NoGui 1 <<<'q'
```

You should see `Connecting to target via SWD ... J-Link found 2 JTAG devices`.
If you instead see `Cannot connect to J-Link`, the runner user lacks
USB permission -- on Linux confirm `udev` rules from the SEGGER pack
are installed (`/etc/udev/rules.d/99-jlink.rules`) and re-plug J10.

### 3.4 Configure auto-start

#### Linux (systemd)

The runner's installer ships `svc.sh` which generates a unit. Run as
the `ci` user from the runner directory:

```sh
sudo ./svc.sh install ci
sudo ./svc.sh start
sudo systemctl status actions.runner.<owner>-<repo>.<runner-name>.service
```

The unit lives at `/etc/systemd/system/actions.runner.<owner>-<repo>.<runner-name>.service`.
Verify it has `Restart=always` so the runner survives a reboot.

#### macOS (launchd)

```sh
./svc.sh install
./svc.sh start
launchctl list | grep actions.runner
```

The plist is dropped at `~/Library/LaunchAgents/actions.runner.<owner>-<repo>.<runner-name>.plist`.
On macOS the plist must be loaded by the `ci` user's GUI session for
USB access to work, so log in once as `ci` before unplugging the
keyboard, or enable auto-login for that account.

### 3.5 Firewall / network

The runner is an **outbound-only** client -- it dials GitHub on TCP/443
and holds the connection open. You do **not** need to open any inbound
port. Allow:

- Outbound 443 to `*.github.com`, `*.actions.githubusercontent.com`, `objects.githubusercontent.com`.
- Outbound 443 to `pkg-containers.githubusercontent.com` (artifact upload).
- DHCP on the J7-attached LAN if you intend to run `net_*` apps that
  enumerate on the wired interface.

If the host is behind an HTTP proxy, set `https_proxy` in the systemd
unit / launchd plist before starting the service.

### 3.6 First end-to-end check

Open a draft PR that touches a comment in any app. Within ~30 s the
`hardware-smoke / EK-RA8D2 hardware smoke` check should turn yellow
(in progress) on the PR, and within ~5 minutes flip to green or red
with a comment posted.

## 4. What the PR comment looks like

The workflow renders a sticky comment (one per PR, edited in place)
that wraps the existing `build/smoke/results.md` table:

```
## EK-RA8D2 hardware smoke (self-hosted runner)

# EK-RA8D2 hardware smoke-test results

Generated: 2026-05-02T17:34:11Z
Probe: on-board J-Link OB (R7KA8D2KF_CPU0)
Settle window: 5s

| App         | Result | PC          | Symbol                    |
|-------------|--------|-------------|---------------------------|
| blink       | PASS   | 0x02000B2E  | ra8_delay_ms ra8_time.c     |
| blink_hal   | PASS   | 0x02000B2E  | ra8_delay_ms ra8_time.c     |
| uart_hello  | WIP    | 0x020014C2  | demo_panic_halt main.c    |
| net_dhcp    | FAIL   | 0xEFFFFFFE  | (lockup)                  |

## Summary
- Total apps: 4
- PASS:    2
- WIP:     1
- FAIL:    1
- NOBUILD: 0

---
Legend: **PASS** firmware reached its main loop, **WIP** firmware
reached a caught-error sink (warning), **UNKNOWN** PC did not match
any known pattern (warning), **FAIL** chip locked up or took a fault
(PR blocked), **NOBUILD** binary missing.

Job status: `failure`. Per-app JLink transcripts are in the
`hw-smoke-results` artifact.
```

The PR check goes red iff at least one row is FAIL. WIP / UNKNOWN /
NOBUILD are surfaced in the comment but do **not** block the PR.

## 5. Troubleshooting

### Stuck JLinkExe session

Symptom: `JLinkExe ... Cannot connect to J-Link, already in use.`

Cause: a previous job died without quitting `JLinkExe` (e.g. cancelled
mid-flash, or a pre-empted process holding the SWD lock).

Fix:

```sh
pkill -9 JLinkExe
pkill -9 JLinkGDBServer
```

If the J-Link OB itself is wedged (LEDs frozen), unplug J10 for 5 s
and replug. With a `uhubctl`-controlled hub:

```sh
sudo uhubctl -l 2-1 -a cycle -p <port>
```

### USB enumeration cache

Symptom: A `usb_hid_*` or `usb_msc_*` app flashes successfully but
the host never sees the device enumerate, even though the previous
run did.

Cause: macOS / Linux is caching the prior descriptor.

Fix on Linux:

```sh
sudo udevadm control --reload-rules && sudo udevadm trigger
```

Fix on macOS: unplug J11/J12, wait 10 s, replug. If that fails,
`sudo killall -HUP usbd` (macOS only; logs out the USB daemon).

### Ethernet not reachable

Symptom: `net_dhcp` parks in `panic_halt` (WIP) on every run.

Diagnose from the runner host:

```sh
ip a show dev <runner-ethernet-iface>
ping -c 1 <gateway>
```

If the host has no link, the EK-RA8D2 will not get DHCP either. Most
common cause is a managed switch port that took the runner off the
voice VLAN -- ask network ops to put the port on the same untagged
VLAN as the development LAN.

### Hung flash

Symptom: `scripts/dev/flash.sh` runs for > 60 s and the job hits the
`timeout-minutes: 30` cap.

Diagnose:

```sh
JLinkExe -device R7KA8D2KF_CPU0 -if SWD -speed 4000 -autoconnect 1 -NoGui 1 <<EOF
unlock RA
erase
q
EOF
```

`unlock RA` clears the DLM (Device Lifecycle Management) lock that
`secure_app` writes during PSA bring-up. After an unlock, the next
flash should succeed. If not, the OctoSPI may have latched into a
mode the J-Link cannot recover from -- power-cycle the board.

### Runner offline in GitHub UI

Check the agent log:

```sh
# Linux
sudo journalctl -u actions.runner.<owner>-<repo>.<runner-name>.service -n 200

# macOS
tail -200 ~/actions-runner/_diag/Runner_*.log
```

Most common: the agent's PAT or registration token expired. Re-run
`./config.sh remove` followed by a fresh `./config.sh` from the repo's
runner-add page.

## 6. Dual-billing avoidance

The cross-build matrix in `.github/workflows/firmware.yml` is pinned
to `runs-on: ubuntu-latest`, so it never lands on the HW runner. Do
not change that pin without first re-reading this document -- letting
the matrix run on the HW runner saturates the single board and will
queue every PR behind whichever job grabbed it first.
