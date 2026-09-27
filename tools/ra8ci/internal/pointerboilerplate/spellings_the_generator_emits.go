// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package pointerboilerplate

import "regexp"

// generated matches the pointer-only definition comment in the spellings the
// generator actually emits into this tree.
//
// The original pattern knew two of them, "header" and "the internal header",
// and only the block-comment form. Neither limit holds. The spelling emitted
// most often in this checkout is "the public header", seventeen times against
// two for "the internal header", and the generator does not care which subtree
// it writes into: the gate's scope, apps/ and examples/, would have taken that
// spelling untouched. The same comment written with a line comment is the same
// generated comment, and the gate never looked for it at all.
//
// What the gate is rejecting is a comment whose entire content is that
// sentence, so the sentence still has to be the whole comment and the comment
// still has to be the whole line. That is what keeps an implementation-specific
// note ("... -- bounded scan.") and a string literal holding the same words out
// of the findings; both are pinned by the self-test and by the package test.
var generated = regexp.MustCompile(`(?i)^\s*(?:/\*\s*see (?:the )?(?:public |internal |private )?header for the documented contract\.\s*\*/|//\s*see (?:the )?(?:public |internal |private )?header for the documented contract\.)\s*$`)

// lineIsGeneratedPointerComment reports whether a line is nothing but the
// generated pointer-only definition comment.
func lineIsGeneratedPointerComment(line string) bool {
	return generated.MatchString(line)
}
