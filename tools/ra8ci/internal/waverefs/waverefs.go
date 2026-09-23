// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

// Package waverefs rejects numbered session-bookkeeping wave references in first-party text.
package waverefs

import (
	"bytes"
	"context"
	"fmt"
	"io"
	"os"
	"os/exec"
	"path/filepath"
	"regexp"
	"sort"
	"strings"
	"unicode"
	"unicode/utf8"
)

const self = "tools/ra8ci/internal/waverefs/waverefs.go"
const fileFloor = 2500
const maxFindingsShown = 50
const snippetMaxLen = 120
const snippetTrimLen = 117

var (
	wavePattern    = regexp.MustCompile("\\b[Ww]ave[\\s_-]?\\d+[A-Za-z]?\\b")
	optOutPattern  = regexp.MustCompile("WAVE-OK\\s*:")
	scanExtensions = map[string]bool{
		".c": true, ".h": true, ".cpp": true, ".hpp": true, ".cc": true, ".cmake": true,
		".md": true, ".yml": true, ".yaml": true, ".sh": true, ".py": true, ".txt": true,
		".mk": true, ".just": true,
	}
	scanBasenames = map[string]bool{
		"justfile": true, "Dockerfile": true, "CMakeLists.txt": true, "GNUmakefile": true, "Justfile": true,
	}
	excludedPrefixes = []string{"libs/third_party/", "apps/shared_libs/third_party/", "libs/ra8_fonts/", "tools/vela/generated/"}
	docsVendorDirs   = map[string]bool{"reference": true, "doxygen": true, "html": true}
	buildRoots       = map[string]bool{"docs": true, "examples": true, "local-poc": true, "port": true, "tests": true, "tools": true, "apps": true}
	buildOutputNames = map[string]bool{"CMakeFiles": true, "_deps": true, "__pycache__": true, "node_modules": true}
)

type finding struct {
	path string
	line int
	text string
}

// Run executes detector self-tests or scans the full derived first-party text scope.
func Run(ctx context.Context, root string, args []string, stdout, stderr io.Writer) int {
	if ctx == nil || root == "" || stdout == nil || stderr == nil {
		fmt.Fprintln(stderr, "ra8ci wave-references: invalid input")
		return 2
	}
	if contains(args, "--selftest") {
		if selfTest(ctx, root, stdout, stderr) {
			return 0
		}
		return 1
	}
	paths, err := sourceFiles(ctx, root)
	if err != nil {
		fmt.Fprintln(stderr, "ra8ci wave-references: cannot derive source scope:", err)
		return 2
	}
	if len(paths) < fileFloor {
		fmt.Fprintf(stderr, "ra8ci wave-references: FATAL -- only %d file(s) in scope, floor is %d. A collapsed scope reports a clean tree because it scanned almost nothing.\n", len(paths), fileFloor)
		return 2
	}
	findings, err := scan(ctx, root, paths)
	if err != nil {
		fmt.Fprintln(stderr, "ra8ci wave-references: scan failed:", err)
		return 2
	}
	if len(findings) == 0 {
		fmt.Fprintln(stdout, "no-wave-refs: 0 violations -- gate clean.")
		return 0
	}
	fmt.Fprintf(stdout, "no-wave-refs: %d violations found.\n", len(findings))
	limit := len(findings)
	if limit > maxFindingsShown {
		limit = maxFindingsShown
	}
	for _, item := range findings[:limit] {
		text := item.text
		if utf8.RuneCountInString(text) > snippetMaxLen {
			text = string([]rune(text)[:snippetTrimLen]) + "..."
		}
		fmt.Fprintf(stdout, "  %s:%d %s\n", item.path, item.line, text)
	}
	if len(findings) > maxFindingsShown {
		fmt.Fprintf(stdout, "  ... %d more (truncated)\n", len(findings)-maxFindingsShown)
	}
	fmt.Fprintln(stdout)
	fmt.Fprintln(stdout, "Per-line opt-out: append \"WAVE-OK: <reason>\" on the offending line.")
	return 1
}

func sourceFiles(ctx context.Context, root string) ([]string, error) {
	cmd := exec.CommandContext(ctx, "git", "-C", root, "ls-files", "-z", "--cached", "--others", "--exclude-standard")
	out, err := cmd.Output()
	if err != nil {
		return nil, fmt.Errorf("git ls-files: %w", err)
	}
	selected := map[string]bool{}
	for _, raw := range bytes.Split(out, []byte{0}) {
		rel := filepath.ToSlash(string(raw))
		if rel == "" || excluded(rel) {
			continue
		}
		path := filepath.Join(root, filepath.FromSlash(rel))
		info, statErr := os.Stat(path)
		if statErr != nil || !info.Mode().IsRegular() {
			continue
		}
		if !isScannedName(rel) {
			continue
		}
		if strings.HasPrefix(rel, "port/threadx/") && isCSource(filepath.Ext(rel)) {
			continue
		}
		selected[rel] = true
	}
	if info, err := os.Stat(filepath.Join(root, filepath.FromSlash(self))); err == nil && info.Mode().IsRegular() {
		selected[self] = true
	}
	paths := make([]string, 0, len(selected))
	for rel := range selected {
		paths = append(paths, rel)
	}
	sort.Strings(paths)
	return paths, nil
}

