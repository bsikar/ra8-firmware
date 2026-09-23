// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

// Package executor runs reviewed local task steps with bounded lifetimes.
package executor

import (
	"context"
	"crypto/sha256"
	"encoding/hex"
	"errors"
	"fmt"
	"hash"
	"io"
	"os"
	"os/exec"
	"path/filepath"
	"reflect"
	"runtime"
	"strings"
	"time"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/asciigate"
	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/catalog"
	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/committerms"
	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/driverasmguard"
	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/gotosetjmp"
	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/legacymake"
	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/newlinegate"
	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/nscveneers"
	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/pointerboilerplate"
	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/runnerclock"
	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/sincegate"
	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/stubcryptoguard"
	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/testsreadme"
	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/tzdiscard"
	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/unsafeinstall"
	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/waverefs"
)

const stopGrace = 30 * time.Second

var (
	ErrUnsupportedOS     = errors.New("task does not support this operating system")
	ErrUnreviewedTask    = errors.New("task differs from the reviewed catalog")
	ErrToolMissing       = errors.New("required task tool is missing")
	ErrUnsafeEnvironment = errors.New("unsafe task environment")
)

// contextExpiration reports cancellation even when the runtime has not yet
// delivered the timer callback that makes ctx.Err() observable.
func contextExpiration(ctx context.Context) error {
	if err := ctx.Err(); err != nil {
		return err
	}
	if deadline, ok := ctx.Deadline(); ok && !time.Now().Before(deadline) {
		return context.DeadlineExceeded
	}
	return nil
}

// StepResult captures the actual outcome and timing of one command step.
type StepResult struct {
	Name         string        `json:"name"`
	StartedAt    time.Time     `json:"started_at"`
	EndedAt      time.Time     `json:"ended_at"`
	Duration     time.Duration `json:"duration_ns"`
	ExitCode     int           `json:"exit_code"`
	TimedOut     bool          `json:"timed_out"`
	Cancelled    bool          `json:"cancelled"`
	StdoutSHA256 string        `json:"stdout_sha256"`
	StderrSHA256 string        `json:"stderr_sha256"`
	StdoutBytes  int64         `json:"stdout_bytes"`
	StderrBytes  int64         `json:"stderr_bytes"`
}

// Result is a task attempt, with exact child exit distinct from ra8ci errors.
type Result struct {
	TaskName  string        `json:"task_name"`
	StartedAt time.Time     `json:"started_at"`
	EndedAt   time.Time     `json:"ended_at"`
	Duration  time.Duration `json:"duration_ns"`
	ExitCode  int           `json:"exit_code"`
	TimedOut  bool          `json:"timed_out"`
	Cancelled bool          `json:"cancelled"`
	Steps     []StepResult  `json:"steps"`
}

type commandResult struct {
	ExitCode  int
	TimedOut  bool
	Cancelled bool
}

// Run executes only the exact task embedded in this binary and repository.
// A nonzero child exit is returned in Result, not as a Go error.
func Run(ctx context.Context, root string, task catalog.Task, stdout, stderr io.Writer, stepWriters ...func(string) (io.Writer, io.Writer)) (Result, error) {
	verifiedRoot, err := catalog.VerifyCheckout(root)
	if err != nil {
		return Result{}, err
	}
	definitions, err := catalog.Load()
	if err != nil {
		return Result{}, err
	}
	reviewed, found := definitions.Task(task.Name)
	if !found || !reflect.DeepEqual(reviewed, task) {
		return Result{}, ErrUnreviewedTask
	}
	if !task.IsSafeLocal() {
		return Result{}, ErrUnreviewedTask
	}
	if len(stepWriters) > 1 {
		return Result{}, fmt.Errorf("at most one step-writer selector is allowed")
	}
	writers := func(string) (io.Writer, io.Writer) { return stdout, stderr }
	if len(stepWriters) == 1 {
		if stepWriters[0] == nil {
			return Result{}, fmt.Errorf("nil step-writer selector")
		}
		writers = stepWriters[0]
	}
	return runTaskWithStepWriters(ctx, verifiedRoot, task, writers, stopGrace)
}

