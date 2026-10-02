// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package main

import (
	"context"
	"os"
	"path/filepath"
	"strings"
	"syscall"
	"testing"
)

const budgetUsage = "usage: ra8ci hil budget --board-id ID --manifest examples/.../hil.conf " +
	"--board-model MODEL --program-family NAME --flash-restore-bound DURATION [--safety-maximum DURATION]"

const captureUsage = "usage: ra8ci hil verify-capture --manifest examples/.../hil.conf --capture FILE"

func TestAHILLabelIsPlainAndCarriesNoSurroundingSpace(t *testing.T) {
	held := map[string]bool{
		"hal_timebase_demo":      true,
		"EK-RA8D2":               true,
		"npu.vela+int8":          true,
		"9":                      true,
		strings.Repeat("m", 128): true,
		"":                       false,
		strings.Repeat("m", 129): false,
		" EK-RA8D2":              false,
		"EK-RA8D2 ":              false,
		"EK RA8D2":               false,
		"\tEK-RA8D2":             false,
		"boards/ek-ra8d2":        false,
		"ek-ra8d2\n":             false,
		"ek-ra8d2;reboot":        false,
		"ek\u2011ra8d2":          false,
		"ek-ra8d2\x00":           false,
	}
	for value, want := range held {
		if got := validHILLabel(value); got != want {
			t.Fatalf("validHILLabel(%q)=%v; want %v", value, got, want)
		}
	}
}

func TestHILBudgetRefusesEveryInvocationItCouldNotPrice(t *testing.T) {
	t.Setenv("RA8CI_DATABASE_URL", "")
	spoiled := map[string]struct {
		args []string
		want string
	}{
		"nothing at all": {nil, budgetUsage},
		// Budget wraps its parse failure in a SHORT usage line naming the
		// offending flag, while verify-capture states its whole grammar.
		// Both are pinned as they are rather than made to agree.
		"a flag that is not one": {append(soundBudgetArguments(), "--hurry"),
			"usage: ra8ci hil budget: flag provided but not defined: -hurry"},
		"a positional left over":           {append(soundBudgetArguments(), "please"), budgetUsage},
		"no board":                         {withoutBudgetFlag(t, "--board-id"), budgetUsage},
		"no manifest":                      {withoutBudgetFlag(t, "--manifest"), budgetUsage},
		"no board model":                   {withoutBudgetFlag(t, "--board-model"), budgetUsage},
		"no program family":                {withoutBudgetFlag(t, "--program-family"), budgetUsage},
		"no restore bound":                 {withoutBudgetFlag(t, "--flash-restore-bound"), budgetUsage},
		"a board model with a space in it": {withBudgetFlag(t, "--board-model", "EK RA8D2"), budgetUsage},
		"a program family that is a path":  {withBudgetFlag(t, "--program-family", "../etc/passwd"), budgetUsage},
		"a restore bound that is not one": {withBudgetFlag(t, "--flash-restore-bound", "soon"),
			"flash-restore-bound must be greater than zero and no longer than 1h"},
		"no restore time at all": {withBudgetFlag(t, "--flash-restore-bound", "0s"),
			"flash-restore-bound must be greater than zero and no longer than 1h"},
		"restore time going backwards": {withBudgetFlag(t, "--flash-restore-bound", "-45s"),
			"flash-restore-bound must be greater than zero and no longer than 1h"},
		"a restore bound past the hour": {withBudgetFlag(t, "--flash-restore-bound", "1h1s"),
			"flash-restore-bound must be greater than zero and no longer than 1h"},
		"a safety cap that is not one": {append(soundBudgetArguments(), "--safety-maximum", "later"),
			"safety-maximum must be greater than zero and no longer than 1h"},
		"no safety time at all": {append(soundBudgetArguments(), "--safety-maximum", "0s"),
			"safety-maximum must be greater than zero and no longer than 1h"},
		"a safety cap past the hour": {append(soundBudgetArguments(), "--safety-maximum", "1h1s"),
			"safety-maximum must be greater than zero and no longer than 1h"},
	}
	for name, testCase := range spoiled {
		t.Run(name, func(t *testing.T) {
			err := hilBudgetCommand(context.Background(), testCase.args)
			if err == nil || !strings.Contains(err.Error(), testCase.want) {
				t.Fatalf("err=%v; want it to carry %q", err, testCase.want)
			}
			// Pricing a window reads a manifest and then a database. Neither
			// may be touched by an invocation this malformed.
			if strings.Contains(err.Error(), "RA8CI_DATABASE_URL") ||
				strings.Contains(err.Error(), "load HIL manifest") {
				t.Fatalf("err=%v; want the refusal taken before the manifest and the database", err)
			}
		})
	}
}

