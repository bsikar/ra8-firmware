// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package testsreadme

import (
	"bytes"
	"context"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"testing"
)

// The self-test builds its fixtures under the temp directory and then asks
// Git what is ignored there. Git answers from the closest repository above
// the path it is handed, so a temp directory that happens to sit inside a
// repository puts that repository's ignore rules over the fixtures. A rule
// covering a fixture name makes a subdirectory invisible to the scan, and
// the self-test's own expectations stop describing what the gate did.
//
// The only safe outcome is a loud failure. A self-test that quietly passes
// under borrowed ignore rules certifies a detector nobody exercised.
func repoTempDir(t *testing.T, ignore string) {
	t.Helper()
	git := trustedGitForTest(t)
	root := t.TempDir()
	if output, err := exec.Command(git, "init", "-q", root).CombinedOutput(); err != nil {
		t.Skipf("git init: %v: %s", err, strings.TrimSpace(string(output)))
	}
	if err := os.WriteFile(filepath.Join(root, ".gitignore"), []byte(ignore), 0o600); err != nil {
		t.Fatal(err)
	}
	setSelfTestTempDir(t, root)
}

func selfTestUnder(t *testing.T) (bool, string, string) {
	t.Helper()
	stdout, stderr := &bytes.Buffer{}, &bytes.Buffer{}
	held := selfTest(context.Background(), stdout, stderr)
	return held, stdout.String(), stderr.String()
}

// A rule covering one fixture name hides that subdirectory from the scan,
// so the cases stop answering what they were written to expect. The
// self-test has to report that rather than read it as agreement.
func TestASelfTestUnderBorrowedIgnoreRulesFailsLoudly(t *testing.T) {
	repoTempDir(t, "gamma/\n")

	held, stdout, stderr := selfTestUnder(t)
	if held {
		t.Fatal("the self-test passed with a fixture subdirectory hidden from it")
	}
	if !strings.Contains(stderr, "tests-readme selftest FAILED:") {
		t.Fatalf("the failure was not announced: %q", stderr)
	}
	if !strings.Contains(stderr, "expected") {
		t.Fatalf("the failure did not say which expectation broke: %q", stderr)
	}
	if stdout != "" {
		t.Fatalf("a failed self-test still reported OK: %q", stdout)
	}
}

// The contrast that makes the case above mean something: a temp directory
// inside a repository is not itself the problem, so with no rule covering a
// fixture name the self-test passes exactly as it does anywhere else.
func TestASelfTestInsideARepositoryWithNoMatchingRulePasses(t *testing.T) {
	repoTempDir(t, "unrelated-name/\n")

	held, stdout, stderr := selfTestUnder(t)
	if !held {
		t.Fatalf("an unrelated ignore rule failed the self-test: %q", stderr)
	}
	if !strings.Contains(stdout, "selftest OK:") || stderr != "" {
		t.Fatalf("a passing self-test did not announce itself cleanly: stdout=%q stderr=%q", stdout, stderr)
	}
}
