// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package scaler

import (
	"strings"
	"testing"
)

// The VMID allowlist is the disposable pool itself, so a VMID that does not
// belong in it is a configuration mistake the handler must not start with.
// A duplicate is already held elsewhere; these are the two ways an entry can
// be the wrong guest rather than a repeated one.
func TestAVMIDOutsideTheDisposablePoolIsRefusedAtConstruction(t *testing.T) {
	h, _, _, _, _ := testHarness(t)
	base := h.config
	if base.TemplateVMID < 9000 {
		t.Fatalf("harness template VMID = %d, want one inside the pool range", base.TemplateVMID)
	}
	for name, allowlist := range map[string][]int{
		"below the pool":      {8999},
		"far below the pool":  {0},
		"negative":            {-9000},
		"the template itself": {base.TemplateVMID},
		"good entry first":    {9000, base.TemplateVMID},
		"good entry last":     {8999, 9000},
	} {
		t.Run(name, func(t *testing.T) {
			cfg := base
			cfg.VMIDs = allowlist
			_, err := NewHandler(cfg, h.ledger, h.vms, h.metadata, h.bootstrap, h.runners, h.backup, h.admission)
			if err == nil {
				t.Fatal("the handler started with a VMID outside the disposable pool")
			}
			if !strings.Contains(err.Error(), "outside disposable reservation pool") {
				t.Fatalf("error = %v, want the refusal naming the pool", err)
			}
		})
	}
}

// The floor is exact, and the template's neighbours are ordinary members of
// the pool, so the refusal above is about those two entries and not about
// the handler being shy of VMIDs near the template.
func TestThePoolAdmitsItsFloorAndTheTemplatesNeighbours(t *testing.T) {
	h, _, _, _, _ := testHarness(t)
	base := h.config
	for name, allowlist := range map[string][]int{
		"the floor itself":  {9000},
		"either side of it": {base.TemplateVMID - 1, base.TemplateVMID + 1},
		"a wide pool":       {9000, 9002, 9003, 9099},
	} {
		t.Run(name, func(t *testing.T) {
			cfg := base
			cfg.VMIDs = allowlist
			if _, err := NewHandler(cfg, h.ledger, h.vms, h.metadata, h.bootstrap, h.runners, h.backup, h.admission); err != nil {
				t.Fatalf("a valid allowlist was refused: %v", err)
			}
		})
	}
}
