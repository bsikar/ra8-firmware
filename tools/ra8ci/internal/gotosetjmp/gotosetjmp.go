// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

// Package gotosetjmp enforces the parser-independent NASA Power-of-10 Rule 1
// backstop for first-party C and C++ translation units.
package gotosetjmp

import (
	"context"
	"fmt"
	"io"
	"os"
	"os/exec"
	"path"
	"path/filepath"
	"strings"
)

type finding struct {
	line   int
	token  string
	source string
}

var sourceRoots = map[string]struct{}{
	"libs": {}, "examples": {}, "port": {}, "tools": {}, "apps": {},
}

var excludedPrefixes = []string{
	"libs/third_party/",
	"apps/shared_libs/third_party/",
	"libs/ra8_fonts/",
}

var sourceExtensions = map[string]struct{}{
	".c": {}, ".h": {}, ".cpp": {}, ".hpp": {},
}

var toolOutputDirectories = map[string]struct{}{
	"CMakeFiles": {}, "_deps": {}, "__pycache__": {}, "node_modules": {},
}

var buildTreeRoots = map[string]struct{}{
	"docs": {}, "examples": {}, "local-poc": {}, "port": {},
	"tests": {}, "tools": {}, "apps": {},
}

// Run performs the self-test or scans the tracked and untracked, non-ignored
// first-party C/C++ source files beneath root. A Git or file read failure is a
// failure rather than a vacuous clean result.
func Run(ctx context.Context, root string, args []string, stdout, stderr io.Writer) int {
	if ctx == nil || root == "" || stdout == nil || stderr == nil {
		fmt.Fprintln(stderr, "ra8ci no-goto-setjmp: invalid input")
		return 2
	}
	if len(args) == 1 && args[0] == "--selftest" {
		return selfTest(stdout, stderr)
	}
	if len(args) != 0 {
		fmt.Fprintln(stderr, "usage: ra8ci no-goto-setjmp [--selftest]")
		return 2
	}
	paths, err := trackedSourcePaths(ctx, root)
	if err != nil {
		fmt.Fprintln(stderr, "ra8ci no-goto-setjmp:", err)
		return 2
	}
	total := 0
	for _, rel := range paths {
		if err := ctx.Err(); err != nil {
			fmt.Fprintln(stderr, "ra8ci no-goto-setjmp: cancelled:", err)
			return 2
		}
		body, err := os.ReadFile(filepath.Join(root, filepath.FromSlash(rel)))
		if err != nil {
			fmt.Fprintf(stderr, "ra8ci no-goto-setjmp: cannot read %s: %v\n", rel, err)
			return 2
		}
		for _, item := range scanText(string(body)) {
			fmt.Fprintf(stderr, "%s:%d: `%s` is banned (NASA Power-of-10 Rule 1: no goto/setjmp/longjmp): %s\n",
				rel, item.line, item.token, item.source)
			total++
		}
	}
	if total != 0 {
		fmt.Fprintf(stderr, "ra8ci no-goto-setjmp: %d banned control-flow token(s) found.\n", total)
		return 1
	}
	fmt.Fprintf(stdout, "ra8ci no-goto-setjmp: PASS -- %d file(s) scanned, 0 findings.\n", len(paths))
	return 0
}

func trackedSourcePaths(ctx context.Context, root string) ([]string, error) {
	command := exec.CommandContext(ctx, "git", "-C", root, "ls-files", "-z", "--cached", "--others", "--exclude-standard")
	output, err := command.Output()
	if err != nil {
		if ctx.Err() != nil {
			return nil, fmt.Errorf("source enumeration cancelled: %w", ctx.Err())
		}
		return nil, fmt.Errorf("enumerate repository sources: %w", err)
	}
	unique := make(map[string]struct{})
	paths := make([]string, 0, 4096)
	for _, raw := range strings.Split(string(output), "\x00") {
		if raw == "" || !isScopedPath(raw) {
			continue
		}
		rel := filepath.ToSlash(raw)
		if _, exists := unique[rel]; exists {
			continue
		}
		unique[rel] = struct{}{}
		paths = append(paths, rel)
	}
	return paths, nil
}

func isScopedPath(raw string) bool {
	rel := filepath.ToSlash(raw)
	if rel == "" || path.IsAbs(rel) || path.Clean(rel) != rel || strings.HasPrefix(rel, "../") {
		return false
	}
	extension := strings.ToLower(path.Ext(rel))
	if _, ok := sourceExtensions[extension]; !ok {
		return false
	}
	first := strings.SplitN(rel, "/", 2)[0]
	if _, ok := sourceRoots[first]; !ok {
		return false
	}
	for _, prefix := range excludedPrefixes {
		if strings.HasPrefix(rel, prefix) {
			return false
		}
	}
	return !isBuildOutputPath(rel)
}

func isBuildOutputPath(rel string) bool {
	parts := strings.Split(rel, "/")
	for index, part := range parts[:len(parts)-1] {
		if _, reserved := toolOutputDirectories[part]; reserved {
			return true
		}
		if isBuildDirectory(part) {
			if index == 0 {
				return true
			}
			if _, buildRoot := buildTreeRoots[parts[0]]; buildRoot {
				return true
			}
		}
	}
	return false
}

