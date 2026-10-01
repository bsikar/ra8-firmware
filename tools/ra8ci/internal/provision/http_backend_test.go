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
	"path/filepath"
	"strings"
	"testing"
	"time"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/store"
)

func testHTTPBackendConfig(t *testing.T) HTTPBackendConfig {
	t.Helper()
	directory := t.TempDir()
	now := time.Now()
	caKey, err := ecdsa.GenerateKey(elliptic.P256(), rand.Reader)
	if err != nil {
		t.Fatal(err)
	}
	caTemplate := &x509.Certificate{
		SerialNumber:          big.NewInt(1),
		Subject:               pkix.Name{CommonName: "ra8ci test CA"},
		NotBefore:             now.Add(-time.Hour),
		NotAfter:              now.Add(24 * time.Hour),
		IsCA:                  true,
		BasicConstraintsValid: true,
		KeyUsage:              x509.KeyUsageCertSign | x509.KeyUsageDigitalSignature,
	}
	caDER, err := x509.CreateCertificate(rand.Reader, caTemplate, caTemplate, &caKey.PublicKey, caKey)
	if err != nil {
		t.Fatal(err)
	}
	clientKey, err := ecdsa.GenerateKey(elliptic.P256(), rand.Reader)
	if err != nil {
		t.Fatal(err)
	}
	clientTemplate := &x509.Certificate{
		SerialNumber: big.NewInt(2),
		Subject:      pkix.Name{CommonName: "ra8ci-terraform-state"},
		NotBefore:    now.Add(-time.Hour),
		NotAfter:     now.Add(time.Hour),
		KeyUsage:     x509.KeyUsageDigitalSignature,
		ExtKeyUsage:  []x509.ExtKeyUsage{x509.ExtKeyUsageClientAuth},
	}
	clientDER, err := x509.CreateCertificate(rand.Reader, clientTemplate, caTemplate, &clientKey.PublicKey, caKey)
	if err != nil {
		t.Fatal(err)
	}
	keyDER, err := x509.MarshalPKCS8PrivateKey(clientKey)
	if err != nil {
		t.Fatal(err)
	}
	certPath := filepath.Join(directory, "client.pem")
	keyPath := filepath.Join(directory, "client-key.pem")
	caPath := filepath.Join(directory, "ca.pem")
	writePEM := func(file, kind string, der []byte, mode os.FileMode) {
		t.Helper()
		body := pem.EncodeToMemory(&pem.Block{Type: kind, Bytes: der})
		if err := os.WriteFile(file, body, mode); err != nil {
			t.Fatal(err)
		}
	}
	writePEM(certPath, "CERTIFICATE", clientDER, 0o644)
	writePEM(keyPath, "PRIVATE KEY", keyDER, 0o600)
	writePEM(caPath, "CERTIFICATE", caDER, 0o644)
	return HTTPBackendConfig{
		ServerURL:             "https://ra8ci.internal.example:8443/api",
		ReservationID:         mustProvisionID(t),
		ClientCertificateFile: certPath,
		ClientPrivateKeyFile:  keyPath,
		ServerCABundleFile:    caPath,
	}
}

func mustProvisionID(t *testing.T) string {
	t.Helper()
	id, err := store.NewID()
	if err != nil {
		t.Fatal(err)
	}
	return id
}

func environmentMap(t *testing.T, entries []string) map[string]string {
	t.Helper()
	result := make(map[string]string, len(entries))
	for _, entry := range entries {
		key, value, found := strings.Cut(entry, "=")
		if !found {
			t.Fatalf("invalid environment entry %q", key)
		}
		result[key] = value
	}
	return result
}

