// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package executor

import (
	"strings"
	"testing"
)

// The dispatch seam keeps a concatenated command away from a privileged
// boundary by fixing the program and putting the script on argv. The
// environment is the other way in: bash runs BASH_ENV before a script's first
// line, imports a BASH_FUNC_ entry as a shell function that can shadow any
// command the script calls, and the loader honours LD_PRELOAD whatever the
// program turns out to be. None of those is on argv, so every argv rule holds
// perfectly while the step runs somebody else's code.
//
// cleanEnvironment answers all of them the same way, by handing the step an
// allow list rather than filtering a deny list. That is the right shape, and
// it is also the kind of property that quietly stops holding: the allow list
// is a convenient place to add a key, and nothing in it says which additions
// would give the caller back the door. These pin the property by name.

// keysThatRunCode are the environment entries that get code running without
// appearing on argv. Each is a real mechanism, not a category.
var keysThatRunCode = []string{
	"BASH_ENV",        // sourced before a non-interactive bash script runs
	"ENV",             // the same door on a POSIX shell
	"BASH_FUNC_x%%",   // an exported shell function, free to shadow a command
	"SHELLOPTS",       // turns on shell options the script never asked for
	"PS4",             // expanded, so it executes under set -x
	"IFS",             // re-splits what the script reads
	"LD_PRELOAD",      // loaded ahead of the program's own libraries
	"LD_LIBRARY_PATH", // decides which libraries the program resolves
	"LD_AUDIT",        // an audit library the loader runs
	"GIT_SSH_COMMAND", // a command git runs on the step's behalf
	"GIT_EXTERNAL_DIFF",
	"GIT_PAGER",
	"PYTHONSTARTUP", // sourced by an interpreter the script may call
	"PERL5OPT",
}

func environmentFor(t *testing.T) map[string]string {
	t.Helper()
	items, err := cleanEnvironment(t.TempDir())
	if err != nil {
		t.Fatalf("cleanEnvironment: %v", err)
	}
	values := make(map[string]string, len(items))
	for _, item := range items {
		key, value, ok := strings.Cut(item, "=")
		if !ok {
			t.Fatalf("environment entry %q carries no value", item)
		}
		values[key] = value
	}
	return values
}

// No entry that runs code on the step's behalf reaches it, however ordinary
// the step's argv looks.
func TestTheEnvironmentNeverCarriesAKeyThatRunsCode(t *testing.T) {
	for _, key := range keysThatRunCode {
		t.Setenv(key, "echo owned")
	}

	environment := environmentFor(t)
	for _, key := range keysThatRunCode {
		if value, carried := environment[key]; carried {
			t.Errorf("the step was handed %s=%q", key, value)
		}
	}
}

// And the reason none of them arrives is the SHAPE, not a list of known-bad
// names: a key nobody has ever thought about is dropped exactly as firmly.
// Without this the test above would pass over a deny list that the next
// mechanism walks straight around.
func TestTheEnvironmentIsAnAllowListRatherThanAListOfKnownDoors(t *testing.T) {
	t.Setenv("RA8CI_A_KEY_NOBODY_LISTED", "carried")
	t.Setenv("AWS_SECRET_ACCESS_KEY", "carried")

	environment := environmentFor(t)
	for _, key := range []string{"RA8CI_A_KEY_NOBODY_LISTED", "AWS_SECRET_ACCESS_KEY"} {
		if _, carried := environment[key]; carried {
			t.Errorf("an unlisted key %s reached the step", key)
		}
	}
}

// The step still gets what it needs, so the refusals above are a filter rather
// than an empty environment that would pass them for the wrong reason.
func TestTheKeysAStepWasPromisedStillArrive(t *testing.T) {
	t.Setenv("LANG", "C.UTF-8")

	environment := environmentFor(t)
	if environment["LANG"] != "C.UTF-8" {
		t.Errorf("LANG = %q, want it carried through", environment["LANG"])
	}
	if environment["PATH"] == "" {
		t.Error("the step was handed no PATH")
	}
}

// The toolchain is the plane's call, not the caller's: whatever GOTOOLCHAIN
// the environment arrives with, the step gets local, once. A second entry
// would leave which one wins up to the reader.
func TestTheToolchainIsPinnedWhateverTheCallerAsksFor(t *testing.T) {
	t.Setenv("GOTOOLCHAIN", "go1.0.0+auto")

	items, err := cleanEnvironment(t.TempDir())
	if err != nil {
		t.Fatalf("cleanEnvironment: %v", err)
	}
	var found []string
	for _, item := range items {
		if strings.HasPrefix(item, "GOTOOLCHAIN=") {
			found = append(found, item)
		}
	}
	if len(found) != 1 || found[0] != "GOTOOLCHAIN=local" {
		t.Fatalf("GOTOOLCHAIN entries = %v, want exactly [GOTOOLCHAIN=local]", found)
	}
}
