// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package mtls

import (
	"fmt"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/privatefile"
)

// checkPrivateKeyFileAccess refuses a key whose underlying file is not
// restricted to its owner on the current platform. Following symlinks checks
// the same file that TLS will read.
func checkPrivateKeyFileAccess(keyFile string) error {
	if err := privatefile.Check(keyFile); err != nil {
		return fmt.Errorf("%w: private key file %s is not owner-only: %v", ErrIdentity, keyFile, err)
	}
	return nil
}
