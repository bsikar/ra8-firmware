// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package provision

import (
	"crypto/ecdsa"
	"crypto/elliptic"
	"crypto/rand"
	"crypto/x509"
	"crypto/x509/pkix"
	"encoding/pem"
	"math/big"
	"os"
	"strings"
	"testing"
	"time"
)

// authorityPEM mints a self-signed certificate from a template the caller
// shaped, which is how a CA bundle that parses but cannot authenticate anyone
// is built.
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

func caTemplateAt(serial int64, notBefore, notAfter time.Time) *x509.Certificate {
	return &x509.Certificate{
		SerialNumber:          big.NewInt(serial),
		Subject:               pkix.Name{CommonName: "ra8ci state CA"},
		NotBefore:             notBefore,
		NotAfter:              notAfter,
		IsCA:                  true,
		BasicConstraintsValid: true,
		KeyUsage:              x509.KeyUsageCertSign | x509.KeyUsageDigitalSignature,
	}
}

func TestTerraformBackendRefusesAServerCABundleThatAuthenticatesNobody(t *testing.T) {
	now := time.Now()
	for name, bundle := range map[string][]byte{
		"not an authority": authorityPEM(t, &x509.Certificate{
			SerialNumber: big.NewInt(9),
			Subject:      pkix.Name{CommonName: "ra8ci state server"},
			NotBefore:    now.Add(-time.Hour),
			NotAfter:     now.Add(time.Hour),
			KeyUsage:     x509.KeyUsageDigitalSignature,
			ExtKeyUsage:  []x509.ExtKeyUsage{x509.ExtKeyUsageServerAuth},
		}),
		"may not sign": authorityPEM(t, &x509.Certificate{
			SerialNumber:          big.NewInt(10),
			Subject:               pkix.Name{CommonName: "ra8ci state CA"},
			NotBefore:             now.Add(-time.Hour),
			NotAfter:              now.Add(time.Hour),
			IsCA:                  true,
			BasicConstraintsValid: true,
			KeyUsage:              x509.KeyUsageDigitalSignature,
		}),
		"every authority expired": authorityPEM(t, caTemplateAt(11, now.Add(-48*time.Hour), now.Add(-time.Hour))),
		"no certificate in it":    []byte("-----BEGIN PRIVATE KEY-----\nZm9v\n-----END PRIVATE KEY-----\n"),
	} {
		t.Run(name, func(t *testing.T) {
			config := testHTTPBackendConfig(t)
			if err := os.WriteFile(config.ServerCABundleFile, bundle, 0o644); err != nil {
				t.Fatal(err)
			}
			_, err := HTTPBackendEnvironment(config)
			if err == nil {
				t.Fatal("a CA bundle that can authenticate nobody was accepted")
			}
			if !strings.Contains(err.Error(), "Terraform server CA bundle") {
				t.Fatalf("refusal does not name the bundle an operator must fix: %v", err)
			}
		})
	}
}

// A rotation puts the outgoing authority beside the incoming one. The bundle
// is usable while at least one of them is, which is the rule the listener and
// every other client already read.
func TestTerraformBackendAcceptsARotatingServerCABundle(t *testing.T) {
	now := time.Now()
	config := testHTTPBackendConfig(t)
	existing, err := os.ReadFile(config.ServerCABundleFile)
	if err != nil {
		t.Fatal(err)
	}
	retired := authorityPEM(t, caTemplateAt(12, now.Add(-72*time.Hour), now.Add(-time.Hour)))
	if err := os.WriteFile(config.ServerCABundleFile, append(retired, existing...), 0o644); err != nil {
		t.Fatal(err)
	}
	if _, err := HTTPBackendEnvironment(config); err != nil {
		t.Fatalf("rotation bundle was refused: %v", err)
	}
}

// The bundle still reaches Terraform verbatim: the check narrows what may be
// configured, it does not rewrite what the child process is handed.
func TestTerraformBackendPassesTheCheckedBundleThrough(t *testing.T) {
	config := testHTTPBackendConfig(t)
	bundle, err := os.ReadFile(config.ServerCABundleFile)
	if err != nil {
		t.Fatal(err)
	}
	environment, err := HTTPBackendEnvironment(config)
	if err != nil {
		t.Fatal(err)
	}
	want := "TF_HTTP_CLIENT_CA_CERTIFICATE_PEM=" + string(bundle)
	for _, value := range environment {
		if value == want {
			return
		}
	}
	t.Fatal("checked CA bundle did not reach the Terraform environment unchanged")
}
