// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package waverefs

import "testing"

func TestWavePolicyCases(t *testing.T) {
	cases := []struct {
		text string
		want bool
	}{
		{"Wave 70", true}, {"wave-3", true}, {"Wave_43b", true},
		{"waveform", false}, {"sine wave", false}, {"wave_table", false},
	}
	for _, item := range cases {
		if got := wavePattern.MatchString(item.text); got != item.want {
			t.Errorf("wavePattern(%q)=%v, want %v", item.text, got, item.want)
		}
	}
}
