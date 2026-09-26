// Package hilspec parses HIL manifests as data and chooses observation bounds.
// It does not source shell files, run commands, or touch the board.
package hilspec

import (
	"bufio"
	"bytes"
	"errors"
	"fmt"
	"io"
	"net/netip"
	"os"
	"path/filepath"
	"strconv"
	"strings"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/hilpolicy"
)

const (
	maxManifestBytes = 1 << 20
	maxLineBytes     = 64 << 10
)

var (
	ErrInvalidManifest = errors.New("invalid HIL manifest")
	ErrUnsafePath      = errors.New("HIL manifest escapes examples tree")
	ErrInvalidHistory  = errors.New("invalid HIL timing history")
)

type Mode string

const (
	ModeAlive              Mode = "alive"
	ModeUARTScrape         Mode = "uart_scrape"
	ModeRTTScrape          Mode = "rtt_scrape"
	ModeJLinkMemprobe      Mode = "jlink_memprobe"
	ModeEthernetTCP        Mode = "hil_eth_tcp"
	ModeC6CameraLivestream Mode = "c6_camera_livestream"
)

type Kind uint8

const (
	TextValue Kind = iota + 1
	NumberValue
	FlagValue
)

type Value struct {
	Kind   Kind
	Text   string
	Number int64
	Flag   bool
}

// Spec preserves every currently known HIL_* knob in a typed value map while
// promoting the timing and expected-output fields used by the scheduler.
type Spec struct {
	Path                 string
	Mode                 Mode
	Expect               string
	ExpectNegative       string
	TimeoutSeconds       int
	TimeoutDeclared      bool
	SafetyMaximumSeconds int
	BootTimeoutSeconds   int
	ProbeTimeoutSeconds  int
	BootSeconds          int
	ProbeSeconds         int
	ProbeBootSeconds     int
	Values               map[string]Value
}

// The allowlist mirrors scripts/hil/lib/hil_conf.sh plus an optional
// HIL_MAX_TIMEOUT_S safety cap for the new data-only orchestrator. Unknown
// variables cannot silently leak into any subprocess environment.
var schema = map[string]Kind{
	"HIL_MODE": TextValue, "HIL_EXPECT": TextValue, "HIL_EXPECT_NEGATIVE": TextValue,
	"HIL_EXPECT_SHORT_OK": FlagValue, "HIL_EXPECT_OVERLAP_OK": FlagValue,
	"HIL_PROVISION_WIFI": FlagValue, "HIL_TIMEOUT_S": NumberValue,
	"HIL_MAX_TIMEOUT_S": NumberValue, "HIL_EMU_ARGS": TextValue,
	"HIL_EMU_MAX_CHUNKS": NumberValue, "HIL_VIDPID": TextValue,
	"HIL_HUB_PORT": TextValue, "HIL_PPPS_MODE": TextValue,
	"HIL_MPS_CHUNK": NumberValue, "HIL_STREAM_BYTES": NumberValue,
	"HIL_STREAM_FLOOR_KBS": NumberValue, "HIL_BOOT_S": NumberValue,
	"HIL_FAULT_EXPECTED": FlagValue, "HIL_PROBE_SYMBOL": TextValue,
	"HIL_PROBE_MIN_ADVANCE": NumberValue, "HIL_PROBE_SECONDS": NumberValue,
	"HIL_PROBE_BOOT_S": NumberValue, "HIL_PROBE_FAILURE_SYMBOL": TextValue,
	"HIL_PROBE_MAX_FAILURE": NumberValue, "HIL_BOARD_IP": TextValue,
	"HIL_PORT": NumberValue, "HIL_PROTO": TextValue,
	"HIL_PAYLOAD_BYTES": NumberValue, "HIL_BOOT_TIMEOUT_S": NumberValue,
	"HIL_PROBE_TIMEOUT_S": NumberValue, "HIL_RTT_BUF_SYMBOL": TextValue,
	"HIL_RTT_BUF_BYTES": NumberValue, "HIL_SELF_BUILD": FlagValue,
	"HIL_FRAME_WIDTH": NumberValue, "HIL_FRAME_HEIGHT": NumberValue,
	"HIL_POST_INITIALIZE": FlagValue, "HIL_POST_POWER_CYCLE_HALT": FlagValue,
}

