// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

// Package gnuattribute checks first-party C/C++ sources for GNU attributes
// that should use C23 attribute syntax.
package gnuattribute

import (
	"context"
	"fmt"
	"io"
	"os"
	"path"
	"path/filepath"
	"regexp"
	"sort"
	"strings"
)

const fileFloor = 1700

var (
	roots      = map[string]bool{"libs": true, "tests": true, "examples": true, "port": true, "tools": true, "apps": true}
	exts       = map[string]bool{".c": true, ".h": true, ".cpp": true, ".hpp": true}
	allowed    = map[string]bool{"interrupt": true, "cmse_nonsecure_entry": true, "cmse_nonsecure_call": true}
	waiver     = regexp.MustCompile("ATTR-OK:\\s*\\S")
	attr       = regexp.MustCompile("__attribute__\\s*\\(\\(")
	excluded   = []string{"libs/third_party/", "apps/shared_libs/third_party/", "libs/ra8_fonts/"}
	outputDirs = map[string]bool{"CMakeFiles": true, "_deps": true, "__pycache__": true, "node_modules": true}
	buildRoots = map[string]bool{"docs": true, "examples": true, "local-poc": true, "port": true, "tests": true, "tools": true, "apps": true}
)

type finding struct {
	line    int
	snippet string
}

// Run executes the check. No args scans the tree; explicit paths narrow scope.
// A collapsed whole-tree scan, cancellation, or read failure returns 2.
func Run(ctx context.Context, root string, args []string, stdout, stderr io.Writer) int {
	if ctx == nil || root == "" || stdout == nil || stderr == nil {
		fmt.Fprintln(stderr, "ra8ci gnu-attribute: invalid input")
		return 2
	}
	if len(args) == 1 && args[0] == "--selftest" {
		return selfTest(stdout, stderr)
	}
	for _, arg := range args {
		if strings.HasPrefix(arg, "-") {
			fmt.Fprintln(stderr, "usage: ra8ci gnu-attribute [--selftest] [file ...]")
			return 2
		}
	}
	files := args
	if len(args) == 0 {
		var err error
		files, err = discover(root)
		if err != nil {
			fmt.Fprintln(stderr, "ra8ci gnu-attribute: discovery failed:", err)
			return 2
		}
		if len(files) < fileFloor {
			fmt.Fprintf(stderr, "ra8ci gnu-attribute: FATAL -- only %d first-party source file(s) in scope, floor is %d; collapsed sweep is not trustworthy\n", len(files), fileFloor)
			return 2
		}
	}
	files = unique(files)
	total, scanned := 0, 0
	for _, rel := range files {
		if err := ctx.Err(); err != nil {
			fmt.Fprintln(stderr, "ra8ci gnu-attribute: cancelled:", err)
			return 2
		}
		rel = filepath.ToSlash(rel)
		if !inScope(rel) {
			continue
		}
		body, err := os.ReadFile(filepath.Join(root, filepath.FromSlash(rel)))
		if err != nil {
			fmt.Fprintf(stderr, "ra8ci gnu-attribute: cannot read %s: %v\n", rel, err)
			return 2
		}
		scanned++
		for _, item := range scan(string(body)) {
			fmt.Fprintf(stdout, "%s:%d: GNU __attribute__ -- use C23 [[...]] syntax; %s\n", rel, item.line, item.snippet)
			total++
		}
	}
	if total > 0 {
		fmt.Fprintf(stdout, "\nra8ci gnu-attribute: %d violation(s); only interrupt / cmse_nonsecure_entry / cmse_nonsecure_call may stay, or add ATTR-OK: <reason>.\n", total)
		return 1
	}
	fmt.Fprintf(stdout, "ra8ci gnu-attribute: clean -- %d file(s) scanned.\n", scanned)
	return 0
}

func discover(root string) ([]string, error) {
	var files []string
	for name := range roots {
		base := filepath.Join(root, name)
		info, err := os.Stat(base)
		if os.IsNotExist(err) {
			continue
		}
		if err != nil {
			return nil, err
		}
		if !info.IsDir() {
			continue
		}
		err = filepath.WalkDir(base, func(full string, entry os.DirEntry, walkErr error) error {
			if walkErr != nil {
				return walkErr
			}
			if entry.IsDir() {
				return nil
			}
			rel, err := filepath.Rel(root, full)
			if err != nil {
				return err
			}
			rel = filepath.ToSlash(rel)
			if inScope(rel) {
				files = append(files, rel)
			}
			return nil
		})
		if err != nil {
			return nil, err
		}
	}
	return unique(files), nil
}

func inScope(rel string) bool {
	if rel == "" || path.IsAbs(rel) || path.Clean(rel) != rel || strings.HasPrefix(rel, "../") {
		return false
	}
	parts := strings.Split(rel, "/")
	if !roots[parts[0]] || !exts[strings.ToLower(path.Ext(rel))] {
		return false
	}
	for _, prefix := range excluded {
		if strings.HasPrefix(rel, prefix) {
			return false
		}
	}
	for i, part := range parts[:len(parts)-1] {
		if outputDirs[part] || (isBuildDir(part) && (i == 0 || buildRoots[parts[0]])) {
			return false
		}
	}
	return true
}