func withoutBudgetFlag(t *testing.T, flag string) []string {
	t.Helper()
	args := soundBudgetArguments()
	for index, value := range args {
		if value == flag {
			return append(append([]string{}, args[:index]...), args[index+2:]...)
		}
	}
	t.Fatalf("%s is not part of a sound budget invocation", flag)
	return nil
}

func withBudgetFlag(t *testing.T, flag, value string) []string {
	t.Helper()
	args := soundBudgetArguments()
	for index, existing := range args {
		if existing == flag {
			args[index+1] = value
			return args
		}
	}
	t.Fatalf("%s is not part of a sound budget invocation", flag)
	return nil
}

func TestHILBudgetReadsItsManifestBeforeItAsksForADatabase(t *testing.T) {
	t.Setenv("RA8CI_DATABASE_URL", "")
	// A manifest that is not there stops the command at the manifest, never at
	// the database: an operator who mistyped a path is told about the path.
	err := hilBudgetCommand(context.Background(), withBudgetFlag(t, "--manifest", "examples/nowhere/hil.conf"))
	if err == nil || !strings.Contains(err.Error(), "load HIL manifest") {
		t.Fatalf("err=%v; want the manifest refusal", err)
	}
	if strings.Contains(err.Error(), "RA8CI_DATABASE_URL") {
		t.Fatalf("err=%v; want the database left alone while the manifest is unreadable", err)
	}
	// With a manifest that does load, the missing database is what stops it,
	// and it is named rather than dialled.
	err = hilBudgetCommand(context.Background(), soundBudgetArguments())
	if err == nil || !strings.Contains(err.Error(), "RA8CI_DATABASE_URL") {
		t.Fatalf("err=%v; want the command stopped at its absent database", err)
	}
}

func TestHILBudgetWithADatabaseNamedStillRefusesAnAbsentContext(t *testing.T) {
	t.Setenv("RA8CI_DATABASE_URL", "postgres://ra8ci@127.0.0.1:1/ra8ci")
	//lint:ignore SA1012 the refusal of a nil context is the behaviour under test
	err := hilBudgetCommand(nil, soundBudgetArguments()) //nolint:staticcheck
	if err == nil || !strings.Contains(err.Error(), "HIL budget requires a context") {
		t.Fatalf("err=%v; want the absent context refused before a database is opened", err)
	}
}

func TestHILVerifyCaptureRefusesEveryInvocationBeforeItOpensAnything(t *testing.T) {
	capture := filepath.Join(t.TempDir(), "uart.log")
	if err := os.WriteFile(capture, []byte("boot\n"), 0o600); err != nil {
		t.Fatalf("plant capture: %v", err)
	}
	sound := []string{"--manifest", "examples/ek_ra8d2/hw_pending/hal_timebase_demo/hil.conf", "--capture", capture}
	usageCases := map[string][]string{
		"nothing at all":         nil,
		"a flag that is not one": append(append([]string{}, sound...), "--strict"),
		"a positional left over": append(append([]string{}, sound...), "now"),
		"no manifest":            {"--capture", capture},
		"no capture":             {"--manifest", "examples/ek_ra8d2/hw_pending/hal_timebase_demo/hil.conf"},
	}
	for name, args := range usageCases {
		t.Run(name, func(t *testing.T) {
			err := hilVerifyCaptureCommand(context.Background(), args)
			if err == nil || !strings.Contains(err.Error(), captureUsage) {
				t.Fatalf("err=%v; want the verify-capture usage", err)
			}
		})
	}
	// The context is judged after the arguments and before the capture is
	// stat'd, so a cancelled verification reads no file at all.
	cancelled, cancel := context.WithCancel(context.Background())
	cancel()
	for name, ctx := range map[string]context.Context{"a cancelled context": cancelled, "no context": nil} {
		t.Run(name, func(t *testing.T) {
			err := hilVerifyCaptureCommand(ctx, sound) //nolint:staticcheck
			if err == nil || !strings.Contains(err.Error(), "HIL capture verification requires an active context") {
				t.Fatalf("err=%v; want the context refusal", err)
			}
		})
	}
}

