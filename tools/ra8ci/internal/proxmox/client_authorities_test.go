// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package proxmox

import (
	"crypto/ecdsa"
	"crypto/elliptic"
	"crypto/rand"
	"crypto/x509"
	"crypto/x509/pkix"
	"encoding/pem"
	"errors"
	"math/big"
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"
)

// authorityPEM issues a self-signed certificate from template and returns it
// as a PEM block, so each test states exactly the one property it is about.
func authorityPEM(t *testing.T, template *x509.Certificate) []byte {
	t.Helper()
	key, err := ecdsa.GenerateKey(elliptic.P256(), rand.Reader)
	if err != nil {
		t.Fatal(err)
	}
	der, err := x509.CreateCertificate(rand.Reader, template, template, &key.PublicKey, key)
	if err != nil {
		t.Fatal(err)
	}
	return pem.EncodeToMemory(&pem.Block{Type: "CERTIFICATE", Bytes: der})
}

// authorityTemplate is a Proxmox cluster authority as pve-root-ca.pem is one:
// a signing certificate inside its validity window.
func authorityTemplate(name string) *x509.Certificate {
	return &x509.Certificate{
		SerialNumber:          big.NewInt(time.Now().UnixNano()),
		Subject:               pkix.Name{CommonName: name},
		NotBefore:             time.Now().Add(-time.Hour),
		NotAfter:              time.Now().Add(24 * time.Hour),
		KeyUsage:              x509.KeyUsageCertSign | x509.KeyUsageDigitalSignature,
		BasicConstraintsValid: true,
		IsCA:                  true,
	}
}

// clientWithCA states every other part of a reviewed configuration and varies
// only the trust bundle, so a refusal can only be about the bundle.
func clientWithCA(t *testing.T, bundle []byte) error {
	t.Helper()
	dir := t.TempDir()
	ca := filepath.Join(dir, "ca.pem")
	if err := os.WriteFile(ca, bundle, 0600); err != nil {
		t.Fatal(err)
	}
	token := filepath.Join(dir, "token")
	if err := os.WriteFile(token, []byte("ra8ci@pve!client=secret-token\n"), 0600); err != nil {
		t.Fatal(err)
	}
	_, err := New(Config{
		Endpoint: "https://pve.lab.example:8006", CAFile: ca, TokenFile: token,
		Node: "pve", Pool: "ra8-tf-lab", Storage: "ra8-tf-lab",
		AllowedVMIDs: []int{9000}, TemplateVMIDs: []int{9001}, Bridges: []string{"vmbr8"},
		RequestTimeout: time.Second, OperationTimeout: time.Second, TaskPollInterval: time.Millisecond,
	})
	return err
}

func TestAProxmoxAuthorityBundleIsAccepted(t *testing.T) {
	if err := clientWithCA(t, authorityPEM(t, authorityTemplate("pve-root-ca"))); err != nil {
		t.Fatalf("reviewed Proxmox authority refused: %v", err)
	}
}

func TestARetiringProxmoxAuthorityStaysAcceptableBesideALiveOne(t *testing.T) {
	retired := authorityTemplate("pve-root-ca-old")
	retired.NotBefore = time.Now().Add(-48 * time.Hour)
	retired.NotAfter = time.Now().Add(-time.Hour)
	bundle := append(authorityPEM(t, retired), authorityPEM(t, authorityTemplate("pve-root-ca"))...)
	if err := clientWithCA(t, bundle); err != nil {
		t.Fatalf("rotation bundle refused: %v", err)
	}
}

func TestAnEmptyProxmoxCAFileIsRefused(t *testing.T) {
	if err := clientWithCA(t, nil); !errors.Is(err, ErrInvalid) {
		t.Fatalf("empty CA file accepted: %v", err)
	}
}

func TestProxmoxCAFileWithoutACertificateIsRefused(t *testing.T) {
	err := clientWithCA(t, []byte("# the operator pasted the token here by mistake\n"))
	if !errors.Is(err, ErrInvalid) {
		t.Fatalf("CA file holding no certificate accepted: %v", err)
	}
}

func TestAnExpiredProxmoxAuthorityAloneIsRefused(t *testing.T) {
	expired := authorityTemplate("pve-root-ca")
	expired.NotBefore = time.Now().Add(-48 * time.Hour)
	expired.NotAfter = time.Now().Add(-time.Hour)
	err := clientWithCA(t, authorityPEM(t, expired))
	if !errors.Is(err, ErrInvalid) {
		t.Fatalf("bundle of expired authorities accepted: %v", err)
	}
	if !strings.Contains(err.Error(), "validity window") {
		t.Fatalf("refusal does not name the expiry: %v", err)
	}
}

func TestAProxmoxAuthorityThatMayNotSignIsRefused(t *testing.T) {
	unable := authorityTemplate("pve-root-ca")
	unable.KeyUsage = x509.KeyUsageDigitalSignature
	err := clientWithCA(t, authorityPEM(t, unable))
	if !errors.Is(err, ErrInvalid) {
		t.Fatalf("authority that may not sign accepted: %v", err)
	}
	if !strings.Contains(err.Error(), "may not sign") {
		t.Fatalf("refusal does not name the key usage: %v", err)
	}
}

func TestTheProxmoxAPIsOwnCertificateIsNotAnAuthority(t *testing.T) {
	leaf := authorityTemplate("pve.lab.example")
	leaf.IsCA = false
	leaf.KeyUsage = x509.KeyUsageDigitalSignature
	leaf.ExtKeyUsage = []x509.ExtKeyUsage{x509.ExtKeyUsageServerAuth}
	leaf.DNSNames = []string{"pve.lab.example"}
	err := clientWithCA(t, authorityPEM(t, leaf))
	if !errors.Is(err, ErrInvalid) {
		t.Fatalf("end-entity certificate accepted as an authority: %v", err)
	}
	if !strings.Contains(err.Error(), "not a certificate authority") {
		t.Fatalf("refusal does not name what the file holds: %v", err)
	}
}

func TestAnUnparseableProxmoxAuthorityIsRefused(t *testing.T) {
	bundle := pem.EncodeToMemory(&pem.Block{Type: "CERTIFICATE", Bytes: []byte("not a certificate")})
	if err := clientWithCA(t, bundle); !errors.Is(err, ErrInvalid) {
		t.Fatalf("unparseable certificate accepted: %v", err)
	}
}

func TestAProxmoxCARefusalNamesTheFileItIsAbout(t *testing.T) {
	leaf := authorityTemplate("pve.lab.example")
	leaf.IsCA = false
	err := clientWithCA(t, authorityPEM(t, leaf))
	if err == nil || !strings.Contains(err.Error(), "configured Proxmox API CA") {
		t.Fatalf("refusal does not name the Proxmox API CA: %v", err)
	}
}
