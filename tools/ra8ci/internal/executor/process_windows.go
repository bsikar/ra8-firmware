//go:build windows

// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package executor

import (
	"context"
	"errors"
	"fmt"
	"io"
	"os"
	"runtime"
	"sort"
	"strings"
	"syscall"
	"time"
	"unsafe"
)

const (
	createSuspended                   = 0x00000004
	jobObjectExtendedLimitInformation = 9
	jobObjectLimitKillOnJobClose      = 0x00002000
	extendedStartupInfoPresent        = 0x00080000
	procThreadAttributeHandleList     = 0x00020002
	processPollInterval               = 50 * time.Millisecond
	hardStopWait                      = 5 * time.Second
)

// The syscall package does not expose the Job Object APIs, but it does expose
// CreateProcess and its primary thread handle. Keeping that handle is essential:
// os/exec closes it before Cmd.Start returns, so a suspended Cmd cannot be
// assigned to a job and then resumed without another process-creation path.
var (
	kernel32                     = syscall.NewLazyDLL("kernel32.dll")
	procCreateJobObjectW         = kernel32.NewProc("CreateJobObjectW")
	procSetInformationJobObject  = kernel32.NewProc("SetInformationJobObject")
	procAssignProcessToJobObject = kernel32.NewProc("AssignProcessToJobObject")
	procTerminateJobObject       = kernel32.NewProc("TerminateJobObject")
	procResumeThread             = kernel32.NewProc("ResumeThread")
	procInitializeAttributeList  = kernel32.NewProc("InitializeProcThreadAttributeList")
	procUpdateThreadAttribute    = kernel32.NewProc("UpdateProcThreadAttribute")
	procDeleteAttributeList      = kernel32.NewProc("DeleteProcThreadAttributeList")
	procGenerateConsoleCtrlEvent = kernel32.NewProc("GenerateConsoleCtrlEvent")
)

type jobBasicLimitInformation struct {
	PerProcessUserTimeLimit int64
	PerJobUserTimeLimit     int64
	LimitFlags              uint32
	MinimumWorkingSetSize   uintptr
	MaximumWorkingSetSize   uintptr
	ActiveProcessLimit      uint32
	Affinity                uintptr
	PriorityClass           uint32
	SchedulingClass         uint32
}

type jobIOCounters struct {
	ReadOperationCount  uint64
	WriteOperationCount uint64
	OtherOperationCount uint64
	ReadTransferCount   uint64
	WriteTransferCount  uint64
	OtherTransferCount  uint64
}

type jobExtendedLimitInformation struct {
	BasicLimitInformation jobBasicLimitInformation
	IOInfo                jobIOCounters
	ProcessMemoryLimit    uintptr
	JobMemoryLimit        uintptr
	PeakProcessMemoryUsed uintptr
	PeakJobMemoryUsed     uintptr
}

type startupInfoEx struct {
	syscall.StartupInfo
	AttributeList uintptr
}

