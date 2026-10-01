// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package protocol

import "unicode/utf8"

// stepNameCanBeFiled reports whether a step name is text the plane can store
// under and a reader can read back. It is the half of the step-name rule that
// nothing stated: validStepName asked only that a name be non-empty, within 128
// bytes, and free of surrounding whitespace, so every byte in between was
// accepted as long as it was not a leading or trailing space. A name carrying a
// newline, a tab, an escape sequence, or a NUL passed, and so did a name that
// was not valid UTF-8 at all.
//
// That name is not a label. It is the key the attempt's evidence is filed
// under, and it reaches a text column on the way: the store writes it as the
// step key of every log chunk (store/dispatch.go) and of every uploaded
// artifact (store/agent_artifacts.go, the agent_artifacts insert), and the
// migrations bound only its LENGTH (0001_initial.sql, 0016_agent_log_step_keys
// and 0022_agent_artifacts all CHECK length BETWEEN 1 AND 128). A text column
// holds no NUL byte and no invalid UTF-8 whatever its length, so those two
// spellings do not get refused at the protocol boundary where a bad message
// belongs; they get as far as the write and fail there, which turns a
// malformed name into a lost upload on a run that otherwise worked.
//
// The rest of the control range is the reading half. An operator identifies a
// step by that name in a log view and on the command line, and a name carrying
// a newline reads as two steps in anything line-oriented, while a name carrying
// an escape sequence rewrites the terminal that prints it. The catalog already
// holds the far end to a far narrower alphabet where steps are declared
// (catalog.go, validName: lowercase letters, digits and hyphens), so no
// first-party task can name a step this way. These names arrive from a guest
// instead, which is exactly why the boundary has to say it.
//
// C1 is refused with C0: a receipt's name is compared byte for byte against the
// key already stored (store/agent_artifacts.go, refusing a chunk whose step key
// differs), and a name that renders as nothing is a name no one can match by
// reading it.
func stepNameCanBeFiled(name string) bool {
	if !utf8.ValidString(name) {
		return false
	}
	for _, char := range name {
		if char < 0x20 || char == 0x7f || (char >= 0x80 && char <= 0x9f) {
			return false
		}
	}
	return true
}
