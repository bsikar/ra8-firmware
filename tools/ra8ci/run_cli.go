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
	"os/exec"
	"strings"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/catalog"
	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/runclient"
	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/source"
	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/store"
)

// runSubcommand is one thing `ra8ci run` dispatches, named the way it is
// typed.
type runSubcommand struct {
	Name string
	Run  func(ctx context.Context, args []string) error
}

// runSubcommands is the one list: what runCommand dispatches and what its
// usage states. The order is the order a run is lived through, deliberately
// not alphabetical.
func runSubcommands() []runSubcommand {
	return []runSubcommand{
		{Name: "submit", Run: submitRun},
		{Name: "status", Run: showRun},
		{Name: "logs", Run: showRunLogs},
		{Name: "events", Run: showRunEvents},
		{Name: "cancel", Run: cancelRun},
	}
}

// runUsage states `ra8ci run` the way the front door and the command itself
// both print it, built from the table so neither can name a subcommand the
// other does not dispatch.
func runUsage() string {
	subcommands := runSubcommands()
	named := make([]string, 0, len(subcommands))
	for _, subcommand := range subcommands {
		named = append(named, subcommand.Name)
	}
	return "run " + strings.Join(named, "|")
}

func runCommand(ctx context.Context, args []string) error {
	if len(args) == 0 {
		return errors.New("usage: ra8ci " + runUsage())
	}
	for _, subcommand := range runSubcommands() {
		if subcommand.Name == args[0] {
			return subcommand.Run(ctx, args[1:])
		}
	}
	return errors.New("usage: ra8ci " + runUsage())
}

func showRunLogs(ctx context.Context, args []string) error {
	flags := flag.NewFlagSet("run logs", flag.ContinueOnError)
	flags.SetOutput(io.Discard)
	after := flags.Int64("after", 0, "resume after this global log sequence")
	limit := flags.Int("limit", store.MaxLogPageSize, "chunks per API page")
	if err := flags.Parse(args); err != nil || flags.NArg() != 2 {
		return errors.New("usage: ra8ci run logs [--after SEQ] [--limit 1..8] RUN_ID ATTEMPT_ID")
	}
	client, err := newRunClient()
	if err != nil {
		return err
	}
	defer client.Close()
	cursor := *after
	for {
		page, err := client.Logs(ctx, flags.Arg(0), flags.Arg(1), cursor, *limit)
		if err != nil {
			return err
		}
		for _, chunk := range page.Chunks {
			output := io.Writer(os.Stdout)
			if chunk.Stream == "stderr" {
				output = os.Stderr
			}
			written, writeErr := output.Write(chunk.Data)
			if writeErr != nil {
				return fmt.Errorf("write %s log chunk %d: %w", chunk.Stream, chunk.Sequence, writeErr)
			}
			if written != len(chunk.Data) {
				return fmt.Errorf("write %s log chunk %d: %w", chunk.Stream, chunk.Sequence, io.ErrShortWrite)
			}
		}
		if !page.HasMore {
			return nil
		}
		if page.NextAfter <= cursor {
			return errors.New("run log cursor did not advance")
		}
		cursor = page.NextAfter
	}
}

// submitUsage is the one place the submit grammar is stated. Arguments follow
// the task they belong to, so several parameterised tasks fit in one run.
const submitUsage = "usage: ra8ci run submit --idempotency-key KEY TASK [NAME=VALUE...] [TASK [NAME=VALUE...]]..."