func runTask(ctx context.Context, root string, task catalog.Task, stdout, stderr io.Writer, grace time.Duration) (result Result, runErr error) {
	return runTaskWithStepWriters(ctx, root, task, func(string) (io.Writer, io.Writer) {
		return stdout, stderr
	}, grace)
}

func runTaskWithStepWriters(ctx context.Context, root string, task catalog.Task,
	writers func(stepName string) (io.Writer, io.Writer), grace time.Duration) (result Result, runErr error) {
	result = Result{TaskName: task.Name, ExitCode: -1}
	if ctx == nil || writers == nil {
		return result, fmt.Errorf("invalid executor input: nil context or log writer")
	}
	if err := catalog.ValidateTask(task); err != nil {
		return result, err
	}
	if !task.SupportsOS(runtime.GOOS) {
		return result, ErrUnsupportedOS
	}
	if grace < 0 {
		return result, fmt.Errorf("invalid negative stop grace")
	}
	env, err := cleanEnvironment(root)
	if err != nil {
		return result, err
	}
	runCtx, cancel := context.WithTimeout(ctx, time.Duration(task.DeadlineSeconds)*time.Second)
	defer cancel()
	started := time.Now()
	result.StartedAt = started.UTC()
	defer func() {
		end := time.Now()
		result.EndedAt = end.UTC()
		result.Duration = end.Sub(started)
	}()
	for _, step := range task.Steps {
		if err := contextExpiration(runCtx); err != nil {
			result.TimedOut = errors.Is(err, context.DeadlineExceeded)
			result.Cancelled = !result.TimedOut
			return result, nil
		}
		stepStdout, stepStderr := writers(step.Name)
		if stepStdout == nil || stepStderr == nil {
			return result, fmt.Errorf("step %s has nil log writer", step.Name)
		}
		stepResult, stepErr := runStep(runCtx, root, env, step, stepStdout, stepStderr, grace)
		result.Steps = append(result.Steps, stepResult)
		result.ExitCode = stepResult.ExitCode
		result.TimedOut = stepResult.TimedOut
		result.Cancelled = stepResult.Cancelled
		if stepErr != nil || stepResult.ExitCode != 0 || result.TimedOut || result.Cancelled {
			return result, stepErr
		}
	}
	return result, nil
}

