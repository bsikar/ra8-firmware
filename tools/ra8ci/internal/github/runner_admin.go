// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package github

import (
	"context"
	"errors"
	"fmt"
	"regexp"

	"github.com/actions/scaleset"
)

const maxJITConfigBytes = 64 << 10

var runnerIdentityName = regexp.MustCompile(`^[A-Za-z0-9][A-Za-z0-9-]{0,63}$`)

// RunnerIdentity is an independently observed GitHub runner reference.
type RunnerIdentity struct {
	ID   int
	Name string
}

// JITRunnerConfig contains a one-time credential. Call Clear immediately after
// delivery; String deliberately redacts the credential from formatted output.
type JITRunnerConfig struct {
	Runner        RunnerIdentity
	EncodedConfig []byte
}

func (c JITRunnerConfig) String() string {
	return fmt.Sprintf("JITRunnerConfig{Runner:%+v, EncodedConfig:[REDACTED]}", c.Runner)
}

// Clear overwrites the caller-owned encoded configuration buffer.
func (c JITRunnerConfig) Clear() {
	clear(c.EncodedConfig)
}

// GenerateJIT requests a JIT config for one validated runner name and checks
// the returned runner identity against this session's scale set.
func (s *Session) GenerateJIT(ctx context.Context, name string) (JITRunnerConfig, error) {
	if s == nil || s.admin == nil || ctx == nil || s.scaleSetID <= 0 || !runnerIdentityName.MatchString(name) {
		return JITRunnerConfig{}, errors.New("invalid JIT runner request")
	}
	result, err := s.admin.GenerateJitRunnerConfig(ctx,
		&scaleset.RunnerScaleSetJitRunnerSetting{Name: name, WorkFolder: "_work"}, s.scaleSetID)
	if err != nil {
		return JITRunnerConfig{}, fmt.Errorf("generate GitHub JIT runner config: %w", err)
	}
	if result == nil || result.Runner == nil || result.Runner.ID <= 0 || result.Runner.Name != name ||
		result.Runner.RunnerScaleSetID != s.scaleSetID || len(result.EncodedJITConfig) == 0 || len(result.EncodedJITConfig) > maxJITConfigBytes {
		return JITRunnerConfig{}, errors.New("GitHub returned a mismatched or invalid JIT runner config")
	}
	return JITRunnerConfig{Runner: RunnerIdentity{ID: result.Runner.ID, Name: result.Runner.Name},
		EncodedConfig: []byte(result.EncodedJITConfig)}, nil
}

// RunnerByName returns a runner only if GitHub associates it with this exact
// scale set. A missing runner is a normal false result.
func (s *Session) RunnerByName(ctx context.Context, name string) (RunnerIdentity, bool, error) {
	if s == nil || s.admin == nil || ctx == nil || s.scaleSetID <= 0 || !runnerIdentityName.MatchString(name) {
		return RunnerIdentity{}, false, errors.New("invalid GitHub runner lookup")
	}
	runner, err := s.admin.GetRunnerByName(ctx, name)
	if err != nil {
		return RunnerIdentity{}, false, fmt.Errorf("look up GitHub runner by name: %w", err)
	}
	if runner == nil {
		return RunnerIdentity{}, false, nil
	}
	if runner.ID <= 0 || runner.Name != name || runner.RunnerScaleSetID != s.scaleSetID {
		return RunnerIdentity{}, false, errors.New("GitHub runner lookup returned a foreign identity")
	}
	return RunnerIdentity{ID: runner.ID, Name: runner.Name}, true, nil
}

// RunnerByID returns a runner only if its ID and scale-set ownership agree.
func (s *Session) RunnerByID(ctx context.Context, id int) (RunnerIdentity, bool, error) {
	if s == nil || s.admin == nil || ctx == nil || s.scaleSetID <= 0 || id <= 0 {
		return RunnerIdentity{}, false, errors.New("invalid GitHub runner lookup")
	}
	runner, err := s.admin.GetRunner(ctx, id)
	if err != nil {
		return RunnerIdentity{}, false, fmt.Errorf("look up GitHub runner by ID: %w", err)
	}
	if runner == nil {
		return RunnerIdentity{}, false, nil
	}
	if runner.ID != id || runner.Name == "" || runner.RunnerScaleSetID != s.scaleSetID {
		return RunnerIdentity{}, false, errors.New("GitHub runner lookup returned a foreign identity")
	}
	return RunnerIdentity{ID: runner.ID, Name: runner.Name}, true, nil
}

func (s *Session) RemoveRunner(ctx context.Context, id int) error {
	if s == nil || s.admin == nil || ctx == nil || s.scaleSetID <= 0 || id <= 0 {
		return errors.New("invalid GitHub runner removal")
	}
	runner, exists, err := s.RunnerByID(ctx, id)
	if err != nil {
		return err
	}
	if !exists {
		return nil
	}
	if runner.ID != id {
		return errors.New("refusing to remove a mismatched GitHub runner")
	}
	if err := s.admin.RemoveRunner(ctx, int64(id)); err != nil {
		return fmt.Errorf("remove GitHub runner %d: %w", id, err)
	}
	return nil
}
