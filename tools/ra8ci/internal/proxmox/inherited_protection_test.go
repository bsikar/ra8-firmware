// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package proxmox

import (
	"context"
	"strings"
	"testing"
)

func inheritedProtectionIdentity() Identity {
	identity := testIdentity
	identity.VMID = 9020
	identity.RunID = "0000000000000037"
	return identity
}

func TestClearInheritedProtectionDeletesOnlyProtectionAndVerifies(t *testing.T) {
	f := newFake()
	f.exists = true
	f.vmid = 9020
	f.protected = true
	f.marker = inheritedProtectionIdentity().marker()
	client, _ := testClient(t, f)

	if err := client.ClearInheritedProtection(context.Background(), inheritedProtectionIdentity()); err != nil {
		t.Fatalf("clear inherited protection: %v", err)
	}
	f.mu.Lock()
	defer f.mu.Unlock()
	if f.protected || f.form["delete"] != "protection" {
		t.Fatalf("update did not delete only protection: protected=%t form=%v", f.protected, f.form)
	}
}

func TestClearInheritedProtectionRefusesUnverifiedGuestsBeforeMutation(t *testing.T) {
	for _, test := range []struct {
		name   string
		change func(*fakePVE, *Identity)
		want   string
	}{
		{"marker mismatch", func(f *fakePVE, _ *Identity) { f.marker = "other" }, "marker"},
		{"template", func(f *fakePVE, _ *Identity) { f.configOverride = map[string]any{"template": 1} }, "template"},
		{"out of range VMID", func(_ *fakePVE, id *Identity) { id.VMID = 9040 }, "VMID"},
		{"wrong identity pool", func(_ *fakePVE, id *Identity) { id.Pool = "other" }, "pool"},
		{"wrong pool", func(f *fakePVE, _ *Identity) { f.pool = "other" }, "identity"},
		{"running", func(f *fakePVE, _ *Identity) { f.status = "running" }, "stopped"},
		{"locked", func(f *fakePVE, _ *Identity) { f.lock = "backup" }, "lock"},
	} {
		t.Run(test.name, func(t *testing.T) {
			f := newFake()
			f.exists = true
			f.vmid = 9020
			f.protected = true
			identity := inheritedProtectionIdentity()
			f.marker = identity.marker()
			test.change(f, &identity)
			client, _ := testClient(t, f)

			err := client.ClearInheritedProtection(context.Background(), identity)
			if err == nil || !strings.Contains(err.Error(), test.want) {
				t.Fatalf("error = %v, want failed %s check", err, test.want)
			}
			f.mu.Lock()
			defer f.mu.Unlock()
			if !f.protected || f.form != nil {
				t.Fatalf("refusal mutated the guest: protected=%t form=%v", f.protected, f.form)
			}
		})
	}
}
