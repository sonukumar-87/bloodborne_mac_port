/* bbmouse: mouse look and a custom keyboard layout for an installed Bloodborne.app, without
 * rebuilding the port. Loaded into both processes with DYLD_INSERT_LIBRARIES (set by run.sh).
 *
 * Window side (bbgpu-host, arm64): wraps SDL_PollEvent. A click in the game window captures the
 * mouse (relative mode); motion, buttons and the wheel go to a small shared file. Cmd, focus loss
 * or the settings overlay release it.
 *
 * Game side (bb-probe, x86-64 under Rosetta): when no real controller is connected, presents a
 * virtual SDL gamepad to the port's pad code, built from the keyboard and that mouse state. The
 * camera follows the Windows port's mouse look (impulse with 60 ms decay, past the game's
 * stick deadzone). Settings: ~/Library/Application Support/Bloodborne/mouse.ini (re-read live). */
#include <SDL3/SDL.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define INTERPOSE(replacement, original) \
    __attribute__((used)) static const struct { const void *r, *o; } interpose_##original \
    __attribute__((section("__DATA,__interpose"))) = {(const void *)(replacement), (const void *)(original)}

typedef struct {
    uint32_t magic, captured, buttons, wheel_up, wheel_down, reserved;
    int64_t dx_milli, dy_milli;
} Shared;
enum { MOUSE_L = 1, MOUSE_R = 2, MOUSE_M = 4 };
#define MAGIC 0x62626d31u

static Shared *shared(void) {
    static Shared *m;
    static int tried;
    if (m || tried) return m;
    tried = 1;
    const char *path = getenv("BB_MOUSE_SHM");
    if (!path || !*path) return NULL;
    int fd = open(path, O_RDWR | O_CREAT, 0600);
    if (fd < 0) return NULL;
    if (ftruncate(fd, 4096) == 0) {
        void *p = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        if (p != MAP_FAILED) m = p;
    }
    close(fd);
    if (m) __atomic_store_n(&m->magic, MAGIC, __ATOMIC_RELEASE);
    return m;
}

__attribute__((constructor)) static void announce(void) {
    if (getenv("BB_MOUSE_SHM")) fprintf(stderr, "bbmouse: loaded (%s, pid %d)\n",
#if defined(__arm64__) || defined(__aarch64__)
                                        "window side",
#else
                                        "game side",
#endif
                                        (int)getpid());
}

#if defined(__arm64__) || defined(__aarch64__)
/* ---------------------------------------------------------------- window side (bbgpu-host) */
static SDL_Window *window;
static int captured;
static float wheel_acc;

