// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package proxmox

import (
	"encoding/json"
	"fmt"
	"regexp"
	"strings"
)

var (
	hostPCIKeyPattern  = regexp.MustCompile(`^hostpci[0-9]+$`)
	usbKeyPattern      = regexp.MustCompile(`^usb[0-9]+$`)
	serialKeyPattern   = regexp.MustCompile(`^serial[0-9]+$`)
	parallelKeyPattern = regexp.MustCompile(`^parallel[0-9]+$`)
)

// checkNoHostDevices refuses a guest that reaches host hardware directly.
//
// checkNetworks holds every interface to a reviewed guest bridge and checkDisks
// holds every volume to the approved storage, so the two paths a disposable
// guest normally has off its own machine are both bounded. A passed-through
// device is a third path neither of them can see. hostpciN hands the guest a
// real PCI function, including a NIC that sits on whatever the host has it
// plugged into, so a guest with an impeccable net0 can still be on the
// management network through a card no line of its config describes. usbN and
// parallelN are the same shape at a smaller size, serialN bound to a host path
// is a line onto the host's own console hardware, and args is the QEMU command
// line itself, which can carry -netdev, -drive and -device settings the config
// keys this package reads never mention. None of them are refused by pool,
// marker, name, digest, bridge or storage.
//
// The judgement is on the KEY, not on how harmless a particular setting looks:
// this client approves bridges and storage by allowlist, and there is no
// allowlist of reviewed host devices to check a value against. A serial port
// on "socket" and a USB port on "spice" are the two settings that name no host
// device at all (both are Proxmox's own virtual ports), so they are the only
// values accepted, and anything else, an unreadable setting included, is
// refused.
//
// subject names whose configuration it is, so a refusal says whether the device
// is on the reservation or on the template it would be cloned from.
func checkNoHostDevices(config map[string]json.RawMessage, subject string) error {
	for key, raw := range config {
		switch {
		case hostPCIKeyPattern.MatchString(key):
			return fmt.Errorf("%w: %s passes through host PCI device %s", ErrConflict, subject, key)
		case parallelKeyPattern.MatchString(key):
			return fmt.Errorf("%w: %s passes through host parallel port %s", ErrConflict, subject, key)
		case key == "args":
			value, err := deviceSetting(raw)
			if err != nil {
				return err
			}
			if strings.TrimSpace(value) != "" {
				return fmt.Errorf("%w: %s declares raw QEMU arguments", ErrConflict, subject)
			}
		case usbKeyPattern.MatchString(key):
			value, err := deviceSetting(raw)
			if err != nil {
				return err
			}
			if firstSetting(value) != "spice" {
				return fmt.Errorf("%w: %s USB port %s names a host device", ErrConflict, subject, key)
			}
		case serialKeyPattern.MatchString(key):
			value, err := deviceSetting(raw)
			if err != nil {
				return err
			}
			if firstSetting(value) != "socket" {
				return fmt.Errorf("%w: %s serial port %s names a host device", ErrConflict, subject, key)
			}
		}
	}
	return nil
}

// deviceSetting reads a configuration value that must be a string. A setting
// this client cannot read is a setting it cannot judge, so it is reported as a
// protocol failure rather than skipped.
func deviceSetting(raw json.RawMessage) (string, error) {
	var value string
	if err := json.Unmarshal(raw, &value); err != nil {
		return "", ErrProtocol
	}
	return value, nil
}

// firstSetting returns the leading comma-separated field of a Proxmox setting,
// which is where a port's backing device is named: "spice", "socket",
// "host=046d:c52b", "/dev/ttyS0".
func firstSetting(value string) string {
	field, _, _ := strings.Cut(value, ",")
	return field
}
