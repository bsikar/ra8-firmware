// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package github

import (
	"errors"
	"math"
	"os"
	"strconv"
	"strings"
	"testing"
)

func clearSessionEnvironment(t *testing.T) {
	t.Helper()
	for _, name := range sessionEnvironmentNames {
		t.Setenv(name, "")
		if err := os.Unsetenv(name); err != nil {
			t.Fatal(err)
		}
	}
}

func validSessionEnvironment(t *testing.T) {
	t.Helper()
	values := map[string]string{
		EnvConfigURL:      "https://github.com/bsikar/ra8-firmware",
		EnvAppClientID:    "Iv1.example",
		EnvInstallationID: "123456",
		EnvPrivateKeyFile: "/run/ra8ci/github-app.pem",
		EnvOwner:          "bsikar",
		EnvScaleSetID:     "42",
		EnvMaxRunners:     "8",
	}
	for name, value := range values {
		t.Setenv(name, value)
	}
}

func TestLoadSessionConfigFromEnvDisabledWhenAllAbsent(t *testing.T) {
	clearSessionEnvironment(t)
	config, enabled, err := LoadSessionConfigFromEnv()
	if err != nil || enabled || config != (SessionConfig{}) {
		t.Fatalf("disabled config = %+v, %v, %v", config, enabled, err)
	}
}

func TestLoadSessionConfigFromEnvComplete(t *testing.T) {
	clearSessionEnvironment(t)
	validSessionEnvironment(t)
	config, enabled, err := LoadSessionConfigFromEnv()
	if err != nil || !enabled {
		t.Fatalf("complete config rejected: enabled=%v err=%v", enabled, err)
	}
	want := SessionConfig{GitHubConfigURL: "https://github.com/bsikar/ra8-firmware", AppClientID: "Iv1.example",
		InstallationID: 123456, PrivateKeyFile: "/run/ra8ci/github-app.pem", Owner: "bsikar", ScaleSetID: 42, MaxRunners: 8}
	if config != want {
		t.Fatalf("config = %+v, want %+v", config, want)
	}
}

func TestLoadSessionConfigFromEnvPartialNamesMissingVariable(t *testing.T) {
	clearSessionEnvironment(t)
	t.Setenv(EnvConfigURL, "https://github.com/bsikar")
	_, enabled, err := LoadSessionConfigFromEnv()
	var configErr *EnvConfigError
	if enabled || !errors.As(err, &configErr) || configErr.Variable != EnvAppClientID || !IsEnvConfigError(err) {
		t.Fatalf("partial config error = %#v, enabled=%v", err, enabled)
	}
}

func TestLoadSessionConfigFromEnvRejectsEmptyAndWhitespace(t *testing.T) {
	for _, tc := range []struct {
		name, variable, value string
	}{
		{name: "empty", variable: EnvOwner, value: ""},
		{name: "leading space", variable: EnvConfigURL, value: " https://github.com/bsikar"},
		{name: "trailing newline", variable: EnvPrivateKeyFile, value: "/run/key.pem\n"},
		{name: "integer trailing space", variable: EnvScaleSetID, value: "42 "},
	} {
		t.Run(tc.name, func(t *testing.T) {
			clearSessionEnvironment(t)
			validSessionEnvironment(t)
			t.Setenv(tc.variable, tc.value)
			_, enabled, err := LoadSessionConfigFromEnv()
			var configErr *EnvConfigError
			if enabled || !errors.As(err, &configErr) || configErr.Variable != tc.variable {
				t.Fatalf("error = %#v, enabled=%v", err, enabled)
			}
		})
	}
}

func TestLoadSessionConfigFromEnvRejectsNonCanonicalAndOutOfRangeIntegers(t *testing.T) {
	for _, tc := range []struct {
		name, variable, value string
	}{
		{name: "negative installation", variable: EnvInstallationID, value: "-1"},
		{name: "plus scale set", variable: EnvScaleSetID, value: "+42"},
		{name: "leading zero", variable: EnvScaleSetID, value: "042"},
		{name: "decimal junk", variable: EnvMaxRunners, value: "8runners"},
		{name: "hex", variable: EnvInstallationID, value: "0x2a"},
		{name: "zero installation", variable: EnvInstallationID, value: "0"},
		{name: "zero scale set", variable: EnvScaleSetID, value: "0"},
		{name: "too many runners", variable: EnvMaxRunners, value: "10001"},
		{name: "int64 overflow", variable: EnvInstallationID, value: "9223372036854775808"},
		{name: "platform int overflow", variable: EnvScaleSetID, value: strconv.FormatUint(uint64(math.MaxInt64), 10) + "0"},
	} {
		t.Run(tc.name, func(t *testing.T) {
			clearSessionEnvironment(t)
			validSessionEnvironment(t)
			t.Setenv(tc.variable, tc.value)
			_, enabled, err := LoadSessionConfigFromEnv()
			var configErr *EnvConfigError
			if enabled || !errors.As(err, &configErr) || configErr.Variable != tc.variable {
				t.Fatalf("error = %#v, enabled=%v", err, enabled)
			}
		})
	}
}

func TestLoadSessionConfigFromEnvDoesNotInterpretInlinePrivateKey(t *testing.T) {
	clearSessionEnvironment(t)
	validSessionEnvironment(t)
	inline := "-----BEGIN PRIVATE KEY-----\nsecret\n-----END PRIVATE KEY-----"
	t.Setenv(EnvPrivateKeyFile, inline)
	_, enabled, err := LoadSessionConfigFromEnv()
	var configErr *EnvConfigError
	if enabled || !errors.As(err, &configErr) || configErr.Variable != EnvPrivateKeyFile ||
		strings.Contains(err.Error(), "secret") {
		t.Fatalf("inline key was accepted or exposed: enabled=%v err=%v", enabled, err)
	}
}
