// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package unsafeinstall

import (
	"bytes"
	"context"
	"strings"
	"testing"
)

// The self-test is this gate's claim that its detector still reads every
// spelling of the override. The claim is only worth having if a detector gone
// wrong actually breaks it, so these blind the detector in turn and watch the
// self-test refuse, name the case that broke, and answer 1.

// withOverrideKey swaps the key the detector looks for.
func withOverrideKey(t *testing.T, key string) {
	t.Helper()
	kept := overrideKey
	overrideKey = key
	t.Cleanup(func() { overrideKey = kept })
}

// withFalsyValues swaps the values that pin the override off.
func withFalsyValues(t *testing.T, values []string) {
	t.Helper()
	kept := falsyValues
	falsyValues = values
	t.Cleanup(func() { falsyValues = kept })
}

// selfTested runs the gate's self-test over root and answers its status and
// both streams.
func selfTested(t *testing.T, root string) (int, string, string) {
	t.Helper()
	var stdout, stderr bytes.Buffer
	code := Run(context.Background(), root, []string{"--selftest"}, &stdout, &stderr)
	return code, stdout.String(), stderr.String()
}

func TestASelfTestWhoseDetectorWentBlindFails(t *testing.T) {
	withOverrideKey(t, "never-written-in-any-configuration")

	root := plantRepo(t, map[string]string{"justfile": "ci:\n"})
	code, stdout, stderr := selfTested(t, root)

	if code != 1 {
		t.Fatalf("exit = %d, want 1; stderr = %q", code, stderr)
	}
	// The refusal names the case that broke: a bare FAIL would leave the
	// reader to re-derive the whole table.
	if !strings.Contains(stderr, "FAIL: active unsafe install") {
		t.Fatalf("stderr = %q, want the first case named", stderr)
	}
	// No PASS line beside it, or a CI log would carry both verdicts.
	if strings.Contains(stdout, "PASS") {
		t.Fatalf("stdout = %q, want no PASS alongside the failure", stdout)
	}
}

// A detector that stopped honouring a pinned-off override still passes every
// case ahead of that one, so the self-test names the later case rather than
// the first. That is what pins the table stopping at its first break.
func TestASelfTestNamesTheCaseThatBrokeNotTheFirst(t *testing.T) {
	withFalsyValues(t, nil)

	root := plantRepo(t, map[string]string{"justfile": "ci:\n"})
	code, stdout, stderr := selfTested(t, root)

	if code != 1 {
		t.Fatalf("exit = %d, want 1; stderr = %q", code, stderr)
	}
	if !strings.Contains(stderr, "FAIL: override pinned off") {
		t.Fatalf("stderr = %q, want the pinned-off case named", stderr)
	}
	if strings.Contains(stdout, "PASS") {
		t.Fatalf("stdout = %q, want no PASS alongside the failure", stdout)
	}
}

// A detector gone too broad fails the same way as one gone blind: the
// self-test watches both directions, so a key that matches an ordinary
// virtual-environment install is refused too.
func TestASelfTestWhoseDetectorWentTooBroadFails(t *testing.T) {
	withOverrideKey(t, "pip")

	root := plantRepo(t, map[string]string{"justfile": "ci:\n"})
	code, _, stderr := selfTested(t, root)

	if code != 1 {
		t.Fatalf("exit = %d, want 1; stderr = %q", code, stderr)
	}
	if !strings.Contains(stderr, "FAIL: ") {
		t.Fatalf("stderr = %q, want a named failing case", stderr)
	}
}

// The self-test answers on its own terms, ahead of the scope and its floor of
// four thousand files, so a blinded detector is refused as a self-test
// failure rather than reported as a collapsed scope.
func TestAFailingSelfTestIsAnsweredAheadOfTheFloor(t *testing.T) {
	withOverrideKey(t, "never-written-in-any-configuration")

	code, _, stderr := selfTested(t, t.TempDir())

	if code != 1 {
		t.Fatalf("exit = %d, want 1; stderr = %q", code, stderr)
	}
	if strings.Contains(stderr, "scope collapsed") {
		t.Fatalf("stderr = %q, want the self-test refusal rather than the floor", stderr)
	}
}
