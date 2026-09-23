// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package store

import (
	"strings"
	"testing"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/hilspec"
)

func TestValidHILWorkloadRequiresExactSafeIdentity(t *testing.T) {
	valid := hilspec.Workload{
		ManifestPath:    "examples/ek_ra8d2/hw_validated/hil/demo/hil.conf",
		BoardModel:      "EK-RA8D2",
		FixtureRevision: "fixture-v1",
		ProfileSHA256:   strings.Repeat("a", 64),
		ProgramFamily:   "uart-demo",
		Mode:            hilspec.ModeUARTScrape,
	}
	if !validHILWorkload(valid) {
		t.Fatal("valid HIL workload was rejected")
	}
	tests := []struct {
		name string
		edit func(*hilspec.Workload)
	}{
		{"manifest outside examples", func(w *hilspec.Workload) { w.ManifestPath = "docs/hil.conf" }},
		{"manifest traversal", func(w *hilspec.Workload) { w.ManifestPath = "examples/../docs/hil.conf" }},
		{"manifest wrong file", func(w *hilspec.Workload) { w.ManifestPath = "examples/demo/run.sh" }},
		{"manifest backslash", func(w *hilspec.Workload) { w.ManifestPath = "examples/demo\\hil.conf" }},
		{"blank board", func(w *hilspec.Workload) { w.BoardModel = " " }},
		{"blank fixture", func(w *hilspec.Workload) { w.FixtureRevision = "fixture-v1 " }},
		{"invalid profile digest", func(w *hilspec.Workload) { w.ProfileSHA256 = strings.Repeat("A", 64) }},
		{"blank program family", func(w *hilspec.Workload) { w.ProgramFamily = " " }},
		{"unsupported mode", func(w *hilspec.Workload) { w.Mode = "unknown" }},
	}
	for _, test := range tests {
		t.Run(test.name, func(t *testing.T) {
			workload := valid
			test.edit(&workload)
			if validHILWorkload(workload) {
				t.Fatalf("invalid workload accepted: %+v", workload)
			}
		})
	}
}
