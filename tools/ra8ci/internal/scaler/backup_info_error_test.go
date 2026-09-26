// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package scaler

import (
	"testing"
	"time"
)

// pgBackRest flags a backup whose copy finished with file-level failures by
// setting "error" on it. Such a backup still carries type "full" and a stop
// timestamp, so nothing in the timestamps distinguishes it from a good one.

const (
	cleanFullBackup   = `{"type":"full","error":false,"timestamp":{"start":1786942240,"stop":1786942241}}`
	erroredFullBackup = `{"type":"full","error":true,"timestamp":{"start":1787028640,"stop":1787028641}}`
)

func backupInfo(backups ...string) []byte {
	raw := `[{"name":"ra8ci","backup":[`
	for index, backup := range backups {
		if index > 0 {
			raw += ","
		}
		raw += backup
	}
	return []byte(raw + `]}]`)
}

func TestErroredFullBackupIsNotTakenAsEvidence(t *testing.T) {
	got, err := ParseLatestFullBackupInfo(backupInfo(cleanFullBackup, erroredFullBackup), "ra8ci")
	want := time.Unix(1786942241, 0).UTC()
	if err != nil || !got.Equal(want) {
		t.Fatalf("ParseLatestFullBackupInfo = %s, %v; want the older clean full %s", got, err, want)
	}
}

func TestEveryFullBackupErroredIsRefused(t *testing.T) {
	if _, err := ParseLatestFullBackupInfo(backupInfo(erroredFullBackup), "ra8ci"); err == nil {
		t.Fatal("a stanza whose only full backup errored was accepted as evidence")
	}
}

func TestExplicitlyCleanFullBackupIsAccepted(t *testing.T) {
	got, err := ParseLatestFullBackupInfo(backupInfo(cleanFullBackup), "ra8ci")
	want := time.Unix(1786942241, 0).UTC()
	if err != nil || !got.Equal(want) {
		t.Fatalf("ParseLatestFullBackupInfo = %s, %v; want %s", got, err, want)
	}
}

// Older pgBackRest builds omit the field entirely. An absent flag is not a
// claim either way, so those responses must keep working exactly as before.
func TestFullBackupWithNoErrorFieldIsStillAccepted(t *testing.T) {
	raw := backupInfo(`{"type":"full","timestamp":{"start":1786942240,"stop":1786942241}}`)
	got, err := ParseLatestFullBackupInfo(raw, "ra8ci")
	want := time.Unix(1786942241, 0).UTC()
	if err != nil || !got.Equal(want) {
		t.Fatalf("ParseLatestFullBackupInfo = %s, %v; want %s", got, err, want)
	}
}

// A failed copy can stop before it records a stop timestamp. That used to
// fail the whole response, hiding the good backups behind it; the errored
// one is now skipped before the timestamp is demanded of it.
func TestErroredFullBackupWithNoStopDoesNotHideAGoodOne(t *testing.T) {
	raw := backupInfo(cleanFullBackup, `{"type":"full","error":true,"timestamp":{}}`)
	got, err := ParseLatestFullBackupInfo(raw, "ra8ci")
	want := time.Unix(1786942241, 0).UTC()
	if err != nil || !got.Equal(want) {
		t.Fatalf("ParseLatestFullBackupInfo = %s, %v; want %s", got, err, want)
	}
}

// The existing rule is unchanged for a backup pgBackRest has not flagged:
// no stop timestamp on an unflagged full still fails the whole response.
func TestUnflaggedFullBackupWithNoStopStillFailsClosed(t *testing.T) {
	raw := backupInfo(cleanFullBackup, `{"type":"full","timestamp":{}}`)
	if _, err := ParseLatestFullBackupInfo(raw, "ra8ci"); err == nil {
		t.Fatal("an unflagged full backup with no stop timestamp was accepted")
	}
}

func TestNonBooleanErrorFieldFailsClosed(t *testing.T) {
	raw := backupInfo(`{"type":"full","error":"maybe","timestamp":{"stop":1786942241}}`)
	if _, err := ParseLatestFullBackupInfo(raw, "ra8ci"); err == nil {
		t.Fatal("an unreadable error flag was accepted")
	}
}

// A diff or incremental backup is skipped before the flag is read, so an
// errored one does not change what the stanza's full backups report.
func TestErroredIncrementalDoesNotAffectTheFullBackup(t *testing.T) {
	raw := backupInfo(cleanFullBackup, `{"type":"incr","error":true,"timestamp":{"stop":1787028641}}`)
	got, err := ParseLatestFullBackupInfo(raw, "ra8ci")
	want := time.Unix(1786942241, 0).UTC()
	if err != nil || !got.Equal(want) {
		t.Fatalf("ParseLatestFullBackupInfo = %s, %v; want %s", got, err, want)
	}
}
