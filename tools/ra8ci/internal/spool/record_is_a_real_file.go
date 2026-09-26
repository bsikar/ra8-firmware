// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package spool

import (
	"errors"
	"fmt"
	"os"
)

// The spool is a private outbox and says so at every door it has opened so
// far: Open refuses a path that is not a real directory or that other users
// can reach, and readStarted refuses a start record that is not a regular
// file rather than following it. Pending reads the two remaining names under
// that same directory, the terminal record it hands to the upload and the
// receipt it trusts to skip one, and followed both.
//
// A record this package wrote is always a regular file, because write() is
// the only thing that creates one: it writes a private temporary file and
// hard-links it into place. Nothing here produces a symlink, so a name under
// the spool that is not a regular file was not written by this spool, and
// reading through it puts a file from outside the private directory into
// durable server history under a local run's identity.
//
// Neither check can close the window between looking and reading, exactly as
// readStarted's cannot. It refuses the name that is a link when it is looked
// at, which is what the package's own privacy rule is worth.

// readRegularFile returns the contents of path, refusing anything that is not
// a regular file without following it.
func readRegularFile(path string) ([]byte, error) {
	info, err := os.Lstat(path)
	if err != nil {
		return nil, err
	}
	if !info.Mode().IsRegular() {
		return nil, fmt.Errorf("spool record %q is not a regular file", path)
	}
	return os.ReadFile(path)
}

// syncReceiptPresent reports whether the acknowledgement at path is on disk.
//
// Absent means the record still has to be uploaded, present means it has been
// acknowledged and can be retired. A name that exists without being a regular
// file is neither, and guessing either way is wrong in a different direction:
// read as absent it uploads a record the server may already hold, read as
// present it drops one the server does not. So it is refused and the pass
// says which name it was.
func syncReceiptPresent(path string) (bool, error) {
	info, err := os.Lstat(path)
	if errors.Is(err, os.ErrNotExist) {
		return false, nil
	}
	if err != nil {
		return false, err
	}
	if !info.Mode().IsRegular() {
		return false, fmt.Errorf("sync receipt %q is not a regular file", path)
	}
	return true, nil
}
