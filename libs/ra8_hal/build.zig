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
    const abi_units = [_][]const u8{ "eth", "canfd", "layer3_switch", "icu", "iwdt", "npu_quant", "glcdc_gamma", "elc", "epaper_devinfo", "eth_coma", "bscan", "eth_mfwd", "fuelgauge", "sram_security", "bkup_security", "lpm_graphics", "i3c_i2c_peripheral", "ether_phy", "mpc", "doc", "cac", "epaper_geom", "pwr", "sau", "ethosu_shim", "crc", "mipi_phy_ops", "spi_b_dma", "tsn", "acmphs", "sci_spi", "canfd_timing", "dotf_power", "usb_pvnd", "canfd_afl", "cgc_eswclk", "dac_b", "usb_pprn", "canfd_frame", "dtc", "eth_gptp", "etha_tas", "etha_stats", "sci_dma_isr", "ceu_init_regs", "usb_pmsc_scsi", "pdm", "spi_b_target", "ulpt", "poeg", "bkup_tamper", "bkup", "glcdc_layer" };
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
}
