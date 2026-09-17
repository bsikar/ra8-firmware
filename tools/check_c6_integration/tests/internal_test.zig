//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Behavioural regression tests for the offline C6 integration-contract
//! detector (#858).
//!
//! Every expectation below was established by probing CPython's `re` and
//! `pathlib` against the predecessor's patterns BEFORE any of this was
//! written, so these tests pin inherited behaviour rather than a fresh
//! opinion about what the gate ought to accept.

const std = @import("std");
const implementation = @import("implementation");

const testing = std.testing;

fn copies(allocator: std.mem.Allocator, text: []const u8) ![]implementation.StagedCopy {
    return implementation.parseStagedCopies(allocator, text);
}

test "a staged copy on one line yields its source and destination" {
    const found = try copies(testing.allocator, "cp \"${SCRIPT_DIR}/../../a/b.h\" \"${COMPONENT_DIR}/x/b.h\"");
    defer testing.allocator.free(found);
    try testing.expectEqual(@as(usize, 1), found.len);
    try testing.expectEqualStrings("a/b.h", found[0].source);
    try testing.expectEqualStrings("x/b.h", found[0].destination);
}

test "a backslash continuation between the quoted paths is one copy" {
    const found = try copies(testing.allocator, "cp \"${SCRIPT_DIR}/../../a/b.h\" \\\n  \"${COMPONENT_DIR}/include/b.h\"");
    defer testing.allocator.free(found);
    try testing.expectEqual(@as(usize, 1), found.len);
    try testing.expectEqualStrings("include/b.h", found[0].destination);
}

test "two backslash continuations are invisible, the group appears once" {
    const found = try copies(testing.allocator, "cp \"${SCRIPT_DIR}/../../a/b.h\" \\\n \\\n \"${COMPONENT_DIR}/x/b.h\"");
    defer testing.allocator.free(found);
    try testing.expectEqual(@as(usize, 0), found.len);
}

test "no whitespace at all between the quoted paths still matches" {
    const found = try copies(testing.allocator, "cp \"${SCRIPT_DIR}/../../a/b.h\"\"${COMPONENT_DIR}/x/b.h\"");
    defer testing.allocator.free(found);
    try testing.expectEqual(@as(usize, 1), found.len);
}

test "the copy pattern has no leading word boundary" {
    const found = try copies(testing.allocator, "xcp \"${SCRIPT_DIR}/../../a/b.h\" \"${COMPONENT_DIR}/x/b.h\"");
    defer testing.allocator.free(found);
    try testing.expectEqual(@as(usize, 1), found.len);
}

test "a tab or a newline separates cp from its first argument" {
    const tabbed = try copies(testing.allocator, "cp\t\"${SCRIPT_DIR}/../../a/b.h\" \"${COMPONENT_DIR}/x/b.h\"");
    defer testing.allocator.free(tabbed);
    try testing.expectEqual(@as(usize, 1), tabbed.len);
    const broken = try copies(testing.allocator, "cp\n\"${SCRIPT_DIR}/../../a/b.h\" \"${COMPONENT_DIR}/x/b.h\"");
    defer testing.allocator.free(broken);
    try testing.expectEqual(@as(usize, 1), broken.len);
}

test "cp run together with its argument is not a copy" {
    const found = try copies(testing.allocator, "cp\"${SCRIPT_DIR}/../../a/b.h\" \"${COMPONENT_DIR}/x/b.h\"");
    defer testing.allocator.free(found);
    try testing.expectEqual(@as(usize, 0), found.len);
}

test "a non-breaking space separates the quoted paths, because the class is Unicode" {
    const found = try copies(testing.allocator, "cp \"${SCRIPT_DIR}/../../a/b.h\"\u{a0}\"${COMPONENT_DIR}/x/b.h\"");
    defer testing.allocator.free(found);
    try testing.expectEqual(@as(usize, 1), found.len);
}

test "a source path may contain a newline, the class excludes only the quote" {
    const found = try copies(testing.allocator, "cp \"${SCRIPT_DIR}/../../a\nb.h\" \"${COMPONENT_DIR}/x/b.h\"");
    defer testing.allocator.free(found);
    try testing.expectEqual(@as(usize, 1), found.len);
    try testing.expectEqualStrings("a\nb.h", found[0].source);
}

