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

// The pinned runtime constructor is the last cheap refusal in front of every
// Terraform operation: past it the control plane runs an executable with a
// reservation-bound Vault token against a real Proxmox lab. These tests hold
// it from the outside, with a stand-in executable whose digest is pinned the
// same way the real one is, so no lab and no Terraform install is needed.

const trustedProbeVersion = "1.10.5"

func trustedProbeBody(version string) string {
	return "#!/bin/sh\nprintf '%s' '{\"terraform_version\":\"" + version + "\"}'\n"
}

func trustedProbeScript(t *testing.T, directory, body string) (string, string) {
	t.Helper()
	if err := os.MkdirAll(directory, 0o700); err != nil {
		t.Fatalf("prepare probe directory: %v", err)
	}
	path, err := installTerraformProbe(t, directory, body)
	if err != nil {
		t.Fatalf("install probe fixture: %v", err)
	}
	digest, err := fileSHA256(path, maxTerraformBinaryBytes)
	if err != nil {
		t.Fatalf("digest probe script: %v", err)
	}
	return path, digest
}

// trustedRuntimeConfig is the reviewed shape: a pinned executable that answers
// with the pinned version, a real source environment, and a runtime state
// directory outside it. Neither the state nor the plugin cache exists yet.
func trustedRuntimeConfig(t *testing.T) TerraformConfig {
	t.Helper()
	root := t.TempDir()
	environment := filepath.Join(root, "environment")
	if err := os.MkdirAll(environment, 0o700); err != nil {
		t.Fatalf("prepare source environment: %v", err)
	}
	binary, digest := trustedProbeScript(t, filepath.Join(root, "bin"),
		trustedProbeBody(trustedProbeVersion))
	return TerraformConfig{
		BinaryPath:           binary,
		BinarySHA256:         digest,
		Version:              trustedProbeVersion,
		EnvironmentDirectory: environment,
		StateDirectory:       filepath.Join(root, "state"),
		PluginCacheDirectory: filepath.Join(root, "cache"),
	}
}

func trustedRuntime(t *testing.T, config TerraformConfig) *TerraformRuntime {
	t.Helper()
	runtime, err := OpenTerraformRuntime(context.Background(), config)
	if err != nil || runtime == nil {
		t.Fatalf("expected the reviewed runtime to open, got runtime %v error %v", runtime, err)
	}
	return runtime
}

func refusedRuntime(t *testing.T, name string, config TerraformConfig, want string) {
	t.Helper()
	runtime, err := OpenTerraformRuntime(context.Background(), config)
	if err == nil || runtime != nil {
		t.Fatalf("%s: expected a refusal, got runtime %v error %v", name, runtime, err)
	}
	if !strings.Contains(err.Error(), want) {
		t.Fatalf("%s: expected a refusal naming %q, got %q", name, want, err.Error())
	}
}

func TestTheReviewedRuntimeOpensAndKeepsItsPinnedConfiguration(t *testing.T) {
	config := trustedRuntimeConfig(t)
	runtime := trustedRuntime(t, config)
	if runtime.config.BinaryPath != config.BinaryPath ||
		runtime.config.BinarySHA256 != config.BinarySHA256 ||
		runtime.config.Version != trustedProbeVersion ||
		runtime.config.EnvironmentDirectory != config.EnvironmentDirectory ||
		runtime.config.StateDirectory != config.StateDirectory ||
		runtime.config.PluginCacheDirectory != config.PluginCacheDirectory {
		t.Fatalf("the opened runtime does not carry the pinned configuration: %+v", runtime.config)
	}
	// An unstated timeout becomes the twenty-minute default rather than a
	// command with no deadline at all.
	if runtime.config.OperationTimeout != 20*time.Minute {
		t.Fatalf("expected an unstated timeout to default to 20m, got %v", runtime.config.OperationTimeout)
	}
}

func TestOpeningARuntimeCreatesNeitherStateDirectoryNorPluginCache(t *testing.T) {
	config := trustedRuntimeConfig(t)
	trustedRuntime(t, config)
	for _, directory := range []string{config.StateDirectory, config.PluginCacheDirectory} {
		if _, err := os.Lstat(directory); !os.IsNotExist(err) {
			t.Fatalf("expected %s to stay absent until a session is opened, stat error %v", directory, err)
		}
	}
}

