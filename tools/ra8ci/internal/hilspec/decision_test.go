package hilspec

import (
	"context"
	"errors"
	"strings"
	"testing"
	"time"
)

type historySource struct {
	rows  []HistoricalObservation
	err   error
	calls int
	seen  Workload
}

func (h *historySource) Observations(_ context.Context, workload Workload) ([]HistoricalObservation, error) {
	h.calls++
	h.seen = workload
	return h.rows, h.err
}

func decisionFixture(t *testing.T) (Spec, Workload, Options) {
	t.Helper()
	spec, err := Parse(strings.NewReader("HIL_MODE=uart_scrape\nHIL_TIMEOUT_S=12\nHIL_EXPECT=\"PASS\"\n"),
		"examples/ek_ra8d2/hw_validated/hil/demo/hil.conf")
	if err != nil {
		t.Fatal(err)
	}
	workload := Workload{ManifestPath: spec.Path, BoardModel: "RA8D2", FixtureRevision: "r3",
		ProfileSHA256: strings.Repeat("a", 64), ProgramFamily: "demo", Mode: spec.Mode}
	return spec, workload, Options{FlashRestoreBound: 8 * time.Second}
}

func TestDecisionUsesDeclaredFallbackUntilComparableHistoryExists(t *testing.T) {
	spec, key, options := decisionFixture(t)
	decision, err := Decide(context.Background(), spec, key, nil, options)
	if err != nil || decision.ValidityWindow != 12*time.Second || decision.FlashRestoreBound != 8*time.Second ||
		decision.MinimumLeaseBudget() != 20*time.Second || decision.Source != "hil.conf" {
		t.Fatalf("declared fallback wrong: %+v err=%v", decision, err)
	}
	source := &historySource{}
	for _, duration := range []time.Duration{3, 4, 5, 6} {
		source.rows = append(source.rows, HistoricalObservation{Workload: key, Duration: duration * time.Second,
			Succeeded: true, EvidenceComplete: true})
	}
	decision, err = Decide(context.Background(), spec, key, source, options)
	if err != nil || decision.ValidityWindow != 12*time.Second || decision.Samples != 4 || source.calls != 1 || source.seen != key {
		t.Fatalf("insufficient history overrode fallback: %+v err=%v", decision, err)
	}
	withoutTimeout, err := Parse(strings.NewReader("HIL_MODE=alive\n"), spec.Path)
	if err != nil {
		t.Fatal(err)
	}
	key.Mode = ModeAlive
	decision, err = Decide(context.Background(), withoutTimeout, key, nil, options)
	if err != nil || decision.ValidityWindow != 30*time.Second || decision.Source != "default" {
		t.Fatalf("missing declaration did not default to 30s: %+v err=%v", decision, err)
	}
}

func TestDecisionUsesConservativeEstimatorAndSeparatesRestoreBound(t *testing.T) {
	spec, key, options := decisionFixture(t)
	source := &historySource{}
	for _, seconds := range []int{10, 11, 12, 13, 20} {
		source.rows = append(source.rows, HistoricalObservation{Workload: key, Duration: time.Duration(seconds) * time.Second,
			Succeeded: true, EvidenceComplete: true})
	}
	decision, err := Decide(context.Background(), spec, key, source, options)
	if err != nil || decision.Source != "observed" || decision.ValidityWindow <= 20*time.Second ||
		decision.FlashRestoreBound != options.FlashRestoreBound || decision.Samples != 5 || decision.MaxSeconds != 20 {
		t.Fatalf("historical max/headroom not used: %+v err=%v", decision, err)
	}
	if decision.MinimumLeaseBudget() != decision.ValidityWindow+8*time.Second {
		t.Fatal("flash/restore was mixed into observation validity")
	}
}

func TestDecisionFiltersUncomparableFailedOrIncompleteRows(t *testing.T) {
	spec, key, options := decisionFixture(t)
	other := key
	other.ProfileSHA256 = strings.Repeat("b", 64)
	source := &historySource{rows: []HistoricalObservation{
		{Workload: key, Duration: 5 * time.Second, Succeeded: true, EvidenceComplete: true},
		{Workload: other, Duration: 1 * time.Second, Succeeded: true, EvidenceComplete: true},
		{Workload: key, Duration: 1 * time.Second, Succeeded: false, EvidenceComplete: true},
		{Workload: key, Duration: 1 * time.Second, Succeeded: true, EvidenceComplete: false},
		{Workload: key, Duration: 1 * time.Second, Succeeded: true, EvidenceComplete: true, TimedOut: true},
	}}
	decision, err := Decide(context.Background(), spec, key, source, options)
	if err != nil || decision.Samples != 1 || decision.RejectedRows != 4 || decision.ValidityWindow != 12*time.Second {
		t.Fatalf("bad rows contaminated estimate: %+v err=%v", decision, err)
	}
}

func TestDecisionClampsOnlyAnExplicitSafetyMaximum(t *testing.T) {
	spec, key, options := decisionFixture(t)
	spec.SafetyMaximumSeconds = 24
	options.SafetyMaximum = 22 * time.Second
	source := &historySource{}
	for _, seconds := range []int{10, 20, 21, 22, 23} {
		source.rows = append(source.rows, HistoricalObservation{Workload: key, Duration: time.Duration(seconds) * time.Second,
			Succeeded: true, EvidenceComplete: true})
	}
	decision, err := Decide(context.Background(), spec, key, source, options)
	if err != nil || decision.ValidityWindow != 22*time.Second || decision.SafetyMaximum != 22*time.Second ||
		decision.Source != "observed-capped" || decision.FlashRestoreBound != 8*time.Second {
		t.Fatalf("explicit cap was not enforced: %+v err=%v", decision, err)
	}
	options.SafetyMaximum = 10 * time.Second
	if _, err := Decide(context.Background(), spec, key, source, options); !errors.Is(err, ErrInvalidManifest) {
		t.Fatalf("cap below fallback was accepted: %v", err)
	}
}

func TestDecisionRejectsCorruptHistoryAndMissingRestoreBudget(t *testing.T) {
	spec, key, options := decisionFixture(t)
	options.FlashRestoreBound = 0
	if _, err := Decide(context.Background(), spec, key, nil, options); !errors.Is(err, ErrInvalidManifest) {
		t.Fatalf("missing recovery budget accepted: %v", err)
	}
	options.FlashRestoreBound = time.Second
	source := &historySource{err: errors.New("Postgres unavailable")}
	if _, err := Decide(context.Background(), spec, key, source, options); !errors.Is(err, source.err) {
		t.Fatalf("database failure silently treated as no history: %v", err)
	}
	source.err = nil
	source.rows = []HistoricalObservation{{Workload: key, Duration: -time.Second, Succeeded: true, EvidenceComplete: true}}
	if _, err := Decide(context.Background(), spec, key, source, options); !errors.Is(err, ErrInvalidHistory) {
		t.Fatalf("invalid successful duration accepted: %v", err)
	}
	source.rows = make([]HistoricalObservation, maxHistoryRows+1)
	if _, err := Decide(context.Background(), spec, key, source, options); !errors.Is(err, ErrInvalidHistory) {
		t.Fatalf("unbounded history accepted: %v", err)
	}
}