func runCommand(ctx context.Context, program string, args []string, root string, env []string, stdout, stderr io.Writer, grace time.Duration) (result commandResult, runErr error) {
	result.ExitCode = -1
	if err := contextExpiration(ctx); err != nil {
		result.TimedOut = errors.Is(err, context.DeadlineExceeded)
		result.Cancelled = !result.TimedOut
		return result, nil
	}
	job, err := createKillJob()
	if err != nil {
		return result, fmt.Errorf("create process job: %w", err)
	}
	defer func() {
		if job != 0 {
			_ = syscall.CloseHandle(job)
		}
	}()

	stdoutReader, stdoutWriter, err := commandPipe()
	if err != nil {
		return result, fmt.Errorf("create stdout pipe: %w", err)
	}
	defer stdoutReader.Close()
	defer closeHandle(&stdoutWriter)
	stderrReader, stderrWriter, err := commandPipe()
	if err != nil {
		return result, fmt.Errorf("create stderr pipe: %w", err)
	}
	defer stderrReader.Close()
	defer closeHandle(&stderrWriter)

	nullInput, err := os.Open(os.DevNull)
	if err != nil {
		return result, fmt.Errorf("open null stdin: %w", err)
	}
	defer nullInput.Close()
	processHandle, err := syscall.GetCurrentProcess()
	if err != nil {
		return result, fmt.Errorf("get current process: %w", err)
	}
	var childInput syscall.Handle
	if err := syscall.DuplicateHandle(processHandle, syscall.Handle(nullInput.Fd()), processHandle, &childInput, 0, true, syscall.DUPLICATE_SAME_ACCESS); err != nil {
		return result, fmt.Errorf("duplicate stdin handle: %w", err)
	}
	defer closeHandle(&childInput)

	application, commandLine, directory, environment, err := processParameters(program, args, root, env)
	if err != nil {
		return result, err
	}
	inherited := []syscall.Handle{childInput, stdoutWriter, stderrWriter}
	attributeList, releaseAttributes, err := makeHandleAttributeList(inherited)
	if err != nil {
		return result, fmt.Errorf("restrict inherited handles: %w", err)
	}
	defer releaseAttributes()
	startup := startupInfoEx{StartupInfo: syscall.StartupInfo{
		Cb:        uint32(unsafe.Sizeof(startupInfoEx{})),
		Flags:     syscall.STARTF_USESTDHANDLES,
		StdInput:  childInput,
		StdOutput: stdoutWriter,
		StdErr:    stderrWriter,
	}, AttributeList: attributeList}
	var process syscall.ProcessInformation
	flags := uint32(createSuspended | syscall.CREATE_NEW_PROCESS_GROUP | syscall.CREATE_UNICODE_ENVIRONMENT | extendedStartupInfoPresent)
	if err := syscall.CreateProcess(application, commandLine, nil, nil, true, flags, &environment[0], directory, &startup.StartupInfo, &process); err != nil {
		return result, fmt.Errorf("create suspended process: %w", err)
	}
	runtime.KeepAlive(startup)
	defer syscall.CloseHandle(process.Process)
	defer closeHandle(&process.Thread)
	if err := assignProcessToJob(job, process.Process); err != nil {
		_ = syscall.TerminateProcess(process.Process, 1)
		_, _ = syscall.WaitForSingleObject(process.Process, uint32(hardStopWait/time.Millisecond))
		return result, fmt.Errorf("assign suspended process to job: %w", err)
	}
	if err := contextExpiration(ctx); err != nil {
		result.TimedOut = errors.Is(err, context.DeadlineExceeded)
		result.Cancelled = !result.TimedOut
		_ = terminateJob(job)
		return result, nil
	}
	if err := resumeProcess(process.Thread); err != nil {
		_ = terminateJob(job)
		return result, fmt.Errorf("resume job process: %w", err)
	}
	closeHandle(&process.Thread)
	closeHandle(&childInput)
	closeHandle(&stdoutWriter)
	closeHandle(&stderrWriter)

	stdoutDone := copyCommandOutput(stdout, stdoutReader)
	stderrDone := copyCommandOutput(stderr, stderrReader)
	exited, err := waitForProcess(ctx, process.Process)
	if err != nil {
		runErr = errors.Join(runErr, fmt.Errorf("wait for process: %w", err))
	}
	// Give an already-expired task context precedence if process and timer
	// completion became observable together under scheduler pressure.
	if cause := contextExpiration(ctx); exited && cause != nil {
		result.TimedOut = errors.Is(cause, context.DeadlineExceeded)
		result.Cancelled = !result.TimedOut
	}
	if cause := contextExpiration(ctx); !exited && cause != nil {
		result.TimedOut = errors.Is(cause, context.DeadlineExceeded)
		result.Cancelled = !result.TimedOut
		if grace > 0 && sendCtrlBreak(process.ProcessId) == nil {
			exited, err = waitForProcessFor(process.Process, grace)
			runErr = errors.Join(runErr, err)
		}
	}
	if !exited {
		if err := terminateJob(job); err != nil {
			runErr = errors.Join(runErr, fmt.Errorf("terminate process job: %w", err))
		}
		exited, err = waitForProcessFor(process.Process, hardStopWait)
		runErr = errors.Join(runErr, err)
	}
	if exited {
		var exitCode uint32
		if err := syscall.GetExitCodeProcess(process.Process, &exitCode); err != nil {
			runErr = errors.Join(runErr, fmt.Errorf("read process exit: %w", err))
		} else {
			result.ExitCode = int(exitCode)
		}
	} else {
		runErr = errors.Join(runErr, errors.New("process did not exit after hard stop"))
	}
	if err := syscall.CloseHandle(job); err != nil {
		runErr = errors.Join(runErr, fmt.Errorf("close process job: %w", err))
	}
	job = 0
	if !exited {
		_ = stdoutReader.Close()
		_ = stderrReader.Close()
	}
	runErr = errors.Join(runErr, <-stdoutDone, <-stderrDone)
	return result, runErr
}

