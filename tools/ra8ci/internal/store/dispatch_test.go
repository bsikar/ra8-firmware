// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package store

import (
	"testing"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/protocol"
)

func TestValidPartialStepEvidence(t *testing.T) {
	tests := []struct {
		name    string
		receipt protocol.TerminalReceipt
		want    bool
	}{
		{name: "complete prefix after nonzero child", receipt: protocol.TerminalReceipt{EvidenceComplete: true,
			Steps: []protocol.StepSummary{{ExitCode: 7}}}, want: true},
		{name: "deadline between steps", receipt: protocol.TerminalReceipt{EvidenceComplete: true, TimedOut: true,
			Steps: []protocol.StepSummary{{ExitCode: 0}}}, want: true},
		{name: "cancel between steps", receipt: protocol.TerminalReceipt{EvidenceComplete: true, Cancelled: true,
			Steps: []protocol.StepSummary{{ExitCode: 0}}}, want: true},
		{name: "omitted successful suffix", receipt: protocol.TerminalReceipt{EvidenceComplete: true,
			Steps: []protocol.StepSummary{{ExitCode: 0}}}, want: false},
		{name: "incomplete executor evidence", receipt: protocol.TerminalReceipt{EvidenceComplete: false,
			Steps: []protocol.StepSummary{{ExitCode: 0}}}, want: true},
		{name: "empty complete prefix", receipt: protocol.TerminalReceipt{EvidenceComplete: true}, want: false},
		{name: "all steps present", receipt: protocol.TerminalReceipt{EvidenceComplete: true,
			Steps: []protocol.StepSummary{{}, {}}}, want: true},
	}
	for _, test := range tests {
		t.Run(test.name, func(t *testing.T) {
			if got := validPartialStepEvidence(test.receipt, 2); got != test.want {
				t.Fatalf("validPartialStepEvidence() = %t, want %t", got, test.want)
			}
		})
	}
}
