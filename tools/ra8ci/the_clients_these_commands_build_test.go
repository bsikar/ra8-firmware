// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package main

import (
	"os"
	"path/filepath"
	"strings"
	"testing"
)

// Both client constructors sit between the environment and the transport: the
// endpoint is resolved from the surface, then the material on disk is loaded
// before any request is sent. Nothing here reaches a server, so the whole
// constructor is testable on a box with no plane running, and the refusal
// wording is what an operator with a half-installed identity actually reads.

// The board constructor wraps whatever the client library refuses with its own
// prefix, so an operator can tell a board-client fault from a run-client one
// when both commands are failing against the same broken material.
func TestTheBoardClientNamesItselfWhenTheMaterialIsRefused(t *testing.T) {
	material := mintReportMaterial(t)

	for _, testCase := range []struct {
		name   string
		break_ func(t *testing.T)
	}{
		{"an absent authority", func(t *testing.T) {
			t.Setenv(envServerCA, filepath.Join(t.TempDir(), "absent-ca.pem"))
		}},
		{"an authority that is not a certificate", func(t *testing.T) {
			path := filepath.Join(t.TempDir(), "ca.pem")
			if err := os.WriteFile(path, []byte("not a certificate\n"), 0o600); err != nil {
				t.Fatal(err)
			}
			t.Setenv(envServerCA, path)
		}},
		{"an absent identity", func(t *testing.T) {
			t.Setenv(roleOperator.certEnv, filepath.Join(t.TempDir(), "absent-cert.pem"))
		}},
		{"a key that does not match the certificate", func(t *testing.T) {
			other := mintReportMaterial(t)
			t.Setenv(roleOperator.keyEnv, other.keyPath)
		}},
		{"an origin that is not https", func(t *testing.T) {
			t.Setenv(envServerURL, "http://ra8ci.example:8443")
		}},
		{"an origin carrying a path", func(t *testing.T) {
			t.Setenv(envServerURL, "https://ra8ci.example:8443/v1")
		}},
	} {
		t.Run(testCase.name, func(t *testing.T) {
			bindReportEnvironment(t, material, "https://ra8ci.example:8443")
			testCase.break_(t)

			client, err := newBoardClient()
			if err == nil {
				t.Fatalf("%s was accepted as %v", testCase.name, client)
			}
			if !strings.HasPrefix(err.Error(), "board client: ") {
				t.Errorf("err=%q; want the board client to name itself", err)
			}
		})
	}
}

// With the surface unset the refusal comes from the environment rather than
// the client library, so it names the variables to set and does NOT carry the
// board-client prefix. An operator who has installed nothing yet should be
// told to set variables, not told their client is broken.
func TestAnUnsetSurfaceIsRefusedBeforeTheBoardClientIsBuilt(t *testing.T) {
	for _, name := range []string{envServerURL, envServerCA, roleOperator.certEnv, roleOperator.keyEnv} {
		t.Setenv(name, "")
		os.Unsetenv(name)
	}
	client, err := newBoardClient()
	if err == nil {
		t.Fatalf("an unset surface built %v", client)
	}
	if strings.Contains(err.Error(), "board client:") {
		t.Errorf("err=%q; an unset surface is not a client fault", err)
	}
	if !strings.HasPrefix(err.Error(), "ra8ci board: set ") {
		t.Errorf("err=%q; want the command and what to set", err)
	}
}

// Sound material builds a client without any server being reachable: the
// constructor loads files and arranges a transport, and the first request is
// what needs a plane. This is the arm every working board command runs
// through, and it must not be accidentally coupled to a live endpoint.
func TestSoundMaterialBuildsABoardClientWithNoPlaneRunning(t *testing.T) {
	material := mintReportMaterial(t)
	bindReportEnvironment(t, material, "https://ra8ci.example:8443")

	client, err := newBoardClient()
	if err != nil {
		t.Fatalf("sound material was refused: %v", err)
	}
	if client == nil {
		t.Fatal("sound material built a nil client and no error")
	}
}

// The run constructor hands the library's refusal back unwrapped, which is the
// deliberate difference from the board one. Pin both halves so a later edit
// cannot quietly swap which command adds a prefix.
func TestTheRunClientRefusesTheSameMaterialWithoutNamingItself(t *testing.T) {
	material := mintReportMaterial(t)
	bindReportEnvironment(t, material, "https://ra8ci.example:8443")
	t.Setenv(envServerCA, filepath.Join(t.TempDir(), "absent-ca.pem"))

	client, err := newRunClient()
	if err == nil {
		t.Fatalf("an absent authority built %v", client)
	}
	if strings.HasPrefix(err.Error(), "run client: ") {
		t.Errorf("err=%q; the run client does not wrap the library refusal", err)
	}
}

func TestSoundMaterialBuildsARunClientWithNoPlaneRunning(t *testing.T) {
	material := mintReportMaterial(t)
	bindReportEnvironment(t, material, "https://ra8ci.example:8443")

	client, err := newRunClient()
	if err != nil {
		t.Fatalf("sound material was refused: %v", err)
	}
	if client == nil {
		t.Fatal("sound material built a nil client and no error")
	}
}

func TestAnUnsetSurfaceIsRefusedBeforeTheRunClientIsBuilt(t *testing.T) {
	for _, name := range []string{envServerURL, envServerCA, roleOperator.certEnv, roleOperator.keyEnv} {
		t.Setenv(name, "")
		os.Unsetenv(name)
	}
	client, err := newRunClient()
	if err == nil {
		t.Fatalf("an unset surface built %v", client)
	}
	if !strings.HasPrefix(err.Error(), "ra8ci run: set ") {
		t.Errorf("err=%q; want the command and what to set", err)
	}
}
