// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package catalog

import (
	"fmt"
	"io"
	"os"
)

// VerifyCheckout answers one question about a tree a guest hands it: does this
// checkout carry the exact reviewed catalog this binary was built from. Both
// halves of that answer were read with os.ReadFile, which asks nothing about
// the name it was given and nothing about how much it is about to read.
//
// Two costs follow, and both are paid BEFORE the digest comparison that is the
// whole point of the call. os.ReadFile follows a symlink, so a manifest name
// pointing anywhere on the host is read as if the checkout carried it; and it
// reads to EOF, so a multi-gigabyte tasks.json is pulled into memory, then
// walked twice more by CanonicalJSON (once by inspectJSONValue, once into an
// any) and a third time into the manifest struct, all to be refused a moment
// later for a digest that could never have matched.
//
// The bounds here are the ones this codebase already states for untrusted
// JSON, restated rather than invented: protocol.MaxJSONBytes and
// hilspec.maxManifestBytes are both 1 MiB, against an embedded manifest of
// about 56 KiB. The digest file holds 64 hex characters and a newline; 1 KiB
// is room for whitespace, not for content. The file-kind rule is the spool's
// (record_is_a_real_file.go): a name that was not written as a regular file
// was not written by the thing that is supposed to have written it.
//
// Deliberately NOT a defence against a hostile checkout: a tree that reaches
// VerifyCheckout has already been granted, and the digest is what decides
// whether its catalog is the reviewed one. This door only stops the reading
// itself from being the expensive part, and stops the read from wandering out
// of the tree by way of a link.
const (
	maxReadableManifestBytes = 1 << 20
	maxReadableDigestBytes   = 1 << 10
)

// readCheckoutFile reads one reviewed file out of a checkout, refusing a name
// that is not a regular file and content past what review can hold.
func readCheckoutFile(path string, limit int64) ([]byte, error) {
	info, err := os.Lstat(path)
	if err != nil {
		return nil, err
	}
	if !info.Mode().IsRegular() {
		return nil, fmt.Errorf("%s is not a regular file", path)
	}
	if info.Size() > limit {
		return nil, fmt.Errorf("%s is %d bytes, more than the %d review reads", path, info.Size(), limit)
	}
	file, err := os.Open(path)
	if err != nil {
		return nil, err
	}
	defer file.Close()
	raw, err := io.ReadAll(io.LimitReader(file, limit+1))
	if err != nil {
		return nil, err
	}
	if int64(len(raw)) > limit {
		return nil, fmt.Errorf("%s grew past the %d bytes review reads", path, limit)
	}
	return raw, nil
}
