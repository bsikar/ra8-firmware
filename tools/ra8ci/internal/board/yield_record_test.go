package board

import (
	"testing"
	"time"
)

// yieldedBoard drives a CI lease into yield_requested by queueing a human
// behind it, and returns the snapshot plus the moment the yield was asked.
func yieldedBoard(t *testing.T) (Snapshot, time.Time) {
	t.Helper()
	s := boardForTest(t)
	s, _ = applyForTest(t, s, request("ci-one", ClassCI), testEpoch)
	s = ack(t, s, testEpoch.Add(time.Second))
	asked := testEpoch.Add(2 * time.Second)
	s, _ = applyForTest(t, s, request("human-one", ClassHuman), asked)
	if s.Phase != YieldRequested || s.Lease.YieldRequestedAt != asked {
		t.Fatalf("board did not enter yield_requested: %#v", s.Lease)
	}
	return s, asked
}

func TestYieldSampleMeasuresRequestToNeutral(t *testing.T) {
	s, asked := yieldedBoard(t)
	neutral := asked.Add(18 * time.Second)
	before := s
	_, events := applyForTest(t, s, Release{
		Actor: before.Lease.Holder, LeaseID: before.Lease.ID,
		Generation: before.Generation, NeutralReceipt: "neutral-proof",
	}, neutral)

	sample, ok, err := YieldSampleFor(before, events, testCohort(), 30*time.Second)
	if err != nil || !ok {
		t.Fatalf("no sample for a completed handoff: %v %v", ok, err)
	}
	if !sample.Completed() || sample.Latency() != 18*time.Second {
		t.Fatalf("latency = %s, want 18s", sample.Latency())
	}
	if sample.LeaseID != before.Lease.ID || sample.WaiterID != before.Lease.WaiterID {
		t.Fatalf("sample is not identified by its lease: %#v", sample)
	}
	if sample.SafetyOverrun || sample.ExclusionReason != "" {
		t.Fatalf("clean handoff recorded as an exception: %#v", sample)
	}

	// The same handoff against a shorter shown estimate is an overrun, and
	// stays a measured sample rather than being censored.
	sample, _, err = YieldSampleFor(before, events, testCohort(), 15*time.Second)
	if err != nil || !sample.SafetyOverrun || !sample.Completed() {
		t.Fatalf("overrun not flagged: %#v %v", sample, err)
	}
	// Exactly at the target is not an overrun.
	sample, _, err = YieldSampleFor(before, events, testCohort(), 18*time.Second)
	if err != nil || sample.SafetyOverrun {
		t.Fatalf("on-target handoff flagged as overrun: %#v %v", sample, err)
	}
}

func TestYieldSampleRetainsAFailedHandoffWithoutMeasuringIt(t *testing.T) {
	cases := []struct {
		name    string
		command Command
		at      time.Duration
		want    string
	}{
		{"no receipt", nil, 18 * time.Second, YieldExcludedNoReceipt},
		{"expired", Tick{Actor: "server"}, 25 * time.Minute, YieldExcludedExpired},
		{"quarantined", nil, 18 * time.Second, YieldExcludedQuarantine},
	}
	for _, tc := range cases {
		s, asked := yieldedBoard(t)
		before := s
		command := tc.command
		switch tc.name {
		case "no receipt":
			command = Release{Actor: before.Lease.Holder, LeaseID: before.Lease.ID, Generation: before.Generation}
		case "quarantined":
			command = Quarantine{Actor: "operator", Reason: "fixture smoke"}
		}
		_, events := applyForTest(t, s, command, asked.Add(tc.at))

		sample, ok, err := YieldSampleFor(before, events, testCohort(), 30*time.Second)
		if err != nil || !ok {
			t.Fatalf("%s: no sample retained: %v %v", tc.name, ok, err)
		}
		if sample.ExclusionReason != tc.want {
			t.Fatalf("%s: exclusion = %q, want %q", tc.name, sample.ExclusionReason, tc.want)
		}
		if sample.Completed() || !sample.NeutralAt.IsZero() || sample.Latency() != 0 {
			t.Fatalf("%s: censored row carries a latency: %#v", tc.name, sample)
		}
		if sample.RequestedAt != asked || sample.LeaseID != before.Lease.ID {
			t.Fatalf("%s: censored row lost its request: %#v", tc.name, sample)
		}

		// A censored row must not reach the estimator's sample floor however
		// many of them there are.
		history := make([]YieldSample, 0, MinimumHandoffSamples+1)
		for i := 0; i <= MinimumHandoffSamples; i++ {
			history = append(history, sample)
		}
		estimate, err := EstimateHandoff(testCohort(), testBounds(), history, asked.Add(time.Hour))
		if err != nil {
			t.Fatalf("%s: %v", tc.name, err)
		}
		if estimate.Samples != 0 || estimate.Censored != len(history) || estimate.Source != HandoffFromDeclaredBounds {
			t.Fatalf("%s: censored rows were measured: %#v", tc.name, estimate)
		}
	}
}

