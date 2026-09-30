// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package catalog

import (
	"os"
	"strings"
	"testing"
)

// A file can pass every check the review makes about it and still refuse to
// be read. The name resolves, it is a regular file, it reports a size under
// the bound, and the open succeeds; the read is where it fails. That is a
// distinct outcome from a name that does not resolve and from a file that is
// too large, and the review has to hand the read's own error back rather
// than treat the empty result as an empty manifest: a checkout whose
// manifest could not be read must not review as a checkout with no tasks.
func TestAFileThatOpensAndWillNotReadIsRefused(t *testing.T) {
	// This process's own memory is a regular file of size zero whose read
	// faults, which is the only honest way to fail a read at this layer
	// without staging a fault the production code cannot meet.
	const faulting = "/proc/self/mem"
	info, err := os.Lstat(faulting)
	if err != nil || !info.Mode().IsRegular() || info.Size() != 0 {
		t.Skip("this system has no zero-sized regular file that refuses to be read")
	}
	handle, err := os.Open(faulting)
	if err != nil {
		t.Skip("this system does not allow the faulting file to be opened")
	}
	_, readErr := handle.Read(make([]byte, 1))
	handle.Close()
	if readErr == nil {
		t.Skip("the faulting file read cleanly here")
	}

	raw, err := readCheckoutFile(faulting, maxReadableManifestBytes)
	if err == nil {
		t.Fatalf("read %d bytes from a file whose read faults, want the read's own error", len(raw))
	}
	if raw != nil {
		t.Fatal("bytes were handed back beside the refusal")
	}
	// The earlier gates each have their own wording, and none of them is
	// what happened here. Reaching for one of those would send an operator
	// to look at the wrong thing.
	for _, wrong := range []string{"is not a regular file", "more than the", "grew past"} {
		if strings.Contains(err.Error(), wrong) {
			t.Fatalf("a failed read was reported as %q: %v", wrong, err)
		}
	}
}
