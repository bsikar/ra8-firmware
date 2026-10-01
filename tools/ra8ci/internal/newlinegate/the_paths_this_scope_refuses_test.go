// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package newlinegate

import (
	"os"
	"path/filepath"
	"strings"
	"testing"
)

// A path the gate cannot place under the root it was handed is excluded rather
// than judged. Rel fails when one side is absolute and the other is not, and a
// file the gate cannot locate is not a file it should be reporting on.
func TestAPathThatCannotBePlacedUnderTheRootIsExcluded(t *testing.T) {
	if !excluded("/srv/ra8", "libs/ra8_core/ra8_gpio.c") {
		t.Fatal("a relative path against an absolute root was judged in scope")
	}
}

// The generated directory names are excluded wherever they appear in the path,
// not only directly under the root.
func TestAGeneratedDirectoryIsExcludedAtAnyDepth(t *testing.T) {
	root := t.TempDir()
	for _, rel := range []string{
		"CMakeFiles/ra8.c",
		"libs/ra8_core/CMakeFiles/ra8.c",
		"libs/_deps/ra8.c",
		"tools/vela/__pycache__/ra8.py",
		"apps/ui/node_modules/pkg/ra8.ts",
	} {
		if !excluded(root, filepath.Join(root, filepath.FromSlash(rel))) {
			t.Fatalf("%s was judged in scope", rel)
		}
	}
}

// Only the directory elements are judged, so a FILE carrying one of those
// names is still first-party source.
func TestAFileNamedLikeAGeneratedDirectoryIsStillJudged(t *testing.T) {
	root := t.TempDir()
	for _, rel := range []string{"libs/ra8_core/CMakeFiles", "libs/_deps", "tools/node_modules"} {
		if excluded(root, filepath.Join(root, filepath.FromSlash(rel))) {
			t.Fatalf("the file %s was excluded as though it were a directory", rel)
		}
	}
}

// The vendored prefixes are matched anywhere in the path rather than anchored
// at the root, so a vendored tree nested under a first-party one is still
// vendored. The opt-out directory is matched the same way, by substring, so a
// directory whose name merely ENDS in _unsupported is excluded too. What stays
// in scope is a name that does not carry the marker at all, and a FILE named
// after it, since both markers carry their own trailing slash.
func TestAVendoredPrefixIsMatchedWhereverItAppears(t *testing.T) {
	root := t.TempDir()
	for _, rel := range []string{
		"libs/third_party/lvgl/lv_conf.h",
		"apps/ui/libs/third_party/lvgl/lv_conf.h",
		"port/threadx/tx_port.h",
		"libs/ra8_core/_unsupported/ra8_gpio.c",
		"libs/ra8_unsupported/ra8_gpio.c",
	} {
		if !excluded(root, filepath.Join(root, filepath.FromSlash(rel))) {
			t.Fatalf("%s was judged in scope", rel)
		}
	}
	for _, rel := range []string{
		"libs/third_party_notes/ra8.c",
		"libs/ra8_core/unsupported/ra8.c",
		"libs/ra8_core/_unsupported.c",
	} {
		if excluded(root, filepath.Join(root, filepath.FromSlash(rel))) {
			t.Fatalf("%s was excluded by a near miss", rel)
		}
	}
}

// A directory handed to the gate is walked, and an excluded subdirectory under
// it is skipped whole. A file missing its newline in there is not a finding,
// and neither is the same file named outright.
func TestAWalkSkipsAnExcludedSubdirectoryWhole(t *testing.T) {
	root := t.TempDir()
	sourceFile(t, root, "tools/ra8.c", "int main(void) { return 0; }\n")
	offender := sourceFile(t, root, "tools/build/generated/ra8.c", "int main(void) { return 0; }")

	code, stdout, stderr := scan(t, root, "tools")
	if code != 0 {
		t.Fatalf("a tree whose only offender is under a build directory answered %d: %s%s", code, stdout, stderr)
	}
	if strings.Contains(stderr, "build") {
		t.Fatalf("an excluded file was reported: %s", stderr)
	}
	if code, _, stderr := scan(t, root, offender); code != 0 {
		t.Fatalf("the same file named outright answered %d: %s", code, stderr)
	}
}

// A subdirectory the gate cannot read is a refusal, not a tree it quietly
// reports clean. The walk hands its own error back and the run exits 2.
func TestASubdirectoryTheWalkCannotReadIsARefusal(t *testing.T) {
	root := t.TempDir()
	sourceFile(t, root, "tools/ra8.c", "int main(void) { return 0; }\n")
	sealed := filepath.Join(root, "tools", "sealed")
	if err := os.MkdirAll(sealed, 0700); err != nil {
		t.Fatal(err)
	}
	if err := os.Chmod(sealed, 0o000); err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { _ = os.Chmod(sealed, 0700) })
	if _, err := os.ReadDir(sealed); err == nil {
		t.Skip("this process can read a sealed directory")
	}

	code, stdout, stderr := scan(t, root, "tools")
	if code != 2 {
		t.Fatalf("a tree with an unreadable subdirectory answered %d: %s%s", code, stdout, stderr)
	}
	if !strings.Contains(stderr, "sealed") {
		t.Fatalf("the refusal did not name the directory at fault: %s", stderr)
	}
}

// The shebang decides whether an extensionless file is a script, and it is read
// from the file. One the gate cannot open is not a script, so it stays out of
// scope rather than refusing the run over a file nobody asked it to judge.
func TestAnExtensionlessFileTheGateCannotOpenIsNotAScript(t *testing.T) {
	root := t.TempDir()
	sealed := script(t, root, "tools/hooks/pre-commit", "#!/bin/sh\necho ra8")
	if err := os.Chmod(sealed, 0o000); err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { _ = os.Chmod(sealed, 0700) })
	if _, err := os.ReadFile(sealed); err == nil {
		t.Skip("this process can read a sealed file")
	}
	if isScriptWithoutSuffix(sealed) {
		t.Fatal("a file that cannot be opened was read as a script")
	}

	code, stdout, stderr := scan(t, root, "tools")
	if code != 0 {
		t.Fatalf("a tree whose only extensionless file is unreadable answered %d: %s%s", code, stdout, stderr)
	}
}