func submitRun(ctx context.Context, args []string) error {
	flags := flag.NewFlagSet("run submit", flag.ContinueOnError)
	flags.SetOutput(io.Discard)
	key := flags.String("idempotency-key", "", "stable caller-generated retry key")
	if err := flags.Parse(args); err != nil {
		return fmt.Errorf("%s: %w", submitUsage, err)
	}
	if *key == "" || flags.NArg() == 0 {
		return errors.New(submitUsage)
	}
	submitted, err := splitSubmitTasks(flags.Args())
	if err != nil {
		return fmt.Errorf("%s: %w", submitUsage, err)
	}
	root, err := findCheckout()
	if err != nil {
		return err
	}
	snapshot, err := source.Snapshot(ctx, root)
	if err != nil {
		return fmt.Errorf("dispatch requires a clean pinned source snapshot: %w", err)
	}
	branch := ""
	branchOutput, err := exec.CommandContext(ctx, "git", "-C", root, "symbolic-ref", "--short", "-q", "HEAD").Output()
	if err == nil {
		branch = strings.TrimSpace(string(branchOutput))
	}
	definitions, err := catalog.Load()
	if err != nil {
		return err
	}
	seen := make(map[string]bool, len(submitted))
	tasks := make([]runclient.Task, 0, len(submitted))
	for index, entry := range submitted {
		definition, found := definitions.Task(entry.Name)
		if !found {
			return fmt.Errorf("unknown task %q", entry.Name)
		}
		values, err := taskArgumentValues(definition, entry.Words)
		if err != nil {
			return fmt.Errorf("task %q: %w", entry.Name, err)
		}
		// Bind here as well as at the plane. The plane binds argv itself and
		// is the enforcing side, but a missing positional or an undeclared
		// name is a typing mistake, and it reads better as a local refusal
		// naming the argument than as an invalid_argument from the API.
		if _, err := definition.BindArguments(values); err != nil {
			return fmt.Errorf("task %q: %w (accepts %s)", entry.Name, err, argumentUsage(definition))
		}
		if definition.Scope != "safe-local-read-only" || definition.BoardPolicy != "none" {
			return fmt.Errorf("task %q is not eligible for remote dispatch", entry.Name)
		}
		identity := submissionIdentity(entry.Name, values)
		if seen[identity] {
			return fmt.Errorf("task %q appears more than once with the same arguments", entry.Name)
		}
		seen[identity] = true
		tasks = append(tasks, runclient.Task{Key: fmt.Sprintf("task-%03d", index+1), Name: entry.Name,
			Args: []string{}, Values: values, DependsOnKeys: []string{}})
	}
	repository := os.Getenv("RA8CI_REPOSITORY")
	if repository == "" {
		repository = "bsikar/ra8-firmware"
	}
	client, err := newRunClient()
	if err != nil {
		return err
	}
	defer client.Close()
	receipt, err := client.Submit(ctx, *key, runclient.SubmitRequest{
		Trigger: "cli", Source: runclient.Source{Repository: repository, Branch: branch,
			CommitSHA: snapshot.RootCommit, SnapshotSHA256: snapshot.Digest},
		CatalogDigest: definitions.Digest(), Tasks: tasks,
	})
	if err != nil {
		return err
	}
	return writeJSON(os.Stdout, receipt)
}

func showRunEvents(ctx context.Context, args []string) error {
	flags := flag.NewFlagSet("run events", flag.ContinueOnError)
	flags.SetOutput(io.Discard)
	after := flags.Int64("after", 0, "resume after this run event sequence")
	limit := flags.Int("limit", store.MaxEventPageSize, "events per API page")
	if err := flags.Parse(args); err != nil || flags.NArg() != 1 || !store.ValidID(flags.Arg(0)) {
		return errors.New("usage: ra8ci run events [--after SEQ] [--limit 1..50] RUN_ID")
	}
	client, err := newRunClient()
	if err != nil {
		return err
	}
	defer client.Close()
	cursor := *after
	for {
		page, err := client.Events(ctx, flags.Arg(0), cursor, *limit)
		if err != nil {
			return err
		}
		for _, event := range page.Events {
			if err := writeJSON(os.Stdout, event); err != nil {
				return err
			}
		}
		if !page.HasMore {
			return nil
		}
		if page.NextAfter <= cursor {
			return errors.New("run event cursor did not advance")
		}
		cursor = page.NextAfter
	}
}

func cancelRun(ctx context.Context, args []string) error {
	if len(args) != 1 || !store.ValidID(args[0]) {
		return errors.New("usage: ra8ci run cancel RUN_ID")
	}
	client, err := newRunClient()
	if err != nil {
		return err
	}
	defer client.Close()
	run, err := client.Cancel(ctx, args[0])
	if err != nil {
		return err
	}
	return writeJSON(os.Stdout, run)
}

func showRun(ctx context.Context, args []string) error {
	if len(args) != 1 {
		return errors.New("usage: ra8ci run status RUN_ID")
	}
	client, err := newRunClient()
	if err != nil {
		return err
	}
	defer client.Close()
	run, err := client.Get(ctx, args[0])
	if err != nil {
		return err
	}
	return writeJSON(os.Stdout, run)
}

func newRunClient() (*runclient.Client, error) {
	endpoint, err := resolveClientEndpoint("ra8ci run", roleOperator, os.Getenv)
	if err != nil {
		return nil, err
	}
	return runclient.New(runclient.Config{ServerURL: endpoint.ServerURL,
		CAFile: endpoint.CAFile, CertFile: endpoint.CertFile, KeyFile: endpoint.KeyFile})
}

func writeJSON(writer io.Writer, value any) error {
	if err := json.NewEncoder(writer).Encode(value); err != nil {
		return fmt.Errorf("write JSON output: %w", err)
	}
	return nil
}
