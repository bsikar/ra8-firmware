// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package scaler

import (
	"testing"
	"time"
)

func TestParseLatestFullBackupInfo(t *testing.T) {
	raw := []byte(`[{"name":"ra8ci","backup":[
        {"type":"full","timestamp":{"start":1786942240,"stop":1786942241}},
        {"type":"diff","timestamp":{"start":1786942242,"stop":1786942243}},
        {"type":"full","timestamp":{"start":1787028640,"stop":1787028641}}
    ]}]`)
	got, err := ParseLatestFullBackupInfo(raw, "ra8ci")
	want := time.Unix(1787028641, 0).UTC()
	if err != nil || !got.Equal(want) {
		t.Fatalf("ParseLatestFullBackupInfo = %s, %v; want %s", got, err, want)
	}
}

func TestParseLatestFullBackupInfoFailsClosed(t *testing.T) {
	tests := []struct{ name, raw, stanza string }{
		{"empty", `[]`, "ra8ci"},
		{"wrong stanza", `[{"name":"other","backup":[]}]`, "ra8ci"},
		{"no full", `[{"name":"ra8ci","backup":[{"type":"diff","timestamp":{"stop":1787028641}}]}]`, "ra8ci"},
		{"incomplete full", `[{"name":"ra8ci","backup":[{"type":"full","timestamp":{}}]}]`, "ra8ci"},
		{"invalid stop", `[{"name":"ra8ci","backup":[{"type":"full","timestamp":{"stop":"bad"}}]}]`, "ra8ci"},
		{"trailing value", `[{"name":"ra8ci","backup":[]}] {}`, "ra8ci"},
	}
	for _, test := range tests {
		t.Run(test.name, func(t *testing.T) {
			if _, err := ParseLatestFullBackupInfo([]byte(test.raw), test.stanza); err == nil {
				t.Fatal("invalid backup evidence was accepted")
			}
		})
	}
}
