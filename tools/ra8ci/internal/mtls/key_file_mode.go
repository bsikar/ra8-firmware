// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package mtls

import (
	"fmt"
	"io/fs"
	"os"
)

// checkPrivateKeyFileMode refuses a private key file every account on the host
// can read.
//
// Everything else in this package judges what a certificate SAYS. This judges
// the one file in the pair that must never be read by anyone but the process
// presenting it. tls.LoadX509KeyPair reads whatever mode the file carries, so a
// key written by a provisioning step with a default umask, copied off a build
// box, or restored from an archive is loaded and presented exactly like a key
// nobody else can read, and nothing on the host ever says otherwise.
//
// The failure it replaces is not a handshake error, which is what the rest of
// this package is about; it is silence. A readable key stays readable for as
// long as the runner lives, and the operator finds out when the identity is
// used from somewhere it was never installed, by which time the audit trail
// names a fingerprint this host believes is only its own.
//
// It reads the mode with os.Stat rather than os.Lstat on purpose: a key path is
// often a symlink into a secrets directory, and the bytes handed to the TLS
// stack come from the file at the end of the link, so that is the file whose
// permissions decide the question.
func checkPrivateKeyFileMode(keyFile string) error {
	info, err := os.Stat(keyFile)
	if err != nil {
		return fmt.Errorf("%w: read the private key file: %v", ErrIdentity, err)
	}
	if info.IsDir() {
		return fmt.Errorf("%w: private key path %s is a directory", ErrIdentity, keyFile)
	}
	// Only the world bits. A key readable by a group is a deliberate and
	// common arrangement: the file is owned by the account that provisions it
	// and read by the group the runner runs as. Refusing that would lock out
	// running deployments to say nothing they did not already decide. A key
	// readable by every account on the host was decided by nobody.
	if mode := info.Mode().Perm(); mode&0o007 != 0 {
		return fmt.Errorf("%w: private key file %s is mode %04o and readable by every account on this host",
			ErrIdentity, keyFile, mode)
	}
	return nil
}

// permissionsOf is the mode a caller would see, kept beside the rule so a test
// reads the same bits the rule does rather than restating the mask.
func permissionsOf(info fs.FileInfo) fs.FileMode {
	if info == nil {
		return 0
	}
	return info.Mode().Perm()
}