test "an empty quoted path never matches, the class is one-or-more" {
    const found = try copies(testing.allocator, "cp \"${SCRIPT_DIR}/../../\" \"${COMPONENT_DIR}/x/b.h\"");
    defer testing.allocator.free(found);
    try testing.expectEqual(@as(usize, 0), found.len);
}

test "the wrong variable in either position is not a staged copy" {
    const wrong_source = try copies(testing.allocator, "cp \"${SRC_DIR}/../../a/b.h\" \"${COMPONENT_DIR}/x/b.h\"");
    defer testing.allocator.free(wrong_source);
    try testing.expectEqual(@as(usize, 0), wrong_source.len);
    const wrong_dest = try copies(testing.allocator, "cp \"${SCRIPT_DIR}/../../a/b.h\" \"${OTHER_DIR}/x/b.h\"");
    defer testing.allocator.free(wrong_dest);
    try testing.expectEqual(@as(usize, 0), wrong_dest.len);
}

test "copies are returned in recipe order" {
    const text =
        "cp \"${SCRIPT_DIR}/../../one.h\" \"${COMPONENT_DIR}/one.h\"\n" ++
        "cp \"${SCRIPT_DIR}/../../two.h\" \"${COMPONENT_DIR}/two.h\"\n";
    const found = try copies(testing.allocator, text);
    defer testing.allocator.free(found);
    try testing.expectEqual(@as(usize, 2), found.len);
    try testing.expectEqualStrings("one.h", found[0].source);
    try testing.expectEqualStrings("two.h", found[1].source);
}

test "the component name is the literal directory under components" {
    try testing.expectEqualStrings(
        "mdl_service",
        implementation.findComponentName("COMPONENT_DIR=\"${PERIPHERAL_DIR}/components/mdl_service\"").?,
    );
}

test "leading spaces and tabs are allowed before the component assignment" {
    try testing.expectEqualStrings(
        "x",
        implementation.findComponentName("\t COMPONENT_DIR=\"${PERIPHERAL_DIR}/components/x\"\n").?,
    );
}

test "a non-breaking space is not indentation, the class is ASCII" {
    try testing.expect(implementation.findComponentName("\u{a0}COMPONENT_DIR=\"${PERIPHERAL_DIR}/components/x\"") == null);
}

test "a trailing space after the closing quote hides the component assignment" {
    try testing.expect(implementation.findComponentName("COMPONENT_DIR=\"${PERIPHERAL_DIR}/components/x\" ") == null);
}

test "a carriage return after the closing quote hides it too" {
    try testing.expect(implementation.findComponentName("COMPONENT_DIR=\"${PERIPHERAL_DIR}/components/x\"\r\n") == null);
}

test "a nested path is not a literal component name" {
    try testing.expect(implementation.findComponentName("COMPONENT_DIR=\"${PERIPHERAL_DIR}/components/a/b\"") == null);
}

test "an expanded variable is not a literal component name" {
    try testing.expect(implementation.findComponentName("COMPONENT_DIR=\"${PERIPHERAL_DIR}/components/${NAME}\"") != null);
}

test "an empty component name never matches" {
    try testing.expect(implementation.findComponentName("COMPONENT_DIR=\"${PERIPHERAL_DIR}/components/\"") == null);
}

test "the first component assignment wins" {
    const text =
        "COMPONENT_DIR=\"${PERIPHERAL_DIR}/components/a\"\n" ++
        "COMPONENT_DIR=\"${PERIPHERAL_DIR}/components/b\"";
    try testing.expectEqualStrings("a", implementation.findComponentName(text).?);
}

test "the component assignment may end the text without a newline" {
    try testing.expectEqualStrings(
        "x",
        implementation.findComponentName("head\nCOMPONENT_DIR=\"${PERIPHERAL_DIR}/components/x\"").?,
    );
}

fn componentSets(allocator: std.mem.Allocator, text: []const u8) ![][]const u8 {
    return implementation.patchComponentSets(allocator, text);
}

test "an added COMPONENTS declaration yields its names" {
    const sets = try componentSets(testing.allocator, "+set(COMPONENTS a b c)");
    defer testing.allocator.free(sets);
    try testing.expectEqual(@as(usize, 1), sets.len);
    try testing.expectEqualStrings("a b c", sets[0]);
}

