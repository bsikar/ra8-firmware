// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package executor

import (
	"context"
	"io"
	"os"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/asciigate"
	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/assertcasts"
	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/committerms"
	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/driverasmguard"
	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/gnuattribute"
	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/gotosetjmp"
	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/legacymake"
	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/newlinegate"
	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/nscveneers"
	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/nullgate"
	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/pointerboilerplate"
	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/runnerclock"
	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/sincegate"
	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/stubcryptoguard"
	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/testsreadme"
	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/tzdiscard"
	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/unsafeinstall"
	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/waverefs"
)

// gateCall is the one shape every in-process gate is called in. The gates do
// not agree on a signature (runner-clock reads no checkout, and the commit
// gate reads stdin), so the entries that differ adapt here rather than making
// runStep know which is which.
type gateCall func(ctx context.Context, root string, args []string, stdout, stderr io.Writer) int

// gateDispatch is the executor's half of the ra8ci: dispatch seam, named
// once. It used to be named twice in runStep: an eighteen-clause condition
// deciding whether a step was a gate at all, and then an eighteen-arm ladder
// deciding which gate to call. Either list could grow a name the other
// lacked, which is the drift catalog/tool_programs.go describes from the far
// side and reviewed_tools_test.go pins.
//
// Adding a gate is still two edits, and that is deliberate: implement it
// here, and name it in catalog.reviewedToolPrograms so a task may be reviewed
// against it. The pin in reviewed_tools_test.go holds the two together.
var gateDispatch = map[string]gateCall{
	"ra8ci:ascii":            asciigate.Run,
	"ra8ci:assert-casts":     assertcasts.Run,
	"ra8ci:driver-asm-guard": driverasmguard.Run,
	"ra8ci:final-newline":    newlinegate.Run,
	"ra8ci:gnu-attribute":    gnuattribute.Run,
	"ra8ci:inclusive-terminology-commits": func(ctx context.Context, _ string, args []string, stdout, stderr io.Writer) int {
		return committerms.Run(ctx, args, os.Stdin, stdout, stderr)
	},
	"ra8ci:legacy-make":              legacymake.Run,
	"ra8ci:no-goto-setjmp":           gotosetjmp.Run,
	"ra8ci:no-null":                  nullgate.Run,
	"ra8ci:no-unsafe-python-install": unsafeinstall.Run,
	"ra8ci:nsc-veneer-defs":          nscveneers.Run,
	"ra8ci:pointer-boilerplate":      pointerboilerplate.Run,
	"ra8ci:runner-clock": func(ctx context.Context, _ string, args []string, stdout, stderr io.Writer) int {
		return runnerclock.Run(ctx, args, stdout, stderr)
	},
	"ra8ci:since":               sincegate.Run,
	"ra8ci:stub-crypto-guard":   stubcryptoguard.Run,
	"ra8ci:tests-readme":        testsreadme.Run,
	"ra8ci:tz-boundary-discard": tzdiscard.Run,
	"ra8ci:wave-references":     waverefs.Run,
}

// dispatchedGate answers the gate a step names, if this executor implements
// one. A step naming anything else is an external program.
func dispatchedGate(program string) (gateCall, bool) {
	gate, found := gateDispatch[program]
	return gate, found
}
