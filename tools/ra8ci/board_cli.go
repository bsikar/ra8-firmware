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
	"time"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/board"
	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/boardclient"
	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/store"
)

func boardCommand(ctx context.Context, args []string) error {
	if len(args) > 0 && args[0] == "checkpoint" {
		if len(args) != 2 || !validBoardIDArgument(args[1]) {
			return errors.New("usage: ra8ci board checkpoint <board-id>")
		}
		client, err := newBoardClient()
		if err != nil {
			return err
		}
		defer client.CloseIdleConnections()
		directory, err := currentBoardLeaseDirectory()
		if err != nil {
			return err
		}
		snapshot, err := checkpointBoardLease(ctx, client, directory, args[1])
		if err != nil {
			return fmt.Errorf("board checkpoint: %w", err)
		}
		return json.NewEncoder(os.Stdout).Encode(snapshot)
	}
	if len(args) > 0 && args[0] == "extend" {
		return boardExtendCommand(ctx, args[1:])
	}
	if len(args) > 0 && args[0] == "recover" {
		return boardRecoverCommand(ctx, args[1:])
	}
	if len(args) > 0 && args[0] == "cancel" {
		ticket, err := parseBoardCancel(args[1:])
		if err != nil {
			return err
		}
		client, err := newBoardClient()
		if err != nil {
			return err
		}
		defer client.CloseIdleConnections()
		if err := client.Cancel(ctx, ticket); err != nil {
			if errors.Is(err, boardclient.ErrAlreadyGranted) {
				return fmt.Errorf("board request %s was already granted; cancellation cannot release the board", ticket.RequestID)
			}
			return fmt.Errorf("cancel board request %s: %w", ticket.RequestID, err)
		}
		return json.NewEncoder(os.Stdout).Encode(map[string]any{
			"board_id": ticket.BoardID, "request_id": ticket.RequestID, "cancelled": true,
		})
	}
	if len(args) == 2 && args[0] == "status" {
		client, err := newBoardClient()
		if err != nil {
			return err
		}
		defer client.CloseIdleConnections()
		snapshot, err := client.Status(ctx, args[1])
		if err != nil {
			return err
		}
		return json.NewEncoder(os.Stdout).Encode(snapshot)
	}
	if len(args) < 2 || args[0] != "take" {
		return errors.New("usage: ra8ci board status <board-id> | board take <board-id> --class human|ci|agent --why <reason> --duration <duration> | board checkpoint <board-id> | board extend <board-id> --why <reason> --duration <duration> | board recover <board-id> --plan <plan-id> --why <reason> | board cancel <board-id> <request-id> <lease-id>")
	}
	flags := flag.NewFlagSet("board take", flag.ContinueOnError)
	flags.SetOutput(io.Discard)
	classText := flags.String("class", "human", "priority class: human or agent")
	why := flags.String("why", "", "reason for taking the board")
	durationText := flags.String("duration", "", "requested lease duration")
	if err := flags.Parse(args[2:]); err != nil {
		return fmt.Errorf("usage: ra8ci board take <board-id> --why <reason> --duration <duration>: %w", err)
	}
	if flags.NArg() != 0 || *why == "" || *durationText == "" {
		return errors.New("usage: ra8ci board take <board-id> --class human|ci|agent --why <reason> --duration <duration>")
	}
	class := board.ClassHuman
	maxDuration := 8 * time.Hour
	switch *classText {
	case "human":
	case "ci":
		class = board.ClassCI
		maxDuration = 2 * time.Hour
	case "agent":
		class = board.ClassAI
		maxDuration = time.Hour
	default:
		return errors.New("board take class must be human, ci, or agent")
	}
	duration, err := time.ParseDuration(*durationText)
	if err != nil || duration <= 0 || duration%time.Second != 0 || duration > maxDuration {
		return fmt.Errorf("board lease duration must be a whole number of seconds between 1s and %s", maxDuration)
	}
	client, err := newBoardClient()
	if err != nil {
		return err
	}
	defer client.CloseIdleConnections()
	ticket, err := client.RequestTake(ctx, args[1], class, *why, duration)
	if err != nil {
		if ticket.RequestID != "" {
			return fmt.Errorf("board request outcome may be ambiguous (request %s, lease %s): %w", ticket.RequestID, ticket.LeaseID, err)
		}
		return err
	}
	fmt.Fprintf(os.Stderr, "ra8ci: waiting for board %s (request %s, lease %s)\n", ticket.BoardID, ticket.RequestID, ticket.LeaseID)
	token, err := client.WaitForGrant(ctx, ticket)
	if err != nil {
		cleanupCtx, cancel := context.WithTimeout(context.WithoutCancel(ctx), 3*time.Second)
		defer cancel()
		if cancelErr := client.Cancel(cleanupCtx, ticket); cancelErr != nil && !errors.Is(cancelErr, boardclient.ErrAlreadyGranted) {
			return fmt.Errorf("board request %s is still queued or granted; wait failed: %v; cancel failed: %w", ticket.RequestID, err, cancelErr)
		}
		return fmt.Errorf("waiting for board request %s: %w", ticket.RequestID, err)
	}
	directory, directoryErr := currentBoardLeaseDirectory()
	var saveErr error
	if directoryErr == nil {
		saveErr = writeBoardLeaseToken(directory, token)
	} else {
		saveErr = directoryErr
	}
	if encodeErr := json.NewEncoder(os.Stdout).Encode(struct {
		Ticket boardclient.Ticket     `json:"ticket"`
		Lease  boardclient.LeaseToken `json:"lease"`
	}{Ticket: ticket, Lease: token}); encodeErr != nil {
		return encodeErr
	}
	if saveErr != nil {
		return fmt.Errorf("board lease is granted; token JSON was written to stdout but could not be saved privately: %w", saveErr)
	}
	return nil
}

