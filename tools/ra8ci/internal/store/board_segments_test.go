package store

import "testing"

func TestValidSegmentKey(t *testing.T) {
	for _, tc := range []struct {
		key  string
		want bool
	}{
		{key: "flash", want: true},
		{key: "hil:emulator", want: true},
		{key: "", want: false},
		{key: "  ", want: false},
		{key: " padded ", want: false},
		{key: string([]byte{'b', 'a', 'd', '\n'}), want: false},
	} {
		if got := validSegmentKey(tc.key); got != tc.want {
			t.Errorf("validSegmentKey(%q) = %v, want %v", tc.key, got, tc.want)
		}
	}
}
