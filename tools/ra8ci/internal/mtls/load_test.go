// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package mtls

import (
	"crypto/ecdsa"
	"crypto/x509"
	"encoding/pem"
	"errors"
	"os"
	"path/filepath"
	"testing"
	"time"
)

// clientKeyPairPEM mints a client certificate and its key in PEM form, the
// shape an operator actually has on disk.
func clientKeyPairPEM(t *testing.T, notBefore, notAfter time.Time) ([]byte, []byte) {
	t.Helper()
	template := clientTemplate()
	template.NotBefore, template.NotAfter = notBefore, notAfter
	identity := issue(t, template)
	key, ok := identity.PrivateKey.(*ecdsa.PrivateKey)
	if !ok {
		t.Fatalf("unexpected key type %T", identity.PrivateKey)
	}
	der, err := x509.MarshalECPrivateKey(key)
	if err != nil {
		t.Fatalf("marshal key: %v", err)
	}
	certPEM := pem.EncodeToMemory(&pem.Block{Type: "CERTIFICATE", Bytes: identity.Certificate[0]})
	keyPEM := pem.EncodeToMemory(&pem.Block{Type: "EC PRIVATE KEY", Bytes: der})
	if len(certPEM) == 0 || len(keyPEM) == 0 {
		t.Fatal("empty PEM encoding")
	}
	return certPEM, keyPEM
}

// writeKeyPair puts a freshly minted certificate and key on disk and returns
// the two paths.
func writeKeyPair(t *testing.T, certPEM, keyPEM []byte) (string, string) {
	t.Helper()
	dir := t.TempDir()
	certPath := filepath.Join(dir, "identity.pem")
	keyPath := filepath.Join(dir, "identity.key")
	if err := os.WriteFile(certPath, certPEM, 0o600); err != nil {
		t.Fatalf("write certificate: %v", err)
	}
	if err := os.WriteFile(keyPath, keyPEM, 0o600); err != nil {
		t.Fatalf("write key: %v", err)
	}
	return certPath, keyPath
}

func TestLoadClientIdentityAcceptsAUsableClientCertificate(t *testing.T) {
	now := time.Now()
	certPEM, keyPEM := clientKeyPairPEM(t, now.Add(-time.Hour), now.Add(time.Hour))
	certPath, keyPath := writeKeyPair(t, certPEM, keyPEM)

	identity, err := LoadClientIdentity(certPath, keyPath, now)
	if err != nil {
		t.Fatalf("load: %v", err)
	}
	leaf, err := Leaf(identity)
	if err != nil {
		t.Fatalf("leaf: %v", err)
	}
	if Fingerprint(leaf) == "" {
		t.Fatal("loaded identity has no fingerprint")
	}
}

func TestLoadClientIdentityRefusesAnExpiredCertificateOnDisk(t *testing.T) {
	now := time.Now()
	certPEM, keyPEM := clientKeyPairPEM(t, now.Add(-48*time.Hour), now.Add(-time.Hour))
	certPath, keyPath := writeKeyPair(t, certPEM, keyPEM)

	// Without this the pair loads, the process starts, and the first request
	// fails as a handshake error indistinguishable from a missing grant.
	if _, err := LoadClientIdentity(certPath, keyPath, now); !errors.Is(err, ErrIdentity) {
		t.Fatalf("expired identity loaded from disk: %v", err)
	}
}

func TestLoadClientIdentityRefusesAMissingPath(t *testing.T) {
	now := time.Now()
	certPEM, keyPEM := clientKeyPairPEM(t, now.Add(-time.Hour), now.Add(time.Hour))
	certPath, keyPath := writeKeyPair(t, certPEM, keyPEM)

	if _, err := LoadClientIdentity("", keyPath, now); !errors.Is(err, ErrIdentity) {
		t.Fatalf("empty certificate path: %v", err)
	}
	if _, err := LoadClientIdentity(certPath, "", now); !errors.Is(err, ErrIdentity) {
		t.Fatalf("empty key path: %v", err)
	}
	if _, err := LoadClientIdentity(filepath.Join(t.TempDir(), "absent.pem"), keyPath, now); !errors.Is(err, ErrIdentity) {
		t.Fatalf("absent certificate file: %v", err)
	}
}
