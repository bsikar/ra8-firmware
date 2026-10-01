// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package testsreadme

import (
	"bytes"
	"context"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

// The self-test builds every fixture it judges in a fresh temporary directory,
// so a box whose temporary directory is unusable can build none of them. The
// honest answer there is a named failure, not a pass: a self-test that cannot
// construct its own evidence has proved nothing about the gate, and a green
// CI line over that is worse than a red one.
//
// TMPDIR is what decides where those fixtures land, so pointing it at a path
// that is not there is the one wedge on this box that makes fixture
// construction fail without touching the gate itself.

// A self-test that could not build a single fixture fails, names the fixture
// it could not build, and never prints its success line.
func TestASelfTestWithNoUsableTempDirectoryIsFailedNotPassed(t *testing.T) {
	absent := filepath.Join(t.TempDir(), "not-a-directory")
	t.Setenv("TMPDIR", absent)

	var stdout, stderr bytes.Buffer
	code := Run(context.Background(), t.Name(), []string{"--selftest"}, &stdout, &stderr)

	if code != exitDrift {
		t.Fatalf("exit %d, want %d; stdout=%q stderr=%q", code, exitDrift, stdout.String(), stderr.String())
	}
	for _, want := range []string{"tests-readme selftest FAILED", "create selftest fixture"} {
		if !strings.Contains(stderr.String(), want) {
			t.Fatalf("stderr=%q; want %q named", stderr.String(), want)
		}
	}
	if strings.Contains(stdout.String(), "selftest OK") {
		t.Fatalf("stdout=%q; a self-test that built nothing announced success", stdout.String())
	}
}

// The gitignore carve-out builds its own repository fixture, and it is checked
// even when the fixture loop above has already broken. So the same unusable
// temporary directory is reported twice, once per half of the self-test, which
// is what tells an operator the box is at fault rather than one case.
func TestBothHalvesOfTheSelfTestNameTheFixtureTheyCouldNotBuild(t *testing.T) {
	absent := filepath.Join(t.TempDir(), "not-a-directory")
	t.Setenv("TMPDIR", absent)

	var stdout, stderr bytes.Buffer
	if selfTest(context.Background(), &stdout, &stderr) {
		t.Fatalf("the self-test passed with no usable temporary directory: %q", stdout.String())
	}
	for _, want := range []string{"create selftest fixture", "create gitignore fixture"} {
		if !strings.Contains(stderr.String(), want) {
			t.Fatalf("stderr=%q; want %q named", stderr.String(), want)
		}
	}
}

// The carve-out hands its failure back as an error rather than swallowing it,
// which is what lets the self-test above list it beside the other failures.
func TestTheGitignoreCarveOutHandsBackAFixtureItCannotBuild(t *testing.T) {
	absent := filepath.Join(t.TempDir(), "not-a-directory")
	t.Setenv("TMPDIR", absent)

	err := selfTestGitignore(context.Background())
	if err == nil {
		t.Fatal("the gitignore carve-out reported success with no usable temporary directory")
	}
	if !strings.Contains(err.Error(), "create gitignore fixture") {
		t.Fatalf("error = %q; want the fixture it could not build named", err)
	}
}

// And the wedge is the temporary directory, not the gate: restore a usable one
// and both halves pass again. Without this the tests above would hold just as
// well against a self-test that always failed.
func TestTheSelfTestPassesAgainOnceTheTempDirectoryIsUsable(t *testing.T) {
	usable := t.TempDir()
	if err := os.MkdirAll(usable, 0755); err != nil {
		t.Fatal(err)
	}
	t.Setenv("TMPDIR", usable)

	var stdout, stderr bytes.Buffer
	if !selfTest(context.Background(), &stdout, &stderr) {
		t.Fatalf("the self-test failed over a usable temporary directory: %s", stderr.String())
	}
	if !strings.Contains(stdout.String(), "selftest OK") {
		t.Fatalf("stdout=%q; want the success line", stdout.String())
	}
}
