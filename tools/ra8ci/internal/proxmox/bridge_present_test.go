// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package proxmox

import (
	"context"
	"errors"
	"net/http"
	"testing"
)

func TestBridgePresentReadsThePinnedNodeAndRequiresAnActiveBridge(t *testing.T) {
	for _, test := range []struct {
		name string
		data string
		want bool
	}{
		{"active reviewed bridge", `[{"iface":"vmbr9","type":"bridge","active":1}]`, true},
		{"configured but inactive bridge", `[{"iface":"vmbr9","type":"bridge"}]`, false},
		{"same name but not a bridge", `[{"iface":"vmbr9","type":"eth","active":1}]`, false},
		{"different interface", `[{"iface":"vmbr0","type":"bridge","active":1}]`, false},
	} {
		t.Run(test.name, func(t *testing.T) {
			client := clientAnswering(t, func(w http.ResponseWriter, r *http.Request) {
				if r.Method != http.MethodGet || r.URL.Path != "/api2/json/nodes/pve/network" {
					t.Errorf("bridge query = %s %s", r.Method, r.URL.Path)
					http.NotFound(w, r)
					return
				}
				w.Header().Set("Content-Type", jsonType)
				_, _ = w.Write([]byte(`{"data":` + test.data + `}`))
			})
			got, err := client.BridgePresent(context.Background(), "vmbr9")
			if err != nil || got != test.want {
				t.Fatalf("BridgePresent(vmbr9) = %t, %v; want %t, nil", got, err, test.want)
			}
		})
	}
}

func TestBridgePresentRefusesAnUnreviewedNameBeforeRequest(t *testing.T) {
	called := false
	client := clientAnswering(t, func(w http.ResponseWriter, r *http.Request) {
		called = true
		w.Header().Set("Content-Type", jsonType)
		_, _ = w.Write([]byte(`{"data":[]}`))
	})
	if _, err := client.BridgePresent(context.Background(), "vmbr0"); !errors.Is(err, ErrInvalid) {
		t.Fatalf("unreviewed bridge result = %v, want ErrInvalid", err)
	}
	if called {
		t.Fatal("unreviewed bridge name reached the Proxmox API")
	}
}
