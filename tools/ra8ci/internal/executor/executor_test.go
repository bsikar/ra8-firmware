// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package executor

import (
	"bytes"
	"context"
	"crypto/sha256"
	"encoding/hex"
	"errors"
	"fmt"
	"io"
	"os"
	"os/exec"
	"path/filepath"
	"reflect"
	"runtime"
	"strconv"
	"strings"
	"testing"
	"time"

	embedded "github.com/bsikar/ra8-firmware/tools/ra8ci/catalog"
	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/catalog"
)

func TestMain(m *testing.M) {
	coverDir, err := os.MkdirTemp("", "ra8ci-executor-cover-")
	if err != nil {
		fmt.Fprintln(os.Stderr, err)
		os.Exit(2)
	}
	if err := os.Setenv("GOCOVERDIR", coverDir); err != nil {
		fmt.Fprintln(os.Stderr, err)
		os.Exit(2)
	}
	code := m.Run()
	_ = os.RemoveAll(coverDir)
	os.Exit(code)
}

type deadlineOnlyContext struct {
	context.Context
	deadline time.Time
}

func (ctx deadlineOnlyContext) Deadline() (time.Time, bool) { return ctx.deadline, true }
func (deadlineOnlyContext) Err() error                      { return nil }
func (deadlineOnlyContext) Done() <-chan struct{}           { return nil }

func TestContextExpirationUsesAbsoluteDeadline(t *testing.T) {
	ctx := deadlineOnlyContext{Context: context.Background(), deadline: time.Now().Add(-time.Second)}
	if err := contextExpiration(ctx); !errors.Is(err, context.DeadlineExceeded) {
		t.Fatalf("contextExpiration() = %v, want DeadlineExceeded", err)
	}
}

func TestRunTaskStreamsAndRecordsStep(t *testing.T) {
	task := fixtureTask("log")
	var stdout, stderr bytes.Buffer
	result, err := runTask(context.Background(), t.TempDir(), task, &stdout, &stderr, time.Millisecond)
	if err != nil {
		t.Fatal(err)
	}
	if result.ExitCode != 0 || result.TimedOut || result.Cancelled || len(result.Steps) != 1 {
		t.Fatalf("result = %+v", result)
	}
	if stdout.String() != "stdout\n" || stderr.String() != "stderr\n" {
		t.Fatalf("logs stdout=%q stderr=%q", stdout.String(), stderr.String())
	}
	step := result.Steps[0]
	stdoutSum := sha256.Sum256(stdout.Bytes())
	stderrSum := sha256.Sum256(stderr.Bytes())
	if step.StdoutSHA256 != hex.EncodeToString(stdoutSum[:]) || step.StderrSHA256 != hex.EncodeToString(stderrSum[:]) ||
		step.StdoutBytes != int64(stdout.Len()) || step.StderrBytes != int64(stderr.Len()) {
		t.Fatalf("log evidence = %+v", step)
	}
	if result.StartedAt.IsZero() || result.EndedAt.Before(result.StartedAt) || result.Duration <= 0 ||
		step.StartedAt.IsZero() || step.EndedAt.Before(step.StartedAt) || step.Duration <= 0 {
		t.Fatalf("invalid timing: task=%+v step=%+v", result, step)
	}
}

func TestRunStepNativeDriverAsmGuardSelftest(t *testing.T) {
	var stdout, stderr bytes.Buffer
	result, err := runStep(context.Background(), t.TempDir(), nil,
		catalog.Step{Name: "driver-asm-guard-selftest", Program: "ra8ci:driver-asm-guard", Args: []string{"--selftest"}},
		&stdout, &stderr, time.Millisecond)
	if err != nil || result.ExitCode != 0 || result.TimedOut || result.Cancelled || stderr.Len() != 0 ||
		!strings.Contains(stdout.String(), "all cases pass") {
		t.Fatalf("result=%+v stdout=%q stderr=%q err=%v", result, stdout.String(), stderr.String(), err)
	}
}

