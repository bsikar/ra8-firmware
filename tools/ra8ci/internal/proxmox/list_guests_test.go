// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package proxmox

import (
	"context"
	"errors"
	"testing"
)

// listWith serves the reservation with the given cluster-record fields edited.
func listWith(t *testing.T, override map[string]any) ([]VM, error) {
	t.Helper()
	f := newFake()
	f.exists = true
	f.resourceOverride = override
	client, _ := testClient(t, f)
	return client.List(context.Background())
}

func TestListReportsTheReservedGuest(t *testing.T) {
	listed, err := listWith(t, nil)
	if err != nil || len(listed) != 1 || listed[0].Identity.VMID != testIdentity.VMID {
		t.Fatalf("reserved guest not listed: %+v, %v", listed, err)
	}
	if listed[0].Identity.Node != testIdentity.Node || listed[0].Status != "stopped" {
		t.Fatalf("listed guest carries the wrong identity: %+v", listed[0])
	}
}

func TestListRefusesAnAllowedIDOnAnotherNode(t *testing.T) {
	listed, err := listWith(t, map[string]any{"node": "pve2"})
	if !errors.Is(err, ErrConflict) {
		t.Fatalf("guest on an unconfigured node listed: %+v, %v", listed, err)
	}
	if listed != nil {
		t.Fatalf("refused list still reported guests: %+v", listed)
	}
}

func TestListRefusesAnAllowedIDThatIsATemplate(t *testing.T) {
	for _, flag := range []any{true, 1, "1"} {
		listed, err := listWith(t, map[string]any{"template": flag})
		if !errors.Is(err, ErrConflict) {
			t.Fatalf("template listed as a disposable guest (template=%v): %+v, %v", flag, listed, err)
		}
	}
}

func TestListRefusesAnUnreadableTemplateFlag(t *testing.T) {
	if _, err := listWith(t, map[string]any{"template": "maybe"}); !errors.Is(err, ErrProtocol) {
		t.Fatalf("unreadable template flag accepted: %v", err)
	}
}

// The template the clone path reads is not an allowed VM ID, so the refusals
// above must not swallow it: it is skipped by the allowlist, as before.
func TestListStillSkipsTheReviewedTemplateID(t *testing.T) {
	f := newFake()
	client, _ := testClient(t, f)
	listed, err := client.List(context.Background())
	if err != nil || len(listed) != 0 {
		t.Fatalf("reviewed template or foreign guest leaked into the list: %+v, %v", listed, err)
	}
}