func TestTheBoundedOperationTimeoutIsExactAtBothEnds(t *testing.T) {
	for _, admitted := range []time.Duration{time.Second, 20 * time.Minute, 30 * time.Minute} {
		config := trustedRuntimeConfig(t)
		config.OperationTimeout = admitted
		if runtime := trustedRuntime(t, config); runtime.config.OperationTimeout != admitted {
			t.Fatalf("expected %v to be kept, got %v", admitted, runtime.config.OperationTimeout)
		}
	}
	for _, refused := range []time.Duration{
		-time.Second,
		time.Nanosecond,
		time.Second - time.Nanosecond,
		30*time.Minute + time.Nanosecond,
		time.Hour,
	} {
		config := trustedRuntimeConfig(t)
		config.OperationTimeout = refused
		refusedRuntime(t, refused.String(), config, "outside bounded policy")
	}
}

func TestEveryUnpinnedFieldRefusesTheRuntimeBeforeAnythingRuns(t *testing.T) {
	relative := "relative/path"
	cases := []struct {
		name string
		edit func(*TerraformConfig)
	}{
		{"relative binary", func(c *TerraformConfig) { c.BinaryPath = relative }},
		{"empty binary", func(c *TerraformConfig) { c.BinaryPath = "" }},
		{"relative environment", func(c *TerraformConfig) { c.EnvironmentDirectory = relative }},
		{"empty environment", func(c *TerraformConfig) { c.EnvironmentDirectory = "" }},
		{"relative state", func(c *TerraformConfig) { c.StateDirectory = relative }},
		{"empty state", func(c *TerraformConfig) { c.StateDirectory = "" }},
		{"relative cache", func(c *TerraformConfig) { c.PluginCacheDirectory = relative }},
		{"empty cache", func(c *TerraformConfig) { c.PluginCacheDirectory = "" }},
		{"no digest", func(c *TerraformConfig) { c.BinarySHA256 = "" }},
		{"short digest", func(c *TerraformConfig) { c.BinarySHA256 = strings.Repeat("a", 63) }},
		{"long digest", func(c *TerraformConfig) { c.BinarySHA256 = strings.Repeat("a", 65) }},
		{"upper-case digest", func(c *TerraformConfig) { c.BinarySHA256 = strings.Repeat("A", 64) }},
		{"non-hex digest", func(c *TerraformConfig) { c.BinarySHA256 = strings.Repeat("g", 64) }},
		{"no version", func(c *TerraformConfig) { c.Version = "" }},
		{"two-part version", func(c *TerraformConfig) { c.Version = "1.10" }},
		{"prefixed version", func(c *TerraformConfig) { c.Version = "v1.10.5" }},
		{"pre-release version", func(c *TerraformConfig) { c.Version = "1.10.5-beta1" }},
		{"padded version", func(c *TerraformConfig) { c.Version = " 1.10.5" }},
	}
	for _, testCase := range cases {
		config := trustedRuntimeConfig(t)
		testCase.edit(&config)
		refusedRuntime(t, testCase.name, config, "invalid pinned Terraform runtime configuration")
	}
}

func TestAMissingContextIsRefusedWithoutProbingAnything(t *testing.T) {
	config := trustedRuntimeConfig(t)
	runtime, err := OpenTerraformRuntime(nil, config) //nolint:staticcheck // the nil context is the case under test
	if err == nil || runtime != nil {
		t.Fatalf("expected a nil context to be refused, got runtime %v error %v", runtime, err)
	}
	if !strings.Contains(err.Error(), "invalid pinned Terraform runtime configuration") {
		t.Fatalf("expected the configuration refusal, got %q", err.Error())
	}
}

func TestTheConfigurationGuardRunsAheadOfTheDigest(t *testing.T) {
	// Wrong in both ways at once: the guard's reading is the one an operator
	// gets, so a configuration mistake is never reported as a tampered binary.
	config := trustedRuntimeConfig(t)
	config.BinaryPath = "bin/terraform"
	config.BinarySHA256 = strings.Repeat("b", 64)
	refusedRuntime(t, "relative path and wrong digest", config,
		"invalid pinned Terraform runtime configuration")
}