func TestRunStepNativeNoGotoSetjmpSelftest(t *testing.T) {
	var stdout, stderr bytes.Buffer
	result, err := runStep(context.Background(), t.TempDir(), nil,
		catalog.Step{Name: "no-goto-setjmp-selftest", Program: "ra8ci:no-goto-setjmp", Args: []string{"--selftest"}},
		&stdout, &stderr, time.Millisecond)
	if err != nil || result.ExitCode != 0 || result.TimedOut || result.Cancelled || stderr.Len() != 0 ||
		!strings.Contains(stdout.String(), "all cases pass") {
		t.Fatalf("result=%+v stdout=%q stderr=%q err=%v", result, stdout.String(), stderr.String(), err)
	}
}

func TestRunStepNativeRunnerClockSelftest(t *testing.T) {
	var stdout, stderr bytes.Buffer
	result, err := runStep(context.Background(), t.TempDir(), nil,
		catalog.Step{Name: "runner-clock-selftest", Program: "ra8ci:runner-clock", Args: []string{"--selftest"}},
		&stdout, &stderr, time.Millisecond)
	if err != nil || result.ExitCode != 0 || result.TimedOut || result.Cancelled {
		t.Fatalf("runner-clock selftest result=%+v err=%v stderr=%q", result, err, stderr.String())
	}
	if !strings.Contains(stdout.String(), "7/7 cases as documented") || result.StdoutBytes != int64(stdout.Len()) ||
		result.StderrBytes != int64(stderr.Len()) || result.StdoutSHA256 == "" || result.StderrSHA256 == "" {
		t.Fatalf("runner-clock output/evidence mismatch: result=%+v stdout=%q stderr=%q", result, stdout.String(), stderr.String())
	}
}

func TestRunTaskAttributesStreamsToEachStep(t *testing.T) {
	task := fixtureTask("log")
	task.Steps = append(task.Steps, catalog.Step{Name: "second-step", Program: os.Args[0], Args: helperArgs("log")})
	outputs := map[string][2]*bytes.Buffer{}
	result, err := runTaskWithStepWriters(context.Background(), t.TempDir(), task, func(name string) (io.Writer, io.Writer) {
		streams := [2]*bytes.Buffer{&bytes.Buffer{}, &bytes.Buffer{}}
		outputs[name] = streams
		return streams[0], streams[1]
	}, time.Millisecond)
	if err != nil || result.ExitCode != 0 || len(result.Steps) != 2 {
		t.Fatalf("multi-step result=%+v err=%v", result, err)
	}
	for _, step := range result.Steps {
		streams := outputs[step.Name]
		if streams[0].String() != "stdout\n" || streams[1].String() != "stderr\n" ||
			step.StdoutBytes != int64(streams[0].Len()) || step.StderrBytes != int64(streams[1].Len()) {
			t.Fatalf("step %q output was not attributed: %+v", step.Name, step)
		}
	}
}

func TestRunTaskReturnsExactChildExit(t *testing.T) {
	task := fixtureTask("exit17")
	var stdout, stderr bytes.Buffer
	result, err := runTask(context.Background(), t.TempDir(), task, &stdout, &stderr, time.Millisecond)
	if err != nil || result.ExitCode != 17 || result.Steps[0].ExitCode != 17 {
		t.Fatalf("result = %+v, error = %v", result, err)
	}
}

func TestRunTaskStopsAfterFailedStep(t *testing.T) {
	task := fixtureTask("exit17")
	task.Steps = append(task.Steps, catalog.Step{Name: "must-not-run", Program: os.Args[0], Args: helperArgs("log")})
	result, err := runTask(context.Background(), t.TempDir(), task, io.Discard, io.Discard, time.Millisecond)
	if err != nil || result.ExitCode != 17 || len(result.Steps) != 1 {
		t.Fatalf("result = %+v, error = %v", result, err)
	}
}