var durationKeys = map[string]bool{
	"HIL_TIMEOUT_S": true, "HIL_MAX_TIMEOUT_S": true,
	"HIL_BOOT_S": true, "HIL_PROBE_SECONDS": true, "HIL_PROBE_BOOT_S": true,
	"HIL_BOOT_TIMEOUT_S": true, "HIL_PROBE_TIMEOUT_S": true,
}

// Load reads only a regular hil.conf beneath root/examples after resolving
// symlinks. It refuses path traversal and symlink escapes before opening.
func Load(root, relativePath string) (Spec, error) {
	if filepath.IsAbs(relativePath) || filepath.Base(relativePath) != "hil.conf" ||
		strings.Contains(relativePath, "\\") {
		return Spec{}, ErrUnsafePath
	}
	clean := filepath.Clean(relativePath)
	if clean == "." || clean == ".." || strings.HasPrefix(clean, ".."+string(filepath.Separator)) ||
		!strings.HasPrefix(clean, "examples"+string(filepath.Separator)) {
		return Spec{}, ErrUnsafePath
	}
	base, err := filepath.EvalSymlinks(filepath.Join(root, "examples"))
	if err != nil {
		return Spec{}, err
	}
	path, err := filepath.EvalSymlinks(filepath.Join(root, clean))
	if err != nil {
		return Spec{}, err
	}
	if !strings.HasPrefix(path, base+string(filepath.Separator)) {
		return Spec{}, ErrUnsafePath
	}
	file, err := os.Open(path)
	if err != nil {
		return Spec{}, err
	}
	defer file.Close()
	info, err := file.Stat()
	if err != nil || !info.Mode().IsRegular() || info.Size() > maxManifestBytes {
		return Spec{}, ErrInvalidManifest
	}
	return Parse(file, clean)
}

// Parse accepts a deliberately small assignment grammar, not Bash. Literal
// quote delimiters are removed, but no expansion or command substitution is
// performed. Backslashes inside quoted regexes remain literal.
func Parse(reader io.Reader, path string) (Spec, error) {
	if reader == nil {
		return Spec{}, ErrInvalidManifest
	}
	rawManifest, err := io.ReadAll(io.LimitReader(reader, maxManifestBytes+1))
	if err != nil || len(rawManifest) > maxManifestBytes {
		return Spec{}, ErrInvalidManifest
	}
	spec := Spec{Path: path, Values: make(map[string]Value)}
	scanner := bufio.NewScanner(bytes.NewReader(rawManifest))
	scanner.Buffer(make([]byte, 4096), maxLineBytes)
	lineNumber := 0
	for scanner.Scan() {
		lineNumber++
		line := strings.TrimSpace(scanner.Text())
		if line == "" || strings.HasPrefix(line, "#") {
			continue
		}
		key, raw, found := strings.Cut(line, "=")
		key = strings.TrimSpace(key)
		kind, known := schema[key]
		if !found || !known || !validKey(key) {
			return Spec{}, fmt.Errorf("%w: %s:%d unknown assignment", ErrInvalidManifest, path, lineNumber)
		}
		if _, duplicate := spec.Values[key]; duplicate {
			return Spec{}, fmt.Errorf("%w: %s:%d duplicate %s", ErrInvalidManifest, path, lineNumber, key)
		}
		literal, err := parseLiteral(strings.TrimSpace(raw))
		if err != nil {
			return Spec{}, fmt.Errorf("%w: %s:%d %s: %v", ErrInvalidManifest, path, lineNumber, key, err)
		}
		value := Value{Kind: kind}
		switch kind {
		case TextValue:
			value.Text = literal
		case FlagValue:
			if literal != "0" && literal != "1" {
				return Spec{}, fmt.Errorf("%w: %s:%d %s must be 0 or 1", ErrInvalidManifest, path, lineNumber, key)
			}
			value.Flag = literal == "1"
		case NumberValue:
			number, err := strconv.ParseInt(literal, 10, 64)
			if err != nil || number < 0 || number > 1_000_000_000 ||
				(durationKeys[key] && (number < 1 || number > hilpolicy.MaximumSeconds)) ||
				(key == "HIL_PORT" && (number < 1 || number > 65535)) {
				return Spec{}, fmt.Errorf("%w: %s:%d invalid %s", ErrInvalidManifest, path, lineNumber, key)
			}
			value.Number = number
		default:
			return Spec{}, ErrInvalidManifest
		}
		if err := validateTypedValue(key, value); err != nil {
			return Spec{}, fmt.Errorf("%w: %s:%d %s: %v", ErrInvalidManifest, path, lineNumber, key, err)
		}
		spec.Values[key] = value
		switch key {
		case "HIL_MODE":
			spec.Mode = Mode(value.Text)
		case "HIL_EXPECT":
			spec.Expect = value.Text
		case "HIL_EXPECT_NEGATIVE":
			spec.ExpectNegative = value.Text
		case "HIL_TIMEOUT_S":
			spec.TimeoutSeconds, spec.TimeoutDeclared = int(value.Number), true
		case "HIL_MAX_TIMEOUT_S":
			spec.SafetyMaximumSeconds = int(value.Number)
		case "HIL_BOOT_TIMEOUT_S":
			spec.BootTimeoutSeconds = int(value.Number)
		case "HIL_PROBE_TIMEOUT_S":
			spec.ProbeTimeoutSeconds = int(value.Number)
		case "HIL_BOOT_S":
			spec.BootSeconds = int(value.Number)
		case "HIL_PROBE_SECONDS":
			spec.ProbeSeconds = int(value.Number)
		case "HIL_PROBE_BOOT_S":
			spec.ProbeBootSeconds = int(value.Number)
		}
	}
	if err := scanner.Err(); err != nil {
		return Spec{}, fmt.Errorf("%w: %v", ErrInvalidManifest, err)
	}
	if spec.Mode == "" {
		return Spec{}, fmt.Errorf("%w: %s has no HIL_MODE", ErrInvalidManifest, path)
	}
	if spec.SafetyMaximumSeconds > 0 && spec.TimeoutDeclared && spec.SafetyMaximumSeconds < spec.TimeoutSeconds {
		return Spec{}, fmt.Errorf("%w: safety maximum is below declared fallback", ErrInvalidManifest)
	}
	if err := checkPhasesFitTheSafetyCap(spec); err != nil {
		return Spec{}, err
	}
	return spec, nil
}

