// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package committerms

import (
	"bytes"
	"context"
	"errors"
	"io"
	"strings"
	"testing"
)

// The gate's own invocation, the part that runs before any commit text is
// judged: what it will read, what it refuses, and what it prints when the
// text does turn out to carry a deprecated term.

type refusingReader struct{ err error }

func (r refusingReader) Read([]byte) (int, error) { return 0, r.err }

func ranOn(t *testing.T, ctx context.Context, args []string, stdin io.Reader) (int, string, string) {
	t.Helper()
	var out, errs bytes.Buffer
	code := Run(ctx, args, stdin, &out, &errs)
	return code, out.String(), errs.String()
}

// A commit message the gate cannot even read is a hard failure naming the
// read, not a clean verdict. Reporting PASS on unreadable input would wave
// through every commit in a hook whose stdin broke.
func TestAMessageThatCannotBeReadIsRefusedNotPassed(t *testing.T) {
	broken := errors.New("stdin went away mid-hook")
	code, out, errs := ranOn(t, context.Background(), nil, refusingReader{err: broken})
	if code != 2 {
		t.Fatalf("code = %d, want 2 (stdout %q)", code, out)
	}
	if !strings.Contains(errs, "read stdin") || !strings.Contains(errs, broken.Error()) {
		t.Fatalf("stderr = %q, want the read named with its cause", errs)
	}
	if strings.Contains(out, "[PASS]") {
		t.Fatalf("stdout = %q, must never pass a message it could not read", out)
	}
}

// A cancelled run is refused after the read rather than judged, and it is
// refused even when the text was clean: an interrupted hook has no verdict
// to give.
func TestACancelledRunIsRefusedEvenWhenTheTextIsClean(t *testing.T) {
	ctx, cancel := context.WithCancel(context.Background())
	cancel()
	code, out, errs := ranOn(t, ctx, nil, strings.NewReader("fix(ui): tidy the status bar\n"))
	if code != 2 {
		t.Fatalf("code = %d, want 2 (stdout %q, stderr %q)", code, out, errs)
	}
	if !strings.Contains(errs, context.Canceled.Error()) {
		t.Fatalf("stderr = %q, want the cancellation named", errs)
	}
	if out != "" {
		t.Fatalf("stdout = %q, want no verdict at all", out)
	}
}

// Every violation is listed, not just the first, and each carries its line
// number and the offending line: a hook that named only the first term
// would send an author round the loop once per line.
func TestEveryViolationIsListedWithItsLineAndText(t *testing.T) {
	message := "fix(spi): rework the MOSI pin mux\nalso swap the MISO pull-up\nand drop the slave select strap\n"
	code, out, errs := ranOn(t, context.Background(), nil, strings.NewReader(message))
	if code != 1 {
		t.Fatalf("code = %d, want 1 (stderr %q)", code, errs)
	}
	if !strings.Contains(out, "[FAIL] Non-inclusive terminology in commit message(s):") {
		t.Fatalf("stdout = %q, want the failure header", out)
	}
	for _, want := range []string{
		"line 1: MOSI -- use COPI",
		"> fix(spi): rework the MOSI pin mux",
		"line 2: MISO -- use CIPO",
		"> also swap the MISO pull-up",
		"line 3: ",
		"> and drop the slave select strap",
	} {
		if !strings.Contains(out, want) {
			t.Fatalf("stdout = %q, missing %q", out, want)
		}
	}
	if errs != "" {
		t.Fatalf("stderr = %q, a verdict belongs on stdout", errs)
	}
}

// Empty stdin is a clean pass rather than a refusal. A hook amending a
// commit can hand the gate nothing, and nothing carries no deprecated term.
func TestAnEmptyMessageIsClean(t *testing.T) {
	code, out, errs := ranOn(t, context.Background(), nil, strings.NewReader(""))
	if code != 0 {
		t.Fatalf("code = %d, want 0 (stderr %q)", code, errs)
	}
	if !strings.Contains(out, "[PASS] Commit message terminology clean.") {
		t.Fatalf("stdout = %q, want the clean verdict", out)
	}
}

// --selftest is answered only when it arrives ALONE. Paired with anything
// else the invocation is refused with the usage line, so a hook that passes
// a stray path cannot silently run the self-test instead of the check.
func TestSelfTestIsAnsweredOnlyWhenItArrivesAlone(t *testing.T) {
	for _, args := range [][]string{
		{"--selftest", "extra"},
		{"extra", "--selftest"},
		{"--selftest", "--selftest"},
		{"--SELFTEST"},
		{"selftest"},
		{""},
	} {
		code, out, errs := ranOn(t, context.Background(), args, strings.NewReader("rename MOSI pin\n"))
		if code != 2 {
			t.Fatalf("args %q: code = %d, want 2", args, code)
		}
		if !strings.Contains(errs, "usage: ra8ci inclusive-terminology-commits") {
			t.Fatalf("args %q: stderr = %q, want the usage line", args, errs)
		}
		if out != "" {
			t.Fatalf("args %q: stdout = %q, want nothing", args, out)
		}
	}
}

// The refused invocation is refused BEFORE stdin is read, so a hook that
// mis-invokes the gate does not also consume the commit message it was
// handed.
func TestARefusedInvocationNeverConsumesTheMessage(t *testing.T) {
	stdin := strings.NewReader("rename MOSI pin\n")
	if code, _, _ := ranOn(t, context.Background(), []string{"--unknown"}, stdin); code != 2 {
		t.Fatalf("code = %d, want 2", code)
	}
	rest, err := io.ReadAll(stdin)
	if err != nil {
		t.Fatalf("read back: %v", err)
	}
	if string(rest) != "rename MOSI pin\n" {
		t.Fatalf("stdin was consumed, %q left", rest)
	}
}

// The self-test announces what it proved on stdout and says nothing on
// stderr, which is what makes a green hook run readable.
func TestTheSelfTestSaysWhatItProved(t *testing.T) {
	code, out, errs := ranOn(t, context.Background(), []string{"--selftest"}, strings.NewReader("ignored"))
	if code != 0 {
		t.Fatalf("code = %d, want 0 (stderr %q)", code, errs)
	}
	if !strings.Contains(out, "[SELFTEST OK]") {
		t.Fatalf("stdout = %q, want the self-test verdict", out)
	}
	for _, want := range []string{"un-annotated term", "paragraph-scoped LEGACY-OK", "does not leak across paragraphs"} {
		if !strings.Contains(out, want) {
			t.Fatalf("stdout = %q, missing %q", out, want)
		}
	}
	if errs != "" {
		t.Fatalf("stderr = %q, want nothing", errs)
	}
}
