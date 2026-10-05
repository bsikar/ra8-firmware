//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Build graph for `ra8_hal`'s Zig half (RA8FW-497).
//!
//! ra8_hal is mid-port: most of src/ is still C, globbed into the universal
//! set every app compiles. This archive carries the units that moved to Zig
//! and is linked beside those C objects as a universal archive, the same way
//! ra8_secure_app's is, so a unit joins the link by moving here and deleting
//! its .c with no other build edit. The headers in inc/ are unchanged.

const std = @import("std");
const ra8_build = @import("ra8_zig_build");

pub fn build(b: *std.Build) void {
    const target = b.standardTargetOptions(.{
        .default_target = ra8_build.hostDefaultTargetQuery(b),
    });
    const optimize = b.standardOptimizeOption(.{});
    // No unwind tables in a freestanding archive, as in ra8_core's: an
    // .ARM.exidx entry names __aeabi_unwind_cpp_pr0, which a -nostdlib CPU1
    // link (no -lgcc) cannot resolve (RA8FW-571).
    const unwind: ?std.builtin.UnwindTables = if (target.result.os.tag == .freestanding) .none else null;

    const library = b.addLibrary(.{
        .name = "ra8_hal",
        .linkage = .static,
        .root_module = b.createModule(.{
            .root_source_file = b.path("src/ra8_hal_abi.zig"),
            .target = target,
            .optimize = optimize,
            .pic = true,
            .unwind_tables = unwind,
        }),
    });
    library.bundle_compiler_rt = false;
    // One archive member per ported unit (RA8FW-542): the linker pulls only
    // the members an image references. Zig merges an object's string
    // literals into one .rodata.str1.1 that --gc-sections cannot split, so a
    // single shared object would carry every unit's log strings.
    const abi_units = [_][]const u8{ "eth", "canfd", "layer3_switch", "icu", "iwdt", "npu_quant", "glcdc_gamma", "elc", "epaper_devinfo", "eth_coma", "bscan", "eth_mfwd", "fuelgauge", "sram_security", "bkup_security", "lpm_graphics", "i3c_i2c_peripheral", "ether_phy", "mpc", "doc", "cac", "epaper_geom", "pwr", "sau", "ethosu_shim", "crc", "mipi_phy_ops", "spi_b_dma", "tsn", "acmphs", "sci_spi", "canfd_timing", "dotf_power", "usb_pvnd", "canfd_afl", "cgc_eswclk", "dac_b", "usb_pprn", "canfd_frame", "dtc", "eth_gptp", "etha_tas", "etha_stats", "sci_dma_isr", "ceu_init_regs", "usb_pmsc_scsi", "pdm", "spi_b_target", "ulpt", "poeg", "bkup_tamper", "bkup", "glcdc_layer", "ssie_stream", "ipc_sem_ring", "sdramc", "adc_selfdiag", "cache", "usb_paud", "eth_link", "smbus", "usb_cdc", "lvd_events", "mipi_csi_irq", "mipi_dsi_cmd", "mipi_csi_status", "mipi_csi_config", "mipi_csi_info", "mipi_csi_lifecycle", "mipi_dsi_lanes", "mipi_dsi_lifecycle", "mipi_dsi_status", "mipi_dsi_video", "mipi_dsi_dispatch", "fpu_probe", "mipi_dsi_command", "i3c_i2c_control", "i2c_status", "i2c_clock", "mstp_ids", "isr_globals", "i2c_config", "exit_stop", "forwarders", "dma", "usb_phid", "lvd_runtime", "rmac_phy", "rmac_mgmt", "dmac", "eth_gwca_queue", "reset", "sci_lin", "usb_host_bulk", "flash_irq", "usb_pmsc", "ble", "gpio_pins", "isr", "rmac_phy_drv", "gpio", "dual_core", "i2c_target", "usb_hhub", "usb_composite", "vreg", "rsip_protected", "touch", "sdcard", "lpm", "cnecc", "lvd", "sram", "pdg", "agt" };
    for (abi_units) |unit| {
        const object = b.addObject(.{
            .name = b.fmt("ra8_hal_{s}", .{unit}),
            .root_module = b.createModule(.{
                .root_source_file = b.path(b.fmt("src/{s}_abi.zig", .{unit})),
                .target = target,
                .optimize = optimize,
                .pic = true,
                .unwind_tables = unwind,
            }),
        });
        object.bundle_compiler_rt = false;
        object.link_function_sections = true;
        object.link_data_sections = true;
        library.addObject(object);
    }
    library.link_function_sections = true;
    library.link_data_sections = true;
    b.installArtifact(library);

    const test_step = b.step("test", "Run Zig ra8_hal tests");
    const units = [_]struct { name: []const u8, source: []const u8, root: []const u8 }{
        .{ .name = "eth_media", .source = "src/internal/eth_media.zig", .root = "tests/eth_media_test.zig" },
        .{ .name = "canfd_tdc", .source = "src/internal/canfd_tdc.zig", .root = "tests/canfd_tdc_test.zig" },
        .{ .name = "layer3_switch", .source = "src/internal/layer3_switch.zig", .root = "tests/layer3_switch_test.zig" },
        .{ .name = "icu", .source = "src/internal/icu.zig", .root = "tests/icu_test.zig" },
        .{ .name = "iwdt", .source = "src/internal/iwdt.zig", .root = "tests/iwdt_test.zig" },
        .{ .name = "npu_quant", .source = "src/internal/npu_quant.zig", .root = "tests/npu_quant_test.zig" },
        .{ .name = "glcdc_gamma", .source = "src/internal/glcdc_gamma.zig", .root = "tests/glcdc_gamma_test.zig" },
        .{ .name = "elc", .source = "src/internal/elc.zig", .root = "tests/elc_test.zig" },
        .{ .name = "epaper_devinfo", .source = "src/internal/epaper_devinfo.zig", .root = "tests/epaper_devinfo_test.zig" },
        .{ .name = "eth_coma", .source = "src/internal/eth_coma.zig", .root = "tests/eth_coma_test.zig" },
        .{ .name = "bscan", .source = "src/internal/bscan.zig", .root = "tests/bscan_test.zig" },
        .{ .name = "eth_mfwd", .source = "src/internal/eth_mfwd.zig", .root = "tests/eth_mfwd_test.zig" },
        .{ .name = "fuelgauge", .source = "src/internal/fuelgauge.zig", .root = "tests/fuelgauge_test.zig" },
        .{ .name = "sram_security", .source = "src/internal/sram_security.zig", .root = "tests/sram_security_test.zig" },
        .{ .name = "bkup_security", .source = "src/internal/bkup_security.zig", .root = "tests/bkup_security_test.zig" },
        .{ .name = "lpm_graphics", .source = "src/internal/lpm_graphics.zig", .root = "tests/lpm_graphics_test.zig" },
        .{ .name = "i3c_i2c_peripheral", .source = "src/internal/i3c_i2c_peripheral.zig", .root = "tests/i3c_i2c_peripheral_test.zig" },
        .{ .name = "i3c_i2c_errors", .source = "src/internal/i3c_i2c_errors.zig", .root = "tests/i3c_i2c_errors_test.zig" },
        .{ .name = "i3c_i2c_scan", .source = "src/internal/i3c_i2c_scan.zig", .root = "tests/i3c_i2c_scan_test.zig" },
        .{ .name = "i3c_i2c_abort", .source = "src/internal/i3c_i2c_abort.zig", .root = "tests/i3c_i2c_abort_test.zig" },
        .{ .name = "i3c_i2c_irq", .source = "src/internal/i3c_i2c_irq.zig", .root = "tests/i3c_i2c_irq_test.zig" },
        .{ .name = "mstp_ids", .source = "src/internal/mstp_ids.zig", .root = "tests/mstp_ids_test.zig" },
        .{ .name = "isr_globals", .source = "src/internal/isr_globals.zig", .root = "tests/isr_globals_test.zig" },
        .{ .name = "exit_stop", .source = "src/internal/exit_stop.zig", .root = "tests/exit_stop_test.zig" },
        .{ .name = "ether_phy", .source = "src/internal/ether_phy.zig", .root = "tests/ether_phy_test.zig" },
        .{ .name = "mpc", .source = "src/internal/mpc.zig", .root = "tests/mpc_test.zig" },
        .{ .name = "doc", .source = "src/internal/doc.zig", .root = "tests/doc_test.zig" },
        .{ .name = "cac", .source = "src/internal/cac.zig", .root = "tests/cac_test.zig" },
        .{ .name = "epaper_geom", .source = "src/internal/epaper_geom.zig", .root = "tests/epaper_geom_test.zig" },
        .{ .name = "pwr", .source = "src/internal/pwr.zig", .root = "tests/pwr_test.zig" },
        .{ .name = "sau", .source = "src/internal/sau.zig", .root = "tests/sau_test.zig" },
        .{ .name = "ethosu_shim", .source = "src/internal/ethosu_shim.zig", .root = "tests/ethosu_shim_test.zig" },
        .{ .name = "crc", .source = "src/internal/crc.zig", .root = "tests/crc_test.zig" },
        .{ .name = "mipi_phy_ops", .source = "src/internal/mipi_phy_ops.zig", .root = "tests/mipi_phy_ops_test.zig" },
        .{ .name = "spi_b_dma", .source = "src/internal/spi_b_dma.zig", .root = "tests/spi_b_dma_test.zig" },
        .{ .name = "tsn", .source = "src/internal/tsn.zig", .root = "tests/tsn_test.zig" },
        .{ .name = "acmphs", .source = "src/internal/acmphs.zig", .root = "tests/acmphs_test.zig" },
        .{ .name = "sci_spi", .source = "src/internal/sci_spi.zig", .root = "tests/sci_spi_test.zig" },
        .{ .name = "canfd_timing", .source = "src/internal/canfd_timing.zig", .root = "tests/canfd_timing_test.zig" },
        .{ .name = "dotf_power", .source = "src/internal/dotf_power.zig", .root = "tests/dotf_power_test.zig" },
        .{ .name = "usb_pvnd", .source = "src/internal/usb_pvnd.zig", .root = "tests/usb_pvnd_test.zig" },
        .{ .name = "canfd_afl", .source = "src/internal/canfd_afl.zig", .root = "tests/canfd_afl_test.zig" },
        .{ .name = "cgc_eswclk", .source = "src/internal/cgc_eswclk.zig", .root = "tests/cgc_eswclk_test.zig" },
        .{ .name = "dac_b", .source = "src/internal/dac_b.zig", .root = "tests/dac_b_test.zig" },
        .{ .name = "usb_pprn", .source = "src/internal/usb_pprn.zig", .root = "tests/usb_pprn_test.zig" },
        .{ .name = "usb_phid", .source = "src/internal/usb_phid.zig", .root = "tests/usb_phid_test.zig" },
        .{ .name = "rmac_phy", .source = "src/internal/rmac_phy.zig", .root = "tests/rmac_phy_test.zig" },
        .{ .name = "rmac_mgmt", .source = "src/internal/rmac_mgmt.zig", .root = "tests/rmac_mgmt_test.zig" },
        .{ .name = "canfd_frame", .source = "src/internal/canfd_frame.zig", .root = "tests/canfd_frame_test.zig" },
        .{ .name = "dtc", .source = "src/internal/dtc.zig", .root = "tests/dtc_test.zig" },
        .{ .name = "eth_gptp", .source = "src/internal/eth_gptp.zig", .root = "tests/eth_gptp_test.zig" },
        .{ .name = "etha_tas", .source = "src/internal/etha_tas.zig", .root = "tests/etha_tas_test.zig" },
        .{ .name = "etha_stats", .source = "src/internal/etha_stats.zig", .root = "tests/etha_stats_test.zig" },
        .{ .name = "sci_dma_isr", .source = "src/internal/sci_dma_isr.zig", .root = "tests/sci_dma_isr_test.zig" },
        .{ .name = "ceu_init_regs", .source = "src/internal/ceu_init_regs.zig", .root = "tests/ceu_init_regs_test.zig" },
        .{ .name = "usb_pmsc_scsi", .source = "src/internal/usb_pmsc_scsi.zig", .root = "tests/usb_pmsc_scsi_test.zig" },
        .{ .name = "pdm", .source = "src/internal/pdm.zig", .root = "tests/pdm_test.zig" },
        .{ .name = "spi_b_target", .source = "src/internal/spi_b_target.zig", .root = "tests/spi_b_target_test.zig" },
        .{ .name = "ulpt", .source = "src/internal/ulpt.zig", .root = "tests/ulpt_test.zig" },
        .{ .name = "poeg", .source = "src/internal/poeg.zig", .root = "tests/poeg_test.zig" },
        .{ .name = "bkup_tamper", .source = "src/internal/bkup_tamper.zig", .root = "tests/bkup_tamper_test.zig" },
        .{ .name = "bkup", .source = "src/internal/bkup.zig", .root = "tests/bkup_test.zig" },
        .{ .name = "glcdc_layer", .source = "src/internal/glcdc_layer.zig", .root = "tests/glcdc_layer_test.zig" },
        .{ .name = "ssie_stream", .source = "src/internal/ssie_stream.zig", .root = "tests/ssie_stream_test.zig" },
        .{ .name = "ipc_sem_ring", .source = "src/internal/ipc_sem_ring.zig", .root = "tests/ipc_sem_ring_test.zig" },
        .{ .name = "sdramc", .source = "src/internal/sdramc.zig", .root = "tests/sdramc_test.zig" },
        .{ .name = "adc_selfdiag", .source = "src/internal/adc_selfdiag.zig", .root = "tests/adc_selfdiag_test.zig" },
        .{ .name = "cache", .source = "src/internal/cache.zig", .root = "tests/cache_test.zig" },
        .{ .name = "usb_paud", .source = "src/internal/usb_paud.zig", .root = "tests/usb_paud_test.zig" },
        .{ .name = "eth_link", .source = "src/internal/eth_link.zig", .root = "tests/eth_link_test.zig" },
        .{ .name = "smbus", .source = "src/internal/smbus.zig", .root = "tests/smbus_test.zig" },
        .{ .name = "usb_cdc", .source = "src/internal/usb_cdc.zig", .root = "tests/usb_cdc_test.zig" },
        .{ .name = "lvd_events", .source = "src/internal/lvd_events.zig", .root = "tests/lvd_events_test.zig" },
        .{ .name = "lvd_runtime", .source = "src/internal/lvd_runtime.zig", .root = "tests/lvd_runtime_test.zig" },
        .{ .name = "dmac", .source = "src/internal/dmac.zig", .root = "tests/dmac_test.zig" },
        .{ .name = "eth_gwca_queue", .source = "src/internal/eth_gwca_queue.zig", .root = "tests/eth_gwca_queue_test.zig" },
        .{ .name = "reset", .source = "src/internal/reset.zig", .root = "tests/reset_test.zig" },
        .{ .name = "sci_lin", .source = "src/internal/sci_lin.zig", .root = "tests/sci_lin_test.zig" },
        .{ .name = "dual_core", .source = "src/internal/dual_core.zig", .root = "tests/dual_core_test.zig" },
        .{ .name = "usb_host_bulk", .source = "src/internal/usb_host_bulk.zig", .root = "tests/usb_host_bulk_test.zig" },
        .{ .name = "flash_irq", .source = "src/internal/flash_irq.zig", .root = "tests/flash_irq_test.zig" },
        .{ .name = "usb_pmsc", .source = "src/internal/usb_pmsc.zig", .root = "tests/usb_pmsc_test.zig" },
        .{ .name = "ble", .source = "src/internal/ble.zig", .root = "tests/ble_test.zig" },
        .{ .name = "gpio_pins", .source = "src/internal/gpio_pins.zig", .root = "tests/gpio_pins_test.zig" },
        .{ .name = "isr", .source = "src/internal/isr.zig", .root = "tests/isr_test.zig" },
        .{ .name = "rmac_phy_drv", .source = "src/internal/rmac_phy_drv.zig", .root = "tests/rmac_phy_drv_test.zig" },
        .{ .name = "mipi_csi_irq", .source = "src/internal/mipi_csi_irq.zig", .root = "tests/mipi_csi_irq_test.zig" },
        .{ .name = "mipi_dsi_cmd", .source = "src/internal/mipi_dsi_cmd.zig", .root = "tests/mipi_dsi_cmd_test.zig" },
        .{ .name = "mipi_csi_status", .source = "src/internal/mipi_csi_status.zig", .root = "tests/mipi_csi_status_test.zig" },
        .{ .name = "mipi_csi_config", .source = "src/internal/mipi_csi_config.zig", .root = "tests/mipi_csi_config_test.zig" },
        .{ .name = "mipi_csi_info", .source = "src/internal/mipi_csi_info.zig", .root = "tests/mipi_csi_info_test.zig" },
        .{ .name = "mipi_csi_lifecycle", .source = "src/internal/mipi_csi_lifecycle.zig", .root = "tests/mipi_csi_lifecycle_test.zig" },
        .{ .name = "mipi_dsi_lanes", .source = "src/internal/mipi_dsi_lanes.zig", .root = "tests/mipi_dsi_lanes_test.zig" },
        .{ .name = "mipi_dsi_lifecycle", .source = "src/internal/mipi_dsi_lifecycle.zig", .root = "tests/mipi_dsi_lifecycle_test.zig" },
        .{ .name = "mipi_dsi_status", .source = "src/internal/mipi_dsi_status.zig", .root = "tests/mipi_dsi_status_test.zig" },
        .{ .name = "mipi_dsi_video", .source = "src/internal/mipi_dsi_video.zig", .root = "tests/mipi_dsi_video_test.zig" },
        .{ .name = "mipi_dsi_dispatch", .source = "src/internal/mipi_dsi_dispatch.zig", .root = "tests/mipi_dsi_dispatch_test.zig" },
        .{ .name = "fpu_probe", .source = "src/fpu_probe_abi.zig", .root = "tests/fpu_probe_test.zig" },
        .{ .name = "mipi_dsi_command", .source = "src/internal/mipi_dsi_command.zig", .root = "tests/mipi_dsi_command_test.zig" },
        .{ .name = "i2c_status", .source = "src/internal/i2c_status.zig", .root = "tests/i2c_status_test.zig" },
        .{ .name = "i2c_bitrate", .source = "src/internal/i2c_bitrate.zig", .root = "tests/i2c_bitrate_test.zig" },
        .{ .name = "i2c_config", .source = "src/internal/i2c_config.zig", .root = "tests/i2c_config_test.zig" },
        .{ .name = "i2c_target", .source = "src/internal/i2c_target.zig", .root = "tests/i2c_target_test.zig" },
        .{ .name = "usb_hhub", .source = "src/internal/usb_hhub.zig", .root = "tests/usb_hhub_test.zig" },
        .{ .name = "usb_composite", .source = "src/internal/usb_composite.zig", .root = "tests/usb_composite_test.zig" },
        .{ .name = "vreg", .source = "src/internal/vreg.zig", .root = "tests/vreg_test.zig" },
        .{ .name = "rsip_protected", .source = "src/internal/rsip_protected.zig", .root = "tests/rsip_protected_test.zig" },
        .{ .name = "touch", .source = "src/internal/touch.zig", .root = "tests/touch_test.zig" },
        .{ .name = "sdcard", .source = "src/internal/sdcard.zig", .root = "tests/sdcard_test.zig" },
        .{ .name = "lpm", .source = "src/internal/lpm.zig", .root = "tests/lpm_test.zig" },
        .{ .name = "cnecc", .source = "src/internal/cnecc.zig", .root = "tests/cnecc_test.zig" },
        .{ .name = "lvd", .source = "src/internal/lvd.zig", .root = "tests/lvd_test.zig" },
        .{ .name = "sram", .source = "src/internal/sram.zig", .root = "tests/sram_test.zig" },
        .{ .name = "pdg", .source = "src/internal/pdg.zig", .root = "tests/pdg_test.zig" },
        .{ .name = "agt", .source = "src/internal/agt.zig", .root = "tests/agt_test.zig" },
        .{ .name = "dma", .source = "src/internal/dma.zig", .root = "tests/dma_test.zig" },
    };
    for (units) |unit| {
        const test_module = b.createModule(.{
            .root_source_file = b.path(unit.root),
            .target = target,
            .optimize = optimize,
        });
        test_module.addImport(unit.name, b.createModule(.{
            .root_source_file = b.path(unit.source),
            .target = target,
            .optimize = optimize,
        }));
        const tests = b.addTest(.{ .root_module = test_module });
        test_step.dependOn(&b.addRunArtifact(tests).step);
    }
    addFpuLoweringCheck(b, test_step);
}

/// Compile the FPU probe for the default cortex_m85 target (single-precision
/// FPU, hard-float ABI) and require its f64 math to call __aeabi_d* helpers.
fn addFpuLoweringCheck(b: *std.Build, test_step: *std.Build.Step) void {
    const arm = b.resolveTargetQuery(.{
        .cpu_arch = .thumb,
        .os_tag = .freestanding,
        .abi = .eabihf,
        .cpu_model = .{ .explicit = &std.Target.arm.cpu.cortex_m85 },
    });
    const probe = b.addObject(.{
        .name = "ra8_hal_fpu_probe_witness",
        .root_module = b.createModule(.{
            .root_source_file = b.path("src/fpu_probe_abi.zig"),
            .target = arm,
            .optimize = .ReleaseSmall,
        }),
    });
    probe.bundle_compiler_rt = false;
    const checker = b.addExecutable(.{
        .name = "fpu_lowering_check",
        .root_module = b.createModule(.{
            .root_source_file = b.path("tests/fpu_lowering_check.zig"),
            .target = b.graph.host,
        }),
    });
    const run = b.addRunArtifact(checker);
    run.addFileArg(probe.getEmittedBin());
    test_step.dependOn(&run.step);
}
