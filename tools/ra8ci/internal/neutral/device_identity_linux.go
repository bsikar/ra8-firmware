// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package neutral

import (
	"os"
	"syscall"
)

// deviceNumber returns the device number behind a node, which is the identity
// the kernel gives the hardware itself rather than any name pointing at it.
func deviceNumber(info os.FileInfo) (uint64, bool) {
	if info == nil || info.Mode()&os.ModeDevice == 0 {
		return 0, false
	}
	raw, ok := info.Sys().(*syscall.Stat_t)
	if !ok || raw == nil {
		return 0, false
	}
	return uint64(raw.Rdev), true
}

// checkHeldDescriptorIsNotTheFixture judges a held descriptor against the
// protected fixture by the device it opens, not by the name it opens it under.
//
// CheckIdle resolved every protected device path and every descriptor target
// through filepath.EvalSymlinks and then compared the two as strings. That
// closes the symlink case, which is the ordinary one: udev's /dev/serial/by-id
// names all resolve onto the node the profile already lists. It does not close
// the case where a second NODE exists for the same hardware. A node is a
// (major, minor) pair written into a directory entry, and nothing makes that
// pair unique to one entry: mknod writes another, a container's own /dev
// carries its own set, and a udev rule can be written to create a real node
// rather than a link. Two entries, one device, two different resolved paths,
// and the string comparison clears the second one.
//
// What clears is a board with a programmer still attached to it. The pass then
// signs a neutral receipt, and the board is released or recovered on the
// strength of it, which is the single outcome this inspector exists to
// prevent. The rest of this package already judges a file by identity rather
// than by name (LoadProfile, LoadVerifierFile and confirmSignalFile all reach
// os.SameFile before reading a byte); the descriptor arm was the one place
// still trusting the path.
//
// The extra stat is bounded to descriptors that resolve under the device root,
// so the ordinary descriptors on a live host (sockets, pipes, anonymous
// inodes, and every regular file elsewhere) cost nothing. A descriptor that
// resolved a moment ago and cannot be stat'd now is a race on the fixture's
// own directory, and this inspector is documented to fail closed on anything
// it cannot inspect.
func checkHeldDescriptorIsNotTheFixture(devRoot, resolved string, fixtures map[uint64]bool) error {
	if len(fixtures) == 0 || devRoot == "" || resolved == "" || !withinRoot(devRoot, resolved) {
		return nil
	}
	info, err := os.Stat(resolved)
	if err != nil {
		return ErrObservationAbsent
	}
	number, ok := deviceNumber(info)
	if !ok {
		return nil
	}
	if fixtures[number] {
		return ErrHardwareBusy
	}
	return nil
}