func TestAnExecutableThatDoesNotMatchItsPinnedDigestIsRefused(t *testing.T) {
	stated := trustedRuntimeConfig(t)

	mismatch := trustedRuntimeConfig(t)
	mismatch.BinarySHA256 = strings.Repeat("c", 64)
	refusedRuntime(t, "another digest", mismatch, "digest differs from pinned configuration")

	missing := trustedRuntimeConfig(t)
	if err := os.Remove(missing.BinaryPath); err != nil {
		t.Fatalf("remove the pinned binary: %v", err)
	}
	refusedRuntime(t, "absent binary", missing, "digest differs from pinned configuration")

	empty := trustedRuntimeConfig(t)
	if err := os.WriteFile(empty.BinaryPath, nil, 0o755); err != nil {
		t.Fatalf("empty the pinned binary: %v", err)
	}
	empty.BinarySHA256 = stated.BinarySHA256
	refusedRuntime(t, "empty binary", empty, "digest differs from pinned configuration")

	// A symlink pointing at the reviewed executable is refused rather than
	// followed: the pinned path must be the file that runs.
	linked := trustedRuntimeConfig(t)
	target := linked.BinaryPath
	link := filepath.Join(filepath.Dir(target), "terraform-link")
	symlinkTest(t, target, link)
	linked.BinaryPath = link
	refusedRuntime(t, "symlinked binary", linked, "digest differs from pinned configuration")

	directory := trustedRuntimeConfig(t)
	directory.BinaryPath = directory.EnvironmentDirectory
	refusedRuntime(t, "directory as binary", directory, "digest differs from pinned configuration")
}

func TestTheDigestIsJudgedBeforeTheSourceEnvironment(t *testing.T) {
	config := trustedRuntimeConfig(t)
	config.BinarySHA256 = strings.Repeat("d", 64)
	if err := os.RemoveAll(config.EnvironmentDirectory); err != nil {
		t.Fatalf("remove the source environment: %v", err)
	}
	refusedRuntime(t, "wrong digest and absent environment", config,
		"digest differs from pinned configuration")
}

func TestAnUnavailableSourceEnvironmentIsRefused(t *testing.T) {
	absent := trustedRuntimeConfig(t)
	if err := os.RemoveAll(absent.EnvironmentDirectory); err != nil {
		t.Fatalf("remove the source environment: %v", err)
	}
	refusedRuntime(t, "absent environment", absent, "environment directory is unavailable")

	file := trustedRuntimeConfig(t)
	if err := os.RemoveAll(file.EnvironmentDirectory); err != nil {
		t.Fatalf("remove the source environment: %v", err)
	}
	if err := os.WriteFile(file.EnvironmentDirectory, []byte("not a directory\n"), 0o600); err != nil {
		t.Fatalf("write a file where the environment belongs: %v", err)
	}
	refusedRuntime(t, "file as environment", file, "environment directory is unavailable")
}

func TestRuntimeStateMayNotLiveInsideTheSourceEnvironment(t *testing.T) {
	same := trustedRuntimeConfig(t)
	same.StateDirectory = same.EnvironmentDirectory
	refusedRuntime(t, "state is the environment", same, "outside the source environment")

	spelled := trustedRuntimeConfig(t)
	spelled.StateDirectory = spelled.EnvironmentDirectory + "/."
	refusedRuntime(t, "state is the environment spelled differently", spelled,
		"outside the source environment")

	nested := trustedRuntimeConfig(t)
	nested.StateDirectory = filepath.Join(nested.EnvironmentDirectory, "state")
	refusedRuntime(t, "state under the environment", nested, "outside the source environment")

	deep := trustedRuntimeConfig(t)
	deep.StateDirectory = filepath.Join(deep.EnvironmentDirectory, "a", "b", "c")
	refusedRuntime(t, "state deep under the environment", deep, "outside the source environment")

	// The refusal is about containment, not about sharing a parent.
	sibling := trustedRuntimeConfig(t)
	sibling.StateDirectory = sibling.EnvironmentDirectory + "-state"
	trustedRuntime(t, sibling)
}