func TestYieldSampleRecordsAWithdrawnRequest(t *testing.T) {
	s, asked := yieldedBoard(t)
	before := s
	// The human who triggered the yield gives up; the holder keeps the board
	// and there is no handoff to measure, but the request happened.
	_, events := applyForTest(t, s, CancelWaiter{Actor: "server", WaiterID: "human-one"}, asked.Add(4*time.Second))

	sample, ok, err := YieldSampleFor(before, events, testCohort(), 0)
	if err != nil || !ok || sample.ExclusionReason != YieldExcludedWithdrawn {
		t.Fatalf("withdrawn yield not retained: %#v %v %v", sample, ok, err)
	}
}

func TestYieldSampleMeasuresNothingWithoutAYieldOrATerminalEvent(t *testing.T) {
	s := boardForTest(t)
	s, _ = applyForTest(t, s, request("ci-one", ClassCI), testEpoch)
	s = ack(t, s, testEpoch.Add(time.Second))
	before := s
	// Released with nobody waiting: no yield was ever requested.
	_, events := applyForTest(t, s, Release{
		Actor: before.Lease.Holder, LeaseID: before.Lease.ID,
		Generation: before.Generation, NeutralReceipt: "neutral-proof",
	}, testEpoch.Add(2*time.Second))
	if _, ok, err := YieldSampleFor(before, events, testCohort(), 0); ok || err != nil {
		t.Fatalf("sample invented without a yield request: %v %v", ok, err)
	}

	// A yield is outstanding but this transition did not end it.
	yielded, asked := yieldedBoard(t)
	before = yielded
	_, events = applyForTest(t, yielded, BeginDrain{
		Actor: before.Lease.Holder, LeaseID: before.Lease.ID, Generation: before.Generation,
	}, asked.Add(time.Second))
	if _, ok, err := YieldSampleFor(before, events, testCohort(), 0); ok || err != nil {
		t.Fatalf("drain closed a handoff it did not finish: %v %v", ok, err)
	}

	// Another lease's terminal event is not this lease's handoff.
	foreign := []Event{{Kind: LeaseReleased, At: asked.Add(time.Second), LeaseID: "lease-someone-else"}}
	if _, ok, err := YieldSampleFor(before, foreign, testCohort(), 0); ok || err != nil {
		t.Fatalf("another lease closed this handoff: %v %v", ok, err)
	}
}

func TestYieldSampleRefusesAContradictoryTransition(t *testing.T) {
	s, asked := yieldedBoard(t)
	before := s
	_, events := applyForTest(t, s, Release{
		Actor: before.Lease.Holder, LeaseID: before.Lease.ID,
		Generation: before.Generation, NeutralReceipt: "neutral-proof",
	}, asked.Add(10*time.Second))

	// A board that reached neutral before the yield that asked for it is a
	// contradiction, not a zero-second handoff.
	impossible := before
	lease := *before.Lease
	lease.YieldRequestedAt = asked.Add(time.Minute)
	impossible.Lease = &lease
	if _, _, err := YieldSampleFor(impossible, events, testCohort(), 0); !IsCode(err, InvalidArgument) {
		t.Fatalf("neutral before the request accepted: %v", err)
	}

	if _, _, err := YieldSampleFor(before, events, YieldCohort{BoardID: "b"}, 0); !IsCode(err, InvalidArgument) {
		t.Fatalf("incomplete cohort accepted: %v", err)
	}
	if _, _, err := YieldSampleFor(before, events, testCohort(), MaxHandoffBound+time.Second); !IsCode(err, InvalidArgument) {
		t.Fatalf("out-of-range shown target accepted: %v", err)
	}
}
