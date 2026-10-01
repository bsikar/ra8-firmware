// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package hilspec

import (
	"errors"
	"io/fs"
	"path/filepath"
	"strings"
	"testing"
)

const expectationManifest = "examples/ek_ra8d2/hw_validated/hil/uart_expect/hil.conf"

// captureModes are the two modes whose verdict is the text capture.
var captureModes = []Mode{ModeUARTScrape, ModeRTTScrape}

// nonCaptureModes reach their verdict by other means, so HIL_EXPECT says
// nothing under them.
var nonCaptureModes = []Mode{ModeAlive, ModeJLinkMemprobe, ModeEthernetTCP, ModeC6CameraLivestream}

// manifestExpecting builds the smallest manifest carrying a positive
// expectation, so a test varies one line and nothing else. An empty
// expectation omits the line entirely, which is how a manifest states it:
// the grammar refuses an empty value outright.
func manifestExpecting(mode Mode, expect string, shortOK bool) string {
	body := "HIL_MODE=" + string(mode) + "\n"
	if expect != "" {
		body += "HIL_EXPECT=\"" + expect + "\"\n"
	}
	if shortOK {
		body += "HIL_EXPECT_SHORT_OK=1\n"
	}
	return body
}

func parseExpecting(t *testing.T, mode Mode, expect string, shortOK bool) (Spec, error) {
	t.Helper()
	return Parse(strings.NewReader(manifestExpecting(mode, expect, shortOK)), expectationManifest)
}

func TestACaptureManifestWithNoExpectationIsRefusedBeforeTheBoard(t *testing.T) {
	for _, mode := range captureModes {
		spec, err := parseExpecting(t, mode, "", false)
		if !errors.Is(err, ErrInvalidManifest) {
			t.Fatalf("%s with no HIL_EXPECT was accepted: spec=%+v err=%v", mode, spec, err)
		}
		if !strings.Contains(err.Error(), expectationManifest) {
			t.Fatalf("%s was refused without naming the manifest to fix: %v", mode, err)
		}
		if !strings.Contains(err.Error(), "HIL_EXPECT") {
			t.Fatalf("%s was refused without naming the field to add: %v", mode, err)
		}
	}
}

func TestAShortExpectationIsRefusedBeforeTheBoard(t *testing.T) {
	for _, mode := range captureModes {
		for _, expect := range []string{"ok", "PASS", "rw=ok", "bkup: rw=ok", "verdict=PAS"} {
			spec, err := parseExpecting(t, mode, expect, false)
			if !errors.Is(err, ErrInvalidManifest) {
				t.Fatalf("%s accepted a %d-byte expectation %q: spec=%+v err=%v",
					mode, len(expect), expect, spec, err)
			}
			if !strings.Contains(err.Error(), "HIL_EXPECT_SHORT_OK") {
				t.Fatalf("%q was refused without naming the way to state it is deliberate: %v", expect, err)
			}
		}
	}
}

func TestADeliberatelyShortExpectationIsAccepted(t *testing.T) {
	for _, mode := range captureModes {
		spec, err := parseExpecting(t, mode, "rw=ok", true)
		if err != nil {
			t.Fatalf("%s refused a short expectation the manifest called deliberate: %v", mode, err)
		}
		if spec.Expect != "rw=ok" {
			t.Fatalf("expectation was not preserved: %q", spec.Expect)
		}
	}
}

// The floor is a length, not a taste: every length from one byte up to a
// comfortable margin past it is judged the same way on both sides of it.
func TestTheFloorIsJudgedByLength(t *testing.T) {
	for size := 1; size <= minimumExpectationBytes+8; size++ {
		expect := strings.Repeat("p", size)
		_, err := parseExpecting(t, ModeUARTScrape, expect, false)
		refused := errors.Is(err, ErrInvalidManifest)
		if want := size < minimumExpectationBytes; refused != want {
			t.Fatalf("a %d-byte expectation: refused=%v want=%v err=%v", size, refused, want, err)
		}
	}
}

func TestTheFloorIsMeasuredInBytes(t *testing.T) {
	// Eleven runes, more than eleven bytes. The verdict reads the capture
	// with bytes.Contains, so bytes are what the assertion costs to satisfy
	// and bytes are what the floor counts.
	expect := strings.Repeat("\u00e9", 11)
	if len(expect) < minimumExpectationBytes {
		t.Fatalf("fixture is not over the floor in bytes: %d", len(expect))
	}
	if _, err := parseExpecting(t, ModeUARTScrape, expect, false); err != nil {
		t.Fatalf("an expectation over the floor in bytes was refused: %v", err)
	}
}

func TestANonCaptureModeIsNotHeldToAnExpectation(t *testing.T) {
	for _, mode := range nonCaptureModes {
		if _, err := parseExpecting(t, mode, "", false); err != nil {
			t.Fatalf("%s was held to an expectation nothing reads: %v", mode, err)
		}
		if _, err := parseExpecting(t, mode, "ok", false); err != nil {
			t.Fatalf("%s was held to the expectation floor: %v", mode, err)
		}
	}
}

