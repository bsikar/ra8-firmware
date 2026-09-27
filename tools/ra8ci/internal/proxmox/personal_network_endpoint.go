// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package proxmox

import (
	"net/netip"
	"strings"
)

// The personal-network refusal reads a host, and a host has more than one
// spelling.
//
// New refuses an endpoint on the operator's own tailnet before it does
// anything else with it, and the reason is the whole shape of this package:
// every other boundary here (the pool, the reviewed bridges, the approved
// storage, the passed-through-device refusal) exists to keep a disposable
// guest away from networks nobody reviewed, and a control plane that reaches
// its hypervisor over a personal overlay has put the hypervisor itself on one.
// It is also the boundary an operator crosses by accident, copying a working
// endpoint out of their own shell.
//
// The check that did this looked at two spellings: a MagicDNS name ending
// ".ts.net", and an IPv4 address inside 100.64.0.0/10. The same host answers
// to at least two more, and both of them passed:
//
// The IPv4-mapped form. netip.ParseAddr("::ffff:100.64.1.2") parses, and
// Prefix.Contains compares address families before anything else, so a
// 4-in-6 address is not inside an IPv4 prefix however plainly it names an
// IPv4 host. Go's own resolver and Proxmox's listener both reach the same
// machine from it. internal/provision already closed this one in both of its
// copies of this rule (approle.go and http_backend.go each call Unmap before
// the Contains), and this copy was left behind; the drift, not the shape of
// the rule, is what made it reachable here.
//
// The tailnet's IPv6 address. Tailscale gives every node an address in
// fd7a:115c:a1e0::/48 alongside its 100.64.0.0/10 one, and it is what MagicDNS
// hands back on an IPv6-preferring host. Refusing the name and the IPv4
// address while accepting the IPv6 address of the same node refuses a habit
// rather than a network.
//
// A trailing dot is the third: "pve.tailnet.ts.net." is the same fully
// qualified name, is a legal URL host, and does not end in ".ts.net".
//
// None of this judges whether the endpoint is reachable or trusted; the CA
// pin and the token do that. It judges what kind of network the operator is
// naming, so the answer must not depend on which of that network's names they
// happened to type.
var personalNetworkPrefixes = []netip.Prefix{
	netip.MustParsePrefix("100.64.0.0/10"),
	netip.MustParsePrefix("fd7a:115c:a1e0::/48"),
}

// isPersonalNetworkHost reports whether a URL host names a node on a personal
// overlay network, by any of the spellings that reach one.
func isPersonalNetworkHost(host string) bool {
	name := strings.TrimSuffix(strings.ToLower(host), ".")
	if strings.HasSuffix(name, ".ts.net") {
		return true
	}
	addr, err := netip.ParseAddr(host)
	if err != nil {
		return false
	}
	// Unmap first: an IPv4 host written in the 4-in-6 form is an IPv4 host,
	// and a prefix of the other family contains nothing.
	addr = addr.Unmap()
	for _, prefix := range personalNetworkPrefixes {
		if prefix.Contains(addr) {
			return true
		}
	}
	return false
}
