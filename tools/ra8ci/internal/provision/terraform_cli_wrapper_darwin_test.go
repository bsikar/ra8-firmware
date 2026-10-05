//go:build darwin

// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package provision

import (
	"context"
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"
)

func TestRunnerRuntimeUsesTheWrapperForEveryTofuCommand(t *testing.T) {
	root := t.TempDir()
	binDir := filepath.Join(root, "toolchain")
	environment := filepath.Join(root, "environments", "ra8ci-runner")
	wrapperHome := filepath.Join(root, "operator-home")
	for _, directory := range []string{binDir, environment, wrapperHome} {
		if err := os.MkdirAll(directory, 0o700); err != nil {
			t.Fatal("prepare wrapper runtime fixture")
		}
	}
	tofuPath := filepath.Join(binDir, "tofu")
	tofuBody := "#!/bin/sh\nif [ \"$1\" = version ]; then printf '%s' '{\"terraform_version\":\"1.13.0\"}'; fi\n"
	if err := os.WriteFile(tofuPath, []byte(tofuBody), 0o700); err != nil {
		t.Fatal("write pinned tofu stand-in")
	}
	digest, err := fileSHA256(tofuPath, maxTerraformBinaryBytes)
	if err != nil {
		t.Fatal("hash pinned tofu stand-in")
	}
	wrapperPath := filepath.Join(root, "run-with-openbao.sh")
	wrapperBody := "#!/bin/sh\nprintf '%s\\n' \"$*\" >> \"$HOME/wrapper.log\"\nexec tofu \"$@\"\n"
	if err := os.WriteFile(wrapperPath, []byte(wrapperBody), 0o700); err != nil {
		t.Fatal("write wrapper stand-in")
	}
	config := TerraformConfig{BinaryPath: tofuPath, BinarySHA256: digest, Version: "1.13.0",
		CommandWrapper: wrapperPath, WrapperHome: wrapperHome, EnvironmentDirectory: environment,
		StateDirectory: filepath.Join(root, "state"), PluginCacheDirectory: filepath.Join(root, "cache"),
		OperationTimeout: time.Minute}
	runtime, err := OpenTerraformRuntime(context.Background(), config)
	if err != nil {
		t.Fatal("open wrapped runner runtime")
	}
	if err := (&TerraformSession{runtime: runtime, reservationID: "0192f3a4-b5c6-7d8e-9f01-1234567890ab",
		environment: wrapperEnvironment(config, nil)}).run(context.Background(), nil, "plan", "-input=false"); err != nil {
		t.Fatal("run wrapped plan stand-in")
	}
	log, err := os.ReadFile(filepath.Join(wrapperHome, "wrapper.log"))
	if err != nil {
		t.Fatal("read wrapper invocation log")
	}
	defer clear(log)
	lines := strings.Split(strings.TrimSpace(string(log)), "\n")
	if len(lines) != 2 || lines[0] != "version -json" || lines[1] != "plan -input=false" {
		t.Fatalf("OpenTofu calls did not both pass through the wrapper: %q", lines)
	}
}
