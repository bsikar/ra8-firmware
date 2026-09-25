// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package proxmox

import (
	"encoding/json"
	"fmt"
	"strings"
)

// checkDisks requires every disk a guest carries to sit on the approved
// storage, not merely one of them: a second disk on an unreviewed storage is
// the same escape checkNetworks already refuses for a second interface on the
// management bridge, and the pool, marker, and name checks cannot see it.
//
// A drive that references no volume at all ("none", an empty CD tray) carries
// nothing and is accepted; a drive that names a volume must name one on the
// approved storage. At least one such volume is still required, so a guest
// with no disk of its own is refused exactly as before.
func checkDisks(config map[string]json.RawMessage, storage, subject string) error {
	found := false
	for key, raw := range config {
		if !diskKeyPattern.MatchString(key) {
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
			return fmt.Errorf("%w: %s disk %s is on unreviewed storage %q", ErrConflict, subject, key, volumeStorage)
		}
		found = true
	}
	if !found {
		return fmt.Errorf("%w: no disk on approved storage", ErrConflict)
	}
	return nil
}

// diskVolumeStorage reads the storage ID out of a Proxmox drive setting. The
// first comma-separated field is the volume reference, either "storage:volume"
// or the literal "none" for a drive that holds nothing. Anything else names a
// volume this client cannot place, so it is reported with an empty storage ID
// and refused by the caller rather than read as harmless.
func diskVolumeStorage(value string) (string, bool) {
	volume, _, _ := strings.Cut(value, ",")
	if volume == "none" || volume == "" {
		return "", false
	}
	storage, _, ok := strings.Cut(volume, ":")
	if !ok {
		return "", true
	}
	return storage, true
}
