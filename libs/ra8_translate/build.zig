//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Build graph for `ra8_translate`: freestanding per-chapter `TRN1` sidecar
//! codec and future offline translation runtime primitives.

const std = @import("std");
const ra8_build = @import("ra8_zig_build");

pub fn build(b: *std.Build) void {
    const target = b.standardTargetOptions(.{
        .default_target = ra8_build.hostDefaultTargetQuery(b),
    });
    const optimize = b.standardOptimizeOption(.{});

    const err_mod = b.createModule(.{
        .root_source_file = b.path("src/internal/err.zig"),
        .target = target,
        .optimize = optimize,
    });

    const crc32_mod = b.createModule(.{
        .root_source_file = b.path("src/internal/crc32.zig"),
        .target = target,
        .optimize = optimize,
    });

    const sidecar_mod = b.createModule(.{
        .root_source_file = b.path("src/internal/sidecar.zig"),
        .target = target,
        .optimize = optimize,
    });
    sidecar_mod.addImport("crc32", crc32_mod);
    sidecar_mod.addImport("err", err_mod);

    const lang_mod = b.createModule(.{
        .root_source_file = b.path("src/internal/lang.zig"),
        .target = target,
        .optimize = optimize,
    });
    lang_mod.addImport("err", err_mod);
    lang_mod.addImport("sidecar", sidecar_mod);

    const glossary_mod = b.createModule(.{
        .root_source_file = b.path("src/internal/glossary.zig"),
        .target = target,
        .optimize = optimize,
    });
    glossary_mod.addImport("crc32", crc32_mod);
    glossary_mod.addImport("err", err_mod);
    glossary_mod.addImport("sidecar", sidecar_mod);
    glossary_mod.addImport("lang", lang_mod);

    const ocr_mod = b.createModule(.{
        .root_source_file = b.path("src/internal/ocr_preprocess.zig"),
        .target = target,
        .optimize = optimize,
    });
    ocr_mod.addImport("err", err_mod);

    const vocab_mod = b.createModule(.{
        .root_source_file = b.path("src/internal/vocab.zig"),
        .target = target,
        .optimize = optimize,
    });
    vocab_mod.addImport("crc32", crc32_mod);
    vocab_mod.addImport("err", err_mod);

    const tok_mod = b.createModule(.{
        .root_source_file = b.path("src/internal/tokenizer.zig"),
        .target = target,
        .optimize = optimize,
    });
    tok_mod.addImport("vocab", vocab_mod);
    tok_mod.addImport("err", err_mod);

    const overlay_mask_mod = b.createModule(.{
        .root_source_file = b.path("src/internal/overlay_mask.zig"),
        .target = target,
        .optimize = optimize,
    });
    overlay_mask_mod.addImport("sidecar", sidecar_mod);

    const overlay_mod = b.createModule(.{
        .root_source_file = b.path("src/internal/overlay.zig"),
        .target = target,
        .optimize = optimize,
    });
    overlay_mod.addImport("err", err_mod);
    overlay_mod.addImport("sidecar", sidecar_mod);
    overlay_mod.addImport("overlay_mask", overlay_mask_mod);

    const ctc_mod = b.createModule(.{
        .root_source_file = b.path("src/internal/ctc_decode.zig"),
        .target = target,
        .optimize = optimize,
    });
    ctc_mod.addImport("crc32", crc32_mod);
    ctc_mod.addImport("err", err_mod);
    ctc_mod.addImport("lang", lang_mod);

    const det_grp_mod = b.createModule(.{
        .root_source_file = b.path("src/internal/detector_group.zig"),
        .target = target,
        .optimize = optimize,
    });

    const det_mod = b.createModule(.{
        .root_source_file = b.path("src/internal/detector_post.zig"),
        .target = target,
        .optimize = optimize,
    });
    det_mod.addImport("detector_group", det_grp_mod);
    det_mod.addImport("err", err_mod);

    const ckpt_mod = b.createModule(.{
        .root_source_file = b.path("src/internal/chapter_checkpoint.zig"),
        .target = target,
        .optimize = optimize,
    });
    ckpt_mod.addImport("crc32", crc32_mod);
    ckpt_mod.addImport("err", err_mod);
    ckpt_mod.addImport("glossary", glossary_mod);
    ckpt_mod.addImport("sidecar", sidecar_mod);

    const job_mod = b.createModule(.{
        .root_source_file = b.path("src/internal/chapter_job.zig"),
        .target = target,
        .optimize = optimize,
    });
    job_mod.addImport("chapter_checkpoint", ckpt_mod);
    job_mod.addImport("err", err_mod);
    job_mod.addImport("glossary", glossary_mod);
    job_mod.addImport("lang", lang_mod);
    job_mod.addImport("overlay", overlay_mod);
    job_mod.addImport("sidecar", sidecar_mod);

    const job_abi_mod = b.createModule(.{
        .root_source_file = b.path("src/chapter_job_abi.zig"),
        .target = target,
        .optimize = optimize,
        .pic = true,
    });
    job_abi_mod.addImport("err", err_mod);
    job_abi_mod.addImport("chapter_job", job_mod);
    job_abi_mod.addImport("sidecar", sidecar_mod);
    job_abi_mod.addImport("overlay", overlay_mod);

    const abi_mod = b.createModule(.{
        .root_source_file = b.path("src/ra8_translate_abi.zig"),
        .target = target,
        .optimize = optimize,
        .pic = true,
    });
    abi_mod.addImport("err", err_mod);
    abi_mod.addImport("sidecar", sidecar_mod);
    abi_mod.addImport("lang", lang_mod);
    abi_mod.addImport("glossary", glossary_mod);
    abi_mod.addImport("ocr", ocr_mod);
    abi_mod.addImport("tokenizer", tok_mod);
    abi_mod.addImport("overlay", overlay_mod);

    const ctc_abi_mod = b.createModule(.{
        .root_source_file = b.path("src/ctc_decode_abi.zig"),
        .target = target,
        .optimize = optimize,
        .pic = true,
    });
    ctc_abi_mod.addImport("ctc_decode", ctc_mod);
    ctc_abi_mod.addImport("err", err_mod);

    const det_abi_mod = b.createModule(.{
        .root_source_file = b.path("src/detector_post_abi.zig"),
        .target = target,
        .optimize = optimize,
        .pic = true,
    });
    det_abi_mod.addImport("detector_post", det_mod);
    det_abi_mod.addImport("err", err_mod);

    const root_mod = b.createModule(.{
        .root_source_file = b.path("src/root.zig"),
        .target = target,
        .optimize = optimize,
        .pic = true,
    });
    root_mod.addImport("ra8_translate_abi", abi_mod);
    root_mod.addImport("ctc_decode_abi", ctc_abi_mod);
    root_mod.addImport("detector_post_abi", det_abi_mod);
    root_mod.addImport("chapter_job_abi", job_abi_mod);

    const library = b.addLibrary(.{
        .name = "ra8_translate",
        .linkage = .static,
        .root_module = root_mod,
    });
    library.bundle_compiler_rt = false;
    library.link_function_sections = true;
    library.link_data_sections = true;
    library.root_module.pic = true;
    b.installArtifact(library);

    // Keep the cross-target C23 header contract smoke compile on default_step.
    const smoke_mod = b.createModule(.{
        .target = target,
        .optimize = optimize,
    });
    smoke_mod.addIncludePath(b.path("inc"));
    smoke_mod.addIncludePath(b.path("../ra8_core/inc"));
    smoke_mod.addIncludePath(b.path("../ra8_widget/inc"));
    smoke_mod.addIncludePath(b.path("../ra8_box/inc"));
    smoke_mod.addIncludePath(b.path("../ra8_ui/inc"));
    smoke_mod.addCSourceFile(.{
        .file = b.path("tests/ra8_translate_smoke.c"),
        .flags = &.{"-std=c23"},
    });
    const smoke = b.addObject(.{
        .name = "ra8_translate_smoke",
        .root_module = smoke_mod,
    });
    b.default_step.dependOn(&smoke.step);

    b.default_step.dependOn(&b.addInstallDirectory(.{
        .source_dir = b.path("inc"),
        .install_dir = .header,
        .install_subdir = "",
    }).step);

    const test_step = b.step("test", "Run Zig ra8_translate unit and ABI tests");

    const crc_tmod = b.createModule(.{
        .root_source_file = b.path("tests/crc32_test.zig"),
        .target = target,
        .optimize = optimize,
    });
    crc_tmod.addImport("crc32", crc32_mod);
    const crc_tests = b.addTest(.{ .root_module = crc_tmod });
    const run_crc = b.addRunArtifact(crc_tests);
    ra8_build.configureHostTestRun(b, test_step, crc_tests, run_crc);
    test_step.dependOn(&run_crc.step);

    const sidecar_tmod = b.createModule(.{
        .root_source_file = b.path("tests/sidecar_test.zig"),
        .target = target,
        .optimize = optimize,
    });
    sidecar_tmod.addImport("sidecar", sidecar_mod);
    const sidecar_tests = b.addTest(.{ .root_module = sidecar_tmod });
    const run_sidecar = b.addRunArtifact(sidecar_tests);
    ra8_build.configureHostTestRun(b, test_step, sidecar_tests, run_sidecar);
    test_step.dependOn(&run_sidecar.step);

    const abi_tmod = b.createModule(.{
        .root_source_file = b.path("tests/abi_test.zig"),
        .target = target,
        .optimize = optimize,
    });
    abi_tmod.addImport("err", err_mod);
    abi_tmod.addImport("ra8_translate_abi", abi_mod);
    abi_tmod.addImport("tokenizer", tok_mod);
    const abi_tests = b.addTest(.{ .root_module = abi_tmod });
    const run_abi = b.addRunArtifact(abi_tests);
    ra8_build.configureHostTestRun(b, test_step, abi_tests, run_abi);
    test_step.dependOn(&run_abi.step);

    const lang_tmod = b.createModule(.{
        .root_source_file = b.path("tests/lang_test.zig"),
        .target = target,
        .optimize = optimize,
    });
    lang_tmod.addImport("lang", lang_mod);
    lang_tmod.addImport("sidecar", sidecar_mod);
    const lang_tests = b.addTest(.{ .root_module = lang_tmod });
    const run_lang = b.addRunArtifact(lang_tests);
    ra8_build.configureHostTestRun(b, test_step, lang_tests, run_lang);
    test_step.dependOn(&run_lang.step);

    const glossary_tmod = b.createModule(.{
        .root_source_file = b.path("tests/glossary_test.zig"),
        .target = target,
        .optimize = optimize,
    });
    glossary_tmod.addImport("glossary", glossary_mod);
    const glossary_tests = b.addTest(.{ .root_module = glossary_tmod });
    const run_glossary = b.addRunArtifact(glossary_tests);
    ra8_build.configureHostTestRun(b, test_step, glossary_tests, run_glossary);
    test_step.dependOn(&run_glossary.step);

    const ocr_tmod = b.createModule(.{
        .root_source_file = b.path("tests/ocr_preprocess_test.zig"),
        .target = target,
        .optimize = optimize,
    });
    ocr_tmod.addImport("ocr", ocr_mod);
    const ocr_tests = b.addTest(.{ .root_module = ocr_tmod });
    const run_ocr = b.addRunArtifact(ocr_tests);
    ra8_build.configureHostTestRun(b, test_step, ocr_tests, run_ocr);
    test_step.dependOn(&run_ocr.step);

    const tok_tmod = b.createModule(.{
        .root_source_file = b.path("tests/tokenizer_test.zig"),
        .target = target,
        .optimize = optimize,
    });
    tok_tmod.addImport("tokenizer", tok_mod);
    const tok_tests = b.addTest(.{ .root_module = tok_tmod });
    const run_tok = b.addRunArtifact(tok_tests);
    ra8_build.configureHostTestRun(b, test_step, tok_tests, run_tok);
    test_step.dependOn(&run_tok.step);

    const gfx_mod = b.createModule(.{
        .root_source_file = b.path("../ra8_gfx/src/ra8_gfx.zig"),
        .target = target,
        .optimize = optimize,
    });
    const panel_paint_mod = b.createModule(.{
        .root_source_file = b.path("../ra8_widget/src/panel_paint.zig"),
        .target = target,
        .optimize = optimize,
    });
    panel_paint_mod.addImport("ra8_gfx", gfx_mod);
    const widget_paint_mod = b.createModule(.{
        .root_source_file = b.path("../ra8_widget/src/widget_paint_abi.zig"),
        .target = target,
        .optimize = optimize,
    });
    const overlay_tmod = b.createModule(.{
        .root_source_file = b.path("tests/overlay_test.zig"),
        .target = target,
        .optimize = optimize,
    });
    overlay_tmod.addImport("overlay", overlay_mod);
    overlay_tmod.addImport("sidecar", sidecar_mod);
    overlay_tmod.addImport("ra8_gfx", gfx_mod);
    overlay_tmod.addImport("panel_paint", panel_paint_mod);
    overlay_tmod.addImport("widget_paint_abi", widget_paint_mod);
    const overlay_tests = b.addTest(.{ .root_module = overlay_tmod });
    const run_overlay = b.addRunArtifact(overlay_tests);
    ra8_build.configureHostTestRun(b, test_step, overlay_tests, run_overlay);
    test_step.dependOn(&run_overlay.step);

    const ctc_tmod = b.createModule(.{
        .root_source_file = b.path("tests/ctc_decode_test.zig"),
        .target = target,
        .optimize = optimize,
    });
    ctc_tmod.addImport("ctc_decode_abi", ctc_abi_mod);
    ctc_tmod.addImport("ctc_decode", ctc_mod);
    ctc_tmod.addImport("err", err_mod);
    const ctc_tests = b.addTest(.{ .root_module = ctc_tmod });
    const run_ctc = b.addRunArtifact(ctc_tests);
    ra8_build.configureHostTestRun(b, test_step, ctc_tests, run_ctc);
    test_step.dependOn(&run_ctc.step);

    const det_tmod = b.createModule(.{
        .root_source_file = b.path("tests/detector_post_test.zig"),
        .target = target,
        .optimize = optimize,
    });
    det_tmod.addImport("detector_post_abi", det_abi_mod);
    det_tmod.addImport("detector_post", det_mod);
    det_tmod.addImport("err", err_mod);
    const det_tests = b.addTest(.{ .root_module = det_tmod });
    const run_det = b.addRunArtifact(det_tests);
    ra8_build.configureHostTestRun(b, test_step, det_tests, run_det);
    test_step.dependOn(&run_det.step);

    const job_tmod = b.createModule(.{
        .root_source_file = b.path("tests/chapter_job_test.zig"),
        .target = target,
        .optimize = optimize,
    });
    job_tmod.addImport("err", err_mod);
    job_tmod.addImport("chapter_checkpoint", ckpt_mod);
    job_tmod.addImport("chapter_job", job_mod);
    job_tmod.addImport("chapter_job_abi", job_abi_mod);
    job_tmod.addImport("sidecar", sidecar_mod);
    job_tmod.addImport("glossary", glossary_mod);
    job_tmod.addImport("overlay", overlay_mod);
    job_tmod.addImport("ra8_gfx", gfx_mod);
    job_tmod.addImport("panel_paint", panel_paint_mod);
    const job_tests = b.addTest(.{ .root_module = job_tmod });
    const run_job = b.addRunArtifact(job_tests);
    ra8_build.configureHostTestRun(b, test_step, job_tests, run_job);
    test_step.dependOn(&run_job.step);
}
