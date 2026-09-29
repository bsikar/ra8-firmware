// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package waverefs

import (
	"context"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"testing"
)

// The other half of the gate's honesty: the scope it derives from git before
// any of the rules in the_text_this_gate_will_read_test.go get a path to
// judge. A derivation that quietly returns nothing reports a clean tree.

func plantRepo(t *testing.T, files map[string]string) string {
	t.Helper()
	root := plantTree(t, files)
	command := exec.Command("git", "init", "-q", root)
	if out, err := command.CombinedOutput(); err != nil {
		t.Skipf("git init unavailable on this box: %v (%s)", err, out)
	}
	return root
}

func derivedScope(t *testing.T, root string) []string {
	t.Helper()
	paths, err := sourceFiles(context.Background(), root)
	if err != nil {
		t.Fatalf("sourceFiles: %v", err)
	}
	return paths
}

func holds(paths []string, want string) bool {
	for _, rel := range paths {
		if rel == want {
			return true
		}
	}
	return false
}

func TestTheDerivedScopeKeepsFirstPartyTextAndDropsTheRest(t *testing.T) {
	root := plantRepo(t, map[string]string{
		"README.md":                  "text\n",
		"infra/deploy.yml":           "text\n",
		"just/build.just":            "text\n",
		"justfile":                   "text\n",
		"apps/ui/main.c":             "text\n",
		"docs/notes.txt":             "text\n",
		"tools/ra8ci/main.go":        "text\n",
		"web/app.ts":                 "text\n",
		"libs/third_party/lvgl/lv.h": "text\n",
		"docs/reference/api.md":      "text\n",
		"build/out.md":               "text\n",
		"a/CMakeFiles/link.txt":      "text\n",
		"port/threadx/tx_api.c":      "text\n",
		"port/threadx/PORTING.md":    "text\n",
		"apps/shared_libs/third_party/cmsis/core.h": "text\n",
	})
	paths := derivedScope(t, root)
	for _, want := range []string{
		"README.md", "infra/deploy.yml", "just/build.just", "justfile",
		"apps/ui/main.c", "docs/notes.txt", "port/threadx/PORTING.md",
	} {
		if !holds(paths, want) {
			t.Errorf("scope omits %q: %v", want, paths)
		}
	}
	for _, unwanted := range []string{
		"tools/ra8ci/main.go", "web/app.ts",
		"libs/third_party/lvgl/lv.h", "apps/shared_libs/third_party/cmsis/core.h",
		"docs/reference/api.md", "build/out.md", "a/CMakeFiles/link.txt",
		"port/threadx/tx_api.c",
	} {
		if holds(paths, unwanted) {
			t.Errorf("scope keeps %q, want it dropped", unwanted)
		}
	}
}

func TestTheScopeIsSortedAndFreeOfDuplicates(t *testing.T) {
	root := plantRepo(t, map[string]string{
		"z.md": "text\n", "a.md": "text\n", "m/n.md": "text\n", "b.txt": "text\n",
	})
	paths := derivedScope(t, root)
	for index := 1; index < len(paths); index++ {
		if paths[index-1] >= paths[index] {
			t.Fatalf("scope not strictly sorted at %d: %v", index, paths)
		}
	}
}

func TestTheGateAlwaysScopesItsOwnSourceEvenThoughGoIsNotScanned(t *testing.T) {
	// waverefs.go is a .go file, which isScannedName refuses. The derivation
	// adds it back by name, and scan() then skips it by name: that pair is how
	// the gate's own examples stay out of its own report.
	root := plantRepo(t, map[string]string{
		self:        "// fixed in Wave 70\n",
		"README.md": "text\n",
	})
	paths := derivedScope(t, root)
	if !holds(paths, self) {
		t.Fatalf("scope omits the gate's own source: %v", paths)
	}
	found := scanned(t, root, paths...)
	if len(found) != 0 {
		t.Errorf("scan reported the gate's own source: %+v", found)
	}
}

func TestAnAbsentGateSourceIsSimplyNotScoped(t *testing.T) {
	root := plantRepo(t, map[string]string{"README.md": "text\n"})
	if paths := derivedScope(t, root); holds(paths, self) {
		t.Errorf("scope invents %q: %v", self, paths)
	}
}

func TestTheScopeFollowsGitIgnoreAndStillTakesUntrackedText(t *testing.T) {
	root := plantRepo(t, map[string]string{
		".gitignore":   "ignored.md\nsecret/\n",
		"ignored.md":   "text\n",
		"secret/x.md":  "text\n",
		"untracked.md": "text\n",
	})
	paths := derivedScope(t, root)
	if !holds(paths, "untracked.md") {
		t.Errorf("scope omits untracked first-party text: %v", paths)
	}
	for _, unwanted := range []string{"ignored.md", "secret/x.md"} {
		if holds(paths, unwanted) {
			t.Errorf("scope keeps ignored %q: %v", unwanted, paths)
		}
	}
}

