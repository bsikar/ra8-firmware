// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package github

import (
	"context"
	"errors"
	"fmt"
	"strings"
	"testing"

	"github.com/actions/scaleset"
)

type fakeScaleSetAdmin struct {
	jit       *scaleset.RunnerScaleSetJitRunnerConfig
	runner    *scaleset.RunnerReference
	byName    *scaleset.RunnerReference
	err       error
	requested string
	removed   int64
}

func (f *fakeScaleSetAdmin) GenerateJitRunnerConfig(_ context.Context, setting *scaleset.RunnerScaleSetJitRunnerSetting, _ int) (*scaleset.RunnerScaleSetJitRunnerConfig, error) {
	f.requested = setting.Name
	return f.jit, f.err
}

func (f *fakeScaleSetAdmin) GetRunner(context.Context, int) (*scaleset.RunnerReference, error) {
	return f.runner, f.err
}

func (f *fakeScaleSetAdmin) GetRunnerByName(context.Context, string) (*scaleset.RunnerReference, error) {
	return f.byName, f.err
}
func (f *fakeScaleSetAdmin) RemoveRunner(_ context.Context, id int64) error {
	f.removed = id
	return f.err
}

func TestGenerateJITBindsRunnerAndRedactsCredential(t *testing.T) {
	admin := &fakeScaleSetAdmin{jit: &scaleset.RunnerScaleSetJitRunnerConfig{
		Runner:           &scaleset.RunnerReference{ID: 7, Name: "ra8-lab-7", RunnerScaleSetID: 42},
		EncodedJITConfig: "one-time-secret",
	}}
	session := &Session{admin: admin, scaleSetID: 42}
	got, err := session.GenerateJIT(context.Background(), "ra8-lab-7")
	if err != nil {
		t.Fatal(err)
	}
	if got.Runner != (RunnerIdentity{ID: 7, Name: "ra8-lab-7"}) || string(got.EncodedConfig) != "one-time-secret" || admin.requested != "ra8-lab-7" {
		t.Fatalf("unexpected JIT result: runner=%+v config=%q requested=%q", got.Runner, got.EncodedConfig, admin.requested)
	}
	if strings.Contains(got.String(), "one-time-secret") {
		if strings.Contains(fmt.Sprintf("%#v", got), "one-time-secret") {
			t.Fatal("JIT config leaked through Go-syntax formatting")
		}
		t.Fatal("JIT config leaked through String")
	}
	got.Clear()
	if strings.Trim(string(got.EncodedConfig), "\x00") != "" {
		t.Fatal("Clear did not wipe the encoded config")
	}
}

func TestGenerateJITFailsClosedOnForeignRunner(t *testing.T) {
	for _, runner := range []*scaleset.RunnerReference{
		{ID: 7, Name: "ra8-lab-7", RunnerScaleSetID: 43},
	} {
		session := &Session{admin: &fakeScaleSetAdmin{jit: &scaleset.RunnerScaleSetJitRunnerConfig{
			Runner: runner, EncodedJITConfig: "secret",
		}}, scaleSetID: 42}
		if result, err := session.GenerateJIT(context.Background(), "ra8-lab-7"); err == nil || len(result.EncodedConfig) != 0 {
			t.Fatalf("foreign JIT identity accepted: result=%+v err=%v", result, err)
		}
	}
}

func TestRunnerLookupsRequireExactScaleSetIdentity(t *testing.T) {
	ctx := context.Background()
	admin := &fakeScaleSetAdmin{byName: &scaleset.RunnerReference{ID: 9, Name: "ra8-lab-9", RunnerScaleSetID: 42},
		runner: &scaleset.RunnerReference{ID: 9, Name: "ra8-lab-9", RunnerScaleSetID: 42}}
	session := &Session{admin: admin, scaleSetID: 42}
	if got, ok, err := session.RunnerByName(ctx, "ra8-lab-9"); err != nil || !ok || got.ID != 9 {
		t.Fatalf("RunnerByName = %+v, %v, %v", got, ok, err)
	}
	if got, ok, err := session.RunnerByID(ctx, 9); err != nil || !ok || got.Name != "ra8-lab-9" {
		t.Fatalf("RunnerByID = %+v, %v, %v", got, ok, err)
	}
	admin.byName.RunnerScaleSetID++
	if _, ok, err := session.RunnerByName(ctx, "ra8-lab-9"); err == nil || ok {
		t.Fatalf("foreign scale set accepted: ok=%v err=%v", ok, err)
	}
	admin.err = errors.New("API unavailable")
	if _, _, err := session.RunnerByID(ctx, 9); err == nil || !strings.Contains(err.Error(), "API unavailable") {
		t.Fatalf("API error was lost: %v", err)
	}
}

func TestRunnerAdminRejectsInvalidRequestsBeforeCallingAPI(t *testing.T) {
	admin := &fakeScaleSetAdmin{}
	session := &Session{admin: admin, scaleSetID: 42}
	if _, err := session.GenerateJIT(context.Background(), "../bad"); err == nil {
		t.Fatal("invalid runner name accepted")
	}
	if _, _, err := session.RunnerByID(context.Background(), 0); err == nil {
		t.Fatal("invalid runner ID accepted")
	}
	if admin.requested != "" {
		t.Fatalf("invalid request reached GitHub API: %q", admin.requested)
	}
}

func TestRemoveRunnerVerifiesScaleSetOwnership(t *testing.T) {
	admin := &fakeScaleSetAdmin{runner: &scaleset.RunnerReference{ID: 9, Name: "ra8-lab-9", RunnerScaleSetID: 42}}
	session := &Session{admin: admin, scaleSetID: 42}
	if err := session.RemoveRunner(context.Background(), 9); err != nil || admin.removed != 9 {
		t.Fatalf("RemoveRunner = %v, removed=%d", err, admin.removed)
	}
	admin.removed = 0
	admin.runner = nil
	if err := session.RemoveRunner(context.Background(), 9); err != nil || admin.removed != 0 {
		t.Fatalf("absent runner removal = %v, removed=%d", err, admin.removed)
	}
	admin.runner = &scaleset.RunnerReference{ID: 9, Name: "foreign", RunnerScaleSetID: 99}
	if err := session.RemoveRunner(context.Background(), 9); err == nil || admin.removed != 0 {
		t.Fatalf("foreign runner removal not rejected: err=%v removed=%d", err, admin.removed)
	}
}
