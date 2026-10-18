#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Bloodborne launcher for the native port (GTK4 / libadwaita).

Picks the game folder, edits the port's settings (bbport.ini: upscaler, preset, ...) and the
start-up tweaks passed as environment variables to run.sh, starts and stops the game and shows
its output. Launcher settings live in ~/.config/bbport-launcher/settings.json.
Linux reference path (GTK4/libadwaita); not used by the macOS build yet.
"""

import json
import os
import signal
import sys
from pathlib import Path
from bbport_assets import fsr411_problem

import gi

gi.require_version("Gtk", "4.0")
gi.require_version("Adw", "1")
from gi.repository import Adw, Gio, GLib, Gtk  # noqa: E402

PORT_DIR = Path(__file__).resolve().parent.parent  # the repository root (or the package's copy)
sys.path.insert(0, str(PORT_DIR / 'scripts'))
from mods import discover as discover_mods  # noqa: E402
from patches import external_patches  # noqa: E402
# Packaged (AppImage): generated files, saves and bbport.ini live in BB_DATA_DIR.
PACKAGED = bool(os.environ.get("BB_PREBUILT"))
DATA_DIR = Path(os.environ.get("BB_DATA_DIR", PORT_DIR))
CONFIG_DIR = Path(os.environ.get("XDG_CONFIG_HOME", Path.home() / ".config")) / "bbport-launcher"
CONFIG_FILE = CONFIG_DIR / "settings.json"
MAX_LOG_LINES = 5000

# Choices: (label, value). The first entry is the default.
UPSCALERS = [("FSR 4", "fsr4"), ("FSR 4.1.1", "fsr411"), ("FSR 3", "fsr3"),
             ("TAA (native anti-aliasing)", "taa"), ("Off", "off")]
PRESETS = [("Native AA", 0), ("Quality (x1.5)", 1), ("Balanced (x1.7)", 2),
           ("Performance (x2)", 3), ("Ultra Performance (x3)", 4)]
OUTPUT_RES = [("1280×720 (Steam Deck)", "1280x720"), ("1920×1080", "1920x1080"), ("2560×1440", "2560x1440"), ("3840×2160", "3840x2160")]
# Game effects (patches applied at start): bbport.ini key, title, default.
EFFECTS = [
    ("effect_chromatic_aberration", "Chromatic aberration", True),
    ("effect_dof", "Depth of field (DoF)", True),
    ("effect_motion_blur", "Motion blur", True),
    ("effect_ssao", "SSAO", True),
    ("effect_game_aa", "The game's own anti-aliasing", True),
    ("effect_dynamic_shadows", "Dynamic light shadows", True),
    ("effect_ssr", "SSR reflections (not in the original game)", False),
    ("skip_intro", "Skip the intro videos", False),
    ("debug_camera", "Free camera (Cross + L3 / Space + Z)", False),
    ("debug_menu", "Debug menu (left touchpad / Tab; needs fonts)", False),
]
MODEL_LOD = [("As in the game", "0"), ("Highest (-2)", "-2"), ("Lower (1)", "1"),
             ("Lowest (2)", "2")]
FPS_MODES = [("Unlocked (patch)", "uncap"), ("60", "60"), ("90", "90"),
             ("30 (as on PS4)", "30")]
PRESENT_MODES = [("Mailbox", "Mailbox"), ("FIFO (VSync)", "Fifo"),
                 ("FIFO Relaxed", "FifoRelaxed"), ("Immediate", "Immediate")]
DRAW_PIPE = [("Auto (8+ threads)", ""), ("On", "1"), ("Off (more stable)", "0")]
LANGUAGES = [("English", "1"), ("Russian", "8"), ("Japanese", "0"), ("French", "2"),
             ("Spanish", "3"), ("German", "4"), ("Italian", "5")]
LIVE_RESOLUTION = [("Auto (by GPU)", "auto"), ("Off (faster)", "0"), ("On", "1")]
READBACKS = [("Relaxed (default)", ""), ("Off", "0"), ("Precise", "2")]

DEFAULTS = {
    "game_dir": "" if PACKAGED else str(PORT_DIR.parent / "CUSA03173"),
    "user_dir": "",
    "mods_dir": "",
    "mods_enabled": True,
    "patches_dir": "",
    "language": "1",
    "fullscreen": False,
    "hdr": False,
    "present_mode": "Mailbox",
    "fps_mode": "uncap",
    "fps_limit": 0,
    "draw_pipe": "",
    "readbacks": "",
    "mangohud": False,
    "frame_stats": False,
    "gpu_profile": False,
    "vk_validation": False,
    "extra_env": "",
}

# bbport.ini keys the launcher edits; the rest of the file is kept.
INI_DEFAULTS = {
    "upscaler": "fsr4",
    "preset": "4",
    "sharpen": "1",
    "sharpness": "0.50",
    "object_motion": "1",
    "show_fps": "1",
    "output_res": "1920x1080",
    "model_lod": "0",
    "live_resolution": "0",
    **{key: "1" if default else "0" for key, _, default in EFFECTS},
}


def load_settings():
    settings = dict(DEFAULTS)
    try:
        settings.update(json.loads(CONFIG_FILE.read_text()))
    except (OSError, ValueError):
        pass
    return settings


def save_settings(settings):
    CONFIG_DIR.mkdir(parents=True, exist_ok=True)
    CONFIG_FILE.write_text(json.dumps(settings, indent=2, ensure_ascii=False))


def ini_path():
    return Path(os.environ.get("BB_CONFIG", DATA_DIR / "bbport.ini"))


def load_ini():
    values = dict(INI_DEFAULTS)
    lines = []
    try:
        lines = ini_path().read_text().splitlines()
    except OSError:
        pass
    for line in lines:
        if "=" in line and not line.lstrip().startswith("#"):
            key, value = line.split("=", 1)
            values[key.strip()] = value.strip()
    return values, lines


def save_ini(values, lines):
    """Rewrites the edited keys in place, appends missing ones, keeps comments and others."""
    written = set()
    out = []
    for line in lines:
        if "=" in line and not line.lstrip().startswith("#"):
            key = line.split("=", 1)[0].strip()
            if key in values:
                out.append(f"{key}={values[key]}")
                written.add(key)
                continue
        out.append(line)
    if not lines:
        out.append("# bbport settings (in-game menu: Insert / L3+R3)")
    for key, value in values.items():
        if key not in written:
            out.append(f"{key}={value}")
    ini_path().write_text("\n".join(out) + "\n")


def patches_dir(settings):
    return Path(settings.get("patches_dir") or DATA_DIR / "patches").expanduser()


def game_environment(s):
    """Environment for run.sh from the launcher settings."""
    env = dict(os.environ)
    env["BB_GAME_DIR"] = str(Path(s["game_dir"]).expanduser())
    if s["user_dir"]:
        env["BB_USER_DIR"] = s["user_dir"]
    env["BB_MODS_DIR"] = str(Path(s.get("mods_dir") or DATA_DIR / "mods").expanduser())
    env["BB_MODS_CONFIG"] = str(DATA_DIR / "mods.json")
    env["BB_MODS_ENABLED"] = "1" if s.get("mods_enabled", True) else "0"
    env["BB_PATCHES_DIR"] = str(patches_dir(s))
    env["BB_PATCHES_CONFIG"] = str(DATA_DIR / "patches.json")
    env["BB_LANGUAGE"] = s["language"]
    env["BB_FULLSCREEN"] = "1" if s["fullscreen"] else "0"
    env["BB_PRESENT_MODE"] = s["present_mode"]
    if s["hdr"]:
        env["BB_HDR"] = "1"
    env["BB_FPS"] = s["fps_mode"]
    if s["fps_limit"] > 0:
        env["BB_FPS_LIMIT"] = str(s["fps_limit"])
    if s["draw_pipe"]:
        env["BB_DRAW_PIPE"] = s["draw_pipe"]
    if s["readbacks"]:
        env["BB_READBACKS"] = s["readbacks"]
    if s["mangohud"]:
        env["MANGOHUD"] = "1"
    if s["frame_stats"]:
        env["BB_FRAME_STATS"] = "1"
    if s["gpu_profile"]:
        env["BB_GPU_PROFILE"] = "1"
    if s["vk_validation"]:
        env["BB_VK_VALIDATION"] = "1"
    for item in s["extra_env"].split():
        if "=" in item:
            key, value = item.split("=", 1)
            env[key] = value
    return env


def flat_button(icon, tooltip, handler):
    button = Gtk.Button(icon_name=icon, valign=Gtk.Align.CENTER, tooltip_text=tooltip)
    button.add_css_class("flat")
    button.connect("clicked", handler)
    return button


def open_folder(window, path):
    """Opens `path` in the file manager (created first, so that a new saves folder opens)."""
    try:
        Path(path).mkdir(parents=True, exist_ok=True)
    except OSError:
        return
    Gtk.FileLauncher.new(Gio.File.new_for_path(str(path))).launch(window, None, None)


def combo_row(title, subtitle, choices, current):
    model = Gtk.StringList.new([label for label, _ in choices])
    row = Adw.ComboRow(title=title, model=model)
    if subtitle:
        row.set_subtitle(subtitle)
    values = [value for _, value in choices]
    row.set_selected(values.index(current) if current in values else 0)
    row.values = values
    return row


def combo_value(row):
    return row.values[row.get_selected()]


class FolderList:
    """A preferences group with a folder row and one switch row per item below it."""

    def __init__(self, group):
        self.group = group
        self.rows = []

    def clear(self):
        for _, row in self.rows:
            self.group.remove(row)
        self.rows = []

    def add(self, key, row):
        self.group.add(row)
        self.rows.append((key, row))


class LauncherWindow(Adw.ApplicationWindow):
    def __init__(self, app):
        super().__init__(application=app, title="Bloodborne")
        self.set_default_size(760, 820)
        self.settings = load_settings()
        self.ini, self.ini_lines = load_ini()
        self.process = None
        self.stream = None
        self.build()
        self.connect("close-request", self.on_close)

    def build(self):
        """Creates the window's content."""
        toolbar = Adw.ToolbarView()
        header = Adw.HeaderBar()
        self.stack = Adw.ViewStack()
        switcher = Adw.ViewSwitcher(stack=self.stack, policy=Adw.ViewSwitcherPolicy.WIDE)
        header.set_title_widget(switcher)
        self.launch_button = Gtk.Button()
        self.launch_button.connect("clicked", self.on_launch)
        header.pack_end(self.launch_button)
        toolbar.add_top_bar(header)

        self.toasts = Adw.ToastOverlay()
        self.toasts.set_child(self.stack)
        toolbar.set_content(self.toasts)
        self.set_content(toolbar)

        log_text = self.log_view.get_buffer().get_text(
            *self.log_view.get_buffer().get_bounds(), False) if hasattr(self, "log_view") else ""
        self.stack.add_titled_with_icon(self.build_settings_page(), "settings", "Settings",
                                        "preferences-system-symbolic")
        self.stack.add_titled_with_icon(self.build_log_page(), "log", "Log",
                                        "utilities-terminal-symbolic")
        self.log_view.get_buffer().set_text(log_text)
        self.update_launch_button()
        self.update_game_status()
        self.update_user_status()
        self.update_upscaler_status()

    def update_launch_button(self):
        running = self.process is not None
        self.launch_button.set_label("Stop" if running else "Play")
        self.launch_button.remove_css_class("destructive-action" if not running else "suggested-action")
        self.launch_button.add_css_class("destructive-action" if running else "suggested-action")

    # --- settings page -------------------------------------------------------------------

    def build_settings_page(self):
        page = Adw.PreferencesPage()

        game = Adw.PreferencesGroup(title="Game")
        self.game_row = Adw.ActionRow(title="Game folder (CUSA03173)")
        self.game_status = Gtk.Image()
        self.game_row.add_suffix(self.game_status)
        self.game_row.add_suffix(flat_button("folder-open-symbolic", "Choose the folder with eboot.bin",
                                             self.on_choose_game))
        self.game_row.add_suffix(flat_button("system-file-manager-symbolic",
                                             "Open in the file manager",
                                             lambda _b: open_folder(self, self.game_dir())))
        game.add(self.game_row)
        # Saves and the shader cache: user/ in the data directory unless chosen.
        self.user_row = Adw.ActionRow(title="Saves folder")
        self.user_status = Gtk.Image()
        self.user_row.add_suffix(self.user_status)
        self.user_row.add_suffix(flat_button("folder-open-symbolic", "Choose the saves folder",
                                             self.on_choose_user))
        self.user_row.add_suffix(flat_button("system-file-manager-symbolic",
                                             "Open in the file manager",
                                             lambda _b: open_folder(self, self.user_dir())))
        self.user_reset = flat_button("edit-undo-symbolic", "Back to the default folder",
                                      self.on_reset_user)
        self.user_row.add_suffix(self.user_reset)
        game.add(self.user_row)
        self.language_row = combo_row("System language", None, LANGUAGES, self.settings["language"])
        game.add(self.language_row)
        page.add(game)

        self.mods_group = Adw.PreferencesGroup(
            title="Mods", description=(
                "Extract each mod into its own folder (with dvdroot_ps4, or chr/, parts/, ... directly). "
                "When files collide, the mod lower in the list wins. Applied at start."))
        self.mods_enabled_row = Adw.SwitchRow(title="Load mods",
                                               active=self.settings["mods_enabled"])
        self.mods_group.add(self.mods_enabled_row)
        self.mods_folder_row = Adw.ActionRow(title="Mods folder")
        self.mods_folder_row.add_suffix(flat_button("folder-open-symbolic", "Choose the mods folder",
                                                    self.on_choose_mods))
        self.mods_folder_row.add_suffix(flat_button("system-file-manager-symbolic", "Open the mods folder",
                                                    lambda _b: open_folder(self, self.mods_dir())))
        self.mods_folder_row.add_suffix(flat_button("view-refresh-symbolic", "Refresh the list",
                                                    self.on_refresh_mods))
        self.mods_group.add(self.mods_folder_row)
        self.mod_list = FolderList(self.mods_group)
        self.refresh_mods()
        page.add(self.mods_group)

        self.patches_group = Adw.PreferencesGroup(
            title="Third-party patches",
            description="shadPS4-format XML patches for version 01.09 from the patches folder. "
                        "Applied at start.")
        self.patches_folder_row = Adw.ActionRow(title="Patches folder")
        self.patches_folder_row.add_suffix(flat_button("folder-open-symbolic", "Choose the patches folder",
                                                       self.on_choose_patches))
        self.patches_folder_row.add_suffix(flat_button(
            "system-file-manager-symbolic", "Open the patches folder",
            lambda _b: open_folder(self, patches_dir(self.settings))))
        self.patches_folder_row.add_suffix(flat_button("view-refresh-symbolic", "Refresh the list",
                                                       self.on_refresh_patches))
        self.patches_group.add(self.patches_folder_row)
        self.patch_list = FolderList(self.patches_group)
        self.refresh_patches()
        page.add(self.patches_group)

        screen = Adw.PreferencesGroup(title="Display")
        self.output_row = combo_row("Output resolution",
                                    "The upscaler fills the frame; Steam Deck: 720p",
                                    OUTPUT_RES, self.ini.get("output_res", "1920x1080"))
        screen.add(self.output_row)
        self.live_row = combo_row("Live resolution changes",
                                  "No restart needed, but slower on the Steam Deck and older GPUs",
                                  LIVE_RESOLUTION, self.ini.get("live_resolution", "0"))
        screen.add(self.live_row)
        self.fullscreen_row = Adw.SwitchRow(title="Fullscreen",
                                            active=self.settings["fullscreen"])
        screen.add(self.fullscreen_row)
        self.present_row = combo_row("Present mode", None, PRESENT_MODES,
                                     self.settings["present_mode"])
        screen.add(self.present_row)
        self.hdr_row = Adw.SwitchRow(title="Allow HDR", active=self.settings["hdr"])
        screen.add(self.hdr_row)
        page.add(screen)

        upscaler = Adw.PreferencesGroup(
            title="Upscaler",
            description="Stored in bbport.ini; in game, change it in the menu (Insert or L3+R3)")
        self.upscaler_row = combo_row("Upscaler", None, UPSCALERS, self.ini["upscaler"])
        self.upscaler_row.connect("notify::selected", lambda *_: self.update_upscaler_status())
        upscaler.add(self.upscaler_row)
        self.preset_row = combo_row("Preset", None, PRESETS, int(self.ini.get("preset", "4")))
        self.preset_row.connect("notify::selected", lambda *_: self.update_upscaler_status())
        self.output_row.connect("notify::selected", lambda *_: self.update_upscaler_status())
        upscaler.add(self.preset_row)
        self.sharpen_row = Adw.SwitchRow(title="Sharpening (RCAS)",
                                         active=self.ini.get("sharpen") == "1")
        upscaler.add(self.sharpen_row)
        self.sharpness_row = Adw.SpinRow.new_with_range(0.0, 2.0, 0.05)
        self.sharpness_row.set_title("Sharpness")
        self.sharpness_row.set_digits(2)
        self.sharpness_row.set_value(float(self.ini.get("sharpness", "0.5")))
        upscaler.add(self.sharpness_row)
        self.motion_row = Adw.SwitchRow(
            title="Object motion vectors",
            subtitle="Less ghosting on characters; costs about 10% FPS",
            active=self.ini.get("object_motion") == "1")
        upscaler.add(self.motion_row)
        self.show_fps_row = Adw.SwitchRow(title="Show FPS",
                                          active=self.ini.get("show_fps") == "1")
        upscaler.add(self.show_fps_row)
        page.add(upscaler)

        effects = Adw.PreferencesGroup(title="Game effects",
                                       description="Game patches, applied at start")
        self.lod_row = combo_row("Model detail", None, MODEL_LOD,
                                 self.ini.get("model_lod", "0"))
        effects.add(self.lod_row)
        self.effect_rows = {}
        for key, title, default in EFFECTS:
            row = Adw.SwitchRow(title=title,
                                active=self.ini.get(key, "1" if default else "0") == "1")
            if key == "debug_menu":
                row.set_subtitle("Install DbgFont14h.ccm and DbgFont14h.tpf into dvdroot_ps4/font "
                                 "from Nexus mod #253")
            self.effect_rows[key] = row
            effects.add(row)
        page.add(effects)

        frames = Adw.PreferencesGroup(title="Frame rate")
        self.fps_row = combo_row("Mode", "Which frame rate patch to apply to the game",
                                 FPS_MODES, self.settings["fps_mode"])
        frames.add(self.fps_row)
        self.limit_row = Adw.SpinRow.new_with_range(0, 480, 1)
        self.limit_row.set_title("FPS limit")
        self.limit_row.set_subtitle("0: the display refresh rate (up to 120 Hz); "
                                    "set a number for another limit")
        self.limit_row.set_value(self.settings["fps_limit"])
        frames.add(self.limit_row)
        page.add(frames)

        perf = Adw.PreferencesGroup(title="Performance")
        self.pipe_row = combo_row(
            "Two-stage GPU pipeline",
            "20–30% faster; turn off if unstable", DRAW_PIPE,
            self.settings["draw_pipe"])
        perf.add(self.pipe_row)
        self.readbacks_row = combo_row("GPU data readbacks by the CPU", None, READBACKS,
                                       self.settings["readbacks"])
        perf.add(self.readbacks_row)
        page.add(perf)

        dev = Adw.PreferencesGroup(title="Developer")
        self.mangohud_row = Adw.SwitchRow(title="MangoHud", active=self.settings["mangohud"])
        dev.add(self.mangohud_row)
        self.stats_row = Adw.SwitchRow(title="Frame statistics in the log",
                                       subtitle="BB_FRAME_STATS",
                                       active=self.settings["frame_stats"])
        dev.add(self.stats_row)
        self.profile_row = Adw.SwitchRow(title="GPU profile in the log",
                                         subtitle="BB_GPU_PROFILE",
                                         active=self.settings["gpu_profile"])
        dev.add(self.profile_row)
        self.validation_row = Adw.SwitchRow(title="Vulkan validation layers",
                                            subtitle="Much slower",
                                            active=self.settings["vk_validation"])
        dev.add(self.validation_row)
        self.extra_row = Adw.EntryRow(title="Extra variables (NAME=value, space-separated)",
                                      text=self.settings["extra_env"])
        dev.add(self.extra_row)
        page.add(dev)
        return page

    def game_dir(self):
        return Path(self.settings["game_dir"]).expanduser()

    def user_dir(self):
        return Path(self.settings["user_dir"]).expanduser() if self.settings["user_dir"] \
            else DATA_DIR / "user"

    def update_user_status(self):
        path = self.user_dir()
        custom = bool(self.settings["user_dir"])
        self.user_row.set_subtitle(str(path) if custom else "Default: {}".format(path))
        saves = path / "savedata"
        found = saves.is_dir() and any(saves.iterdir())
        self.user_status.set_from_icon_name("object-select-symbolic" if found else "document-new-symbolic")
        self.user_status.set_tooltip_text("Saves found" if found
                                          else "No saves yet: the game will create them here")
        self.user_reset.set_sensitive(custom)

    def choose_folder(self, title, current, done):
        dialog = Gtk.FileDialog(title=title)
        if Path(current).is_dir():
            dialog.set_initial_folder(Gio.File.new_for_path(str(current)))

        def finish(dialog, result):
            try:
                folder = dialog.select_folder_finish(result)
            except GLib.Error:
                return
            if folder:
                done(folder.get_path())
        dialog.select_folder(self, None, finish)

    def on_choose_user(self, _button):
        def chosen(path):
            self.settings["user_dir"] = path
            self.update_user_status()
            self.store()
        self.choose_folder("Saves folder", self.user_dir(), chosen)

    def on_reset_user(self, _button):
        self.settings["user_dir"] = ""
        self.update_user_status()
        self.store()

    def update_upscaler_status(self):
        value = combo_value(self.upscaler_row)
        if value == "fsr4":
            ok = (PORT_DIR / "fsr4_shaders").is_dir()
            hint = "Assets found" if ok else "No assets: tools/fetch_fsr4_assets.sh"
        elif value == "fsr411":
            directory = Path(os.environ["BB_FSR411_DIR"]) if os.environ.get("BB_FSR411_DIR") else (
                PORT_DIR / "fsr4_411" if (PORT_DIR / "fsr4_411").is_dir() else DATA_DIR / "fsr4_411")
            problem = fsr411_problem(directory, combo_value(self.output_row),
                                     int(combo_value(self.preset_row)))
            hint = "Assets for the selected mode found" if not problem else (
                "{}. Install the full fsr4_411 set into {}.".format(problem, directory))
        else:
            hint = "Anti-aliasing at the output resolution, no FSR model" if value == "taa" else None
        self.preset_row.set_sensitive(value not in ("taa", "off"))
        self.sharpen_row.set_sensitive(value != "off")
        self.sharpness_row.set_sensitive(value != "off")
        self.upscaler_row.set_subtitle(hint or "")

    def update_game_status(self):
        path = self.game_dir()
        ok = bool(self.settings["game_dir"]) and (path / "eboot.bin").is_file()
        self.game_row.set_subtitle(str(path) if self.settings["game_dir"] else "not chosen")
        self.game_status.set_from_icon_name("object-select-symbolic" if ok else "dialog-warning-symbolic")
        self.game_status.set_tooltip_text("eboot.bin found" if ok else "No eboot.bin in the folder")
        self.launch_button.set_sensitive(ok or self.process is not None)

    def on_choose_game(self, _button):
        def chosen(path):
            self.settings["game_dir"] = path
            self.update_game_status()
            self.store()
        self.choose_folder("Game folder (with eboot.bin)", self.game_dir(), chosen)

    def store(self):
        s = self.settings
        s["language"] = combo_value(self.language_row)
        s["fullscreen"] = self.fullscreen_row.get_active()
        s["present_mode"] = combo_value(self.present_row)
        s["hdr"] = self.hdr_row.get_active()
        s["fps_mode"] = combo_value(self.fps_row)
        s["fps_limit"] = int(self.limit_row.get_value())
        s["draw_pipe"] = combo_value(self.pipe_row)
        s["readbacks"] = combo_value(self.readbacks_row)
        s["mangohud"] = self.mangohud_row.get_active()
        s["frame_stats"] = self.stats_row.get_active()
        s["gpu_profile"] = self.profile_row.get_active()
        s["vk_validation"] = self.validation_row.get_active()
        s["extra_env"] = self.extra_row.get_text().strip()
        s["mods_enabled"] = self.mods_enabled_row.get_active()
        self.save_mod_profile()
        self.save_patch_profile()
        save_settings(s)
        self.ini.update({
            "upscaler": combo_value(self.upscaler_row),
            "preset": str(combo_value(self.preset_row)),
            "sharpen": "1" if self.sharpen_row.get_active() else "0",
            "sharpness": f"{self.sharpness_row.get_value():.2f}",
            "object_motion": "1" if self.motion_row.get_active() else "0",
            "show_fps": "1" if self.show_fps_row.get_active() else "0",
            "output_res": combo_value(self.output_row),
            "model_lod": combo_value(self.lod_row),
            "live_resolution": combo_value(self.live_row),
            **{key: "1" if row.get_active() else "0" for key, row in self.effect_rows.items()},
        })
        save_ini(self.ini, self.ini_lines)
        self.ini, self.ini_lines = load_ini()

    def environment(self):
        return game_environment(self.settings)

    # --- mods ----------------------------------------------------------------------------

    def mods_dir(self):
        return Path(self.settings.get("mods_dir") or DATA_DIR / "mods").expanduser()

    def save_mod_profile(self):
        DATA_DIR.mkdir(parents=True, exist_ok=True)
        rows = self.mod_list.rows
        profile = {"order": [name for name, _ in rows],
                   "disabled": [name for name, row in rows if not row.get_active()]}
        (DATA_DIR / "mods.json").write_text(json.dumps(profile, indent=2, ensure_ascii=False) + "\n")

    def refresh_mods(self):
        self.mod_list.clear()
        self.mods_folder_row.set_subtitle(str(self.mods_dir()))
        try:
            profile = json.loads((DATA_DIR / "mods.json").read_text())
        except (OSError, ValueError):
            profile = {}
        available = discover_mods(self.mods_dir())
        order = list(dict.fromkeys(n for n in [*profile.get("order", []), *available] if n in available))
        for name in order:
            row = Adw.SwitchRow(title=name, active=name not in profile.get("disabled", []))
            row.add_suffix(flat_button("go-up-symbolic", "Load earlier",
                                        lambda _b, n=name: self.move_mod(n, -1)))
            row.add_suffix(flat_button("go-down-symbolic", "Load later",
                                        lambda _b, n=name: self.move_mod(n, 1)))
            self.mod_list.add(name, row)
        if not order:
            self.mods_folder_row.set_subtitle(f"{self.mods_dir()} — {'No mods'}")

    def move_mod(self, name, direction):
        rows = self.mod_list.rows
        index = next(i for i, (n, _) in enumerate(rows) if n == name)
        target = index + direction
        if 0 <= target < len(rows):
            rows[index], rows[target] = rows[target], rows[index]
            self.save_mod_profile()
            self.refresh_mods()

    def on_refresh_mods(self, _button):
        self.save_mod_profile()
        self.refresh_mods()

    def on_choose_mods(self, _button):
        self.save_mod_profile()

        def chosen(path):
            self.settings["mods_dir"] = path
            self.refresh_mods()
            self.store()
        self.choose_folder("Mods folder", self.mods_dir(), chosen)

    # --- third-party patches -------------------------------------------------------------

    def save_patch_profile(self):
        """patches.json: the switches that differ from each file's isEnabled."""
        enabled, disabled = [], []
        for key, row in self.patch_list.rows:
            if row.get_active() != row.default:
                (enabled if row.get_active() else disabled).append(key)
        path = DATA_DIR / "patches.json"
        try:
            profile = json.loads(path.read_text())
        except (OSError, ValueError):
            profile = {}
        # Patches of files not listed now (another folder) keep their choice.
        shown = {key for key, _ in self.patch_list.rows}
        profile = {"enabled": sorted({k for k in profile.get("enabled", []) if k not in shown} | set(enabled)),
                   "disabled": sorted({k for k in profile.get("disabled", []) if k not in shown} | set(disabled))}
        DATA_DIR.mkdir(parents=True, exist_ok=True)
        path.write_text(json.dumps(profile, indent=2, ensure_ascii=False) + "\n")

    def refresh_patches(self):
        self.patch_list.clear()
        directory = patches_dir(self.settings)
        try:
            profile = json.loads((DATA_DIR / "patches.json").read_text())
        except (OSError, ValueError):
            profile = {}
        found = external_patches(directory)
        for key, _path, meta in found:
            default = meta.get("isEnabled", "false").lower() == "true"
            active = key in profile.get("enabled", []) or (default and key not in profile.get("disabled", []))
            subtitle = key.split("/", 1)[0]
            if meta.get("Author"):
                subtitle += " · " + "Author: {}".format(meta.get("Author"))
            row = Adw.SwitchRow(title=GLib.markup_escape_text(meta.get("Name") or key),
                                subtitle=GLib.markup_escape_text(subtitle), active=active)
            if meta.get("Note"):
                row.set_tooltip_text(meta.get("Note").replace("\\n", "\n"))
            row.default = default
            self.patch_list.add(key, row)
        self.patches_folder_row.set_subtitle(
            str(directory) if found else f"{directory} — {'No patches'}")

    def on_refresh_patches(self, _button):
        self.save_patch_profile()
        self.refresh_patches()

    def on_choose_patches(self, _button):
        self.save_patch_profile()

        def chosen(path):
            self.settings["patches_dir"] = path
            self.refresh_patches()
            self.store()
        self.choose_folder("Patches folder", patches_dir(self.settings), chosen)

    # --- log page ------------------------------------------------------------------------

    def build_log_page(self):
        box = Gtk.Box(orientation=Gtk.Orientation.VERTICAL)
        self.log_view = Gtk.TextView(editable=False, monospace=True, cursor_visible=False,
                                     wrap_mode=Gtk.WrapMode.WORD_CHAR)
        self.log_view.set_top_margin(8)
        self.log_view.set_left_margin(8)
        self.log_view.set_right_margin(8)
        scroller = Gtk.ScrolledWindow(vexpand=True, child=self.log_view)
        self.log_scroller = scroller
        box.append(scroller)
        return box

    def append_log(self, text):
        buffer = self.log_view.get_buffer()
        buffer.insert(buffer.get_end_iter(), text)
        extra = buffer.get_line_count() - MAX_LOG_LINES
        if extra > 0:
            buffer.delete(buffer.get_start_iter(), buffer.get_iter_at_line(extra)[1])
        adj = self.log_scroller.get_vadjustment()
        GLib.idle_add(lambda: adj.set_value(adj.get_upper()) and False)

    # --- process -------------------------------------------------------------------------

    def on_launch(self, _button):
        if self.process:
            self.stop_game()
            return
        self.store()
        self.log_view.get_buffer().set_text("")
        launcher = Gio.SubprocessLauncher.new(
            Gio.SubprocessFlags.STDOUT_PIPE | Gio.SubprocessFlags.STDERR_MERGE)
        launcher.set_environ([f"{k}={v}" for k, v in self.environment().items()])
        launcher.set_cwd(str(PORT_DIR))
        try:
            # setsid: the game and its helpers form one process group, stopped together.
            self.process = launcher.spawnv(["setsid", "bash", str(PORT_DIR / "run.sh")])
        except GLib.Error as error:
            self.toasts.add_toast(Adw.Toast(title="Could not start: {}".format(error.message)))
            return
        self.stream = Gio.DataInputStream.new(self.process.get_stdout_pipe())
        self.read_line()
        self.process.wait_async(None, self.on_exit)
        self.update_launch_button()
        self.stack.set_visible_child_name("log")

    def read_line(self):
        self.stream.read_line_async(GLib.PRIORITY_DEFAULT, None, self.on_line)

    def on_line(self, stream, result):
        try:
            line, _length = stream.read_line_finish_utf8(result)
        except GLib.Error:
            return
        if line is None:
            return
        self.append_log(line + "\n")
        self.read_line()

    def stop_game(self):
        if not self.process:
            return
        pid = int(self.process.get_identifier())
        try:
            os.killpg(pid, signal.SIGTERM)
        except OSError:
            self.process.force_exit()
        GLib.timeout_add_seconds(3, self.kill_if_running, pid)

    def kill_if_running(self, pid):
        if self.process:
            try:
                os.killpg(pid, signal.SIGKILL)
            except OSError:
                pass
        return False

    def on_exit(self, process, result):
        try:
            process.wait_finish(result)
        except GLib.Error:
            pass
        status = process.get_exit_status() if process.get_if_exited() else -1
        self.process = None
        self.update_launch_button()
        self.update_game_status()
        self.append_log("\n— the game exited (code {}) —\n".format(status))

    def on_close(self, _window):
        self.store()
        if self.process:
            self.stop_game()
        return False


class LauncherApp(Adw.Application):
    def __init__(self):
        super().__init__(application_id="io.github.bbport.Launcher",
                         flags=Gio.ApplicationFlags.DEFAULT_FLAGS)

    def do_activate(self):
        window = self.get_active_window() or LauncherWindow(self)
        window.present()


def play():
    """--play: the game with the saved settings, no window (Steam Deck game mode)."""
    settings = load_settings()
    if not (Path(settings["game_dir"]).expanduser() / "eboot.bin").is_file():
        print("bbport: choose the game folder in the launcher first", file=sys.stderr)
        return 1
    os.chdir(PORT_DIR)
    os.execvpe("bash", ["bash", str(PORT_DIR / "run.sh")], game_environment(settings))


if __name__ == "__main__":
    if "--play" in sys.argv[1:]:
        sys.exit(play())
    sys.exit(LauncherApp().run(sys.argv))