func boardExtendCommand(ctx context.Context, args []string) error {
	if len(args) < 1 || !validBoardIDArgument(args[0]) {
		return errors.New("usage: ra8ci board extend <board-id> --why <reason> --duration <duration>")
	}
	flags := flag.NewFlagSet("board extend", flag.ContinueOnError)
	flags.SetOutput(io.Discard)
	why := flags.String("why", "", "reason for extending the board lease")
	durationText := flags.String("duration", "", "additional lease duration")
	if err := flags.Parse(args[1:]); err != nil {
		return fmt.Errorf("usage: ra8ci board extend <board-id> --why <reason> --duration <duration>: %w", err)
	}
	if flags.NArg() != 0 || *why == "" || *durationText == "" {
		return errors.New("usage: ra8ci board extend <board-id> --why <reason> --duration <duration>")
	}
	duration, err := time.ParseDuration(*durationText)
	if err != nil || duration <= 0 || duration%time.Second != 0 || duration > 8*time.Hour {
		return errors.New("board extension duration must be a whole number of seconds between 1s and 8h")
	}
	directory, err := currentBoardLeaseDirectory()
	if err != nil {
		return err
	}
	client, err := newBoardClient()
	if err != nil {
		return err
	}
	defer client.CloseIdleConnections()
	snapshot, err := extendBoardLease(ctx, client, directory, args[0], time.Now().UTC().Add(duration), *why)
	if err != nil {
		return fmt.Errorf("extend board lease: %w", err)
	}
	return json.NewEncoder(os.Stdout).Encode(snapshot)
}

func parseBoardCancel(args []string) (boardclient.Ticket, error) {
	if len(args) != 3 || !validBoardIDArgument(args[0]) || !store.ValidID(args[1]) || !store.ValidID(args[2]) {
		return boardclient.Ticket{}, errors.New("usage: ra8ci board cancel <board-id> <request-id> <lease-id>")
	}
	return boardclient.Ticket{BoardID: args[0], RequestID: args[1], LeaseID: args[2]}, nil
}

func validBoardIDArgument(id string) bool {
	if id == "" || len(id) > 128 {
		return false
	}
	for _, value := range id {
		if !((value >= 97 && value <= 122) || (value >= 65 && value <= 90) ||
			(value >= 48 && value <= 57) || value == '-' || value == '_' || value == '.') {
			return false
		}
	}
	return true
}
func newBoardClient() (*boardclient.Client, error) {
	endpoint, err := resolveClientEndpoint("ra8ci board", roleOperator, os.Getenv)
	if err != nil {
		return nil, err
	}
	client, err := boardclient.New(boardclient.Config{ServerURL: endpoint.ServerURL,
		CAFile: endpoint.CAFile, CertFile: endpoint.CertFile, KeyFile: endpoint.KeyFile})
	if err != nil {
		return nil, fmt.Errorf("board client: %w", err)
	}
	return client, nil
}

// boardRecoverCommand hands a reviewed recovery plan to a board that is waiting
// for one. The plan identifier is required: an operator naming no plan is not
// approving a hardware sequence, and nothing else in the tree may name one on
// their behalf.
func boardRecoverCommand(ctx context.Context, args []string) error {
	usage := "usage: ra8ci board recover <board-id> --plan <plan-id> --why <reason>"
	if len(args) < 1 || !validBoardIDArgument(args[0]) {
		return errors.New(usage)
	}
	flags := flag.NewFlagSet("board recover", flag.ContinueOnError)
	flags.SetOutput(io.Discard)
	plan := flags.String("plan", "", "identifier of the reviewed recovery plan")
	why := flags.String("why", "", "reason this board needs recovery")
	if err := flags.Parse(args[1:]); err != nil {
		return fmt.Errorf("%s: %w", usage, err)
	}
	if flags.NArg() != 0 || *why == "" {
		return errors.New(usage)
	}
	if !store.ValidID(*plan) {
		return errors.New("board recover --plan must be the identifier of a reviewed recovery plan")
	}
	client, err := newBoardClient()
	if err != nil {
		return err
	}
	defer client.CloseIdleConnections()
	snapshot, err := client.StartRecovery(ctx, args[0], *plan, *why)
	if err != nil {
		if errors.Is(err, boardclient.ErrNoRecoveryPending) {
			return fmt.Errorf("board %s is not waiting for a recovery plan", args[0])
		}
		return fmt.Errorf("start board recovery: %w", err)
	}
	return json.NewEncoder(os.Stdout).Encode(snapshot)
}
