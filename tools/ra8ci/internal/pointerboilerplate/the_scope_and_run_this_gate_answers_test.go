package pointerboilerplate

import (
	"bytes"
	"context"
	"fmt"
	"io"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"testing"
)

// The gate's detector was already held both directions. What was not held is
// everything around it: which files the gate decides to read, and what Run
// answers once it has them. These tests take that, including the floor, which
// is the branch that actually protects CI from a scope that silently
// collapsed to nothing.

func plantTree(t *testing.T, files map[string]string) string {
	t.Helper()
	root := t.TempDir()
	for rel, body := range files {
		full := filepath.Join(root, filepath.FromSlash(rel))
		if err := os.MkdirAll(filepath.Dir(full), 0o755); err != nil {
			t.Fatalf("plant %s: %v", rel, err)
		}
		if err := os.WriteFile(full, []byte(body), 0o644); err != nil {
			t.Fatalf("plant %s: %v", rel, err)
		}
	}
	return root
}

// git ls-files --others --exclude-standard answers for untracked files, so an
// empty repository is enough: no commit, and no git identity configuration.
func plantRepo(t *testing.T, files map[string]string) string {
	t.Helper()
	if _, err := exec.LookPath("git"); err != nil {
		t.Skip("git is unavailable on this box")
	}
	root := plantTree(t, files)
	if out, err := exec.Command("git", "init", "-q", root).CombinedOutput(); err != nil {
		t.Fatalf("git init: %v: %s", err, out)
	}
	return root
}

func derivedScope(t *testing.T, root string) []string {
	t.Helper()
	paths, err := scopedFiles(context.Background(), root)
	if err != nil {
		t.Fatalf("scopedFiles: %v", err)
	}
	return paths
}

func holds(t *testing.T, got []string, want ...string) {
	t.Helper()
	if strings.Join(got, ",") != strings.Join(want, ",") {
		t.Fatalf("scope = %v, want %v", got, want)
	}
}

// A tree large enough to clear the floor, so Run's reporting branches are
// reachable. The bodies are empty, which the detector reads as one blank line
// and no finding.
func plantFullScope(t *testing.T, extra map[string]string) string {
	t.Helper()
	files := map[string]string{}
	for i := 0; i < minimumScopedFiles; i++ {
		files[fmt.Sprintf("apps/unit%04d.c", i)] = ""
	}
	for rel, body := range extra {
		files[rel] = body
	}
	return plantRepo(t, files)
}

type ran struct {
	code   int
	stdout string
	stderr string
}

func run(t *testing.T, ctx context.Context, root string, args ...string) ran {
	t.Helper()
	var out, errs bytes.Buffer
	code := Run(ctx, root, args, &out, &errs)
	return ran{code: code, stdout: out.String(), stderr: errs.String()}
}

func TestOnlyAppAndExampleSourceIsScoped(t *testing.T) {
	root := plantRepo(t, map[string]string{
		"apps/blink/main.c":        "",
		"apps/blink/main.h":        "",
		"examples/hello/hello.cc":  "",
		"examples/hello/hello.mm":  "",
		"src/driver.c":             "",
		"libs/board/board.h":       "",
		"port/threadx/port.c":      "",
		"apps/blink/build.py":      "",
		"apps/blink/README.md":     "",
		"apps/blink/Makefile":      "",
		"examples/hello/notes.txt": "",
	})
	holds(t, derivedScope(t, root),
		"apps/blink/main.c", "apps/blink/main.h",
		"examples/hello/hello.cc", "examples/hello/hello.mm")
}

// The extension is lowered before it is looked up, so a shouted extension is
// still C source. The prefix is not lowered, so a shouted directory is not.
func TestAShoutedExtensionIsStillSourceButAShoutedPrefixIsNotTheScope(t *testing.T) {
	root := plantRepo(t, map[string]string{
		"apps/legacy.C":     "",
		"apps/legacy.CPP":   "",
		"apps/legacy.HXX":   "",
		"APPS/legacy.c":     "",
		"Examples/hello.cc": "",
	})
	holds(t, derivedScope(t, root), "apps/legacy.C", "apps/legacy.CPP", "apps/legacy.HXX")
}

