// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package proxmox

import (
	"encoding/json"
	"errors"
	"strings"
	"testing"
)

const approvedStorage = "ra8-tf-lab"

func diskConfig(t *testing.T, drives map[string]any) map[string]json.RawMessage {
	t.Helper()
	config := make(map[string]json.RawMessage, len(drives))
	for key, value := range drives {
		raw, err := json.Marshal(value)
		if err != nil {
			t.Fatalf("marshal %s: %v", key, err)
		}
		config[key] = raw
	}
	return config
}

func TestASingleDiskOnTheApprovedStorageIsAccepted(t *testing.T) {
	config := diskConfig(t, map[string]any{
		"scsi0": approvedStorage + ":vm-9000-disk-0,size=32G",
		"name":  "ra8-lab-ci-9000",
	})
	if err := checkDisks(config, approvedStorage, "reservation"); err != nil {
		t.Fatalf("approved disk refused: %v", err)
	}
}

func TestEveryDiskMustSitOnTheApprovedStorage(t *testing.T) {
	config := diskConfig(t, map[string]any{
		"scsi0":   approvedStorage + ":vm-9000-disk-0,size=32G",
		"virtio1": "shared-nfs:vm-4001-disk-0,size=512G",
	})
	err := checkDisks(config, approvedStorage, "reservation")
	if !errors.Is(err, ErrConflict) {
		t.Fatalf("second disk on unreviewed storage accepted: %v", err)
	}
	if !strings.Contains(err.Error(), "virtio1") || !strings.Contains(err.Error(), "shared-nfs") {
		t.Fatalf("refusal does not name the drive and its storage: %v", err)
	}
}

func TestADiskOnAnotherStorageIsRefusedEvenWhenItIsTheOnlyOne(t *testing.T) {
	config := diskConfig(t, map[string]any{"scsi0": "other:vm-9000-disk-0"})
	if err := checkDisks(config, approvedStorage, "reservation"); !errors.Is(err, ErrConflict) {
		t.Fatalf("lone unapproved disk accepted: %v", err)
	}
}

func TestAGuestWithNoDiskOfItsOwnIsRefused(t *testing.T) {
	config := diskConfig(t, map[string]any{"net0": "virtio=AA:BB:CC:DD:EE:01,bridge=vmbr8"})
	err := checkDisks(config, approvedStorage, "reservation")
	if !errors.Is(err, ErrConflict) || !strings.Contains(err.Error(), "no disk on approved storage") {
		t.Fatalf("diskless guest accepted: %v", err)
	}
}

func TestAnEmptyDriveCarriesNothingAndDoesNotCountAsADisk(t *testing.T) {
	empty := diskConfig(t, map[string]any{"scsi1": "none,media=cdrom"})
	if err := checkDisks(empty, approvedStorage, "reservation"); !errors.Is(err, ErrConflict) {
		t.Fatalf("an empty tray alone satisfied the approved-storage rule: %v", err)
	}
	beside := diskConfig(t, map[string]any{
		"scsi0": approvedStorage + ":vm-9000-disk-0",
		"scsi1": "none,media=cdrom",
	})
	if err := checkDisks(beside, approvedStorage, "reservation"); err != nil {
		t.Fatalf("empty tray beside an approved disk refused: %v", err)
	}
}

func TestADriveThisClientCannotPlaceIsRefused(t *testing.T) {
	for _, value := range []string{"vm-9000-disk-0", "/dev/disk/by-id/wwn-0x5000", "local-lvm"} {
		config := diskConfig(t, map[string]any{
			"scsi0": approvedStorage + ":vm-9000-disk-0",
			"sata0": value,
		})
		if err := checkDisks(config, approvedStorage, "reservation"); !errors.Is(err, ErrConflict) {
			t.Fatalf("drive %q accepted: %v", value, err)
		}
	}
}

func TestANonStringDriveSettingIsAProtocolFault(t *testing.T) {
	config := diskConfig(t, map[string]any{"scsi0": 123})
	if err := checkDisks(config, approvedStorage, "reservation"); !errors.Is(err, ErrProtocol) {
		t.Fatalf("numeric drive setting accepted: %v", err)
	}
}

func TestOnlyDiskKeysAreJudged(t *testing.T) {
	config := diskConfig(t, map[string]any{
		"scsi0":     approvedStorage + ":vm-9000-disk-0",
		"ide2":      "local:iso/seed.iso,media=cdrom",
		"efidisk0":  "other:vm-9000-disk-1",
		"scsihw":    "virtio-scsi-single",
		"scsi0copy": "other:vm-9000-disk-2",
	})
	if err := checkDisks(config, approvedStorage, "reservation"); err != nil {
		t.Fatalf("a key outside the disk pattern was judged as a disk: %v", err)
	}
}

func TestTheSubjectNamesWhoseDiskWasRefused(t *testing.T) {
	config := diskConfig(t, map[string]any{"scsi0": "other:vm-9000-disk-0"})
	err := checkDisks(config, approvedStorage, "source template")
	if err == nil || !strings.Contains(err.Error(), "source template") {
		t.Fatalf("refusal does not name its subject: %v", err)
	}
}

func TestDiskVolumeStorageReadsTheFirstField(t *testing.T) {
	for _, testCase := range []struct {
		value      string
		storage    string
		references bool
	}{
		{value: "ra8-tf-lab:vm-9000-disk-0,size=32G,ssd=1", storage: "ra8-tf-lab", references: true},
		{value: "ra8-tf-lab:vm-9000-disk-0", storage: "ra8-tf-lab", references: true},
		{value: "none,media=cdrom", references: false},
		{value: "", references: false},
		{value: "bare-volume,size=1G", storage: "", references: true},
	} {
		storage, references := diskVolumeStorage(testCase.value)
		if storage != testCase.storage || references != testCase.references {
			t.Fatalf("%q read as (%q, %v), want (%q, %v)", testCase.value, storage, references, testCase.storage, testCase.references)
		}
	}
}
