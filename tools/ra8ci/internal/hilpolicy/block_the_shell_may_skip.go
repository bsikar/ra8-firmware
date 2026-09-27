// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package hilpolicy

import "strings"

// The four doors before this one all hold the same line: this reader and the
// shell that sources the same file have to agree about whether the app
// declared a timeout at all. They judge the name, the "=", and the value.
// None of them judges whether the line RUNS.
//
// A hil.conf is sourced, so it is a shell script and may carry shell
// structure. This reader is line-oriented: it trims each line and asks what
// the trimmed text says. That reading is exactly right for a flat file of
// assignments, which is every hil.conf today, and it is wrong the moment an
// assignment sits inside a block the shell may not enter:
//
//	if [ "$HIL_MODE" = uart_scrape ]; then
//	  HIL_TIMEOUT_S=180
//	fi
//
// Trimmed, the middle line is indistinguishable from a top-level assignment,
// so this reader returns 180 with no reservation while the shell assigns 180
// only when the condition holds. A block whose condition is false leaves the
// bench on the runner's own fallback, which is 30s in run_local.sh and 10s in
// all.sh, and the control plane's audit record says the bound came from
// hil.conf and was 180. That is the disagreement the other doors exist to
// prevent, arriving through the one shape all four of them pass: the key
// spells the name exactly, the "=" assigns, and the value resolves cleanly.
//
// The same reading is wrong in the other direction inside a loop or a
// function body, where the line may run many times or not at all, and inside
// a subshell, where it never reaches the sourcing shell whatever happens.
//
// So a HIL_TIMEOUT_S statement read inside a block this reader cannot decide
// is a declaration it cannot read, and it is refused with the line named
// rather than quietly taken, which is how every other refusal here already
// behaves. This reader does not evaluate the condition: deciding whether the
// block runs would mean evaluating shell syntax, which is the thing
// DeclaredTimeout deliberately does not do.
//
// The boundary keeps this off every file that has no structure at all. Depth
// is counted only from block words standing in COMMAND position, so a value
// reading "if" or an app named "done" is not structure, and a file with no
// block words is exactly as flat to this rule as it was before. Structure
// this reader cannot account for is refused rather than assumed flat: a
// closer with nothing open, and a file whose blocks are still open when the
// scan ends, both mean the depth beside the declaration was not the shell's.
//
// What it cannot see is a block opened by syntax rather than a word, which a
// hil.conf has no reason to carry: a bare "(" subshell, and a "&&" or "||"
// guard. Those are named here rather than claimed. The guard, at least, the
// spelling door already catches, since a line reading `[ -n "$X" ] &&
// HIL_TIMEOUT_S=180` splits on its first "=" into a key that is not the name.

// blockOpeners are the words that, in command position, open a block whose
// body this line-oriented reader cannot decide the fate of.
var blockOpeners = map[string]bool{
	"if": true, "for": true, "while": true, "until": true,
	"case": true, "select": true, "{": true,
}

// blockClosers are the words that close one.
var blockClosers = map[string]bool{
	"fi": true, "done": true, "esac": true, "}": true,
}

// blockRunners introduce the body of a block already counted, so the word
// after them is itself in command position.
var blockRunners = map[string]bool{
	"then": true, "do": true, "else": true, "elif": true,
	"!": true, "time": true,
}

// blockDepthAfter reports the block depth a shell sourcing the file would be
// at once it has read line, given the depth it was at before. A closer with
// nothing open reports -1, which is structure this reader cannot account for.
func blockDepthAfter(line string, depth int) int {
	if depth < 0 {
		return depth
	}
	for _, word := range commandWords(line) {
		switch {
		case blockOpeners[word]:
			depth++
		case blockClosers[word]:
			depth--
			if depth < 0 {
				return -1
			}
		}
	}
	return depth
}

// commandWords reports the words of line that stand where a shell reads a
// command, which is the start of the line, the start of each segment a ";",
// "&", or "|" opens, and whatever follows a word that introduces a block
// body. A "{" or "}" standing alone as a word is reported wherever it sits,
// so that a function body opening after its name is counted.
func commandWords(line string) []string {
	var words []string
	atCommand := true
	for _, segment := range shellSegments(line) {
		for index, word := range segment {
			switch {
			case word == "{" || word == "}":
				words = append(words, word)
			case index == 0 || atCommand:
				words = append(words, word)
			}
			atCommand = (index == 0 || atCommand) && blockRunners[word]
		}
		atCommand = true
	}
	return words
}

// shellSegments splits line into the runs of words a shell would read as
// separate commands, honouring quotes and stopping at a comment. Quoting is
// what keeps a value out of this: `HIL_EXPECT="if it boots"` carries no
// command word at all.
func shellSegments(line string) [][]string {
	var (
		segments [][]string
		words    []string
		word     strings.Builder
		quote    byte
	)
	endWord := func() {
		if word.Len() > 0 {
			words = append(words, word.String())
			word.Reset()
		}
	}
	endSegment := func() {
		endWord()
		if len(words) > 0 {
			segments = append(segments, words)
			words = nil
		}
	}
	for index := 0; index < len(line); index++ {
		character := line[index]
		switch {
		case quote != 0:
			if character == quote {
				quote = 0
			}
			word.WriteByte(character)
		case character == '\'' || character == '"':
			quote = character
			word.WriteByte(character)
		case character == '#' && word.Len() == 0:
			endSegment()
			return segments
		case isBlank(character):
			endWord()
		case character == ';' || character == '&' || character == '|':
			endSegment()
		default:
			word.WriteByte(character)
		}
	}
	endSegment()
	return segments
}