static int overlay_open(void) {
    static int (*fn)(void);
    static int looked;
    if (!looked) { fn = (int (*)(void))dlsym(RTLD_DEFAULT, "bbgpu_overlay_captures_input"); looked = 1; }
    return fn ? fn() : 0;
}
static void set_capture(int on) {
    Shared *m = shared();
    if (on == captured || !window || !m) return;
    SDL_SetWindowRelativeMouseMode(window, on);
    captured = on;
    __atomic_store_n(&m->buttons, 0u, __ATOMIC_RELAXED);
    __atomic_store_n(&m->dx_milli, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&m->dy_milli, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&m->captured, (uint32_t)on, __ATOMIC_RELEASE);
}
static uint32_t button_bit(Uint8 b) {
    return b == SDL_BUTTON_LEFT ? MOUSE_L : b == SDL_BUTTON_RIGHT ? MOUSE_R : b == SDL_BUTTON_MIDDLE ? MOUSE_M : 0;
}
/* Returns 1 to swallow the event. */
static int handle(const SDL_Event *e) {
    Shared *m = shared();
    if (!m) return 0;
    if (captured && overlay_open()) set_capture(0);
    switch (e->type) {
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
        if (!window) window = SDL_GetWindowFromID(e->button.windowID);
        if (!captured) {
            if (overlay_open()) return 0;
            set_capture(1);
            return captured; /* the click that takes the mouse is not an attack */
        }
        __atomic_fetch_or(&m->buttons, button_bit(e->button.button), __ATOMIC_RELAXED);
        return 0;
    case SDL_EVENT_MOUSE_BUTTON_UP:
        if (captured) __atomic_fetch_and(&m->buttons, ~button_bit(e->button.button), __ATOMIC_RELAXED);
        return 0;
    case SDL_EVENT_MOUSE_MOTION:
        if (captured) {
            __atomic_fetch_add(&m->dx_milli, (int64_t)(e->motion.xrel * 1000.0f), __ATOMIC_RELAXED);
            __atomic_fetch_add(&m->dy_milli, (int64_t)(e->motion.yrel * 1000.0f), __ATOMIC_RELAXED);
        }
        return 0;
    case SDL_EVENT_MOUSE_WHEEL:
        if (captured) {
            /* Physical direction: undo macOS natural scrolling. */
            wheel_acc += e->wheel.direction == SDL_MOUSEWHEEL_FLIPPED ? -e->wheel.y : e->wheel.y;
            while (wheel_acc >= 1.0f) { __atomic_fetch_add(&m->wheel_up, 1u, __ATOMIC_RELAXED); wheel_acc -= 1.0f; }
            while (wheel_acc <= -1.0f) { __atomic_fetch_add(&m->wheel_down, 1u, __ATOMIC_RELAXED); wheel_acc += 1.0f; }
        }
        return 0;
    case SDL_EVENT_KEY_DOWN:
        if (e->key.scancode == SDL_SCANCODE_LGUI || e->key.scancode == SDL_SCANCODE_RGUI) set_capture(0);
        return 0;
    case SDL_EVENT_WINDOW_FOCUS_LOST:
        set_capture(0);
        return 0;
    default:
        return 0;
    }
}
static bool my_PollEvent(SDL_Event *e) {
    static int announced;
    if (!announced) { announced = 1; fprintf(stderr, "bbmouse: window side loaded (click the game to take the mouse, Cmd releases it)\n"); }
    if (!e) return SDL_PollEvent(e);
    for (;;) {
        if (!SDL_PollEvent(e)) {
            if (captured && overlay_open()) set_capture(0);
            return false;
        }
        if (!handle(e)) return true;
    }
}
INTERPOSE(my_PollEvent, SDL_PollEvent);

#else
/* ---------------------------------------------------------------- game side (bb-probe) */
#define FAKE_ID ((SDL_JoystickID)0x7ffffff0)
static char fake_storage;
#define FAKE ((SDL_Gamepad *)&fake_storage)

static struct {
    float sens_x, sens_y, deadzone, smoothing;
    int invert_x, invert_y, hold_ms, enabled, jump_sprint_ms;
} cfg = {1.5f, 1.5f, 0.13f, 0.0f, 0, 0, 250, 1, 350};

static uint64_t now_us(void) { return clock_gettime_nsec_np(CLOCK_UPTIME_RAW) / 1000u; }

static const char default_ini[] =
    "# Bloodborne mouse settings (bbmouse). Saved changes apply within a second, even in game.\n"
    "enabled=1\n"
    "sensitivity_x=1.5\n"
    "sensitivity_y=1.5\n"
    "invert_x=0\n"
    "invert_y=0\n"
    "# Stick deadzone the camera starts past (0.13 = the game's own, about 16/127).\n"
    "deadzone=0.13\n"
    "# 0 = raw, up to 0.95 = very smooth.\n"
    "smoothing=0\n"
    "# How long left click must be held before it becomes a strong attack (R2), in ms.\n"
    "hold_ms=250\n"
    "# Space while moving: how long to sprint (hold circle) before the jump, in ms.\n"
    "jump_sprint_ms=350\n";

