// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package github

import (
	"crypto/rand"
	"crypto/rsa"
	"crypto/x509"
	"encoding/pem"
	"errors"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

// publisherEnvironment sets a complete, valid check-run publisher
// environment. Individual tests override or unset one variable at a time.
func publisherEnvironment(t *testing.T) string {
	t.Helper()
	keyFile := filepath.Join(t.TempDir(), "app.pem")
	t.Setenv(EnvCheckRunRepository, "ra8-firmware")
	t.Setenv(EnvAppClientID, "Iv1.0123456789abcdef")
	t.Setenv(EnvInstallationID, "94213")
	t.Setenv(EnvPrivateKeyFile, keyFile)
	t.Setenv(EnvOwner, "bsikar")
	return keyFile
}

func TestPublishingIsDisabledWithoutItsOwnRepositoryVariable(t *testing.T) {
	// The credentials belong to the scale-set session too, so a session
	// deployment that publishes nothing must read as disabled rather than
	// as a publisher missing its repository.
	publisherEnvironment(t)
	os.Unsetenv(EnvCheckRunRepository)

	config, enabled, err := LoadCheckRunPublisherConfigFromEnv(ModeShadow)
	if err != nil {
		t.Fatalf("a session-only environment is not an error: %v", err)
	}
	if enabled {
		t.Fatal("publishing reported enabled with no repository named")
	}
	if config != (CheckRunPublisherConfig{}) {
		t.Fatalf("disabled configuration is not zero: %+v", config)
	}
}

func TestPublisherConfigIsBuiltFromTheSessionCredentials(t *testing.T) {
	keyFile := publisherEnvironment(t)

	config, enabled, err := LoadCheckRunPublisherConfigFromEnv(ModeShadow)
	if err != nil || !enabled {
		t.Fatalf("enabled=%v err=%v", enabled, err)
	}
	want := CheckRunPublisherConfig{
		AppClientID:    "Iv1.0123456789abcdef",
		InstallationID: 94213,
		PrivateKeyFile: keyFile,
		Owner:          "bsikar",
		Repository:     "ra8-firmware",
		Mode:           ModeShadow,
	}
	if config != want {
		t.Fatalf("configuration\n got %+v\nwant %+v", config, want)
	}
}

func TestPublisherConfigCarriesTheModeItIsGiven(t *testing.T) {
	for _, mode := range []CheckRunMode{ModeShadow, ModeAuthoritative} {
		t.Run(mode.String(), func(t *testing.T) {
			publisherEnvironment(t)
			config, enabled, err := LoadCheckRunPublisherConfigFromEnv(mode)
			if err != nil || !enabled {
				t.Fatalf("enabled=%v err=%v", enabled, err)
			}
			if config.Mode != mode {
				t.Fatalf("mode %s, want %s", config.Mode, mode)
			}
		})
	}
}

func TestPublisherConfigRefusesAModeThisBuildWillNotRun(t *testing.T) {
	// Refused on the mode alone, with every credential absent, so the
	// refusal does not depend on what the environment happens to carry.
	t.Setenv(EnvCheckRunRepository, "ra8-firmware")
	for _, name := range checkRunCredentialNames {
		t.Setenv(name, "")
		os.Unsetenv(name)
	}

	_, enabled, err := LoadCheckRunPublisherConfigFromEnv(CheckRunMode(7))
	if enabled {
		t.Fatal("an unknown mode reported enabled")
	}
	if !errors.Is(err, ErrInvalidCheckRunMode) {
		t.Fatalf("error %v, want ErrInvalidCheckRunMode", err)
	}
	if IsEnvConfigError(err) {
		t.Fatal("an unknown mode is a caller error, not an operator environment error")
	}
}

func TestPublisherConfigRefusalsNameTheVariable(t *testing.T) {
	cases := []struct {
		name     string
		variable string
		value    string
		absent   bool
	}{
		{name: "missing client id", variable: EnvAppClientID, absent: true},
		{name: "missing installation", variable: EnvInstallationID, absent: true},
		{name: "missing key file", variable: EnvPrivateKeyFile, absent: true},
		{name: "missing owner", variable: EnvOwner, absent: true},
		{name: "empty repository", variable: EnvCheckRunRepository, value: ""},
		{name: "empty client id", variable: EnvAppClientID, value: ""},
		{name: "spaced key file", variable: EnvPrivateKeyFile, value: "/etc/ra8ci/app key.pem"},
		{name: "owner and name pair", variable: EnvCheckRunRepository, value: "bsikar/ra8-firmware"},
		{name: "owner is a url", variable: EnvOwner, value: "github.com"},
		{name: "installation is not a number", variable: EnvInstallationID, value: "0x4f"},
		{name: "installation is zero", variable: EnvInstallationID, value: "0"},
	}
	for _, testCase := range cases {
		t.Run(testCase.name, func(t *testing.T) {
			publisherEnvironment(t)
			t.Setenv(testCase.variable, testCase.value)
			if testCase.absent {
				os.Unsetenv(testCase.variable)
			}

			_, enabled, err := LoadCheckRunPublisherConfigFromEnv(ModeShadow)
			if enabled {
				t.Fatal("a refused environment reported enabled")
			}
			var target *EnvConfigError
			if !errors.As(err, &target) {
				t.Fatalf("error %v, want an EnvConfigError", err)
			}
			if target.Variable != testCase.variable {
				t.Fatalf("refusal names %s, want %s", target.Variable, testCase.variable)
			}
		})
	}
}

func TestPublisherConfigRefusalsDoNotQuoteTheValues(t *testing.T) {
	// One of these variables is a path and another is an account name.
	// A refusal that quotes them copies deployment detail into whatever
	// reads the error.
	const secretPath = "/var/run/secrets/ra8ci app key.pem"
	publisherEnvironment(t)
	t.Setenv(EnvPrivateKeyFile, secretPath)

	_, _, err := LoadCheckRunPublisherConfigFromEnv(ModeShadow)
	if err == nil {
		t.Fatal("a whitespace path was accepted")
	}
	if strings.Contains(err.Error(), "secrets") || strings.Contains(err.Error(), secretPath) {
		t.Fatalf("refusal quotes the value: %v", err)
	}
	if !strings.Contains(err.Error(), EnvPrivateKeyFile) {
		t.Fatalf("refusal does not name the variable: %v", err)
	}
}

func TestPublisherConfigIsOneNewCheckRunPublisherAccepts(t *testing.T) {
	// The loader and the constructor are two different validations of the
	// same configuration. This pins that a configuration the loader
	// produces is one the constructor takes, so a deployment cannot pass
	// startup configuration and then fail to build a publisher.
	keyFile := publisherEnvironment(t)
	key, err := rsa.GenerateKey(rand.Reader, 2048)
	if err != nil {
		t.Fatalf("generate key: %v", err)
	}
	encoded := pem.EncodeToMemory(&pem.Block{Type: "RSA PRIVATE KEY", Bytes: x509.MarshalPKCS1PrivateKey(key)})
	if err := os.WriteFile(keyFile, encoded, 0o600); err != nil {
		t.Fatalf("write key: %v", err)
	}

	config, enabled, err := LoadCheckRunPublisherConfigFromEnv(ModeShadow)
	if err != nil || !enabled {
		t.Fatalf("enabled=%v err=%v", enabled, err)
	}
	publisher, err := NewCheckRunPublisher(config)
	if err != nil {
		t.Fatalf("the loaded configuration was refused by the constructor: %v", err)
	}
	if publisher == nil {
		t.Fatal("no publisher built")
	}
}
