// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package hilpolicy

import (
	"encoding/json"
	"math"
	"testing"
	"time"
)

// steady is five ordinary observations settling around 24s.
func steady() []Observation {
	return []Observation{
		{Duration: 10 * time.Second}, {Duration: 12 * time.Second},
		{Duration: 14 * time.Second}, {Duration: 20 * time.Second},
		{Duration: 24 * time.Second},
	}
}

// flat is count observations of one duration, so the evidence the estimator
// demands is exactly that duration and the fixtures stay readable.
func flat(seconds float64, count int) []Observation {
	samples := make([]Observation, 0, count)
	for i := 0; i < count; i++ {
		samples = append(samples, Observation{Duration: time.Duration(seconds * float64(time.Second))})
	}
	return samples
}

// spread is a bench whose slowest run sits far above its mean, so the headroom
// the estimator adds carries the demand past MaximumSeconds. A flat bench
// cannot: identical durations leave both stddev and max-mean at zero.
func spread() []Observation {
	return []Observation{
		{Duration: 1000 * time.Second}, {Duration: 1500 * time.Second},
		{Duration: 2000 * time.Second}, {Duration: 2500 * time.Second},
		{Duration: 3500 * time.Second},
	}
}

func TestEvidenceThatChoseTheBoundIsRecorded(t *testing.T) {
	decision, err := Choose(0, false, steady())
	if err != nil {
		t.Fatalf("err = %v", err)
	}
	if decision.Source != "observed" {
		t.Fatalf("source = %q", decision.Source)
	}
	if decision.RaisedToFallback || decision.ClippedToMaximum {
		t.Fatalf("nothing overruled this evidence: %+v", decision)
	}
	if decision.EvidenceSeconds <= 0 {
		t.Fatalf("evidence_seconds = %v, want what the observations demanded", decision.EvidenceSeconds)
	}
	if decision.Seconds != int(math.Ceil(decision.EvidenceSeconds)) {
		t.Fatalf("seconds = %d, want ceil(%v)", decision.Seconds, decision.EvidenceSeconds)
	}
}

func TestADeclaredFloorOverAQuietBenchSaysSo(t *testing.T) {
	decision, err := Choose(600, true, steady())
	if err != nil {
		t.Fatalf("err = %v", err)
	}
	if decision.Seconds != 600 {
		t.Fatalf("seconds = %d, want the declared floor", decision.Seconds)
	}
	if !decision.RaisedToFallback {
		t.Fatal("the declared fallback chose the bound and the record does not say so")
	}
	if decision.ClippedToMaximum {
		t.Fatal("nothing was clipped here")
	}
	if decision.EvidenceSeconds >= 600 {
		t.Fatalf("evidence_seconds = %v, want the smaller number the observations actually demanded", decision.EvidenceSeconds)
	}
	if decision.MaximumObservedSeconds != 24 {
		t.Fatalf("maximum_observed_seconds = %v, want the evidence left intact beside the floor", decision.MaximumObservedSeconds)
	}
}

// The costly direction: the bound comes back BELOW what the observations asked
// for, so the record has to carry the demand that was cut.
func TestABoundCutByTheCeilingSaysSoAndKeepsTheDemand(t *testing.T) {
	decision, err := Choose(0, false, spread())
	if err != nil {
		t.Fatalf("err = %v", err)
	}
	if decision.Seconds != MaximumSeconds {
		t.Fatalf("seconds = %d, want the ceiling", decision.Seconds)
	}
	if !decision.ClippedToMaximum {
		t.Fatal("the ceiling chose the bound and the record does not say so")
	}
	if decision.RaisedToFallback {
		t.Fatal("no floor was involved")
	}
	if decision.EvidenceSeconds <= MaximumSeconds {
		t.Fatalf("evidence_seconds = %v, want the demand that exceeded the ceiling", decision.EvidenceSeconds)
	}
}

func TestCensoredHeadroomIsPartOfTheDemandThatIsRecorded(t *testing.T) {
	samples := flat(100, MinimumSamples)
	samples[len(samples)-1].TimedOut = true
	decision, err := Choose(0, false, samples)
	if err != nil {
		t.Fatalf("err = %v", err)
	}
	if decision.Censored != 1 {
		t.Fatalf("censored = %d", decision.Censored)
	}
	if decision.EvidenceSeconds != 150 {
		t.Fatalf("evidence_seconds = %v, want the 1.5x a right-censored sample demands", decision.EvidenceSeconds)
	}
	if decision.RaisedToFallback || decision.ClippedToMaximum {
		t.Fatalf("nothing overruled this evidence: %+v", decision)
	}
}

func TestNeitherBoundIsClaimedOnAnExactMatch(t *testing.T) {
	decision, err := Choose(100, true, flat(100, MinimumSamples))
	if err != nil {
		t.Fatalf("err = %v", err)
	}
	if decision.Seconds != 100 || decision.RaisedToFallback {
		t.Fatalf("a floor equal to the evidence did not choose anything: %+v", decision)
	}
	decision, err = Choose(0, false, flat(MaximumSeconds, MinimumSamples))
	if err != nil {
		t.Fatalf("err = %v", err)
	}
	if decision.Seconds != MaximumSeconds || decision.ClippedToMaximum {
		t.Fatalf("evidence landing exactly on the ceiling was not cut by it: %+v", decision)
	}
}