func isBuildDir(s string) bool {
	return s == "build" || strings.HasPrefix(s, "build-") || strings.HasPrefix(s, "build_") || strings.HasPrefix(s, "cmake-build-")
}

func unique(values []string) []string {
	seen := make(map[string]bool, len(values))
	out := make([]string, 0, len(values))
	for _, v := range values {
		if !seen[v] {
			seen[v] = true
			out = append(out, v)
		}
	}
	sort.Strings(out)
	return out
}

// scan masks comments and C string/character literals, preserving line positions.
func scan(text string) []finding {
	lines, code := strings.Split(text, "\n"), strings.Split(mask(text), "\n")
	var found []finding
	for i, line := range code {
		for _, m := range attr.FindAllStringIndex(line, -1) {
			if waiver.MatchString(lines[i]) {
				continue
			}
			body, ok := attrBody(line, m[0])
			if ok {
				names := attrNames(body)
				if len(names) > 0 {
					exempt := true
					for n := range names {
						if !allowed[n] {
							exempt = false
						}
					}
					if exempt {
						continue
					}
				}
			}
			snippet := strings.TrimSpace(lines[i])
			if len(snippet) > 100 {
				snippet = snippet[:100]
			}
			found = append(found, finding{i + 1, snippet})
		}
	}
	return found
}

func attrBody(s string, pos int) (string, bool) {
	start := strings.Index(s[pos:], "((")
	if start < 0 {
		return "", false
	}
	start += pos + 2
	depth := 1
	for i := start; i < len(s); i++ {
		if s[i] == '(' {
			depth++
		}
		if s[i] == ')' {
			depth--
			if depth == 0 {
				return s[start:i], true
			}
		}
	}
	return "", false
}

func attrNames(body string) map[string]bool {
	out := map[string]bool{}
	for _, item := range strings.Split(body, ",") {
		name := strings.TrimSpace(item)
		if i := strings.IndexByte(name, '('); i >= 0 {
			name = name[:i]
		}
		name = strings.TrimSpace(name)
		if len(name) > 4 && strings.HasPrefix(name, "__") && strings.HasSuffix(name, "__") {
			name = name[2 : len(name)-2]
		}
		if name != "" {
			out[name] = true
		}
	}
	return out
}

func mask(src string) string {
	b := []byte(src)
	const (
		normal = iota
		block
		line
		quoted
		character
	)
	state := normal
	for i := 0; i < len(b); i++ {
		c, n := b[i], byte(0)
		if i+1 < len(b) {
			n = b[i+1]
		}
		switch state {
		case block:
			if c == '*' && n == '/' {
				b[i], b[i+1] = ' ', ' '
				i++
				state = normal
			} else if c != '\n' && c != '\r' {
				b[i] = ' '
			}
		case line:
			if c == '\n' || c == '\r' {
				state = normal
			} else {
				b[i] = ' '
			}
		case quoted, character:
			if c == '\\' {
				if c != '\n' && c != '\r' {
					b[i] = ' '
				}
				if i+1 < len(b) && b[i+1] != '\n' && b[i+1] != '\r' {
					b[i+1] = ' '
				}
				i++
			} else if (state == quoted && c == '"') || (state == character && c == '\'') {
				b[i] = ' '
				state = normal
			} else if c != '\n' && c != '\r' {
				b[i] = ' '
			}
		default:
			if c == '/' && n == '*' {
				b[i], b[i+1] = ' ', ' '
				i++
				state = block
			} else if c == '/' && n == '/' {
				b[i], b[i+1] = ' ', ' '
				i++
				state = line
			} else if c == '"' {
				b[i] = ' '
				state = quoted
			} else if c == '\'' {
				b[i] = ' '
				state = character
			}
		}
	}
	return string(b)
}

func selfTest(out, errOut io.Writer) int {
	cases := []struct {
		name, src string
		want      int
	}{
		{"detect weak", "void f(void) __attribute__((weak));\n", 1},
		{"detect dunder packed", "int x __attribute__((__packed__));\n", 1},
		{"allowed interrupt", "void irq(void) __attribute__((interrupt));\n", 0},
		{"allowed CMSE", "void f(void) __attribute__((__cmse_nonsecure_entry__));\nvoid g(void) __attribute__((cmse_nonsecure_call));\n", 0},
		{"waiver", "int x __attribute__((packed)); /* ATTR-OK: ABI */\n", 0},
		{"comment/string prose", "// __attribute__((weak))\n/* __attribute__((weak)) */\nchar *s = \"__attribute__((weak))\";\n", 0},
		{"C23", "[[gnu::weak]] void f(void);\n", 0},
	}
	failed := 0
	for _, tc := range cases {
		ok := len(scan(tc.src)) == tc.want
		label := "FAIL"
		if ok {
			label = "ok"
		} else {
			failed++
		}
		fmt.Fprintf(out, "  [%s] %s\n", label, tc.name)
	}
	if failed > 0 {
		fmt.Fprintf(errOut, "ra8ci gnu-attribute selftest: %d failure(s)\n", failed)
		return 1
	}
	fmt.Fprintln(out, "ra8ci gnu-attribute selftest: pass (detections and exemptions)")
	return 0
}