func TestHILVerifyCaptureRefusesACaptureThatIsNotAPlainFileUnderTheBound(t *testing.T) {
	home := t.TempDir()
	directory := filepath.Join(home, "capture.d")
	if err := os.Mkdir(directory, 0o700); err != nil {
		t.Fatalf("plant directory: %v", err)
	}
	real := filepath.Join(home, "real.log")
	if err := os.WriteFile(real, []byte("boot\n"), 0o600); err != nil {
		t.Fatalf("plant capture: %v", err)
	}
	link := filepath.Join(home, "link.log")
	if err := os.Symlink(real, link); err != nil {
		t.Fatalf("plant symlink: %v", err)
	}
	pipe := filepath.Join(home, "pipe.log")
	if err := syscall.Mkfifo(pipe, 0o600); err != nil {
		t.Fatalf("plant fifo: %v", err)
	}
	// A sparse file reaches the size bound without spending the disk: the
	// bound is read off the stat, so a hole answers as a run of zeros would.
	oversized := filepath.Join(home, "oversized.log")
	handle, err := os.OpenFile(oversized, os.O_CREATE|os.O_WRONLY, 0o600)
	if err != nil {
		t.Fatalf("plant oversized capture: %v", err)
	}
	if err := handle.Truncate(maxHILCaptureBytes + 1); err != nil {
		t.Fatalf("size oversized capture: %v", err)
	}
	handle.Close()

	refused := "capture must be a regular file no larger than 8 MiB"
	for name, path := range map[string]string{
		"a capture that is not there": filepath.Join(home, "absent.log"),
		"a directory":                 directory,
		"a symlink to a real capture": link,
		"a pipe nothing has written":  pipe,
		"a capture past 8 MiB":        oversized,
	} {
		t.Run(name, func(t *testing.T) {
			err := hilVerifyCaptureCommand(context.Background(),
				[]string{"--manifest", "examples/ek_ra8d2/hw_pending/hal_timebase_demo/hil.conf", "--capture", path})
			if err == nil || !strings.Contains(err.Error(), refused) {
				t.Fatalf("err=%v; want %q", err, refused)
			}
		})
	}

	// Exactly 8 MiB is the bound rather than past it, so the same command gets
	// as far as reading the manifest.
	atBound := filepath.Join(home, "at-bound.log")
	edge, err := os.OpenFile(atBound, os.O_CREATE|os.O_WRONLY, 0o600)
	if err != nil {
		t.Fatalf("plant bound capture: %v", err)
	}
	if err := edge.Truncate(maxHILCaptureBytes); err != nil {
		t.Fatalf("size bound capture: %v", err)
	}
	edge.Close()
	err = hilVerifyCaptureCommand(context.Background(),
		[]string{"--manifest", "examples/nowhere/hil.conf", "--capture", atBound})
	if err == nil || strings.Contains(err.Error(), refused) {
		t.Fatalf("err=%v; want the capture at the bound accepted and the manifest reached", err)
	}
	if !strings.Contains(err.Error(), "load HIL manifest") {
		t.Fatalf("err=%v; want the manifest refusal", err)
	}
}
