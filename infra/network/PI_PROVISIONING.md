# infra/network/PI_PROVISIONING.md -- provisioning a Raspberry Pi on the bench

Reference notes for bringing up a fresh Raspberry Pi headless on the bench
network. Every gotcha below cost a real boot cycle to discover, and the next
Pi on this bench will hit all of them, so they are recorded here rather than
relearned. For the bench network itself, see [README.md](README.md).

The reference image is **`2026-06-18-raspios-arm64-lite`** (Raspberry Pi OS,
Debian GNU/Linux 13 "trixie", arm64, Lite). The details below were confirmed
against that image; a different image may move some of them.

## 1. This image provisions via cloud-init, not custom.toml

The trixie `2026-06-18-raspios-arm64-lite` image provisions from **cloud-init**:
`user-data`, `network-config`, and `meta-data` files on the boot (FAT)
partition. The older Raspberry Pi Imager `custom.toml` mechanism is **silently
ignored** on this image -- it is not read, and nothing warns that it was
skipped. Put your configuration in the cloud-init files, not `custom.toml`.

## 2. network-config applies on FIRST BOOT ONLY

cloud-init renders `network-config` **only on the first boot** (documented
cloud-init behaviour, not a Pi quirk). Bumping the `instance_id` in `meta-data`
makes cloud-init re-run the `user-data` modules, but it does **not** re-render
the network configuration. So if the network is wrong on the first boot, editing
`network-config` and bumping `instance_id` will not fix it -- re-flash, or
reconfigure the network on the running system by hand.

## 3. Raspberry Pi OS ships sshd DISABLED -- the `ssh` flag file enables it

`ssh.service` is disabled out of the box. The switch that enables it is an
**empty file named `ssh`** in the root of the boot partition: `sshswitch.service`
consumes that file on boot, enables sshd, then deletes the flag. cloud-init
writing an `authorized_keys` entry does **not** enable sshd -- you can hand the
Pi your key and still not be able to reach it. Drop the empty `ssh` file on the
boot partition every time.

## 4. cloud-init `users:` FAILED SILENTLY -- use userconf.txt for the account

On this image the cloud-init `users:` module created **no account at all**, and
said nothing about it -- while `hostname` and `packages` from the *same*
`user-data` file applied normally. The result is a Pi you cannot log into.

What works instead is Raspberry Pi OS's own first-user mechanism: **`userconf.txt`**
on the boot partition, a single line of `username:sha512-hash` (generate the hash
with, e.g., `echo -n 'password' | openssl passwd -6 -stdin`). Use `userconf.txt`
to create the account; do not rely on cloud-init `users:` on this image.
cloud-init is still fine for hostname, packages, and (first boot) network.

## 5. enable_uart=1 is NOT set by default -- the serial console is silent without it

`cmdline.txt` already carries `console=serial0,115200`, which makes it look like
the serial console is on. It is not: **`enable_uart=1` is absent from
`config.txt` by default**, and without it the UART is never brought up and the
console is completely silent -- no boot log, no login prompt, nothing on the
wire. Add `enable_uart=1` to `config.txt` on every image. On a headless bench Pi
this one line is the difference between debugging a failed boot and guessing at
it.

## Boot-partition checklist

Before first boot, the boot (FAT) partition should carry:

| File             | Purpose                                                     |
|------------------|-------------------------------------------------------------|
| `config.txt`     | contains `enable_uart=1` (gotcha 5)                          |
| `ssh`            | empty file; enables sshd (gotcha 3)                         |
| `userconf.txt`   | `username:sha512-hash`; creates the account (gotcha 4)     |
| `user-data`      | cloud-init: hostname, packages, keys (NOT the account, NOT `custom.toml`) |
| `network-config` | cloud-init network; first boot only (gotcha 2)             |
| `meta-data`      | cloud-init; may be empty or carry `instance_id`            |

With those in place the Pi comes up headless with a serial console, an enabled
sshd, a real login account, and (on first boot) its network -- which is the
outcome the gotchas above are here to guarantee.
