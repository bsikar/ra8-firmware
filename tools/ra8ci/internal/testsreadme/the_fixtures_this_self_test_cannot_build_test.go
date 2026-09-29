// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package testsreadme

import (
	"context"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

// The gate's own self-test builds real fixture trees on disk and runs the
// scan over each one. Its failure arms are what an operator sees when the
// self-test cannot be trusted, and they were unreached: the arms only fire
// when a fixture cannot be built or the run is already over, neither of which
// happens on a healthy box.
//
// They matter because the self-test is what `--selftest` answers CI with. An
// arm that has never run is an arm that could report a fixture failure as a
// clean gate, which is the one reading that would let real drift through.

// selfTested runs the gate's self-test and hands back what it told each
// writer alongside its verdict.
func selfTested(ctx context.Context) (string, string, bool) {
	var out, errs strings.Builder
	ok := selfTest(ctx, &out, &errs)
	return out.String(), errs.String(), ok
}

// A run that is already over is reported as a cancelled self-test rather than
// as a gate that passed. The first case is abandoned before its fixture is
// built, so nothing is written to a temporary directory the run will not
// clean up.
func TestASelfTestOverBeforeItStartsIsReportedCancelled(t *testing.T) {
	ctx, stop := context.WithCancel(context.Background())
	stop()

	out, errs, ok := selfTested(ctx)
	if ok {
		t.Fatal("a cancelled self-test reported the gate healthy")
	}
	if !strings.Contains(errs, "selftest cancelled") {
		t.Fatalf("stderr=%q; want the cancellation named", errs)
	}
	if !strings.Contains(errs, "tests-readme selftest FAILED") {
		t.Fatalf("stderr=%q; want the failure headline", errs)
	}
	if strings.Contains(out, "selftest OK") {
		t.Fatalf("stdout=%q; a cancelled self-test still reported OK", out)
	}
}

// A healthy self-test says how much it covered, so an operator reading CI can
// tell a real pass from a pass over nothing.
func TestAHealthySelfTestSaysWhatItCovered(t *testing.T) {
	out, errs, ok := selfTested(context.Background())
	if !ok {
		t.Fatalf("the self-test failed on a healthy box: %s", errs)
	}
	for _, want := range []string{"selftest OK", "gitignore"} {
		if !strings.Contains(out, want) {
			t.Fatalf("stdout=%q; want %q named", out, want)
		}
	}
	if errs != "" {
		t.Fatalf("a passing self-test wrote %q to stderr", errs)
	}
}

// Each way a fixture cannot be built is handed back as an error rather than a
// half-written tree the scan would then judge. A fixture that is not there is
// not a clean fixture.
func TestAFixtureThatCannotBeBuiltIsHandedBack(t *testing.T) {
	t.Run("no directory to build it in", func(t *testing.T) {
		// A regular file where the root should be: the tests
		// directory cannot be made under it.
		root := filepath.Join(t.TempDir(), "root")
		if err := os.WriteFile(root, []byte("not a directory"), 0644); err != nil {
			t.Fatal(err)
		}
		if _, _, err := writeFixture(root, fixtureCase{name: "x", subdirs: []string{"alpha"}}); err == nil {
			t.Fatal("a fixture was built under a regular file")
		}
	})
	t.Run("a subdirectory named twice", func(t *testing.T) {
		_, _, err := writeFixture(t.TempDir(), fixtureCase{
			name: "x", subdirs: []string{"alpha", "alpha"},
		})
		if err == nil {
			t.Fatal("a repeated fixture subdirectory was accepted")
		}
	})
	t.Run("the README's name already taken", func(t *testing.T) {
		// A subdirectory called README.md leaves no name for the
		// README itself, so the write fails rather than the scan
		// reading a directory as documentation.
		_, _, err := writeFixture(t.TempDir(), fixtureCase{
			name: "x", subdirs: []string{"alpha", "README.md"},
		})
		if err == nil {
			t.Fatal("a README was written over a directory of the same name")
		}
	})
}

// A fixture that CAN be built carries every subdirectory it was asked for and
// a README row per documented name, which is what makes the refusals above
// refusals rather than the ordinary outcome.
func TestABuiltFixtureCarriesItsTreeAndItsRows(t *testing.T) {
	testsDir, readme, err := writeFixture(t.TempDir(), fixtureCase{
		name: "x", subdirs: []string{"alpha", "beta"}, documented: []string{"alpha"},
	})
	if err != nil {
		t.Fatalf("an ordinary fixture was refused: %v", err)
	}
	for _, name := range []string{"alpha", "beta"} {
		info, err := os.Stat(filepath.Join(testsDir, name))
		if err != nil || !info.IsDir() {
			t.Fatalf("subdirectory %s: %v", name, err)
		}
	}
	body, err := os.ReadFile(readme)
	if err != nil {
		t.Fatal(err)
	}
	if !strings.Contains(string(body), "| `alpha/` |") {
		t.Fatalf("README=%q; want a row for alpha", body)
	}
	if strings.Contains(string(body), "| `beta/` |") {
		t.Fatalf("README=%q; beta was documented without being asked for", body)
	}
}

// containsMessage answers over the whole list and says no when the needle is
// in none of it. The negative is the arm the self-test's drift cases lean on:
// a message list that does not name the expected path is a gate that stopped
// firing, and reporting it as a match would hide exactly that.
func TestAMessageListIsSearchedWholeAndCanSayNo(t *testing.T) {
	messages := []string{"tests/alpha/ is undocumented", "tests/beta/ is undocumented"}
	for _, needle := range []string{"tests/alpha/", "tests/beta/", "undocumented"} {
		if !containsMessage(messages, needle) {
			t.Fatalf("%q was not found in %v", needle, messages)
		}
	}
	for _, needle := range []string{"tests/gamma/", "TESTS/ALPHA/", "documented "} {
		if containsMessage(messages, needle) {
			t.Fatalf("%q was reported found in %v", needle, messages)
		}
	}
	if containsMessage(nil, "anything") {
		t.Fatal("an empty list answered found")
	}
}
