// SPDX-License-Identifier: GPL-2.0-or-later
import SwiftUI

struct LauncherView: View {
    /// The window's content size (main.swift).
    static let size = CGSize(width: 920, height: 680)

    @ObservedObject var model: LauncherModel

    var body: some View {
        VStack(spacing: 0) {
            // Two columns so the whole window fits a 13" MacBook Air (1440 x 900 points, Dock shown)
            // without scrolling, even with every optional row showing.
            HStack(alignment: .top, spacing: 0) {
                Form {
                    Section("Display") {
                        Picker("Output resolution", selection: $model.settings.output) {
                            ForEach(model.outputChoices, id: \.self) { Text($0.description).tag($0) }
                        }
                        Picker("Window", selection: $model.settings.fullscreen) {
                            Text("Windowed").tag(false)
                            Text("Full screen").tag(true)
                        }
                        .pickerStyle(.segmented)
                        Picker("Frame rate cap", selection: $model.settings.frameCap) {
                            ForEach(FrameCap.allCases) { Text($0.label).tag($0) }
                        }
                        .pickerStyle(.segmented)
                        if model.settings.frameCap.rawValue > 60 {
                            Text("Above 60 FPS is experimental: the game was made for 30. 120 needs a 120 Hz display.")
                                .font(.caption).foregroundStyle(.secondary)
                        }
                    }
                    Section("Upscaler") {
                        Picker("Upscaler", selection: $model.settings.upscaler) {
                            ForEach(model.upscalerChoices) { Text($0.label).tag($0) }
                        }
                        Picker("Quality", selection: $model.settings.preset) {
                            ForEach(Resolution.presetNames.indices, id: \.self) { Text(Resolution.presetNames[$0]).tag($0) }
                        }
                        .disabled(!model.settings.upscaler.usesPreset)
                        LabeledContent("Renders", value: "\(model.settings.renderSize) → \(model.settings.output)")
                        if model.settings.upscaler == .fsr3 && model.settings.renderer == .metal {
                            Text("FSR 3.1 returns to the Metal renderer in a later 0.5 update; MetalFX runs until then (your choice stays saved).")
                                .font(.caption).foregroundStyle(.secondary)
                        }
                        if model.settings.upscaler == .fsr3 {
                            Toggle("Sharpening", isOn: $model.settings.sharpen)
                            if model.settings.sharpen {
                                Slider(value: $model.settings.sharpness, in: 0...2) { Text("Strength") }
                            }
                        }
                        Picker("Frame generation", selection: $model.frameGeneration) {
                            ForEach(FrameGeneration.allCases) { Text($0.label).tag($0) }
                        }
                        .pickerStyle(.segmented)
                        .help("MetalFX frame interpolation: a generated frame between every two real ones (2× the frames on screen, a little more input lag)")
                        if let note = model.frameGenNote {
                            Text(note).font(.caption).foregroundStyle(.secondary)
                        }
                    }
                    Section("Advanced") {
                        if model.layout.hasMetalRenderer {
                            Picker("Renderer", selection: $model.settings.renderer) {
                                ForEach(Renderer.allCases) { Text($0.label).tag($0) }
                            }
                        }
                        Picker("Shader compilation", selection: $model.settings.shaderCompile) {
                            ForEach(ShaderCompile.allCases) { Text($0.label).tag($0) }
                        }
                    }
                }
                .formStyle(.grouped)
                Form {
                    Section("Effects") {
                        ForEach(GameSettings.visualEffects) { effect in
                            Toggle(effect.label, isOn: Binding(get: { model.effect(effect.key) },
                                                               set: { model.setEffect(effect.key, $0) }))
                        }
                        Toggle("Show FPS", isOn: $model.settings.showFPS)
                        Toggle("Metal Performance HUD", isOn: $model.settings.metalHUD)
                            .help("Apple's Metal HUD: FPS, GPU time and frame pacing, top right")
                        Toggle("Skip intro videos", isOn: Binding(get: { model.effect("skip_intro") },
                                                                  set: { model.setEffect("skip_intro", $0) }))
                    }
                    Section("Locations") {
                        FolderRow(title: "Game folder", path: model.config.gameDir?.path ?? "Not chosen",
                                  problem: model.checkingGame ? nil : model.gameProblem,
                                  choose: model.chooseGameFolder, reset: model.resetGameFolder)
                        FolderRow(title: "Save folder", path: model.saveDir.path, problem: model.saveProblem,
                                  choose: model.chooseSaveFolder, reset: model.resetSaveFolder)
                    }
                }
                .formStyle(.grouped)
            }
            Divider()
            HStack(spacing: 12) {
                Toggle("Show launcher every time", isOn: $model.config.showLauncher)
                Spacer()
                if let message = model.exitMessage {
                    Text(message).foregroundStyle(.red).lineLimit(2)
                    Button("Show Log", action: model.showLog)
                }
                Button(action: model.play) {
                    Text(model.running ? "Running…" : "Play").frame(minWidth: 80)
                }
                .keyboardShortcut(.defaultAction)
                .controlSize(.large)
                .disabled(!model.canPlay)
            }
            .padding()
        }
        .frame(width: LauncherView.size.width, height: LauncherView.size.height)
    }
}

private struct FolderRow: View {
    let title: String
    let path: String
    let problem: String?
    let choose: () -> Void
    let reset: () -> Void

    var body: some View {
        VStack(alignment: .leading, spacing: 4) {
            HStack {
                Text(title)
                Spacer()
                Button("Choose…", action: choose)
                Button("Reset to Default", action: reset)
            }
            Text(path).font(.caption).foregroundStyle(.secondary).textSelection(.enabled)
                .lineLimit(1).truncationMode(.middle).help(path)
            if let problem {
                Text(problem).font(.caption).foregroundStyle(.red)
            }
        }
    }
}
