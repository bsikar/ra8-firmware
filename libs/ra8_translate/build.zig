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

    const marian_ops_mod = b.createModule(.{
        .root_source_file = b.path("src/internal/marian_ops.zig"),
        .target = target,
        .optimize = optimize,
    });

    const marian_cont_mod = b.createModule(.{
        .root_source_file = b.path("src/internal/marian_container.zig"),
        .target = target,
        .optimize = optimize,
    });
    marian_cont_mod.addImport("crc32", crc32_mod);
    marian_cont_mod.addImport("err", err_mod);

    const marian_mod = b.createModule(.{
        .root_source_file = b.path("src/internal/marian.zig"),
        .target = target,
        .optimize = optimize,
    });
    marian_mod.addImport("marian_ops", marian_ops_mod);
    marian_mod.addImport("marian_container", marian_cont_mod);
    marian_mod.addImport("err", err_mod);

    const marian_batch_plan_mod = b.createModule(.{
        .root_source_file = b.path("src/internal/marian_batch_plan.zig"),
        .target = target,
        .optimize = optimize,
    });
    marian_batch_plan_mod.addImport("marian_container", marian_cont_mod);
    marian_batch_plan_mod.addImport("err", err_mod);

    const marian_batch_step_mod = b.createModule(.{
        .root_source_file = b.path("src/internal/marian_batch_step.zig"),
        .target = target,
        .optimize = optimize,
    });
    marian_batch_step_mod.addImport("marian_ops", marian_ops_mod);
    marian_batch_step_mod.addImport("marian_container", marian_cont_mod);
    marian_batch_step_mod.addImport("marian_batch_plan", marian_batch_plan_mod);

    const marian_batch_mod = b.createModule(.{
        .root_source_file = b.path("src/internal/marian_batch.zig"),
        .target = target,
        .optimize = optimize,
    });
    marian_batch_mod.addImport("marian_ops", marian_ops_mod);
    marian_batch_mod.addImport("marian_batch_plan", marian_batch_plan_mod);
    marian_batch_mod.addImport("marian_batch_step", marian_batch_step_mod);

    const marian_abi_mod = b.createModule(.{
        .root_source_file = b.path("src/marian_abi.zig"),
        .target = target,
        .optimize = optimize,
        .pic = true,
    });
    marian_abi_mod.addImport("marian", marian_mod);
    marian_abi_mod.addImport("marian_batch", marian_batch_mod);
    marian_abi_mod.addImport("err", err_mod);

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
    root_mod.addImport("marian_abi", marian_abi_mod);

    const core_lib = b.dependency("ra8_core", .{
        .target = target,
        .optimize = optimize,
    }).artifact("ra8_core");
    const gfx_dep = b.dependency("ra8_gfx", .{
        .target = target,
        .optimize = optimize,
    });
    const widget_dep = b.dependency("ra8_widget", .{
        .target = target,
        .optimize = optimize,
    });
    const widget_lib = widget_dep.artifact("ra8_widget");
    const box_lib = b.dependency("ra8_box", .{
        .target = target,
        .optimize = optimize,
    }).artifact("ra8_box");
    const ui_lib = b.dependency("ra8_ui", .{
        .target = target,
        .optimize = optimize,
    }).artifact("ra8_ui");

    const library = b.addLibrary(.{
        .name = "ra8_translate",
        .linkage = .static,
        .root_module = root_mod,
    });
    library.bundle_compiler_rt = false;
    library.link_function_sections = true;
    library.link_data_sections = true;
    library.root_module.pic = true;
    library.installHeadersDirectory(b.path("inc"), "", .{});
    b.installArtifact(library);

    // Keep the cross-target C23 header contract smoke compile on default_step.
    const smoke_mod = b.createModule(.{
        .target = target,
        .optimize = optimize,
    });
    smoke_mod.addIncludePath(library.getEmittedIncludeTree());
    smoke_mod.addIncludePath(core_lib.getEmittedIncludeTree());
    smoke_mod.addIncludePath(widget_lib.getEmittedIncludeTree());
    smoke_mod.addIncludePath(box_lib.getEmittedIncludeTree());
    smoke_mod.addIncludePath(ui_lib.getEmittedIncludeTree());
    smoke_mod.addCSourceFile(.{
        .file = b.path("tests/ra8_translate_smoke.c"),
        .flags = &.{"-std=c23"},
    });
    const smoke = b.addObject(.{
        .name = "ra8_translate_smoke",
        .root_module = smoke_mod,
    });
    b.default_step.dependOn(&smoke.step);

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

    const gfx_mod = gfx_dep.module("ra8_gfx");
    const panel_paint_mod = widget_dep.module("panel_paint");
    const widget_paint_mod = widget_dep.module("widget_paint_abi");
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

    const marian_tmod = b.createModule(.{
        .root_source_file = b.path("tests/marian_test.zig"),
        .target = target,
        .optimize = optimize,
    });
    marian_tmod.addImport("marian_ops", marian_ops_mod);
    marian_tmod.addImport("marian_container", marian_cont_mod);
    marian_tmod.addImport("marian", marian_mod);
    marian_tmod.addImport("err", err_mod);
    const marian_tests = b.addTest(.{ .root_module = marian_tmod });
    const run_marian = b.addRunArtifact(marian_tests);
    ra8_build.configureHostTestRun(b, test_step, marian_tests, run_marian);
    test_step.dependOn(&run_marian.step);

    const marian_batch_tmod = b.createModule(.{
        .root_source_file = b.path("tests/marian_batch_test.zig"),
        .target = target,
        .optimize = optimize,
    });
    marian_batch_tmod.addImport("marian_ops", marian_ops_mod);
    marian_batch_tmod.addImport("marian_container", marian_cont_mod);
    marian_batch_tmod.addImport("marian", marian_mod);
    marian_batch_tmod.addImport("marian_batch", marian_batch_mod);
    const marian_batch_tests = b.addTest(.{ .root_module = marian_batch_tmod });
    const run_marian_batch = b.addRunArtifact(marian_batch_tests);
    ra8_build.configureHostTestRun(b, test_step, marian_batch_tests, run_marian_batch);
    test_step.dependOn(&run_marian_batch.step);
}
