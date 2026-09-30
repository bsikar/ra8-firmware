//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Build graph for `ra8_core`.
//!
//! Twelve seams of this library are Zig so far: the freestanding runtime
//! primitives (#2820) and the deterministic `rand()` / `srand()` override
//! that joins them (#2890), the pin-claim validator (#2825), the SysTick timebase
//! with its time-interface binding (#2830), the log backend with
//! `ra8_err_to_str` (#2836), the millisecond tick counter, delay policy and
//! SysTick IRQ body (#2851), the decompression-limits policy every archive
//! and stream decoder charges against (#2862) and the fault block: the
//! exception reporter, the cross-reset crash log and the SCB register window
//! (#2868) and the error sink pair: the weak fatal trap every failed
//! `RA8_ASSERT` lands on, plus the log-backed non-fatal sink (#2875) and
//! the application-layer bring-up with its stack-canary sentinel (#2884) and
//! the newlib `_sbrk` heap trap (#2895) and the startup SDRAM zero-fill
//! (#2901).
//! Everything else in `src/` is still C, which
//! `.github/zig-parallel-tree-allowlist.tsv` records per file.
//!
//! WHAT THIS LIBRARY SHIPS DEPENDS ON WHO LINKS IT.
//!
//! A HOST build gets TWO archives, and the split is the point.
//!
//! `ra8_core` holds the freestanding primitives alone. Its exported names are
//! the bare standard ones an image needs (`memcpy`, `memset`, `strlen`,
//! `abs`, `rand`), so it cannot be linked into a host test binary, which
//! already has a real libc defining every one of them. `-Dabi-prefix=ra8_`
//! renames that whole surface for the two host suites that do test it,
//! `tests/core/src/test_ra8_freestanding.c` and
//! `tests/core/src/test_ra8_rand_stub.c`.
//!
//! `ra8_core_zig` holds every other ported TU. Those export ordinary `ra8_*`
//! names that collide with nothing, so `tests/cmake/zig_libraries.cmake`
//! links it into every host test the way it links the other migrated
//! libraries. New ra8_core slices belong here; only a libc-named primitive
//! belongs in the other one.
//!
//! A FREESTANDING build gets ONE archive, `ra8_core`, holding both roots
//! (`src/image_root.zig`). An image has no libc, so it needs the bare names
//! and the ports together and nothing it links defines either twice; the host
//! hazard the split exists for is not present there.
//!
//! The one archive is also what makes the ports REACHABLE from an image.
//! `_ra8_zig_build_archive()` in cmake/ra8_app/zig_libs.cmake names a
//! cross-built archive `lib<lib>.a`, so `ra8_link_zig_library_for_cpu(LIB
//! ra8_core)` can only ever fetch `libra8_core.a`. Applying the host split to
//! an image build leaves every cross-target consumer looking at the
//! freestanding half alone, so an image could not take an ra8_core port at
//! all: the three ARM images that still compile `ra8_log.c` by path had no
//! archive to move to until these two were composed.
//!
//! `bundle_compiler_rt` is OFF on both. Zig's compiler_rt carries its own
//! `memcpy` / `memset` / `memmove` / `memcmp`: in the freestanding archive
//! that would double-define the names this archive exports itself, and in the
//! general archive it would drag libc names into every host test link. The
//! firmware links compiler_rt from the other Zig archives.

const std = @import("std");

/// Units under `src/internal/freestanding/`. Each is its own module so the
/// tests can import the same module objects the archive does.
const freestanding_units = [_][]const u8{ "mem", "str", "math", "rand" };

