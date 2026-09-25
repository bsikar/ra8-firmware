// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package provision

import (
	"context"
	"crypto/ecdsa"
	"crypto/elliptic"
	"crypto/rand"
	"crypto/tls"
	"crypto/x509"
	"crypto/x509/pkix"
	"encoding/pem"
	"math/big"
	"net"
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"
)

// appRoleConfigWithCA writes a working AppRole credential set beside the CA
// bundle under test, so a test changes one thing: the trust file.
func appRoleConfigWithCA(t *testing.T, address string, bundle []byte) AppRoleConfig {
	t.Helper()
	directory := t.TempDir()
	write := func(name string, value []byte) string {
		t.Helper()
		file := filepath.Join(directory, name)
		if err := os.WriteFile(file, value, 0o600); err != nil {
			t.Fatal(err)
		}
		return file
	}
	return AppRoleConfig{
		Address:      address,
		AuthMount:    "auth/approle",
		RoleIDFile:   write("role-id", []byte("role-id-1234567890\n")),
		SecretIDFile: write("secret-id", []byte("secret-id-12345678901234567890\n")),
		CAFile:       write("ca.pem", bundle),
		Timeout:      time.Second,
	}
}

// The Vault endpoint is reached with the operator's own trust file, so a
// bundle that can authenticate nobody is the operator's file to fix and has to
// be refused before the handshake turns it into "certificate signed by unknown
// authority", which reads as a broken Vault host.
func TestLoginAppRoleRefusesACABundleThatAuthenticatesNobody(t *testing.T) {
	now := time.Now()
	_, leaf := vaultChain(t)
	server := vaultServer(t, leaf)
	defer server.Close()
	for name, bundle := range map[string][]byte{
		"the endpoint's own leaf": authorityPEM(t, &x509.Certificate{
			SerialNumber: big.NewInt(21),
			Subject:      pkix.Name{CommonName: "vault.internal"},
			NotBefore:    now.Add(-time.Hour),
			NotAfter:     now.Add(time.Hour),
			KeyUsage:     x509.KeyUsageDigitalSignature,
			ExtKeyUsage:  []x509.ExtKeyUsage{x509.ExtKeyUsageServerAuth},
		}),
		"may not sign": authorityPEM(t, &x509.Certificate{
			SerialNumber:          big.NewInt(22),
			Subject:               pkix.Name{CommonName: "vault CA"},
			NotBefore:             now.Add(-time.Hour),
			NotAfter:              now.Add(time.Hour),
			IsCA:                  true,
			BasicConstraintsValid: true,
			KeyUsage:              x509.KeyUsageDigitalSignature,
		}),
		"every authority expired": authorityPEM(t, caTemplateAt(23, now.Add(-48*time.Hour), now.Add(-time.Hour))),
		"no certificate in it":    []byte("-----BEGIN PRIVATE KEY-----\nZm9v\n-----END PRIVATE KEY-----\n"),
		"empty":                   nil,
	} {
		t.Run(name, func(t *testing.T) {
			config := appRoleConfigWithCA(t, server.URL+"/bao", bundle)
			_, err := LoginAppRole(context.Background(), config)
			if err == nil {
				t.Fatal("a CA bundle that can authenticate nobody was accepted")
			}
			if !strings.Contains(err.Error(), "AppRole CA bundle") {
				t.Fatalf("refusal does not name the file an operator must fix: %v", err)
			}
		})
	}
}

// The refusal happens while the bundle is being read, before any credential
// reaches the network.
func TestLoginAppRoleRefusesTheBundleWithoutCallingTheEndpoint(t *testing.T) {
	var called bool
	server := httptest.NewTLSServer(http.HandlerFunc(func(http.ResponseWriter, *http.Request) {
		called = true
	}))
	defer server.Close()
	config := appRoleConfigWithCA(t, server.URL+"/bao", authorityPEM(t,
		caTemplateAt(24, time.Now().Add(-48*time.Hour), time.Now().Add(-time.Hour))))
	if _, err := LoginAppRole(context.Background(), config); err == nil {
		t.Fatal("an expired CA bundle was accepted")
	}
	if called {
		t.Fatal("the login request was sent under a bundle that authenticates nobody")
	}
}

// A rotation puts the outgoing authority beside the incoming one, and the
// bundle stays usable while at least one of them is. Same rule the listener
// and every other client in this tree already read.
func TestLoginAppRoleAcceptsARotatingCABundle(t *testing.T) {
	authority, leaf := vaultChain(t)
	server := vaultServer(t, leaf)
	defer server.Close()
	retired := authorityPEM(t, caTemplateAt(25, time.Now().Add(-72*time.Hour), time.Now().Add(-time.Hour)))
	config := appRoleConfigWithCA(t, server.URL+"/bao", append(retired, authority...))
	token, err := LoginAppRole(context.Background(), config)
	if err != nil {
		t.Fatalf("rotation bundle was refused: %v", err)
	}
	token.Clear()
}