// The two prefixes carry their own trailing slash, which is what keeps a
// sibling directory whose name merely starts with "apps" out of scope, and
// HasPrefix is anchored at the start, which keeps a scoped directory nested
// under a vendor tree out too.
func TestThePrefixIsAnchoredAtTheStartAndCarriesItsOwnSlash(t *testing.T) {
	root := plantRepo(t, map[string]string{
		"apps/main.c":                 "",
		"examples/main.c":             "",
		"appsuite/main.c":             "",
		"examples-old/main.c":         "",
		"vendor/apps/main.c":          "",
		"third_party/examples/main.c": "",
	})
	holds(t, derivedScope(t, root), "apps/main.c", "examples/main.c")
}

func TestEveryDeclaredSourceExtensionIsScopedAndNothingElseIs(t *testing.T) {
	files := map[string]string{}
	var want []string
	for _, ext := range []string{".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp", ".hxx", ".m", ".mm"} {
		rel := "apps/unit" + ext
		files[rel] = ""
		want = append(want, rel)
	}
	for _, ext := range []string{".go", ".rs", ".s", ".S", ".asm", ".inc", ".ld", ".py", ".md", ".json", ""} {
		files["apps/other"+ext+".keep"+ext] = ""
	}
	files["apps/noextension"] = ""
	root := plantRepo(t, files)
	got := derivedScope(t, root)
	if len(got) != len(want) {
		t.Fatalf("scope = %v, want the %d declared extensions", got, len(want))
	}
	for _, rel := range want {
		found := false
		for _, have := range got {
			found = found || have == rel
		}
		if !found {
			t.Fatalf("%s is not in scope %v", rel, got)
		}
	}
}

// A path git reports that is not a regular file must be dropped rather than
// handed to the reader, which would fail the whole scan on a dangling link.
func TestAPathThatIsNotARegularFileNeverEntersTheScope(t *testing.T) {
	root := plantRepo(t, map[string]string{"apps/real.c": ""})
	if err := os.Symlink(filepath.Join(root, "apps", "absent.c"), filepath.Join(root, "apps", "broken.c")); err != nil {
		t.Skipf("symlinks are unavailable here: %v", err)
	}
	if err := os.MkdirAll(filepath.Join(root, "apps", "directory.c"), 0o755); err != nil {
		t.Fatalf("plant directory: %v", err)
	}
	holds(t, derivedScope(t, root), "apps/real.c")
}

func TestTheScopeIsSortedAndCarriesEachPathOnce(t *testing.T) {
	root := plantRepo(t, map[string]string{
		"apps/zeta.c":  "",
		"apps/alpha.c": "",
		"apps/mid.c":   "",
		"examples/a.c": "",
	})
	holds(t, derivedScope(t, root), "apps/alpha.c", "apps/mid.c", "apps/zeta.c", "examples/a.c")
}

// Untracked source is judged, because the gate has to catch a file before it
// is committed, but an ignored file is not.
func TestUntrackedSourceIsJudgedAndIgnoredSourceIsNot(t *testing.T) {
	root := plantRepo(t, map[string]string{
		".gitignore":         "apps/generated/\napps/ignored.c\n",
		"apps/fresh.c":       "",
		"apps/ignored.c":     "",
		"apps/generated/g.c": "",
	})
	holds(t, derivedScope(t, root), "apps/fresh.c")
}

// A root that is not a repository must be an error naming the command, never
// an empty scope, which the floor would then report as a collapse with no
// explanation of the real cause.
func TestARootThatIsNotARepositoryIsAnErrorNotAnEmptyScope(t *testing.T) {
	root := plantTree(t, map[string]string{"apps/main.c": ""})
	paths, err := scopedFiles(context.Background(), root)
	if err == nil {
		t.Fatalf("a non-repository yielded a scope: %v", paths)
	}
	if paths != nil {
		t.Fatalf("a refused derivation still handed back %v", paths)
	}
	if !strings.Contains(err.Error(), "git ls-files") {
		t.Fatalf("error %q does not name the command that failed", err)
	}
}

func TestADerivationIsRefusedOnceItsContextIsCancelled(t *testing.T) {
	root := plantRepo(t, map[string]string{"apps/main.c": ""})
	ctx, cancel := context.WithCancel(context.Background())
	cancel()
	if _, err := scopedFiles(ctx, root); err == nil {
		t.Fatal("a cancelled derivation was answered")
	}
}

