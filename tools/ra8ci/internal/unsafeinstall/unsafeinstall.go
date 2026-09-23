// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

// Package unsafeinstall detects the PEP 668 system-package override in first-party files.
package unsafeinstall

import (
	"bytes"
	"context"
	"fmt"
	"io"
	"os"
	"os/exec"
	"path/filepath"
	"sort"
	"strings"
	"unicode/utf8"
)

const self = "tools/ra8ci/internal/unsafeinstall/unsafeinstall.go"
const minimumScopedFiles = 4000
const forbidden = "--break-" + "system-packages"

var excludedPrefixes = []string{
	"docs/sbom/upstream/",
	"libs/third_party/",
	"apps/shared_libs/third_party/",
	"port/netxduo/",
	"port/nimble/",
	"port/threadx/",
	"port/usbx/",
	"tests/fixtures/",
}

type finding struct {
	path string
	line int
}

// Run executes detector self-tests or scans every first-party file Git knows about.
func Run(ctx context.Context, root string, args []string, stdout, stderr io.Writer) int {
	if ctx == nil || root == "" || stdout == nil || stderr == nil {
		fmt.Fprintln(stderr, "ra8ci no-unsafe-python-install: invalid input")
		return 2
	}
	if len(args) == 1 && args[0] == "--selftest" {
		if selfTest(stdout, stderr) {
			return 0
		}
		return 1
	}
	if len(args) != 0 {
		fmt.Fprintln(stderr, "usage: ra8ci no-unsafe-python-install [--selftest]")
		return 2
	}
	paths, err := scopedFiles(ctx, root)
	if err != nil {
		fmt.Fprintln(stderr, "ra8ci no-unsafe-python-install: cannot enumerate first-party files:", err)
		return 2
	}
	if len(paths) < minimumScopedFiles || !hasPath(paths, self) {
		fmt.Fprintf(stderr, "ra8ci no-unsafe-python-install: scope collapsed to %d files; expected at least %d including %s\n", len(paths), minimumScopedFiles, self)
		return 2
	}
	findings, err := scan(ctx, root, paths)
	if err != nil {
		fmt.Fprintln(stderr, "ra8ci no-unsafe-python-install: scan failed:", err)
		return 2
	}
	if len(findings) != 0 {
		fmt.Fprintln(stderr, "unsafe system-Python package override found:")
		for _, item := range findings {
			fmt.Fprintf(stderr, "  %s:%d\n", item.path, item.line)
		}
		fmt.Fprintln(stderr, "Create a venv and wire its interpreter/PATH explicitly.")
		return 1
	}
	fmt.Fprintf(stdout, "ra8ci no-unsafe-python-install: clean (%d first-party files)\n", len(paths))
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
		info, err := os.Stat(filepath.Join(root, filepath.FromSlash(rel)))
		if err == nil && info.Mode().IsRegular() {
			selected[rel] = true
		}
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

func isExcluded(rel string) bool {
	for _, prefix := range excludedPrefixes {
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
	lineBreaks := strings.NewReplacer("\r\n", "\n", "\r", "\n", "\v", "\n", "\f", "\n",
		"\u001c", "\n", "\u001d", "\n", "\u001e", "\n", "\u0085", "\n", "\u2028", "\n", "\u2029", "\n")
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
		for number, line := range strings.Split(lineBreaks.Replace(string(data)), "\n") {
			if strings.Contains(line, forbidden) {
				findings = append(findings, finding{path: rel, line: number + 1})
			}
		}
	}
	return findings, nil
}

func selfTest(stdout, stderr io.Writer) bool {
	unsafe := "python3 -m pip install " + forbidden + " libclang"
	cases := []struct {
		text string
		want []int
		name string
	}{
		{unsafe, []int{1}, "active unsafe install"},
		{"hint: " + unsafe, []int{1}, "documentation hint"},
		{"python3 -m venv .venv\n.venv/bin/pip install libclang", nil, "virtual environment install"},
		{"python3 -m pip --version", nil, "non-mutating pip probe"},
	}
	for _, item := range cases {
		got := scanText(item.text)
		if len(got) != len(item.want) || (len(got) > 0 && got[0] != item.want[0]) {
			fmt.Fprintf(stderr, "ra8ci no-unsafe-python-install --selftest: FAIL: %s\n", item.name)
			return false
		}
	}
	fmt.Fprintf(stdout, "ra8ci no-unsafe-python-install --selftest: PASS (%d cases)\n", len(cases))
	return true
}

func scanText(text string) []int {
	lineBreaks := strings.NewReplacer("\r\n", "\n", "\r", "\n", "\v", "\n", "\f", "\n",
		"\u001c", "\n", "\u001d", "\n", "\u001e", "\n", "\u0085", "\n", "\u2028", "\n", "\u2029", "\n")
	var lines []int
	for number, line := range strings.Split(lineBreaks.Replace(text), "\n") {
		if strings.Contains(line, forbidden) {
			lines = append(lines, number+1)
		}
	}
	return lines
}
