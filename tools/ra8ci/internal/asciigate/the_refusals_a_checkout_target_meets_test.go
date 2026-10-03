// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package asciigate

import (
	"bytes"
	"context"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

// checkoutRoot plants a checkout holding one ordinary file, a directory, and a
// symlink to the file, which are the three things a target can turn out to be.
func checkoutRoot(t *testing.T) string {
	t.Helper()
	root := t.TempDir()
	if err := os.WriteFile(filepath.Join(root, "page.md"), []byte("ascii\n"), 0o644); err != nil {
		t.Fatalf("plant page: %v", err)
	}
	if err := os.Mkdir(filepath.Join(root, "section"), 0o755); err != nil {
		t.Fatalf("plant section: %v", err)
	}
	symlinkTest(t, filepath.Join(root, "page.md"), filepath.Join(root, "link.md"))
	return root
}

// A checkout target is a single file to rewrite, so a directory or a symlink
// standing where that file should be is refused rather than followed. Following
// a link here would let a target outside the checkout be rewritten.
func TestACheckoutTargetThatIsNotARegularFileIsRefused(t *testing.T) {
	root := checkoutRoot(t)
	for _, target := range []string{"section", "link.md"} {
		targets, err := checkoutTargets(root, target)
		if err == nil {
			t.Errorf("%s was accepted as a checkout target: %v", target, targets)
			continue
		}
		if !strings.Contains(err.Error(), "must be a regular file") {
			t.Errorf("%s was refused for the wrong reason: %v", target, err)
		}
	}
	if targets, err := checkoutTargets(root, "page.md"); err != nil || len(targets) != 1 || targets[0] != "page.md" {
		t.Fatalf("an ordinary file was not accepted: %v %v", targets, err)
	}
}

// The scan re-checks the target rather than trusting the list it was handed,
// because the two happen at different moments and the file can change between
// them.
func TestTheScanRefusesATargetThatIsNoLongerARegularFile(t *testing.T) {
	root := checkoutRoot(t)
	for _, target := range []string{"section", "link.md"} {
		if _, err := processCheckout(root, target, false); err == nil {
			t.Errorf("%s was scanned as a regular file", target)
		} else if !strings.Contains(err.Error(), "no longer a regular file") {
			t.Errorf("%s was refused for the wrong reason: %v", target, err)
		}
	}
}

// A named target that cannot be read is an error carrying the name, since a
// scan over many files is only useful if the failure says which one.
func TestAnUnreadableTargetIsAnErrorThatNamesIt(t *testing.T) {
	missing := filepath.Join(t.TempDir(), "absent.md")
	count, err := process(missing, false)
	if err == nil {
		t.Fatalf("a missing file was scanned and answered %d", count)
	}
	if !strings.Contains(err.Error(), "absent.md") {
		t.Fatalf("the error does not name the file it could not read: %v", err)
	}
}

// An empty target is refused before anything is stat-ed, so an unset argument
// never becomes a walk of the working directory.
func TestAnEmptyWalkTargetIsRefusedRatherThanWalked(t *testing.T) {
	targets, err := walkTargets("")
	if err == nil {
		t.Fatalf("an empty target was walked and answered %v", targets)
	}
	if !strings.Contains(err.Error(), "empty target") {
		t.Fatalf("the refusal does not name the empty target: %v", err)
	}
}

// The walk prunes the directories this gate never reads. Pruning matters more
// than filtering: a vendored tree can be large, and descending into it to
// discard the results afterwards is the slow way to the same answer.
func TestTheWalkPrunesTheDirectoriesItNeverReads(t *testing.T) {
	root := t.TempDir()
	for _, pruned := range []string{"third_party", "_deps", "build", "build-cov", "doxygen_theme", "fixtures"} {
		if err := os.MkdirAll(filepath.Join(root, pruned, "deep"), 0o755); err != nil {
			t.Fatalf("plant %s: %v", pruned, err)
		}
		if err := os.WriteFile(filepath.Join(root, pruned, "deep", "buried.md"), []byte("x\n"), 0o644); err != nil {
			t.Fatalf("plant %s: %v", pruned, err)
		}
	}
	if err := os.WriteFile(filepath.Join(root, "kept.md"), []byte("x\n"), 0o644); err != nil {
		t.Fatalf("plant kept: %v", err)
	}

	targets, err := walkTargets(root)
	if err != nil {
		t.Fatalf("walk refused: %v", err)
	}
	if len(targets) != 1 || filepath.Base(targets[0]) != "kept.md" {
		t.Fatalf("the walk read more than the file it should have: %v", targets)
	}
}

// A shebang is read off the file, so a path that cannot be opened is simply not
// a script. It must answer false rather than fail the derivation around it.
func TestAPathThatCannotBeOpenedIsNotAScript(t *testing.T) {
	if hasShellOrPythonShebang(filepath.Join(t.TempDir(), "absent")) {
		t.Fatal("a missing path was read as a script")
	}
}

// The self-test writes its own fixture into a temporary directory. When that
// directory cannot be made it fails rather than reporting on a fixture it never
// wrote. Pointing TMPDIR at an absent path reaches this on any box, including
// one running as root, where an unwritable directory would not.
func TestTheSelfTestFailsWhenItCannotBuildItsFixture(t *testing.T) {
	root := t.TempDir()
	absent := filepath.Join(t.TempDir(), "no-such-directory")
	t.Setenv("TMPDIR", absent)

	var stdout, stderr bytes.Buffer
	if selfTest(context.Background(), root, &stdout, &stderr) {
		t.Fatal("the self-test passed without a fixture to test against")
	}
}
