// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

// Package committerms checks commit-message text for deprecated SPI/I2C terminology.
package committerms

import (
	"context"
	"fmt"
	"io"
	"regexp"
	"strings"
	"unicode"
	"unicode/utf8"
)

type termPattern struct {
	pattern *regexp.Regexp
	message string
}

var banned = []termPattern{
	{regexp.MustCompile(`[Mm][Aa][Ss][Tt][Ee][Rr]([Ss][Hh][Ii][Pp]|[Ii][Nn][Gg]|[Ee][Dd]|[Ss])?`), "master -- use Primary/Controller"},
	{regexp.MustCompile(`[Ss][Ll][Aa][Vv][Ee]([Ss]|[Dd])?`), "slave -- use Peripheral"},
	{regexp.MustCompile(`MOSI`), "MOSI -- use COPI"},
	{regexp.MustCompile(`MISO`), "MISO -- use CIPO"},
	{regexp.MustCompile(`[Ss][Ll][Aa][Vv][Ee][ _-][Ss][Ee][Ll][Ee][Cc][Tt]`), "Slave Select -- use CS"},
}

// Run checks commit-message text from stdin, or verifies the detector with --selftest.
func Run(ctx context.Context, args []string, stdin io.Reader, stdout, stderr io.Writer) int {
	if ctx == nil || stdin == nil || stdout == nil || stderr == nil {
		if stderr != nil {
			fmt.Fprintln(stderr, "inclusive-terminology-commits: invalid input")
		}
		return 2
	}
	if len(args) != 0 {
		if len(args) == 1 && args[0] == "--selftest" {
			if selfTest(stdout, stderr) {
				return 0
			}
			return 1
		}
		fmt.Fprintln(stderr, "usage: ra8ci inclusive-terminology-commits [--selftest] < commit-messages")
		return 2
	}
	text, err := io.ReadAll(stdin)
	if err != nil {
		fmt.Fprintln(stderr, "inclusive-terminology-commits: read stdin:", err)
		return 2
	}
	if err := ctx.Err(); err != nil {
		fmt.Fprintln(stderr, "inclusive-terminology-commits:", err)
		return 2
	}
	violations := FindViolations(string(text))
	if len(violations) > 0 {
		fmt.Fprintln(stdout, "[FAIL] Non-inclusive terminology in commit message(s):")
		for _, violation := range violations {
			fmt.Fprintln(stdout, violation)
		}
		return 1
	}
	fmt.Fprintln(stdout, "[PASS] Commit message terminology clean.")
	return 0
}

// FindViolations applies the paragraph-scoped LEGACY-OK exemption to each line.
func FindViolations(text string) []string {
	lines := splitLines(text)
	var violations []string
	paragraphStart := 0
	for index := 0; index <= len(lines); index++ {
		if index < len(lines) && strings.TrimSpace(lines[index]) != "" {
			continue
		}
		paragraph := lines[paragraphStart:index]
		if len(paragraph) > 0 && !paragraphHasLegacyOK(paragraph) {
			for offset, line := range paragraph {
				if message, ok := firstViolation(line); ok {
					violations = append(violations, fmt.Sprintf("  line %d: %s\n    > %s", paragraphStart+offset+1, message, strings.TrimSpace(line)))
				}
			}
		}
		paragraphStart = index + 1
	}
	return violations
}

func splitLines(text string) []string {
	var lines []string
	start := 0
	for index, value := range text {
		if index < start {
			continue
		}
		if !strings.ContainsRune("\n\r\v\f\u001c\u001d\u001e\u0085\u2028\u2029", value) {
			continue
		}
		lines = append(lines, text[start:index])
		start = index + utf8.RuneLen(value)
		if value == '\r' && start < len(text) && text[start] == '\n' {
			start++
		}
	}
	if start < len(text) {
		lines = append(lines, text[start:])
	}
	return lines
}

func paragraphHasLegacyOK(lines []string) bool {
	for _, line := range lines {
		runes := []rune(line)
		for index := 0; index+len("LEGACY-OK") <= len(runes); index++ {
			if !strings.EqualFold(string(runes[index:index+len("LEGACY-OK")]), "LEGACY-OK") {
				continue
			}
			cursor := index + len("LEGACY-OK")
			for cursor < len(runes) && unicode.IsSpace(runes[cursor]) {
				cursor++
			}
			if cursor < len(runes) && runes[cursor] == ':' {
				return true
			}
		}
	}
	return false
}

func firstViolation(line string) (string, bool) {
	for _, item := range banned {
		for _, location := range item.pattern.FindAllStringIndex(line, -1) {
			if hasWordBoundaries(line, location[0], location[1]) {
				return item.message, true
			}
		}
	}
	return "", false
}

func hasWordBoundaries(text string, start, end int) bool {
	if start > 0 {
		previous, _ := utf8.DecodeLastRuneInString(text[:start])
		if isWord(previous) {
			return false
		}
	}
	if end < len(text) {
		next, _ := utf8.DecodeRuneInString(text[end:])
		if isWord(next) {
			return false
		}
	}
	return true
}

func isWord(value rune) bool {
	return value == '_' || unicode.IsLetter(value) || unicode.IsNumber(value)
}

func selfTest(stdout, stderr io.Writer) bool {
	fired := FindViolations("fix(spi): rework the MOSI/MISO pin mux\n")
	if len(fired) == 0 {
		fmt.Fprintln(stderr, "[SELFTEST FAIL] an un-annotated MOSI in a commit message was not flagged.")
		return false
	}
	quiet := FindViolations("ci(gates): widen scope\n\nWidening surfaced only verbatim upstream terms (IEEE 1588 PTP\nmaster/slave, datasheet MOSI/MISO pin labels), LEGACY-OK: upstream\ndomain terminology quoted verbatim, not our naming.\n")
	if len(quiet) != 0 {
		fmt.Fprintln(stderr, "[SELFTEST FAIL] a paragraph-scoped LEGACY-OK opt-out did not cover its whole paragraph:")
		for _, violation := range quiet {
			fmt.Fprintln(stderr, violation)
		}
		return false
	}
	crossParagraph := FindViolations("fix(spi): rework the MOSI pin mux\n\nLEGACY-OK: unrelated note in the next paragraph\n")
	if len(crossParagraph) == 0 {
		fmt.Fprintln(stderr, "[SELFTEST FAIL] LEGACY-OK in one paragraph suppressed a violation in a different paragraph.")
		return false
	}
	fmt.Fprintln(stdout, "[SELFTEST OK] fires on an un-annotated term, stays quiet on a wrapped")
	fmt.Fprintln(stdout, "              paragraph-scoped LEGACY-OK, and does not leak across paragraphs.")
	return true
}
