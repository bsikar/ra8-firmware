// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

// Package pointerboilerplate rejects generated pointer-only comments in app/example source.
package pointerboilerplate

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

const minimumScopedFiles = 850

var (
	banned   = regexp.MustCompile("(?i)^\\s*/\\*\\s*see (the )?(internal )?header for the documented contract\\.\\s*\\*/\\s*$")
	prefixes = []string{"apps/", "examples/"}
	suffixes = map[string]bool{".c": true, ".cc": true, ".cpp": true, ".cxx": true, ".h": true, ".hh": true, ".hpp": true, ".hxx": true, ".m": true, ".mm": true}
)

type finding struct {
	path string
	line int
}

// Run executes the self-test or scans the live apps/examples source scope.
func Run(ctx context.Context, root string, args []string, stdout, stderr io.Writer) int {
	if ctx == nil || root == "" || stdout == nil || stderr == nil {
		fmt.Fprintln(stderr, "ra8ci pointer-boilerplate: invalid input")
		return 2
	}
	if len(args) == 1 && args[0] == "--selftest" {
		if selfTest(stdout, stderr) {
			return 0
		}
		return 1
	}
	if len(args) != 0 {
		fmt.Fprintln(stderr, "usage: ra8ci pointer-boilerplate [--selftest]")
		return 2
	}
	paths, err := scopedFiles(ctx, root)
	if err != nil {
		fmt.Fprintln(stderr, "ra8ci pointer-boilerplate: cannot enumerate source tree:", err)
		return 2
	}
	findings, err := scan(ctx, root, paths)
	if err != nil {
		fmt.Fprintln(stderr, "ra8ci pointer-boilerplate: cannot scan source tree:", err)
		return 2
	}
	if len(paths) < minimumScopedFiles {
		fmt.Fprintf(stderr, "ra8ci pointer-boilerplate: scope collapsed to %d file(s); expected at least %d\n", len(paths), minimumScopedFiles)
		return 2
	}
	if len(findings) > 0 {
		fmt.Fprintln(stderr, "Generated pointer-only definition comment(s):")
		for _, item := range findings {
			fmt.Fprintf(stderr, "  %s:%d\n", item.path, item.line)
		}
		fmt.Fprintln(stderr, "Delete the comment; the declaration owns the contract.")
		return 1
	}
	fmt.Fprintf(stdout, "ra8ci pointer-boilerplate: clean (%d app/example source files)\n", len(paths))
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
		if rel == "" {
			continue
		}
		inScope := false
		for _, prefix := range prefixes {
			inScope = inScope || strings.HasPrefix(rel, prefix)
		}
		ext := strings.ToLower(filepath.Ext(rel))
		if !inScope || !suffixes[ext] {
			continue
		}
		info, err := os.Stat(filepath.Join(root, filepath.FromSlash(rel)))
		if err == nil && info.Mode().IsRegular() {
			selected[rel] = true
		}
	}
	paths := make([]string, 0, len(selected))
	for rel := range selected {
		paths = append(paths, rel)
	}
	sort.Strings(paths)
	return paths, nil
}

func scan(ctx context.Context, root string, paths []string) ([]finding, error) {
	var out []finding
	for _, rel := range paths {
		if err := ctx.Err(); err != nil {
			return nil, err
		}
		data, err := os.ReadFile(filepath.Join(root, filepath.FromSlash(rel)))
		if err != nil {
			return nil, fmt.Errorf("read %s: %w", rel, err)
		}
		if !utf8.Valid(data) {
			return nil, fmt.Errorf("decode %s: invalid UTF-8", rel)
		}
		text := strings.NewReplacer("\r\n", "\n", "\r", "\n", "\v", "\n", "\f", "\n",
			"\u001c", "\n", "\u001d", "\n", "\u001e", "\n", "\u0085", "\n", "\u2028", "\n", "\u2029", "\n").Replace(string(data))
		for number, line := range strings.Split(text, "\n") {
			if banned.MatchString(line) {
				out = append(out, finding{path: rel, line: number + 1})
			}
		}
	}
	return out, nil
}

func selfTest(stdout, stderr io.Writer) bool {
	cases := []struct {
		line  string
		want  bool
		label string
	}{
		{"/* see header for the documented contract. */", true, "plain generated form"},
		{"/* See the internal header for the documented contract. */", true, "internal-header form"},
		{"/* see header for full description */", false, "legacy wording"},
		{"/* See header for the documented contract -- bounded scan. */", false, "implementation-specific note"},
		{"const char* text = \"see header for the documented contract.\";", false, "string literal"},
	}
	for _, item := range cases {
		if got := banned.MatchString(item.line); got != item.want {
			fmt.Fprintf(stderr, "ra8ci pointer-boilerplate --selftest: FAIL: %s\n", item.label)
			return false
		}
	}
	fmt.Fprintf(stdout, "ra8ci pointer-boilerplate --selftest: PASS (%d both-direction cases)\n", len(cases))
	return true
}
