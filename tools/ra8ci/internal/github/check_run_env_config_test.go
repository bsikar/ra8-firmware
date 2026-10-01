// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package github

import (
	"errors"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

func writeCorrespondenceFile(t *testing.T, body string) string {
	t.Helper()
	path := filepath.Join(t.TempDir(), "correspondence.json")
	if err := os.WriteFile(path, []byte(body), 0o600); err != nil {
		t.Fatalf("WriteFile: %v", err)
	}
	return path
}

func TestCheckRunConfigIsDisabledWhenNeitherVariableIsSet(t *testing.T) {
	t.Setenv(EnvShadowCorrespondenceFile, "")
	t.Setenv(EnvCheckRunMode, "")
	os.Unsetenv(EnvShadowCorrespondenceFile)
	os.Unsetenv(EnvCheckRunMode)
	config, enabled, err := LoadCheckRunConfigFromEnv(catalogNames(t))
	if err != nil || enabled {
		t.Fatalf("enabled=%v err=%v", enabled, err)
	}
	if config.Correspondence != nil {
		t.Fatal("a disabled configuration carries no correspondence")
	}
}

// The move onto the merge gate is spelled out or it does not happen. An
// operator who declares a correspondence and says nothing about the mode gets
// the one that cannot move a pull request.
func TestCheckRunConfigDefaultsToShadow(t *testing.T) {
	path := writeCorrespondenceFile(t, `{"format": "lint-format"}`)
	t.Setenv(EnvShadowCorrespondenceFile, path)
	os.Unsetenv(EnvCheckRunMode)
	config, enabled, err := LoadCheckRunConfigFromEnv(catalogNames(t))
	if err != nil || !enabled {
		t.Fatalf("enabled=%v err=%v", enabled, err)
	}
	if config.Mode != ModeShadow {
		t.Fatalf("mode = %s, want shadow", config.Mode)
	}
	job, covered := config.Correspondence.Job("format")
	if !covered || job != "lint-format" {
		t.Fatalf("Job(format) = %q %v", job, covered)
	}
}

func TestCheckRunConfigReadsAnAuthoritativeDeployment(t *testing.T) {
	path := writeCorrespondenceFile(t, `{"format": "lint-format", "tidy": "lint-tidy"}`)
	t.Setenv(EnvShadowCorrespondenceFile, path)
	t.Setenv(EnvCheckRunMode, "authoritative")
	config, enabled, err := LoadCheckRunConfigFromEnv(catalogNames(t))
	if err != nil || !enabled {
		t.Fatalf("enabled=%v err=%v", enabled, err)
	}
	if config.Mode != ModeAuthoritative {
		t.Fatalf("mode = %s", config.Mode)
	}
	if tasks := config.Correspondence.Tasks(); len(tasks) != 2 {
		t.Fatalf("tasks = %v", tasks)
	}
}

// A mode this build will not run refuses before anything is opened, so the
// path is never read on the way to rejecting the deployment.
func TestCheckRunConfigRefusesAnUnknownModeWithoutReadingTheFile(t *testing.T) {
	t.Setenv(EnvShadowCorrespondenceFile, filepath.Join(t.TempDir(), "absent.json"))
	t.Setenv(EnvCheckRunMode, "required")
	_, enabled, err := LoadCheckRunConfigFromEnv(catalogNames(t))
	if enabled {
		t.Fatal("a refused configuration is not enabled")
	}
	if !IsEnvConfigError(err) {
		t.Fatalf("want an EnvConfigError, got %v", err)
	}
	if errors.Is(err, ErrCheckRunConfigUnreadable) {
		t.Fatalf("the file must not have been opened: %v", err)
	}
	if !strings.Contains(err.Error(), EnvCheckRunMode) {
		t.Fatalf("error should name the variable, got %v", err)
	}
}

func TestCheckRunConfigRequiresACorrespondenceOnceConfigured(t *testing.T) {
	for _, testCase := range []struct {
		name string
		set  bool
		path string
	}{
		{"mode alone", false, ""},
		{"empty path", true, ""},
		{"blank path", true, "   "},
	} {
		t.Run(testCase.name, func(t *testing.T) {
			if testCase.set {
				t.Setenv(EnvShadowCorrespondenceFile, testCase.path)
			} else {
				os.Unsetenv(EnvShadowCorrespondenceFile)
			}
			t.Setenv(EnvCheckRunMode, "shadow")
			_, enabled, err := LoadCheckRunConfigFromEnv(catalogNames(t))
			if enabled {
				t.Fatal("a configuration with nothing to compare is not enabled")
			}
			if !IsEnvConfigError(err) {
				t.Fatalf("want an EnvConfigError, got %v", err)
			}
			if !strings.Contains(err.Error(), EnvShadowCorrespondenceFile) {
				t.Fatalf("error should name the variable, got %v", err)
			}
		})
	}
}

// encoding/json resolves a repeated key by keeping the last one, so the
// correspondence reviewed in the diff would not be the one running. The file
// is read key by key to refuse it instead.
func TestCheckRunConfigRefusesATaskDeclaredTwice(t *testing.T) {
	path := writeCorrespondenceFile(t, `{"format": "lint-format", "format": "other-job"}`)
	t.Setenv(EnvShadowCorrespondenceFile, path)
	os.Unsetenv(EnvCheckRunMode)
	_, _, err := LoadCheckRunConfigFromEnv(catalogNames(t))
	if !errors.Is(err, ErrCheckRunConfigUnreadable) {
		t.Fatalf("want ErrCheckRunConfigUnreadable, got %v", err)
	}
	if !strings.Contains(err.Error(), "format") || !strings.Contains(err.Error(), "twice") {
		t.Fatalf("error should name the repeated task, got %v", err)
	}
}

func TestCheckRunConfigRefusesAFileThatIsNotACorrespondence(t *testing.T) {
	cases := []struct {
		name string
		body string
	}{
		{"empty file", ""},
		{"an array", `["format"]`},
		{"a bare string", `"format"`},
		{"a job that is not a string", `{"format": 7}`},
		{"a job that is an object", `{"format": {"job": "lint"}}`},
		{"truncated object", `{"format": "lint-format"`},
		{"trailing document", `{"format": "lint-format"} {"tidy": "lint-tidy"}`},
	}
	for _, testCase := range cases {
		t.Run(testCase.name, func(t *testing.T) {
			t.Setenv(EnvShadowCorrespondenceFile, writeCorrespondenceFile(t, testCase.body))
			os.Unsetenv(EnvCheckRunMode)
			_, enabled, err := LoadCheckRunConfigFromEnv(catalogNames(t))
			if enabled {
				t.Fatal("a file that is not a correspondence does not enable publishing")
			}
			if !errors.Is(err, ErrCheckRunConfigUnreadable) {
				t.Fatalf("want ErrCheckRunConfigUnreadable, got %v", err)
			}
		})
	}
}

func TestCheckRunConfigRefusesAnAbsentFile(t *testing.T) {
	path := filepath.Join(t.TempDir(), "absent.json")
	t.Setenv(EnvShadowCorrespondenceFile, path)
	os.Unsetenv(EnvCheckRunMode)
	_, _, err := LoadCheckRunConfigFromEnv(catalogNames(t))
	if !errors.Is(err, ErrCheckRunConfigUnreadable) {
		t.Fatalf("want ErrCheckRunConfigUnreadable, got %v", err)
	}
	if !strings.Contains(err.Error(), path) {
		t.Fatalf("error should name the path, got %v", err)
	}
}

// The catalog check happens at startup, not at the first comparison.
func TestCheckRunConfigRefusesADeclarationTheCatalogDoesNotCarry(t *testing.T) {
	path := writeCorrespondenceFile(t, `{"no-such-task": "lint-format"}`)
	t.Setenv(EnvShadowCorrespondenceFile, path)
	os.Unsetenv(EnvCheckRunMode)
	_, enabled, err := LoadCheckRunConfigFromEnv(catalogNames(t))
	if enabled {
		t.Fatal("a correspondence naming no real task does not enable publishing")
	}
	if !errors.Is(err, ErrShadowCorrespondenceInvalid) {
		t.Fatalf("want ErrShadowCorrespondenceInvalid, got %v", err)
	}
	if !strings.Contains(err.Error(), path) {
		t.Fatalf("error should name the file it came from, got %v", err)
	}
}

// A declaration larger than the bound is refused rather than read.
func TestCheckRunConfigRefusesAnOversizedFile(t *testing.T) {
	body := `{"format": "` + strings.Repeat("j", maxCorrespondenceFileBytes) + `"}`
	t.Setenv(EnvShadowCorrespondenceFile, writeCorrespondenceFile(t, body))
	os.Unsetenv(EnvCheckRunMode)
	_, enabled, err := LoadCheckRunConfigFromEnv(catalogNames(t))
	if enabled {
		t.Fatal("an oversized declaration does not enable publishing")
	}
	if err == nil {
		t.Fatal("want a refusal")
	}
}

// A refusal names the variable, the path or the task. It must never carry the
// file's contents, which is where a job name an operator has not reviewed
// would first reach a log.
func TestCheckRunConfigRefusalsDoNotQuoteJobNames(t *testing.T) {
	const secret = "job-name-that-should-not-be-echoed"
	path := writeCorrespondenceFile(t, `{"no-such-task": "`+secret+`"}`)
	t.Setenv(EnvShadowCorrespondenceFile, path)
	os.Unsetenv(EnvCheckRunMode)
	_, _, err := LoadCheckRunConfigFromEnv(catalogNames(t))
	if err == nil {
		t.Fatal("want a refusal")
	}
	if strings.Contains(err.Error(), secret) {
		t.Fatalf("refusal quoted the declared job: %v", err)
	}
}