static void load_config(void) {
    static uint64_t last_check;
    static struct timespec mtime;
    uint64_t now = now_us();
    if (last_check && now - last_check < 1000000) return;
    last_check = now;
    const char *home = getenv("HOME");
    if (!home) return;
    char path[1024];
    snprintf(path, sizeof path, "%s/Library/Application Support/Bloodborne/mouse.ini", home);
    struct stat st;
    if (stat(path, &st) != 0) {
        FILE *f = fopen(path, "w");
        if (f) { fputs(default_ini, f); fclose(f); }
        return;
    }
    if (st.st_mtimespec.tv_sec == mtime.tv_sec && st.st_mtimespec.tv_nsec == mtime.tv_nsec) return;
    mtime = st.st_mtimespec;
    FILE *f = fopen(path, "r");
    if (!f) return;
    char line[256], key[64];
    float v;
    while (fgets(line, sizeof line, f)) {
        if (sscanf(line, " %63[a-z_] = %f", key, &v) != 2) continue;
        if (!strcmp(key, "sensitivity_x")) cfg.sens_x = v;
        else if (!strcmp(key, "sensitivity_y")) cfg.sens_y = v;
        else if (!strcmp(key, "deadzone")) cfg.deadzone = v < 0 ? 0 : v > 0.5f ? 0.5f : v;
        else if (!strcmp(key, "smoothing")) cfg.smoothing = v < 0 ? 0 : v > 0.95f ? 0.95f : v;
        else if (!strcmp(key, "invert_x")) cfg.invert_x = v != 0;
        else if (!strcmp(key, "invert_y")) cfg.invert_y = v != 0;
        else if (!strcmp(key, "hold_ms")) cfg.hold_ms = v < 50 ? 50 : (int)v;
        else if (!strcmp(key, "enabled")) cfg.enabled = v != 0;
        else if (!strcmp(key, "jump_sprint_ms")) cfg.jump_sprint_ms = v < 50 ? 50 : v > 2000 ? 2000 : (int)v;
    }
    fclose(f);
    fprintf(stderr, "bbmouse: settings sens %.2f/%.2f invert %d/%d deadzone %.2f smoothing %.2f hold %d ms jump %d ms%s\n",
            cfg.sens_x, cfg.sens_y, cfg.invert_x, cfg.invert_y, cfg.deadzone, cfg.smoothing, cfg.hold_ms,
            cfg.jump_sprint_ms, cfg.enabled ? "" : " (disabled)");
}

/* A short button press the game can see; presses that arrive during one queue up (at most 2). */
typedef struct { uint64_t on_until, off_until; int queued; } Pulse;
#define PULSE_ON_US 90000u
#define PULSE_GAP_US 60000u
static void pulse_fire(Pulse *p, uint64_t now) {
    if (now < p->off_until) { if (p->queued < 2) ++p->queued; return; }
    p->on_until = now + PULSE_ON_US;
    p->off_until = p->on_until + PULSE_GAP_US;
}
static int pulse_down(Pulse *p, uint64_t now) {
    if (now >= p->off_until && p->queued) {
        --p->queued;
        p->on_until = now + PULSE_ON_US;
        p->off_until = p->on_until + PULSE_GAP_US;
    }
    return now < p->on_until;
}

static struct { int buttons[SDL_GAMEPAD_BUTTON_COUNT]; Sint16 axes[SDL_GAMEPAD_AXIS_COUNT]; } pad;

static Sint16 stick(int byte) { int v = byte * 256 - 32768; return (Sint16)(v > 32767 ? 32767 : v); }
static int look_axis(float val, int current, int deadzone) {
    if (val > -0.5f && val < 0.5f) return current;
    const float mag = val < 0 ? -val : val;
    const float out = (float)deadzone + mag * (127.0f - (float)deadzone) / 127.0f;
    const int v = 128 + (int)(val < 0 ? -out : out);
    return v < 0 ? 0 : v > 255 ? 255 : v;
}

