// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package proxmox

import (
	"encoding/json"
	"fmt"
	"regexp"
)

// A guest carries volumes on keys checkDisks never reads.
//
// checkDisks judges scsiN, virtioN and sataN, and TestOnlyDiskKeysAreJudged
// pins that it judges nothing else. Those are not the only keys that name a
// volume. ideN is a disk bus in its own right and is where Proxmox puts an
// installer ISO and a cloud-init drive; efidiskN holds the guest's NVRAM,
// tpmstateN its TPM state, and unusedN a volume that was detached from the
// guest and is still owned by it. Every one of them is written
// "storage:volume" exactly as a disk is, and every one of them could name a
// storage this operator never reviewed.
//
// What that costs is not a tidiness point. The approved storage is where a
// disposable guest's bytes are allowed to live, and the checks around it are
// what make a reservation a clone of a reviewed template rather than a machine
// of unknown provenance. An ide0 on another storage is a second bootable disk
// this client cannot see, and boot order is a config line away from preferring
// it; an ide2 pointing at an ISO on a shared store hands the guest whatever
// media sits there, which is the same escape one bus over. Neither is refused
// by pool, marker, name, digest, bridge or by checkDisks, and the guest still
// presents one impeccable scsi0 on the approved storage while it does it.
//
// The source template is judged too, and for a reason specific to cloning:
// Clone passes storage=, which relocates the volumes the template OWNS, but a
// cdrom reference is copied through verbatim. A reviewed template with an ISO
// from an unreviewed store therefore produces a reservation carrying that same
// reference, and refusing it before the clone is cheaper than discovering it
// on the next inspection.
//
// What is accepted is what carries nothing: a drive referencing "none" or
// nothing at all, the empty tray checkDisks already waves through. Nothing
// here requires such a key to be present, because checkDisks already requires
// the guest to have a disk of its own on the approved storage; this rule only
// says that anything ELSE it carries sits there too.
var attachedVolumeKeyPattern = regexp.MustCompile(`^(ide|efidisk|tpmstate|unused)[0-9]+$`)

// checkAttachedVolumes holds every volume-bearing key outside the disk buses
// to the approved storage. subject names whose configuration it is, so a
// refusal says whether the volume is on the reservation or on the template it
// would be cloned from.
func checkAttachedVolumes(config map[string]json.RawMessage, storage, subject string) error {
	for key, raw := range config {
		if !attachedVolumeKeyPattern.MatchString(key) {
			continue
		}
		var value string
		if err := json.Unmarshal(raw, &value); err != nil {
			return ErrProtocol
		}
		volumeStorage, references := diskVolumeStorage(value)
		if !references {
			continue
		}
		if volumeStorage != storage {
			return fmt.Errorf("%w: %s volume %s is on unreviewed storage %q", ErrConflict, subject, key, volumeStorage)
		}
	}
	return nil
}