func TestHTTPBackendEnvironmentBindsReservationAndUsesMTLS(t *testing.T) {
	config := testHTTPBackendConfig(t)
	environment, err := HTTPBackendEnvironment(config)
	if err != nil {
		t.Fatal(err)
	}
	values := environmentMap(t, environment)
	wantURL := "https://ra8ci.internal.example:8443/api/v1/terraform/runner-states/" + config.ReservationID
	for _, name := range []string{"TF_HTTP_ADDRESS", "TF_HTTP_LOCK_ADDRESS", "TF_HTTP_UNLOCK_ADDRESS"} {
		if values[name] != wantURL {
			t.Fatalf("%s=%q want %q", name, values[name], wantURL)
		}
	}
	if values["TF_HTTP_LOCK_METHOD"] != "LOCK" || values["TF_HTTP_UNLOCK_METHOD"] != "UNLOCK" ||
		values["TF_HTTP_UPDATE_METHOD"] != "POST" || values["TF_HTTP_RETRY_MAX"] != "1" {
		t.Fatalf("unexpected Terraform HTTP backend methods: %+v", values)
	}
	for _, name := range []string{
		"TF_HTTP_CLIENT_CERTIFICATE_PEM",
		"TF_HTTP_CLIENT_PRIVATE_KEY_PEM",
		"TF_HTTP_CLIENT_CA_CERTIFICATE_PEM",
	} {
		if !strings.Contains(values[name], "-----BEGIN ") {
			t.Fatalf("%s is not PEM material", name)
		}
	}
	if _, exists := values["TF_HTTP_PASSWORD"]; exists {
		t.Fatal("backend unexpectedly configured basic authentication")
	}
}

func TestHTTPBackendEnvironmentRejectsUnapprovedEndpointsAndWeakKeys(t *testing.T) {
	base := testHTTPBackendConfig(t)
	for _, endpoint := range []string{
		"http://ra8ci.internal.example:8443",
		"https://ra8ci.internal.example",
		"https://user@ra8ci.internal.example:8443",
		"https://ra8ci.internal.example:8443?x=y",
		"https://ra8ci.internal.example:8443/../outside",
		"https://control.tail123.ts.net:8443",
		"https://100.64.1.2:8443",
	} {
		t.Run(endpoint, func(t *testing.T) {
			config := base
			config.ServerURL = endpoint
			if _, err := HTTPBackendEnvironment(config); err == nil {
				t.Fatalf("accepted unapproved endpoint %q", endpoint)
			}
		})
	}
	if err := os.Chmod(base.ClientPrivateKeyFile, 0o640); err != nil {
		t.Fatal(err)
	}
	if _, err := HTTPBackendEnvironment(base); err == nil {
		t.Fatal("accepted a private key accessible to group")
	}
}

func TestOverlayEnvironmentScrubsInheritedBackendConfig(t *testing.T) {
	result, err := OverlayEnvironment(
		[]string{
			"PATH=/usr/bin",
			"TF_DATA_DIR=/safe/tfdata",
			"TF_PLUGIN_CACHE_DIR=/safe/plugins",
			"TF_HTTP_ADDRESS=https://attacker.invalid/state",
			"TF_HTTP_PASSWORD=stale",
			"TF_CLI_ARGS_plan=-destroy",
			"TF_VAR_vmid=1234",
		},
		[]string{
			"TF_HTTP_ADDRESS=https://ra8ci.internal/state",
			"TF_HTTP_LOCK_METHOD=LOCK",
			"TF_IN_AUTOMATION=1",
		},
	)
	if err != nil {
		t.Fatal(err)
	}
	values := environmentMap(t, result)
	if values["PATH"] != "/usr/bin" || values["TF_DATA_DIR"] != "/safe/tfdata" ||
		values["TF_PLUGIN_CACHE_DIR"] != "/safe/plugins" ||
		values["TF_HTTP_ADDRESS"] != "https://ra8ci.internal/state" ||
		values["TF_HTTP_LOCK_METHOD"] != "LOCK" || values["TF_IN_AUTOMATION"] != "1" {
		t.Fatalf("overlay failed to retain trusted values: %+v", values)
	}
	for _, name := range []string{"TF_HTTP_PASSWORD", "TF_CLI_ARGS_plan", "TF_VAR_vmid"} {
		if _, exists := values[name]; exists {
			t.Fatalf("inherited Terraform override %s survived scrub", name)
		}
	}
	if _, err := OverlayEnvironment(nil, []string{"TF_HTTP_ADDRESS=one", "TF_HTTP_ADDRESS=two"}); err == nil {
		t.Fatal("accepted duplicate Terraform backend variables")
	}
	if _, err := OverlayEnvironment(nil, []string{"PATH=/tmp"}); err == nil {
		t.Fatal("accepted an unapproved overlay variable")
	}
	if _, err := OverlayEnvironment([]string{"PATH=/usr/bin", "PATH=/tmp"}, nil); err == nil {
		t.Fatal("accepted duplicate base environment variables")
	}
}
