// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package provision

import (
	"context"
	"crypto/x509"
	"encoding/json"
	"encoding/pem"
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"
)

func TestLoginAppRoleUsesProtectedCredentialsAndRevokesShortLivedToken(t *testing.T) {
	var revoked bool
	server := httptest.NewTLSServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		switch r.URL.Path {
		case "/bao/v1/auth/approle/login":
			if r.Method != http.MethodPost {
				t.Errorf("login method = %s", r.Method)
			}
			var request appRoleLoginRequest
			if err := json.NewDecoder(r.Body).Decode(&request); err != nil {
				t.Errorf("decode login payload: %v", err)
			}
			if request.RoleID != "role-id-1234567890" || request.SecretID != "secret-id-12345678901234567890" {
				t.Errorf("unexpected login credentials")
			}
			_, _ = w.Write([]byte(`{"auth":{"client_token":"hvs.test-token-123456","lease_duration":300,"renewable":true,"token_policies":["ra8ci"]}}`))
		case "/bao/v1/auth/token/revoke-self":
			if r.Method != http.MethodPost || r.Header.Get("X-Vault-Token") != "hvs.test-token-123456" {
				t.Errorf("invalid revoke request")
			}
			revoked = true
			w.WriteHeader(http.StatusNoContent)
		default:
			http.NotFound(w, r)
		}
	}))
	defer server.Close()

	directory := t.TempDir()
	roleFile := filepath.Join(directory, "role-id")
	secretFile := filepath.Join(directory, "secret-id")
	caFile := filepath.Join(directory, "ca.pem")
	writePrivate := func(file, value string) {
		t.Helper()
		if err := os.WriteFile(file, []byte(value), 0o600); err != nil {
			t.Fatal(err)
		}
	}
	writePrivate(roleFile, "role-id-1234567890\n")
	writePrivate(secretFile, "secret-id-12345678901234567890\n")
	ca := pem.EncodeToMemory(&pem.Block{Type: "CERTIFICATE", Bytes: server.Certificate().Raw})
	if err := os.WriteFile(caFile, ca, 0o600); err != nil {
		t.Fatal(err)
	}

	token, err := LoginAppRole(context.Background(), AppRoleConfig{
		Address: server.URL + "/bao", AuthMount: "auth/approle",
		RoleIDFile: roleFile, SecretIDFile: secretFile, CAFile: caFile,
		Timeout: time.Second,
	})
	if err != nil {
		t.Fatal(err)
	}
	environment, err := token.TerraformEnvironment()
	if err != nil || len(environment) != 1 || environment[0] != "VAULT_TOKEN=hvs.test-token-123456" {
		t.Fatalf("Terraform token environment = %v, error = %v", environment, err)
	}
	if strings.Contains(token.String(), "hvs.test-token-123456") {
		t.Fatal("token string leaked its value")
	}
	if err := token.Revoke(context.Background()); err != nil {
		t.Fatal(err)
	}
	if !revoked {
		t.Fatal("token was not revoked")
	}
	token.Clear()
	if _, err := token.TerraformEnvironment(); err == nil {
		t.Fatal("cleared token remained usable")
	}
}

func TestLoginAppRoleRejectsInvalidConfigurationAndLongLease(t *testing.T) {
	if _, err := LoginAppRole(context.Background(), AppRoleConfig{}); err == nil {
		t.Fatal("accepted empty AppRole configuration")
	}
	server := httptest.NewTLSServer(http.HandlerFunc(func(w http.ResponseWriter, _ *http.Request) {
		_, _ = w.Write([]byte(`{"auth":{"client_token":"hvs.test-token-123456","lease_duration":3601}}`))
	}))
	defer server.Close()
	directory := t.TempDir()
	writeFile := func(name, value string, mode os.FileMode) string {
		t.Helper()
		file := filepath.Join(directory, name)
		if err := os.WriteFile(file, []byte(value), mode); err != nil {
			t.Fatal(err)
		}
		return file
	}
	roleFile := writeFile("role", "role-id-1234567890", 0o600)
	secretFile := writeFile("secret", "secret-id-12345678901234567890", 0o600)
	ca := pem.EncodeToMemory(&pem.Block{Type: "CERTIFICATE", Bytes: server.Certificate().Raw})
	caFile := writeFile("ca", string(ca), 0o600)
	if _, err := x509.ParseCertificate(server.Certificate().Raw); err != nil {
		t.Fatal(err)
	}
	if _, err := LoginAppRole(context.Background(), AppRoleConfig{
		Address: server.URL, AuthMount: "auth/approle", RoleIDFile: roleFile,
		SecretIDFile: secretFile, CAFile: caFile, Timeout: time.Second,
	}); err == nil {
		t.Fatal("accepted token with a lease exceeding one hour")
	}
}
