// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package github

import (
	"errors"
	"os"
	"strings"
	"testing"
)

// The correspondence is read with a token decoder under a limit reader, so
// the two ends of the file are judged separately from its pairs: a file that
// simply stops has no closing brace to find, and a file that parses to its
// end can still have spent more bytes than the bound allows. Neither may be
// read as a correspondence, because a half-read declaration would leave tasks
// silently uncompared.

// A declaration that stops before it closes is refused rather than taken for
// the pairs it happened to carry.
func TestACorrespondenceThatStopsBeforeItClosesIsRefused(t *testing.T) {
	for _, truncated := range []struct {
		name string
		body string
	}{
		{"cut after one pair", `{"format": "lint-format"`},
		{"cut after a comma", `{"format": "lint-format",`},
		{"cut inside the closing brace's whitespace", `{"format": "lint-format"` + "\n  "},
	} {
		t.Setenv(EnvShadowCorrespondenceFile, writeCorrespondenceFile(t, truncated.body))
		os.Unsetenv(EnvCheckRunMode)

		config, enabled, err := LoadCheckRunConfigFromEnv(catalogNames(t))
		if !errors.Is(err, ErrCheckRunConfigUnreadable) {
			t.Errorf("a declaration %s answered %v, want ErrCheckRunConfigUnreadable", truncated.name, err)
			continue
		}
		if enabled || config.Correspondence != nil {
			t.Errorf("a declaration %s enabled publishing anyway", truncated.name)
		}
	}
}

// The bound is judged on what the decoder actually spent, so a declaration
// that parses cleanly is still refused one byte past it, and accepted at it.
func TestTheCorrespondenceBoundIsJudgedOnWhatWasRead(t *testing.T) {
	// Padding sits inside the object as whitespace the decoder skips, so
	// the file grows to the bound without the declared job name growing
	// with it.
	const head = `{"format": "lint-format"`
	const tail = "}"
	padded := func(size int) string {
		return head + strings.Repeat(" ", size-len(head)-len(tail)) + tail
	}

	atTheBound := padded(maxCorrespondenceFileBytes)
	if len(atTheBound) != maxCorrespondenceFileBytes {
		t.Fatalf("the fixture at the bound is %d bytes", len(atTheBound))
	}
	t.Setenv(EnvShadowCorrespondenceFile, writeCorrespondenceFile(t, atTheBound))
	os.Unsetenv(EnvCheckRunMode)
	config, enabled, err := LoadCheckRunConfigFromEnv(catalogNames(t))
	if err != nil {
		t.Fatalf("a declaration exactly at the bound answered %v", err)
	}
	if !enabled || config.Correspondence == nil {
		t.Fatal("a declaration exactly at the bound did not enable publishing")
	}

	pastTheBound := padded(maxCorrespondenceFileBytes + 1)
	t.Setenv(EnvShadowCorrespondenceFile, writeCorrespondenceFile(t, pastTheBound))
	os.Unsetenv(EnvCheckRunMode)
	config, enabled, err = LoadCheckRunConfigFromEnv(catalogNames(t))
	if !errors.Is(err, ErrCheckRunConfigUnreadable) {
		t.Fatalf("a declaration one byte past the bound answered %v", err)
	}
	if !strings.Contains(err.Error(), "larger than") {
		t.Fatalf("the refusal past the bound reads %v, want the bound named", err)
	}
	if enabled || config.Correspondence != nil {
		t.Fatal("a declaration past the bound enabled publishing anyway")
	}
}
