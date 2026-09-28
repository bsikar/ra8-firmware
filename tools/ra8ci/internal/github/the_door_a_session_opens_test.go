// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package github

import (
	"context"
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"
)

// Opening a session sends App credentials to GitHub, so everything that can
// be judged locally is judged before the first byte leaves this process.
// session_test.go pins the owner, the nil caller, a group-readable key, a
// directory and a symlink; this pins the rest of that door and the two
// entry points that walk through it.

// openable is a configuration that would be accepted as far as the network.
func openable(t *testing.T, key []byte) SessionConfig {
	t.Helper()
	file := filepath.Join(t.TempDir(), "app.pem")
	if err := os.WriteFile(file, key, 0o600); err != nil {
		t.Fatal(err)
	}
	if err := os.Chmod(file, 0o600); err != nil {
		t.Fatal(err)
	}
	return SessionConfig{
		GitHubConfigURL: "https://github.com/bsikar/ra8-firmware", AppClientID: "Iv1.0123456789abcdef",
		InstallationID: 7, PrivateKeyFile: file, Owner: "bsikar", ScaleSetID: 42, MaxRunners: 2,
	}
}

// Every field of the configuration is judged before anything is read from
// disk or sent anywhere, so a typo in a unit file is an immediate refusal
// rather than a credential posted somewhere unintended.
func TestASessionConfigurationIsJudgedWholeBeforeAnythingIsRead(t *testing.T) {
	sound := openable(t, []byte("placeholder key material"))

	blankURL := sound
	blankURL.GitHubConfigURL = ""

	paddedURL := sound
	paddedURL.GitHubConfigURL = " https://github.com/bsikar "

	noClient := sound
	noClient.AppClientID = ""

	noInstallation := sound
	noInstallation.InstallationID = 0

	negativeInstallation := sound
	negativeInstallation.InstallationID = -7

	noKeyFile := sound
	noKeyFile.PrivateKeyFile = ""

	noScaleSet := sound
	noScaleSet.ScaleSetID = 0

	negativeRunners := sound
	negativeRunners.MaxRunners = -1

	tooManyRunners := sound
	tooManyRunners.MaxRunners = 10001

	dottedOwner := sound
	dottedOwner.Owner = "bsi.kar"

	longOwner := sound
	longOwner.Owner = strings.Repeat("b", 40)

	for name, config := range map[string]SessionConfig{
		"no URL":                 blankURL,
		"a padded URL":           paddedURL,
		"no client":              noClient,
		"no installation":        noInstallation,
		"a negative installat.":  negativeInstallation,
		"no key file":            noKeyFile,
		"no scale set":           noScaleSet,
		"negative runners":       negativeRunners,
		"more than ten thousand": tooManyRunners,
		"a dotted owner":         dottedOwner,
		"an over-long owner":     longOwner,
	} {
		if _, err := OpenSession(context.Background(), config); err == nil ||
			!strings.Contains(err.Error(), "configuration") {
			t.Fatalf("%s was not refused as configuration: %v", name, err)
		}
	}

	// A URL that parses but names somewhere else is refused by its own
	// rule, after the shape check and still before any read.
	elsewhere := sound
	elsewhere.GitHubConfigURL = "https://github.example.com/bsikar"
	if _, err := OpenSession(context.Background(), elsewhere); err == nil ||
		strings.Contains(err.Error(), "private key") {
		t.Fatalf("a foreign host reached the key: %v", err)
	}

	// Zero runners is a valid scale set that simply may not grow, and ten
	// thousand is the stated ceiling, so neither is refused here. Both run
	// on to the key, which is as far as this box can take them.
	for name, runners := range map[string]int{"no runners": 0, "the ceiling": 10000} {
		config := sound
		config.MaxRunners = runners
		config.PrivateKeyFile = filepath.Join(t.TempDir(), "absent.pem")
		if _, err := OpenSession(context.Background(), config); err == nil ||
			strings.Contains(err.Error(), "configuration") {
			t.Fatalf("%s was refused as configuration: %v", name, err)
		}
	}
}

// The private key is read from a bounded regular file. An empty one and an
// oversized one are both refused before the credential is assembled.
func TestAPrivateKeyOutsideItsBoundsIsRefusedBeforeTheCredential(t *testing.T) {
	empty := openable(t, nil)
	if _, err := OpenSession(context.Background(), empty); err == nil ||
		!strings.Contains(err.Error(), "bounded regular file") {
		t.Fatalf("an empty key file: %v", err)
	}

	oversized := openable(t, []byte(strings.Repeat("k", maxGitHubPrivateKeyBytes+1)))
	if _, err := OpenSession(context.Background(), oversized); err == nil ||
		!strings.Contains(err.Error(), "bounded regular file") {
		t.Fatalf("an oversized key file: %v", err)
	}

	absent := openable(t, []byte("placeholder key material"))
	absent.PrivateKeyFile = filepath.Join(t.TempDir(), "absent.pem")
	if _, err := OpenSession(context.Background(), absent); err == nil ||
		!strings.Contains(err.Error(), "stat GitHub App private key") {
		t.Fatalf("an absent key file: %v", err)
	}
}

// This client talks to public GitHub only. The upstream library reads an
// environment variable to force a GitHub Enterprise host; setting it stops
// the session rather than quietly redirecting the credential.
func TestAnEnterpriseOverrideStopsTheSessionRatherThanRedirectingIt(t *testing.T) {
	t.Setenv("GITHUB_ACTIONS_FORCE_GHES", "1")

	config := openable(t, []byte("placeholder key material"))
	_, err := OpenSession(context.Background(), config)
	if err == nil || !strings.Contains(err.Error(), "GITHUB_ACTIONS_FORCE_GHES") {
		t.Fatalf("the override was not refused: %v", err)
	}

	// The override is judged before the key is read, so an unreadable key
	// is not what answers here.
	if strings.Contains(err.Error(), "private key") {
		t.Fatalf("the key was read before the override was judged: %v", err)
	}
}

// Both entry points open the session first, so a configuration the session
// refuses never reaches the handler factory: nothing is built and there is
// nothing to close.
func TestNeitherEntryPointBuildsAnythingOnARefusedSession(t *testing.T) {
	config := openable(t, []byte("placeholder key material"))
	config.ScaleSetID = 0

	consulted := false
	factory := func(*Session) (Handler, error) {
		consulted = true
		return &testHandler{}, nil
	}

	bound, err := OpenControllerWithHandlerFactory(context.Background(), config,
		&fakeInbox{}, testAdmission{}, time.Second, factory)
	if err == nil || bound != nil {
		t.Fatalf("a refused session was composed: %+v, %v", bound, err)
	}

	bound, err = OpenController(context.Background(), config,
		&fakeInbox{}, &testHandler{}, testAdmission{}, time.Second)
	if err == nil || bound != nil {
		t.Fatalf("a refused session was composed: %+v, %v", bound, err)
	}
	if consulted {
		t.Fatal("the handler factory was consulted for a session that never opened")
	}
}
