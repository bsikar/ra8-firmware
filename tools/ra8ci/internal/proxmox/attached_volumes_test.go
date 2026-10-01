// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package proxmox

import (
	"context"
	"errors"
	"strings"
	"testing"
)

func TestAGuestCarryingNothingBesidesItsDisksIsAccepted(t *testing.T) {
	config := diskConfig(t, map[string]any{
		"scsi0":  approvedStorage + ":vm-9000-disk-0,size=32G",
		"net0":   "virtio=AA:BB:CC:DD:EE:01,bridge=vmbr8",
		"scsihw": "virtio-scsi-single",
	})
	if err := checkAttachedVolumes(config, approvedStorage, "reservation"); err != nil {
		t.Fatalf("a guest with no volume outside the disk buses was refused: %v", err)
	}
}

func TestASecondBootableDiskOnAnotherStorageIsRefused(t *testing.T) {
	config := diskConfig(t, map[string]any{
		"scsi0": approvedStorage + ":vm-9000-disk-0,size=32G",
		"ide0":  "shared-nfs:vm-4001-disk-0,size=64G",
	})
	err := checkAttachedVolumes(config, approvedStorage, "reservation")
	if !errors.Is(err, ErrConflict) {
		t.Fatalf("ide disk on unreviewed storage accepted: %v", err)
	}
	if !strings.Contains(err.Error(), "ide0") || !strings.Contains(err.Error(), "shared-nfs") {
		t.Fatalf("refusal does not name the volume and its storage: %v", err)
	}
}

func TestMediaFromAnUnreviewedStoreIsRefused(t *testing.T) {
	config := diskConfig(t, map[string]any{
		"scsi0": approvedStorage + ":vm-9000-disk-0",
		"ide2":  "local:iso/whatever.iso,media=cdrom",
	})
	if err := checkAttachedVolumes(config, approvedStorage, "reservation"); !errors.Is(err, ErrConflict) {
		t.Fatalf("ISO from an unreviewed store accepted: %v", err)
	}
}

func TestAnEmptyTrayCarriesNothingAndIsAccepted(t *testing.T) {
	for _, value := range []string{"none,media=cdrom", "none", ""} {
		config := diskConfig(t, map[string]any{"ide2": value})
		if err := checkAttachedVolumes(config, approvedStorage, "reservation"); err != nil {
			t.Fatalf("empty tray %q refused: %v", value, err)
		}
	}
}

func TestEveryVolumeBearingKeyOutsideTheDiskBusesIsJudged(t *testing.T) {
	for _, key := range []string{"ide0", "ide2", "efidisk0", "tpmstate0", "unused0", "unused7"} {
		config := diskConfig(t, map[string]any{
			"scsi0": approvedStorage + ":vm-9000-disk-0",
			key:     "other:vm-9000-disk-9",
		})
		err := checkAttachedVolumes(config, approvedStorage, "reservation")
		if !errors.Is(err, ErrConflict) {
			t.Fatalf("%s on unreviewed storage accepted: %v", key, err)
		}
		if !strings.Contains(err.Error(), key) {
			t.Fatalf("refusal of %s does not name it: %v", key, err)
		}
	}
}

func TestThoseSameKeysOnTheApprovedStorageAreAccepted(t *testing.T) {
	config := diskConfig(t, map[string]any{
		"scsi0":     approvedStorage + ":vm-9000-disk-0",
		"ide0":      approvedStorage + ":vm-9000-disk-1,size=8G",
		"efidisk0":  approvedStorage + ":vm-9000-disk-2",
		"tpmstate0": approvedStorage + ":vm-9000-disk-3,size=4M",
		"unused0":   approvedStorage + ":vm-9000-disk-4",
	})
	if err := checkAttachedVolumes(config, approvedStorage, "reservation"); err != nil {
		t.Fatalf("volumes on the approved storage refused: %v", err)
	}
}

func TestAKeyThatOnlyLooksLikeAVolumeIsLeftAlone(t *testing.T) {
	config := diskConfig(t, map[string]any{
		"scsi0":    approvedStorage + ":vm-9000-disk-0",
		"ide":      "other:vm-9000-disk-1",
		"ide0copy": "other:vm-9000-disk-2",
		"idea0":    "other:vm-9000-disk-3",
		"unusedx":  "other:vm-9000-disk-4",
		"boot":     "order=scsi0",
	})
	if err := checkAttachedVolumes(config, approvedStorage, "reservation"); err != nil {
		t.Fatalf("a key outside the pattern was judged as a volume: %v", err)
	}
}

func TestAVolumeSettingThisClientCannotReadIsAProtocolFault(t *testing.T) {
	config := diskConfig(t, map[string]any{"ide0": 123})
	if err := checkAttachedVolumes(config, approvedStorage, "reservation"); !errors.Is(err, ErrProtocol) {
		t.Fatalf("numeric volume setting accepted: %v", err)
	}
}

func TestAVolumeThatNamesNoStorageIsRefused(t *testing.T) {
	config := diskConfig(t, map[string]any{"efidisk0": "bare-volume,size=1M"})
	if err := checkAttachedVolumes(config, approvedStorage, "reservation"); !errors.Is(err, ErrConflict) {
		t.Fatalf("volume with no storage ID accepted: %v", err)
	}
}

func TestTheSubjectNamesWhoseVolumeWasRefused(t *testing.T) {
	config := diskConfig(t, map[string]any{"ide2": "local:iso/seed.iso,media=cdrom"})
	err := checkAttachedVolumes(config, approvedStorage, "source template")
	if err == nil || !strings.Contains(err.Error(), "source template") {
		t.Fatalf("refusal does not name its subject: %v", err)
	}
}

func TestAReservationCarryingAnUnreviewedVolumeIsRefusedOnInspection(t *testing.T) {
	f := newFake()
	f.exists = true
	f.configOverride = map[string]any{"ide0": "shared-nfs:vm-4001-disk-0,size=64G"}
	client, _ := testClient(t, f)
	if _, err := client.Get(context.Background(), testIdentity); !errors.Is(err, ErrConflict) {
		t.Fatalf("Get accepted a reservation carrying an unreviewed volume: %v", err)
	}
}

func TestATemplateCarryingUnreviewedMediaIsNotCloned(t *testing.T) {
	f := newFake()
	f.templateConfig = map[string]any{"ide2": "local:iso/seed.iso,media=cdrom"}
	client, _ := testClient(t, f)
	spec := CloneSpec{Target: testIdentity, TemplateVMID: 9001, TemplateName: "ra8-lab-template", TemplateDigest: testDigest}
	_, err := client.Clone(context.Background(), Action{ID: testCreation}, spec)
	if !errors.Is(err, ErrConflict) {
		t.Fatalf("clone of a template carrying unreviewed media accepted: %v", err)
	}
	if f.form != nil {
		t.Fatal("a clone request was sent for a template this client refuses")
	}
}