// The endpoint is trusted through a real chain: an authority in the bundle
// that issued the certificate the endpoint presents. The fixture that came
// before this pinned the endpoint's own certificate as the anchor, which is
// the shape this check now refuses.
func TestLoginAppRoleTrustsAnEndpointIssuedByTheBundle(t *testing.T) {
	authority, leaf := vaultChain(t)
	server := vaultServer(t, leaf)
	defer server.Close()
	token, err := LoginAppRole(context.Background(), appRoleConfigWithCA(t, server.URL+"/bao", authority))
	if err != nil {
		t.Fatalf("an endpoint issued by the trusted authority was refused: %v", err)
	}
	environment, err := token.TerraformEnvironment()
	if err != nil || len(environment) != 1 || environment[0] != "VAULT_TOKEN=hvs.test-token-123456" {
		t.Fatalf("Terraform token environment = %v, error = %v", environment, err)
	}
	token.Clear()
}

// An authority that is perfectly well formed but issued nothing on this
// endpoint is still not trust for it: the bundle is the whole answer, with no
// system roots behind it.
func TestLoginAppRoleRefusesAnEndpointTheBundleDidNotIssue(t *testing.T) {
	_, leaf := vaultChain(t)
	server := vaultServer(t, leaf)
	defer server.Close()
	stranger := authorityPEM(t, caTemplateAt(26, time.Now().Add(-time.Hour), time.Now().Add(24*time.Hour)))
	if _, err := LoginAppRole(context.Background(), appRoleConfigWithCA(t, server.URL+"/bao", stranger)); err == nil {
		t.Fatal("an endpoint no authority in the bundle issued was trusted")
	}
}

// vaultChain mints an authority and a server certificate it signed for the
// loopback address, which is what a real Vault deployment presents.
func vaultChain(t *testing.T) (authorityPEMBytes []byte, leaf tls.Certificate) {
	t.Helper()
	now := time.Now()
	authorityKey, err := ecdsa.GenerateKey(elliptic.P256(), rand.Reader)
	if err != nil {
		t.Fatal(err)
	}
	template := caTemplateAt(27, now.Add(-time.Hour), now.Add(24*time.Hour))
	template.Subject = pkix.Name{CommonName: "vault CA"}
	authorityDER, err := x509.CreateCertificate(rand.Reader, template, template, &authorityKey.PublicKey, authorityKey)
	if err != nil {
		t.Fatal(err)
	}
	parent, err := x509.ParseCertificate(authorityDER)
	if err != nil {
		t.Fatal(err)
	}
	leafKey, err := ecdsa.GenerateKey(elliptic.P256(), rand.Reader)
	if err != nil {
		t.Fatal(err)
	}
	leafDER, err := x509.CreateCertificate(rand.Reader, &x509.Certificate{
		SerialNumber:          big.NewInt(28),
		Subject:               pkix.Name{CommonName: "vault.internal"},
		NotBefore:             now.Add(-time.Hour),
		NotAfter:              now.Add(24 * time.Hour),
		KeyUsage:              x509.KeyUsageDigitalSignature,
		ExtKeyUsage:           []x509.ExtKeyUsage{x509.ExtKeyUsageServerAuth},
		BasicConstraintsValid: true,
		IPAddresses:           []net.IP{net.ParseIP("127.0.0.1"), net.ParseIP("::1")},
	}, parent, &leafKey.PublicKey, authorityKey)
	if err != nil {
		t.Fatal(err)
	}
	return pem.EncodeToMemory(&pem.Block{Type: "CERTIFICATE", Bytes: authorityDER}),
		tls.Certificate{Certificate: [][]byte{leafDER}, PrivateKey: leafKey}
}

// vaultServer answers the AppRole login the way OpenBao does, under a
// certificate the test controls.
func vaultServer(t *testing.T, leaf tls.Certificate) *httptest.Server {
	t.Helper()
	server := httptest.NewUnstartedServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if r.URL.Path != "/bao/v1/auth/approle/login" {
			http.NotFound(w, r)
			return
		}
		_, _ = w.Write([]byte(`{"auth":{"client_token":"hvs.test-token-123456","lease_duration":300,"renewable":true,"token_policies":["ra8ci"]}}`))
	}))
	server.TLS = &tls.Config{Certificates: []tls.Certificate{leaf}, MinVersion: tls.VersionTLS12}
	server.StartTLS()
	return server
}
