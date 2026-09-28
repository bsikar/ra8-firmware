// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package waverefs

import (
	"context"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

// The gate is only as honest as the text it agrees to read. Everything below
// pins that half: which paths reach the detector, which are dropped before a
// byte is read, and what a scanned line is reported as once it does.

func plantTree(t *testing.T, files map[string]string) string {
	t.Helper()
	root := t.TempDir()
	for rel, contents := range files {
		path := filepath.Join(root, filepath.FromSlash(rel))
		if err := os.MkdirAll(filepath.Dir(path), 0o700); err != nil {
			t.Fatalf("plant %s: %v", rel, err)
		}
		if err := os.WriteFile(path, []byte(contents), 0o600); err != nil {
			t.Fatalf("plant %s: %v", rel, err)
		}
	}
	return root
}

func scanned(t *testing.T, root string, paths ...string) []finding {
	t.Helper()
	out, err := scan(context.Background(), root, paths)
	if err != nil {
		t.Fatalf("scan: %v", err)
	}
	return out
}

func TestTheExtensionsAndBasenamesThisGateReads(t *testing.T) {
	admitted := []string{
		"a.c", "a.h", "a.cpp", "a.hpp", "a.cc", "a.cmake",
		"a.md", "a.yml", "a.yaml", "a.sh", "a.py", "a.txt",
		"a.mk", "a.just",
		"justfile", "Justfile", "Dockerfile", "CMakeLists.txt", "GNUmakefile",
		"deep/nested/justfile", "deep/nested/a.md",
	}
	for _, rel := range admitted {
		if !isScannedName(rel) {
			t.Errorf("isScannedName(%q) = false, want true", rel)
		}
	}
	refused := []string{
		"a.go", "a.rs", "a.json", "a.toml", "a.zig", "a.png", "a",
		"a.MD", "a.YML", "JUSTFILE", "dockerfile",
		"justfile.bak", "a.md.bak", "notes.md5",
	}
	// Basename matching is exact, but the extension table still decides first:
	// "cmakelists.txt" is read for its .txt, not for its name.
	if !isScannedName("cmakelists.txt") {
		t.Error("isScannedName(cmakelists.txt) = false, want true via .txt")
	}
	for _, rel := range refused {
		if isScannedName(rel) {
			t.Errorf("isScannedName(%q) = true, want false", rel)
		}
	}
}

func TestOnlyCAndCPlusPlusSourcesCountAsCSource(t *testing.T) {
	for _, ext := range []string{".c", ".h", ".cpp", ".hpp", ".cc"} {
		if !isCSource(ext) {
			t.Errorf("isCSource(%q) = false, want true", ext)
		}
	}
	for _, ext := range []string{".cmake", ".md", ".py", ".sh", ".C", ".H", "", ".cxx", "c"} {
		if isCSource(ext) {
			t.Errorf("isCSource(%q) = true, want false", ext)
		}
	}
}

func TestTheVendoredPrefixesAreExcludedAndOnlyOnAPathBoundary(t *testing.T) {
	for _, rel := range []string{
		"libs/third_party/lvgl/lv_conf.h",
		"apps/shared_libs/third_party/cmsis/core.h",
		"libs/ra8_fonts/font_16.c",
		"tools/vela/generated/model.py",
	} {
		if !excluded(rel) {
			t.Errorf("excluded(%q) = false, want true", rel)
		}
	}
	// The prefixes carry their trailing slash, so a sibling directory whose
	// name merely starts with one of them stays in scope. Dropping first-party
	// text is the expensive mistake here: the gate would report a clean tree.
	for _, rel := range []string{
		"libs/third_party_notes/README.md",
		"libs/ra8_fonts_tool/build.py",
		"tools/vela/generated_by_hand/model.py",
		"vendor/libs/third_party/x.c",
	} {
		if excluded(rel) {
			t.Errorf("excluded(%q) = true, want false", rel)
		}
	}
}

func TestBuildDirectoriesAreExcludedAtTheRootOrUnderAKnownBuildRoot(t *testing.T) {
	for _, rel := range []string{
		"build/x.md",
		"build-debug/x.md",
		"build_arm/x.md",
		"cmake-build-debug/x.md",
		"docs/build/x.md",
		"examples/build-rel/x.md",
		"local-poc/build_a/x.md",
		"port/cmake-build-debug/x.md",
		"tests/build/deep/x.md",
		"docs/deep/build/x.md",
		"tools/build/x.md",
		"apps/build/x.md",
	} {
		if !excluded(rel) {
			t.Errorf("excluded(%q) = false, want true", rel)
		}
	}
	// A build directory somewhere other than the root, under a top level this
	// gate does not treat as a build root, is ordinary first-party text.
	for _, rel := range []string{
		"libs/build/x.md",
		"infra/build-debug/x.md",
		"just/cmake-build-debug/x.md",
		"libs/deep/build/x.md",
	} {
		if excluded(rel) {
			t.Errorf("excluded(%q) = true, want false", rel)
		}
	}
	// "builder" is not "build-"; the prefixes are the whole rule.
	for _, rel := range []string{"builder/x.md", "buildings/x.md", "rebuild/x.md"} {
		if excluded(rel) {
			t.Errorf("excluded(%q) = true, want false", rel)
		}
	}
}

func TestGeneratedAndVendoredDirectoryNamesAreExcludedAtAnyDepth(t *testing.T) {
	for _, rel := range []string{
		"a/CMakeFiles/x.md",
		"a/b/_deps/x.md",
		"tools/__pycache__/x.py",
		"web/node_modules/pkg/x.md",
		"docs/reference/x.md",
		"a/b/doxygen/x.md",
		"a/html/x.md",
		"CMakeFiles/x.md",
	} {
		if !excluded(rel) {
			t.Errorf("excluded(%q) = false, want true", rel)
		}
	}
}

func TestTheLastPathElementIsNeverJudgedAsADirectory(t *testing.T) {
	// excluded() walks parts[:len(parts)-1], so a FILE named like one of the
	// excluded directories is still scanned. A file called docs/html is text.
	for _, rel := range []string{
		"docs/html", "docs/reference", "a/b/doxygen",
		"CMakeFiles", "build", "a/node_modules", "libs/third_party",
	} {
		if excluded(rel) {
			t.Errorf("excluded(%q) = true, want false", rel)
		}
	}
	if excluded("README.md") {
		t.Error("excluded(README.md) = true, want false")
	}
}

func TestScanReportsThePathLineAndTrimmedTextOfEachHit(t *testing.T) {
	root := plantTree(t, map[string]string{
		"docs/plan.md": "intro\nfixed in Wave 70   \t\nthe sine wave is smooth\nsee wave-43b for context\n",
	})
	found := scanned(t, root, "docs/plan.md")
	if len(found) != 2 {
		t.Fatalf("found %d, want 2: %+v", len(found), found)
	}
	if found[0].path != "docs/plan.md" || found[0].line != 2 {
		t.Errorf("first hit = %s:%d, want docs/plan.md:2", found[0].path, found[0].line)
	}
	if found[0].text != "fixed in Wave 70" {
		t.Errorf("first text = %q, want trailing whitespace trimmed", found[0].text)
	}
	if found[1].line != 4 || found[1].text != "see wave-43b for context" {
		t.Errorf("second hit = %d %q", found[1].line, found[1].text)
	}
}

func TestScanOrdersFindingsByPathThenLine(t *testing.T) {
	root := plantTree(t, map[string]string{
		"b.md": "Wave 2\nWave 1\n",
		"a.md": "quiet\nWave 9\n",
	})
	found := scanned(t, root, "b.md", "a.md")
	got := make([]string, 0, len(found))
	for _, item := range found {
		got = append(got, item.path)
	}
	want := "a.md:2 b.md:1 b.md:2"
	have := ""
	for _, item := range found {
		have += item.path + ":" + itoa(item.line) + " "
	}
	if strings.TrimSpace(have) != want {
		t.Errorf("order = %q, want %q (paths seen: %v)", strings.TrimSpace(have), want, got)
	}
}

func itoa(value int) string {
	if value == 0 {
		return "0"
	}
	digits := ""
	for value > 0 {
		digits = string(rune('0'+value%10)) + digits
		value /= 10
	}
	return digits
}

func TestScanSkipsTheGateItsOwnStyleGuideAndClaudeNotes(t *testing.T) {
	line := "fixed in Wave 70\n"
	root := plantTree(t, map[string]string{
		self:                  line,
		"docs/STYLE_GUIDE.md": line,
		"CLAUDE.md":           line,
		"docs/other.md":       line,
	})
	found := scanned(t, root, self, "docs/STYLE_GUIDE.md", "CLAUDE.md", "docs/other.md")
	if len(found) != 1 || found[0].path != "docs/other.md" {
		t.Fatalf("found %+v, want only docs/other.md", found)
	}
}

func TestScanPassesOverAMissingFileRatherThanFailing(t *testing.T) {
	root := plantTree(t, map[string]string{"present.md": "fixed in Wave 70\n"})
	found := scanned(t, root, "absent.md", "present.md")
	if len(found) != 1 || found[0].path != "present.md" {
		t.Fatalf("found %+v, want only present.md", found)
	}
}

func TestScanPassesOverTextThatIsNotValidUTF8(t *testing.T) {
	root := plantTree(t, map[string]string{
		"binary.txt": "fixed in Wave 70\n\xff\xfe\n",
		"text.txt":   "fixed in Wave 70\n",
	})
	found := scanned(t, root, "binary.txt", "text.txt")
	if len(found) != 1 || found[0].path != "text.txt" {
		t.Fatalf("found %+v, want only text.txt", found)
	}
}

func TestScanCountsTheExoticLineBreaksSoLineNumbersStayTrue(t *testing.T) {
	for label, separator := range map[string]string{
		"CRLF":             "\r\n",
		"CR":               "\r",
		"vertical tab":     "\v",
		"form feed":        "\f",
		"file separator":   "\u001c",
		"group separator":  "\u001d",
		"record separator": "\u001e",
		"next line":        "\u0085",
		"line separator":   "\u2028",
		"paragraph marker": "\u2029",
	} {
		root := plantTree(t, map[string]string{
			"doc.md": "one" + separator + "two" + separator + "fixed in Wave 70" + separator,
		})
		found := scanned(t, root, "doc.md")
		if len(found) != 1 {
			t.Fatalf("%s: found %d, want 1", label, len(found))
		}
		if found[0].line != 3 {
			t.Errorf("%s: line = %d, want 3", label, found[0].line)
		}
		if found[0].text != "fixed in Wave 70" {
			t.Errorf("%s: text = %q", label, found[0].text)
		}
	}
}

func TestScanHonoursAnOptOutAndStillReportsABareOne(t *testing.T) {
	root := plantTree(t, map[string]string{
		"doc.md": "see Wave 12 WAVE-OK: quoted source symbol\nsee Wave 13 WAVE-OK:\n",
	})
	found := scanned(t, root, "doc.md")
	if len(found) != 1 || found[0].line != 2 {
		t.Fatalf("found %+v, want only the bare opt-out on line 2", found)
	}
}

func TestScanStopsOnACancelledContext(t *testing.T) {
	root := plantTree(t, map[string]string{"doc.md": "fixed in Wave 70\n"})
	ctx, cancel := context.WithCancel(context.Background())
	cancel()
	found, err := scan(ctx, root, []string{"doc.md"})
	if err != context.Canceled {
		t.Fatalf("err = %v, want context.Canceled", err)
	}
	if found != nil {
		t.Errorf("found = %+v, want nil", found)
	}
}

func TestTheDetectorNeedsADigitAndAWholeWord(t *testing.T) {
	for _, text := range []string{
		"Wave 70", "wave 1", "Wave12", "wave_7", "wave-3", "Wave_43b", "WAVE 5 wave 5",
	} {
		if !wavePattern.MatchString(text) {
			t.Errorf("wavePattern(%q) = false, want true", text)
		}
	}
	for _, text := range []string{
		"waveform", "sine wave", "wave_table[0]", "k_ra8_pdg_wave_saw",
		"wave", "microwave 7", "wave-43bc", "wavelength 3",
	} {
		if wavePattern.MatchString(text) {
			t.Errorf("wavePattern(%q) = true, want false", text)
		}
	}
}

func TestContainsIsAnExactArgumentMatch(t *testing.T) {
	args := []string{"--quiet", "--selftest", "root"}
	if !contains(args, "--selftest") {
		t.Error("contains(--selftest) = false, want true")
	}
	for _, value := range []string{"selftest", "--selftes", "--SELFTEST", "", "--selftest=1"} {
		if contains(args, value) {
			t.Errorf("contains(%q) = true, want false", value)
		}
	}
	if contains(nil, "--selftest") {
		t.Error("contains(nil) = true, want false")
	}
}

func TestRunRefusesAnIncompleteInvocation(t *testing.T) {
	var out, errs strings.Builder
	cases := []struct {
		label  string
		ctx    context.Context
		root   string
		stdout *strings.Builder
		stderr *strings.Builder
	}{
		{"nil context", nil, "/tmp", &out, &errs},
		{"empty root", context.Background(), "", &out, &errs},
	}
	for _, item := range cases {
		out.Reset()
		errs.Reset()
		if code := Run(item.ctx, item.root, nil, item.stdout, item.stderr); code != 2 {
			t.Errorf("%s: code = %d, want 2", item.label, code)
		}
		if !strings.Contains(errs.String(), "invalid input") {
			t.Errorf("%s: stderr = %q, want the invalid-input refusal", item.label, errs.String())
		}
		if out.String() != "" {
			t.Errorf("%s: stdout = %q, want empty", item.label, out.String())
		}
	}
	errs.Reset()
	if code := Run(context.Background(), "/tmp", nil, nil, &errs); code != 2 {
		t.Errorf("nil stdout: code = %d, want 2", code)
	}
}

func TestRunRefusesACollapsedScopeRatherThanReportingACleanTree(t *testing.T) {
	// A scope smaller than the floor means the derivation broke, not that the
	// tree is clean, and the gate has to say so on stderr and fail hard.
	root := plantTree(t, map[string]string{"README.md": "fixed in Wave 70\n"})
	var out, errs strings.Builder
	code := Run(context.Background(), root, nil, &out, &errs)
	if code != 2 {
		t.Fatalf("code = %d, want 2 (stderr %q)", code, errs.String())
	}
	if strings.Contains(out.String(), "gate clean") {
		t.Errorf("stdout = %q, must never call a collapsed scope clean", out.String())
	}
}
