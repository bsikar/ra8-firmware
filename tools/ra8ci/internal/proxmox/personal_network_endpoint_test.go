// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package proxmox

import (
	"encoding/pem"
	"errors"
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"testing"
	"time"
)

func TestMagicDNSNameIsPersonal(t *testing.T) {
	for _, host := range []string{"pve.ts.net", "pve.tailnet-1234.ts.net", "PVE.TAILNET.TS.NET"} {
		if !isPersonalNetworkHost(host) {
			t.Fatalf("MagicDNS host %q accepted", host)
		}
	}
}

func TestFullyQualifiedMagicDNSNameIsPersonal(t *testing.T) {
	if !isPersonalNetworkHost("pve.tailnet-1234.ts.net.") {
		t.Fatal("fully qualified MagicDNS host accepted")
	}
}

func TestCGNATAddressIsPersonal(t *testing.T) {
	for _, host := range []string{"100.64.0.0", "100.100.100.100", "100.127.255.255"} {
		if !isPersonalNetworkHost(host) {
			t.Fatalf("CGNAT host %q accepted", host)
		}
	}
}

func TestMappedCGNATAddressIsPersonal(t *testing.T) {
	for _, host := range []string{"::ffff:100.64.1.2", "::ffff:100.100.100.100"} {
		if !isPersonalNetworkHost(host) {
			t.Fatalf("4-in-6 CGNAT host %q accepted", host)
		}
	}
}

func TestTailnetIPv6AddressIsPersonal(t *testing.T) {
	for _, host := range []string{"fd7a:115c:a1e0::1", "fd7a:115c:a1e0:ab12:4843:cd96:6464:1", "FD7A:115C:A1E0::1"} {
		if !isPersonalNetworkHost(host) {
			t.Fatalf("tailnet IPv6 host %q accepted", host)
		}
	}
}

func TestOrdinaryHostIsNotPersonal(t *testing.T) {
	for _, host := range []string{
		"pve.lab.example.com",
		"127.0.0.1",
		"10.0.0.5",
		"::1",
		"2001:db8::1",
		"100.63.255.255",
		"100.128.0.0",
		"fd7a:115b:a1e0::1",
		"fd7a:115c:a1df::1",
		"::ffff:10.0.0.5",
	} {
		if isPersonalNetworkHost(host) {
			t.Fatalf("ordinary host %q refused as personal", host)
		}
	}
}

// A name that merely contains the suffix is not on the tailnet: the rule is
// about the label boundary, not about the letters appearing somewhere.
func TestLookalikeNameIsNotPersonal(t *testing.T) {
	for _, host := range []string{"ts.net.example.com", "notts.net.example.com", "pve.ts.network"} {
		if isPersonalNetworkHost(host) {
			t.Fatalf("lookalike host %q refused as personal", host)
		}
	}
}

// An unparseable host is judged by name alone and otherwise left to the URL
// rules in New, which already require an explicit HTTPS origin with a port.
func TestUnparseableHostIsNotPersonal(t *testing.T) {
	for _, host := range []string{"", "not an address", "100.64.0.1.5"} {
		if isPersonalNetworkHost(host) {
			t.Fatalf("unparseable host %q refused as personal", host)
		}
	}
}

// The constructor is the only caller, so the spellings it used to accept are
// pinned through it as well as through the rule.
func TestNewRefusesEveryPersonalEndpointSpelling(t *testing.T) {
	config := personalEndpointConfig(t)
	for _, endpoint := range []string{
		"https://pve.tailnet-1234.ts.net:8006",
		"https://pve.tailnet-1234.ts.net.:8006",
		"https://100.100.100.100:8006",
		"https://[::ffff:100.64.1.2]:8006",
		"https://[fd7a:115c:a1e0::1]:8006",
	} {
		candidate := config
		candidate.Endpoint = endpoint
		if _, err := New(candidate); !errors.Is(err, ErrInvalid) {
			t.Fatalf("personal endpoint %q accepted: %v", endpoint, err)
		}
	}
}

// The same configuration on an ordinary origin still builds, so the rule is
// shown to refuse the network rather than the port or the scheme.
func TestNewAcceptsAnOrdinaryEndpoint(t *testing.T) {
	config := personalEndpointConfig(t)
	if _, err := New(config); err != nil {
		t.Fatalf("ordinary endpoint refused: %v", err)
	}
}

// personalEndpointConfig is a configuration valid in every respect except the
// endpoint the caller substitutes, built against a live test server so its CA
// and token pass the checks that run beside the endpoint rule.
func personalEndpointConfig(t *testing.T) Config {
	t.Helper()
	server := httptest.NewTLSServer(http.HandlerFunc(func(w http.ResponseWriter, _ *http.Request) {
		w.WriteHeader(http.StatusNotFound)
	}))
	t.Cleanup(server.Close)
	ca := filepath.Join(t.TempDir(), "ca.pem")
	if err := os.WriteFile(ca, pem.EncodeToMemory(&pem.Block{Type: "CERTIFICATE", Bytes: server.Certificate().Raw}), 0600); err != nil {
		t.Fatal(err)
	}
	token := filepath.Join(t.TempDir(), "token")
	if err := os.WriteFile(token, []byte("ra8ci@pve!client=secret-token\n"), 0600); err != nil {
		t.Fatal(err)
	}
	return Config{
		Endpoint: server.URL, CAFile: ca, TokenFile: token,
		Node: "pve", Pool: "ra8-tf-lab", Storage: "ra8-tf-lab",
		AllowedVMIDs: []int{9000}, TemplateVMIDs: []int{9001},
		Bridges:        []string{"vmbr8"},
		RequestTimeout: time.Second, OperationTimeout: time.Second, TaskPollInterval: time.Millisecond,
	}
}
