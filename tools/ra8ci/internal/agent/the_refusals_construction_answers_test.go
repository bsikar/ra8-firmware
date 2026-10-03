// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package agent

// The refusals New answers before an agent exists. Every one of these is a
// misconfigured host rather than a misbehaving plane, and an operator reading
// the unit's log has only this error to go on: if construction succeeded on a
// lapsed identity or an absent checkout the failure would surface much later,
// against the server, and be read as the server's fault.

import (
	"crypto/ed25519"
	"crypto/rand"
	"crypto/x509"
	"crypto/x509/pkix"
	"encoding/pem"
	"math/big"
	"os"
	"path/filepath"
	"testing"
	"time"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/testprivatefile"
)

// mintAuthority returns a self-signed CA and its key, written as PEM.
func mintAuthority(t *testing.T, dir string) (*x509.Certificate, ed25519.PrivateKey, string) {
	t.Helper()
	public, private, err := ed25519.GenerateKey(rand.Reader)
	if err != nil {
		t.Fatal(err)
	}
	template := &x509.Certificate{SerialNumber: big.NewInt(100),
		Subject:   pkix.Name{CommonName: "test-authority"},
		NotBefore: time.Now().Add(-time.Hour), NotAfter: time.Now().Add(24 * time.Hour),
		KeyUsage: x509.KeyUsageCertSign | x509.KeyUsageDigitalSignature,
		IsCA:     true, BasicConstraintsValid: true}
	der, err := x509.CreateCertificate(rand.Reader, template, template, public, private)
	if err != nil {
		t.Fatal(err)
	}
	authority, err := x509.ParseCertificate(der)
	if err != nil {
		t.Fatal(err)
	}
	path := filepath.Join(dir, "ca.pem")
	writePEM(t, path, "CERTIFICATE", der)
	return authority, private, path
}

// mintIdentity writes a client certificate and key under prefix. A mismatched
// key is written from a second pair, which is the shape of a half-rotated host.
func mintIdentity(t *testing.T, dir, prefix string, authority *x509.Certificate,
	authorityKey ed25519.PrivateKey, notBefore, notAfter time.Time, mismatched bool) (string, string) {
	t.Helper()
	public, private, err := ed25519.GenerateKey(rand.Reader)
	if err != nil {
		t.Fatal(err)
	}
	template := &x509.Certificate{SerialNumber: big.NewInt(time.Now().UnixNano()),
		Subject:   pkix.Name{CommonName: prefix},
		NotBefore: notBefore, NotAfter: notAfter,
		KeyUsage:    x509.KeyUsageDigitalSignature,
		ExtKeyUsage: []x509.ExtKeyUsage{x509.ExtKeyUsageClientAuth}}
	der, err := x509.CreateCertificate(rand.Reader, template, authority, public, authorityKey)
	if err != nil {
		t.Fatal(err)
	}
	if mismatched {
		if _, private, err = ed25519.GenerateKey(rand.Reader); err != nil {
			t.Fatal(err)
		}
	}
	keyDER, err := x509.MarshalPKCS8PrivateKey(private)
	if err != nil {
		t.Fatal(err)
	}
	certFile := filepath.Join(dir, prefix+".pem")
	keyFile := filepath.Join(dir, prefix+"-key.pem")
	writePEM(t, certFile, "CERTIFICATE", der)
	writePEM(t, keyFile, "PRIVATE KEY", keyDER)
	return certFile, keyFile
}

func writePEM(t *testing.T, path, kind string, der []byte) {
	t.Helper()
	if err := os.WriteFile(path, pem.EncodeToMemory(&pem.Block{Type: kind, Bytes: der}), 0600); err != nil {
		t.Fatal(err)
	}
	if kind != "CERTIFICATE" {
		if err := testprivatefile.OwnerOnly(path); err != nil {
			t.Fatal(err)
		}
	}
}

// soundConfig is a configuration New accepts, so each case below can spoil
// exactly one field and the refusal can only be about that field.
func soundConfig(t *testing.T) Config {
	t.Helper()
	dir := t.TempDir()
	authority, authorityKey, caFile := mintAuthority(t, dir)
	certFile, keyFile := mintIdentity(t, dir, "client", authority, authorityKey,
		time.Now().Add(-time.Hour), time.Now().Add(time.Hour), false)
	return Config{ServerURL: "https://plane.example.invalid", CAFile: caFile,
		CertFile: certFile, KeyFile: keyFile, Root: dir}
}

func TestNewAcceptsASoundConfiguration(t *testing.T) {
	config := soundConfig(t)
	agent, err := New(config)
	if err != nil {
		t.Fatalf("sound configuration refused: %v", err)
	}
	if agent == nil || agent.root == "" || agent.catalog == nil || agent.authorities == nil {
		t.Fatalf("agent built without its root, catalog or authority source: %+v", agent)
	}
	// An unset poll wait takes the default rather than zero, which would spin
	// the claim loop against the plane as fast as the network allows.
	if agent.pollWait != defaultPollWait {
		t.Fatalf("poll wait = %v, want the default %v", agent.pollWait, defaultPollWait)
	}
	// The trailing slash is dropped once, at construction, so every path the
	// agent joins later cannot produce a double slash the plane may 404.
	config.ServerURL = "https://plane.example.invalid/"
	agent, err = New(config)
	if err != nil {
		t.Fatalf("origin with a trailing slash refused: %v", err)
	}
	if agent.base != "https://plane.example.invalid" {
		t.Fatalf("base = %q, want the origin without its trailing slash", agent.base)
	}
}

