// SPDX-License-Identifier: GPL-2.0-or-later
import Foundation

/// bbport (local build): frame generation, MetalFX frame interpolation in the Metal renderer
/// (gpu/shim/bbport_framegen.h). Kept in <data>/framegen.ini (frame_generation=0|1), which
/// run.sh turns into BB_FRAME_GENERATION; not in bbport.ini, which the in-game menu rewrites with
/// its own keys only. MetalFX generates one frame between two real ones: 2x at most.
enum FrameGeneration: Int, CaseIterable, Identifiable {
    case off = 0
    case x2 = 1

    var id: Int { rawValue }
    var label: String { self == .off ? "Off" : "2×" }

    static let fileName = "framegen.ini"
    static let newFileHeader = "# Frame generation (MetalFX frame interpolation): 1 = on (2x), 0 = off. Set in the launcher.\n"

    /// A missing file or key is on: run.sh's default.
    init(ini: IniFile) {
        self = ini["frame_generation"].map { (Int($0) ?? 0) != 0 ? .x2 : .off } ?? .x2
    }

    func apply(to ini: inout IniFile) {
        ini["frame_generation"] = String(rawValue)
    }

    /// Why frame generation will not run with these settings, or a hint; nil when all is well.
    static func note(_ value: FrameGeneration, settings: GameSettings, hasMetalRenderer: Bool) -> String? {
        guard value != .off else { return nil }
        if settings.upscaler == .off {
            return "Frame generation needs an upscaler (FSR 3.1, MetalFX or TAA): no frames are generated while it is off."
        }
        if hasMetalRenderer && settings.renderer != .metal {
            return "Frame generation needs the Metal renderer."
        }
        if settings.frameCap.rawValue > 60 {
            return "With frame generation, use the 60 cap: each real frame gets a generated one, 120 on a 120 Hz display."
        }
        return nil
    }
}
