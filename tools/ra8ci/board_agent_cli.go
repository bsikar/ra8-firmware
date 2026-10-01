// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package main

import (
	"context"
	"errors"
	"fmt"
	"os"
	"runtime"
	"time"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/boardagent"
	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/boardclient"
)

// runBoardAgent operates with a board-agent-only certificate and a durable,
// board-bound generation file. It intentionally has no task execution hooks.
func runBoardAgent(ctx context.Context) error {
	if runtime.GOOS != "linux" {
		return errors.New("board-agent service mode is supported only on Linux")
	}
	endpoint, err := resolveClientEndpoint("ra8ci board-agent", roleBoardAgent, os.Getenv, envBoardID, envBoardStateFile)
	if err != nil {
		return err
	}
	boardID := os.Getenv(envBoardID)
	stateFile := os.Getenv(envBoardStateFile)
	config := boardclient.Config{
		ServerURL: endpoint.ServerURL,
		CAFile:    endpoint.CAFile,
		CertFile:  endpoint.CertFile,
		KeyFile:   endpoint.KeyFile,
	}
	interval := time.Second
	if value := os.Getenv("RA8CI_BOARD_AGENT_POLL_INTERVAL"); value != "" {
		parsed, err := time.ParseDuration(value)
		if err != nil || parsed < 250*time.Millisecond || parsed > 30*time.Second {
			return errors.New("RA8CI_BOARD_AGENT_POLL_INTERVAL must be between 250ms and 30s")
		}
		interval = parsed
	}
	store, err := boardagent.NewFileHighWater(stateFile, boardID)
	if err != nil {
		return fmt.Errorf("board-agent state: %w", err)
	}
	client, err := boardclient.New(config)
	if err != nil {
		return fmt.Errorf("board-agent client: %w", err)
	}
	defer client.CloseIdleConnections()
	service, err := boardagent.New(boardID, client, store, interval)
	if err != nil {
		return err
	}
	return service.Run(ctx)
}