func TestRunTaskDeadlineAndCancellation(t *testing.T) {
	for _, test := range []struct {
		name    string
		context func() (context.Context, context.CancelFunc)
		timed   bool
	}{
		{"deadline", func() (context.Context, context.CancelFunc) {
			return context.WithTimeout(context.Background(), 40*time.Millisecond)
		}, true},
		{"cancel", func() (context.Context, context.CancelFunc) {
			ctx, cancel := context.WithCancel(context.Background())
			go func() { time.Sleep(40 * time.Millisecond); cancel() }()
			return ctx, cancel
		}, false},
	} {
		t.Run(test.name, func(t *testing.T) {
			ctx, cancel := test.context()
			defer cancel()
			start := time.Now()
			result, err := runTask(ctx, t.TempDir(), fixtureTask("sleep"), io.Discard, io.Discard, 20*time.Millisecond)
			if err != nil || result.TimedOut != test.timed || result.Cancelled == test.timed || time.Since(start) > 2*time.Second {
				t.Fatalf("result = %+v, error = %v, elapsed = %s", result, err, time.Since(start))
			}
		})
	}
}

func TestRunTaskMissingToolIsFailure(t *testing.T) {
	task := fixtureTask("log")
	task.Steps[0].Program = "ra8ci-this-tool-does-not-exist"
	result, err := runTask(context.Background(), t.TempDir(), task, io.Discard, io.Discard, 0)
	if !errors.Is(err, ErrToolMissing) || len(result.Steps) != 1 || result.Steps[0].ExitCode != -1 || result.Steps[0].EndedAt.IsZero() {
		t.Fatalf("result = %+v, error = %v", result, err)
	}
}

func TestRunTaskScrubsSecretEnvironment(t *testing.T) {
	t.Setenv("RA8CI_SECRET_TEST", "must-not-leak")
	var stdout bytes.Buffer
	result, err := runTask(context.Background(), t.TempDir(), fixtureTask("environment"), &stdout, io.Discard, 0)
	if err != nil || result.ExitCode != 0 || strings.Contains(stdout.String(), "must-not-leak") || stdout.String() != "\n" {
		t.Fatalf("secret reached task: result=%+v output=%q error=%v", result, stdout.String(), err)
	}
}

func TestRunTaskRejectsUnsafePath(t *testing.T) {
	t.Setenv("PATH", "."+string(os.PathListSeparator)+os.Getenv("PATH"))
	_, err := runTask(context.Background(), t.TempDir(), fixtureTask("log"), io.Discard, io.Discard, 0)
	if !errors.Is(err, ErrUnsafeEnvironment) {
		t.Fatalf("error = %v", err)
	}
}

func TestRunTaskRejectsInTreeCache(t *testing.T) {
	root := t.TempDir()
	t.Setenv("GOCACHE", filepath.Join(root, ".cache"))
	_, err := runTask(context.Background(), root, fixtureTask("log"), io.Discard, io.Discard, 0)
	if !errors.Is(err, ErrUnsafeEnvironment) {
		t.Fatalf("error = %v", err)
	}
}

func TestRunTaskRejectsInTreePathEntry(t *testing.T) {
	root := t.TempDir()
	t.Setenv("PATH", filepath.Join(root, "bin")+string(os.PathListSeparator)+os.Getenv("PATH"))
	_, err := runTask(context.Background(), root, fixtureTask("log"), io.Discard, io.Discard, 0)
	if !errors.Is(err, ErrUnsafeEnvironment) {
		t.Fatalf("error = %v", err)
	}
}

func TestRunTaskPinsLocalGoToolchain(t *testing.T) {
	t.Setenv("GOTOOLCHAIN", "auto")
	var stdout bytes.Buffer
	result, err := runTask(context.Background(), t.TempDir(), fixtureTask("toolchain"), &stdout, io.Discard, 0)
	if err != nil || result.ExitCode != 0 || stdout.String() != "local\n" {
		t.Fatalf("result = %+v output = %q error = %v", result, stdout.String(), err)
	}
}

