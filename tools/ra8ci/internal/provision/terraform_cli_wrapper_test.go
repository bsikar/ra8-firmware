// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package provision

import (
	"path/filepath"
	"strings"
	"testing"
)

func TestRunnerTerraformCommandsUseTheOpenBaoWrapper(t *testing.T) {
	config := TerraformConfig{
		BinaryPath:           filepath.Join("/private", "toolchain", "tofu"),
		CommandWrapper:       filepath.Join("/repo", "infra", "terraform", "run-with-openbao.sh"),
		WrapperHome:          filepath.Join("/Users", "operator"),
		EnvironmentDirectory: filepath.Join("/repo", "infra", "terraform", "environments", "ra8ci-runner"),
	}
	if got := terraformCommandPath(config); got != config.CommandWrapper {
		t.Fatalf("Terraform command path = %q; want the OpenBao wrapper", got)
	}
	environment := wrapperEnvironment(config, []string{"HOME=/private/session", "TF_DATA_DIR=/private/data"})
	values := make(map[string]string, len(environment))
	for _, entry := range environment {
		key, value, found := strings.Cut(entry, "=")
		if !found {
			t.Fatalf("invalid wrapper environment entry %q", key)
		}
		values[key] = value
	}
	if values["HOME"] != config.WrapperHome ||
		values["PATH"] != filepath.Dir(config.BinaryPath)+":/usr/bin:/bin" ||
		values["RA8_TOFU_ENV"] != "ra8ci-runner" ||
		values["TF_DATA_DIR"] != "/private/data" {
		t.Fatalf("wrapper environment does not select the pinned runner runtime: %#v", values)
	}
}
