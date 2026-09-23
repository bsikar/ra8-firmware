// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

// Package legacymake rejects command-shaped uses of Make as the repository task runner.
package legacymake

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
	"unicode/utf8"
)

const self = "tools/ra8ci/internal/legacymake/legacymake.go"
const minimumScopedFiles = 650

var (
	makeExecutable  = `(?:g?make|"g?make"|'g?make')`
	activeCommand   = regexp.MustCompile(`^\s*(?:(?:RUN|run:)\s+)?(` + makeExecutable + `)(?:\s+([^\s#;&|]+)|$|\s)`)
	arrayCommand    = regexp.MustCompile(`^\s*[A-Za-z_][A-Za-z0-9_]*\s*=\(\s*(` + makeExecutable + `)(?:\s+([^\s)]+)|\s|\))`)
	commentCommand  = regexp.MustCompile(`^\s*#\s*(` + makeExecutable + `)\s+([^\s]+)\s*\.?\s*$`)
	quotedCommand   = regexp.MustCompile("(?:\x60|'|\")(g?make)(?:\\s+([^\\s\x60'\"]+)|(?:\x60|'|\")+\\s+(?:target|recipe|task)\\b)")
	guidanceCommand = regexp.MustCompile(`(?i)\b(?:run|use|invoke|try|rerun|execute)\s+(` + makeExecutable + `)\s+([^\s\x60'\"]+)`)
	exactFiles      = map[string]bool{".clangd": true, ".cppcheck-suppressions": true, ".env.example": true, "CMakePresets.json": true, "justfile": true}
	prefixes        = []string{".devcontainer/", ".github/workflows/", ".vscode/", "just/", "scripts/", "tools/mcp/"}
	excluded        = []string{"docs/sbom/upstream/", "libs/third_party/", "apps/shared_libs/third_party/", "port/netxduo/", "port/nimble/", "port/threadx/", "port/usbx/", "tests/fixtures/"}
)

type finding struct {
	path    string
	line    int
	command string
}

// Run executes detector self-tests or scans the authored automation and documentation scope.
func Run(ctx context.Context, root string, args []string, stdout, stderr io.Writer) int {
	if ctx == nil || root == "" || stdout == nil || stderr == nil {
		fmt.Fprintln(stderr, "ra8ci legacy-make: invalid input")
		return 2
	}
	if len(args) == 1 && args[0] == "--selftest" {
		if selfTest(stdout, stderr) {
			return 0
		}
		return 1
	}
	if len(args) != 0 {
		fmt.Fprintln(stderr, "usage: ra8ci legacy-make [--selftest]")
		return 2
	}
	paths, err := scopedFiles(ctx, root)
	if err != nil {
		fmt.Fprintln(stderr, "ra8ci legacy-make: cannot enumerate tracked files:", err)
		return 2
	}
	if len(paths) < minimumScopedFiles || !hasPath(paths, self) {
		fmt.Fprintf(stderr, "ra8ci legacy-make: scope collapsed to %d file(s); expected at least %d including %s\n", len(paths), minimumScopedFiles, self)
		return 2
	}
	findings, err := scan(ctx, root, paths)
	if err != nil {
		fmt.Fprintln(stderr, "ra8ci legacy-make: scan failed:", err)
		return 2
	}
	if len(findings) != 0 {
		fmt.Fprintln(stderr, "ra8ci legacy-make: legacy repository task references:")
		for _, item := range findings {
			fmt.Fprintf(stderr, "  %s:%d: legacy repository task: %s\n", item.path, item.line, item.command)
		}
		fmt.Fprintln(stderr, "Use the authoritative namespaced Just recipe instead.")
		return 1
	}
	fmt.Fprintf(stdout, "ra8ci legacy-make: clean (%d authored files)\n", len(paths))
	return 0
}

func scopedFiles(ctx context.Context, root string) ([]string, error) {
	cmd := exec.CommandContext(ctx, "git", "-C", root, "ls-files", "--cached", "--others", "--exclude-standard", "-z")
	out, err := cmd.Output()
	if err != nil {
		return nil, fmt.Errorf("git ls-files: %w", err)
	}
	selected := map[string]bool{}
	for _, raw := range bytes.Split(out, []byte{0}) {
		rel := filepath.ToSlash(string(raw))
		if rel == "" || isExcluded(rel) {
			continue
		}
		absolute := filepath.Join(root, filepath.FromSlash(rel))
		info, statErr := os.Stat(absolute)
		if statErr != nil || !info.Mode().IsRegular() {
			continue
		}
		suffix := strings.ToLower(filepath.Ext(rel))
		keep := exactFiles[rel] || filepath.Base(rel) == "Dockerfile" || suffix == ".md" || suffix == ".mdx" || suffix == ".rst"
		for _, prefix := range prefixes {
			keep = keep || strings.HasPrefix(rel, prefix)
		}
		base := filepath.Base(rel)
		githubRelative := strings.TrimPrefix(rel, ".github/")
		if strings.HasPrefix(rel, ".github/") && !strings.Contains(githubRelative, "/") && strings.Contains(base, "baseline") && suffix == ".txt" {
			keep = true
		}
		if keep {
			selected[rel] = true
		}
	}
	if _, err := os.Stat(filepath.Join(root, filepath.FromSlash(self))); err == nil {
		selected[self] = true
	}
	paths := make([]string, 0, len(selected))
	for rel := range selected {
		paths = append(paths, rel)
	}
	sort.Strings(paths)
	return paths, nil
}

