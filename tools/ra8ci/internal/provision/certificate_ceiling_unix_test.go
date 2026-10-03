//go:build unix

// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package provision

import (
	"os"
	"strings"
	"testing"
)

func TestCredentialCertificateAtTheCeilingIsRead(t *testing.T) {
	config := shapedBackendConfig(t, nil)
	body := []byte(strings.Repeat("a", maxClientCertificateBytes))
	if err := os.WriteFile(config.ClientCertificateFile, body, 0o644); err != nil {
		t.Fatal(err)
	}
	err := refusedBackend(t, config)
	if err.Error() != "Terraform client certificate and private key do not match" {
		t.Fatalf("a certificate at the ceiling was not read: %q", err.Error())
	}
}
