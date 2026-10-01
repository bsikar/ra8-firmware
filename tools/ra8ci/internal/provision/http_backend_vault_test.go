// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package provision

import "testing"

func TestOverlayEnvironmentScrubsInheritedVaultCredentials(t *testing.T) {
	result, err := OverlayEnvironment(
		[]string{
			"PATH=/usr/bin",
			"VAULT_ADDR=https://attacker.invalid",
			"VAULT_TOKEN=attacker-token-value",
			"VAULT_SKIP_VERIFY=true",
		},
		[]string{"VAULT_TOKEN=trusted-token-value-123456"},
	)
	if err != nil {
		t.Fatal(err)
	}
	values := environmentMap(t, result)
	if values["VAULT_TOKEN"] != "trusted-token-value-123456" {
		t.Fatalf("trusted Vault token missing: %+v", values)
	}
	for _, name := range []string{"VAULT_ADDR", "VAULT_SKIP_VERIFY"} {
		if _, exists := values[name]; exists {
			t.Fatalf("inherited Vault setting %s survived scrub", name)
		}
	}
}

func TestTerraformBaseEnvironmentDoesNotInheritServiceSecrets(t *testing.T) {
	values := environmentMap(t, terraformBaseEnvironment("/private/runner/one"))
	want := map[string]string{
		"PATH": "/usr/bin:/bin", "HOME": "/private/runner/one",
		"LANG": "C.UTF-8", "TF_IN_AUTOMATION": "1",
	}
	if len(values) != len(want) {
		t.Fatalf("Terraform base environment has unexpected entries: %+v", values)
	}
	for name, expected := range want {
		if values[name] != expected {
			t.Fatalf("Terraform environment %s = %q, want %q", name, values[name], expected)
		}
	}
}
