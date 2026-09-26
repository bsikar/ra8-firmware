// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package neutral

import (
	"path/filepath"
	"strings"
)

// deletedDescriptorSuffix is what Linux appends to a /proc/<pid>/fd link whose
// target was unlinked while the descriptor is still open.
const deletedDescriptorSuffix = " (deleted)"

// descriptorCouldBeProtectedDevice reports whether a descriptor target the
// inspector could not resolve still names a path under the protected device
// root, which makes it a descriptor this pass cannot clear.
//
// CheckIdle is documented to fail closed on any live process it cannot
// inspect, and every other arm of it does: an unreadable comm, an oversized
// cmdline, an unreadable fd directory or fd link all end the pass. The
// descriptor comparison did the opposite. It resolved each target and
// compared it only when resolution SUCCEEDED, so a descriptor the inspector
// could not resolve was read as "not one of the protected devices" and the
// fixture was reported quiescent.
//
// That is the case the rule exists for. A programmer holding the fixture
// across a udev rename, or across the unlink that makes its link read
// "/dev/ttyUSB0 (deleted)", still owns the board, and the open descriptor is
// the only evidence left that it does. Resolution fails for both, and what
// followed was not a refusal but a neutral receipt signed over a board with a
// tool still attached to it.
//
// Failing closed on EVERY unresolvable descriptor is not the answer. Most
// descriptors on a live host do not name paths at all (socket:[...],
// pipe:[...], anon_inode:[...]) and resolution fails for all of them, so a
// blanket rule would refuse every pass on every machine. The rule is bounded
// twice instead: the target must lexically name a path under the device root,
// and the pass must be protecting a device in the first place.
func descriptorCouldBeProtectedDevice(devRoot, target string, protecting bool) bool {
	if !protecting || devRoot == "" {
		return false
	}
	path := strings.TrimSuffix(target, deletedDescriptorSuffix)
	if path == "" || !filepath.IsAbs(path) {
		return false
	}
	return withinRoot(devRoot, filepath.Clean(path))
}
