// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package provision

import (
	"net/netip"
	"testing"
)

// Every reviewed bridge must carry a segment validRunnerIPv4 already accepts,
// so widening reviewedRunnerBridges to a bridge this lab has no network for
// fails here rather than in the lab.
func TestEveryReviewedBridgeCarriesAnAcceptedSegment(t *testing.T) {
	for _, bridge := range reviewedRunnerBridges {
		segment, reviewed := runnerBridgeSegment(bridge)
		if !reviewed {
			t.Fatalf("reviewed bridge %q has no segment", bridge)
		}
		address := netip.AddrFrom4([4]byte{10, 250, segment, 42})
		gateway := netip.AddrFrom4([4]byte{10, 250, segment, 1})
		prefix := netip.PrefixFrom(address, 24).String()
		if !validRunnerIPv4(prefix, gateway.String()) {
			t.Fatalf("bridge %q carries segment %d, which is not a lab address: %s", bridge, segment, prefix)
		}
		if !runnerAddressOnBridge(bridge, prefix, gateway.String()) {
			t.Fatalf("bridge %q refused its own segment: %s", bridge, prefix)
		}
	}
}

func TestRunnerAddressMustSitOnItsOwnBridgeSegment(t *testing.T) {
	tests := []struct {
		name    string
		bridge  string
		address string
		gateway string
		want    bool
	}{
		{name: "vmbr8 on its own segment", bridge: "vmbr8", address: "10.250.8.42/24", gateway: "10.250.8.1", want: true},
		{name: "vmbr9 on its own segment", bridge: "vmbr9", address: "10.250.9.18/24", gateway: "10.250.9.1", want: true},
		{name: "vmbr8 addressed for vmbr9", bridge: "vmbr8", address: "10.250.9.42/24", gateway: "10.250.9.1"},
		{name: "vmbr9 addressed for vmbr8", bridge: "vmbr9", address: "10.250.8.18/24", gateway: "10.250.8.1"},
		{name: "gateway on the other segment", bridge: "vmbr8", address: "10.250.8.42/24", gateway: "10.250.9.1"},
		{name: "management bridge", bridge: "vmbr0", address: "10.250.8.42/24", gateway: "10.250.8.1"},
		{name: "unreviewed bridge", bridge: "vmbr1", address: "10.250.8.42/24", gateway: "10.250.8.1"},
		{name: "bridge with a reviewed prefix", bridge: "vmbr80", address: "10.250.8.42/24", gateway: "10.250.8.1"},
		{name: "tagged bridge", bridge: "vmbr8.100", address: "10.250.8.42/24", gateway: "10.250.8.1"},
		{name: "empty bridge", bridge: "", address: "10.250.8.42/24", gateway: "10.250.8.1"},
		{name: "network address", bridge: "vmbr8", address: "10.250.8.0/24", gateway: "10.250.8.1"},
		{name: "broadcast address", bridge: "vmbr8", address: "10.250.8.255/24", gateway: "10.250.8.1"},
		{name: "outside the lab range", bridge: "vmbr8", address: "10.250.7.42/24", gateway: "10.250.7.1"},
		{name: "not an address at all", bridge: "vmbr8", address: "10.250.8.42", gateway: "10.250.8.1"},
	}
	for _, test := range tests {
		t.Run(test.name, func(t *testing.T) {
			if got := runnerAddressOnBridge(test.bridge, test.address, test.gateway); got != test.want {
				t.Fatalf("runnerAddressOnBridge(%q, %q, %q)=%t, want %t",
					test.bridge, test.address, test.gateway, got, test.want)
			}
		})
	}
}

// A bridge outside the reviewed set never yields a segment, so the segment
// lookup cannot become a second, looser way to name a bridge.
func TestBridgeSegmentIsOnlyReadForReviewedBridges(t *testing.T) {
	for _, bridge := range []string{"", "vmbr0", "vmbr1", "vmbr80", "VMBR8", "vmbr8 ", "vmbr8.100", "br8", "vmbr"} {
		if segment, reviewed := runnerBridgeSegment(bridge); reviewed {
			t.Fatalf("unreviewed bridge %q reported segment %d", bridge, segment)
		}
	}
}