func TestNewRequiresEveryPieceOfItsIdentity(t *testing.T) {
	for _, missing := range []string{"CAFile", "CertFile", "KeyFile", "Root"} {
		config := soundConfig(t)
		switch missing {
		case "CAFile":
			config.CAFile = ""
		case "CertFile":
			config.CertFile = ""
		case "KeyFile":
			config.KeyFile = ""
		case "Root":
			config.Root = ""
		}
		if _, err := New(config); err == nil {
			t.Fatalf("configuration accepted with no %s", missing)
		}
	}
}

func TestNewRefusesMaterialItCannotRead(t *testing.T) {
	t.Run("absent authority", func(t *testing.T) {
		config := soundConfig(t)
		config.CAFile = filepath.Join(t.TempDir(), "absent.pem")
		if _, err := New(config); err == nil {
			t.Fatal("absent CA bundle accepted")
		}
	})
	t.Run("unreadable authority", func(t *testing.T) {
		if os.Geteuid() == 0 {
			t.Skip("root reads a sealed file")
		}
		config := soundConfig(t)
		if err := os.Chmod(config.CAFile, 0o000); err != nil {
			t.Fatal(err)
		}
		if _, err := New(config); err == nil {
			t.Fatal("unreadable CA bundle accepted")
		}
	})
	t.Run("absent identity", func(t *testing.T) {
		config := soundConfig(t)
		config.CertFile = filepath.Join(t.TempDir(), "absent.pem")
		if _, err := New(config); err == nil {
			t.Fatal("absent client certificate accepted")
		}
	})
	t.Run("absent key", func(t *testing.T) {
		config := soundConfig(t)
		config.KeyFile = filepath.Join(t.TempDir(), "absent-key.pem")
		if _, err := New(config); err == nil {
			t.Fatal("absent client key accepted")
		}
	})
}

// A half-rotated host is the case worth naming: both files are present, both
// are well-formed PEM, and they simply do not belong to each other.
func TestNewRefusesAnIdentityThatIsNotAPair(t *testing.T) {
	dir := t.TempDir()
	authority, authorityKey, caFile := mintAuthority(t, dir)
	certFile, keyFile := mintIdentity(t, dir, "mismatched", authority, authorityKey,
		time.Now().Add(-time.Hour), time.Now().Add(time.Hour), true)
	if _, err := New(Config{ServerURL: "https://plane.example.invalid", CAFile: caFile,
		CertFile: certFile, KeyFile: keyFile, Root: dir}); err == nil {
		t.Fatal("certificate and key from different pairs accepted")
	}
}

// A lapsed identity is refused HERE rather than at the handshake, where the
// error names the server and sends the operator to the wrong log.
func TestNewRefusesAnIdentityOutsideItsValidity(t *testing.T) {
	for _, window := range []struct {
		name                string
		notBefore, notAfter time.Time
	}{
		{"expired", time.Now().Add(-48 * time.Hour), time.Now().Add(-time.Hour)},
		{"not yet valid", time.Now().Add(time.Hour), time.Now().Add(48 * time.Hour)},
	} {
		t.Run(window.name, func(t *testing.T) {
			dir := t.TempDir()
			authority, authorityKey, caFile := mintAuthority(t, dir)
			certFile, keyFile := mintIdentity(t, dir, "lapsed", authority, authorityKey,
				window.notBefore, window.notAfter, false)
			if _, err := New(Config{ServerURL: "https://plane.example.invalid", CAFile: caFile,
				CertFile: certFile, KeyFile: keyFile, Root: dir}); err == nil {
				t.Fatalf("%s identity accepted", window.name)
			}
		})
	}
}

func TestNewRefusesACheckoutItCannotResolve(t *testing.T) {
	config := soundConfig(t)
	config.Root = filepath.Join(t.TempDir(), "no-such-checkout")
	if _, err := New(config); err == nil {
		t.Fatal("absent checkout accepted")
	}
	// A symlink IS resolved rather than refused: the agent stores the resolved
	// path, so a later path check cannot be walked around through the link.
	config = soundConfig(t)
	link := filepath.Join(t.TempDir(), "link")
	if err := os.Symlink(config.Root, link); err != nil {
		t.Skipf("symlinks unavailable: %v", err)
	}
	config.Root = link
	agent, err := New(config)
	if err != nil {
		t.Fatalf("checkout behind a symlink refused: %v", err)
	}
	resolved, err := filepath.EvalSymlinks(link)
	if err != nil {
		t.Fatal(err)
	}
	if agent.root != resolved {
		t.Fatalf("root = %q, want the resolved %q", agent.root, resolved)
	}
}

// The poll wait is the agent's half of the plane's long poll. A negative one
// would expire the claim before it is sent; one past the plane's own ceiling
// would hold a request open longer than the plane will answer it.
func TestNewBoundsThePollWait(t *testing.T) {
	for _, wait := range []time.Duration{-time.Nanosecond, -time.Minute,
		25*time.Second + time.Nanosecond, time.Hour} {
		config := soundConfig(t)
		config.PollWait = wait
		if _, err := New(config); err == nil {
			t.Fatalf("poll wait %v accepted", wait)
		}
	}
	for _, wait := range []time.Duration{time.Nanosecond, time.Second, 25 * time.Second} {
		config := soundConfig(t)
		config.PollWait = wait
		agent, err := New(config)
		if err != nil {
			t.Fatalf("poll wait %v refused: %v", wait, err)
		}
		if agent.pollWait != wait {
			t.Fatalf("poll wait = %v, want %v", agent.pollWait, wait)
		}
	}
}