func commandPipe() (*os.File, syscall.Handle, error) {
	attributes := syscall.SecurityAttributes{
		Length:        uint32(unsafe.Sizeof(syscall.SecurityAttributes{})),
		InheritHandle: 1,
	}
	var reader, writer syscall.Handle
	if err := syscall.CreatePipe(&reader, &writer, &attributes, 0); err != nil {
		return nil, 0, err
	}
	if err := syscall.SetHandleInformation(reader, syscall.HANDLE_FLAG_INHERIT, 0); err != nil {
		_ = syscall.CloseHandle(reader)
		_ = syscall.CloseHandle(writer)
		return nil, 0, err
	}
	return os.NewFile(uintptr(reader), "ra8ci-command-output"), writer, nil
}

func processParameters(program string, args []string, root string, env []string) (*uint16, *uint16, *uint16, []uint16, error) {
	application, err := syscall.UTF16PtrFromString(program)
	if err != nil {
		return nil, nil, nil, nil, fmt.Errorf("invalid program path: %w", err)
	}
	parts := make([]string, 0, len(args)+1)
	parts = append(parts, syscall.EscapeArg(program))
	for _, arg := range args {
		parts = append(parts, syscall.EscapeArg(arg))
	}
	commandLine, err := syscall.UTF16PtrFromString(strings.Join(parts, " "))
	if err != nil {
		return nil, nil, nil, nil, fmt.Errorf("invalid command line: %w", err)
	}
	directory, err := syscall.UTF16PtrFromString(root)
	if err != nil {
		return nil, nil, nil, nil, fmt.Errorf("invalid working directory: %w", err)
	}
	ordered := append([]string(nil), env...)
	for _, item := range ordered {
		if !strings.Contains(item, "=") || strings.ContainsRune(item, '\x00') {
			return nil, nil, nil, nil, errors.New("invalid process environment")
		}
	}
	sort.SliceStable(ordered, func(i, j int) bool {
		left, _, _ := strings.Cut(ordered[i], "=")
		right, _, _ := strings.Cut(ordered[j], "=")
		return strings.ToUpper(left) < strings.ToUpper(right)
	})
	environment := syscall.StringToUTF16(strings.Join(ordered, "\x00") + "\x00")
	return application, commandLine, directory, environment, nil
}

func copyCommandOutput(output io.Writer, reader *os.File) <-chan error {
	done := make(chan error, 1)
	go func() {
		_, err := io.Copy(output, reader)
		_ = reader.Close()
		done <- err
	}()
	return done
}

func waitForProcess(ctx context.Context, process syscall.Handle) (bool, error) {
	for {
		event, err := syscall.WaitForSingleObject(process, uint32(processPollInterval/time.Millisecond))
		if err != nil {
			return false, err
		}
		if event == syscall.WAIT_OBJECT_0 {
			return true, nil
		}
		if event != syscall.WAIT_TIMEOUT {
			return false, fmt.Errorf("unexpected process wait event %d", event)
		}
		if contextExpiration(ctx) != nil {
			return false, nil
		}
	}
}