func TestTheSelfTestPassesOnItsOwnBothDirectionCases(t *testing.T) {
	got := run(t, context.Background(), plantRepo(t, nil), "--selftest")
	if got.code != 0 {
		t.Fatalf("--selftest = %d, want 0 (stderr %q)", got.code, got.stderr)
	}
	if !strings.Contains(got.stdout, "--selftest: PASS (7 both-direction cases)") {
		t.Fatalf("stdout = %q, want the PASS line naming its case count", got.stdout)
	}
	if got.stderr != "" {
		t.Fatalf("a passing self-test wrote to stderr: %q", got.stderr)
	}
}

// The self-test is judged on its own before the scope is derived, so it
// answers over a root that is not a repository at all.
func TestTheSelfTestIsAnsweredWithoutEverDerivingAScope(t *testing.T) {
	got := run(t, context.Background(), plantTree(t, nil), "--selftest")
	if got.code != 0 {
		t.Fatalf("--selftest over a non-repository = %d, want 0 (stderr %q)", got.code, got.stderr)
	}
}

func TestEveryOtherInvocationShapeIsRefusedWithTheUsageLine(t *testing.T) {
	root := plantRepo(t, map[string]string{"apps/main.c": ""})
	for _, args := range [][]string{
		{"--selftest", "extra"},
		{"extra", "--selftest"},
		{"--help"},
		{"-x"},
		{""},
		{"apps/main.c"},
	} {
		got := run(t, context.Background(), root, args...)
		if got.code != 2 {
			t.Fatalf("Run(%q) = %d, want 2", args, got.code)
		}
		if !strings.Contains(got.stderr, "usage: ra8ci pointer-boilerplate [--selftest]") {
			t.Fatalf("Run(%q) stderr = %q, want the usage line", args, got.stderr)
		}
		if got.stdout != "" {
			t.Fatalf("Run(%q) wrote to stdout: %q", args, got.stdout)
		}
	}
}

// A scope that collapsed is the failure this floor exists to catch: reporting
// a two-file tree as clean would let a broken checkout pass CI silently.
func TestACollapsedScopeIsRefusedRatherThanReportedClean(t *testing.T) {
	got := run(t, context.Background(), plantRepo(t, map[string]string{
		"apps/main.c": "", "examples/hello.c": "",
	}))
	if got.code != 2 {
		t.Fatalf("a two-file scope = %d, want 2", got.code)
	}
	if !strings.Contains(got.stderr, "scope collapsed to 2 file(s); expected at least 850") {
		t.Fatalf("stderr = %q, want the floor naming both counts", got.stderr)
	}
	if strings.Contains(got.stdout, "clean") {
		t.Fatalf("a collapsed scope was reported clean: %q", got.stdout)
	}
}

func TestAnEmptyScopeIsRefusedByTheFloorTooRatherThanPassing(t *testing.T) {
	got := run(t, context.Background(), plantRepo(t, map[string]string{"src/driver.c": ""}))
	if got.code != 2 || !strings.Contains(got.stderr, "scope collapsed to 0 file(s)") {
		t.Fatalf("an empty scope = %d, stderr %q", got.code, got.stderr)
	}
}

func TestAFullScopeWithNoGeneratedCommentIsReportedCleanWithItsCount(t *testing.T) {
	got := run(t, context.Background(), plantFullScope(t, nil))
	if got.code != 0 {
		t.Fatalf("a clean full scope = %d, want 0 (stderr %q)", got.code, got.stderr)
	}
	want := fmt.Sprintf("clean (%d app/example source files)", minimumScopedFiles)
	if !strings.Contains(got.stdout, want) {
		t.Fatalf("stdout = %q, want it to contain %q", got.stdout, want)
	}
	if got.stderr != "" {
		t.Fatalf("a clean run wrote to stderr: %q", got.stderr)
	}
}

func TestAGeneratedCommentIsReportedAtItsPathAndLineWithTheRemedy(t *testing.T) {
	got := run(t, context.Background(), plantFullScope(t, map[string]string{
		"apps/blink/main.c": "#include <stdint.h>\n\n/* See the public header for the documented contract. */\nvoid blink(void);\n",
		"examples/hi/hi.h":  "// See the public header for the documented contract.\n",
	}))
	if got.code != 1 {
		t.Fatalf("a scope carrying boilerplate = %d, want 1 (stderr %q)", got.code, got.stderr)
	}
	for _, want := range []string{
		"Generated pointer-only definition comment(s):",
		"  apps/blink/main.c:3",
		"  examples/hi/hi.h:1",
		"Delete the comment; the declaration owns the contract.",
	} {
		if !strings.Contains(got.stderr, want) {
			t.Fatalf("stderr = %q, want it to contain %q", got.stderr, want)
		}
	}
	if strings.Contains(got.stdout, "clean") {
		t.Fatalf("a run with findings reported clean: %q", got.stdout)
	}
}

