//go:build unix

// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package github

import (
	"crypto/ecdsa"
	"crypto/elliptic"
	"crypto/rand"
	"crypto/x509"
	"encoding/pem"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

func appKeyFile(t *testing.T, body []byte, mode os.FileMode) string {
	t.Helper()
	file := filepath.Join(t.TempDir(), "app.pem")
	if err := os.WriteFile(file, body, 0o600); err != nil {
		t.Fatal(err)
	}
	if err := os.Chmod(file, mode); err != nil {
		t.Fatal(err)
	}
	return file
}

func TestAnAppKeyIsReadOnlyFromAPrivateBoundedRegularFile(t *testing.T) {
	_, keyPEM := mintedAppKey(t)
	good := appKeyFile(t, keyPEM, 0o600)
	if _, err := loadAppPrivateKey(good); err != nil {
		t.Fatalf("a private RSA key file was refused: %v", err)
	}
	if _, err := loadAppPrivateKey(filepath.Join(t.TempDir(), "absent.pem")); err == nil {
		t.Fatal("an absent key file was loaded")
	}
	if _, err := loadAppPrivateKey(t.TempDir()); err == nil {
		t.Fatal("a directory was loaded as a key")
	}
	for _, mode := range []os.FileMode{0o604, 0o640, 0o644, 0o660} {
		if _, err := loadAppPrivateKey(appKeyFile(t, keyPEM, mode)); err == nil {
			t.Fatalf("a key readable at mode %v was loaded", mode)
		}
	}
	if _, err := loadAppPrivateKey(appKeyFile(t, nil, 0o600)); err == nil {
		t.Fatal("an empty key file was loaded")
	}
	if _, err := loadAppPrivateKey(appKeyFile(t, []byte(strings.Repeat("k", maxGitHubPrivateKeyBytes+1)), 0o600)); err == nil {
		t.Fatal("an oversized key file was loaded")
	}
	if _, err := loadAppPrivateKey(appKeyFile(t, []byte("not pem at all\n"), 0o600)); err == nil {
		t.Fatal("a file that is not PEM was loaded")
	}
	link := filepath.Join(t.TempDir(), "link.pem")
	symlinkTest(t, good, link)
	if _, err := loadAppPrivateKey(link); err == nil {
		t.Fatal("a symlinked key was loaded")
	}
}

func TestAnAppKeyOfAnotherKindIsRefused(t *testing.T) {
	elliptical, err := ecdsa.GenerateKey(elliptic.P256(), rand.Reader)
	if err != nil {
		t.Fatal(err)
	}
	der, err := x509.MarshalECPrivateKey(elliptical)
	if err != nil {
		t.Fatal(err)
	}
	body := pem.EncodeToMemory(&pem.Block{Type: "EC PRIVATE KEY", Bytes: der})
	if _, err := loadAppPrivateKey(appKeyFile(t, body, 0o600)); err == nil {
		t.Fatal("an elliptic-curve key was loaded for an RS256 App")
	}
}
