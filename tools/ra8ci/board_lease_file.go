// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package main

import (
	"bytes"
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"os"
	"path/filepath"
	"time"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/board"
	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/boardclient"
	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/store"
)

func currentBoardLeaseDirectory() (string, error) {
	config, err := os.UserConfigDir()
	if err != nil || config == "" {
		return "", errors.New("user configuration directory is unavailable")
	}
	appDirectory := filepath.Join(config, "ra8ci")
	if err := os.MkdirAll(appDirectory, 0o700); err != nil {
		return "", errors.New("create private ra8ci configuration directory")
	}
	info, err := os.Lstat(appDirectory)
	if err != nil || !info.IsDir() || info.Mode()&os.ModeSymlink != 0 || info.Mode().Perm()&0o077 != 0 {
		return "", errors.New("ra8ci configuration directory must be private and not a symlink")
	}
	return filepath.Join(appDirectory, "board-leases"), nil
}

func writeBoardLeaseToken(directory string, token boardclient.LeaseToken) error {
	if !validBoardIDArgument(token.BoardID) || !store.ValidID(token.RequestID) ||
		!store.ValidID(token.LeaseID) || token.Generation == 0 || token.Version == 0 || token.ExpiresAt.IsZero() {
		return errors.New("invalid board lease token")
	}
	if err := os.MkdirAll(directory, 0o700); err != nil {
		return errors.New("create private board lease directory")
	}
	info, err := os.Lstat(directory)
	if err != nil || !info.IsDir() || info.Mode()&os.ModeSymlink != 0 || info.Mode().Perm()&0o077 != 0 {
		return errors.New("board lease directory must be a private, nonsymlink directory")
	}
	encoded, err := json.Marshal(token)
	if err != nil {
		return errors.New("encode board lease token")
	}
	defer clear(encoded)
	temporary, err := os.CreateTemp(directory, ".lease-*.tmp")
	if err != nil {
		return errors.New("create temporary board lease token")
	}
	tempPath := temporary.Name()
	defer func() { _ = os.Remove(tempPath) }()
	if err := temporary.Chmod(0o600); err != nil {
		_ = temporary.Close()
		return errors.New("protect temporary board lease token")
	}
	if _, err := temporary.Write(encoded); err != nil {
		_ = temporary.Close()
		return errors.New("write board lease token")
	}
	if err := temporary.Sync(); err != nil {
		_ = temporary.Close()
		return errors.New("sync board lease token")
	}
	if err := temporary.Close(); err != nil {
		return errors.New("close board lease token")
	}
	destination := filepath.Join(directory, token.BoardID+".json")
	if err := os.Rename(tempPath, destination); err != nil {
		return errors.New("atomically store board lease token")
	}
	return nil
}

func readBoardLeaseToken(directory, boardID string) (boardclient.LeaseToken, error) {
	if !validBoardIDArgument(boardID) {
		return boardclient.LeaseToken{}, errors.New("invalid board ID")
	}
	info, err := os.Lstat(directory)
	if err != nil || !info.IsDir() || info.Mode()&os.ModeSymlink != 0 || info.Mode().Perm()&0o077 != 0 {
		return boardclient.LeaseToken{}, errors.New("board lease directory is unavailable or unsafe")
	}
	path := filepath.Join(directory, boardID+".json")
	info, err = os.Lstat(path)
	if err != nil || !info.Mode().IsRegular() || info.Mode()&os.ModeSymlink != 0 ||
		info.Size() <= 0 || info.Size() > 4096 || info.Mode().Perm()&0o077 != 0 {
		return boardclient.LeaseToken{}, errors.New("board lease token is unavailable or unsafe")
	}
	file, err := os.Open(path)
	if err != nil {
		return boardclient.LeaseToken{}, errors.New("open board lease token")
	}
	defer file.Close()
	opened, err := file.Stat()
	if err != nil || !opened.Mode().IsRegular() || !os.SameFile(info, opened) ||
		opened.Size() <= 0 || opened.Size() > 4096 || opened.Mode().Perm()&0o077 != 0 {
		return boardclient.LeaseToken{}, errors.New("board lease token changed or became unsafe")
	}
	raw, err := io.ReadAll(io.LimitReader(file, 4097))
	if err != nil || len(raw) == 0 || len(raw) > 4096 {
		return boardclient.LeaseToken{}, errors.New("read bounded board lease token")
	}
	defer clear(raw)
	decoder := json.NewDecoder(bytes.NewReader(raw))
	decoder.DisallowUnknownFields()
	var token boardclient.LeaseToken
	if err := decoder.Decode(&token); err != nil {
		return boardclient.LeaseToken{}, errors.New("decode board lease token")
	}
	var trailing any
	if err := decoder.Decode(&trailing); err != io.EOF {
		return boardclient.LeaseToken{}, errors.New("board lease token has trailing data")
	}
	if token.BoardID != boardID || !store.ValidID(token.RequestID) || !store.ValidID(token.LeaseID) ||
		token.Generation == 0 || token.Version == 0 || token.ExpiresAt.IsZero() {
		return boardclient.LeaseToken{}, errors.New("board lease token does not match its board")
	}
	return token, nil
}

type boardLeaseCheckpointer interface {
	Checkpoint(context.Context, boardclient.LeaseToken) (board.Snapshot, error)
}

// checkpointBoardLease moves a yielded holder into drain state after it has
// reached an application-defined safe point. It does not claim the hardware
// is neutral or release the lease.
func checkpointBoardLease(ctx context.Context, client boardLeaseCheckpointer, directory, boardID string) (board.Snapshot, error) {
	if ctx == nil || client == nil {
		return board.Snapshot{}, errors.New("board checkpoint requires context and client")
	}
	token, err := readBoardLeaseToken(directory, boardID)
	if err != nil {
		return board.Snapshot{}, err
	}
	snapshot, err := client.Checkpoint(ctx, token)
	if err != nil {
		return board.Snapshot{}, err
	}
	if snapshot.BoardID != token.BoardID || snapshot.Phase != board.Draining || snapshot.Lease == nil ||
		snapshot.Lease.ID != token.LeaseID || snapshot.Lease.WaiterID != token.RequestID ||
		snapshot.Lease.Generation != token.Generation {
		return board.Snapshot{}, errors.New("server returned a checkpoint for another board lease or phase")
	}
	return snapshot, nil
}

type boardLeaseExtender interface {
	Extend(context.Context, boardclient.LeaseToken, time.Time, string) (board.Snapshot, error)
}

func extendBoardLease(ctx context.Context, client boardLeaseExtender, directory, boardID string,
	expiry time.Time, why string) (board.Snapshot, error) {
	if ctx == nil || client == nil {
		return board.Snapshot{}, errors.New("board lease extension requires context and client")
	}
	token, err := readBoardLeaseToken(directory, boardID)
	if err != nil {
		return board.Snapshot{}, err
	}
	snapshot, err := client.Extend(ctx, token, expiry, why)
	if err != nil {
		return board.Snapshot{}, err
	}
	if snapshot.BoardID != token.BoardID || snapshot.Lease == nil || snapshot.Lease.ID != token.LeaseID ||
		snapshot.Lease.WaiterID != token.RequestID || snapshot.Lease.Generation != token.Generation {
		return board.Snapshot{}, errors.New("server returned an extension for another board lease")
	}
	token.ExpiresAt = snapshot.Lease.ExpiresAt
	token.Version = snapshot.Version
	if err := writeBoardLeaseToken(directory, token); err != nil {
		return board.Snapshot{}, fmt.Errorf("lease was extended but local token could not be updated: %w", err)
	}
	return snapshot, nil
}
