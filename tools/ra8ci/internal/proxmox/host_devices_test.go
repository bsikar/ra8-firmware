// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package proxmox

import (
	"context"
	"encoding/json"
	"errors"
	"strconv"
	"strings"
	"testing"
)

// rawConfig builds the shape inspect() reads, so a rule test can state a
// configuration the way Proxmox answers one.
func rawConfig(values map[string]any) map[string]json.RawMessage {
	config := make(map[string]json.RawMessage, len(values))
	for key, value := range values {
		encoded, err := json.Marshal(value)
		if err != nil {
			panic(err)
		}
		config[key] = encoded
	}
	return config
}

// A passed-through device is a path off the guest that neither the bridge rule
// nor the storage rule can see, so each family is refused on its own.
func TestAPassedThroughHostDeviceIsRefused(t *testing.T) {
	for _, tc := range []struct {
		name   string
		config map[string]any
	}{
		{name: "PCI function", config: map[string]any{"hostpci0": "0000:01:00.0,pcie=1"}},
		{name: "second PCI function", config: map[string]any{"hostpci3": "0000:03:00.0"}},
		{name: "PCI mapped by name", config: map[string]any{"hostpci0": "mapping=nic-lab"}},
		{name: "parallel port", config: map[string]any{"parallel0": "/dev/parport0"}},
		{name: "USB by vendor and product", config: map[string]any{"usb0": "host=046d:c52b"}},
		{name: "USB by bus path", config: map[string]any{"usb1": "host=1-2"}},
		{name: "USB mapped by name", config: map[string]any{"usb2": "mapped=probe"}},
		{name: "serial on a host tty", config: map[string]any{"serial0": "/dev/ttyS0"}},
		{name: "serial on a USB tty", config: map[string]any{"serial1": "/dev/ttyUSB0"}},
		{name: "raw QEMU arguments", config: map[string]any{"args": "-device vfio-pci,host=01:00.0"}},
		{name: "a USB port that says nothing", config: map[string]any{"usb0": ""}},
		{name: "a serial port that says nothing", config: map[string]any{"serial0": ""}},
		{name: "a settingless port", config: map[string]any{"usb0": nil}},
	} {
		t.Run(tc.name, func(t *testing.T) {
			if err := checkNoHostDevices(rawConfig(tc.config), "reservation"); !errors.Is(err, ErrConflict) {
				t.Fatalf("host device accepted: %v", err)
			}
		})
	}
}

// The ordinary disposable guest carries none of this, and the two virtual
// ports that name no host device stay usable.
func TestAGuestWithNoHostDeviceIsAccepted(t *testing.T) {
	for _, tc := range []struct {
		name   string
		config map[string]any
	}{
		{name: "an ordinary guest", config: map[string]any{
			"name": "ra8-lab-linux-example", "description": "RA8CI_RESERVATION=x;RA8CI_OPERATION=y",
			"net0": "virtio=AA:BB:CC:DD:EE:01,bridge=vmbr8", "scsi0": "ra8-tf-lab:vm-9000-disk-0,size=32G",
		}},
		{name: "a serial console on a socket", config: map[string]any{"serial0": "socket"}},
		{name: "a spice USB port", config: map[string]any{"usb0": "spice"}},
		{name: "a spice USB port with settings", config: map[string]any{"usb1": "spice,usb3=1"}},
		{name: "an empty args", config: map[string]any{"args": ""}},
		{name: "a whitespace args", config: map[string]any{"args": "   "}},
		{name: "no configuration at all", config: map[string]any{}},
	} {
		t.Run(tc.name, func(t *testing.T) {
			if err := checkNoHostDevices(rawConfig(tc.config), "reservation"); err != nil {
				t.Fatalf("ordinary configuration refused: %v", err)
			}
		})
	}
}

// The rule reads device keys, not keys that merely begin like one, so nothing
// ordinary is swept up by the match.
func TestOnlyDeviceKeysAreJudged(t *testing.T) {
	for _, key := range []string{
		"hostpci", "hostpcix", "hostpci0x", "usb", "usbtablet", "usb0x",
		"serial", "serialize", "parallel", "parallelism", "argsx", "sargs", "tablet",
	} {
		t.Run(key, func(t *testing.T) {
			if err := checkNoHostDevices(rawConfig(map[string]any{key: "/dev/ttyS0"}), "reservation"); err != nil {
				t.Fatalf("%q was judged as a device key: %v", key, err)
			}
		})
	}
}

