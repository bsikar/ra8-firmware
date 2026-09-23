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
	boardID := os.Getenv("RA8CI_BOARD_ID")
	stateFile := os.Getenv("RA8CI_BOARD_STATE_FILE")
	config := boardclient.Config{
		ServerURL: os.Getenv("RA8CI_SERVER_URL"),
		CAFile:    os.Getenv("RA8CI_SERVER_CA"),
		CertFile:  os.Getenv("RA8CI_BOARD_AGENT_CERT"),
		KeyFile:   os.Getenv("RA8CI_BOARD_AGENT_KEY"),
	}
	if boardID == "" || stateFile == "" || config.ServerURL == "" || config.CAFile == "" ||
		config.CertFile == "" || config.KeyFile == "" {
		return errors.New("board-agent requires RA8CI_BOARD_ID, RA8CI_BOARD_STATE_FILE, RA8CI_SERVER_URL, RA8CI_SERVER_CA, RA8CI_BOARD_AGENT_CERT, and RA8CI_BOARD_AGENT_KEY")
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
