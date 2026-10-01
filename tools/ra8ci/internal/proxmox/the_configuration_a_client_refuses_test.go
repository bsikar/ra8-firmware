// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package proxmox

import (
	"errors"
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"
)

// reviewedConfig states a configuration this client accepts, so a test that
// varies one field proves the refusal is about that field and nothing else.
// The CA and the token are real files, since both are read before New returns.
func reviewedConfig(t *testing.T) Config {
	t.Helper()
	dir := t.TempDir()
	ca := filepath.Join(dir, "ca.pem")
	if err := os.WriteFile(ca, authorityPEM(t, authorityTemplate("pve-root-ca")), 0600); err != nil {
		t.Fatal(err)
	}
	token := filepath.Join(dir, "token")
	if err := os.WriteFile(token, []byte("ra8ci@pve!client=secret-token\n"), 0600); err != nil {
		t.Fatal(err)
	}
	return Config{
		Endpoint: "https://pve.lab.example:8006", CAFile: ca, TokenFile: token,
		Node: "pve", Pool: "ra8-tf-lab", Storage: "ra8-tf-lab",
		AllowedVMIDs: []int{9000}, TemplateVMIDs: []int{9001}, Bridges: []string{"vmbr8"},
		RequestTimeout: time.Second, OperationTimeout: time.Second, TaskPollInterval: time.Millisecond,
	}
}

func refused(t *testing.T, cfg Config) error {
	t.Helper()
	client, err := New(cfg)
	if err == nil {
		t.Fatal("a configuration that should have been refused built a client")
	}
	if client != nil {
		t.Error("a refused configuration still handed back a client")
	}
	if !errors.Is(err, ErrInvalid) {
		t.Fatalf("refusal = %v, want it to carry ErrInvalid", err)
	}
	return err
}

// The baseline has to be accepted, or every refusal below could be about
// something else entirely.
func TestTheReviewedConfigurationIsAccepted(t *testing.T) {
	if _, err := New(reviewedConfig(t)); err != nil {
		t.Fatalf("reviewed configuration refused: %v", err)
	}
}

// Node, pool and storage are named by an operator, never derived from a job,
// and the control pool is the one name that must never be handed to a client
// that creates and destroys guests: a typo there aims the lifecycle at the
// pool the control plane itself lives in.
func TestTheNodePoolAndStorageMustEachBeExplicit(t *testing.T) {
	for _, one := range []struct {
		name   string
		mutate func(*Config)
	}{
		{"empty node", func(c *Config) { c.Node = "" }},
		{"node with a path separator", func(c *Config) { c.Node = "pve/lab" }},
		{"empty pool", func(c *Config) { c.Pool = "" }},
		{"the control plane's own pool", func(c *Config) { c.Pool = "ra8ci-control" }},
		{"empty storage", func(c *Config) { c.Storage = "" }},
		{"storage with a space", func(c *Config) { c.Storage = "ra8 tf lab" }},
	} {
		t.Run(one.name, func(t *testing.T) {
			cfg := reviewedConfig(t)
			one.mutate(&cfg)
			err := refused(t, cfg)
			if !strings.Contains(err.Error(), "node, disposable pool, and storage") {
				t.Errorf("refusal = %v, want the three explicit names called out", err)
			}
		})
	}
}

// Template IDs are a reviewed reservation, not a range: an empty list or an ID
// below the reserved floor would let a clone read a guest nobody reviewed.
func TestTemplateIDsAreAReviewedReservation(t *testing.T) {
	for _, one := range []struct {
		name string
		ids  []int
	}{
		{"none at all", nil},
		{"an empty list", []int{}},
		{"below the reserved floor", []int{8999}},
		{"a repeated ID", []int{9001, 9001}},
	} {
		t.Run(one.name, func(t *testing.T) {
			cfg := reviewedConfig(t)
			cfg.TemplateVMIDs = one.ids
			err := refused(t, cfg)
			if !strings.Contains(err.Error(), "template") {
				t.Errorf("refusal = %v, want the template reservation named", err)
			}
		})
	}
}

// A CA that cannot be read is refused before any request is built, and the
// refusal names the load rather than leaving an operator to guess whether the
// file was missing or its contents were wrong.
func TestACAFileThatCannotBeReadIsRefusedBeforeAnyRequest(t *testing.T) {
	cfg := reviewedConfig(t)
	cfg.CAFile = filepath.Join(t.TempDir(), "absent", "ca.pem")
	err := refused(t, cfg)
	if !strings.Contains(err.Error(), "load configured CA") {
		t.Errorf("refusal = %v, want the CA load named", err)
	}
}

// A token file can pass every policy check on its metadata and still not open:
// mode 0o000 is private, regular and small. The refusal has to come from the
// read rather than the client starting up with an empty credential.
func TestATokenFileThatPassesPolicyAndStillWillNotOpenIsRefused(t *testing.T) {
	if os.Geteuid() == 0 {
		t.Skip("root reads a mode 0o000 file regardless of its mode")
	}
	cfg := reviewedConfig(t)
	sealed := filepath.Join(t.TempDir(), "token")
	if err := os.WriteFile(sealed, []byte("ra8ci@pve!client=secret-token\n"), 0o000); err != nil {
		t.Fatal(err)
	}
	cfg.TokenFile = sealed

	err := refused(t, cfg)
	if !strings.Contains(err.Error(), "token file unreadable") {
		t.Errorf("refusal = %v, want the unreadable token file named", err)
	}
}

// A file that opens but holds something that is not a token is refused on its
// shape, so a truncated or commented credential never reaches the API as an
// Authorization header.
func TestATokenOfTheWrongShapeIsRefused(t *testing.T) {
	for _, one := range []struct {
		name string
		body string
	}{
		{"empty", ""},
		{"no secret", "ra8ci@pve!client=\n"},
		{"no token name", "ra8ci@pve=secret-token\n"},
		{"a comment above it", "# lab token\nra8ci@pve!client=secret-token\n"},
		{"trailing spaces", "ra8ci@pve!client=secret-token  \n"},
	} {
		t.Run(one.name, func(t *testing.T) {
			cfg := reviewedConfig(t)
			path := filepath.Join(t.TempDir(), "token")
			if err := os.WriteFile(path, []byte(one.body), 0600); err != nil {
				t.Fatal(err)
			}
			cfg.TokenFile = path
			err := refused(t, cfg)
			if !strings.Contains(err.Error(), "malformed Proxmox API token") {
				t.Errorf("refusal = %v, want the token shape named", err)
			}
		})
	}
}

// A task ID is only meaningful beside the operation it claims to be, so an
// operation this package does not know has no expected task type and must be
// refused rather than matched against an empty one, which would accept any
// task at all.
func TestATaskIDForAnUnknownOperationIsRefused(t *testing.T) {
	_, err := parseTaskID("UPID:pve:0000A1B2:00C3D4E5:66F70000:qmclone:9000:ra8ci@pve!client:", "pve", "teleport")
	if !errors.Is(err, ErrInvalid) {
		t.Fatalf("err = %v, want ErrInvalid for an operation kind this package does not know", err)
	}
	if !strings.Contains(err.Error(), "unknown operation kind") {
		t.Errorf("refusal = %v, want the unknown kind named", err)
	}
}