func waitForProcessFor(process syscall.Handle, duration time.Duration) (bool, error) {
	deadline := time.Now().Add(duration)
	for {
		remaining := time.Until(deadline)
		if remaining <= 0 {
			return false, nil
		}
		wait := min(remaining, processPollInterval)
		milliseconds := uint32((wait + time.Millisecond - 1) / time.Millisecond)
		event, err := syscall.WaitForSingleObject(process, milliseconds)
		if err != nil {
			return false, err
		}
		if event == syscall.WAIT_OBJECT_0 {
			return true, nil
		}
		if event != syscall.WAIT_TIMEOUT {
			return false, fmt.Errorf("unexpected process wait event %d", event)
		}
	}
}

func makeHandleAttributeList(handles []syscall.Handle) (uintptr, func(), error) {
	var size uintptr
	_, _, _ = procInitializeAttributeList.Call(0, 1, 0, uintptr(unsafe.Pointer(&size)))
	if size == 0 {
		return 0, nil, errors.New("Windows did not report attribute-list size")
	}
	wordSize := unsafe.Sizeof(uintptr(0))
	words := make([]uintptr, (size+wordSize-1)/wordSize)
	list := uintptr(unsafe.Pointer(&words[0]))
	if err := callWindowsBool(procInitializeAttributeList, list, 1, 0, uintptr(unsafe.Pointer(&size))); err != nil {
		return 0, nil, err
	}
	if err := callWindowsBool(procUpdateThreadAttribute, list, 0, procThreadAttributeHandleList,
		uintptr(unsafe.Pointer(&handles[0])), uintptr(len(handles))*unsafe.Sizeof(handles[0]), 0, 0); err != nil {
		_, _, _ = procDeleteAttributeList.Call(list)
		return 0, nil, err
	}
	release := func() {
		_, _, _ = procDeleteAttributeList.Call(list)
		runtime.KeepAlive(words)
		runtime.KeepAlive(handles)
	}
	return list, release, nil
}

func createKillJob() (syscall.Handle, error) {
	handle, _, callErr := procCreateJobObjectW.Call(0, 0)
	if handle == 0 {
		return 0, windowsCallError(callErr)
	}
	job := syscall.Handle(handle)
	limits := jobExtendedLimitInformation{}
	limits.BasicLimitInformation.LimitFlags = jobObjectLimitKillOnJobClose
	if err := callWindowsBool(procSetInformationJobObject, uintptr(job), jobObjectExtendedLimitInformation, uintptr(unsafe.Pointer(&limits)), unsafe.Sizeof(limits)); err != nil {
		_ = syscall.CloseHandle(job)
		return 0, err
	}
	return job, nil
}

func assignProcessToJob(job, process syscall.Handle) error {
	return callWindowsBool(procAssignProcessToJobObject, uintptr(job), uintptr(process))
}

func terminateJob(job syscall.Handle) error {
	return callWindowsBool(procTerminateJobObject, uintptr(job), 1)
}

func resumeProcess(thread syscall.Handle) error {
	previous, _, callErr := procResumeThread.Call(uintptr(thread))
	if uint32(previous) == ^uint32(0) {
		return windowsCallError(callErr)
	}
	return nil
}

func sendCtrlBreak(processID uint32) error {
	return callWindowsBool(procGenerateConsoleCtrlEvent, uintptr(syscall.CTRL_BREAK_EVENT), uintptr(processID))
}

func callWindowsBool(procedure *syscall.LazyProc, args ...uintptr) error {
	ok, _, callErr := procedure.Call(args...)
	if ok == 0 {
		return windowsCallError(callErr)
	}
	return nil
}

func windowsCallError(err error) error {
	if err != nil && !errors.Is(err, syscall.Errno(0)) {
		return err
	}
	return syscall.EINVAL
}

func closeHandle(handle *syscall.Handle) {
	if *handle != 0 {
		_ = syscall.CloseHandle(*handle)
		*handle = 0
	}
}