func runStep(ctx context.Context, root string, env []string, step catalog.Step, stdout, stderr io.Writer, grace time.Duration) (result StepResult, runErr error) {
	started := time.Now()
	result = StepResult{Name: step.Name, StartedAt: started.UTC(), ExitCode: -1}
	defer func() {
		end := time.Now()
		result.EndedAt = end.UTC()
		result.Duration = end.Sub(started)
	}()
	if step.Program == "ra8ci:ascii" || step.Program == "ra8ci:since" || step.Program == "ra8ci:final-newline" || step.Program == "ra8ci:runner-clock" || step.Program == "ra8ci:tests-readme" || step.Program == "ra8ci:inclusive-terminology-commits" || step.Program == "ra8ci:legacy-make" || step.Program == "ra8ci:no-unsafe-python-install" || step.Program == "ra8ci:wave-references" || step.Program == "ra8ci:pointer-boilerplate" || step.Program == "ra8ci:nsc-veneer-defs" || step.Program == "ra8ci:stub-crypto-guard" || step.Program == "ra8ci:tz-boundary-discard" || step.Program == "ra8ci:driver-asm-guard" || step.Program == "ra8ci:no-goto-setjmp" {
		stdoutLog := newDigestWriter(stdout)
		stderrLog := newDigestWriter(stderr)
		if step.Program == "ra8ci:ascii" {
			result.ExitCode = asciigate.Run(ctx, root, step.Args, stdoutLog, stderrLog)
		} else if step.Program == "ra8ci:since" {
			result.ExitCode = sincegate.Run(ctx, root, step.Args, stdoutLog, stderrLog)
		} else if step.Program == "ra8ci:final-newline" {
			result.ExitCode = newlinegate.Run(ctx, root, step.Args, stdoutLog, stderrLog)
		} else if step.Program == "ra8ci:runner-clock" {
			result.ExitCode = runnerclock.Run(ctx, step.Args, stdoutLog, stderrLog)
		} else if step.Program == "ra8ci:tests-readme" {
			result.ExitCode = testsreadme.Run(ctx, root, step.Args, stdoutLog, stderrLog)
		} else if step.Program == "ra8ci:inclusive-terminology-commits" {
			result.ExitCode = committerms.Run(ctx, step.Args, os.Stdin, stdoutLog, stderrLog)
		} else if step.Program == "ra8ci:legacy-make" {
			result.ExitCode = legacymake.Run(ctx, root, step.Args, stdoutLog, stderrLog)
		} else if step.Program == "ra8ci:no-unsafe-python-install" {
			result.ExitCode = unsafeinstall.Run(ctx, root, step.Args, stdoutLog, stderrLog)
		} else if step.Program == "ra8ci:wave-references" {
			result.ExitCode = waverefs.Run(ctx, root, step.Args, stdoutLog, stderrLog)
		} else if step.Program == "ra8ci:pointer-boilerplate" {
			result.ExitCode = pointerboilerplate.Run(ctx, root, step.Args, stdoutLog, stderrLog)
		} else if step.Program == "ra8ci:nsc-veneer-defs" {
			result.ExitCode = nscveneers.Run(ctx, root, step.Args, stdoutLog, stderrLog)
		} else if step.Program == "ra8ci:stub-crypto-guard" {
			result.ExitCode = stubcryptoguard.Run(ctx, root, step.Args, stdoutLog, stderrLog)
		} else if step.Program == "ra8ci:tz-boundary-discard" {
			result.ExitCode = tzdiscard.Run(ctx, root, step.Args, stdoutLog, stderrLog)
		} else if step.Program == "ra8ci:driver-asm-guard" {
			result.ExitCode = driverasmguard.Run(ctx, root, step.Args, stdoutLog, stderrLog)
		} else if step.Program == "ra8ci:no-goto-setjmp" {
			result.ExitCode = gotosetjmp.Run(ctx, root, step.Args, stdoutLog, stderrLog)
		}
		if expiration := contextExpiration(ctx); expiration != nil {
			result.TimedOut = errors.Is(expiration, context.DeadlineExceeded)
			result.Cancelled = !result.TimedOut
		}
		result.StdoutSHA256, result.StdoutBytes = stdoutLog.digest()
		result.StderrSHA256, result.StderrBytes = stderrLog.digest()
		if logErr := errors.Join(stdoutLog.err, stderrLog.err); logErr != nil {
			return result, fmt.Errorf("execute %s: %w", step.Name, logErr)
		}
		return result, nil
	}
	program, err := resolveTaskProgram(root, step.Program)
	if err != nil {
		return result, err
	}
	stdoutLog := newDigestWriter(stdout)
	stderrLog := newDigestWriter(stderr)
	outcome, commandErr := runCommand(ctx, program, step.Args, root, env, stdoutLog, stderrLog, grace)
	result.ExitCode = outcome.ExitCode
	result.TimedOut = outcome.TimedOut
	result.Cancelled = outcome.Cancelled
	result.StdoutSHA256, result.StdoutBytes = stdoutLog.digest()
	result.StderrSHA256, result.StderrBytes = stderrLog.digest()
	logErr := errors.Join(stdoutLog.err, stderrLog.err)
	if commandErr != nil || logErr != nil {
		return result, fmt.Errorf("execute %s: %w", step.Name, errors.Join(commandErr, logErr))
	}
	return result, nil
}