func TestRunRejectsUnreviewedTask(t *testing.T) {
	task := fixtureTask("log")
	_, err := Run(context.Background(), t.TempDir(), task, io.Discard, io.Discard)
	if !errors.Is(err, catalog.ErrInvalidCheckout) {
		t.Fatalf("invalid checkout error = %v", err)
	}
}

func TestRunExecutesOnlyReviewedTaskFromVerifiedCheckout(t *testing.T) {
	root := t.TempDir()
	if err := os.Mkdir(filepath.Join(root, ".git"), 0700); err != nil {
		t.Fatal(err)
	}
	manifestDir := filepath.Join(root, "tools", "ra8ci", "catalog")
	if err := os.MkdirAll(manifestDir, 0700); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(manifestDir, "tasks.json"), embedded.Manifest(), 0600); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(manifestDir, "sha256.txt"), embedded.Digest(), 0600); err != nil {
		t.Fatal(err)
	}
	scriptDir := filepath.Join(root, "scripts", "checks")
	if err := os.MkdirAll(scriptDir, 0700); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(scriptDir, "format_tree.sh"), []byte("#!/bin/sh\n[ \"$1\" = --check ] || exit 31\nprintf 'fixture-pass\\n'\n"), 0700); err != nil {
		t.Fatal(err)
	}
	c, err := catalog.Load()
	if err != nil {
		t.Fatal(err)
	}
	task, found := c.Task("format-check")
	if !found {
		t.Fatal("format-check absent")
	}
	var stdout bytes.Buffer
	result, err := Run(context.Background(), root, task, &stdout, io.Discard)
	if err != nil || result.ExitCode != 0 || stdout.String() != "fixture-pass\n" {
		t.Fatalf("result = %+v, output = %q, error = %v", result, stdout.String(), err)
	}
	task.Steps[0].Args = []string{"scripts/checks/format_tree.sh"}
	_, err = Run(context.Background(), root, task, io.Discard, io.Discard)
	if !errors.Is(err, ErrUnreviewedTask) {
		t.Fatalf("tampered task error = %v", err)
	}
}

func TestRunTaskRejectsUnsupportedOSAndInvalidInputs(t *testing.T) {
	task := fixtureTask("log")
	otherOS := "windows"
	if runtime.GOOS == "windows" {
		otherOS = "linux"
	}
	task.OS = []string{otherOS}
	_, err := runTask(context.Background(), t.TempDir(), task, io.Discard, io.Discard, 0)
	if !errors.Is(err, ErrUnsupportedOS) {
		t.Fatalf("unsupported OS error = %v", err)
	}
	task.OS = []string{runtime.GOOS}
	_, err = runTask(nil, t.TempDir(), task, io.Discard, io.Discard, 0)
	if err == nil {
		t.Fatal("nil context accepted")
	}
	_, err = runTask(context.Background(), t.TempDir(), task, nil, io.Discard, 0)
	if err == nil {
		t.Fatal("nil writer accepted")
	}
	_, err = runTask(context.Background(), t.TempDir(), task, io.Discard, io.Discard, -1)
	if err == nil {
		t.Fatal("negative grace accepted")
	}
}

func TestDigestWriterPropagatesFailure(t *testing.T) {
	w := newDigestWriter(failingWriter{})
	n, err := w.Write([]byte("abc"))
	if n != 0 || err == nil {
		t.Fatalf("Write = %d, %v", n, err)
	}
	digest, count := w.digest()
	empty := sha256.Sum256(nil)
	if count != 0 || digest != hex.EncodeToString(empty[:]) {
		t.Fatalf("digest = %q count = %d", digest, count)
	}
}

func TestRunTaskPropagatesLogSinkFailure(t *testing.T) {
	result, err := runTask(context.Background(), t.TempDir(), fixtureTask("log"), failingWriter{}, io.Discard, 0)
	if err == nil || len(result.Steps) != 1 || result.Steps[0].StdoutBytes != 0 {
		t.Fatalf("result = %+v, error = %v", result, err)
	}
}