func TestTheSourceEnvironmentMayLiveInsideTheStateDirectory(t *testing.T) {
	// The check is one-way: what must not happen is Terraform runtime state
	// landing among the reviewed sources. A state directory that happens to be
	// an ancestor of the environment is admitted, so this asymmetry is held
	// deliberately rather than by accident.
	config := trustedRuntimeConfig(t)
	config.StateDirectory = filepath.Dir(config.EnvironmentDirectory)
	trustedRuntime(t, config)
}

func TestTheVersionProbeRunsInsideTheSourceEnvironment(t *testing.T) {
	root := t.TempDir()
	environment := filepath.Join(root, "environment")
	if err := os.MkdirAll(environment, 0o700); err != nil {
		t.Fatalf("prepare source environment: %v", err)
	}
	record := filepath.Join(t.TempDir(), "probe-directory")
	body := "#!/bin/sh\npwd > '" + record + "'\nprintf '%s' '{\"terraform_version\":\"" +
		trustedProbeVersion + "\"}'\n"
	binary, digest := trustedProbeScript(t, filepath.Join(root, "bin"), body)
	config := TerraformConfig{
		BinaryPath: binary, BinarySHA256: digest, Version: trustedProbeVersion,
		EnvironmentDirectory: environment,
		StateDirectory:       filepath.Join(root, "state"),
		PluginCacheDirectory: filepath.Join(root, "cache"),
	}
	trustedRuntime(t, config)
	observed, err := os.ReadFile(record)
	if err != nil {
		t.Fatalf("read the probe's working directory: %v", err)
	}
	if strings.TrimSpace(string(observed)) != config.EnvironmentDirectory {
		t.Fatalf("expected the probe to run in %s, it ran in %s",
			config.EnvironmentDirectory, strings.TrimSpace(string(observed)))
	}
}

func TestAProbeThatFailsOrAnswersAnotherVersionIsRefused(t *testing.T) {
	cases := []struct {
		name string
		body string
	}{
		{"exits non-zero", "#!/bin/sh\nexit 1\n"},
		{"says nothing", "#!/bin/sh\nexit 0\n"},
		{"answers a later version", trustedProbeBody("1.10.6")},
		{"answers a two-part version", trustedProbeBody("1.10")},
		{"answers an empty version", trustedProbeBody("")},
		{"answers without the field", "#!/bin/sh\nprintf '%s' '{\"version\":\"1.10.5\"}'\n"},
		{"answers something that is not JSON", "#!/bin/sh\nprintf '%s' 'Terraform v1.10.5'\n"},
		{"answers more than the output bound", "#!/bin/sh\nyes a | head -c 70000\n"},
	}
	for _, testCase := range cases {
		config := trustedRuntimeConfig(t)
		binary, digest := trustedProbeScript(t, filepath.Dir(config.BinaryPath), testCase.body)
		config.BinaryPath, config.BinarySHA256 = binary, digest
		// Both remaining refusals name the version; which one an operator gets
		// depends on whether the executable ran at all.
		refusedRuntime(t, testCase.name, config, "version")
	}
}

func TestAProbeAnsweringTheStatedVersionOnStderrIsStillRefused(t *testing.T) {
	// Stderr is discarded, so a Terraform whose JSON goes to the wrong stream
	// is refused rather than half-read.
	config := trustedRuntimeConfig(t)
	body := "#!/bin/sh\nprintf '%s' '{\"terraform_version\":\"" + trustedProbeVersion +
		"\"}' 1>&2\n"
	binary, digest := trustedProbeScript(t, filepath.Dir(config.BinaryPath), body)
	config.BinaryPath, config.BinarySHA256 = binary, digest
	refusedRuntime(t, "answer on stderr", config, "version differs from pinned configuration")
}

func TestAnAlreadyCancelledContextRefusesTheRuntime(t *testing.T) {
	config := trustedRuntimeConfig(t)
	ctx, cancel := context.WithCancel(context.Background())
	cancel()
	runtime, err := OpenTerraformRuntime(ctx, config)
	if err == nil || runtime != nil {
		t.Fatalf("expected a cancelled context to be refused, got runtime %v error %v", runtime, err)
	}
	if !strings.Contains(err.Error(), "version probe failed") {
		t.Fatalf("expected the probe refusal, got %q", err.Error())
	}
}
