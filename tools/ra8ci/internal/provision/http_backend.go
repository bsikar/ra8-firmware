// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

// Package provision contains the trusted Terraform/Ansible boundary used by
// the control plane. It never accepts backend configuration from a job.
package provision

import (
	"crypto/tls"
	"errors"
	"fmt"
	"io"
	"net/netip"
	"net/url"
	"os"
	"path"
	"regexp"
	"sort"
	"strings"
	"time"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/mtls"
	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/store"
)

const (
	maxClientCertificateBytes = 128 << 10
	maxClientKeyBytes         = 64 << 10
	maxServerCABundleBytes    = 1 << 20
)

var envNamePattern = regexp.MustCompile(`^[A-Za-z_][A-Za-z0-9_]*$`)
var vaultTokenPattern = regexp.MustCompile(`^[A-Za-z0-9._-]{16,1000}$`)

// HTTPBackendConfig is supplied only by server configuration. Its client
// certificate must have a narrowly scoped terraform_state grant.
type HTTPBackendConfig struct {
	ServerURL             string
	ReservationID         string
	ClientCertificateFile string
	ClientPrivateKeyFile  string
	ServerCABundleFile    string
}

// HTTPBackendEnvironment constructs the supported Terraform HTTP backend
// variables for one reservation. Sensitive PEM values stay out of backend
// configuration, plan files, and command arguments.
func HTTPBackendEnvironment(config HTTPBackendConfig) ([]string, error) {
	if strings.TrimSpace(config.ServerURL) != config.ServerURL || config.ServerURL == "" ||
		!store.ValidID(config.ReservationID) || config.ClientCertificateFile == "" ||
		config.ClientPrivateKeyFile == "" || config.ServerCABundleFile == "" {
		return nil, errors.New("invalid Terraform HTTP backend configuration")
	}
	serverURL, err := url.Parse(config.ServerURL)
	if err != nil || serverURL.Scheme != "https" || serverURL.Hostname() == "" ||
		serverURL.Port() == "" || serverURL.User != nil || serverURL.RawQuery != "" ||
		serverURL.Fragment != "" || (serverURL.Path != "" &&
		path.Clean(serverURL.Path) != strings.TrimRight(serverURL.Path, "/")) {
		return nil, errors.New("Terraform state backend requires an explicit HTTPS origin")
	}
	if personalNetworkHost(serverURL.Hostname()) {
		return nil, errors.New("personal-network Terraform endpoint is prohibited")
	}
	endpoint := *serverURL
	endpoint.Path = path.Join(strings.TrimRight(serverURL.Path, "/"),
		"v1", "terraform", "runner-states", config.ReservationID)
	stateURL := endpoint.String()

	certificatePEM, err := readRegularFile(config.ClientCertificateFile, maxClientCertificateBytes, false)
	if err != nil {
		return nil, fmt.Errorf("read Terraform client certificate: %w", err)
	}
	defer clear(certificatePEM)
	privateKeyPEM, err := readRegularFile(config.ClientPrivateKeyFile, maxClientKeyBytes, true)
	if err != nil {
		return nil, fmt.Errorf("read Terraform client private key: %w", err)
	}
	defer clear(privateKeyPEM)
	clientPair, err := tls.X509KeyPair(certificatePEM, privateKeyPEM)
	if err != nil || len(clientPair.Certificate) == 0 || clientPair.PrivateKey == nil {
		return nil, errors.New("Terraform client certificate and private key do not match")
	}
	if err := checkTerraformStateClientIdentity(clientPair, time.Now()); err != nil {
		return nil, err
	}

	caPEM, err := readRegularFile(config.ServerCABundleFile, maxServerCABundleBytes, false)
	if err != nil {
		return nil, fmt.Errorf("read Terraform server CA bundle: %w", err)
	}
	defer clear(caPEM)
	if _, err := mtls.ServerAuthorities(caPEM, time.Now()); err != nil {
		return nil, fmt.Errorf("Terraform server CA bundle cannot authenticate the state server: %w", err)
	}

	return []string{
		"TF_HTTP_ADDRESS=" + stateURL,
		"TF_HTTP_UPDATE_METHOD=POST",
		"TF_HTTP_LOCK_ADDRESS=" + stateURL,
		"TF_HTTP_LOCK_METHOD=LOCK",
		"TF_HTTP_UNLOCK_ADDRESS=" + stateURL,
		"TF_HTTP_UNLOCK_METHOD=UNLOCK",
		"TF_HTTP_CLIENT_CERTIFICATE_PEM=" + string(certificatePEM),
		"TF_HTTP_CLIENT_PRIVATE_KEY_PEM=" + string(privateKeyPEM),
		"TF_HTTP_CLIENT_CA_CERTIFICATE_PEM=" + string(caPEM),
		"TF_HTTP_RETRY_MAX=1",
		"TF_IN_AUTOMATION=1",
	}, nil
}

