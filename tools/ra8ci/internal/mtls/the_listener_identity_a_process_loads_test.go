// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package mtls

import (
	"crypto/ecdsa"
	"crypto/tls"
	"crypto/x509"
	"encoding/pem"
	"errors"
	"os"
	"path/filepath"
	"runtime"
	"strings"
	"testing"
	"time"
)

// The client side of this door is held by load_test.go. What is held here is
// the listener's side and the parse beneath both of them: a process that
// loads a server key pair without being refused gets a handshake failure on
// its first connection instead of a message naming the certificate, and a
// caller that cannot parse a leaf gets a nil dereference instead of a reason.

// serverKeyPairPEM mints a listener certificate and its key in PEM form, the
// shape an operator actually has on disk.
func serverKeyPairPEM(t *testing.T) ([]byte, []byte) {
	t.Helper()
	identity := issue(t, serverTemplate())
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

func TestLoadServerIdentityRequiresBothPaths(t *testing.T) {
	certPEM, keyPEM := serverKeyPairPEM(t)
	certPath, keyPath := writeKeyPair(t, certPEM, keyPEM)

	for _, missing := range []struct {
		name string
		cert string
		key  string
	}{
		{"no certificate path", "", keyPath},
		{"no key path", certPath, ""},
		{"neither path", "", ""},
	} {
		t.Run(missing.name, func(t *testing.T) {
			// Named before anything is read, so an operator who left one
			// setting empty is told which half is missing rather than being
			// handed a file error about the other half.
			if _, err := LoadServerIdentity(missing.cert, missing.key, testNow); !errors.Is(err, ErrIdentity) {
				t.Fatalf("an incomplete pair of paths was not refused as an identity: %v", err)
			}
		})
	}
}

func TestLoadServerIdentityRefusesACertificateItsKeyDoesNotMatch(t *testing.T) {
	certPEM, _ := serverKeyPairPEM(t)
	_, otherKeyPEM := serverKeyPairPEM(t)
	certPath, keyPath := writeKeyPair(t, certPEM, otherKeyPEM)

	// Both files are well-formed and the key's mode is private, so nothing
	// ahead of the pairing refuses them. A listener started on a key that
	// does not hold this certificate answers every handshake with an alert
	// that names nothing.
	_, err := LoadServerIdentity(certPath, keyPath, testNow)
	if !errors.Is(err, ErrIdentity) {
		t.Fatalf("a mismatched key pair was not refused as an identity: %v", err)
	}
	if !strings.Contains(err.Error(), "load key pair") {
		t.Fatalf("refusal should name the pairing that failed: %v", err)
	}
}

func TestLoadServerIdentityAcceptsAUsableListenerCertificate(t *testing.T) {
	certPEM, keyPEM := serverKeyPairPEM(t)
	certPath, keyPath := writeKeyPair(t, certPEM, keyPEM)

	identity, err := LoadServerIdentity(certPath, keyPath, testNow)
	if err != nil {
		t.Fatalf("a usable listener certificate was refused: %v", err)
	}
	leaf, err := Leaf(identity)
	if err != nil {
		t.Fatalf("leaf: %v", err)
	}
	if leaf.Subject.CommonName != "ra8ci-server" {
		t.Fatalf("loaded the wrong certificate: %q", leaf.Subject.CommonName)
	}
	if identity.PrivateKey == nil {
		t.Fatal("loaded identity carries no key, so it can present nothing")
	}

	// The same door still refuses a client identity served from the
	// listener, so the acceptance above is the certificate's doing and not
	// the door waving everything through.
	clientPEM, clientKeyPEM := clientKeyPairPEM(t, testNow.Add(-time.Hour), testNow.Add(time.Hour))
	clientCert, clientKey := writeKeyPair(t, clientPEM, clientKeyPEM)
	if _, err := LoadServerIdentity(clientCert, clientKey, testNow); !errors.Is(err, ErrIdentity) {
		t.Fatalf("a client identity was accepted as this listener's own: %v", err)
	}
}

func TestALeafThatCannotBeParsedIsRefusedRatherThanHandedBackEmpty(t *testing.T) {
	// A key pair whose certificate bytes are not a certificate. Every caller
	// in this package reaches its leaf through Leaf, so a silent nil here
	// would be dereferenced by the first rule that reads a subject.
	leaf, err := Leaf(tls.Certificate{Certificate: [][]byte{{0x30, 0x01, 0x00}}})
	if !errors.Is(err, ErrIdentity) {
		t.Fatalf("unparsable certificate bytes were not refused as an identity: %v", err)
	}
	if leaf != nil {
		t.Fatal("a refused parse handed back a leaf")
	}
	if !strings.Contains(err.Error(), "parse leaf certificate") {
		t.Fatalf("refusal should name the parse that failed: %v", err)
	}

	// And the two other ways a pair can carry nothing to parse stay told
	// apart from a parse failure.
	if _, err := Leaf(tls.Certificate{}); !errors.Is(err, ErrIdentity) {
		t.Fatalf("an empty key pair was not refused: %v", err)
	}
	if _, err := Leaf(tls.Certificate{Certificate: [][]byte{{}}}); !errors.Is(err, ErrIdentity) {
		t.Fatalf("a key pair holding empty bytes was not refused: %v", err)
	}
}

func TestLoadServerIdentityStillRefusesAKeyEveryAccountCanRead(t *testing.T) {
	if runtime.GOOS == "windows" {
		t.Skip("Windows ACL refusal is exercised in key_file_mode_windows_test.go")
	}
	if os.Geteuid() == 0 {
		t.Skip("running as root: file modes are not enforced")
	}
	certPEM, keyPEM := serverKeyPairPEM(t)
	certPath, keyPath := writeKeyPair(t, certPEM, keyPEM)
	if err := os.Chmod(keyPath, 0o644); err != nil {
		t.Fatal(err)
	}

	// Asked before the read, because a key the whole machine can read is
	// already exposed and the listener starting anyway is what hides it.
	if _, err := LoadServerIdentity(certPath, keyPath, testNow); err == nil {
		t.Fatal("a world-readable listener key was loaded")
	}
	if err := os.Chmod(keyPath, 0o600); err != nil {
		t.Fatal(err)
	}
	if _, err := LoadServerIdentity(certPath, keyPath, testNow); err != nil {
		t.Fatalf("the same pair once private: %v", err)
	}
	if _, err := os.Stat(filepath.Dir(keyPath)); err != nil {
		t.Fatal(err)
	}
}
