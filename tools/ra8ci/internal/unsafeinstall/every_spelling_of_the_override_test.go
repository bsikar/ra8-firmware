// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package unsafeinstall

import (
	"context"
	"os"
	"path/filepath"
	"testing"
)

// envName spells the environment variable in pieces, the way the detector
// does, so this test file is not itself a finding when the gate scans it.
func envName() string { return "PIP" + "_BREAK_SYSTEM_" + "PACKAGES" }

// scanFile writes one file into a temporary root and scans exactly it.
func scanFile(t *testing.T, rel, body string) []finding {
	t.Helper()
	root := t.TempDir()
	full := filepath.Join(root, filepath.FromSlash(rel))
	if err := os.MkdirAll(filepath.Dir(full), 0o755); err != nil {
		t.Fatalf("mkdir: %v", err)
	}
	if err := os.WriteFile(full, []byte(body), 0o644); err != nil {
		t.Fatalf("write: %v", err)
	}
	findings, err := scan(context.Background(), root, []string{rel})
	if err != nil {
		t.Fatalf("scan: %v", err)
	}
	return findings
}

func TestTheEnvironmentOverrideIsFound(t *testing.T) {
	findings := scanFile(t, "Dockerfile", "FROM debian\nENV "+envName()+"=1\nRUN pip install libclang\n")
	if len(findings) != 1 || findings[0].line != 2 {
		t.Fatalf("findings = %+v, want line 2", findings)
	}
}

func TestThePipConfigOverrideIsFound(t *testing.T) {
	findings := scanFile(t, "scripts/setup.sh", "#!/bin/sh\npython3 -m pip config set global."+overrideKey+" true\n")
	if len(findings) != 1 || findings[0].line != 2 {
		t.Fatalf("findings = %+v, want line 2", findings)
	}
}

func TestTheConfigFileKeyIsFound(t *testing.T) {
	findings := scanFile(t, "pip.conf", "[global]\n"+overrideKey+" = true\n")
	if len(findings) != 1 || findings[0].line != 2 {
		t.Fatalf("findings = %+v, want line 2", findings)
	}
}

func TestTheFlagIsStillFound(t *testing.T) {
	if got := scanText("python3 -m pip install " + forbidden + " libclang"); len(got) != 1 || got[0] != 1 {
		t.Fatalf("scanText() = %v, want [1]", got)
	}
}

func TestTheOverridePinnedOffIsNotAFinding(t *testing.T) {
	for _, line := range []string{
		envName() + "=0",
		envName() + ": \"false\"",
		"export " + envName() + "=off",
		"pip config set global." + overrideKey + " false",
		overrideKey + " = no",
	} {
		if statesTheOverride(line) {
			t.Errorf("statesTheOverride(%q) = true, want false: this line turns the override OFF", line)
		}
	}
}

func TestEverySpellingOfTheSameDecision(t *testing.T) {
	for _, line := range []string{
		"python3 -m pip install --" + overrideKey,
		"ENV " + envName() + "=1",
		"  " + envName() + ": \"1\"",
		"export " + envName() + "=true",
		"pip config set --global " + overrideKey + " true",
		overrideKey + "=yes",
		"PIP" + "_break_system_" + "packages=1",
	} {
		if !statesTheOverride(line) {
			t.Errorf("statesTheOverride(%q) = false, want true", line)
		}
	}
}

func TestUnrelatedLinesAreNotFindings(t *testing.T) {
	for _, line := range []string{
		"python3 -m venv .venv",
		".venv/bin/pip install libclang",
		"python3 -m pip --version",
		"# create a venv instead of overriding the system packages",
		"break the build if the system packages drift",
		"",
	} {
		if statesTheOverride(line) {
			t.Errorf("statesTheOverride(%q) = true, want false", line)
		}
	}
}

func TestTheValueIsReadPastQuotesAndSpacing(t *testing.T) {
	if statesTheOverride(envName() + "  =  '0'") {
		t.Error("a quoted, spaced falsy value still turns the override off")
	}
	if !statesTheOverride(envName() + "  =  '1'") {
		t.Error("a quoted, spaced truthy value is still the override")
	}
}

func TestAValueThatIsNotAFlagWordIsAFinding(t *testing.T) {
	if !statesTheOverride("pip install --" + overrideKey + " libclang") {
		t.Error("the flag followed by a package name is still the override")
	}
}

func TestEveryLineIsStillReported(t *testing.T) {
	text := "safe\n" + forbidden + "\nENV " + envName() + "=1"
	got := scanText(text)
	if len(got) != 2 || got[0] != 2 || got[1] != 3 {
		t.Fatalf("scanText() = %v, want [2 3]", got)
	}
}