func TestAPathThatIsNotARegularFileIsNotScoped(t *testing.T) {
	root := plantRepo(t, map[string]string{"real.md": "text\n"})
	link := filepath.Join(root, "link.md")
	if err := os.Symlink(filepath.Join(root, "missing.md"), link); err != nil {
		t.Skipf("symlink unavailable: %v", err)
	}
	if paths := derivedScope(t, root); holds(paths, "link.md") {
		t.Errorf("scope keeps a broken symlink: %v", paths)
	}
}

func TestADirectoryThatIsNotARepositoryIsAnErrorNotAnEmptyScope(t *testing.T) {
	root := t.TempDir()
	command := exec.Command("git", "-C", root, "rev-parse", "--git-dir")
	if err := command.Run(); err == nil {
		t.Skip("temp directory sits inside a git repository on this box")
	}
	paths, err := sourceFiles(context.Background(), root)
	if err == nil {
		t.Fatalf("sourceFiles succeeded with %d path(s), want an error", len(paths))
	}
	if !strings.Contains(err.Error(), "git ls-files") {
		t.Errorf("err = %v, want it to name git ls-files", err)
	}
	if paths != nil {
		t.Errorf("paths = %v, want nil beside the error", paths)
	}
}

func TestSelfTestPassesOnAScopeThatCarriesInfraAndJust(t *testing.T) {
	root := plantRepo(t, map[string]string{
		"infra/deploy.yml": "text\n",
		"just/build.just":  "text\n",
		"README.md":        "text\n",
	})
	var out, errs strings.Builder
	if !selfTest(context.Background(), root, &out, &errs) {
		t.Fatalf("selfTest failed: stderr %q", errs.String())
	}
	if !strings.Contains(out.String(), "PASS") {
		t.Errorf("stdout = %q, want the PASS line", out.String())
	}
	if !strings.Contains(out.String(), "scope has") {
		t.Errorf("stdout = %q, want the scope count", out.String())
	}
	if errs.String() != "" {
		t.Errorf("stderr = %q, want nothing from a self-test that held", errs.String())
	}
}

func TestSelfTestFailsWhenTheDerivedScopeOmitsInfraOrJust(t *testing.T) {
	// Losing a whole top-level directory is exactly the silent collapse this
	// detector exists to catch, so the self-test has to name it.
	cases := map[string]map[string]string{
		"no just/":  {"infra/deploy.yml": "text\n", "README.md": "text\n"},
		"no infra/": {"just/build.just": "text\n", "README.md": "text\n"},
		"neither":   {"README.md": "text\n"},
	}
	for label, files := range cases {
		root := plantRepo(t, files)
		var out, errs strings.Builder
		if selfTest(context.Background(), root, &out, &errs) {
			t.Errorf("%s: selfTest passed, want failure", label)
		}
		if !strings.Contains(errs.String(), "omits infra/ or just/") {
			t.Errorf("%s: stderr = %q, want the omission named", label, errs.String())
		}
		if strings.Contains(out.String(), "PASS") {
			t.Errorf("%s: stdout = %q, must not claim PASS", label, out.String())
		}
	}
}

func TestSelfTestFailsWhenTheScopeCannotBeDerived(t *testing.T) {
	root := t.TempDir()
	if err := exec.Command("git", "-C", root, "rev-parse", "--git-dir").Run(); err == nil {
		t.Skip("temp directory sits inside a git repository on this box")
	}
	var out, errs strings.Builder
	if selfTest(context.Background(), root, &out, &errs) {
		t.Fatal("selfTest passed with no repository, want failure")
	}
	if !strings.Contains(errs.String(), "scope error") {
		t.Errorf("stderr = %q, want the scope error named", errs.String())
	}
	if strings.Contains(out.String(), "PASS") {
		t.Errorf("stdout = %q, must not claim PASS", out.String())
	}
}

func TestRunSelfTestAnswersOneOrZeroByItsOutcome(t *testing.T) {
	good := plantRepo(t, map[string]string{
		"infra/deploy.yml": "text\n",
		"just/build.just":  "text\n",
	})
	var out, errs strings.Builder
	if code := Run(context.Background(), good, []string{"--selftest"}, &out, &errs); code != 0 {
		t.Errorf("code = %d, want 0 (stderr %q)", code, errs.String())
	}
	bad := plantRepo(t, map[string]string{"README.md": "text\n"})
	out.Reset()
	errs.Reset()
	if code := Run(context.Background(), bad, []string{"--selftest"}, &out, &errs); code != 1 {
		t.Errorf("code = %d, want 1", code)
	}
	// The self-test runs ahead of the scope floor, so a small tree answers 1
	// on its own terms rather than 2 for a collapsed scope.
	if strings.Contains(errs.String(), "floor is") {
		t.Errorf("stderr = %q, want the self-test outcome not the floor refusal", errs.String())
	}
}