// Findings are reported in scope order, which is sorted, so two runs over the
// same tree name the same file first.
func TestFindingsAreReportedInScopeOrder(t *testing.T) {
	got := run(t, context.Background(), plantFullScope(t, map[string]string{
		"examples/z.c": "/* see header for the documented contract. */\n",
		"apps/a.c":     "/* see header for the documented contract. */\n",
	}))
	if got.code != 1 {
		t.Fatalf("= %d, want 1", got.code)
	}
	first := strings.Index(got.stderr, "apps/a.c:1")
	second := strings.Index(got.stderr, "examples/z.c:1")
	if first < 0 || second < 0 || first > second {
		t.Fatalf("findings out of scope order: %q", got.stderr)
	}
}

// The scan runs before the floor is judged, so an unreadable file is reported
// as a scan failure even in a tree far too small to pass the floor. That
// ordering is what tells an operator which of the two actually went wrong.
func TestAnUndecodableSourceFileIsRefusedAheadOfTheFloor(t *testing.T) {
	root := plantRepo(t, map[string]string{"apps/main.c": "\xff\xfe not utf-8\n"})
	got := run(t, context.Background(), root)
	if got.code != 2 {
		t.Fatalf("= %d, want 2", got.code)
	}
	if !strings.Contains(got.stderr, "cannot scan source tree") || !strings.Contains(got.stderr, "invalid UTF-8") {
		t.Fatalf("stderr = %q, want the scan refusal naming the decode", got.stderr)
	}
	if strings.Contains(got.stderr, "scope collapsed") {
		t.Fatalf("the floor spoke ahead of the scan: %q", got.stderr)
	}
}

func TestAnUnreadableSourceFileIsRefusedNamingIt(t *testing.T) {
	if os.Geteuid() == 0 {
		t.Skip("root reads a sealed file regardless of its mode")
	}
	root := plantRepo(t, map[string]string{"apps/sealed.c": "void f(void);\n"})
	sealed := filepath.Join(root, "apps", "sealed.c")
	if err := os.Chmod(sealed, 0o000); err != nil {
		t.Fatalf("seal: %v", err)
	}
	t.Cleanup(func() { _ = os.Chmod(sealed, 0o644) })
	got := run(t, context.Background(), root)
	if got.code != 2 {
		t.Fatalf("= %d, want 2", got.code)
	}
	if !strings.Contains(got.stderr, "cannot scan source tree") || !strings.Contains(got.stderr, "apps/sealed.c") {
		t.Fatalf("stderr = %q, want the scan refusal naming the file", got.stderr)
	}
}

// A cancelled run must be refused rather than reported clean over the files
// it happened to read before the cancellation landed.
func TestACancelledRunIsRefusedAndNeverReportedClean(t *testing.T) {
	root := plantFullScope(t, nil)
	ctx, cancel := context.WithCancel(context.Background())
	cancel()
	got := run(t, ctx, root)
	if got.code != 2 {
		t.Fatalf("a cancelled run = %d, want 2", got.code)
	}
	if strings.Contains(got.stdout, "clean") {
		t.Fatalf("a cancelled run reported clean: %q", got.stdout)
	}
}

// The gate must answer 2 rather than crash when a writer it was promised is
// absent, including the stderr it would otherwise announce the refusal down.
// The writers have to be nil interface values: a typed nil pointer is a
// non-nil interface and slides straight past the guard.
func TestRunWithoutTheWritersItWasPromisedStillAnswersTwo(t *testing.T) {
	root := plantRepo(t, nil)
	var absent io.Writer
	var out bytes.Buffer
	if code := Run(context.Background(), root, nil, &out, absent); code != 2 {
		t.Fatalf("no stderr = %d, want 2", code)
	}
	if out.Len() != 0 {
		t.Fatalf("a refused run wrote to stdout: %q", out.String())
	}
}