test "the names capture crosses newlines, the class excludes only the parenthesis" {
    const sets = try componentSets(testing.allocator, "+set(COMPONENTS a\nb c)");
    defer testing.allocator.free(sets);
    try testing.expectEqual(@as(usize, 1), sets.len);
    try testing.expectEqualStrings("a\nb c", sets[0]);
}

test "a trailing comment after the parenthesis hides the declaration" {
    const sets = try componentSets(testing.allocator, "+set(COMPONENTS a b c) # note");
    defer testing.allocator.free(sets);
    try testing.expectEqual(@as(usize, 0), sets.len);
}

test "exactly one space follows COMPONENTS, a second lands inside the capture" {
    const tight = try componentSets(testing.allocator, "+set(COMPONENTSa b)");
    defer testing.allocator.free(tight);
    try testing.expectEqual(@as(usize, 0), tight.len);
    const loose = try componentSets(testing.allocator, "+set(COMPONENTS  a b)");
    defer testing.allocator.free(loose);
    try testing.expectEqualStrings(" a b", loose[0]);
}

test "two added declarations are both found" {
    const sets = try componentSets(testing.allocator, "+set(COMPONENTS a)\n+set(COMPONENTS b)");
    defer testing.allocator.free(sets);
    try testing.expectEqual(@as(usize, 2), sets.len);
}

test "an indented added line is not a COMPONENTS declaration" {
    const sets = try componentSets(testing.allocator, " +set(COMPONENTS a)");
    defer testing.allocator.free(sets);
    try testing.expectEqual(@as(usize, 0), sets.len);
}

test "names split on any run of whitespace, Unicode included" {
    try testing.expect(implementation.splitContains("a\u{a0}b c", "b"));
    try testing.expect(implementation.splitContains("  mdl_service  ", "mdl_service"));
    try testing.expect(!implementation.splitContains("ra8_mdl_service", "mdl_service"));
    try testing.expect(!implementation.splitContains("", "mdl_service"));
}

test "the header declares the public ABI" {
    try testing.expect(implementation.declaresAbi(
        "uint32_t ra8_mdl_service_component_abi(void);\n",
        implementation.component_abi,
    ));
}

test "the declaration tolerates spacing everywhere the pattern allows it" {
    try testing.expect(implementation.declaresAbi(
        "  uint32_t  ra8_mdl_service_component_abi ( void ) ;",
        implementation.component_abi,
    ));
}

test "the declaration may sit several blank lines after a line start" {
    try testing.expect(implementation.declaresAbi(
        "head\n\n  uint32_t ra8_mdl_service_component_abi(void);",
        implementation.component_abi,
    ));
}

test "a declaration with no semicolon is not a declaration" {
    try testing.expect(!implementation.declaresAbi(
        "uint32_t ra8_mdl_service_component_abi(void)",
        implementation.component_abi,
    ));
}

test "a storage class on the same line suppresses the public declaration" {
    try testing.expect(!implementation.declaresAbi(
        "static uint32_t ra8_mdl_service_component_abi(void);",
        implementation.component_abi,
    ));
}

test "a keyword on the previous line does not suppress it, whitespace crosses lines" {
    try testing.expect(implementation.declaresAbi(
        "extern\nuint32_t ra8_mdl_service_component_abi(void);",
        implementation.component_abi,
    ));
}

test "a longer symbol is not the declared ABI" {
    try testing.expect(!implementation.declaresAbi(
        "uint32_t ra8_mdl_service_component_abi2(void);",
        implementation.component_abi,
    ));
}

test "a parameter list other than void is not the declaration" {
    try testing.expect(!implementation.declaresAbi(
        "uint32_t ra8_mdl_service_component_abi(int x);",
        implementation.component_abi,
    ));
}

test "an attributed definition is externally visible" {
    try testing.expect(implementation.definesAbi(
        "[[gnu::noinline]] uint32_t ra8_mdl_service_component_abi(void) { return 1U; }",
        implementation.component_abi,
    ));
}

test "a static definition is not externally visible" {
    try testing.expect(!implementation.definesAbi(
        "static uint32_t ra8_mdl_service_component_abi(void) { return 1U; }",
        implementation.component_abi,
    ));
}