// Every index of every family is judged, not just the first one Proxmox
// happens to number.
func TestEveryDeviceIndexIsJudged(t *testing.T) {
	for _, family := range []string{"hostpci", "usb", "serial", "parallel"} {
		for index := 0; index < 10; index++ {
			key := family + strconv.Itoa(index)
			config := rawConfig(map[string]any{
				"net0": "virtio=AA:BB:CC:DD:EE:01,bridge=vmbr8",
				key:    "host=046d:c52b",
			})
			if err := checkNoHostDevices(config, "reservation"); !errors.Is(err, ErrConflict) {
				t.Fatalf("%s accepted: %v", key, err)
			}
		}
	}
}

// A refusal has to say whose configuration carries the device and which key it
// is, because the reservation and the template it came from are fixed in
// different places.
func TestTheRefusalNamesTheSubjectAndTheDevice(t *testing.T) {
	for _, subject := range []string{"reservation", "source template"} {
		err := checkNoHostDevices(rawConfig(map[string]any{"hostpci2": "0000:02:00.0"}), subject)
		if err == nil {
			t.Fatal("no refusal")
		}
		if !strings.Contains(err.Error(), subject) || !strings.Contains(err.Error(), "hostpci2") {
			t.Fatalf("refusal %q names neither %q nor the device", err, subject)
		}
	}
}

// A setting this client cannot read is a setting it cannot judge.
func TestAnUnreadableDeviceSettingIsAProtocolFailure(t *testing.T) {
	for _, key := range []string{"usb0", "serial0", "args"} {
		t.Run(key, func(t *testing.T) {
			if err := checkNoHostDevices(rawConfig(map[string]any{key: 17}), "reservation"); !errors.Is(err, ErrProtocol) {
				t.Fatalf("unreadable %s setting: %v", key, err)
			}
		})
	}
}

// Observation is where a device added after the clone shows up: pool, marker,
// name, digest, bridge and storage all still agree on that guest.
func TestInspectionRefusesAReservationCarryingAHostDevice(t *testing.T) {
	for _, config := range []map[string]any{
		{"hostpci0": "0000:01:00.0,pcie=1"},
		{"usb0": "host=046d:c52b"},
		{"serial0": "/dev/ttyS0"},
		{"args": "-netdev tap,id=escape"},
	} {
		f := newFake()
		f.exists = true
		f.configOverride = config
		client, _ := testClient(t, f)
		if _, err := client.Get(context.Background(), testIdentity); !errors.Is(err, ErrConflict) {
			t.Fatalf("reservation carrying %v accepted: %v", config, err)
		}
	}
}

// A full clone inherits the template's devices, so a template carrying one is
// refused before any clone request is issued, exactly as the bridge rule does.
func TestCloneRefusesATemplateCarryingAHostDeviceWithoutMutating(t *testing.T) {
	for _, config := range []map[string]any{
		{"hostpci0": "0000:01:00.0"},
		{"usb0": "host=1-2"},
		{"parallel0": "/dev/parport0"},
		{"args": "-device vfio-pci,host=01:00.0"},
	} {
		f := newFake()
		f.templateConfig = config
		client, _ := testClient(t, f)
		_, err := client.Clone(context.Background(), Action{ID: testCreation}, CloneSpec{Target: testIdentity, TemplateVMID: 9001, TemplateName: "ra8-lab-template", TemplateDigest: testDigest})
		if !errors.Is(err, ErrConflict) {
			t.Fatalf("template carrying %v accepted: %v", config, err)
		}
		f.mu.Lock()
		for _, req := range f.requests {
			if strings.HasPrefix(req, "POST ") {
				f.mu.Unlock()
				t.Fatalf("clone began from a template carrying %v: %s", config, req)
			}
		}
		f.mu.Unlock()
	}
}

// The setting is read out of the line, never guessed from it.
func TestFirstSettingReadsTheLeadingField(t *testing.T) {
	for _, tc := range []struct {
		value string
		want  string
	}{
		{value: "spice", want: "spice"},
		{value: "spice,usb3=1", want: "spice"},
		{value: "socket", want: "socket"},
		{value: "host=046d:c52b", want: "host=046d:c52b"},
		{value: "/dev/ttyS0", want: "/dev/ttyS0"},
		{value: "", want: ""},
		{value: ",spice", want: ""},
		{value: "spiced", want: "spiced"},
	} {
		if got := firstSetting(tc.value); got != tc.want {
			t.Fatalf("firstSetting(%q) = %q, want %q", tc.value, got, tc.want)
		}
	}
}