func TestRunTaskPreservesChildExitWithBrokenLogSink(t *testing.T) {
	result, err := runTask(context.Background(), t.TempDir(), fixtureTask("exit17log"), failingWriter{}, io.Discard, 0)
	if err == nil || result.ExitCode != 17 || len(result.Steps) != 1 || result.Steps[0].ExitCode != 17 {
		t.Fatalf("result = %+v, error = %v", result, err)
	}
}

func TestWindowsEnvironmentKeysAreCaseInsensitive(t *testing.T) {
	if normalizeEnvironmentKey("Path", "windows") != "PATH" || normalizeEnvironmentKey("SystemRoot", "windows") != "SYSTEMROOT" {
		t.Fatal("mixed-case Windows keys were not normalized")
	}
	if normalizeEnvironmentKey("Path", "linux") != "Path" {
		t.Fatal("Linux key was incorrectly normalized")
	}
}

type failingWriter struct{}

func (failingWriter) Write([]byte) (int, error) {
	return 0, errors.New("sink failed")
}

func fixtureTask(mode string) catalog.Task {
	return catalog.Task{
		Name: "fixture", Version: 1, Tier: "required", Scope: "safe-local-read-only",
		OS: []string{runtime.GOOS}, DeadlineSeconds: 3, BoardPolicy: "none",
		Retry: catalog.RetryPolicy{MaxAttempts: 1},
		Steps: []catalog.Step{{Name: "fixture-step", Program: os.Args[0], Args: helperArgs(mode)}},
	}
}

func helperArgs(mode string) []string {
	return []string{"-test.run=^TestHelperProcess$", "--", mode}
}

// TestHelperProcess is invoked only as a child of an executor test.
func TestHelperProcess(t *testing.T) {
	index := -1
	for i, arg := range os.Args {
		if arg == "--" {
			index = i
			break
		}
	}
	if index < 0 || index+1 >= len(os.Args) {
		return
	}
	switch os.Args[index+1] {
	case "log":
		fmt.Fprintln(os.Stdout, "stdout")
		fmt.Fprintln(os.Stderr, "stderr")
	case "exit17":
		os.Exit(17)
	case "exit17log":
		fmt.Fprintln(os.Stdout, "before-failure")
		os.Exit(17)
	case "sleep":
		time.Sleep(10 * time.Second)
	case "environment":
		fmt.Fprintln(os.Stdout, os.Getenv("RA8CI_SECRET_TEST"))
	case "toolchain":
		fmt.Fprintln(os.Stdout, os.Getenv("GOTOOLCHAIN"))
	case "spawn":
		if index+2 >= len(os.Args) {
			os.Exit(20)
		}
		child := exec.Command(os.Args[0], append(helperArgs("sleep"), strconv.Itoa(os.Getpid()))...)
		if err := child.Start(); err != nil {
			os.Exit(21)
		}
		if err := os.WriteFile(os.Args[index+2], []byte(strconv.Itoa(child.Process.Pid)), 0600); err != nil {
			os.Exit(22)
		}
		_ = child.Wait()
	case "spawn-detached":
		if index+2 >= len(os.Args) {
			os.Exit(20)
		}
		child := exec.Command(os.Args[0], helperArgs("sleep")...)
		if err := child.Start(); err != nil {
			os.Exit(21)
		}
		if err := os.WriteFile(os.Args[index+2], []byte(strconv.Itoa(child.Process.Pid)), 0600); err != nil {
			os.Exit(22)
		}
	default:
		os.Exit(23)
	}
	os.Exit(0)
}

func TestCatalogTaskCopyIsComparableByValue(t *testing.T) {
	c, err := catalog.Load()
	if err != nil {
		t.Fatal(err)
	}
	a, _ := c.Task("format")
	b, _ := c.Task("format")
	if !reflect.DeepEqual(a, b) {
		t.Fatal("catalog copies differ")
	}
}

