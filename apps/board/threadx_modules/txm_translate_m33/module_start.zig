//! The translation module's shell (RA8FW-680, under RA8FW-1186): the module
//! app packaged with the permission set the translation job runs under,
//! display and storage, and no network, because translation is offline by
//! design. The job itself (RA8FW-681 and friends) plugs into `run` below;
//! the shell's own pass proves the granted surface works and the manifest
//! travels: the API generation, the screen region, one fill, a file written
//! under the module's own directory and read back, then `exit(0)`.
//!
//! Like the tour module, every pointer argument comes from the module's data
//! region, so the storage strings live in initialized statics, and the
//! rebase pass builds it through the C backend. The manifest identity (app
//! id, display|storage capabilities) lives in the `.ra8app` the build packs;
//! the resident image admits it before the shell starts, and a grant that
//! withholds either bit refuses the image at admission.

const std = @import("std");
const api = @import("ra8_app_api");
const Api = api.Api;
const abi = api.abi;

/// The module's own request entry point.
const calls = Api(api.target);

/// The steps, in shell order. A failure exits with the failing step's
/// number, and the host's log names it.
const Step = enum(u32) {
    version = 1,
    screen_info = 2,
    draw_rect = 3,
    file_write = 4,
    file_read = 5,

    /// The last step. One past it is `exit(0)`.
    pub const last: Step = .file_read;
};

/// The file the shell writes and reads back under the module's own
/// directory: the storage sidecar slot the translation job fills later.
var sidecar_path: ["sidecar.txt".len]u8 = "sidecar.txt".*;
var sidecar_text: ["translate shell ready".len]u8 = "translate shell ready".*;

fn run() u32 {
    // The generation the resident image serves.
    if (calls.apiVersion() != abi.version) return @backingInt(Step.version);

    // The region is the host's fixture: 64x64, four bytes a pixel.
    const screen = calls.screenInfo() catch return @backingInt(Step.screen_info);
    if (screen.width == 0 or screen.height == 0 or screen.bytes_per_pixel != 4) {
        return @backingInt(Step.screen_info);
    }

    // Display: one fill over the whole region.
    calls.drawRect(.{ .w = screen.width, .h = screen.height }, 0x2E4057FF) catch {
        return @backingInt(Step.draw_rect);
    };

    // Storage: write the sidecar file, close it, open it again, read the
    // bytes back.
    const text: []const u8 = &sidecar_text;
    const written = calls.fileOpen(&sidecar_path, abi.OpenFlag.read | abi.OpenFlag.write | abi.OpenFlag.create) catch {
        return @backingInt(Step.file_write);
    };
    const put = calls.fileWrite(written, text) catch return @backingInt(Step.file_write);
    if (put != text.len) return @backingInt(Step.file_write);
    calls.fileClose(written) catch return @backingInt(Step.file_write);

    const opened = calls.fileOpen(&sidecar_path, abi.OpenFlag.read) catch {
        return @backingInt(Step.file_read);
    };
    var buffer: [text.len]u8 = undefined;
    const got = calls.fileRead(opened, &buffer) catch return @backingInt(Step.file_read);
    if (got != text.len or !std.mem.eql(u8, &buffer, text)) return @backingInt(Step.file_read);
    calls.fileClose(opened) catch return @backingInt(Step.file_read);

    return 0;
}

export fn demo_module_start(id: u32) callconv(.c) noreturn {
    _ = id;
    const code = run();
    calls.exit(code) catch {};
    // The host stops and unloads the module on the exit request; until
    // then the start thread idles.
    while (true) asm volatile ("wfi");
}
