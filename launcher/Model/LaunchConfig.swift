// SPDX-License-Identifier: GPL-2.0-or-later
import Foundation

enum Shell {
    /// Single-quoted for bash: it's -> 'it'\''s'.
    static func quote(_ s: String) -> String {
        "'" + s.replacingOccurrences(of: "'", with: "'\\''") + "'"
    }

    /// The value of a shell word: single quotes, double quotes and backslash escapes (what
    /// quote() and the earlier launchers' printf %q wrote).
    static func unquote(_ s: String) -> String {
        var out = ""
        var single = false, double = false, escaped = false
        for c in s {
            if escaped {
                out.append(c)
                escaped = false
            } else if single {
                if c == "'" { single = false } else { out.append(c) }
            } else if c == "\\" {
                escaped = true
            } else if c == "'" && !double {
                single = true
            } else if c == "\"" {
                double.toggle()
            } else {
                out.append(c)
            }
        }
        return out
    }
}

/// launch.conf: the launcher's own settings as shell assignments (bash sources the file in
/// Bloodborne.command): BB_GAME_DIR, BB_SAVE_DIR (absent: the default), BB_SHOW_LAUNCHER.
/// Other assignments in it (BB_DATA_DIR, BB_FPS, ...) stay in the IniFile.
struct LaunchConfig: Equatable {
    var gameDir: URL?
    var saveDir: URL?
    var showLauncher = true
    private(set) var dataDir: URL?

    init() {}

    init(ini: IniFile) {
        func path(_ key: String) -> URL? {
            guard let v = ini[key].map(Shell.unquote), !v.isEmpty else { return nil }
            return URL(fileURLWithPath: v)
        }
        gameDir = path("BB_GAME_DIR")
        saveDir = path("BB_SAVE_DIR")
        dataDir = path("BB_DATA_DIR")
        if let v = ini["BB_SHOW_LAUNCHER"] { showLauncher = Shell.unquote(v) != "0" }
    }

    func apply(to ini: inout IniFile) {
        ini["BB_GAME_DIR"] = gameDir.map { Shell.quote($0.path) }
        ini["BB_SAVE_DIR"] = saveDir.map { Shell.quote($0.path) }
        ini["BB_SHOW_LAUNCHER"] = showLauncher ? "1" : "0"
    }
}
