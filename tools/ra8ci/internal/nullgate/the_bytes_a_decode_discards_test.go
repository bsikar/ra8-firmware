// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package nullgate

import (
	"bytes"
	"strings"
	"testing"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/testprivatefile"
)

// This gate has to agree with Python's UTF-8 decoder about how much of a bad
// sequence to throw away, because a disagreement shifts every line number
// after it and the findings then point at the wrong code. These are the lead
// bytes and truncations where the two could drift apart.
func TestHowMuchOfABadSequenceIsDiscarded(t *testing.T) {
	for name, item := range map[string]struct {
		data []byte
		want int
	}{
		// A two-byte lead with nothing after it. The lead is well formed, so
		// the length is what condemns it, and only the lead is discarded.
		"truncated two-byte lead": {[]byte{0xc3}, 1},
		// Four-byte lead, valid second, then one continuation and the data
		// runs out. Everything read so far is consumed rather than one byte.
		"truncated four-byte sequence": {[]byte{0xf0, 0x90, 0x80}, 3},
		// The same shape for a three-byte lead.
		"truncated three-byte sequence": {[]byte{0xe0, 0xa0, 0x80}, 3},
		// F0 narrows its second byte to 90..BF, so 0x80 is an overlong
		// encoding and the lead alone is discarded.
		"overlong four-byte second": {[]byte{0xf0, 0x80, 0x80}, 1},
		// F4 narrows the other way, to 80..8F, above the Unicode maximum.
		"four-byte past the maximum": {[]byte{0xf4, 0x90, 0x80}, 1},
		// E0 and ED are the other two leads with narrowed seconds: overlong,
		// and the surrogate range.
		"overlong three-byte second":  {[]byte{0xe0, 0x80, 0x80}, 1},
		"surrogate three-byte second": {[]byte{0xed, 0xa0, 0x80}, 1},
		// A continuation byte standing where a lead should be, and the two
		// leads that are never valid anywhere.
		"continuation as lead": {[]byte{0x80, 0x80}, 1},
		"c0 is never a lead":   {[]byte{0xc0, 0x80}, 1},
		"ff is never a lead":   {[]byte{0xff, 0x80}, 1},
	} {
		if got := invalidUTF8Prefix(item.data); got != item.want {
			t.Errorf("%s: discarded %d bytes of % x, want %d", name, got, item.data, item.want)
		}
	}
}

// A decode that discards the right number of bytes keeps the line numbering
// true, which is the whole reason the prefix length matters.
func TestABadSequenceDoesNotShiftTheLinesAfterIt(t *testing.T) {
	data := append([]byte("first\n"), 0xf0, 0x90, 0x80)
	data = append(data, []byte("\nthird\n")...)
	lines := strings.Split(decodeReplace(data), "\n")
	if len(lines) < 3 {
		t.Fatalf("decode collapsed the text into %d line(s): %q", len(lines), lines)
	}
	if lines[0] != "first" {
		t.Errorf("first line became %q", lines[0])
	}
	if lines[2] != "third" {
		t.Errorf("the line after the bad sequence became %q, want \"third\"", lines[2])
	}
}

// The self-test writes its fixtures into a temporary directory. When that
// directory cannot be made it says so and answers 1, rather than reporting on
// fixtures it never wrote. Pointing TMPDIR at an absent path reaches this on
// any box, including one running as root.
func TestTheSelfTestRefusesWhenItCannotCreateItsFixture(t *testing.T) {
	blocked := t.TempDir()
	if err := testprivatefile.DenyDirectoryCreate(blocked); err != nil {
		t.Fatalf("denying temporary-directory creation: %v", err)
	}
	t.Cleanup(func() {
		if err := testprivatefile.RestoreDirectory(blocked); err != nil {
			t.Errorf("restoring temporary-directory ACL: %v", err)
		}
	})
	for _, key := range []string{"TMPDIR", "TMP", "TEMP"} {
		t.Setenv(key, blocked)
	}

	var out, errOut bytes.Buffer
	if code := selfTest(&out, &errOut); code != 1 {
		t.Fatalf("exit %d, want 1: %s%s", code, out.String(), errOut.String())
	}
	if !strings.Contains(errOut.String(), "cannot create fixture") {
		t.Fatalf("the refusal does not name what it could not create: %q", errOut.String())
	}
	if strings.Contains(out.String(), "all assertions held") {
		t.Fatalf("a self-test with no fixtures still claimed to hold: %q", out.String())
	}
}