pub fn build(b: *std.Build) void {
    const target = b.standardTargetOptions(.{});
    const optimize = b.standardOptimizeOption(.{});
    const abi_prefix = b.option(
        []const u8,
        "abi-prefix",
        "Prefix for the freestanding archive's exported C symbols (\"ra8_\" for the host suite, empty for an image)",
    ) orelse "";

    const build_options = b.addOptions();
    build_options.addOption([]const u8, "abi_prefix", abi_prefix);

    const test_step = b.step("test", "Run Zig ra8_core tests");

    // ---- the freestanding archive -------------------------------------
    var freestanding_modules = std.StringHashMap(*std.Build.Module).init(b.allocator);
    inline for (freestanding_units) |unit| {
        const module = b.createModule(.{
            .root_source_file = b.path(b.fmt("src/internal/freestanding/{s}.zig", .{unit})),
            .target = target,
            .optimize = optimize,
        });
        freestanding_modules.put(unit, module) catch @panic("OOM");
    }

    const freestanding_abi = b.createModule(.{
        .root_source_file = b.path("src/freestanding_abi.zig"),
        .target = target,
        .optimize = optimize,
    });
    freestanding_abi.addOptions("build_options", build_options);
    inline for (freestanding_units) |unit| {
        freestanding_abi.addImport(b.fmt("freestanding_{s}", .{unit}), freestanding_modules.get(unit).?);
    }

    const freestanding_root = b.createModule(.{
        .root_source_file = b.path("src/freestanding_root.zig"),
        .target = target,
        .optimize = optimize,
    });
    freestanding_root.addImport("freestanding_abi", freestanding_abi);

    // A freestanding target takes the one composed archive below instead, so
    // only a host build installs the split halves.
    const image_build = target.result.os.tag == .freestanding;

    if (!image_build) {
        const freestanding_library = b.addLibrary(.{
            .name = "ra8_core",
            .linkage = .static,
            .root_module = freestanding_root,
        });
        freestanding_library.bundle_compiler_rt = false;
        b.installArtifact(freestanding_library);
    }

    const freestanding_tests = b.createModule(.{
        .root_source_file = b.path("tests/freestanding_test.zig"),
        .target = target,
        .optimize = optimize,
    });
    inline for (freestanding_units) |unit| {
        freestanding_tests.addImport(b.fmt("freestanding_{s}", .{unit}), freestanding_modules.get(unit).?);
    }
    test_step.dependOn(&b.addRunArtifact(b.addTest(.{ .root_module = freestanding_tests })).step);

    // ---- the general archive ------------------------------------------
    const pin_validator_registry = b.createModule(.{
        .root_source_file = b.path("src/internal/pin_validator/registry.zig"),
        .target = target,
        .optimize = optimize,
    });

    const pin_validator_abi = b.createModule(.{
        .root_source_file = b.path("src/pin_validator_abi.zig"),
        .target = target,
        .optimize = optimize,
    });
    pin_validator_abi.addImport("pin_validator_registry", pin_validator_registry);

    const systick_reload = b.createModule(.{
        .root_source_file = b.path("src/internal/systick/reload.zig"),
        .target = target,
        .optimize = optimize,
    });

    const systick_regs = b.createModule(.{
        .root_source_file = b.path("src/internal/systick/regs.zig"),
        .target = target,
        .optimize = optimize,
    });

    const systick_abi = b.createModule(.{
        .root_source_file = b.path("src/systick_abi.zig"),
        .target = target,
        .optimize = optimize,
    });
    systick_abi.addImport("systick_reload", systick_reload);
    systick_abi.addImport("systick_regs", systick_regs);

    const time_interface_systick_abi = b.createModule(.{
        .root_source_file = b.path("src/time_interface_systick_abi.zig"),
        .target = target,
        .optimize = optimize,
    });

    const time_units = [_][]const u8{ "reload", "tick", "cpu", "delay", "hooks" };
    var time_modules_by_unit = std.StringHashMap(*std.Build.Module).init(b.allocator);
    inline for (time_units) |unit| {
        const module = b.createModule(.{
            .root_source_file = b.path(b.fmt("src/internal/time/{s}.zig", .{unit})),
            .target = target,
            .optimize = optimize,
        });
        time_modules_by_unit.put(unit, module) catch @panic("OOM");
    }
    const time_cpu = time_modules_by_unit.get("cpu").?;
    time_modules_by_unit.get("delay").?.addImport("time_cpu", time_cpu);
    time_modules_by_unit.get("delay").?.addImport("time_tick", time_modules_by_unit.get("tick").?);
    time_modules_by_unit.get("hooks").?.addImport("time_cpu", time_cpu);

    const time_abi = b.createModule(.{
        .root_source_file = b.path("src/time_abi.zig"),
        .target = target,
        .optimize = optimize,
    });
    inline for (time_units) |unit| {
        time_abi.addImport(b.fmt("time_{s}", .{unit}), time_modules_by_unit.get(unit).?);
    }

    const log_format = b.createModule(.{
        .root_source_file = b.path("src/internal/log/format.zig"),
        .target = target,
        .optimize = optimize,
    });

    const log_err_names = b.createModule(.{
        .root_source_file = b.path("src/internal/log/err_names.zig"),
        .target = target,
        .optimize = optimize,
    });

    const log_itm = b.createModule(.{
        .root_source_file = b.path("src/internal/log/itm.zig"),
        .target = target,
        .optimize = optimize,
    });

    const log_line = b.createModule(.{
        .root_source_file = b.path("src/internal/log/line.zig"),
        .target = target,
        .optimize = optimize,
    });
    log_line.addImport("log_format", log_format);

    const log_abi = b.createModule(.{
        .root_source_file = b.path("src/log_abi.zig"),
        .target = target,
        .optimize = optimize,
    });
    log_abi.addImport("log_itm", log_itm);
    log_abi.addImport("log_line", log_line);
    log_abi.addImport("log_err_names", log_err_names);

    const decomp_units = [_][]const u8{ "policy", "ratio", "budget", "zip_eocd" };
    var decomp_modules_by_unit = std.StringHashMap(*std.Build.Module).init(b.allocator);
    inline for (decomp_units) |unit| {
        const module = b.createModule(.{
            .root_source_file = b.path(b.fmt("src/internal/decomp/{s}.zig", .{unit})),
            .target = target,
            .optimize = optimize,
        });
        decomp_modules_by_unit.put(unit, module) catch @panic("OOM");
    }
    const decomp_policy = decomp_modules_by_unit.get("policy").?;
    const decomp_ratio = decomp_modules_by_unit.get("ratio").?;
    decomp_ratio.addImport("decomp_policy", decomp_policy);
    decomp_modules_by_unit.get("budget").?.addImport("decomp_policy", decomp_policy);
    decomp_modules_by_unit.get("budget").?.addImport("decomp_ratio", decomp_ratio);

    const decomp_abi = b.createModule(.{
        .root_source_file = b.path("src/decomp_abi.zig"),
        .target = target,
        .optimize = optimize,
    });
    inline for (decomp_units) |unit| {
        decomp_abi.addImport(b.fmt("decomp_{s}", .{unit}), decomp_modules_by_unit.get(unit).?);
    }

    // The fault block ports as one unit: the exception reporter, the crash
    // log it persists through, and the SCB window both read. Splitting them
    // would put one record layout across a language boundary at the moment
    // the system is already broken.
    const fault_units = [_][]const u8{ "scb", "record", "crc32", "crashlog", "halt" };
    var fault_modules_by_unit = std.StringHashMap(*std.Build.Module).init(b.allocator);
    inline for (fault_units) |unit| {
        const module = b.createModule(.{
            .root_source_file = b.path(b.fmt("src/internal/fault/{s}.zig", .{unit})),
            .target = target,
            .optimize = optimize,
        });
        fault_modules_by_unit.put(unit, module) catch @panic("OOM");
    }
    const fault_record = fault_modules_by_unit.get("record").?;
    fault_modules_by_unit.get("crashlog").?.addImport("fault_record", fault_record);
    fault_modules_by_unit.get("crashlog").?.addImport("fault_crc32", fault_modules_by_unit.get("crc32").?);

    // The error sink pair. `ra8_fatal_error` is where every failed check
    // ends and `g_ra8_error_sink_log` is the non-fatal counterpart; the
    // allow-list already paired them, because both reach the same log
    // backend and neither is meaningful without the other's contract.
    const error_units = [_][]const u8{ "fatal", "sink" };
    var error_modules_by_unit = std.StringHashMap(*std.Build.Module).init(b.allocator);
    inline for (error_units) |unit| {
        const module = b.createModule(.{
            .root_source_file = b.path(b.fmt("src/internal/error/{s}.zig", .{unit})),
            .target = target,
            .optimize = optimize,
        });
        error_modules_by_unit.put(unit, module) catch @panic("OOM");
    }

    const error_abis = [_][]const u8{ "error_handler_abi", "error_sink_abi" };
    var error_abi_modules = std.StringHashMap(*std.Build.Module).init(b.allocator);
    inline for (error_abis) |name| {
        const module = b.createModule(.{
            .root_source_file = b.path(b.fmt("src/{s}.zig", .{name})),
            .target = target,
            .optimize = optimize,
        });
        inline for (error_units) |unit| {
            module.addImport(b.fmt("error_{s}", .{unit}), error_modules_by_unit.get(unit).?);
        }
        error_abi_modules.put(name, module) catch @panic("OOM");
    }

    // Application-layer bring-up. One internal unit, because the canary
    // region is the only thing here with any logic in it; the order of the
    // three bring-up calls is the membrane's own contract.
    const infrastructure_canary = b.createModule(.{
        .root_source_file = b.path("src/internal/infrastructure/canary.zig"),
        .target = target,
        .optimize = optimize,
    });

    const infrastructure_abi = b.createModule(.{
        .root_source_file = b.path("src/infrastructure_abi.zig"),
        .target = target,
        .optimize = optimize,
    });
    infrastructure_abi.addImport("infrastructure_canary", infrastructure_canary);

    // The newlib heap trap. No internal logic to split out: the policy
    // constants are the unit, and the membrane is three lines on top of
    // them. It exports bare `_sbrk` from THIS archive rather than the
    // freestanding one, because it calls `ra8_fatal_error` and that archive
    // is a self-contained libc subset with no ra8_* dependency.
    const heap_sbrk = b.createModule(.{
        .root_source_file = b.path("src/internal/heap/sbrk.zig"),
        .target = target,
        .optimize = optimize,
    });

    const sbrk_trap_abi = b.createModule(.{
        .root_source_file = b.path("src/sbrk_trap_abi.zig"),
        .target = target,
        .optimize = optimize,
    });
    sbrk_trap_abi.addImport("heap_sbrk", heap_sbrk);

    // The startup zero-fill for `.sdram_data`. One internal unit: the
    // half-open span rule and the byte fill, plus the target/host split over
    // where the section actually is, which is the same shape the stack
    // canary uses.
    const boot_region = b.createModule(.{
        .root_source_file = b.path("src/internal/boot/region.zig"),
        .target = target,
        .optimize = optimize,
    });

    const boot_region_abi = b.createModule(.{
        .root_source_file = b.path("src/boot_region_abi.zig"),
        .target = target,
        .optimize = optimize,
    });
    boot_region_abi.addImport("boot_region", boot_region);

    const fault_abis = [_][]const u8{ "scb_abi", "exception_abi", "crashlog_abi" };
    var fault_abi_modules = std.StringHashMap(*std.Build.Module).init(b.allocator);
    inline for (fault_abis) |name| {
        const module = b.createModule(.{
            .root_source_file = b.path(b.fmt("src/{s}.zig", .{name})),
            .target = target,
            .optimize = optimize,
        });
        inline for (fault_units) |unit| {
            module.addImport(b.fmt("fault_{s}", .{unit}), fault_modules_by_unit.get(unit).?);
        }
        fault_abi_modules.put(name, module) catch @panic("OOM");
    }

    const root = b.createModule(.{
        .root_source_file = b.path("src/root.zig"),
        .target = target,
        .optimize = optimize,
    });
    root.addImport("pin_validator_abi", pin_validator_abi);
    root.addImport("systick_abi", systick_abi);
    root.addImport("time_interface_systick_abi", time_interface_systick_abi);
    root.addImport("time_abi", time_abi);
    root.addImport("log_abi", log_abi);
    root.addImport("decomp_abi", decomp_abi);
    inline for (fault_abis) |name| {
        root.addImport(name, fault_abi_modules.get(name).?);
    }
    inline for (error_abis) |name| {
        root.addImport(name, error_abi_modules.get(name).?);
    }
    root.addImport("infrastructure_abi", infrastructure_abi);
    root.addImport("sbrk_trap_abi", sbrk_trap_abi);
    root.addImport("boot_region_abi", boot_region_abi);

    if (!image_build) {
        const library = b.addLibrary(.{
            .name = "ra8_core_zig",
            .linkage = .static,
            .root_module = root,
        });
        library.bundle_compiler_rt = false;
        b.installArtifact(library);
    } else {
        const image_root = b.createModule(.{
            .root_source_file = b.path("src/image_root.zig"),
            .target = target,
            .optimize = optimize,
        });
        image_root.addImport("freestanding_abi", freestanding_abi);
        image_root.addImport("pin_validator_abi", pin_validator_abi);
        image_root.addImport("systick_abi", systick_abi);
        image_root.addImport("time_interface_systick_abi", time_interface_systick_abi);
        image_root.addImport("time_abi", time_abi);
        image_root.addImport("log_abi", log_abi);
        image_root.addImport("decomp_abi", decomp_abi);
        inline for (fault_abis) |name| {
            image_root.addImport(name, fault_abi_modules.get(name).?);
        }
        inline for (error_abis) |name| {
            image_root.addImport(name, error_abi_modules.get(name).?);
        }
        image_root.addImport("infrastructure_abi", infrastructure_abi);
        image_root.addImport("sbrk_trap_abi", sbrk_trap_abi);
        image_root.addImport("boot_region_abi", boot_region_abi);

        const image_library = b.addLibrary(.{
            .name = "ra8_core",
            .linkage = .static,
            .root_module = image_root,
        });
        image_library.bundle_compiler_rt = false;
        b.installArtifact(image_library);
    }

    const pin_validator_tests = b.createModule(.{
        .root_source_file = b.path("tests/pin_validator_test.zig"),
        .target = target,
        .optimize = optimize,
    });
    pin_validator_tests.addImport("pin_validator_registry", pin_validator_registry);
    test_step.dependOn(&b.addRunArtifact(b.addTest(.{ .root_module = pin_validator_tests })).step);

    const systick_tests = b.createModule(.{
        .root_source_file = b.path("tests/systick_test.zig"),
        .target = target,
        .optimize = optimize,
    });
    systick_tests.addImport("systick_reload", systick_reload);
    test_step.dependOn(&b.addRunArtifact(b.addTest(.{ .root_module = systick_tests })).step);

    const time_tests = b.createModule(.{
        .root_source_file = b.path("tests/time_test.zig"),
        .target = target,
        .optimize = optimize,
    });
    inline for (time_units) |unit| {
        time_tests.addImport(b.fmt("time_{s}", .{unit}), time_modules_by_unit.get(unit).?);
    }
    test_step.dependOn(&b.addRunArtifact(b.addTest(.{ .root_module = time_tests })).step);

    const log_tests = b.createModule(.{
        .root_source_file = b.path("tests/log_test.zig"),
        .target = target,
        .optimize = optimize,
    });
    log_tests.addImport("log_format", log_format);
    log_tests.addImport("log_line", log_line);
    log_tests.addImport("log_err_names", log_err_names);
    test_step.dependOn(&b.addRunArtifact(b.addTest(.{ .root_module = log_tests })).step);

    const decomp_tests = b.createModule(.{
        .root_source_file = b.path("tests/decomp_test.zig"),
        .target = target,
        .optimize = optimize,
    });
    inline for (decomp_units) |unit| {
        decomp_tests.addImport(b.fmt("decomp_{s}", .{unit}), decomp_modules_by_unit.get(unit).?);
    }
    test_step.dependOn(&b.addRunArtifact(b.addTest(.{ .root_module = decomp_tests })).step);

    const fault_tests = b.createModule(.{
        .root_source_file = b.path("tests/fault_test.zig"),
        .target = target,
        .optimize = optimize,
    });
    inline for (fault_units) |unit| {
        fault_tests.addImport(b.fmt("fault_{s}", .{unit}), fault_modules_by_unit.get(unit).?);
    }
    test_step.dependOn(&b.addRunArtifact(b.addTest(.{ .root_module = fault_tests })).step);

    const error_tests = b.createModule(.{
        .root_source_file = b.path("tests/error_test.zig"),
        .target = target,
        .optimize = optimize,
    });
    inline for (error_units) |unit| {
        error_tests.addImport(b.fmt("error_{s}", .{unit}), error_modules_by_unit.get(unit).?);
    }
    test_step.dependOn(&b.addRunArtifact(b.addTest(.{ .root_module = error_tests })).step);

    const infrastructure_tests = b.createModule(.{
        .root_source_file = b.path("tests/infrastructure_test.zig"),
        .target = target,
        .optimize = optimize,
    });
    infrastructure_tests.addImport("infrastructure_canary", infrastructure_canary);
    test_step.dependOn(&b.addRunArtifact(b.addTest(.{ .root_module = infrastructure_tests })).step);

    const heap_tests = b.createModule(.{
        .root_source_file = b.path("tests/heap_test.zig"),
        .target = target,
        .optimize = optimize,
    });
    heap_tests.addImport("heap_sbrk", heap_sbrk);
    test_step.dependOn(&b.addRunArtifact(b.addTest(.{ .root_module = heap_tests })).step);

    const boot_tests = b.createModule(.{
        .root_source_file = b.path("tests/boot_test.zig"),
        .target = target,
        .optimize = optimize,
    });
    boot_tests.addImport("boot_region", boot_region);
    test_step.dependOn(&b.addRunArtifact(b.addTest(.{ .root_module = boot_tests })).step);
}
