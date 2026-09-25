// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

// Package provision contains the trusted Terraform/Ansible boundary used by
// the control plane. It never accepts backend configuration from a job.
package provision

import (
	"bytes"
	"context"
	"crypto/tls"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"net"
	"net/http"
	"net/netip"
	"net/url"
	"path"
	"regexp"
	"strings"
	"time"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/mtls"
)

const maxAppRoleResponseBytes = 64 << 10

var appRoleIDPattern = regexp.MustCompile(`^[A-Za-z0-9._-]{1,256}$`)
var appRoleSecretPattern = regexp.MustCompile(`^[A-Za-z0-9._-]{16,1000}$`)
var appRoleMountPattern = regexp.MustCompile(`^auth/[A-Za-z0-9_-]+(?:/[A-Za-z0-9_-]+)*$`)

// AppRoleConfig contains operator-owned locations and the pinned TLS trust
// used to obtain a short-lived OpenBao/Vault token for one Terraform process.
type AppRoleConfig struct {
	Address      string
	AuthMount    string
	RoleIDFile   string
	SecretIDFile string
	CAFile       string
	Timeout      time.Duration
}

// AppRoleToken owns a short-lived token and never prints its value.
type AppRoleToken struct {
	value     []byte
	client    *http.Client
	revokeURL string
}

type appRoleLoginRequest struct {
	RoleID   string `json:"role_id"`
	SecretID string `json:"secret_id"`
}

type appRoleLoginResponse struct {
	Auth *struct {
		ClientToken   string `json:"client_token"`
		LeaseDuration int    `json:"lease_duration"`
		Renewable     bool   `json:"renewable"`
	} `json:"auth"`
}

// LoginAppRole uses protected files for role credentials and returns only the
// bounded-lived token. Terraform receives the token through its environment.
func LoginAppRole(ctx context.Context, config AppRoleConfig) (*AppRoleToken, error) {
	if ctx == nil || strings.TrimSpace(config.Address) != config.Address ||
		config.RoleIDFile == "" || config.SecretIDFile == "" || config.CAFile == "" ||
		!appRoleMountPattern.MatchString(config.AuthMount) {
		return nil, errors.New("invalid AppRole configuration")
	}
	address, err := url.Parse(config.Address)
	if err != nil || address.Scheme != "https" || address.Hostname() == "" ||
		address.Port() == "" || address.User != nil || address.RawQuery != "" ||
		address.Fragment != "" || personalAppRoleHost(address.Hostname()) {
		return nil, errors.New("AppRole endpoint must be an approved HTTPS origin")
	}
	timeout := config.Timeout
	if timeout == 0 {
		timeout = 10 * time.Second
	}
	if timeout < time.Millisecond || timeout > 30*time.Second {
		return nil, errors.New("AppRole timeout is outside bounded policy")
	}
	roleBytes, err := readRegularFile(config.RoleIDFile, 256, true)
	if err != nil {
		return nil, fmt.Errorf("read AppRole role ID: %w", err)
	}
	defer clear(roleBytes)
	secretBytes, err := readRegularFile(config.SecretIDFile, 4096, true)
	if err != nil {
		return nil, fmt.Errorf("read AppRole secret ID: %w", err)
	}
	defer clear(secretBytes)
	roleID := strings.TrimSpace(string(roleBytes))
	secretID := strings.TrimSpace(string(secretBytes))
	if !appRoleIDPattern.MatchString(roleID) || !appRoleSecretPattern.MatchString(secretID) {
		return nil, errors.New("AppRole credential file has invalid contents")
	}
	caPEM, err := readRegularFile(config.CAFile, maxServerCABundleBytes, false)
	if err != nil {
		return nil, fmt.Errorf("read AppRole CA bundle: %w", err)
	}
	defer clear(caPEM)
	roots, err := mtls.ServerAuthorities(caPEM, time.Now())
	if err != nil {
		return nil, fmt.Errorf("AppRole CA bundle cannot authenticate the Vault endpoint: %w", err)
	}
	transport := &http.Transport{
		Proxy:               nil,
		TLSClientConfig:     &tls.Config{RootCAs: roots, MinVersion: tls.VersionTLS12},
		DialContext:         (&net.Dialer{Timeout: timeout, KeepAlive: 30 * time.Second}).DialContext,
		TLSHandshakeTimeout: timeout, ResponseHeaderTimeout: timeout,
	}
	client := &http.Client{
		Transport: transport, Timeout: timeout,
		CheckRedirect: func(*http.Request, []*http.Request) error { return http.ErrUseLastResponse },
	}
	loginURL := *address
	loginURL.Path = path.Join(strings.TrimRight(address.Path, "/"), "v1", config.AuthMount, "login")
	payload, err := json.Marshal(appRoleLoginRequest{RoleID: roleID, SecretID: secretID})
	if err != nil {
		return nil, errors.New("encode AppRole login request")
	}
	request, err := http.NewRequestWithContext(ctx, http.MethodPost, loginURL.String(), bytes.NewReader(payload))
	defer clear(payload)
	if err != nil {
		return nil, errors.New("create AppRole login request")
	}
	request.Header.Set("Content-Type", "application/json")
	response, err := client.Do(request)
	if err != nil {
		return nil, errors.New("AppRole login request failed")
	}
	defer response.Body.Close()
	body, err := io.ReadAll(io.LimitReader(response.Body, maxAppRoleResponseBytes+1))
	if err != nil || len(body) > maxAppRoleResponseBytes {
		clear(body)
		return nil, errors.New("AppRole login response is unreadable or oversized")
	}
	defer clear(body)
	if response.StatusCode != http.StatusOK {
		return nil, fmt.Errorf("AppRole login rejected with HTTP status %d", response.StatusCode)
	}
	var decoded appRoleLoginResponse
	decoder := json.NewDecoder(bytes.NewReader(body))
	if err := decoder.Decode(&decoded); err != nil {
		return nil, errors.New("AppRole login response is malformed")
	}
	var trailing any
	if err := decoder.Decode(&trailing); err != io.EOF {
		return nil, errors.New("AppRole login response has trailing data")
	}
	if decoded.Auth == nil || !vaultTokenPattern.MatchString(decoded.Auth.ClientToken) ||
		decoded.Auth.LeaseDuration < 1 || decoded.Auth.LeaseDuration > 3600 {
		return nil, errors.New("AppRole login returned an invalid or overlong token")
	}
	revokeURL := *address
	revokeURL.Path = path.Join(strings.TrimRight(address.Path, "/"), "v1", "auth", "token", "revoke-self")
	return &AppRoleToken{value: []byte(decoded.Auth.ClientToken), client: client,
		revokeURL: revokeURL.String()}, nil
}

