// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package nscveneers

import (
	"os"
	"path/filepath"
)

// headerDir is the public include directory of the NSC library. Everything in
// it is reachable from a non-secure translation unit, so everything in it can
// declare an entry point into the Secure world.
const headerDir = "libs/ra8_nsc/inc"

// publicHeaders lists every header a non-secure caller can include from the NSC
// library.
//
// The gate used to read one file, ra8_nsc.h, because that was the whole public
// surface when it was written. It is not any more: the boundary is published
// across several headers now, and a veneer declared in any of them is the same
// promise to the non-secure side as one declared in ra8_nsc.h. Reading only the
// first meant most of the declared surface was never checked at all, which is
// the wrong way round for a gate whose whole point is that a phantom NS->S
// entry point in a public header is a trust hazard.
//
// Scope is the directory, not a spelled-out list, so a header added tomorrow is
// covered the day it lands rather than the day someone remembers this file.
// Paths come back relative to the repository root, in the order os.ReadDir
// gives them, which is sorted by filename.
func publicHeaders(root string) ([]string, error) {
	entries, err := os.ReadDir(filepath.Join(root, filepath.FromSlash(headerDir)))
	if err != nil {
		return nil, err
	}
	var headers []string
	for _, entry := range entries {
		if entry.IsDir() || filepath.Ext(entry.Name()) != ".h" {
			continue
		}
		headers = append(headers, headerDir+"/"+entry.Name())
	}
	return headers, nil
}