// OverlayEnvironment strips inherited Terraform control, backend, and input
// overrides. It preserves only fixed Terraform data/cache paths and automation
// mode, then adds validated backend values.
func OverlayEnvironment(base, overlay []string) ([]string, error) {
	values := make(map[string]string, len(base)+len(overlay))
	baseSeen := make(map[string]struct{}, len(base))
	for _, entry := range base {
		key, value, found := strings.Cut(entry, "=")
		if !found || !envNamePattern.MatchString(key) {
			return nil, errors.New("invalid base environment entry")
		}
		if _, exists := baseSeen[key]; exists {
			return nil, errors.New("duplicate base environment variable")
		}
		baseSeen[key] = struct{}{}
		if strings.HasPrefix(key, "TF_") && key != "TF_DATA_DIR" &&
			key != "TF_PLUGIN_CACHE_DIR" && key != "TF_IN_AUTOMATION" {
			continue
		}
		if strings.HasPrefix(key, "VAULT_") {
			continue
		}
		values[key] = value
	}
	seen := make(map[string]struct{}, len(overlay))
	for _, entry := range overlay {
		key, value, found := strings.Cut(entry, "=")
		allowed := strings.HasPrefix(key, "TF_HTTP_") || key == "TF_IN_AUTOMATION" || key == "VAULT_TOKEN"
		if !found || !envNamePattern.MatchString(key) || !allowed {
			return nil, errors.New("invalid Terraform backend environment entry")
		}
		if key == "VAULT_TOKEN" && !vaultTokenPattern.MatchString(value) {
			return nil, errors.New("invalid short-lived Vault token")
		}
		if _, exists := seen[key]; exists {
			return nil, errors.New("duplicate Terraform backend environment variable")
		}
		seen[key] = struct{}{}
		values[key] = value
	}
	result := make([]string, 0, len(values))
	for key, value := range values {
		result = append(result, key+"="+value)
	}
	sort.Strings(result)
	return result, nil
}

func readRegularFile(file string, limit int64, private bool) ([]byte, error) {
	info, err := os.Lstat(file)
	if err != nil || !info.Mode().IsRegular() || info.Size() < 1 || info.Size() > limit {
		return nil, errors.New("credential must be a bounded regular file")
	}
	if private && info.Mode().Perm()&0o077 != 0 {
		return nil, errors.New("private key file must not be accessible by group or others")
	}
	handle, err := os.Open(file)
	if err != nil {
		return nil, errors.New("credential file is unreadable")
	}
	defer handle.Close()
	opened, err := handle.Stat()
	if err != nil || !opened.Mode().IsRegular() || !os.SameFile(info, opened) ||
		opened.Size() < 1 || opened.Size() > limit ||
		(private && opened.Mode().Perm()&0o077 != 0) {
		return nil, errors.New("credential file changed or violates file policy")
	}
	content, err := io.ReadAll(io.LimitReader(handle, limit+1))
	if err != nil || int64(len(content)) != opened.Size() {
		clear(content)
		return nil, errors.New("credential file read was incomplete or changed")
	}
	return content, nil
}

func personalNetworkHost(host string) bool {
	if strings.HasSuffix(strings.ToLower(host), ".ts.net") {
		return true
	}
	address, err := netip.ParseAddr(host)
	return err == nil && netip.MustParsePrefix("100.64.0.0/10").Contains(address.Unmap())
}
