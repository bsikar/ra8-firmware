// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package testsreadme

import "strings"

// linesOutsideCodeFences drops every line inside a fenced code block.
//
// A table row inside a fence is an example of the row format, not a claim that
// the subdirectory exists, and reading one as documentation breaks this gate in
// both directions. An example naming a directory that was never in the tree
// fails the check for good ("documents tests/x/ but no such subdirectory
// exists"), so the format cannot be documented where it is used. An example
// naming a real directory is worse the other way: it keeps the check quiet
// after that directory's genuine row is deleted, which is exactly the drift the
// gate exists to catch.
//
// Fences are read the way CommonMark defines them: three or more backticks or
// tildes, indented no more than three spaces, closed by a line carrying at
// least as many of the same character and nothing else. An info string is
// allowed on the opening fence only, so "```text" opens a block and does not
// close one. A fence left open runs to the end of the file, so an unterminated
// example cannot leak its rows either.
func linesOutsideCodeFences(readme string) []string {
	lines := strings.Split(readme, "\n")
	outside := make([]string, 0, len(lines))
	var marker byte
	var width int
	for _, line := range lines {
		char, run, rest := fenceMarker(line)
		switch {
		case width == 0 && run != 0:
			marker, width = char, run
		case width != 0 && run != 0 && char == marker && run >= width && rest == "":
			marker, width = 0, 0
		case width == 0:
			outside = append(outside, line)
		}
	}
	return outside
}

// fenceMarker reports the fence character a line opens or closes with, the
// length of that run, and whatever follows it. A line that is not a fence
// reports a zero run, which is what every caller tests.
func fenceMarker(line string) (byte, int, string) {
	trimmed := strings.TrimLeft(line, " ")
	if len(line)-len(trimmed) > 3 {
		return 0, 0, ""
	}
	if !strings.HasPrefix(trimmed, "```") && !strings.HasPrefix(trimmed, "~~~") {
		return 0, 0, ""
	}
	char := trimmed[0]
	run := 0
	for run < len(trimmed) && trimmed[run] == char {
		run++
	}
	return char, run, strings.TrimSpace(trimmed[run:])
}