func TestAFallbackDecisionCarriesNoEvidenceAtAll(t *testing.T) {
	for _, item := range []struct {
		name     string
		declared int
		found    bool
		want     string
	}{
		{"no declaration at all", 0, false, "default"},
		{"a declaration and too little history", 240, true, "hil.conf"},
	} {
		t.Run(item.name, func(t *testing.T) {
			decision, err := Choose(item.declared, item.found, []Observation{{Duration: time.Second}})
			if err != nil {
				t.Fatalf("err = %v", err)
			}
			if decision.Source != item.want {
				t.Fatalf("source = %q, want %q", decision.Source, item.want)
			}
			if decision.EvidenceSeconds != 0 || decision.RaisedToFallback || decision.ClippedToMaximum {
				t.Fatalf("a fallback has no evidence to overrule: %+v", decision)
			}
		})
	}
}

// A declared fallback is bounded by MaximumSeconds before it reaches the bound,
// so a bound raised to the floor is under the ceiling by construction.
func TestTheTwoOverrulesAreNeverBothClaimed(t *testing.T) {
	declared := []int{1, 30, 240, 600, 1800, 3599, MaximumSeconds}
	observed := []float64{1, 12, 100, 1200, 3000, 3600}
	for _, fallback := range declared {
		for _, seconds := range observed {
			decision, err := Choose(fallback, true, flat(seconds, MinimumSamples))
			if err != nil {
				t.Fatalf("declared %d over %vs: %v", fallback, seconds, err)
			}
			if decision.RaisedToFallback && decision.ClippedToMaximum {
				t.Fatalf("declared %d over %vs claims both overrules: %+v", fallback, seconds, decision)
			}
			if decision.Seconds < 1 || decision.Seconds > MaximumSeconds {
				t.Fatalf("declared %d over %vs: seconds = %d, out of bounds", fallback, seconds, decision.Seconds)
			}
		}
	}
}

// Recording what chose the bound must not move the bound.
func TestTheChosenSecondsAreWhatTheyAlwaysWere(t *testing.T) {
	cases := []struct {
		name     string
		declared int
		found    bool
		samples  []Observation
		want     int
	}{
		{"steady bench, no declaration", 0, false, steady(), 32},
		{"steady bench under a 600s declaration", 600, true, steady(), 600},
		{"a bench whose headroom runs past the ceiling", 0, false, spread(), MaximumSeconds},
		{"a flat bench well inside it", 0, false, flat(100, MinimumSamples), 100},
		{"one sample short of the minimum", 45, true, flat(100, MinimumSamples-1), 45},
	}
	for _, item := range cases {
		t.Run(item.name, func(t *testing.T) {
			decision, err := Choose(item.declared, item.found, item.samples)
			if err != nil {
				t.Fatalf("err = %v", err)
			}
			if decision.Seconds != item.want {
				t.Fatalf("seconds = %d, want %d", decision.Seconds, item.want)
			}
		})
	}
}

func TestTheBoundHelperRoundsUpAfterTheCeilingIsApplied(t *testing.T) {
	seconds, raised, clipped := boundFromEvidence(99.1, 0)
	if seconds != 100 || raised || clipped {
		t.Fatalf("seconds=%d raised=%v clipped=%v, want a rounded-up 100 chosen by the evidence", seconds, raised, clipped)
	}
	seconds, raised, clipped = boundFromEvidence(MaximumSeconds+0.4, 0)
	if seconds != MaximumSeconds || raised || !clipped {
		t.Fatalf("seconds=%d raised=%v clipped=%v, want the ceiling, not a rounded-up demand past it", seconds, raised, clipped)
	}
}

// The record is read as JSON, so the names have to be there when they mean
// something and gone when they do not.
func TestTheRecordReadsAsJSON(t *testing.T) {
	clipped, err := Choose(0, false, spread())
	if err != nil {
		t.Fatalf("err = %v", err)
	}
	body, err := json.Marshal(clipped)
	if err != nil {
		t.Fatalf("marshal: %v", err)
	}
	read := map[string]any{}
	if err := json.Unmarshal(body, &read); err != nil {
		t.Fatalf("unmarshal: %v", err)
	}
	if read["clipped_to_maximum"] != true {
		t.Fatalf("clipped_to_maximum missing from %s", body)
	}
	if _, ok := read["evidence_seconds"]; !ok {
		t.Fatalf("evidence_seconds missing from %s", body)
	}
	if _, ok := read["raised_to_fallback"]; ok {
		t.Fatalf("raised_to_fallback claimed on a clipped decision: %s", body)
	}
	fallback, err := Choose(240, true, nil)
	if err != nil {
		t.Fatalf("err = %v", err)
	}
	body, err = json.Marshal(fallback)
	if err != nil {
		t.Fatalf("marshal: %v", err)
	}
	read = map[string]any{}
	if err := json.Unmarshal(body, &read); err != nil {
		t.Fatalf("unmarshal: %v", err)
	}
	for _, name := range []string{"evidence_seconds", "raised_to_fallback", "clipped_to_maximum"} {
		if _, ok := read[name]; ok {
			t.Fatalf("%s on a fallback decision: %s", name, body)
		}
	}
}