type digestWriter struct {
	output io.Writer
	hash   hash.Hash
	bytes  int64
	err    error
}

func newDigestWriter(output io.Writer) *digestWriter {
	return &digestWriter{output: output, hash: sha256.New()}
}

func (writer *digestWriter) Write(data []byte) (int, error) {
	n, err := writer.output.Write(data)
	if err == nil && n != len(data) {
		err = io.ErrShortWrite
	}
	if err != nil && writer.err == nil {
		writer.err = err
	}
	if n > 0 {
		_, _ = writer.hash.Write(data[:n])
		writer.bytes += int64(n)
	}
	return n, err
}

func (writer *digestWriter) digest() (string, int64) {
	return hex.EncodeToString(writer.hash.Sum(nil)), writer.bytes
}

func cleanEnvironment(root string) ([]string, error) {
	allowed := map[string]bool{
		"PATH": true, "HOME": true, "USER": true, "LOGNAME": true,
		"LANG": true, "LC_ALL": true, "LC_CTYPE": true,
		"TMPDIR": true, "TMP": true, "TEMP": true,
		"XDG_CACHE_HOME": true, "XDG_CONFIG_HOME": true, "XDG_DATA_HOME": true,
		"GOCACHE": true, "GOMODCACHE": true, "GOPATH": true, "GOCOVERDIR": true,
		"GOROOT":      true,
		"CGO_ENABLED": true, "CLANG_FORMAT": true,
		"RA8_TOOL_BIN": true, "RA8_TOOL_VENV": true,
		"USERPROFILE": true, "APPDATA": true, "LOCALAPPDATA": true,
		"SYSTEMROOT": true, "COMSPEC": true,
	}
	pathValues := map[string]bool{
		"HOME": true, "TMPDIR": true, "TMP": true, "TEMP": true,
		"XDG_CACHE_HOME": true, "XDG_CONFIG_HOME": true, "XDG_DATA_HOME": true,
		"GOCACHE": true, "GOMODCACHE": true, "GOPATH": true, "GOROOT": true, "GOCOVERDIR": true,
		"USERPROFILE": true, "APPDATA": true, "LOCALAPPDATA": true,
		"RA8_TOOL_BIN": true, "RA8_TOOL_VENV": true,
	}
	env := make([]string, 0, len(allowed))
	seen := make(map[string]bool, len(allowed))
	for _, item := range os.Environ() {
		key, value, ok := strings.Cut(item, "=")
		key = normalizeEnvironmentKey(key, runtime.GOOS)
		if !ok || !allowed[key] {
			continue
		}
		if seen[key] {
			return nil, fmt.Errorf("%w: duplicate environment key %s", ErrUnsafeEnvironment, key)
		}
		seen[key] = true
		if key == "PATH" {
			for _, path := range filepath.SplitList(value) {
				if !filepath.IsAbs(path) {
					return nil, fmt.Errorf("%w: PATH has non-absolute entry", ErrUnsafeEnvironment)
				}
				within, err := isWithin(root, path)
				if err != nil {
					return nil, err
				}
				if within {
					return nil, fmt.Errorf("%w: PATH includes checkout", ErrUnsafeEnvironment)
				}
			}
		}
		if pathValues[key] && value != "" {
			if !filepath.IsAbs(value) {
				return nil, fmt.Errorf("%w: %s is not absolute", ErrUnsafeEnvironment, key)
			}
			if key != "USERPROFILE" && key != "APPDATA" && key != "LOCALAPPDATA" {
				within, err := isWithin(root, value)
				if err != nil {
					return nil, err
				}
				if within {
					return nil, fmt.Errorf("%w: %s points inside checkout", ErrUnsafeEnvironment, key)
				}
			}
		}
		env = append(env, item)
	}
	if runtime.GOOS == "windows" {
		if os.Getenv("SYSTEMROOT") == "" {
			return nil, fmt.Errorf("%w: SYSTEMROOT is missing", ErrUnsafeEnvironment)
		}
	}
	env = append(env, "GOTOOLCHAIN=local")
	return env, nil
}

