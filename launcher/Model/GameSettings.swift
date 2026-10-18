// SPDX-License-Identifier: GPL-2.0-or-later
import Foundation

enum Upscaler: String, CaseIterable, Identifiable {
    case off, fsr3, fsr4, fsr411, taa, metalfx

    var id: String { rawValue }
    var label: String {
        switch self {
        case .off: "Off"
        case .fsr3: "FSR 3.1"
        case .fsr4: "FSR 4"
        case .fsr411: "FSR 4.1.1"
        case .taa: "TAA"
        case .metalfx: "MetalFX"
        }
    }
    /// The quality level sets the render size (TAA always renders at the output size).
    var usesPreset: Bool { self != .off && self != .taa }
    /// Offered in the launcher; FSR 4 needs extra assets and a GPU check (in-game menu only).
    static let launcherChoices: [Upscaler] = [.off, .fsr3, .metalfx, .taa]
}

enum FrameCap: Int, CaseIterable, Identifiable {
    case fps30 = 30, fps40 = 40, fps60 = 60, fps90 = 90, fps120 = 120
    var id: Int { rawValue }
    var label: String { "\(rawValue)" }
}

enum ShaderCompile: String, CaseIterable, Identifiable {
    case balanced, wait, skip
    var id: String { rawValue }
    var label: String {
        switch self {
        case .balanced: "Balanced"
        case .wait: "Wait (no pop-in, stutters)"
        case .skip: "Skip (never stutters, pop-in)"
        }
    }
}

enum Renderer: String, CaseIterable, Identifiable {
    case vulkan, metal
    var id: String { rawValue }
    var label: String { self == .metal ? "Metal" : "MoltenVK (Vulkan)" }
}

struct Effect: Identifiable {
    let key: String
    let label: String
    let defaultOn: Bool
    var id: String { key }
}

/// The launcher's view of bbport.ini (the file the in-game menu writes too). Keys it does not
/// know stay in the IniFile; values it does not offer (an unlisted output, FSR 4) are kept as read.
struct GameSettings: Equatable {
    /// gpu/shim/bbport_settings.h Effects (without the debug ones), same defaults.
    static let effects = [
        Effect(key: "effect_dof", label: "Depth of field", defaultOn: true),
        Effect(key: "effect_motion_blur", label: "Motion blur", defaultOn: true),
        Effect(key: "effect_chromatic_aberration", label: "Chromatic aberration", defaultOn: true),
        Effect(key: "effect_ssao", label: "Ambient occlusion (SSAO)", defaultOn: true),
        Effect(key: "effect_dynamic_shadows", label: "Shadows from dynamic lights", defaultOn: true),
        Effect(key: "effect_ssr", label: "Screen-space reflections (not in the original)", defaultOn: false),
        Effect(key: "effect_game_aa", label: "The game's own anti-aliasing", defaultOn: true),
        Effect(key: "skip_intro", label: "Skip intro videos", defaultOn: false),
    ]
    static var visualEffects: [Effect] { effects.filter { $0.key != "skip_intro" } }
    static let newFileHeader = "# bbport settings (in-game menu: Cmd+, / L3+R3)\n"

    var output = Resolution.defaultOutput
    var upscaler = Upscaler.off
    var preset = 0
    var sharpen = true
    var sharpness = 0.3
    var showFPS = false
    var metalHUD = false
    var effects = Dictionary(uniqueKeysWithValues: GameSettings.effects.map { ($0.key, $0.defaultOn) })
    var frameCap = FrameCap.fps60
    var fullscreen = false
    var renderer = Renderer.vulkan
    var shaderCompile = ShaderCompile.balanced

    init() {}

    init(ini: IniFile) {
        // Booleans as the game reads them (atoi(value) != 0).
        func flag(_ key: String) -> Bool? { ini[key].map { (Int($0) ?? 0) != 0 } }
        if let v = ini["output_res"], let size = PixelSize(iniValue: v) { output = size }
        if let v = ini["upscaler"], let u = Upscaler(rawValue: v) { upscaler = u }
        if let v = ini["preset"], let p = Int(v), Resolution.presetScales.indices.contains(p) { preset = p }
        if let v = flag("sharpen") { sharpen = v }
        if let v = ini["sharpness"], let s = Double(v), (0...2).contains(s) { sharpness = s }
        if let v = flag("show_fps") { showFPS = v }
        if let v = flag("metal_hud") { metalHUD = v }
        for effect in Self.effects {
            if let v = flag(effect.key) { effects[effect.key] = v }
        }
        if let v = ini["fps_cap"], let n = Int(v), let cap = FrameCap(rawValue: n) { frameCap = cap }
        if let v = flag("fullscreen") { fullscreen = v }
        if let v = ini["renderer"], let r = Renderer(rawValue: v) { renderer = r }
        if let v = ini["shader_compile"], let s = ShaderCompile(rawValue: v) { shaderCompile = s }
    }

    /// Writes the launcher's settings into `ini`; every other line stays. `includeRenderer`:
    /// the build has the Metal renderer (otherwise no renderer key is written).
    func apply(to ini: inout IniFile, includeRenderer: Bool) {
        ini["output_res"] = output.iniValue
        ini["upscaler"] = upscaler.rawValue
        ini["preset"] = String(preset)
        ini["sharpen"] = sharpen ? "1" : "0"
        ini["sharpness"] = String(format: "%.2f", sharpness)
        ini["show_fps"] = showFPS ? "1" : "0"
        ini["metal_hud"] = metalHUD ? "1" : "0"
        for effect in Self.effects {
            ini[effect.key] = (effects[effect.key] ?? effect.defaultOn) ? "1" : "0"
        }
        ini["fps_cap"] = String(frameCap.rawValue)
        ini["fullscreen"] = fullscreen ? "1" : "0"
        if includeRenderer {
            ini["renderer"] = renderer.rawValue
        }
        ini["shader_compile"] = shaderCompile.rawValue
    }

    /// The scene size the game renders.
    var renderSize: PixelSize {
        upscaler.usesPreset ? Resolution.renderSize(output: output, preset: preset) : output
    }
}