func validKey(key string) bool {
	if !strings.HasPrefix(key, "HIL_") {
		return false
	}
	for _, c := range key {
		if !((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_') {
			return false
		}
	}
	return true
}

func parseLiteral(raw string) (string, error) {
	if raw == "" || strings.ContainsAny(raw, "$`\r\n\x00") {
		return "", errors.New("empty value or shell interpolation is forbidden")
	}
	if strings.HasPrefix(raw, "\"") {
		if len(raw) < 2 || !strings.HasSuffix(raw, "\"") {
			return "", errors.New("unterminated quote")
		}
		value := raw[1 : len(raw)-1]
		for i := 0; i < len(value); i++ {
			if value[i] == '"' && (i == 0 || value[i-1] != '\\') {
				return "", errors.New("unescaped quote")
			}
		}
		return value, nil
	}
	if strings.ContainsAny(raw, " \t'\";|&<>(){}") {
		return "", errors.New("unquoted shell syntax is forbidden")
	}
	return raw, nil
}

func validateTypedValue(key string, value Value) error {
	switch key {
	case "HIL_MODE":
		switch Mode(value.Text) {
		case ModeAlive, ModeUARTScrape, ModeRTTScrape, ModeJLinkMemprobe, ModeEthernetTCP, ModeC6CameraLivestream:
		default:
			return errors.New("unsupported HIL mode")
		}
	case "HIL_BOARD_IP":
		if _, err := netip.ParseAddr(value.Text); err != nil {
			return errors.New("board IP is not a literal address")
		}
	case "HIL_PROTO":
		if value.Text != "tcp" && value.Text != "udp" {
			return errors.New("unsupported wire protocol")
		}
	case "HIL_PROBE_SYMBOL", "HIL_PROBE_FAILURE_SYMBOL", "HIL_RTT_BUF_SYMBOL":
		if !validSymbol(value.Text) {
			return errors.New("invalid probe symbol")
		}
	}
	return nil
}

func validSymbol(value string) bool {
	if value == "" || !((value[0] >= 'A' && value[0] <= 'Z') ||
		(value[0] >= 'a' && value[0] <= 'z') || value[0] == '_') {
		return false
	}
	for _, c := range value {
		if !((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
			(c >= '0' && c <= '9') || c == '_') {
			return false
		}
	}
	return true
}