test "an attribute split across two lines is invisible to the definition pattern" {
    try testing.expect(!implementation.definesAbi(
        "[[gnu::\nnoinline]] uint32_t ra8_mdl_service_component_abi(void)",
        implementation.component_abi,
    ));
}

test "two attributes on one line still define, the run gives back to the last bracket pair" {
    try testing.expect(implementation.definesAbi(
        "[[a]] [[b]] uint32_t ra8_mdl_service_component_abi(void)",
        implementation.component_abi,
    ));
}

test "junk between the attribute and the return type is not a definition" {
    try testing.expect(!implementation.definesAbi(
        "[[a]] junk uint32_t ra8_mdl_service_component_abi(void)",
        implementation.component_abi,
    ));
}

test "an attribute on its own line still defines, trailing whitespace crosses lines" {
    try testing.expect(implementation.definesAbi(
        "[[gnu::noinline]]\nuint32_t ra8_mdl_service_component_abi(void)",
        implementation.component_abi,
    ));
}

test "the patch declares the weak extension point" {
    try testing.expect(implementation.patchDeclaresWeakHook(
        "+__attribute__((weak)) esp_err_t esp_hosted_custom_rpc_sync_handler(",
        implementation.custom_rpc_hook,
    ));
}

test "the weak declaration may break the line after the attribute" {
    try testing.expect(implementation.patchDeclaresWeakHook(
        "+__attribute__((weak))\nesp_err_t esp_hosted_custom_rpc_sync_handler(",
        implementation.custom_rpc_hook,
    ));
}

test "an indented added line is not the weak declaration" {
    try testing.expect(!implementation.patchDeclaresWeakHook(
        " +__attribute__((weak)) esp_err_t esp_hosted_custom_rpc_sync_handler(",
        implementation.custom_rpc_hook,
    ));
}

test "a context line is not the weak declaration" {
    try testing.expect(!implementation.patchDeclaresWeakHook(
        "__attribute__((weak)) esp_err_t esp_hosted_custom_rpc_sync_handler(",
        implementation.custom_rpc_hook,
    ));
}

test "the component source defines the strong hook" {
    try testing.expect(implementation.definesStrongHook(
        "esp_err_t esp_hosted_custom_rpc_sync_handler(uint32_t id) { return ESP_OK; }",
        implementation.custom_rpc_hook,
    ));
}

test "an indented strong hook still counts, and a static one does not" {
    try testing.expect(implementation.definesStrongHook(
        "   esp_err_t esp_hosted_custom_rpc_sync_handler(",
        implementation.custom_rpc_hook,
    ));
    try testing.expect(!implementation.definesStrongHook(
        "static esp_err_t esp_hosted_custom_rpc_sync_handler(",
        implementation.custom_rpc_hook,
    ));
}

test "a missing open parenthesis is not a hook definition" {
    try testing.expect(!implementation.definesStrongHook(
        "esp_err_t esp_hosted_custom_rpc_sync_handler;",
        implementation.custom_rpc_hook,
    ));
}

test "pathlib basename semantics" {
    try testing.expectEqualStrings("c.h", implementation.pathName("a/b/c.h"));
    try testing.expectEqualStrings("c.h", implementation.pathName("c.h"));
    try testing.expectEqualStrings("b", implementation.pathName("a/b/"));
    try testing.expectEqualStrings("b", implementation.pathName("a//b"));
    try testing.expectEqualStrings("a", implementation.pathName("a/."));
    try testing.expectEqualStrings("..", implementation.pathName("a/.."));
    try testing.expectEqualStrings("", implementation.pathName("."));
    try testing.expectEqualStrings("", implementation.pathName(""));
    try testing.expectEqualStrings("", implementation.pathName("/"));
    try testing.expectEqualStrings(".hidden", implementation.pathName("a/b/.hidden"));
    try testing.expectEqualStrings("x.", implementation.pathName("x."));
    try testing.expectEqualStrings(" ", implementation.pathName("a/b/ "));
}