static const bool *keyboard(void) {
    static const bool *(*split)(void);
    static int looked;
    if (!looked) { split = (const bool *(*)(void))dlsym(RTLD_DEFAULT, "bbgpu_split_keyboard"); looked = 1; }
    const bool *k = split ? split() : NULL;
    if (!k && SDL_WasInit(SDL_INIT_VIDEO)) k = SDL_GetKeyboardState(NULL);
    return k;
}

static void update_pad(void) {
    load_config();
    const uint64_t now = now_us();
    const bool *k = keyboard();
    Shared *m = shared();
    memset(&pad, 0, sizeof pad);
#define KEY(sc) (k && k[sc])
    int *b = pad.buttons;
    b[SDL_GAMEPAD_BUTTON_SOUTH] = KEY(SDL_SCANCODE_X) || KEY(SDL_SCANCODE_RETURN) || KEY(SDL_SCANCODE_KP_ENTER);
    /* C = circle (dodge); Shift = circle too, held for sprinting. */
    b[SDL_GAMEPAD_BUTTON_EAST] = KEY(SDL_SCANCODE_C) || KEY(SDL_SCANCODE_LSHIFT) || KEY(SDL_SCANCODE_RSHIFT);
    b[SDL_GAMEPAD_BUTTON_WEST] = KEY(SDL_SCANCODE_Z);
    b[SDL_GAMEPAD_BUTTON_NORTH] = KEY(SDL_SCANCODE_V);
    b[SDL_GAMEPAD_BUTTON_LEFT_SHOULDER] = KEY(SDL_SCANCODE_K);
    b[SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER] = KEY(SDL_SCANCODE_H);
    b[SDL_GAMEPAD_BUTTON_LEFT_STICK] = KEY(SDL_SCANCODE_E);
    b[SDL_GAMEPAD_BUTTON_RIGHT_STICK] = KEY(SDL_SCANCODE_Q);
    b[SDL_GAMEPAD_BUTTON_START] = KEY(SDL_SCANCODE_ESCAPE) || KEY(SDL_SCANCODE_F1);
    /* 1-4 for item and weapon switching, arrow keys for menus and the inventory (both the D-pad;
     * the camera is on the mouse). */
    b[SDL_GAMEPAD_BUTTON_DPAD_UP] = KEY(SDL_SCANCODE_1) || KEY(SDL_SCANCODE_UP);
    b[SDL_GAMEPAD_BUTTON_DPAD_RIGHT] = KEY(SDL_SCANCODE_2) || KEY(SDL_SCANCODE_RIGHT);
    b[SDL_GAMEPAD_BUTTON_DPAD_DOWN] = KEY(SDL_SCANCODE_3) || KEY(SDL_SCANCODE_DOWN);
    b[SDL_GAMEPAD_BUTTON_DPAD_LEFT] = KEY(SDL_SCANCODE_4) || KEY(SDL_SCANCODE_LEFT);
    int l2 = KEY(SDL_SCANCODE_L), r2 = KEY(SDL_SCANCODE_J);

    int lx = 128, ly = 128, rx = 128, ry = 128;
    if (KEY(SDL_SCANCODE_A)) lx = 0; else if (KEY(SDL_SCANCODE_D)) lx = 255;
    if (KEY(SDL_SCANCODE_W)) ly = 0; else if (KEY(SDL_SCANCODE_S)) ly = 255;

    /* Space = jump. Bloodborne jumps with L3 only while sprinting (circle held): with C held it
     * presses L3 at once; while moving it holds circle until the sprint starts, then presses L3. */
    static int space_was, jump_running;
    static uint64_t jump_start;
    static Pulse jump_tap;
    const int space = KEY(SDL_SCANCODE_SPACE);
    if (space && !space_was) {
        const int moving = lx != 128 || ly != 128;
        if (b[SDL_GAMEPAD_BUTTON_EAST] || !moving) pulse_fire(&jump_tap, now); /* already sprinting */
        else { jump_running = 1; jump_start = now; }
    }
    space_was = space;
    if (jump_running) {
        const uint64_t t = now - jump_start, at = (uint64_t)cfg.jump_sprint_ms * 1000u;
        if (t >= at + PULSE_ON_US) jump_running = 0;
        else {
            b[SDL_GAMEPAD_BUTTON_EAST] = 1; /* sprint */
            if (t >= at) b[SDL_GAMEPAD_BUTTON_LEFT_STICK] = 1;
        }
    }
    if (pulse_down(&jump_tap, now)) b[SDL_GAMEPAD_BUTTON_LEFT_STICK] = 1;

    /* Mouse buttons and wheel. */
    static int left_was;
    static uint64_t left_down_at;
    static uint32_t seen_up, seen_down;
    static Pulse tap, wheel_up, wheel_down;
    const int captured = m && cfg.enabled && __atomic_load_n(&m->captured, __ATOMIC_ACQUIRE);
    const uint32_t mb = captured ? __atomic_load_n(&m->buttons, __ATOMIC_RELAXED) : 0;
    const int left = (mb & MOUSE_L) != 0;
    if (left && !left_was) left_down_at = now;
    const int held = left && now - left_down_at >= (uint64_t)cfg.hold_ms * 1000u;
    if (!left && left_was && now - left_down_at < (uint64_t)cfg.hold_ms * 1000u) pulse_fire(&tap, now);
    left_was = left;
    if (held) r2 = 1;
    if (pulse_down(&tap, now)) b[SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER] = 1;
    if (mb & MOUSE_R) l2 = 1;
    if (mb & MOUSE_M) b[SDL_GAMEPAD_BUTTON_RIGHT_STICK] = 1; /* lock on, like Q */
    if (m) {
        const uint32_t up = __atomic_load_n(&m->wheel_up, __ATOMIC_RELAXED);
        const uint32_t down = __atomic_load_n(&m->wheel_down, __ATOMIC_RELAXED);
        if (captured) {
            if (up != seen_up) pulse_fire(&wheel_up, now);
            if (down != seen_down) pulse_fire(&wheel_down, now);
        }
        seen_up = up; seen_down = down;
    }
    if (pulse_down(&wheel_up, now)) b[SDL_GAMEPAD_BUTTON_DPAD_RIGHT] = 1;
    if (pulse_down(&wheel_down, now)) b[SDL_GAMEPAD_BUTTON_DPAD_LEFT] = 1;

    /* Mouse look (Windows port model): impulse with exponential decay over LOOK_TAU_US. */
#define LOOK_TAU_US 60000.0f
    static float look_x, look_y, smooth_dx, smooth_dy;
    static uint64_t look_last;
    float dt = look_last && now > look_last ? (float)(now - look_last) : 0.0f;
    look_last = now;
    float dx = 0, dy = 0;
    if (m) {
        dx = (float)__atomic_exchange_n(&m->dx_milli, 0, __ATOMIC_RELAXED) / 1000.0f;
        dy = (float)__atomic_exchange_n(&m->dy_milli, 0, __ATOMIC_RELAXED) / 1000.0f;
    }
    if (!captured) {
        look_x = look_y = smooth_dx = smooth_dy = 0;
    } else {
        if (dt > LOOK_TAU_US) dt = LOOK_TAU_US;
        const float decay = 1.0f - dt / LOOK_TAU_US;
        const float gain = 127.0f / (1000.0f * LOOK_TAU_US / 1e6f);
        smooth_dx = smooth_dx * cfg.smoothing + dx * (1.0f - cfg.smoothing);
        smooth_dy = smooth_dy * cfg.smoothing + dy * (1.0f - cfg.smoothing);
        look_x = look_x * decay + smooth_dx * gain * cfg.sens_x * (cfg.invert_x ? -1.0f : 1.0f);
        look_y = look_y * decay + smooth_dy * gain * cfg.sens_y * (cfg.invert_y ? -1.0f : 1.0f);
        if (look_x < -127) look_x = -127; else if (look_x > 127) look_x = 127;
        if (look_y < -127) look_y = -127; else if (look_y > 127) look_y = 127;
        const int dz = (int)(cfg.deadzone * 127.0f + 0.5f);
        rx = look_axis(look_x, rx, dz);
        ry = look_axis(look_y, ry, dz);
    }
    pad.axes[SDL_GAMEPAD_AXIS_LEFTX] = stick(lx);
    pad.axes[SDL_GAMEPAD_AXIS_LEFTY] = stick(ly);
    pad.axes[SDL_GAMEPAD_AXIS_RIGHTX] = stick(rx);
    pad.axes[SDL_GAMEPAD_AXIS_RIGHTY] = stick(ry);
    pad.axes[SDL_GAMEPAD_AXIS_LEFT_TRIGGER] = l2 ? 32767 : 0;
    pad.axes[SDL_GAMEPAD_AXIS_RIGHT_TRIGGER] = r2 ? 32767 : 0;
#undef KEY
}

