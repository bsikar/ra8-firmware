// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package main

import (
	"context"
	"encoding/json"
	"errors"
	"flag"
	"fmt"
	"io"
	"os"
	"strings"
	"time"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/hilspec"
	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/store"
)

// hilSubcommand is one thing `ra8ci hil` does, named the way it is typed.
//
// Usage states the subcommand with its arguments and begins with Name: a
// subcommand cannot be dispatched under one name and stated under another.
type hilSubcommand struct {
	Name  string
	Usage string
	Run   func(ctx context.Context, args []string) error
}

// hilSubcommands is the one list: what hilCommand dispatches and what the
// usage states.
//
// Run is handed the arguments after the subcommand name.
func hilSubcommands() []hilSubcommand {
	return []hilSubcommand{
		{Name: "budget", Usage: "budget --board-id ID --manifest examples/.../hil.conf --board-model MODEL --program-family NAME --flash-restore-bound DURATION [--safety-maximum DURATION]", Run: hilBudgetCommand},
		{Name: "verify-capture", Usage: "verify-capture --manifest examples/.../hil.conf --capture FILE", Run: hilVerifyCaptureCommand},
	}
}

// hilUsage states the HIL subcommands the way the front door lists them: the
// names alone, because each subcommand prints its own arguments where it
// parses them.
func hilUsage() string {
	subcommands := hilSubcommands()
	named := make([]string, 0, len(subcommands))
	for _, subcommand := range subcommands {
		named = append(named, subcommand.Name)
	}
	return "hil " + strings.Join(named, "|")
}

// hilUsageError states every HIL subcommand with its arguments. It is the
// answer to no subcommand at all and to a name nothing dispatches, both of
// which were previously told about one subcommand and not the other.
func hilUsageError() error {
	subcommands := hilSubcommands()
	stated := make([]string, 0, len(subcommands))
	for _, subcommand := range subcommands {
		stated = append(stated, subcommand.Usage)
	}
	return errors.New("usage: ra8ci hil " + strings.Join(stated, " | hil "))
}

func hilCommand(ctx context.Context, args []string) error {
	if len(args) == 0 {
		return hilUsageError()
	}
	for _, subcommand := range hilSubcommands() {
		if subcommand.Name == args[0] {
			return subcommand.Run(ctx, args[1:])
		}
	}
	return hilUsageError()
}

// hilBudgetCommand chooses the validity window a board's fixture supports.
func hilBudgetCommand(ctx context.Context, args []string) error {
	flags := flag.NewFlagSet("hil budget", flag.ContinueOnError)
	flags.SetOutput(io.Discard)
	boardID := flags.String("board-id", "", "registered board ID")
	manifest := flags.String("manifest", "", "HIL manifest beneath examples/")
	boardModel := flags.String("board-model", "", "reviewed board model")
	programFamily := flags.String("program-family", "", "reviewed firmware program family")
	restoreText := flags.String("flash-restore-bound", "", "reserved board flash/restore time")
	safetyText := flags.String("safety-maximum", "", "optional fixture safety cap")
	if err := flags.Parse(args); err != nil {
		return fmt.Errorf("usage: ra8ci hil budget: %w", err)
	}
	if flags.NArg() != 0 || *boardID == "" || *manifest == "" || !validHILLabel(*boardModel) ||
		!validHILLabel(*programFamily) || *restoreText == "" {
		return errors.New("usage: ra8ci hil budget --board-id ID --manifest examples/.../hil.conf --board-model MODEL --program-family NAME --flash-restore-bound DURATION [--safety-maximum DURATION]")
	}
	restoreBound, err := time.ParseDuration(*restoreText)
	if err != nil || restoreBound <= 0 || restoreBound > time.Hour {
		return errors.New("flash-restore-bound must be greater than zero and no longer than 1h")
	}
	var safetyMaximum time.Duration
	if *safetyText != "" {
		safetyMaximum, err = time.ParseDuration(*safetyText)
		if err != nil || safetyMaximum <= 0 || safetyMaximum > time.Hour {
			return errors.New("safety-maximum must be greater than zero and no longer than 1h")
		}
	}
	root, err := findCheckout()
	if err != nil {
		return err
	}
	spec, err := hilspec.Load(root, *manifest)
	if err != nil {
		return fmt.Errorf("load HIL manifest: %w", err)
	}
	dsn := os.Getenv("RA8CI_DATABASE_URL")
	if strings.TrimSpace(dsn) == "" {
		return errors.New("ra8ci hil budget requires RA8CI_DATABASE_URL")
	}
	if ctx == nil {
		return errors.New("HIL budget requires a context")
	}
	queryCtx, cancel := context.WithTimeout(ctx, 15*time.Second)
	defer cancel()
	st, err := store.Open(queryCtx, dsn)
	if err != nil {
		return fmt.Errorf("open HIL timing database: %w", err)
	}
	defer st.Close()
	fixture, err := st.ApprovedBoardFixtureProfile(queryCtx, *boardID)
	if err != nil {
		return fmt.Errorf("load approved board fixture: %w", err)
	}
	workload := hilspec.Workload{ManifestPath: spec.Path, BoardModel: *boardModel,
		FixtureRevision: fixture.FixtureRevision, ProfileSHA256: fixture.ProfileSHA256,
		ProgramFamily: *programFamily, Mode: spec.Mode}
	decision, err := hilspec.Decide(queryCtx, spec, workload, st, hilspec.Options{
		FlashRestoreBound: restoreBound, SafetyMaximum: safetyMaximum,
	})
	if err != nil {
		return fmt.Errorf("choose HIL validity window: %w", err)
	}
	return json.NewEncoder(os.Stdout).Encode(map[string]any{
		"workload":             workload,
		"validity_window":      decision.ValidityWindow.String(),
		"flash_restore_bound":  decision.FlashRestoreBound.String(),
		"safety_maximum":       decision.SafetyMaximum.String(),
		"minimum_lease_budget": decision.MinimumLeaseBudget().String(),
		"source":               decision.Source, "samples": decision.Samples,
		"rejected_rows": decision.RejectedRows, "mean_seconds": decision.MeanSeconds,
		"max_seconds": decision.MaxSeconds, "stddev_seconds": decision.StddevSeconds,
	})
}

func validHILLabel(value string) bool {
	if value == "" || len(value) > 128 || strings.TrimSpace(value) != value {
		return false
	}
	for _, character := range value {
		if !((character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z') ||
			(character >= '0' && character <= '9') || strings.ContainsRune("._+-", character)) {
			return false
		}
	}
	return true
}
