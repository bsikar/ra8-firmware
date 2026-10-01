// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package boardclient

import (
	"errors"
	"os"
	"path/filepath"
	"testing"
)

// New is the only thing that builds a client able to reach the server, so
// every refusal it makes is a process that never opens a socket rather
// than one that fails its first real request with an opaque handshake
// error. These are the refusals that need the files on disk read, which is
// why they are not covered by the configuration-shape cases.

// copiedTo writes content under a fresh name so a case can hand New a file
// that is readable and still wrong.
func copiedTo(t *testing.T, name string, content []byte, mode os.FileMode) string {
	t.Helper()
	path := filepath.Join(t.TempDir(), name)
	if err := os.WriteFile(path, content, mode); err != nil {
		t.Fatal(err)
	}
	if err := os.Chmod(path, mode); err != nil {
		t.Fatal(err)
	}
	return path
}

func readFile(t *testing.T, path string) []byte {
	t.Helper()
	content, err := os.ReadFile(path)
	if err != nil {
		t.Fatal(err)
	}
	return content
}

// A bundle that verifies nothing today is refused at startup. An empty
// file, bytes that are not certificates at all, and a perfectly valid
// certificate that is not an authority all leave this process unable to
// tell the real server from any other, which is the one thing mutual TLS
// is here to decide.
func TestNewRefusesAnAuthorityBundleThatVerifiesNothing(t *testing.T) {
	material := testTLSCertificates(t)
	clientPEM := readFile(t, material.clientCertPath)

	for name, bundle := range map[string][]byte{
		"an empty bundle":              {},
		"bytes that are not PEM":       []byte("this is not a certificate\n"),
		"a PEM block that is not one":  []byte("-----BEGIN CERTIFICATE-----\nbm90IGEgY2VydA==\n-----END CERTIFICATE-----\n"),
		"a certificate that cannot si": clientPEM,
	} {
		_, err := New(Config{
			ServerURL: "https://localhost",
			CAFile:    copiedTo(t, "ca.pem", bundle, 0o600),
			CertFile:  material.clientCertPath,
			KeyFile:   material.clientKeyPath,
		})
		if !errors.Is(err, ErrInvalidConfig) {
			t.Fatalf("%s = %v, want the client refused before it opens a socket", name, err)
		}
	}
}

// An identity this process cannot present is refused the same way. The key
// that does not belong to the certificate would fail at the first
// handshake, and a key every account on the host can read is already
// exposed, so saying so before the process starts is the whole value of
// saying it at all.
func TestNewRefusesAnIdentityItCannotPresent(t *testing.T) {
	material := testTLSCertificates(t)
	certPEM := readFile(t, material.clientCertPath)
	keyPEM := readFile(t, material.clientKeyPath)

	for name, config := range map[string]Config{
		"a key that is not the certificate's": {
			CertFile: material.caPath, KeyFile: material.clientKeyPath,
		},
		"a certificate that is not one": {
			CertFile: copiedTo(t, "cert.pem", []byte("not a certificate\n"), 0o600),
			KeyFile:  material.clientKeyPath,
		},
		"a key the whole host can read": {
			CertFile: copiedTo(t, "cert.pem", certPEM, 0o600),
			KeyFile:  copiedTo(t, "key.pem", keyPEM, 0o644),
		},
	} {
		config.ServerURL = "https://localhost"
		config.CAFile = material.caPath
		if _, err := New(config); !errors.Is(err, ErrInvalidConfig) {
			t.Fatalf("%s = %v, want the identity refused before it opens a socket", name, err)
		}
	}
}
