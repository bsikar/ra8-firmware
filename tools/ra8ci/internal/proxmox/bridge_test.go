// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package proxmox

import (
	"context"
	"errors"
	"strings"
	"testing"
)

// A disposable guest is only disposable while it stays on a reviewed guest
// bridge. Pool, storage, and marker all agree on a guest that has quietly been
// given a second interface on the management network, so the bridge is checked
// on its own, on every observation.
func TestInspectionRejectsUnreviewedGuestBridge(t *testing.T) {
	for _, tc := range []struct {
		name   string
		config map[string]any
		want   error
	}{
		{name: "management bridge", config: map[string]any{"net0": "virtio=AA:BB:CC:DD:EE:01,bridge=vmbr0,firewall=1"}, want: ErrConflict},
		{name: "second interface off the allowlist", config: map[string]any{"net1": "virtio=AA:BB:CC:DD:EE:02,bridge=vmbr0"}, want: ErrConflict},
		{name: "no bridge setting", config: map[string]any{"net0": "virtio=AA:BB:CC:DD:EE:01,firewall=1"}, want: ErrConflict},
		{name: "empty bridge setting", config: map[string]any{"net0": "virtio=AA:BB:CC:DD:EE:01,bridge="}, want: ErrConflict},
		{name: "bridge named as a prefix of a reviewed one", config: map[string]any{"net0": "virtio=AA:BB:CC:DD:EE:01,bridge=vmbr80"}, want: ErrConflict},
		{name: "interface syntax", config: map[string]any{"net0": 123}, want: ErrProtocol},
	} {
		t.Run(tc.name, func(t *testing.T) {
			f := newFake()
			f.exists = true
			f.configOverride = tc.config
			client, _ := testClient(t, f)
			if _, err := client.Get(context.Background(), testIdentity); !errors.Is(err, tc.want) {
				t.Fatalf("Get rejected with %v, want %v", err, tc.want)
			}
		})
	}
}

// A guest carrying no interface at all is refused rather than read as harmless:
// the check reports what it verified, and it verified nothing.
func TestInspectionRejectsGuestWithNoInterface(t *testing.T) {
	f := newFake()
	f.exists = true
	f.configOverride = map[string]any{"net0": nil}
	client, _ := testClient(t, f)
	if _, err := client.Get(context.Background(), testIdentity); !errors.Is(err, ErrConflict) {
		t.Fatalf("guest with no interface accepted: %v", err)
	}
}

// Every reviewed bridge is accepted, not just the first one configured.
func TestInspectionAcceptsEveryReviewedBridge(t *testing.T) {
	for _, value := range []string{
		"virtio=AA:BB:CC:DD:EE:01,bridge=vmbr9,firewall=1",
		"bridge=vmbr8",
		"virtio=AA:BB:CC:DD:EE:01,bridge=vmbr8,tag=42,firewall=1",
	} {
		t.Run(value, func(t *testing.T) {
			f := newFake()
			f.exists = true
			f.configOverride = map[string]any{"net0": value}
			client, _ := testClient(t, f)
			if _, err := client.Get(context.Background(), testIdentity); err != nil {
				t.Fatalf("reviewed bridge refused: %v", err)
			}
		})
	}
}

// A full clone inherits the template's interfaces, so the template is the
// boundary that matters: a template re-pointed at the management bridge must
// be refused before any clone request is issued, not observed afterwards.
func TestCloneRefusesTemplateOnUnreviewedBridgeWithoutMutating(t *testing.T) {
	for _, tc := range []struct {
		name   string
		config map[string]any
	}{
		{name: "management bridge", config: map[string]any{"net0": "virtio=AA:BB:CC:DD:EE:00,bridge=vmbr0"}},
		{name: "extra interface", config: map[string]any{"net1": "virtio=AA:BB:CC:DD:EE:03,bridge=vmbr0"}},
		{name: "no interface", config: map[string]any{"net0": nil}},
	} {
		t.Run(tc.name, func(t *testing.T) {
			f := newFake()
			f.templateConfig = tc.config
			client, _ := testClient(t, f)
			_, err := client.Clone(context.Background(), Action{ID: testCreation}, CloneSpec{Target: testIdentity, TemplateVMID: 9001, TemplateName: "ra8-lab-template", TemplateDigest: testDigest})
			if !errors.Is(err, ErrConflict) {
				t.Fatalf("template on an unreviewed bridge accepted: %v", err)
			}
			f.mu.Lock()
			defer f.mu.Unlock()
			for _, req := range f.requests {
				if strings.HasPrefix(req, "POST ") {
					t.Fatalf("clone began from an unreviewed template: %s", req)
				}
			}
		})
	}
}

// The bridge is read out of the interface line, never guessed from it.
func TestBridgeOfReadsTheSetting(t *testing.T) {
	for _, tc := range []struct {
		value string
		want  string
		ok    bool
	}{
		{value: "virtio=AA:BB:CC:DD:EE:01,bridge=vmbr8,firewall=1", want: "vmbr8", ok: true},
		{value: "bridge=vmbr9", want: "vmbr9", ok: true},
		{value: "virtio=AA:BB:CC:DD:EE:01,firewall=1", want: "", ok: false},
		{value: "virtio=AA:BB:CC:DD:EE:01,bridge=", want: "", ok: false},
		{value: "", want: "", ok: false},
		{value: "bridged=vmbr8", want: "", ok: false},
	} {
		t.Run(tc.value, func(t *testing.T) {
			got, ok := bridgeOf(tc.value)
			if got != tc.want || ok != tc.ok {
				t.Fatalf("bridgeOf(%q) = %q, %v; want %q, %v", tc.value, got, ok, tc.want, tc.ok)
			}
		})
	}
}
