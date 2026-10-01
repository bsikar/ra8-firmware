// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package main

import (
	"context"
	"net/http"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

// Sync reports historical evidence and can schedule nothing, so every refusal
// it makes is about material it was handed rather than work it was asked to
// do. The order matters: trust, then identity, then the local spool, then the
// plane. An operator reading one of these wants to know which file to go and
// look at, so each refusal below is checked for naming its own stage and no
// later one.

// privateStateDirectory returns an absolute directory the spool will accept.
// A temporary directory is created against the process umask, which on a root
// build box leaves it 0755, and the spool refuses anything the rest of the
// machine can reach.
func privateStateDirectory(t *testing.T) string {
	t.Helper()
	directory := filepath.Join(t.TempDir(), "outbox")
	if err := os.Mkdir(directory, 0o700); err != nil {
		t.Fatalf("plant state directory: %v", err)
	}
	if err := os.Chmod(directory, 0o700); err != nil {
		t.Fatalf("seal state directory: %v", err)
	}
	return directory
}

func TestSyncRefusesServerTrustItCannotRead(t *testing.T) {
	material := mintReportMaterial(t)
	home := t.TempDir()

	t.Run("a bundle that is not there", func(t *testing.T) {
		bindReportEnvironment(t, material, "https://ra8ci.invalid:8443")
		t.Setenv(envServerCA, filepath.Join(home, "absent.pem"))
		assertSyncStage(t, "read server CA")
	})

	t.Run("a bundle that is a directory", func(t *testing.T) {
		bindReportEnvironment(t, material, "https://ra8ci.invalid:8443")
		t.Setenv(envServerCA, home)
		assertSyncStage(t, "read server CA")
	})

	// A readable file that is not a bundle gets past the read and is refused
	// by the trust stage instead, which is the difference between "I could not
	// open your CA file" and "what is in it is not an authority".
	for name, content := range map[string]string{
		"a bundle that is not PEM":       "not a certificate\n",
		"an empty bundle":                "",
		"a PEM block that is not a cert": "-----BEGIN RSA PRIVATE KEY-----\nAAAA\n-----END RSA PRIVATE KEY-----\n",
	} {
		t.Run(name, func(t *testing.T) {
			bundle := filepath.Join(t.TempDir(), "ca.pem")
			if err := os.WriteFile(bundle, []byte(content), 0o600); err != nil {
				t.Fatalf("plant bundle: %v", err)
			}
			bindReportEnvironment(t, material, "https://ra8ci.invalid:8443")
			t.Setenv(envServerCA, bundle)
			assertSyncStage(t, "sync server trust")
		})
	}
}

func TestSyncRefusesAnIdentityItCannotPresent(t *testing.T) {
	material := mintReportMaterial(t)
	home := t.TempDir()

	t.Run("a certificate that is not there", func(t *testing.T) {
		bindReportEnvironment(t, material, "https://ra8ci.invalid:8443")
		t.Setenv(roleOperator.certEnv, filepath.Join(home, "absent.pem"))
		assertSyncStage(t, "sync client identity")
	})

	t.Run("a key that is not there", func(t *testing.T) {
		bindReportEnvironment(t, material, "https://ra8ci.invalid:8443")
		t.Setenv(roleOperator.keyEnv, filepath.Join(home, "absent.key"))
		assertSyncStage(t, "sync client identity")
	})

	// A private key the rest of the machine can read is already exposed, and
	// the identity stage says so before the key is used rather than after.
	t.Run("a key the rest of the machine can read", func(t *testing.T) {
		keyPEM, err := os.ReadFile(material.keyPath)
		if err != nil {
			t.Fatalf("read minted key: %v", err)
		}
		loose := filepath.Join(t.TempDir(), "loose.key")
		if err := os.WriteFile(loose, keyPEM, 0o644); err != nil {
			t.Fatalf("plant loose key: %v", err)
		}
		if err := os.Chmod(loose, 0o644); err != nil {
			t.Fatalf("loosen key: %v", err)
		}
		bindReportEnvironment(t, material, "https://ra8ci.invalid:8443")
		t.Setenv(roleOperator.keyEnv, loose)
		assertSyncStage(t, "sync client identity")
	})

	// A certificate and a key that are each sound but not a pair is the
	// mistake a second checkout makes, and it is refused as an identity
	// problem rather than reported as a handshake failure later.
	t.Run("a certificate and key that are not a pair", func(t *testing.T) {
		other := mintReportMaterial(t)
		bindReportEnvironment(t, material, "https://ra8ci.invalid:8443")
		t.Setenv(roleOperator.keyEnv, other.keyPath)
		assertSyncStage(t, "sync client identity")
	})
}

func TestSyncRefusesALocalSpoolItCannotTrust(t *testing.T) {
	material := mintReportMaterial(t)

	t.Run("a state directory named by a relative path", func(t *testing.T) {
		bindReportEnvironment(t, material, "https://ra8ci.invalid:8443")
		t.Setenv("RA8CI_STATE_DIR", "outbox")
		err := runSync(t)
		if err == nil || !strings.Contains(err.Error(), "RA8CI_STATE_DIR must be absolute") {
			t.Fatalf("err=%v; want the relative state directory refused", err)
		}
	})

	t.Run("a state directory the rest of the machine can reach", func(t *testing.T) {
		shared := filepath.Join(t.TempDir(), "shared")
		if err := os.Mkdir(shared, 0o755); err != nil {
			t.Fatalf("plant shared directory: %v", err)
		}
		if err := os.Chmod(shared, 0o755); err != nil {
			t.Fatalf("loosen directory: %v", err)
		}
		bindReportEnvironment(t, material, "https://ra8ci.invalid:8443")
		t.Setenv("RA8CI_STATE_DIR", shared)
		err := runSync(t)
		if err == nil || !strings.Contains(err.Error(), "accessible to other users") {
			t.Fatalf("err=%v; want a world-readable spool refused", err)
		}
	})

	t.Run("a state path that is a file", func(t *testing.T) {
		occupied := filepath.Join(t.TempDir(), "outbox")
		if err := os.WriteFile(occupied, []byte("not a spool"), 0o600); err != nil {
			t.Fatalf("plant file: %v", err)
		}
		bindReportEnvironment(t, material, "https://ra8ci.invalid:8443")
		t.Setenv("RA8CI_STATE_DIR", occupied)
		if err := runSync(t); err == nil {
			t.Fatal("a regular file was accepted as the local spool")
		}
	})
}

// With sound material and nothing spooled, sync reaches the plane and reports
// zero without inventing a record. This is the arm that proves the refusals
// above are about the material and not about sync refusing everything.
func TestSyncWithNothingSpooledReportsNothingAndSucceeds(t *testing.T) {
	material := mintReportMaterial(t)
	servingReport(t, material, func(writer http.ResponseWriter, request *http.Request) {
		t.Errorf("an empty spool asked the plane for %s %s", request.Method, request.URL.Path)
		writer.WriteHeader(http.StatusInternalServerError)
	})
	t.Setenv("RA8CI_STATE_DIR", privateStateDirectory(t))
	if err := syncLocalRuns(context.Background()); err != nil {
		t.Fatalf("sync with nothing to send failed: %v", err)
	}
}

// assertSyncStage runs sync and requires the refusal to name the stage given
// and no stage that comes after it.
func assertSyncStage(t *testing.T, stage string) {
	t.Helper()
	t.Setenv("RA8CI_STATE_DIR", privateStateDirectory(t))
	err := syncLocalRuns(context.Background())
	if err == nil || !strings.Contains(err.Error(), stage) {
		t.Fatalf("err=%v; want it to name %q", err, stage)
	}
	later := map[string][]string{
		"read server CA":       {"sync server trust", "sync client identity", "spool"},
		"sync server trust":    {"sync client identity", "spool"},
		"sync client identity": {"spool"},
	}
	for _, after := range later[stage] {
		if strings.Contains(err.Error(), after) {
			t.Fatalf("err=%q; want nothing from the later %q stage", err, after)
		}
	}
}

func runSync(t *testing.T) error {
	t.Helper()
	return syncLocalRuns(context.Background())
}