const good_build =
    \\  COMPONENT_DIR="${PERIPHERAL_DIR}/components/mdl_service"
    \\cp "${SCRIPT_DIR}/../../port/esp32_c6/CMakeLists.txt" "${COMPONENT_DIR}/CMakeLists.txt"
    \\cp "${SCRIPT_DIR}/../../port/esp32_c6/src/mdl_service.c" "${COMPONENT_DIR}/src/mdl_service.c"
    \\cp "${SCRIPT_DIR}/../../port/esp32_c6/inc/ra8_mdl_service.h" "${COMPONENT_DIR}/include/ra8_mdl_service.h"
    \\cp "${SCRIPT_DIR}/../../libs/ra8_c6link/inc/ra8_mdl_protocol.h" "${COMPONENT_DIR}/include/ra8_mdl_protocol.h"
    \\cp "${SCRIPT_DIR}/../../libs/ra8_c6link/inc/ra8_mdl_http.h" "${COMPONENT_DIR}/include/ra8_mdl_http.h"
    \\grep -Eq 'T[[:space:]]+ra8_mdl_service_component_abi$'
    \\grep -Eq 'T[[:space:]]+esp_hosted_custom_rpc_sync_handler$'
;

const good_patch =
    \\+set(COMPONENTS esp_timer main mdl_service)
    \\+__attribute__((weak)) esp_err_t esp_hosted_custom_rpc_sync_handler(
;

const good_header = "uint32_t ra8_mdl_service_component_abi(void);\n";

const good_source =
    \\[[gnu::noinline]] uint32_t ra8_mdl_service_component_abi(void) { return 1U; }
    \\esp_err_t esp_hosted_custom_rpc_sync_handler(uint32_t id) { return ESP_OK; }
;

fn goodInventory() implementation.Inventory {
    return .{ .sources = &.{
        "port/esp32_c6/CMakeLists.txt",
        "port/esp32_c6/src/mdl_service.c",
        "port/esp32_c6/inc/ra8_mdl_service.h",
        "libs/ra8_c6link/inc/ra8_mdl_protocol.h",
        "libs/ra8_c6link/inc/ra8_mdl_http.h",
    } };
}

fn findingsFor(
    arena: *std.heap.ArenaAllocator,
    build_text: []const u8,
    patch_text: []const u8,
    header_text: []const u8,
    source_text: []const u8,
) ![][]const u8 {
    return implementation.checkContract(arena.allocator(), .{
        .build_text = build_text,
        .patch_text = patch_text,
        .header_text = header_text,
        .source_text = source_text,
    }, goodInventory());
}

test "the agreeing contract reports nothing" {
    var arena = std.heap.ArenaAllocator.init(testing.allocator);
    defer arena.deinit();
    const findings = try findingsFor(&arena, good_build, good_patch, good_header, good_source);
    try testing.expectEqual(@as(usize, 0), findings.len);
}

test "an empty recipe reports the missing copies and the empty recipe itself" {
    var arena = std.heap.ArenaAllocator.init(testing.allocator);
    defer arena.deinit();
    const findings = try findingsFor(&arena, "", good_patch, good_header, good_source);
    try testing.expectEqualStrings(
        "staging: build.sh contains no first-party component copies",
        findings[0],
    );
    try testing.expectEqualStrings(
        "staging: build.sh no longer stages required source libs/ra8_c6link/inc/ra8_mdl_http.h",
        findings[1],
    );
}

test "missing required sources are reported in sorted order" {
    var arena = std.heap.ArenaAllocator.init(testing.allocator);
    defer arena.deinit();
    const findings = try findingsFor(&arena, "", good_patch, good_header, good_source);
    var previous: []const u8 = "";
    var seen: usize = 0;
    for (findings) |finding| {
        const head = "staging: build.sh no longer stages required source ";
        if (!std.mem.startsWith(u8, finding, head)) continue;
        const name = finding[head.len..];
        try testing.expect(std.mem.order(u8, previous, name) == .lt);
        previous = name;
        seen += 1;
    }
    try testing.expectEqual(implementation.required_staged_sources.len, seen);
}

test "a source outside the inventory is reported as nonexistent" {
    var arena = std.heap.ArenaAllocator.init(testing.allocator);
    defer arena.deinit();
    const build_text = good_build ++ "\ncp \"${SCRIPT_DIR}/../../gone/absent.h\" \"${COMPONENT_DIR}/include/absent.h\"";
    const findings = try findingsFor(&arena, build_text, good_patch, good_header, good_source);
    try testing.expectEqual(@as(usize, 1), findings.len);
    try testing.expectEqualStrings("staging: source does not exist: gone/absent.h", findings[0]);
}

test "a renamed destination is reported with both spellings" {
    var arena = std.heap.ArenaAllocator.init(testing.allocator);
    defer arena.deinit();
    const build_text = try std.mem.replaceOwned(
        u8,
        arena.allocator(),
        good_build,
        "include/ra8_mdl_http.h",
        "include/mdl_http.h",
    );
    const findings = try findingsFor(&arena, build_text, good_patch, good_header, good_source);
    try testing.expectEqual(@as(usize, 1), findings.len);
    try testing.expect(std.mem.endsWith(u8, findings[0], "must retain their source basename"));
}

test "a source and destination differing only by directory is not a rename" {
    var arena = std.heap.ArenaAllocator.init(testing.allocator);
    defer arena.deinit();
    const findings = try findingsFor(&arena, good_build, good_patch, good_header, good_source);
    try testing.expectEqual(@as(usize, 0), findings.len);
}

test "a component absent from the patch's explicit set is reported" {
    var arena = std.heap.ArenaAllocator.init(testing.allocator);
    defer arena.deinit();
    const patch_text = try std.mem.replaceOwned(u8, arena.allocator(), good_patch, "main mdl_service", "main other");
    const findings = try findingsFor(&arena, good_build, patch_text, good_header, good_source);
    try testing.expectEqual(@as(usize, 1), findings.len);
    try testing.expectEqualStrings(
        "component: build.sh stages 'mdl_service', but the patch's explicit COMPONENTS set does not include it",
        findings[0],
    );
}

test "two explicit COMPONENTS declarations are rejected, and the membership check is skipped" {
    var arena = std.heap.ArenaAllocator.init(testing.allocator);
    defer arena.deinit();
    const patch_text = good_patch ++ "\n+set(COMPONENTS main mdl_service)";
    const findings = try findingsFor(&arena, good_build, patch_text, good_header, good_source);
    try testing.expectEqual(@as(usize, 1), findings.len);
    try testing.expectEqualStrings(
        "component: patch must add exactly one explicit set(COMPONENTS ...) declaration",
        findings[0],
    );
}

test "a nonliteral component assignment is reported and the membership check is skipped" {
    var arena = std.heap.ArenaAllocator.init(testing.allocator);
    defer arena.deinit();
    const build_text = try std.mem.replaceOwned(
        u8,
        arena.allocator(),
        good_build,
        "components/mdl_service\"",
        "components/${COMPONENT_NAME}/x\"",
    );
    const findings = try findingsFor(&arena, build_text, good_patch, good_header, good_source);
    try testing.expectEqual(@as(usize, 1), findings.len);
    try testing.expectEqualStrings(
        "component: build.sh does not declare a literal staged component name",
        findings[0],
    );
}

test "a drifted post-link ABI assertion is reported" {
    var arena = std.heap.ArenaAllocator.init(testing.allocator);
    defer arena.deinit();
    const build_text = try std.mem.replaceOwned(
        u8,
        arena.allocator(),
        good_build,
        "T[[:space:]]+ra8_mdl_service_component_abi$",
        "T[[:space:]]+mdl_service_component_abi$",
    );
    const findings = try findingsFor(&arena, build_text, good_patch, good_header, good_source);
    try testing.expectEqual(@as(usize, 1), findings.len);
    try testing.expectEqualStrings(
        "ABI: build.sh does not require strong text symbol ra8_mdl_service_component_abi",
        findings[0],
    );
}

test "the post-link check is a substring test including the anchor character" {
    var arena = std.heap.ArenaAllocator.init(testing.allocator);
    defer arena.deinit();
    const build_text = try std.mem.replaceOwned(
        u8,
        arena.allocator(),
        good_build,
        "T[[:space:]]+esp_hosted_custom_rpc_sync_handler$",
        "T[[:space:]]+esp_hosted_custom_rpc_sync_handler",
    );
    const findings = try findingsFor(&arena, build_text, good_patch, good_header, good_source);
    try testing.expectEqual(@as(usize, 1), findings.len);
    try testing.expect(std.mem.startsWith(u8, findings[0], "hook: build.sh does not require"));
}

test "a private ABI definition and a public declaration report only the definition" {
    var arena = std.heap.ArenaAllocator.init(testing.allocator);
    defer arena.deinit();
    const source_text = try std.mem.replaceOwned(
        u8,
        arena.allocator(),
        good_source,
        "[[gnu::noinline]] uint32_t",
        "static uint32_t",
    );
    const findings = try findingsFor(&arena, good_build, good_patch, good_header, source_text);
    try testing.expectEqual(@as(usize, 1), findings.len);
    try testing.expectEqualStrings(
        "ABI: source does not define externally visible ra8_mdl_service_component_abi(void)",
        findings[0],
    );
}

test "a missing weak extension point is reported against the patch" {
    var arena = std.heap.ArenaAllocator.init(testing.allocator);
    defer arena.deinit();
    const patch_text = try std.mem.replaceOwned(
        u8,
        arena.allocator(),
        good_patch,
        "+__attribute__((weak)) esp_err_t",
        "+esp_err_t",
    );
    const findings = try findingsFor(&arena, good_build, patch_text, good_header, good_source);
    try testing.expectEqual(@as(usize, 1), findings.len);
    try testing.expectEqualStrings(
        "hook: patch does not provide weak extension point esp_hosted_custom_rpc_sync_handler",
        findings[0],
    );
}

test "staging findings precede component findings, which precede symbol findings" {
    var arena = std.heap.ArenaAllocator.init(testing.allocator);
    defer arena.deinit();
    const findings = try findingsFor(&arena, "", "", "", "");
    try testing.expect(std.mem.startsWith(u8, findings[0], "staging:"));
    var first_component: usize = 0;
    var first_symbol: usize = 0;
    for (findings, 0..) |finding, index| {
        if (first_component == 0 and std.mem.startsWith(u8, finding, "component:")) first_component = index;
        if (first_symbol == 0 and (std.mem.startsWith(u8, finding, "ABI:") or std.mem.startsWith(u8, finding, "hook:"))) {
            first_symbol = index;
        }
    }
    try testing.expect(first_component > 0);
    try testing.expect(first_symbol > first_component);
}

test "an empty everything reports every seam exactly once" {
    var arena = std.heap.ArenaAllocator.init(testing.allocator);
    defer arena.deinit();
    const findings = try findingsFor(&arena, "", "", "", "");
    // one empty-recipe line, five missing required sources, two component
    // findings, and six symbol findings.
    try testing.expectEqual(@as(usize, 14), findings.len);
}

test "whitespace helpers answer the Python sets" {
    try testing.expect(implementation.isPythonSpace(0x0b));
    try testing.expect(implementation.isPythonSpace(0x1f));
    try testing.expect(implementation.isPythonSpace(0x2028));
    try testing.expect(!implementation.isPythonSpace('x'));
    try testing.expectEqual(@as(usize, 3), implementation.spaceRun("  \tx", 0));
    try testing.expectEqual(@as(usize, 2), implementation.blankRun(" \t\u{a0}", 0));
}

test "line anchors follow the multiline regex definition, not splitlines" {
    try testing.expect(implementation.atLineStart("a\nb", 2));
    try testing.expect(!implementation.atLineStart("a\rb", 2));
    try testing.expect(implementation.atLineEnd("a\nb", 1));
    try testing.expect(!implementation.atLineEnd("a\rb", 1));
    try testing.expect(implementation.atLineEnd("ab", 2));
}

test "text-mode line terminators collapse before any pattern runs" {
    const collapsed = try implementation.normalizeTerminators(testing.allocator, "a\r\nb\rc\nd\r\n");
    defer testing.allocator.free(collapsed);
    try testing.expectEqualStrings("a\nb\nc\nd\n", collapsed);
}

test "a carriage return before the line end no longer hides the component name" {
    var arena = std.heap.ArenaAllocator.init(testing.allocator);
    defer arena.deinit();
    const raw = "COMPONENT_DIR=\"${PERIPHERAL_DIR}/components/mdl_service\"\r\n";
    try testing.expect(implementation.findComponentName(raw) == null);
    const collapsed = try implementation.normalizeTerminators(arena.allocator(), raw);
    try testing.expectEqualStrings("mdl_service", implementation.findComponentName(collapsed).?);
}
