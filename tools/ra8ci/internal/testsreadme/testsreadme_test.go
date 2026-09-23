// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package testsreadme

import (
	"bytes"
	"context"
	"path/filepath"
	"strings"
	"testing"
)

func TestDocumentedSubdirsReadsOnlyFirstMarkdownCell(t *testing.T) {
	readme := "" +
		"prose `not-a-row/`\n" +
		"| `core/` | mentions `fake/` in the description |\n" +
		"| ordinary | `also-fake/` |\n" +
		"| `host-tools/` | utilities |\n" +
		"| `bad name/` | invalid row name |\n"
	got := documentedSubdirs(readme)
	if len(got) != 2 {
		t.Fatalf("documented subdirectories = %v, want core and host-tools", got)
	}
	for _, name := range []string{"core", "host-tools"} {
		if _, ok := got[name]; !ok {
			t.Errorf("documented subdirectories omitted %q: %v", name, got)
		}
	}
}

func TestDriftProblemsReportsBothDirectionsInOrder(t *testing.T) {
	actual := map[string]struct{}{"alpha": {}, "gamma": {}}
	documented := map[string]struct{}{"beta": {}, "gamma": {}}
	got := driftProblems(actual, documented)
	want := []string{
		"tests/alpha/ exists but is not documented in tests/README.md -- add a table row whose first cell is `alpha/`",
		"tests/README.md documents tests/beta/ but no such subdirectory exists -- remove or rename that row",
	}
	if strings.Join(got, "\n") != strings.Join(want, "\n") {
		t.Fatalf("drift problems = %q, want %q", got, want)
	}
}

func TestSanitizedGitEnvironmentRemovesCallerRouting(t *testing.T) {
	got := sanitizedGitEnvironment([]string{
		"PATH=/bin", "HOME=/home/test", "GIT_DIR=/attacker", "GIT_INDEX_FILE=/attacker/index",
		"BASH_FUNC_git%%=evil", "BASH_ENV=/attacker/rc", "PAGER=evil",
	})
	joined := "\n" + strings.Join(got, "\n") + "\n"
	for _, forbidden := range []string{"GIT_DIR=", "GIT_INDEX_FILE=", "BASH_FUNC_git", "BASH_ENV=", "PAGER=evil", "GH_TOKEN="} {
		if strings.Contains(joined, forbidden) {
			t.Errorf("sanitized environment retained %q: %s", forbidden, joined)
		}
	}
	for _, required := range []string{"GIT_CONFIG_NOSYSTEM=1", "GIT_OPTIONAL_LOCKS=0", "GIT_TERMINAL_PROMPT=0", "GIT_CONFIG_COUNT=3", "PAGER=cat", "HOME=/home/test"} {
		if !strings.Contains(joined, "\n"+required+"\n") {
			t.Errorf("sanitized environment omitted %q: %s", required, joined)
		}
	}
}

func TestSelfTestCoversGitignoreAndDrift(t *testing.T) {
	var stdout, stderr bytes.Buffer
	if !selfTest(context.Background(), &stdout, &stderr) {
		t.Fatalf("selftest failed: stdout=%q stderr=%q", stdout.String(), stderr.String())
	}
	if !strings.Contains(stdout.String(), "4 cases plus the gitignore carve-out") || stderr.Len() != 0 {
		t.Fatalf("unexpected selftest output: stdout=%q stderr=%q", stdout.String(), stderr.String())
	}
}

func TestRunChecksCurrentRepository(t *testing.T) {
	root, err := filepath.Abs("../../../../")
	if err != nil {
		t.Fatal(err)
	}
	var stdout, stderr bytes.Buffer
	if code := Run(context.Background(), root, nil, &stdout, &stderr); code != exitOK {
		t.Fatalf("Run current repository = %d; stdout=%q stderr=%q", code, stdout.String(), stderr.String())
	}
	if !strings.Contains(stdout.String(), "tests/README.md OK:") || stderr.Len() != 0 {
		t.Fatalf("unexpected current repository output: stdout=%q stderr=%q", stdout.String(), stderr.String())
	}
}

func TestRunRejectsUnexpectedArguments(t *testing.T) {
	var stdout, stderr bytes.Buffer
	if code := Run(context.Background(), "/repo", []string{"extra"}, &stdout, &stderr); code != 2 {
		t.Fatalf("unexpected-argument exit = %d, want 2; stderr=%q", code, stderr.String())
	}
}