static SDL_JoystickID *my_GetGamepads(int *count) {
    int n = 0;
    SDL_JoystickID *ids = SDL_GetGamepads(&n);
    load_config();
    if (n > 0 || !cfg.enabled) { if (count) *count = n; return ids; }
    SDL_free(ids);
    ids = SDL_malloc(2 * sizeof *ids);
    if (!ids) { if (count) *count = 0; return NULL; }
    ids[0] = FAKE_ID; ids[1] = 0;
    if (count) *count = 1;
    fprintf(stderr, "bbmouse: no controller, using keyboard and mouse\n");
    return ids;
}
static SDL_Gamepad *my_OpenGamepad(SDL_JoystickID id) { return id == FAKE_ID ? FAKE : SDL_OpenGamepad(id); }
static bool my_GamepadConnected(SDL_Gamepad *g) {
    if (g != FAKE) return SDL_GamepadConnected(g);
    if (SDL_HasGamepad() || !cfg.enabled) return false; /* a real controller takes over */
    update_pad();
    return true;
}
static void my_CloseGamepad(SDL_Gamepad *g) { if (g != FAKE) SDL_CloseGamepad(g); }
static const char *my_GetGamepadName(SDL_Gamepad *g) { return g == FAKE ? "Keyboard and mouse (bbmouse)" : SDL_GetGamepadName(g); }
static bool my_GetGamepadButton(SDL_Gamepad *g, SDL_GamepadButton button) {
    if (g != FAKE) return SDL_GetGamepadButton(g, button);
    return button >= 0 && button < SDL_GAMEPAD_BUTTON_COUNT && pad.buttons[button];
}
static Sint16 my_GetGamepadAxis(SDL_Gamepad *g, SDL_GamepadAxis axis) {
    if (g != FAKE) return SDL_GetGamepadAxis(g, axis);
    return axis >= 0 && axis < SDL_GAMEPAD_AXIS_COUNT ? pad.axes[axis] : 0;
}
static int my_GetNumGamepadTouchpads(SDL_Gamepad *g) { return g == FAKE ? 0 : SDL_GetNumGamepadTouchpads(g); }
static bool my_RumbleGamepad(SDL_Gamepad *g, Uint16 lo, Uint16 hi, Uint32 ms) {
    return g == FAKE ? false : SDL_RumbleGamepad(g, lo, hi, ms);
}
INTERPOSE(my_GetGamepads, SDL_GetGamepads);
INTERPOSE(my_OpenGamepad, SDL_OpenGamepad);
INTERPOSE(my_GamepadConnected, SDL_GamepadConnected);
INTERPOSE(my_CloseGamepad, SDL_CloseGamepad);
INTERPOSE(my_GetGamepadName, SDL_GetGamepadName);
INTERPOSE(my_GetGamepadButton, SDL_GetGamepadButton);
INTERPOSE(my_GetGamepadAxis, SDL_GetGamepadAxis);
INTERPOSE(my_GetNumGamepadTouchpads, SDL_GetNumGamepadTouchpads);
INTERPOSE(my_RumbleGamepad, SDL_RumbleGamepad);
#endif
