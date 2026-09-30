// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package github

import (
	"errors"
	"os"
	"strings"
	"testing"
)

// A correspondence file is read token by token, so every way the document can
// stop early has to end in a refusal that names the file. A half-read
// declaration accepted as a whole one would put a deployment on a
// correspondence nobody reviewed.
func TestACorrespondenceThatStopsEarlyIsRefusedAndNamesTheFile(t *testing.T) {
	for _, c := range []struct {
		name string
		body string
	}{
		{"nothing after the opening brace", `{`},
		{"a key and nothing else", `{"format"`},
		{"a key with no value", `{"format":`},
		{"a comma with nothing after it", `{"format": "lint-format",`},
		{"a second pair cut short", `{"format": "lint-format", "tidy"`},
		{"an unterminated job name", `{"format": "lint-form`},
	} {
		t.Run(c.name, func(t *testing.T) {
			path := writeCorrespondenceFile(t, c.body)
			t.Setenv(EnvShadowCorrespondenceFile, path)
			os.Unsetenv(EnvCheckRunMode)
			_, enabled, err := LoadCheckRunConfigFromEnv(catalogNames(t))
			if enabled {
				t.Fatal("a declaration that stops early enabled publishing")
			}
			if !errors.Is(err, ErrCheckRunConfigUnreadable) {
				t.Fatalf("want ErrCheckRunConfigUnreadable, got %v", err)
			}
			if !strings.Contains(err.Error(), path) {
				t.Fatalf("refusal does not name the file: %v", err)
			}
			if strings.Contains(err.Error(), "lint-form") {
				t.Fatalf("refusal quoted the declared job: %v", err)
			}
		})
	}
}

// The mode is read before the file, and a deployment that names no mode lands
// in shadow. A correspondence the catalog carries is the one case that enables
// publishing, and it must still be shadow unless authoritative was spelled out.
func TestAWholeCorrespondenceEnablesShadowPublishing(t *testing.T) {
	path := writeCorrespondenceFile(t, `{"format": "lint-format", "tidy": "lint-tidy"}`)
	t.Setenv(EnvShadowCorrespondenceFile, path)
	os.Unsetenv(EnvCheckRunMode)
	config, enabled, err := LoadCheckRunConfigFromEnv(catalogNames(t))
	if err != nil {
		t.Fatalf("a whole correspondence was refused: %v", err)
	}
	if !enabled {
		t.Fatal("a whole correspondence did not enable publishing")
	}
	if config.Mode != ModeShadow {
		t.Fatalf("mode %v, want shadow", config.Mode)
	}
	job, covered := config.Correspondence.Job("format")
	if !covered || job != "lint-format" {
		t.Fatalf("format is paired with %q covered=%v", job, covered)
	}
}