func normalizeEnvironmentKey(key, goos string) string {
	if goos == "windows" {
		return strings.ToUpper(key)
	}
	return key
}

func isWithin(root, candidate string) (bool, error) {
	resolvedRoot, err := filepath.EvalSymlinks(root)
	if err != nil {
		return false, fmt.Errorf("%w: %v", ErrUnsafeEnvironment, err)
	}
	resolvedCandidate, err := resolvePath(candidate)
	if err != nil {
		return false, fmt.Errorf("%w: %v", ErrUnsafeEnvironment, err)
	}
	relative, err := filepath.Rel(resolvedRoot, resolvedCandidate)
	if err != nil {
		return false, fmt.Errorf("%w: %v", ErrUnsafeEnvironment, err)
	}
	return relative == "." || (relative != ".." && !strings.HasPrefix(relative, ".."+string(filepath.Separator))), nil
}

func resolvePath(path string) (string, error) {
	path = filepath.Clean(path)
	var missing []string
	for {
		resolved, err := filepath.EvalSymlinks(path)
		if err == nil {
			for index := len(missing) - 1; index >= 0; index-- {
				resolved = filepath.Join(resolved, missing[index])
			}
			return resolved, nil
		}
		if !errors.Is(err, os.ErrNotExist) {
			return "", err
		}
		parent := filepath.Dir(path)
		if parent == path {
			return "", err
		}
		missing = append(missing, filepath.Base(path))
		path = parent
	}
}

// resolveTaskProgram resolves explicit relative program paths from the verified
// checkout, never from the caller's current working directory. Symlink targets
// must remain within the checkout so a reviewed task cannot escape its source.
func resolveTaskProgram(root, name string) (string, error) {
	if name == "" {
		return "", fmt.Errorf("%w: empty task program", ErrUnreviewedTask)
	}
	if filepath.IsAbs(name) || (!strings.Contains(name, "/") && !strings.Contains(name, string(filepath.Separator))) {
		program, err := exec.LookPath(name)
		if err != nil {
			return "", fmt.Errorf("%w: %s: %v", ErrToolMissing, name, err)
		}
		return program, nil
	}
	relativeName := filepath.Clean(filepath.FromSlash(name))
	if relativeName == ".." || strings.HasPrefix(relativeName, ".."+string(filepath.Separator)) || filepath.IsAbs(relativeName) {
		return "", fmt.Errorf("%w: task program escapes the verified checkout: %s", ErrUnsafeEnvironment, name)
	}
	root, err := filepath.EvalSymlinks(root)
	if err != nil {
		return "", fmt.Errorf("%w: resolve checkout root: %v", ErrUnreviewedTask, err)
	}
	candidate := filepath.Join(root, filepath.FromSlash(name))
	resolved, err := filepath.EvalSymlinks(candidate)
	if err != nil {
		return "", fmt.Errorf("%w: %s: %v", ErrToolMissing, name, err)
	}
	relative, err := filepath.Rel(root, resolved)
	if err != nil || relative == ".." || strings.HasPrefix(relative, ".."+string(filepath.Separator)) || filepath.IsAbs(relative) {
		return "", fmt.Errorf("%w: task program escapes the verified checkout: %s", ErrUnsafeEnvironment, name)
	}
	info, err := os.Stat(resolved)
	if err != nil || !info.Mode().IsRegular() {
		return "", fmt.Errorf("%w: task program is not a regular file: %s", ErrUnreviewedTask, name)
	}
	if runtime.GOOS != "windows" && info.Mode().Perm()&0111 == 0 {
		return "", fmt.Errorf("%w: task program is not executable: %s", ErrUnreviewedTask, name)
	}
	return resolved, nil
}