func isScannedName(rel string) bool {
	return scanExtensions[filepath.Ext(rel)] || scanBasenames[filepath.Base(rel)]
}

func isCSource(ext string) bool {
	switch ext {
	case ".c", ".h", ".cpp", ".hpp", ".cc":
		return true
	default:
		return false
	}
}

func excluded(rel string) bool {
	for _, prefix := range excludedPrefixes {
		if strings.HasPrefix(rel, prefix) {
			return true
		}
	}
	parts := strings.Split(rel, "/")
	for index, part := range parts[:len(parts)-1] {
		if buildOutputNames[part] {
			return true
		}
		isBuild := part == "build" || strings.HasPrefix(part, "build-") || strings.HasPrefix(part, "build_") || strings.HasPrefix(part, "cmake-build-")
		if isBuild && (index == 0 || buildRoots[parts[0]]) {
			return true
		}
		if docsVendorDirs[part] {
			return true
		}
	}
	return false
}

func scan(ctx context.Context, root string, paths []string) ([]finding, error) {
	var out []finding
	lineBreaks := strings.NewReplacer("\r\n", "\n", "\r", "\n", "\v", "\n", "\f", "\n", "\u001c", "\n", "\u001d", "\n", "\u001e", "\n", "\u0085", "\n", "\u2028", "\n", "\u2029", "\n")
	for _, rel := range paths {
		if err := ctx.Err(); err != nil {
			return nil, err
		}
		if rel == self || rel == "docs/STYLE_GUIDE.md" || rel == "CLAUDE.md" {
			continue
		}
		data, err := os.ReadFile(filepath.Join(root, filepath.FromSlash(rel)))
		if err != nil {
			continue
		}
		if !utf8.Valid(data) {
			continue
		}
		text := lineBreaks.Replace(string(data))
		for number, line := range strings.Split(text, "\n") {
			if optOutPattern.MatchString(line) || !wavePattern.MatchString(line) {
				continue
			}
			out = append(out, finding{path: rel, line: number + 1, text: strings.TrimRightFunc(line, unicode.IsSpace)})
		}
	}
	sort.Slice(out, func(i, j int) bool {
		if out[i].path == out[j].path {
			return out[i].line < out[j].line
		}
		return out[i].path < out[j].path
	})
	return out, nil
}

func selfTest(ctx context.Context, root string, stdout, stderr io.Writer) bool {
	tests := []struct {
		text  string
		want  bool
		label string
	}{
		{"fixed in Wave 70", true, "numbered wave"},
		{"see wave-43b for context", true, "hyphenated sub-number"},
		{"the sine wave is smooth", false, "domain prose"},
		{"k_ra8_pdg_wave_saw selects the waveform", false, "hardware identifier"},
		{"wave_table[0] holds the sample", false, "wave identifier"},
		{"see Wave 12 WAVE-OK: quoted source symbol", false, "per-line opt-out"},
	}
	for _, test := range tests {
		got := wavePattern.MatchString(test.text)
		if optOutPattern.MatchString(test.text) {
			got = false
		}
		if got != test.want {
			fmt.Fprintf(stderr, "ra8ci wave-references --selftest: FAIL: %s\n", test.label)
			return false
		}
	}
	paths, err := sourceFiles(ctx, root)
	if err != nil {
		fmt.Fprintln(stderr, "ra8ci wave-references --selftest: scope error:", err)
		return false
	}
	fmt.Fprintf(stderr, "ra8ci wave-references --selftest: scope has %d file(s), floor %d\n", len(paths), fileFloor)
	hasInfra, hasJust := false, false
	for _, rel := range paths {
		hasInfra = hasInfra || strings.HasPrefix(rel, "infra/")
		hasJust = hasJust || strings.HasPrefix(rel, "just/")
	}
	if !hasInfra || !hasJust {
		fmt.Fprintln(stderr, "ra8ci wave-references --selftest: derived scope omits infra/ or just/")
		return false
	}
	fmt.Fprintf(stdout, "ra8ci wave-references --selftest: PASS (%d detector cases; %d scoped files)\n", len(tests), len(paths))
	return true
}

func contains(args []string, value string) bool {
	for _, arg := range args {
		if arg == value {
			return true
		}
	}
	return false
}
