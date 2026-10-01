// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package executor

import (
	"bytes"
	"context"
	"errors"
	"fmt"
	"io"
	"os"
	"testing"
	"time"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/catalog"
)

// sleepingWriter delays the copy of a step's output past a moment the caller
// chooses. It runs inside cmd.Wait, after runCommand has already judged the
// step against the context, so the step finishes clean and the deadline is
// spent by the time the loop reaches the next one.
type sleepingWriter struct {
	until time.Time
	sink  bytes.Buffer
}

func (writer *sleepingWriter) Write(data []byte) (int, error) {
	if delay := time.Until(writer.until); delay > 0 {
		time.Sleep(delay)
	}
	return writer.sink.Write(data)
}

func TestAnAttemptEndedBetweenStepsReportsNoChildExit(t *testing.T) {
	task := fixtureTask("log")
	task.Steps = append(task.Steps, catalog.Step{Name: "never-started", Program: os.Args[0], Args: helperArgs("log")})
	deadline := time.Now().Add(2 * time.Second)
	ctx, cancel := context.WithDeadline(context.Background(), deadline)
	defer cancel()
	writer := &sleepingWriter{until: deadline.Add(300 * time.Millisecond)}
	result, err := runTaskWithStepWriters(ctx, t.TempDir(), task, func(name string) (io.Writer, io.Writer) {
		if name == task.Steps[0].Name {
			return writer, io.Discard
		}
		return io.Discard, io.Discard
	}, time.Millisecond)
	if err != nil || len(result.Steps) != 1 {
		t.Fatalf("expected one step and no error: result=%+v err=%v", result, err)
	}
	if !result.TimedOut || result.Cancelled {
		t.Fatalf("expected the attempt to report its deadline: result=%+v", result)
	}
	if result.ExitCode != noChildExit {
		t.Fatalf("attempt reported child exit %d for a step that never started: result=%+v", result.ExitCode, result)
	}
	step := result.Steps[0]
	if step.ExitCode != 0 || step.TimedOut || step.Cancelled {
		t.Fatalf("the step that ran lost its own clean verdict: %+v", step)
	}
	if writer.sink.String() != "stdout\n" {
		t.Fatalf("first step did not run to completion: %q", writer.sink.String())
	}
}

func TestADeadlineBetweenStepsIsReportedAsATimeout(t *testing.T) {
	result := endedBetweenSteps(Result{TaskName: "format-check", ExitCode: 0}, context.DeadlineExceeded)
	if !result.TimedOut || result.Cancelled || result.ExitCode != noChildExit || result.TaskName != "format-check" {
		t.Fatalf("result = %+v", result)
	}
}

func TestACancellationBetweenStepsIsNotReportedAsATimeout(t *testing.T) {
	result := endedBetweenSteps(Result{ExitCode: 0}, context.Canceled)
	if result.TimedOut || !result.Cancelled || result.ExitCode != noChildExit {
		t.Fatalf("result = %+v", result)
	}
}

func TestAWrappedCauseIsJudgedByWhatItWraps(t *testing.T) {
	for _, test := range []struct {
		name  string
		cause error
		timed bool
	}{
		{"deadline", context.DeadlineExceeded, true},
		{"wrapped deadline", fmt.Errorf("run step: %w", context.DeadlineExceeded), true},
		{"cancelled", context.Canceled, false},
		{"wrapped cancellation", fmt.Errorf("run step: %w", context.Canceled), false},
		{"any other cause", errors.New("closed"), false},
	} {
		t.Run(test.name, func(t *testing.T) {
			result := endedBetweenSteps(Result{ExitCode: 0}, test.cause)
			if result.TimedOut != test.timed || result.Cancelled == test.timed || result.ExitCode != noChildExit {
				t.Fatalf("result = %+v", result)
			}
		})
	}
}

func TestANilCauseEndsNothing(t *testing.T) {
	before := Result{TaskName: "format-check", ExitCode: 0, Steps: []StepResult{{Name: "first"}}}
	after := endedBetweenSteps(before, nil)
	if after.TimedOut || after.Cancelled || after.ExitCode != 0 {
		t.Fatalf("a nil cause manufactured an ending: %+v", after)
	}
}

func TestTheStepsAlreadyRunAreKeptWithTheirOwnVerdicts(t *testing.T) {
	before := Result{TaskName: "format-check", ExitCode: 0, Steps: []StepResult{
		{Name: "first", ExitCode: 0, StdoutBytes: 7},
		{Name: "second", ExitCode: 0, StdoutBytes: 3},
	}}
	after := endedBetweenSteps(before, context.DeadlineExceeded)
	if len(after.Steps) != 2 || after.Steps[0].ExitCode != 0 || after.Steps[1].ExitCode != 0 ||
		after.Steps[0].StdoutBytes != 7 || after.Steps[1].StdoutBytes != 3 {
		t.Fatalf("steps = %+v", after.Steps)
	}
	for _, step := range after.Steps {
		if step.TimedOut || step.Cancelled {
			t.Fatalf("step %s took the attempt's verdict: %+v", step.Name, step)
		}
	}
}
