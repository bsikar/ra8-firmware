// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

// Package driverasmguard prevents HAL drivers from compiling bare CPU asm out
// of host tests behind RA8_OFF_TARGET conditionals.
package driverasmguard

import (
	"context"
	"fmt"
	"io"
	"os"
	"path/filepath"
	"strings"
)

type finding struct {
	line int
	text string
}

// Run performs the self-test or checks every top-level C translation unit in
// libs/ra8_hal/src. A missing scan root is a failure, never an empty pass.
func Run(ctx context.Context, root string, args []string, stdout, stderr io.Writer) int {
	if ctx == nil || root == "" || stdout == nil || stderr == nil {
		fmt.Fprintln(stderr, "ra8ci driver-asm-guard: invalid input")
		return 2
	}
	if len(args) == 1 && args[0] == "--selftest" {
		return selfTest(stdout, stderr)
	}
	if len(args) != 0 {
		fmt.Fprintln(stderr, "usage: ra8ci driver-asm-guard [--selftest]")
		return 2
	}
	dir := filepath.Join(root, "libs", "ra8_hal", "src")
	info, err := os.Stat(dir)
	if err != nil || !info.IsDir() {
		fmt.Fprintf(stderr, "ra8ci driver-asm-guard: driver dir not found: %s\n", dir)
		return 1
	}
	files, err := filepath.Glob(filepath.Join(dir, "*.c"))
	if err != nil {
		fmt.Fprintln(stderr, "ra8ci driver-asm-guard: driver discovery failed:", err)
		return 2
	}
	type locatedFinding struct {
		path string
		item finding
	}
	var problems []locatedFinding
	for _, path := range files {
		if err := ctx.Err(); err != nil {
			fmt.Fprintln(stderr, "ra8ci driver-asm-guard: cancelled:", err)
			return 2
		}
		body, err := os.ReadFile(path)
		if err != nil {
			fmt.Fprintf(stderr, "ra8ci driver-asm-guard: cannot read %s: %v\n", path, err)
			return 2
		}
		rel, err := filepath.Rel(root, path)
		if err != nil {
			fmt.Fprintln(stderr, "ra8ci driver-asm-guard: cannot relativize driver path:", err)
			return 2
		}
		for _, item := range checkSource(filepath.ToSlash(rel), string(body)) {
			problems = append(problems, locatedFinding{path: filepath.ToSlash(rel), item: item})
		}
	}
	if len(problems) != 0 {
		fmt.Fprintln(stdout, "ra8ci driver-asm-guard: a HAL driver guards bare asm on RA8_OFF_TARGET:")
		for _, problem := range problems {
			fmt.Fprintf(stdout, "%s:%d: inline asm '%s' sits inside a RA8_OFF_TARGET conditional -- route it through libs/ra8_hal/inc/ra8_hw_intrinsics.h instead\n",
				problem.path, problem.item.line, problem.item.text)
		}
		fmt.Fprintln(stdout, "Fix at the root -- call the ra8_hw_* primitive from")
		fmt.Fprintln(stdout, "  libs/ra8_hal/inc/ra8_hw_intrinsics.h")
		fmt.Fprintln(stdout, "(add a new one there plus its host body in")
		fmt.Fprintln(stdout, " tests/mocks/src/ra8_host_asm_stub.c if it does not exist yet).")
		return 1
	}
	fmt.Fprintf(stdout, "ra8ci driver-asm-guard: PASS -- %d HAL driver TU(s) carry no RA8_OFF_TARGET-guarded asm.\n", len(files))
	return 0
}

