// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package provision

import (
	"os"
	"strings"
	"testing"
	"time"
)

// trustFixture builds a CA bundle out of the validity windows a caller names,
// which is the only thing these tests vary. The clock is truncated to the
// second because a certificate records its window to the second and nothing
// finer, so a boundary case built from an unrounded clock would be asking
// about a time the file cannot express.
func trustFixture(t *testing.T, now time.Time, expiries ...time.Duration) []byte {
	t.Helper()
	var bundle []byte
	for index, left := range expiries {
		bundle = append(bundle, authorityPEM(t,
			caTemplateAt(int64(200+index), now.Add(-time.Hour), now.Add(left)))...)
	}
	return bundle
}

func trustClock() time.Time {
	return time.Now().Truncate(time.Second)
}

func TestServerTrustThatCannotCoverOneCommandIsRefused(t *testing.T) {
	now := trustClock()
	for _, testCase := range []struct {
		name     string
		left     time.Duration
		deadline time.Duration
	}{
		{"a minute left against the default deadline", time.Minute, 20 * time.Minute},
		{"one second short", 20*time.Minute - time.Second, 20 * time.Minute},
		{"expiring exactly when the command would end", 20 * time.Minute, 20 * time.Minute},
		{"half a command", 10 * time.Minute, 20 * time.Minute},
	} {
		t.Run(testCase.name, func(t *testing.T) {
			bundle := trustFixture(t, now, testCase.left)
			if err := checkServerTrustCoversOperation(bundle, testCase.deadline, now); err == nil {
				t.Fatal("a trust file that cannot cover one command was accepted")
			}
		})
	}
}

// The refusal has to name the file an operator goes and fixes, and say how
// long the work it could not cover may run. An operator reading it otherwise
// learns only that something about a certificate is wrong.
func TestTheServerTrustRefusalNamesTheBundleAndTheDeadline(t *testing.T) {
	now := trustClock()
	err := checkServerTrustCoversOperation(trustFixture(t, now, time.Minute), 20*time.Minute, now)
	if err == nil {
		t.Fatal("an expiring trust file was accepted")
	}
	for _, want := range []string{"Terraform server CA bundle", "20m0s"} {
		if !strings.Contains(err.Error(), want) {
			t.Fatalf("refusal does not carry %q: %v", want, err)
		}
	}
}

// The ordinary shape of every healthy deployment: trust that outlasts the
// command by days. The rule is one-sided and says nothing about it.
func TestServerTrustThatOutlastsTheCommandIsAccepted(t *testing.T) {
	now := trustClock()
	for _, left := range []time.Duration{20*time.Minute + time.Second, time.Hour, 72 * time.Hour} {
		if err := checkServerTrustCoversOperation(trustFixture(t, now, left), 20*time.Minute, now); err != nil {
			t.Fatalf("trust with %s left was refused against a 20m command: %v", left, err)
		}
	}
}

// A rotation puts the outgoing authority beside the incoming one, and the
// bundle is usable while at least one of them is. Judging the file by its
// earliest expiry would refuse the shape a rotation is supposed to have.
func TestARotatingBundleIsJudgedByItsLiveAuthority(t *testing.T) {
	now := trustClock()
	bundle := trustFixture(t, now, time.Minute, 72*time.Hour)
	if err := checkServerTrustCoversOperation(bundle, 20*time.Minute, now); err != nil {
		t.Fatalf("a rotation bundle was refused: %v", err)
	}
}

// Every authority retiring inside the command is the state this rule exists
// for: the file authenticates somebody now and nobody by the end.
func TestABundleThatRetiresEntirelyInsideTheCommandIsRefused(t *testing.T) {
	now := trustClock()
	bundle := trustFixture(t, now, time.Minute, 5*time.Minute, 10*time.Minute)
	if err := checkServerTrustCoversOperation(bundle, 20*time.Minute, now); err == nil {
		t.Fatal("a bundle whose every authority retires inside the command was accepted")
	}
}

// An unstated deadline leaves the question to the validity check the caller
// already ran, rather than inventing a number here.
func TestAnUnstatedDeadlineLeavesTheBundleAlone(t *testing.T) {
	now := trustClock()
	for _, deadline := range []time.Duration{0, -time.Minute} {
		if err := checkServerTrustCoversOperation(trustFixture(t, now, time.Second), deadline, now); err != nil {
			t.Fatalf("an unstated deadline refused a bundle: %v", err)
		}
	}
}

// A bundle that is unusable for a reason having nothing to do with the clock
// is still refused, because the rule is mtls's rule asked at a later instant
// rather than a private expiry comparison.
func TestAnUnusableBundleIsRefusedWhateverTheDeadline(t *testing.T) {
	now := trustClock()
	for name, bundle := range map[string][]byte{
		"empty":                []byte(nil),
		"no certificate in it": []byte("-----BEGIN PRIVATE KEY-----\nZm9v\n-----END PRIVATE KEY-----\n"),
	} {
		t.Run(name, func(t *testing.T) {
			if err := checkServerTrustCoversOperation(bundle, time.Minute, now); err == nil {
				t.Fatal("an unusable bundle was accepted")
			}
		})
	}
}

// Through the entry point: the backend refuses to build an environment whose
// trust file cannot survive the command it is being built for, and the
// refusal names the bundle.
func TestTerraformBackendRefusesServerTrustThatCannotCoverOneCommand(t *testing.T) {
	config := testHTTPBackendConfig(t)
	config.OperationTimeout = 20 * time.Minute
	now := trustClock()
	if err := os.WriteFile(config.ServerCABundleFile, trustFixture(t, now, time.Minute), 0o644); err != nil {
		t.Fatal(err)
	}
	_, err := HTTPBackendEnvironment(config)
	if err == nil {
		t.Fatal("a trust file that retires inside the command was accepted")
	}
	if !strings.Contains(err.Error(), "Terraform server CA bundle") {
		t.Fatalf("refusal does not name the bundle an operator must fix: %v", err)
	}
}

// The same config with no deadline stated still builds, so the new rule
// narrows only what a caller that states its deadline may configure.
func TestTerraformBackendWithoutADeadlineStillBuildsTheEnvironment(t *testing.T) {
	config := testHTTPBackendConfig(t)
	now := trustClock()
	if err := os.WriteFile(config.ServerCABundleFile, trustFixture(t, now, time.Minute), 0o644); err != nil {
		t.Fatal(err)
	}
	if _, err := HTTPBackendEnvironment(config); err != nil {
		t.Fatalf("an unstated deadline refused a usable bundle: %v", err)
	}
}
