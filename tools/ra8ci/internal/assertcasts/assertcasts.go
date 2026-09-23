// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

// Package assertcasts checks TEST_ASSERT_EQ arguments for redundant leading
// integer casts.
package assertcasts

import (
	"context"
	"fmt"
	"io"
	"os"
	"path/filepath"
	"regexp"
	"sort"
	"strings"
)

const macro = "TEST_ASSERT_EQ("

var cast = regexp.MustCompile(`^\s*\((?:u?int(?:8|16|32|64)?_t|int|size_t|ssize_t)\)`)

// Run checks explicitly named files, or all tests/**/*.c files with --all.
// Violations return 1; invalid arguments and IO errors return 2.
func Run(ctx context.Context, root string, args []string, stdout, stderr io.Writer) int {
	if ctx == nil || root == "" || stdout == nil || stderr == nil {
		fmt.Fprintln(stderr, "ra8ci assert-casts: invalid input")
		return 2
	}
	if len(args) == 1 && args[0] == "--selftest" {
		return selfTest(stdout, stderr)
	}
	if len(args) == 1 && args[0] == "--all" {
		var err error
		args, err = discover(root)
		if err != nil {
			fmt.Fprintln(stderr, "ra8ci assert-casts: discovery failed:", err)
			return 2
		}
	}
	if len(args) == 0 {
		fmt.Fprintln(stderr, "usage: ra8ci assert-casts <file> [...] or ra8ci assert-casts --all")
		return 1
	}
	for _, arg := range args {
		if strings.HasPrefix(arg, "-") {
			fmt.Fprintln(stderr, "ra8ci assert-casts: unknown or incompatible arguments")
			return 2
		}
	}
	count := 0
	for _, name := range args {
		if err := ctx.Err(); err != nil {
			fmt.Fprintln(stderr, "ra8ci assert-casts: cancelled:", err)
			return 2
		}
		path := name
		if !filepath.IsAbs(path) {
			path = filepath.Join(root, path)
		}
		content, err := os.ReadFile(path)
		if err != nil {
			fmt.Fprintf(stderr, "ra8ci assert-casts: cannot read %s: %v\n", name, err)
			return 2
		}
		for _, finding := range scan(decodeASCIIReplace(content), name) {
			fmt.Fprintln(stdout, finding)
			count++
		}
	}
	if count > 0 {
		fmt.Fprintf(stderr, "\n%d redundant cast(s) in TEST_ASSERT_EQ.\nRemove the redundant casts before retrying.\n", count)
		return 1
	}
	return 0
}

func discover(root string) ([]string, error) {
	var files []string
	err := filepath.WalkDir(filepath.Join(root, "tests"), func(path string, entry os.DirEntry, walkErr error) error {
		if walkErr != nil {
			return walkErr
		}
		if !entry.IsDir() && filepath.Ext(path) == ".c" {
			files = append(files, path)
		}
		return nil
	})
	sort.Strings(files)
	return files, err
}

func decodeASCIIReplace(data []byte) string {
	var b strings.Builder
	b.Grow(len(data))
	for _, ch := range data {
		if ch < 0x80 {
			b.WriteByte(ch)
		} else {
			b.WriteRune('�')
		}
	}
	return b.String()
}

func scan(content, path string) []string {
	var out []string
	for pos := 0; ; {
		rel := strings.Index(content[pos:], macro)
		if rel < 0 {
			break
		}
		idx := pos + rel
		start := idx + len(macro)
		close := findCloseParen(content, start)
		inner := content[start:close]
		depth, split := 0, -1
		for i, ch := range inner {
			switch ch {
			case '(', '[', '{':
				depth++
			case ')', ']', '}':
				depth--
			case ',':
				if depth == 0 {
					split = i
				}
			}
			if split >= 0 {
				break
			}
		}
		if split >= 0 {
			line := strings.Count(content[:idx], "\n") + 1
			a, b := inner[:split], inner[split+1:]
			if cast.MatchString(a) {
				out = append(out, fmt.Sprintf("%s:%d: cast in first arg of TEST_ASSERT_EQ: %s%s...", path, line, macro, trunc(strings.TrimSpace(a), 60)))
			}
			if cast.MatchString(b) {
				out = append(out, fmt.Sprintf("%s:%d: cast in second arg of TEST_ASSERT_EQ: ...%s", path, line, trunc(strings.TrimSpace(b), 60)))
			}
		}
		pos = close + 1
	}
	return out
}

func findCloseParen(s string, start int) int {
	depth := 1
	for i := start; i < len(s); i++ {
		switch s[i] {
		case '(':
			depth++
		case ')':
			depth--
			if depth == 0 {
				return i
			}
		}
	}
	return len(s) - 1
}

func trunc(s string, n int) string {
	runes := []rune(s)
	if len(runes) > n {
		return string(runes[:n])
	}
	return s
}

func selfTest(out, errOut io.Writer) int {
	bad := scan("TEST_ASSERT_EQ((int)value, (uint32_t)expected);\n", "bad.c")
	good := scan("TEST_ASSERT_EQ(value, expected);\nTEST_ASSERT_EQ(load((int)value), expected);\n", "good.c")
	raw := scan("// TEST_ASSERT_EQ((int)a, b)\nconst char *s = \"TEST_ASSERT_EQ(a, (size_t)b)\";\n", "raw.c")
	nested := scan("TEST_ASSERT_EQ(fn(a, b), (ssize_t)e);\nTEST_ASSERT_EQ((int)(a[1, 2]), v);\nTEST_ASSERT_EQ((int)x);\n", "nested.c")
	cases := []struct {
		ok   bool
		name string
	}{
		{len(bad) == 2, "leading casts on both arguments fire"},
		{len(good) == 0, "clean and nested casts stay quiet"},
		{len(raw) == 2, "raw-text matching includes comments and strings"},
		{len(nested) == 2, "top-level comma splitting and malformed calls"},
	}
	fail := 0
	for _, tc := range cases {
		label := "ok"
		if !tc.ok {
			label, fail = "FAIL", fail+1
		}
		fmt.Fprintf(out, "  [%s] %s\n", label, tc.name)
	}
	if fail > 0 {
		fmt.Fprintf(errOut, "ra8ci assert-casts --selftest: %d failure(s)\n", fail)
		return 1
	}
	fmt.Fprintln(out, "ra8ci assert-casts --selftest: all cases pass (both directions).")
	return 0
}