func isExcluded(rel string) bool {
	for _, prefix := range excluded {
		if strings.HasPrefix(rel, prefix) {
			return true
		}
	}
	return false
}
func hasPath(paths []string, target string) bool {
	for _, path := range paths {
		if path == target {
			return true
		}
	}
	return false
}

func scan(ctx context.Context, root string, paths []string) ([]finding, error) {
	var findings []finding
	for _, rel := range paths {
		if err := ctx.Err(); err != nil {
			return nil, err
		}
		data, err := os.ReadFile(filepath.Join(root, filepath.FromSlash(rel)))
		if err != nil {
			return nil, fmt.Errorf("read %s: %w", rel, err)
		}
		if !utf8.Valid(data) {
			continue
		}
		active := strings.HasSuffix(rel, ".sh") || strings.HasSuffix(rel, ".yml") || strings.HasSuffix(rel, ".yaml") || filepath.Base(rel) == "Dockerfile"
		lineBreaks := strings.NewReplacer("\r\n", "\n", "\r", "\n", "\v", "\n", "\f", "\n", "\u001c", "\n", "\u001d", "\n", "\u001e", "\n", "\u0085", "\n", "\u2028", "\n", "\u2029", "\n")
		lines := strings.Split(lineBreaks.Replace(string(data)), "\n")
		for n, line := range lines {
			if command := invocation(line, active); command != "" {
				findings = append(findings, finding{rel, n + 1, command})
			}
		}
	}
	sort.Slice(findings, func(i, j int) bool {
		if findings[i].path == findings[j].path {
			return findings[i].line < findings[j].line
		}
		return findings[i].path < findings[j].path
	})
	return findings, nil
}

func invocation(line string, active bool) string {
	patterns := []*regexp.Regexp{commentCommand, quotedCommand, guidanceCommand}
	if active {
		patterns = append([]*regexp.Regexp{activeCommand, arrayCommand}, patterns...)
	}
	for _, pattern := range patterns {
		match := pattern.FindStringSubmatch(line)
		if match == nil {
			continue
		}
		executable := strings.Trim(match[1], "\"'")
		if len(match) > 2 && match[2] != "" {
			return executable + " " + match[2]
		}
		return executable
	}
	return ""
}

func selfTest(stdout, stderr io.Writer) bool {
	word := "ma" + "ke"
	gnu := "g" + word
	cases := []struct {
		text         string
		active, want bool
	}{
		{word + " ci", true, true}, {word + " -C apps/blink build", true, true}, {gnu + " ci", true, true},
		{"cmd=(" + word + " -C apps/blink)", true, true}, {"cmd=(" + "\"" + word + "\" \"-C\" apps/blink)", true, true},
		{"\"" + word + "\" -C apps/blink", true, true}, {"run: " + word + " -C apps/blink", true, true}, {"RUN " + word + " coverage", true, true},
		{"# " + word + " ci-native", true, true}, {"# \x60" + word + " sbom\x60 regenerates it", true, true},
		{"CI (or a local \x60\x60" + word + "\x60\x60 target) catches drift", true, true}, {"Please run " + word + " misra", true, true},
		{"command -v make || missing=build-essential", true, false}, {"command -v gmake || missing=build-essential", true, false},
		{"for tool in curl cmake make tar cc; do", true, false}, {"these controls make an empty scan fail", true, false},
		{"# make the detector fail", true, false}, {"CMakeLists.txt and GNUmakefile", true, false},
		{"# Make is required by an upstream source build", true, false},
	}
	for i, item := range cases {
		if (invocation(item.text, item.active) != "") != item.want {
			fmt.Fprintf(stderr, "ra8ci legacy-make --selftest: FAIL case %d\n", i+1)
			return false
		}
	}
	fmt.Fprintf(stdout, "ra8ci legacy-make --selftest: PASS (%d both-direction cases)\n", len(cases))
	return true
}