func personalAppRoleHost(host string) bool {
	if strings.HasSuffix(strings.ToLower(host), ".ts.net") {
		return true
	}
	address, err := netip.ParseAddr(host)
	return err == nil && netip.MustParsePrefix("100.64.0.0/10").Contains(address.Unmap())
}

// TerraformEnvironment returns a single process environment entry; callers
// must clear the token immediately after the Terraform child process exits.
func (token *AppRoleToken) TerraformEnvironment() ([]string, error) {
	if token == nil || token.client == nil || len(token.value) < 16 ||
		!vaultTokenPattern.Match(token.value) {
		return nil, errors.New("AppRole token is unavailable")
	}
	return []string{"VAULT_TOKEN=" + string(token.value)}, nil
}

// Revoke asks Vault to revoke this token. Callers should still Clear even when
// revocation fails because token TTL remains the independent expiration fence.
func (token *AppRoleToken) Revoke(ctx context.Context) error {
	if token == nil || token.client == nil || ctx == nil || len(token.value) < 16 {
		return errors.New("AppRole token is unavailable")
	}
	request, err := http.NewRequestWithContext(ctx, http.MethodPost, token.revokeURL, strings.NewReader("{}"))
	if err != nil {
		return errors.New("create AppRole revoke request")
	}
	request.Header.Set("X-Vault-Token", string(token.value))
	request.Header.Set("Content-Type", "application/json")
	response, err := token.client.Do(request)
	if err != nil {
		return errors.New("AppRole token revocation failed")
	}
	defer response.Body.Close()
	_, _ = io.Copy(io.Discard, io.LimitReader(response.Body, 4096))
	if response.StatusCode < 200 || response.StatusCode >= 300 {
		return fmt.Errorf("AppRole token revocation returned HTTP status %d", response.StatusCode)
	}
	return nil
}

// Clear overwrites the in-memory token after the provisioner has finished.
func (token *AppRoleToken) Clear() {
	if token != nil {
		clear(token.value)
		token.value = nil
	}
}

// String deliberately redacts the token from logs and formatted errors.
func (token *AppRoleToken) String() string {
	if token == nil {
		return "AppRoleToken{nil}"
	}
	return "AppRoleToken{value:[REDACTED]}"
}
