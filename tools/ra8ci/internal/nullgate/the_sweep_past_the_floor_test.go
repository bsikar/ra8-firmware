// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package nullgate

import (
	"bytes"
	"context"
	"fmt"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

// plantSweep builds a repository past the 1000-path floor: filler prose that
// the scope drops, plus whatever sources the case actually cares about.
func plantSweep(t *testing.T, sources map[string]string) string {
	t.Helper()
	files := make(map[string]string, len(sources)+1000)
	for i := 0; i < 1000; i++ {
		files[fmt.Sprintf("docs/filler/note%04d.md", i)] = "prose\n"
	}
	for name, body := range sources {
		files[name] = body
	}
	return plantRepo(t, files)
}

func swept(t *testing.T, root string, args ...string) (int, string, string) {
	t.Helper()
	var stdout, stderr bytes.Buffer
	code := Run(context.Background(), root, args, &stdout, &stderr)
	return code, stdout.String(), stderr.String()
}

// Past the floor the sweep actually reports, and it reports only what the
// scope keeps. The floor refusal is already pinned; this is the other side of
// it, where a collapsed enumeration would otherwise look identical.
func TestASweepPastTheFloorJudgesOnlyWhatTheScopeKeeps(t *testing.T) {
	root := plantSweep(t, map[string]string{
		"libs/ra8_hal/src/ra8_gpio.c":                   "int f(void) { char *p = NULL; return p == NULL; }\n",
		"libs/ra8_hal/inc/ra8_gpio.h":                   "void f(void);\n",
		"tests/test_gpio.c":                             "char *p = NULL;\n",
		"libs/third_party/threadx/src/tx.c":             "char *p = NULL;\n",
		"apps/shared_libs/third_party/mz.c":             "char *p = NULL;\n",
		"libs/ra8_c6link/src/ra8_media_download.pb-c.c": "char *p = NULL;\n",
		"docs/notes.md":                                 "NULL is discussed here\n",
	})

	code, stdout, stderr := swept(t, root, "--all")
	if code != 1 {
		t.Fatalf("a sweep over one bare NULL answered %d, stdout=%q stderr=%q", code, stdout, stderr)
	}
	if !strings.Contains(stderr, "ra8_gpio.c:1") {
		t.Fatalf("the sweep did not report the first-party source: %q", stderr)
	}
	if !strings.Contains(stderr, "1 bare NULL token(s) found") {
		t.Fatalf("the sweep counted something other than one finding: %q", stderr)
	}
	for _, exempt := range []string{"test_gpio.c", "tx.c", "mz.c", "pb-c.c", "notes.md"} {
		if strings.Contains(stderr, exempt) {
			t.Fatalf("the sweep judged an exempt path %s: %q", exempt, stderr)
		}
	}
}

// The same tree with its one bare NULL spelled nullptr is clean, and the clean
// answer goes to stdout while every finding goes to stderr. That split is what
// lets CI read the verdict without parsing the findings.
func TestASweepPastTheFloorAnswersCleanOnStdout(t *testing.T) {
	root := plantSweep(t, map[string]string{
		"libs/ra8_hal/src/ra8_gpio.c": "int f(void) { char *p = nullptr; return p == UX_NULL; }\n",
	})

	code, stdout, stderr := swept(t, root, "--all")
	if code != 0 || stderr != "" {
		t.Fatalf("a clean sweep answered %d, stderr=%q", code, stderr)
	}
	if !strings.Contains(stdout, "0 findings") {
		t.Fatalf("the clean sweep did not say so: %q", stdout)
	}
}

// A source the gate cannot read yields no violations rather than a refusal:
// the enumeration already decided the file belongs, so an unreadable one is
// passed over the way an undecodable one is. Worth pinning because the quiet
// return is indistinguishable from a clean file at the call site.
func TestASourceTheGateCannotReadYieldsNothing(t *testing.T) {
	root := plantFiles(t, map[string]string{"libs/ra8_hal/src/ra8_gpio.c": "char *p = NULL;\n"})
	sealed := filepath.Join(root, "libs", "ra8_hal", "src", "ra8_gpio.c")
	if err := os.Chmod(sealed, 0o000); err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { _ = os.Chmod(sealed, 0o600) })
	if _, err := os.ReadFile(sealed); err == nil {
		t.Skip("this process can read a sealed file")
	}

	if found := findViolations(sealed); found != nil {
		t.Fatalf("an unreadable source produced violations: %+v", found)
	}
	code, stdout, stderr := swept(t, root, "libs/ra8_hal/src/ra8_gpio.c")
	if code != 0 || stderr != "" || !strings.Contains(stdout, "0 findings") {
		t.Fatalf("an unreadable source answered %d, stdout=%q stderr=%q", code, stdout, stderr)
	}
}

// The scanner walks literals rather than merely noticing their quotes: an
// escaped quote does not end a string, an escaped quote does not end a
// character constant, and code after either is judged normally. Get this
// wrong in one direction and every NULL after an escape is missed; get it
// wrong in the other and prose inside a literal is reported as code.
func TestTheScannerWalksEscapesInsideLiterals(t *testing.T) {
	for _, one := range []struct {
		name     string
		line     string
		reported bool
	}{
		{"an escaped quote does not end the string", `const char *s = "he said \"NULL\" loudly";`, false},
		{"code after a closed string is judged", `const char *s = "quiet"; char *p = NULL;`, true},
		{"an escaped quote does not end the char", `char q = '\''; char *p = "NULL";`, false},
		{"code after a closed char is judged", `char q = '\''; char *p = NULL;`, true},
		{"a NULL inside a character constant is not code", `char q = 'N'; const char *s = "NULL";`, false},
		{"a vendor macro is not a bare NULL", `char *p = UX_NULL;`, false},
		{"a line comment hides the rest", `char *p = nullptr; // NULL here is prose`, false},
	} {
		t.Run(one.name, func(t *testing.T) {
			path := filepath.Join(t.TempDir(), "unit.c")
			if err := os.WriteFile(path, []byte(one.line+"\n"), 0o600); err != nil {
				t.Fatal(err)
			}
			found := findViolations(path)
			if reported := len(found) != 0; reported != one.reported {
				t.Fatalf("%q reported=%v, want %v (%+v)", one.line, reported, one.reported, found)
			}
		})
	}
}