func selfTest(stdout, stderr io.Writer) int {
	bad := "#ifdef RA8_OFF_TARGET\nvoid f(void) { __asm(\"nop\"); }\n#else\nvoid g(void) { __asm__(\"wfi\"); }\n#endif\n"
	good := "// __asm__(\"nop\") under RA8_OFF_TARGET is prose\nvoid f(void) { ra8_hw_wfi(); }\n"
	badFindings := checkSource("libs/ra8_hal/src/bad.c", bad)
	goodFindings := checkSource("libs/ra8_hal/src/good.c", good)
	cases := []struct {
		passed bool
		label  string
	}{
		{len(badFindings) == 2, "asm in both off-target branches fires"},
		{len(goodFindings) == 0, "shared seam calls and comment lookalikes stay quiet"},
	}
	failed := 0
	for _, test := range cases {
		label := "ok"
		if !test.passed {
			label = "FAIL"
			failed++
		}
		fmt.Fprintf(stdout, "  [%s] %s\n", label, test.label)
	}
	if failed != 0 {
		fmt.Fprintf(stderr, "ra8ci driver-asm-guard --selftest: %d failure(s)\n", failed)
		return 1
	}
	fmt.Fprintln(stdout, "ra8ci driver-asm-guard --selftest: all cases pass (both directions).")
	return 0
}

func checkSource(relative, source string) []finding {
	lines := stripComments(source)
	stack := make([]bool, 0, 8)
	var problems []finding
	for index, line := range lines {
		directive, expression, ok := parseDirective(line)
		if !ok {
			if hasInlineAsm(line) && hasTrue(stack) {
				problems = append(problems, finding{line: index + 1, text: strings.TrimSpace(line)})
			}
			continue
		}
		handledDirective := true
		switch directive {
		case "if", "ifdef", "ifndef":
			stack = append(stack, strings.Contains(expression, "RA8_OFF_TARGET"))
		case "elif":
			if len(stack) > 0 {
				stack[len(stack)-1] = stack[len(stack)-1] || strings.Contains(expression, "RA8_OFF_TARGET")
			}
		case "endif":
			if len(stack) > 0 {
				stack = stack[:len(stack)-1]
			}
		default:
			handledDirective = false
		}
		if !handledDirective && hasInlineAsm(line) && hasTrue(stack) {
			problems = append(problems, finding{line: index + 1, text: strings.TrimSpace(line)})
		}
	}
	return problems
}

func parseDirective(line string) (directive, expression string, ok bool) {
	trimmed := strings.TrimSpace(line)
	if !strings.HasPrefix(trimmed, "#") {
		return "", "", false
	}
	body := strings.TrimSpace(trimmed[1:])
	if body == "" {
		return "", "", false
	}
	// Match the Python gate's \b boundary: punctuation such as '(' may
	// immediately follow a directive keyword (for example, #if(...)).
	for _, keyword := range []string{"ifdef", "ifndef", "elif", "endif", "if"} {
		if !strings.HasPrefix(body, keyword) {
			continue
		}
		if len(body) > len(keyword) && isIdentifierPart(body[len(keyword)]) {
			continue
		}
		return keyword, strings.TrimSpace(body[len(keyword):]), true
	}
	end := strings.IndexAny(body, " \t\r\n")
	if end < 0 {
		return body, "", true
	}
	return body[:end], strings.TrimSpace(body[end:]), true
}

func hasTrue(values []bool) bool {
	for _, value := range values {
		if value {
			return true
		}
	}
	return false
}

func hasInlineAsm(line string) bool {
	for index := 0; index < len(line); {
		if !isIdentifierStart(line[index]) {
			index++
			continue
		}
		end := index + 1
		for end < len(line) && isIdentifierPart(line[end]) {
			end++
		}
		token := line[index:end]
		if token == "__asm" || token == "__asm__" {
			return true
		}
		index = end
	}
	return false
}

func isIdentifierStart(value byte) bool {
	return value == '_' || value >= 'a' && value <= 'z' || value >= 'A' && value <= 'Z'
}

func isIdentifierPart(value byte) bool {
	return isIdentifierStart(value) || value >= '0' && value <= '9'
}

func stripComments(source string) []string {
	lines := strings.Split(source, "\n")
	inBlock := false
	for lineIndex, line := range lines {
		var out strings.Builder
		for index := 0; index < len(line); {
			if inBlock {
				if index+1 < len(line) && line[index:index+2] == "*/" {
					inBlock = false
					index += 2
				} else {
					index++
				}
				continue
			}
			if index+1 < len(line) {
				two := line[index : index+2]
				if two == "/*" {
					inBlock = true
					index += 2
					continue
				}
				if two == "//" {
					break
				}
			}
			out.WriteByte(line[index])
			index++
		}
		lines[lineIndex] = out.String()
	}
	return lines
}