func TestResolveTaskProgramIsAnchoredToVerifiedCheckout(t *testing.T) {
	root := t.TempDir()
	programPath := filepath.Join(root, "scripts", "probe")
	if err := os.MkdirAll(filepath.Dir(programPath), 0755); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(programPath, []byte("probe"), 0755); err != nil {
		t.Fatal(err)
	}
	got, err := resolveTaskProgram(root, "scripts/probe")
	if err != nil {
		t.Fatal(err)
	}
	want, err := filepath.Abs(programPath)
	if err != nil {
		t.Fatal(err)
	}
	if got != want {
		t.Fatalf("resolved program=%q, want checkout path %q", got, want)
	}
	if _, err := resolveTaskProgram(root, "../outside"); !errors.Is(err, ErrUnsafeEnvironment) {
		t.Fatalf("checkout escape error=%v", err)
	}
}

func TestCleanEnvironmentKeepsOnlyApprovedExternalToolchainOverrides(t *testing.T) {
	root := t.TempDir()
	toolchain := t.TempDir()
	t.Setenv("RA8_TOOL_BIN", filepath.Join(toolchain, "bin"))
	t.Setenv("RA8_TOOL_VENV", toolchain)
	env, err := cleanEnvironment(root)
	if err != nil {
		t.Fatal(err)
	}
	joined := strings.Join(env, "\n")
	if !strings.Contains(joined, "RA8_TOOL_BIN="+filepath.Join(toolchain, "bin")) || !strings.Contains(joined, "RA8_TOOL_VENV="+toolchain) {
		t.Fatalf("validated toolchain overrides were not preserved: %q", joined)
	}
	t.Setenv("RA8_TOOL_VENV", filepath.Join(root, "venv"))
	if _, err := cleanEnvironment(root); !errors.Is(err, ErrUnsafeEnvironment) {
		t.Fatalf("checkout-local managed environment override error=%v", err)
	}
}

func TestRunStepNativeTestsReadmeSelftest(t *testing.T) {
	var stdout, stderr bytes.Buffer
	result, err := runStep(context.Background(), t.TempDir(), nil,
		catalog.Step{Name: "tests-readme-selftest", Program: "ra8ci:tests-readme", Args: []string{"--selftest"}},
		&stdout, &stderr, time.Millisecond)
	if err != nil || result.ExitCode != 0 || result.TimedOut || result.Cancelled {
		t.Fatalf("tests-readme selftest result=%+v err=%v stderr=%q", result, err, stderr.String())
	}
	if !strings.Contains(stdout.String(), "4 cases plus the gitignore carve-out") || result.StdoutBytes != int64(stdout.Len()) ||
		result.StderrBytes != int64(stderr.Len()) || result.StdoutSHA256 == "" || result.StderrSHA256 == "" {
		t.Fatalf("tests-readme output/evidence mismatch: result=%+v stdout=%q stderr=%q", result, stdout.String(), stderr.String())
	}

}

func TestRunStepNativeCommitTerminologySelftest(t *testing.T) {
	var stdout, stderr bytes.Buffer
	result, err := runStep(context.Background(), t.TempDir(), nil,
		catalog.Step{Name: "inclusive-terminology-commits-selftest", Program: "ra8ci:inclusive-terminology-commits", Args: []string{"--selftest"}},
		&stdout, &stderr, time.Millisecond)
	if err != nil || result.ExitCode != 0 || result.TimedOut || result.Cancelled {
		t.Fatalf("commit terminology selftest result=%+v err=%v stderr=%q", result, err, stderr.String())
	}
	if !strings.Contains(stdout.String(), "paragraph-scoped LEGACY-OK") || result.StdoutBytes != int64(stdout.Len()) || result.StdoutSHA256 == "" {
		t.Fatalf("commit terminology output/evidence mismatch: result=%+v stdout=%q", result, stdout.String())
	}
}