func isBuildDirectory(name string) bool {
	return name == "build" || strings.HasPrefix(name, "build-") ||
		strings.HasPrefix(name, "build_") || strings.HasPrefix(name, "cmake-build-")
}

func scanText(text string) []finding {
	lines := splitLines(text)
	var findings []finding
	inBlockComment := false
	inString := false
	inCharacter := false
	continuedLiteral := false
	for lineIndex, raw := range lines {
		for index := 0; index < len(raw); {
			current := raw[index]
			next := byte(0)
			if index+1 < len(raw) {
				next = raw[index+1]
			}
			if inBlockComment {
				if current == '*' && next == '/' {
					inBlockComment = false
					index += 2
				} else {
					index++
				}
				continue
			}
			if inString || inCharacter {
				quote := byte('"')
				if inCharacter {
					quote = '\''
				}
				if current == '\\' {
					if index+1 < len(raw) {
						index += 2
						continue
					}
					continuedLiteral = true
					index++
					continue
				}
				if current == quote {
					inString = false
					inCharacter = false
				}
				index++
				continue
			}
			if current == '/' && next == '/' {
				break
			}
			if current == '/' && next == '*' {
				inBlockComment = true
				index += 2
				continue
			}
			if current == '"' {
				inString = true
				index++
				continue
			}
			if current == '\'' {
				inCharacter = true
				index++
				continue
			}
			if !identifierStart(current) {
				index++
				continue
			}
			end := index + 1
			for end < len(raw) && identifierPart(raw[end]) {
				end++
			}
			token := raw[index:end]
			if token == "goto" || token == "setjmp" || token == "longjmp" {
				findings = append(findings, finding{line: lineIndex + 1, token: token, source: strings.TrimSpace(raw)})
			}
			index = end
		}
		if inString || inCharacter {
			if continuedLiteral {
				continuedLiteral = false
			} else {
				inString = false
				inCharacter = false
			}
		}
	}
	return findings
}

func splitLines(text string) []string {
	if text == "" {
		return nil
	}
	lines := make([]string, 0, strings.Count(text, "\n")+1)
	for start := 0; start < len(text); {
		relativeEnd := strings.IndexAny(text[start:], "\r\n")
		if relativeEnd < 0 {
			lines = append(lines, text[start:])
			break
		}
		end := start + relativeEnd
		lines = append(lines, text[start:end])
		if text[end] == '\r' && end+1 < len(text) && text[end+1] == '\n' {
			start = end + 2
		} else {
			start = end + 1
		}
	}
	return lines
}

func identifierStart(value byte) bool {
	return value == '_' || value >= 'a' && value <= 'z' || value >= 'A' && value <= 'Z'
}

func identifierPart(value byte) bool {
	return identifierStart(value) || value >= '0' && value <= '9'
}

func selfTest(stdout, stderr io.Writer) int {
	bad := "void f(int *buf)\n{\n    goto done;\ndone:\n    setjmp(buf);\n    longjmp(buf, 1);\n}\n"
	good := "/* goto setjmp longjmp stay quiet */\n" +
		"// setjmp longjmp stay quiet\n" +
		"static const char *note = \"goto setjmp longjmp\";\n" +
		"void f(void) { for (int goto_count = 0; goto_count < 1; ++goto_count) {} }\n" +
		"const char *marker = \"/*\"; goto allowed_code;\n"
	badFindings := scanText(bad)
	seen := make(map[string]struct{})
	for _, item := range badFindings {
		seen[item.token] = struct{}{}
	}
	quietFindings := scanText(good)
	scopeCases := []struct {
		path string
		want bool
	}{
		{"tools/ra8ci/main.go", false},
		{"tools/ra8ci/main.cpp", true},
		{"examples/app/src/main.c", true},
		{"tests/test.c", false},
		{"libs/third_party/vendor.c", false},
		{"apps/shared_libs/third_party/vendor.cpp", false},
		{"libs/ra8_fonts/table.c", false},
		{"tools/project/build/out.c", false},
		{"tools/builders/build_app.c", true},
	}
	failures := 0
	checks := []struct {
		passed bool
		label  string
	}{
		{len(badFindings) == 3 && len(seen) == 3, "code-position goto/setjmp/longjmp all fire"},
		{len(quietFindings) == 1 && quietFindings[0].token == "goto" && quietFindings[0].line == 5,
			"comments, literals, identifier substrings, and comment markers in strings stay quiet"},
	}
	for _, item := range scopeCases {
		checks = append(checks, struct {
			passed bool
			label  string
		}{isScopedPath(item.path) == item.want, "scope " + item.path})
	}
	for _, check := range checks {
		label := "ok"
		if !check.passed {
			label = "FAIL"
			failures++
		}
		fmt.Fprintf(stdout, "  [%s] %s\n", label, check.label)
	}
	if failures != 0 {
		fmt.Fprintf(stderr, "ra8ci no-goto-setjmp --selftest: %d failure(s)\n", failures)
		return 1
	}
	fmt.Fprintln(stdout, "ra8ci no-goto-setjmp --selftest: all cases pass.")
	return 0
}