// The two doors have to agree: whatever Parse accepts, VerifyTextCapture can
// reach a verdict on, and it never refuses it for the reasons Parse now owns.
func TestWhatParseAcceptsReachesAVerdict(t *testing.T) {
	spec, err := parseExpecting(t, ModeUARTScrape, "verdict=PASS ready", false)
	if err != nil {
		t.Fatalf("parse: %v", err)
	}
	if err := VerifyTextCapture(spec, []byte("boot\nverdict=PASS ready\n")); err != nil {
		t.Fatalf("an accepted manifest did not reach a clean verdict: %v", err)
	}
	if err := VerifyTextCapture(spec, []byte("boot\nnothing here\n")); !errors.Is(err, ErrExpectationNotFound) {
		t.Fatalf("a missing banner should be the ordinary failure, got %v", err)
	}
	short, err := parseExpecting(t, ModeUARTScrape, "rw=ok", true)
	if err != nil {
		t.Fatalf("parse short: %v", err)
	}
	if err := VerifyTextCapture(short, []byte("bkup: rw=ok survived=Y\n")); err != nil {
		t.Fatalf("a deliberately short expectation did not reach a clean verdict: %v", err)
	}
}

// Parse closing these doors early does not open them at the verdict. A Spec
// can be built by a caller that never parsed a manifest, so VerifyTextCapture
// keeps both of its own checks.
func TestTheVerdictKeepsItsOwnChecks(t *testing.T) {
	missing := Spec{Path: expectationManifest, Mode: ModeUARTScrape, Values: map[string]Value{}}
	if err := VerifyTextCapture(missing, []byte("anything")); !errors.Is(err, ErrMissingExpectation) {
		t.Fatalf("the verdict stopped refusing a hand-built spec with no expectation: %v", err)
	}
	weak := Spec{Path: expectationManifest, Mode: ModeRTTScrape, Expect: "ok", Values: map[string]Value{}}
	if err := VerifyTextCapture(weak, []byte("ok")); !errors.Is(err, ErrWeakExpectation) {
		t.Fatalf("the verdict stopped refusing a hand-built weak expectation: %v", err)
	}
	allowed := Spec{Path: expectationManifest, Mode: ModeRTTScrape, Expect: "ok",
		Values: map[string]Value{"HIL_EXPECT_SHORT_OK": {Kind: FlagValue, Flag: true}}}
	if err := VerifyTextCapture(allowed, []byte("ok")); err != nil {
		t.Fatalf("the verdict refused a deliberately short expectation: %v", err)
	}
}

// The floor the verdict applies and the floor Parse applies are one number.
func TestBothDoorsShareTheFloor(t *testing.T) {
	under := strings.Repeat("x", minimumExpectationBytes-1)
	if _, err := parseExpecting(t, ModeUARTScrape, under, false); !errors.Is(err, ErrInvalidManifest) {
		t.Fatalf("parse accepted one byte under the floor: %v", err)
	}
	spec := Spec{Path: expectationManifest, Mode: ModeUARTScrape, Expect: under, Values: map[string]Value{}}
	if err := VerifyTextCapture(spec, []byte(under)); !errors.Is(err, ErrWeakExpectation) {
		t.Fatalf("the verdict accepted one byte under the floor: %v", err)
	}
	at := strings.Repeat("x", minimumExpectationBytes)
	if _, err := parseExpecting(t, ModeUARTScrape, at, false); err != nil {
		t.Fatalf("parse refused the floor itself: %v", err)
	}
	spec.Expect = at
	if err := VerifyTextCapture(spec, []byte(at)); err != nil {
		t.Fatalf("the verdict refused the floor itself: %v", err)
	}
}

// Every text-capture manifest in the tree satisfies the rule, so the refusal
// is about manifests nobody has written rather than the ones that exist. It
// walks with Load, so a manifest that no longer parses fails here too.
func TestTheRepositoryManifestsSatisfyTheRule(t *testing.T) {
	root := repositoryRoot(t)
	checked := 0
	err := filepath.WalkDir(filepath.Join(root, "examples"), func(file string, entry fs.DirEntry, walkErr error) error {
		if walkErr != nil {
			return walkErr
		}
		if entry.IsDir() || entry.Name() != "hil.conf" {
			return nil
		}
		relative, err := filepath.Rel(root, file)
		if err != nil {
			return err
		}
		spec, err := Load(root, relative)
		if err != nil {
			t.Errorf("%s: %v", relative, err)
			return nil
		}
		if spec.Mode != ModeUARTScrape && spec.Mode != ModeRTTScrape {
			return nil
		}
		if err := checkPositiveExpectationIsUsable(spec); err != nil {
			t.Errorf("%s: %v", relative, err)
		}
		checked++
		return nil
	})
	if err != nil {
		t.Fatal(err)
	}
	if checked == 0 {
		t.Fatal("no UART/RTT capture manifests were checked")
	}
}
