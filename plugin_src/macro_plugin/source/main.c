// macro_plugin - controller macro recorder / looper for GoldHEN (PS4)
//
// The combo (default L1 + R2) is hidden from the game; actions fire when you RELEASE it.
//   IDLE      --tap-->  RECORDING   (light bar RED)
//   RECORDING --tap-->  PLAYING     (light bar GREEN, loops; macro saved to the current slot)
//   PLAYING   --tap-->  IDLE
//   IDLE + hold combo ~1 s --> play the saved macro of the current slot
// While HOLDING the combo:
//   UP / DOWN       playback speed        LEFT / RIGHT   previous / next macro slot
//   TRIANGLE        pause / resume        CIRCLE         reload config.json
//
// Timing is timestamp based (microseconds), so playback is independent of game FPS.
//
// Files (folder is created automatically):
//   /data/macro_plugin/config.json   - settings; a readable default is written on first run
//   /data/macro_plugin/log.txt       - plugin log, overwritten each time the plugin loads
//   /data/macro_plugin/macro.bin     - saved macro, slot 1 (slots 2.. are macro_2.bin ...)
//   /data/macro_plugin/greeting.txt  - remembers that the welcome screen was shown

#include "plugin_common.h"
#include "Common.h"

#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <orbis/libkernel.h>

// The Pad header declares scePad* functions. The HOOK macros need function-POINTER
// variables with those exact names, so rename the header's declarations out of the way.
#define scePadReadState     hdr_decl_scePadReadState
#define scePadRead          hdr_decl_scePadRead
#define scePadSetLightBar   hdr_decl_scePadSetLightBar
#define scePadResetLightBar hdr_decl_scePadResetLightBar
#if __has_include(<orbis/Pad.h>)
#include <orbis/Pad.h>
#define PAD_STRIDE (sizeof(OrbisPadData))
#define PAD_STRIDE_FALLBACK 0
#else
#define PAD_STRIDE 0x80 // fallback guess for sizeof(OrbisPadData)
#define PAD_STRIDE_FALLBACK 1
#endif
#undef scePadReadState
#undef scePadRead
#undef scePadSetLightBar
#undef scePadResetLightBar

attr_public const char *g_pluginName = "macro_plugin";
attr_public const char *g_pluginDesc = "Record and loop controller macros";
attr_public const char *g_pluginAuth = "you";
attr_public u32 g_pluginVersion = 0x00000200; // 2.00

/* ------------------------------------------------------------------ */
/* Constants                                                           */
/* ------------------------------------------------------------------ */

#define PLUGIN_DIR           "/data/macro_plugin"
#define LOG_FILE             "/data/macro_plugin/log.txt"
#define MACRO_FILE           "/data/macro_plugin/macro.bin"       // slot 1
#define CONFIG_FILE          "/data/macro_plugin/config.json"
#define CONFIG_BACKUP        "/data/macro_plugin/config_old.json"
#define GREETING_FILE        "/data/macro_plugin/greeting.txt"
#define BAR_STATE_FILE       "/data/macro_plugin/lightbar_state.txt"
#define NOTIFY_STATE_FILE    "/data/macro_plugin/notify_state.txt"

#define CFG_MAX              8192           // config.json bigger than this is cut off
#define CONFIG_VERSION       3
#define MAX_SLOTS            8

#define MAX_SAMPLES          16384          // 16 bytes each = 256 KB
#define MIN_MACRO_US         200000         // ignore recordings shorter than 0.2 s
#define MAX_STEP_US          250000         // playback never jumps more than this per pad read
#define TRIGGER_THRESHOLD    200            // an analog trigger pulled past this counts as pressed (0-255)
#define BAR_REAPPLY_EVERY    120            // re-apply light bar every N pad reads

#define USE_LIGHTBAR         1
#define USE_NOTIFY           1
#define NOTIFY_TARGET_ID     (-1)           // if no popup ever shows, try 0 here
#define READY_AFTER_US       5000000        // "ready" popup this long after the first pad read
#define GREET_AFTER_US       10000000       // welcome screen this long after the first pad read
#define MAX_PADS             8              // controllers tracked separately
#define OWNER_TIMEOUT_US     2000000        // the macro stops if its controller goes silent this long

// popup anti-spam: the first popup after a quiet period is instant, then only the NEWEST one is
// kept and shown once you pause (at most one every NOTIFY_MIN_GAP_US)
#define NOTIFY_QUIET_US      1500000
#define NOTIFY_SETTLE_US     400000
#define NOTIFY_MIN_GAP_US    1000000
#define NOTIFY_MAX_WAIT_US   1500000

// Built-in system icons
#define ICON_READY "cxml://psnotification/tex_default_icon_tips"
#define ICON_REC   "cxml://psnotification/tex_icon_live_start"
#define ICON_PLAY  "cxml://psnotification/tex_icon_loading"
#define ICON_END   "cxml://psnotification/tex_icon_ban"

#define MSG_END    "Macro stopped"
#define MSG_SHORT  "Macro: recording too short, discarded"

// Standard PS4 button bits (the combo itself is chosen in config.json)
#define BTN_L3       0x00000002u
#define BTN_R3       0x00000004u
#define BTN_OPTIONS  0x00000008u
#define BTN_UP       0x00000010u
#define BTN_RIGHT    0x00000020u
#define BTN_DOWN     0x00000040u
#define BTN_LEFT     0x00000080u
#define BTN_L2       0x00000100u
#define BTN_R2       0x00000200u
#define BTN_L1       0x00000400u
#define BTN_R1       0x00000800u
#define BTN_TRIANGLE 0x00001000u
#define BTN_CIRCLE   0x00002000u
#define BTN_CROSS    0x00004000u
#define BTN_SQUARE   0x00008000u
#define BTN_TOUCHPAD 0x00100000u

// buttons that act as commands while the combo is held
#define CMD_ALL (BTN_UP | BTN_DOWN | BTN_LEFT | BTN_RIGHT | BTN_TRIANGLE | BTN_CIRCLE | BTN_SQUARE)

#ifndef SCE_KERNEL_O_RDONLY
#define SCE_KERNEL_O_RDONLY 0x0000
#endif
#ifndef SCE_KERNEL_O_WRONLY
#define SCE_KERNEL_O_WRONLY 0x0001
#endif
#ifndef SCE_KERNEL_O_APPEND
#define SCE_KERNEL_O_APPEND 0x0008
#endif
#ifndef SCE_KERNEL_O_CREAT
#define SCE_KERNEL_O_CREAT  0x0200
#endif
#ifndef SCE_KERNEL_O_TRUNC
#define SCE_KERNEL_O_TRUNC  0x0400
#endif

/* ------------------------------------------------------------------ */
/* File logger (no printf-style varargs on purpose)                    */
/* ------------------------------------------------------------------ */

// <<<PURE-BEGIN logger
static size_t put_s(char *b, size_t i, size_t cap, const char *s) {
    while (s && *s && i + 1 < cap) b[i++] = *s++;
    return i;
}

static size_t put_n(char *b, size_t i, size_t cap, uint64_t v, bool hex) {
    char tmp[24];
    int n = 0;
    uint64_t base = hex ? 16 : 10;
    if (v == 0) tmp[n++] = '0';
    while (v && n < 23) {
        uint64_t d = v % base;
        tmp[n++] = (char)(d < 10 ? '0' + d : 'a' + (d - 10));
        v /= base;
    }
    if (hex) i = put_s(b, i, cap, "0x");
    while (n && i + 1 < cap) b[i++] = tmp[--n];
    return i;
}
// >>>PURE-END

static void log_line(const char *msg, bool has_val, uint64_t val, bool hex) {
    char b[240];
    size_t i = 0;
    i = put_s(b, i, sizeof(b), "[");
    i = put_n(b, i, sizeof(b), sceKernelGetProcessTime() / 1000, false);
    i = put_s(b, i, sizeof(b), " ms] ");
    i = put_s(b, i, sizeof(b), msg);
    if (has_val) {
        i = put_s(b, i, sizeof(b), " ");
        i = put_n(b, i, sizeof(b), val, hex);
    }
    b[i++] = '\n';
    int fd = sceKernelOpen(LOG_FILE, SCE_KERNEL_O_WRONLY | SCE_KERNEL_O_CREAT | SCE_KERNEL_O_APPEND, 0777);
    if (fd < 0) return;
    sceKernelWrite(fd, b, i);
    sceKernelClose(fd);
}

static void log_reset(void) { // create folder, start a fresh log file
    sceKernelMkdir(PLUGIN_DIR, 0777);
    int fd = sceKernelOpen(LOG_FILE, SCE_KERNEL_O_WRONLY | SCE_KERNEL_O_CREAT | SCE_KERNEL_O_TRUNC, 0777);
    if (fd >= 0) sceKernelClose(fd);
}

#define mlog(m)          log_line((m), false, 0, false)
#define mlog_dec(m, v)   log_line((m), true, (uint64_t)(v), false)
#define mlog_hex(m, v)   log_line((m), true, (uint64_t)(v), true)

/* ------------------------------------------------------------------ */
/* Config (config.json)                                                */
/* ------------------------------------------------------------------ */

// <<<PURE-BEGIN config
typedef struct {
    uint32_t combo_mask;     // buttons that must be held together
    uint32_t hold_play_us;   // hold the combo this long (idle) to play the saved macro
    uint32_t speed_pct;      // starting playback speed, 100 = normal
    uint32_t slots;          // number of macro slots (1..MAX_SLOTS)
    bool     bar_on;
    uint8_t  col_rec[3];
    uint8_t  col_play[3];
    uint8_t  col_pause[3];
    bool     notify_on;
    bool     ready_popup;
    char     icon[128];      // "" = built-in icons; otherwise this URI is used for every popup
    bool     welcome;        // show the first-run welcome screen
    int32_t  file_version;   // config_version found in the file (0 = old file without one)
} Config;

static Config g_cfg;
static char   g_msg_ready[160];
static char   g_msg_rec[160];
static char   g_msg_play[160];

static void cfg_defaults(void) {
    memset(&g_cfg, 0, sizeof(g_cfg));
    g_cfg.combo_mask = BTN_L1 | BTN_R2;
    g_cfg.hold_play_us = 1000000;
    g_cfg.speed_pct = 100;
    g_cfg.slots = 4;
    g_cfg.bar_on = true;
    g_cfg.col_rec[0] = 255;   g_cfg.col_rec[1] = 0;     g_cfg.col_rec[2] = 0;
    g_cfg.col_play[0] = 0;    g_cfg.col_play[1] = 255;  g_cfg.col_play[2] = 0;
    g_cfg.col_pause[0] = 255; g_cfg.col_pause[1] = 170; g_cfg.col_pause[2] = 0;
    g_cfg.notify_on = true;
    g_cfg.ready_popup = true;
    g_cfg.welcome = true;
    g_cfg.file_version = 0;
}

typedef struct { const char *name; uint32_t bit; } BtnDef;
// canonical names first; the first name listed for a bit is the one shown in popups
static const BtnDef g_btns[] = {
    {"L1", BTN_L1},   {"L2", BTN_L2},    {"L3", BTN_L3},
    {"R1", BTN_R1},   {"R2", BTN_R2},    {"R3", BTN_R3},
    {"CROSS", BTN_CROSS}, {"X", BTN_CROSS},
    {"CIRCLE", BTN_CIRCLE}, {"O", BTN_CIRCLE},
    {"SQUARE", BTN_SQUARE}, {"TRIANGLE", BTN_TRIANGLE},
    {"UP", BTN_UP},   {"DOWN", BTN_DOWN}, {"LEFT", BTN_LEFT}, {"RIGHT", BTN_RIGHT},
    {"OPTIONS", BTN_OPTIONS},
    {"TOUCHPAD", BTN_TOUCHPAD}, {"TOUCH", BTN_TOUCHPAD},
};
#define N_BTNS (sizeof(g_btns) / sizeof(g_btns[0]))

static char up_c(char c) { return (c >= 'a' && c <= 'z') ? (char)(c - 32) : c; }

static bool str_ieq(const char *a, const char *b) {
    while (*a && *b) { if (up_c(*a) != up_c(*b)) return false; a++; b++; }
    return *a == 0 && *b == 0;
}

static uint32_t find_button(const char *name) {
    for (size_t k = 0; k < N_BTNS; k++) if (str_ieq(name, g_btns[k].name)) return g_btns[k].bit;
    return 0;
}

static int popcount32(uint32_t v) { int n = 0; while (v) { n += (int)(v & 1u); v >>= 1; } return n; }

static void combo_name(uint32_t mask, char *out, size_t cap) {
    if (cap == 0) return;
    size_t i = 0;
    uint32_t shown = 0;
    bool first = true;
    for (size_t k = 0; k < N_BTNS; k++) {
        uint32_t bit = g_btns[k].bit;
        if ((mask & bit) && !(shown & bit)) {
            if (!first) i = put_s(out, i, cap, "+");
            i = put_s(out, i, cap, g_btns[k].name);
            shown |= bit;
            first = false;
        }
    }
    out[i] = 0;
}

static void mlog2(const char *a, const char *b) {
    char buf[220];
    size_t i = 0;
    i = put_s(buf, i, sizeof(buf), a);
    i = put_s(buf, i, sizeof(buf), " ");
    i = put_s(buf, i, sizeof(buf), b);
    buf[i] = 0;
    mlog(buf);
}

// "1", "1.5", "1.25" (force_decimal: "1.0", "1.5", "1.25")
static void fmt_speed(char *out, size_t cap, uint32_t pct, bool force_decimal) {
    if (cap == 0) return;
    size_t i = 0;
    i = put_n(out, i, cap, pct / 100, false);
    uint32_t rem = pct % 100;
    if (rem) {
        char d[3];
        d[0] = (char)('0' + rem / 10);
        d[1] = (char)('0' + rem % 10);
        d[2] = 0;
        if (d[1] == '0') d[1] = 0;
        i = put_s(out, i, cap, ".");
        i = put_s(out, i, cap, d);
    } else if (force_decimal) {
        i = put_s(out, i, cap, ".0");
    }
    out[i] = 0;
}

// milliseconds -> "8.3"
static void fmt_tenths(char *out, size_t cap, uint32_t ms) {
    if (cap == 0) return;
    uint32_t t = ms / 100;
    size_t i = 0;
    i = put_n(out, i, cap, t / 10, false);
    i = put_s(out, i, cap, ".");
    i = put_n(out, i, cap, t % 10, false);
    out[i] = 0;
}

static const uint16_t g_speed_steps[] = {25, 50, 75, 100, 125, 150, 200, 300, 400};
#define N_SPEED_STEPS (sizeof(g_speed_steps) / sizeof(g_speed_steps[0]))

static uint32_t speed_up(uint32_t pct) {
    for (size_t k = 0; k < N_SPEED_STEPS; k++) if (g_speed_steps[k] > pct) return g_speed_steps[k];
    return pct;
}

static uint32_t speed_down(uint32_t pct) {
    for (size_t k = N_SPEED_STEPS; k > 0; k--) if (g_speed_steps[k - 1] < pct) return g_speed_steps[k - 1];
    return pct;
}

/* ---- tiny forgiving JSON reader (also allows // and slash-star comments and trailing commas) ---- */

enum { K_STR = 0, K_NUM = 1, K_BOOL = 2, K_NULL = 3 };

static uint32_t t_combo;
static bool     t_combo_seen, t_combo_bad;
static int      g_unknown_logged;

static const char *after_prefix(const char *path, const char *prefix) {
    while (*prefix) { if (*path++ != *prefix++) return 0; }
    return path;
}

static bool path_eq(const char *a, const char *b) {
    const char *r = after_prefix(a, b);
    return r && *r == 0;
}

static uint8_t clamp_u8(int32_t v) { return (uint8_t)(v < 0 ? 0 : (v > 255 ? 255 : v)); }

static int hexv(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static bool parse_hex_color(const char *s, uint8_t *col) {
    if (*s == '#') s++;
    int v[6];
    for (int i = 0; i < 6; i++) { v[i] = hexv(s[i]); if (v[i] < 0) return false; }
    if (s[6] != 0) return false;
    col[0] = (uint8_t)(v[0] * 16 + v[1]);
    col[1] = (uint8_t)(v[2] * 16 + v[3]);
    col[2] = (uint8_t)(v[4] * 16 + v[5]);
    return true;
}

static void combo_add(const char *s) {
    char tok[16];
    size_t n = 0;
    for (const char *c = s;; c++) {
        char ch = *c;
        bool sep = (ch == 0 || ch == '+' || ch == ',' || ch == ' ');
        if (!sep) {
            if (n + 1 < sizeof(tok)) tok[n++] = ch;
            else t_combo_bad = true; // absurdly long name
        } else {
            if (n) {
                tok[n] = 0;
                uint32_t b = find_button(tok);
                if (!b) { mlog2("config: unknown button name:", tok); t_combo_bad = true; }
                else t_combo |= b;
                n = 0;
            }
            if (ch == 0) break;
        }
    }
}

static bool want_bool(int kind, int32_t n, bool *out) {
    if (kind == K_BOOL || kind == K_NUM) { *out = (n != 0); return true; }
    return false;
}

// handles "<base>" (a "#RRGGBB" string) and "<base>.0/.1/.2" (numbers); returns true if path belongs to base
static bool color_key(const char *path, const char *base, uint8_t *col, int kind, const char *s, int32_t n) {
    const char *rest = after_prefix(path, base);
    if (!rest) return false;
    if (*rest == 0) {
        if (!(kind == K_STR && parse_hex_color(s, col))) mlog2("config: bad color (use [r,g,b] or #RRGGBB):", path);
        return true;
    }
    if (rest[0] == '.' && rest[1] >= '0' && rest[1] <= '2' && rest[2] == 0) {
        if (kind == K_NUM) col[rest[1] - '0'] = clamp_u8(n);
        else mlog2("config: color values must be numbers:", path);
        return true;
    }
    return false;
}

static bool key_is(const char *path, const char *a, const char *b) {
    return path_eq(path, a) || (b && path_eq(path, b));
}

// n = whole part of a number, milli = value * 1000 (so 1.25 -> 1250)
// Every setting accepts its current name and the name older versions used.
static void cfg_leaf(const char *path, int kind, const char *s, int32_t n, int32_t milli) {
    bool b;
    if (path[0] == '_') return; // keys starting with _ are ignored

    if (key_is(path, "version", "config_version")) {
        if (kind == K_NUM) g_cfg.file_version = n;
        return;
    }
    if (path_eq(path, "combo") || after_prefix(path, "combo.")) {
        if (kind != K_STR) { mlog2("config: combo entries must be button names in quotes:", path); t_combo_bad = true; return; }
        t_combo_seen = true;
        combo_add(s);
        return;
    }
    if (key_is(path, "hold_to_play_ms", "hold_play_ms")) {
        if (kind != K_NUM) { mlog2("config: expected a number:", path); return; }
        if (n < 300) n = 300;
        if (n > 10000) n = 10000;
        g_cfg.hold_play_us = (uint32_t)n * 1000u;
        return;
    }
    if (key_is(path, "speed", "playback.speed")) {
        if (kind != K_NUM) { mlog2("config: expected a number:", path); return; }
        int32_t pct = milli / 10;
        if (pct < 25) pct = 25;
        if (pct > 400) pct = 400;
        g_cfg.speed_pct = (uint32_t)pct;
        return;
    }
    if (key_is(path, "slots", "playback.slots")) {
        if (kind != K_NUM) { mlog2("config: expected a number:", path); return; }
        if (n < 1) n = 1;
        if (n > MAX_SLOTS) n = MAX_SLOTS;
        g_cfg.slots = (uint32_t)n;
        return;
    }
    if (key_is(path, "light_bar", "light_bar.enabled")) {
        if (want_bool(kind, n, &b)) g_cfg.bar_on = b; else mlog2("config: expected true/false:", path);
        return;
    }
    if (color_key(path, "colors.recording", g_cfg.col_rec, kind, s, n)) return;
    if (color_key(path, "colors.playing", g_cfg.col_play, kind, s, n)) return;
    if (color_key(path, "colors.paused", g_cfg.col_pause, kind, s, n)) return;
    if (color_key(path, "light_bar.recording", g_cfg.col_rec, kind, s, n)) return;
    if (color_key(path, "light_bar.playing", g_cfg.col_play, kind, s, n)) return;
    if (color_key(path, "light_bar.paused", g_cfg.col_pause, kind, s, n)) return;
    if (key_is(path, "popups", "notifications.enabled")) {
        if (want_bool(kind, n, &b)) g_cfg.notify_on = b; else mlog2("config: expected true/false:", path);
        return;
    }
    if (key_is(path, "ready_popup", "notifications.ready_popup")) {
        if (want_bool(kind, n, &b)) g_cfg.ready_popup = b; else mlog2("config: expected true/false:", path);
        return;
    }
    if (key_is(path, "icon", "notifications.icon")) {
        if (kind == K_STR) {
            size_t i = 0;
            while (s[i] && i + 1 < sizeof(g_cfg.icon)) { g_cfg.icon[i] = s[i]; i++; }
            g_cfg.icon[i] = 0;
        } else mlog2("config: icon must be text in quotes:", path);
        return;
    }
    if (key_is(path, "welcome", "welcome_screen")) {
        if (want_bool(kind, n, &b)) g_cfg.welcome = b; else mlog2("config: expected true/false:", path);
        return;
    }
    if (g_unknown_logged < 8) { g_unknown_logged++; mlog2("config: ignoring unknown key:", path); }
}

typedef struct { const char *p; const char *end; bool failed; } JP;

static bool jp_fail(JP *j, const char *why) {
    if (!j->failed) {
        j->failed = true;
        mlog2("config: SYNTAX ERROR:", why);
    }
    return false;
}

static void jp_skip(JP *j) {
    for (;;) {
        while (j->p < j->end && (*j->p == ' ' || *j->p == '\t' || *j->p == '\r' || *j->p == '\n')) j->p++;
        if (j->p + 1 < j->end && j->p[0] == '/' && j->p[1] == '/') {
            while (j->p < j->end && *j->p != '\n') j->p++;
            continue;
        }
        if (j->p + 1 < j->end && j->p[0] == '/' && j->p[1] == '*') {
            j->p += 2;
            while (j->p + 1 < j->end && !(j->p[0] == '*' && j->p[1] == '/')) j->p++;
            j->p = (j->p + 1 < j->end) ? j->p + 2 : j->end;
            continue;
        }
        break;
    }
}

// reads a "quoted string" into out (truncating if too long); j->p must be on the opening quote
static bool jp_str(JP *j, char *out, size_t cap) {
    if (j->p >= j->end || *j->p != '"') return jp_fail(j, "expected a quoted string");
    j->p++;
    size_t n = 0;
    while (j->p < j->end && *j->p != '"') {
        char c = *j->p++;
        if (c == '\\' && j->p < j->end) {
            char e = *j->p++;
            c = (e == 'n') ? '\n' : (e == 't') ? '\t' : e; // \" \\ \/ come through as themselves
        }
        if (n + 1 < cap) out[n++] = c;
    }
    if (j->p >= j->end) return jp_fail(j, "string is missing its closing quote");
    j->p++;
    out[n] = 0;
    return true;
}

static bool jp_value(JP *j, const char *path, int depth);

static void join_path(char *out, size_t cap, const char *parent, const char *key) {
    size_t i = 0;
    if (parent[0]) { i = put_s(out, i, cap, parent); i = put_s(out, i, cap, "."); }
    i = put_s(out, i, cap, key);
    out[i] = 0;
}

static bool jp_object(JP *j, const char *path, int depth) {
    j->p++; // '{'
    for (;;) {
        jp_skip(j);
        if (j->p >= j->end) return jp_fail(j, "file ended inside { }");
        if (*j->p == '}') { j->p++; return true; }
        if (*j->p == ',') { j->p++; continue; }
        char key[40];
        if (!jp_str(j, key, sizeof(key))) return false;
        jp_skip(j);
        if (j->p >= j->end || *j->p != ':') return jp_fail(j, "expected ':' after a key");
        j->p++;
        char sub[80];
        join_path(sub, sizeof(sub), path, key);
        if (!jp_value(j, sub, depth + 1)) return false;
    }
}

static bool jp_array(JP *j, const char *path, int depth) {
    j->p++; // '['
    int idx = 0;
    for (;;) {
        jp_skip(j);
        if (j->p >= j->end) return jp_fail(j, "file ended inside [ ]");
        if (*j->p == ']') { j->p++; return true; }
        if (*j->p == ',') { j->p++; continue; }
        char num[12];
        size_t k = 0;
        int v = idx;
        char tmp[12];
        int t = 0;
        if (v == 0) tmp[t++] = '0';
        while (v && t < 11) { tmp[t++] = (char)('0' + v % 10); v /= 10; }
        while (t) num[k++] = tmp[--t];
        num[k] = 0;
        char sub[80];
        join_path(sub, sizeof(sub), path, num);
        if (!jp_value(j, sub, depth + 1)) return false;
        idx++;
    }
}

static bool jp_lit(JP *j, const char *word) {
    const char *w = word;
    const char *q = j->p;
    while (*w) { if (q >= j->end || *q != *w) return false; q++; w++; }
    j->p = q;
    return true;
}

static bool jp_value(JP *j, const char *path, int depth) {
    jp_skip(j);
    if (j->p >= j->end) return jp_fail(j, "expected a value but the file ended");
    if (depth > 5) return jp_fail(j, "nested too deeply");
    char c = *j->p;
    if (c == '{') return jp_object(j, path, depth);
    if (c == '[') return jp_array(j, path, depth);
    if (c == '"') {
        char val[160];
        if (!jp_str(j, val, sizeof(val))) return false;
        cfg_leaf(path, K_STR, val, 0, 0);
        return true;
    }
    if (jp_lit(j, "true"))  { cfg_leaf(path, K_BOOL, 0, 1, 1000); return true; }
    if (jp_lit(j, "false")) { cfg_leaf(path, K_BOOL, 0, 0, 0); return true; }
    if (jp_lit(j, "null"))  { cfg_leaf(path, K_NULL, 0, 0, 0); return true; }
    if (c == '-' || (c >= '0' && c <= '9')) {
        bool neg = (c == '-');
        if (neg) j->p++;
        int32_t v = 0;
        while (j->p < j->end && *j->p >= '0' && *j->p <= '9') {
            if (v < 100000) v = v * 10 + (*j->p - '0');
            j->p++;
        }
        int32_t frac = 0;
        if (j->p < j->end && *j->p == '.') { // up to 3 decimal digits
            j->p++;
            int32_t scale = 100;
            while (j->p < j->end && *j->p >= '0' && *j->p <= '9') {
                if (scale > 0) { frac += (*j->p - '0') * scale; scale /= 10; }
                j->p++;
            }
        }
        while (j->p < j->end && (*j->p == 'e' || *j->p == 'E' || *j->p == '+' || *j->p == '-' ||
                                 (*j->p >= '0' && *j->p <= '9'))) j->p++; // skip any exponent
        int32_t milli = v * 1000 + frac;
        cfg_leaf(path, K_NUM, 0, neg ? -v : v, neg ? -milli : milli);
        return true;
    }
    return jp_fail(j, "unexpected character (check quotes, commas and brackets)");
}

// parses the whole file text into g_cfg (defaults must already be set)
static bool cfg_parse(const char *text, size_t len) {
    t_combo = 0; t_combo_seen = false; t_combo_bad = false; g_unknown_logged = 0;
    JP j = { text, text + len, false };
    if (len >= 3 && (uint8_t)text[0] == 0xEF && (uint8_t)text[1] == 0xBB && (uint8_t)text[2] == 0xBF) j.p += 3; // BOM
    jp_skip(&j);
    bool ok = false;
    if (j.p >= j.end || *j.p != '{') jp_fail(&j, "file must start with {");
    else ok = jp_object(&j, "", 0);

    if (t_combo_seen) { // validate the combo even if the rest of the file had a syntax error
        int n = popcount32(t_combo);
        if (t_combo_bad) mlog("config: combo has an unknown button, keeping the default combo");
        else if (n < 1 || n > 4) mlog("config: combo needs 1 to 4 buttons, keeping the default combo");
        else {
            g_cfg.combo_mask = t_combo;
            if (n == 1) mlog("config: WARNING single-button combo, that button will always be hidden from the game");
        }
    }
    return ok;
}

/* ---- writes the commented config.json text from the current settings ---- */

typedef struct { char *b; size_t i; size_t cap; } W;

static void w_s(W *w, const char *s) { w->i = put_s(w->b, w->i, w->cap, s); }
static void w_u(W *w, uint32_t v) { w->i = put_n(w->b, w->i, w->cap, v, false); }
static void w_bool(W *w, bool v) { w_s(w, v ? "true" : "false"); }
static void w_qstr(W *w, const char *s) {
    w_s(w, "\"");
    for (; *s; s++) {
        char t[3] = { 0, 0, 0 };
        if (*s == '"' || *s == '\\') { t[0] = '\\'; t[1] = *s; } else { t[0] = *s; }
        w_s(w, t);
    }
    w_s(w, "\"");
}

static void w_hexcolor(W *w, const uint8_t *c) {
    static const char hx[] = "0123456789ABCDEF";
    char t[10] = { '"', '#', hx[c[0] >> 4], hx[c[0] & 15], hx[c[1] >> 4], hx[c[1] & 15], hx[c[2] >> 4], hx[c[2] & 15], '"', 0 };
    w_s(w, t);
}

static size_t cfg_render(char *out, size_t cap) {
    W w = { out, 0, cap };
    char sp[16];
    w_s(&w, "{\n  \"combo\": [");
    {
        uint32_t shown = 0;
        bool first = true;
        for (size_t k = 0; k < N_BTNS; k++) {
            uint32_t bit = g_btns[k].bit;
            if ((g_cfg.combo_mask & bit) && !(shown & bit)) {
                if (!first) w_s(&w, ", ");
                w_s(&w, "\""); w_s(&w, g_btns[k].name); w_s(&w, "\"");
                shown |= bit;
                first = false;
            }
        }
    }
    w_s(&w, "],\n  \"hold_to_play_ms\": "); w_u(&w, g_cfg.hold_play_us / 1000u);
    w_s(&w, ",\n  \"speed\": "); fmt_speed(sp, sizeof(sp), g_cfg.speed_pct, true); w_s(&w, sp);
    w_s(&w, ",\n  \"slots\": "); w_u(&w, g_cfg.slots);
    w_s(&w, ",\n  \"light_bar\": "); w_bool(&w, g_cfg.bar_on);
    w_s(&w, ",\n  \"colors\": {\n    \"recording\": "); w_hexcolor(&w, g_cfg.col_rec);
    w_s(&w, ",\n    \"playing\": "); w_hexcolor(&w, g_cfg.col_play);
    w_s(&w, ",\n    \"paused\": "); w_hexcolor(&w, g_cfg.col_pause);
    w_s(&w, "\n  },\n  \"popups\": "); w_bool(&w, g_cfg.notify_on);
    w_s(&w, ",\n  \"ready_popup\": "); w_bool(&w, g_cfg.ready_popup);
    w_s(&w, ",\n  \"icon\": "); w_qstr(&w, g_cfg.icon);
    w_s(&w, ",\n  \"welcome\": "); w_bool(&w, g_cfg.welcome);
    w_s(&w, ",\n  \"version\": "); w_u(&w, CONFIG_VERSION);
    w_s(&w, "\n}\n");
    if (cap) w.b[w.i] = 0;
    return w.i;
}
// >>>PURE-END

static char g_cfgbuf[CFG_MAX + 1];
static char g_render[CFG_MAX];

static void write_file(const char *path, const void *data, size_t len) {
    int fd = sceKernelOpen(path, SCE_KERNEL_O_WRONLY | SCE_KERNEL_O_CREAT | SCE_KERNEL_O_TRUNC, 0777);
    if (fd < 0) { mlog2("could not write file:", path); return; }
    if (len) sceKernelWrite(fd, data, len);
    sceKernelClose(fd);
}

static void cfg_save_current(void) {
    sceKernelMkdir(PLUGIN_DIR, 0777);
    size_t n = cfg_render(g_render, sizeof(g_render));
    write_file(CONFIG_FILE, g_render, n);
}

static void cfg_load(void) {
    cfg_defaults();
    int fd = sceKernelOpen(CONFIG_FILE, SCE_KERNEL_O_RDONLY, 0);
    if (fd < 0) {
        mlog("config: config.json not found, creating a default one");
        cfg_save_current();
    } else {
        size_t len = 0;
        bool truncated = false;
        for (;;) {
            if (len >= CFG_MAX) { mlog("config: config.json too big, extra text ignored"); truncated = true; break; }
            int r = sceKernelRead(fd, g_cfgbuf + len, CFG_MAX - len);
            if (r <= 0) break;
            len += (size_t)r;
        }
        sceKernelClose(fd);
        g_cfgbuf[len] = 0;
        if (cfg_parse(g_cfgbuf, len)) {
            mlog("config: loaded config.json");
            if (g_cfg.file_version < CONFIG_VERSION && !truncated) {
                // old file: keep a backup, then rewrite it in the new readable format with the same values
                write_file(CONFIG_BACKUP, g_cfgbuf, len);
                cfg_save_current();
                mlog("config: upgraded config.json to the new format (old copy: config_old.json)");
            }
        } else {
            mlog("config: problems in config.json, see messages above (settings before the error still apply)");
        }
    }

    char combo[64];
    combo_name(g_cfg.combo_mask, combo, sizeof(combo));
    mlog2("config: combo =", combo);
    mlog_dec("config: hold_play_ms =", g_cfg.hold_play_us / 1000u);
    mlog_dec("config: start speed percent =", g_cfg.speed_pct);
    mlog_dec("config: slots =", g_cfg.slots);
    mlog_dec("config: light bar on =", g_cfg.bar_on);
    mlog_hex("config: recording color rgb =", ((uint32_t)g_cfg.col_rec[0] << 16) | ((uint32_t)g_cfg.col_rec[1] << 8) | g_cfg.col_rec[2]);
    mlog_hex("config: playing color rgb =", ((uint32_t)g_cfg.col_play[0] << 16) | ((uint32_t)g_cfg.col_play[1] << 8) | g_cfg.col_play[2]);
    mlog_hex("config: paused color rgb =", ((uint32_t)g_cfg.col_pause[0] << 16) | ((uint32_t)g_cfg.col_pause[1] << 8) | g_cfg.col_pause[2]);
    mlog_dec("config: popups on =", g_cfg.notify_on);
    mlog_dec("config: ready popup on =", g_cfg.ready_popup);
    mlog_dec("config: welcome screen on =", g_cfg.welcome);
    if (g_cfg.icon[0]) mlog2("config: custom popup icon:", g_cfg.icon);
}

/* ------------------------------------------------------------------ */
/* Pad data access (stable head of the pad data struct)                */
/* ------------------------------------------------------------------ */

typedef struct {
    uint32_t buttons;
    uint8_t  lx, ly, rx, ry;
    uint8_t  l2, r2, pad2[2];
} PadHead;

typedef struct {
    uint8_t r, g, b, a;
} MacroColor;

/* ------------------------------------------------------------------ */
/* Macro data                                                          */
/* ------------------------------------------------------------------ */

typedef struct __attribute__((packed)) {
    uint32_t t_us;        // microseconds since recording start
    uint32_t buttons;
    uint8_t  lx, ly, rx, ry;
    uint8_t  l2, r2;
    uint8_t  pad[2];
} MacroSample;            // 16 bytes

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint32_t version;
    uint32_t count;
    uint32_t duration_us;
} MacroHeader;

#define MACRO_MAGIC   0x4F52434Du // "MCRO"
#define MACRO_VERSION 1

typedef enum { ST_IDLE = 0, ST_RECORD, ST_PLAY } MacroState;

static MacroState  g_state = ST_IDLE;
static MacroSample g_buf[MAX_SAMPLES];
static uint32_t    g_len = 0;
static uint32_t    g_duration = 0;
static uint32_t    g_idx = 0;
static uint64_t    g_t0 = 0;               // recording start time
static bool        g_have_macro = false;
static uint32_t    g_slot = 1;             // current macro slot, 1-based

static uint32_t    g_speed_pct = 100;      // current playback speed
static bool        g_paused = false;
static uint64_t    g_play_pos = 0;         // position inside the macro, microseconds of macro time
static uint64_t    g_play_last = 0;        // real time of the previous playback step

// Every controller (handle) keeps its own "is the combo held" memory, so a second player's
// controller can't be mistaken for you releasing the combo.
typedef struct {
    bool     used;
    s32      handle;
    bool     combo_prev;
    bool     consumed;    // this hold was used as a control / hold-to-play, so releasing is not a tap
    uint64_t press_t;     // when the combo was pressed
    uint32_t cmd_prev;    // control buttons held on the previous read
} PadCtx;
static PadCtx      g_pads[MAX_PADS];
static int         g_pad_count = 0;

// The controller that starts a recording / playback "owns" the macro; other controllers are never touched.
static s32         g_owner = -1;
static uint64_t    g_owner_last = 0;       // last time the owner's controller was read
static s32         g_bar_handle = -1;      // controller whose light bar we changed

static uint64_t    g_first_read_t = 0;     // time of the first pad read
static bool        g_ready_done = false;
static uint32_t    g_read_count = 0;
static uint32_t    g_total_reads = 0;
static bool        g_seen_state = false;
static bool        g_seen_read = false;

static bool        g_bar_enabled = true;   // false if a previous run crashed in a light bar call
static bool        g_bar_verified = false; // true after the first successful light bar call this run
static bool        g_notify_enabled = true;
static bool        g_notify_verified = false;

/* ------------------------------------------------------------------ */
/* Function pointers + hooks (names must match the library functions)  */
/* ------------------------------------------------------------------ */

s32 (*scePadReadState)(s32 handle, void *data);
s32 (*scePadRead)(s32 handle, void *data, s32 count);
s32 (*scePadSetLightBar)(s32 handle, const MacroColor *color);
s32 (*scePadResetLightBar)(s32 handle);

HOOK_INIT(scePadReadState);
HOOK_INIT(scePadRead);

/* ------------------------------------------------------------------ */
/* Crash guards, popups, light bar                                     */
/* ------------------------------------------------------------------ */

static void guard_write(const char *path, char c) {
    int fd = sceKernelOpen(path, SCE_KERNEL_O_WRONLY | SCE_KERNEL_O_CREAT | SCE_KERNEL_O_TRUNC, 0777);
    if (fd < 0) return;
    sceKernelWrite(fd, &c, 1);
    sceKernelClose(fd);
}

static char guard_read(const char *path) {
    char c = 0;
    int fd = sceKernelOpen(path, SCE_KERNEL_O_RDONLY, 0);
    if (fd < 0) return 0;
    if (sceKernelRead(fd, &c, 1) != 1) c = 0;
    sceKernelClose(fd);
    return c;
}

// Layout of the system notification request (size 0xC30), as used by the payload SDK.
typedef struct {
    int  type;               // 0x00  0 = normal notification
    int  req_id;             // 0x04
    int  priority;           // 0x08
    int  msg_id;             // 0x0C
    int  target_id;          // 0x10
    int  user_id;            // 0x14
    int  unk1;               // 0x18
    int  unk2;               // 0x1C
    int  app_id;             // 0x20
    int  error_num;          // 0x24
    int  unk3;               // 0x28
    char use_icon_image_uri; // 0x2C  1 = use icon_uri
    char message[1024];      // 0x2D
    char icon_uri[1024];     // 0x42D
    char unkstr[1024];       // 0x82D
} NotiReq;
_Static_assert(sizeof(NotiReq) == 0xC30, "notification struct must be 0xC30 bytes");

static NotiReq g_noti; // static so the 3 KB doesn't sit on the game's pad-thread stack
static s32 (*p_sendNotification)(s32 device, void *req, size_t size, s32 blocking) = 0;

static void copy_str(char *dst, size_t cap, const char *src) {
    size_t i = 0;
    while (src && src[i] && i + 1 < cap) { dst[i] = src[i]; i++; }
    dst[i] = 0;
}

static bool str_eq(const char *a, const char *b) {
    while (*a && *b) { if (*a != *b) return false; a++; b++; }
    return *a == 0 && *b == 0;
}

// sends one popup right now
static void notify_now(const char *msg, const char *icon) {
#if USE_NOTIFY
    if (!g_cfg.notify_on || !g_notify_enabled || !p_sendNotification) return;
    memset(&g_noti, 0, sizeof(g_noti));
    g_noti.type = 0;
    g_noti.target_id = NOTIFY_TARGET_ID;
    g_noti.use_icon_image_uri = 1;
    copy_str(g_noti.message, sizeof(g_noti.message), msg);
    copy_str(g_noti.icon_uri, sizeof(g_noti.icon_uri), g_cfg.icon[0] ? g_cfg.icon : icon);

    if (!g_notify_verified) { guard_write(NOTIFY_STATE_FILE, 'P'); mlog("notify: first call (guarded)"); }
    s32 rr = p_sendNotification(0, &g_noti, sizeof(g_noti), 0);
    if (!g_notify_verified) {
        guard_write(NOTIFY_STATE_FILE, 'K');
        g_notify_verified = true;
        mlog_hex("notify: first call OK, returned", (uint32_t)rr);
    }
#else
    (void)msg; (void)icon;
#endif
}

// ---- anti-spam queue ----
static char     g_pend_msg[200], g_pend_icon[128], g_last_msg[200];
static bool     g_pend = false, g_ever_sent = false;
static uint64_t g_pend_since = 0, g_pend_due = 0, g_last_sent = 0;

static void notify_send_tracked(const char *msg, const char *icon, uint64_t now) {
    notify_now(msg, icon);
    copy_str(g_last_msg, sizeof(g_last_msg), msg);
    g_last_sent = now;
    g_ever_sent = true;
}

// Use this for normal popups. After a quiet moment the popup shows instantly. While you keep
// pressing things, only the newest message is kept and shown once you pause.
static void notify(const char *msg, const char *icon) {
    uint64_t now = sceKernelGetProcessTime();
    if (g_ever_sent && str_eq(msg, g_last_msg) && now - g_last_sent < 3000000) { g_pend = false; return; } // already on screen
    if (!g_pend && (!g_ever_sent || now - g_last_sent >= NOTIFY_QUIET_US)) { notify_send_tracked(msg, icon, now); return; }
    copy_str(g_pend_msg, sizeof(g_pend_msg), msg);
    copy_str(g_pend_icon, sizeof(g_pend_icon), icon);
    if (!g_pend) { g_pend = true; g_pend_since = now; }
    uint64_t due = now + NOTIFY_SETTLE_US;
    if (due > g_pend_since + NOTIFY_MAX_WAIT_US) due = g_pend_since + NOTIFY_MAX_WAIT_US;
    if (due < g_last_sent + NOTIFY_MIN_GAP_US) due = g_last_sent + NOTIFY_MIN_GAP_US;
    g_pend_due = due;
}

static void notify_flush(uint64_t now) {
    if (g_pend && now >= g_pend_due) {
        g_pend = false;
        notify_send_tracked(g_pend_msg, g_pend_icon, now);
    }
}

static void set_bar(s32 handle, uint8_t r, uint8_t g, uint8_t b) {
#if USE_LIGHTBAR
    if (!g_cfg.bar_on || !g_bar_enabled || handle < 0 || !scePadSetLightBar) return;
    MacroColor c = { r, g, b, 0 };
    if (!g_bar_verified) { guard_write(BAR_STATE_FILE, 'P'); mlog("light bar: first call (guarded)"); }
    s32 rr = scePadSetLightBar(handle, &c); // direct call, no hook on this function
    if (!g_bar_verified) {
        guard_write(BAR_STATE_FILE, 'K');
        g_bar_verified = true;
        mlog_hex("light bar: first call OK, returned", (uint32_t)rr);
    }
#else
    (void)handle; (void)r; (void)g; (void)b;
#endif
}

static void apply_state_bar(void) {
    switch (g_state) {
    case ST_RECORD:
        g_bar_handle = g_owner;
        set_bar(g_bar_handle, g_cfg.col_rec[0], g_cfg.col_rec[1], g_cfg.col_rec[2]);
        break;
    case ST_PLAY:
        g_bar_handle = g_owner;
        if (g_paused) set_bar(g_bar_handle, g_cfg.col_pause[0], g_cfg.col_pause[1], g_cfg.col_pause[2]);
        else          set_bar(g_bar_handle, g_cfg.col_play[0], g_cfg.col_play[1], g_cfg.col_play[2]);
        break;
    default:
#if USE_LIGHTBAR
        if (g_cfg.bar_on && g_bar_enabled && g_bar_verified && g_bar_handle >= 0 && scePadResetLightBar) {
            scePadResetLightBar(g_bar_handle); // back to the controller's normal color
        }
#endif
        g_bar_handle = -1;
        break;
    }
}

static void enter_state(MacroState s) {
    g_state = s;
    if (s != ST_PLAY) g_paused = false;
    mlog_dec("enter_state:", (uint32_t)s);
    apply_state_bar();
}

/* ------------------------------------------------------------------ */
/* Popup text that depends on settings                                 */
/* ------------------------------------------------------------------ */

static char g_pop[200];

static void build_messages(void) {
    char combo[64], slot[24];
    combo_name(g_cfg.combo_mask, combo, sizeof(combo));
    slot[0] = 0;
    if (g_cfg.slots > 1) {
        size_t i = 0;
        i = put_s(slot, i, sizeof(slot), " (slot ");
        i = put_n(slot, i, sizeof(slot), g_slot, false);
        i = put_s(slot, i, sizeof(slot), ")");
        slot[i] = 0;
    }
    size_t i = 0;
    i = put_s(g_msg_ready, 0, sizeof(g_msg_ready), "Macro ready: tap ");
    i = put_s(g_msg_ready, i, sizeof(g_msg_ready), combo);
    i = put_s(g_msg_ready, i, sizeof(g_msg_ready), " to record");
    g_msg_ready[i] = 0;
    i = put_s(g_msg_rec, 0, sizeof(g_msg_rec), "Macro RECORDING");
    i = put_s(g_msg_rec, i, sizeof(g_msg_rec), slot);
    i = put_s(g_msg_rec, i, sizeof(g_msg_rec), " - tap ");
    i = put_s(g_msg_rec, i, sizeof(g_msg_rec), combo);
    i = put_s(g_msg_rec, i, sizeof(g_msg_rec), " to stop and loop");
    g_msg_rec[i] = 0;
    i = put_s(g_msg_play, 0, sizeof(g_msg_play), "Macro PLAYING");
    i = put_s(g_msg_play, i, sizeof(g_msg_play), slot);
    i = put_s(g_msg_play, i, sizeof(g_msg_play), " (looping) - tap ");
    i = put_s(g_msg_play, i, sizeof(g_msg_play), combo);
    i = put_s(g_msg_play, i, sizeof(g_msg_play), " to stop");
    g_msg_play[i] = 0;
}

/* ------------------------------------------------------------------ */
/* Persistence (one file per slot)                                     */
/* ------------------------------------------------------------------ */

static void slot_path(uint32_t slot, char *out, size_t cap) {
    size_t i = 0;
    if (slot <= 1) {
        i = put_s(out, 0, cap, MACRO_FILE);
    } else {
        i = put_s(out, 0, cap, PLUGIN_DIR "/macro_");
        i = put_n(out, i, cap, slot, false);
        i = put_s(out, i, cap, ".bin");
    }
    out[i] = 0;
}

static bool load_macro(void) {
    char path[96];
    slot_path(g_slot, path, sizeof(path));
    g_have_macro = false;
    g_len = 0;
    g_duration = 0;
    int fd = sceKernelOpen(path, SCE_KERNEL_O_RDONLY, 0);
    if (fd < 0) { mlog_dec("no saved macro in slot (ok if never recorded):", g_slot); return false; }

    MacroHeader h;
    bool ok = false;
    if (sceKernelRead(fd, &h, sizeof(h)) == (int)sizeof(h) &&
        h.magic == MACRO_MAGIC && h.version == MACRO_VERSION &&
        h.count > 0 && h.count <= MAX_SAMPLES && h.duration_us >= MIN_MACRO_US) {
        int want = (int)(h.count * sizeof(MacroSample));
        if (sceKernelRead(fd, g_buf, want) == want) {
            g_len = h.count;
            g_duration = h.duration_us;
            g_have_macro = true;
            ok = true;
        }
    }
    sceKernelClose(fd);
    if (ok) {
        mlog_dec("loaded macro from slot:", g_slot);
        mlog_dec("loaded macro, samples:", g_len);
        mlog_dec("loaded macro, duration us:", g_duration);
    } else {
        mlog("macro file present but invalid, ignored");
    }
    return ok;
}

static void save_macro(void) {
    char path[96];
    slot_path(g_slot, path, sizeof(path));
    sceKernelMkdir(PLUGIN_DIR, 0777);
    int fd = sceKernelOpen(path, SCE_KERNEL_O_WRONLY | SCE_KERNEL_O_CREAT | SCE_KERNEL_O_TRUNC, 0777);
    if (fd < 0) { mlog_hex("SAVE FAILED, open error code:", (uint32_t)fd); return; }
    MacroHeader h = { MACRO_MAGIC, MACRO_VERSION, g_len, g_duration };
    sceKernelWrite(fd, &h, sizeof(h));
    sceKernelWrite(fd, g_buf, g_len * sizeof(MacroSample));
    sceKernelClose(fd);
    mlog_dec("saved macro to slot:", g_slot);
    mlog_dec("saved macro, samples:", g_len);
}

/* ------------------------------------------------------------------ */
/* Sampling helpers                                                    */
/* ------------------------------------------------------------------ */

static MacroSample snapshot(const PadHead *d) {
    MacroSample s;
    memset(&s, 0, sizeof(s));
    s.buttons = d->buttons;
    s.lx = d->lx; s.ly = d->ly;
    s.rx = d->rx; s.ry = d->ry;
    s.l2 = d->l2; s.r2 = d->r2;
    return s;
}

static void write_sample(PadHead *d, const MacroSample *s, uint32_t buttons) {
    d->buttons = buttons;
    d->lx = s->lx; d->ly = s->ly;
    d->rx = s->rx; d->ry = s->ry;
    d->l2 = s->l2; d->r2 = s->r2;
}

static bool same_state(const MacroSample *a, const MacroSample *b) {
    return a->buttons == b->buttons &&
           a->lx == b->lx && a->ly == b->ly &&
           a->rx == b->rx && a->ry == b->ry &&
           a->l2 == b->l2 && a->r2 == b->r2;
}

static uint32_t effective_buttons(const MacroSample *s) {
    uint32_t b = s->buttons;
    if (s->l2 >= TRIGGER_THRESHOLD) b |= BTN_L2; // analog trigger pulls count as presses
    if (s->r2 >= TRIGGER_THRESHOLD) b |= BTN_R2;
    return b;
}

static bool combo_down(const MacroSample *s) {
    uint32_t m = g_cfg.combo_mask;
    return m != 0 && (effective_buttons(s) & m) == m;
}

// hide the combo from the game
static void strip_combo(PadHead *d) {
    uint32_t m = g_cfg.combo_mask;
    d->buttons &= ~m;
    if (m & BTN_L2) d->l2 = 0;
    if (m & BTN_R2) d->r2 = 0;
}

/* ------------------------------------------------------------------ */
/* State machine                                                       */
/* ------------------------------------------------------------------ */

static void start_record(uint64_t now) {
    mlog("start_record: begin");
    g_len = 0;
    g_duration = 0;
    g_t0 = now;
    enter_state(ST_RECORD);
    mlog("STATE: recording started (bar should be red)");
    notify(g_msg_rec, ICON_REC);
}

static void start_play(uint64_t now) {
    g_play_pos = 0;
    g_play_last = now;
    g_idx = 0;
    g_paused = false;
    enter_state(ST_PLAY);
    mlog("STATE: playing (bar should be green)");
    notify(g_msg_play, ICON_PLAY);
}

// end_t = moment the stop combo was first pressed, so the combo isn't part of the macro
static void stop_record_and_play(uint64_t end_t, uint64_t now) {
    uint64_t dur = (end_t > g_t0) ? (end_t - g_t0) : 0;
    mlog_dec("recording ended, samples:", g_len);
    mlog_dec("recording ended, duration ms:", dur / 1000);
    if (g_len == 0 || dur < MIN_MACRO_US || dur > 0xFFFFFFFFull) {
        mlog("recording too short or empty, discarded");
        notify(MSG_SHORT, ICON_END);
        load_macro(); // restore the previous macro of this slot from disk, if any
        enter_state(ST_IDLE);
        return;
    }
    g_duration = (uint32_t)dur;
    g_have_macro = true;
    save_macro();
    start_play(now);
}

static void on_tap(uint64_t now, PadCtx *c, s32 handle) {
    mlog_dec("combo tapped, controller handle:", handle);
    switch (g_state) {
    case ST_IDLE:
        g_owner = handle; // this controller now owns the macro
        g_owner_last = now;
        start_record(now);
        break;
    case ST_RECORD: stop_record_and_play(c->press_t, now); break;
    case ST_PLAY:   enter_state(ST_IDLE); mlog("STATE: stopped"); notify(MSG_END, ICON_END); break;
    }
}

static void record_sample(const MacroSample *real, uint64_t now) {
    if (g_len >= MAX_SAMPLES) return;
    MacroSample s = *real;
    s.t_us = (uint32_t)(now - g_t0);
    if (g_len > 0 && same_state(&g_buf[g_len - 1], &s)) return; // only store changes
    g_buf[g_len++] = s;
}

// Advances the macro by real elapsed time x speed and writes the matching sample into the pad data.
static void apply_playback(PadHead *d, uint64_t now) {
    if (g_len == 0 || g_duration == 0) { enter_state(ST_IDLE); return; }

    uint64_t dt = now - g_play_last;
    g_play_last = now;
    if (g_paused) return; // paused: your real input goes through, the macro waits
    if (dt > MAX_STEP_US) dt = MAX_STEP_US;

    g_play_pos += dt * g_speed_pct / 100;
    bool wrapped = false;
    if (g_play_pos >= g_duration) { g_play_pos %= g_duration; wrapped = true; }

    uint32_t idx = wrapped ? 0 : g_idx;
    while (idx + 1 < g_len && g_buf[idx + 1].t_us <= g_play_pos) idx++;

    uint32_t buttons = g_buf[idx].buttons;
    if (g_speed_pct > 100) {
        // sped up: a very short press could fall between two pad reads, so show every button
        // that was pressed in the samples we just skipped over
        uint32_t acc = 0;
        bool passed = false;
        if (!wrapped) {
            for (uint32_t k = g_idx + 1; k <= idx; k++) { acc |= g_buf[k].buttons; passed = true; }
        } else {
            for (uint32_t k = g_idx + 1; k < g_len; k++) { acc |= g_buf[k].buttons; passed = true; }
            for (uint32_t k = 0; k <= idx; k++) { acc |= g_buf[k].buttons; passed = true; }
        }
        if (passed) buttons = acc;
    }
    g_idx = idx;
    write_sample(d, &g_buf[idx], buttons);
}

/* ------------------------------------------------------------------ */
/* Commands while the combo is held                                    */
/* ------------------------------------------------------------------ */

static void change_speed(int dir) {
    uint32_t n = dir > 0 ? speed_up(g_speed_pct) : speed_down(g_speed_pct);
    bool limit = (n == g_speed_pct);
    g_speed_pct = n;
    char sp[16];
    fmt_speed(sp, sizeof(sp), g_speed_pct, false);
    size_t i = put_s(g_pop, 0, sizeof(g_pop), "Macro speed: ");
    i = put_s(g_pop, i, sizeof(g_pop), sp);
    i = put_s(g_pop, i, sizeof(g_pop), "x");
    if (limit) i = put_s(g_pop, i, sizeof(g_pop), dir > 0 ? " (max)" : " (min)");
    g_pop[i] = 0;
    mlog_dec("speed percent now:", g_speed_pct);
    notify(g_pop, ICON_PLAY);
}

static void change_slot(int dir, uint64_t now) {
    if (g_cfg.slots <= 1) {
        notify("Only 1 macro slot (raise 'slots' in config.json)", ICON_END);
        return;
    }
    bool was_playing = (g_state == ST_PLAY);
    if (dir > 0) g_slot = (g_slot >= g_cfg.slots) ? 1 : g_slot + 1;
    else         g_slot = (g_slot <= 1) ? g_cfg.slots : g_slot - 1;
    if (was_playing) enter_state(ST_IDLE);
    load_macro();
    build_messages();
    mlog_dec("switched to slot:", g_slot);

    if (was_playing && g_have_macro) { start_play(now); return; } // keep playing, now the new macro

    char tmp[16];
    size_t i = put_s(g_pop, 0, sizeof(g_pop), "Slot ");
    i = put_n(g_pop, i, sizeof(g_pop), g_slot, false);
    i = put_s(g_pop, i, sizeof(g_pop), " of ");
    i = put_n(g_pop, i, sizeof(g_pop), g_cfg.slots, false);
    i = put_s(g_pop, i, sizeof(g_pop), ": ");
    if (g_have_macro) {
        fmt_tenths(tmp, sizeof(tmp), g_duration / 1000u);
        i = put_s(g_pop, i, sizeof(g_pop), tmp);
        i = put_s(g_pop, i, sizeof(g_pop), " s");
    } else {
        i = put_s(g_pop, i, sizeof(g_pop), "empty");
    }
    g_pop[i] = 0;
    notify(g_pop, ICON_READY);
}

static void toggle_pause(uint64_t now) {
    if (g_state != ST_PLAY) { notify("Nothing is playing to pause", ICON_END); return; }
    g_paused = !g_paused;
    if (!g_paused) g_play_last = now; // resume from where it stopped
    apply_state_bar();
    mlog(g_paused ? "STATE: paused" : "STATE: resumed");
    notify(g_paused ? "Macro paused - you have control" : "Macro resumed", ICON_PLAY);
}

static void reload_config(void) {
    mlog("reloading config.json");
    cfg_load();
    g_speed_pct = g_cfg.speed_pct;
    if (g_slot > g_cfg.slots) { g_slot = 1; load_macro(); }
    build_messages();
    apply_state_bar();
    notify("Macro config reloaded", ICON_READY);
}

static void greet_begin(bool manual);

static void handle_commands(uint32_t c) {
    uint64_t now = sceKernelGetProcessTime();
    if (g_state == ST_RECORD) { mlog("controls are ignored while recording"); return; }
    if (c & BTN_UP)       change_speed(+1);
    if (c & BTN_DOWN)     change_speed(-1);
    if (c & BTN_LEFT)     change_slot(-1, now);
    if (c & BTN_RIGHT)    change_slot(+1, now);
    if (c & BTN_TRIANGLE) toggle_pause(now);
    if (c & BTN_CIRCLE)   reload_config();
    if (c & BTN_SQUARE) {
        if (g_state == ST_IDLE) greet_begin(true);
        else notify("Stop the macro first to open the guide", ICON_END);
    }
}

/* ------------------------------------------------------------------ */
/* Welcome / controls guide (system message dialog = the dark in-game screen) */
/* ------------------------------------------------------------------ */

// Layouts of the system message dialog structs, as used by the PS4 SDK.
typedef struct { size_t size; uint8_t reserved[36]; uint32_t magic; } MD_Base;
typedef struct { const char *msg1; const char *msg2; uint8_t reserved[32]; } MD_Buttons;
typedef struct { int32_t buttonType; int32_t pad0; const char *msg; MD_Buttons *buttons; uint8_t reserved[24]; } MD_UserMsg;
typedef struct {
    MD_Base base; size_t size; int32_t mode; int32_t pad0;
    MD_UserMsg *userMsg; void *sysMsg; void *progBar; uint8_t reserved[32]; int32_t pad1;
} MD_Param;
typedef struct { int32_t mode; int32_t result; int32_t buttonId; uint8_t reserved[32]; } MD_Result;
_Static_assert(sizeof(MD_Base) == 0x30, "MD_Base size");
_Static_assert(sizeof(MD_Buttons) == 0x30, "MD_Buttons size");
_Static_assert(sizeof(MD_UserMsg) == 0x30, "MD_UserMsg size");
_Static_assert(sizeof(MD_Param) == 0x80, "MD_Param size");

#define MD_MAGIC            0xC0D1A109u
#define MD_MODE_USER_MSG    1
#define MD_BTN_OK           0
#define MD_BTN_TWO          9
#define MD_STATUS_FINISHED  3
#define GREET_LAST_PAGE     4

enum { GR_DONE = 0, GR_PENDING, GR_RUNNING };
static int      g_gr_stage = GR_DONE;
static int      g_gr_page = 0;
static uint32_t g_gr_wait = 0;
static bool     g_gr_api_ok = false;
static bool     g_gr_api_tried = false;
static bool     g_gr_manual = false;   // opened on request (combo + SQUARE) instead of on first run

static s32 (*p_cdInit)(void) = 0;
static s32 (*p_mdInit)(void) = 0;
static s32 (*p_mdOpen)(const void *param) = 0;
static s32 (*p_mdStatus)(void) = 0;
static s32 (*p_mdResult)(void *result) = 0;
static s32 (*p_mdTerm)(void) = 0;

static MD_Param   g_md_param;
static MD_UserMsg g_md_user;
static MD_Buttons g_md_btns;
static uint8_t    g_md_result_buf[128]; // bigger than MD_Result on purpose
static char       g_gr_text[700];

// loads a system module by short name, then by full path if the short name did not work
static s32 load_sys_lib(const char *bare, const char *full) {
    s32 h = 0;
    s32 r = sys_dynlib_load_prx(bare, &h);
    mlog2("welcome: loading module", bare);
    mlog_hex("welcome:   result", (uint32_t)r);
    mlog_hex("welcome:   handle", (uint32_t)h);
    if (r != 0 || h == 0) {
        h = 0;
        r = sys_dynlib_load_prx(full, &h);
        mlog2("welcome: loading module", full);
        mlog_hex("welcome:   result", (uint32_t)r);
        mlog_hex("welcome:   handle", (uint32_t)h);
        if (r != 0) h = 0;
    }
    return h;
}

static bool greet_load_api(void) {
    if (g_gr_api_tried) return g_gr_api_ok;
    g_gr_api_tried = true;
    s32 hc = load_sys_lib("libSceCommonDialog.sprx", "/system/common/lib/libSceCommonDialog.sprx");
    s32 hm = load_sys_lib("libSceMsgDialog.sprx", "/system/common/lib/libSceMsgDialog.sprx");
    if (hc) sys_dynlib_dlsym(hc, "sceCommonDialogInitialize", &p_cdInit);
    if (hm) {
        sys_dynlib_dlsym(hm, "sceMsgDialogInitialize", &p_mdInit);
        sys_dynlib_dlsym(hm, "sceMsgDialogOpen", &p_mdOpen);
        sys_dynlib_dlsym(hm, "sceMsgDialogUpdateStatus", &p_mdStatus);
        sys_dynlib_dlsym(hm, "sceMsgDialogGetResult", &p_mdResult);
        sys_dynlib_dlsym(hm, "sceMsgDialogTerminate", &p_mdTerm);
    }
    mlog_hex("welcome: sceCommonDialogInitialize at", (uint64_t)(uintptr_t)p_cdInit);
    mlog_hex("welcome: sceMsgDialogInitialize at", (uint64_t)(uintptr_t)p_mdInit);
    mlog_hex("welcome: sceMsgDialogOpen at", (uint64_t)(uintptr_t)p_mdOpen);
    mlog_hex("welcome: sceMsgDialogUpdateStatus at", (uint64_t)(uintptr_t)p_mdStatus);
    mlog_hex("welcome: sceMsgDialogGetResult at", (uint64_t)(uintptr_t)p_mdResult);
    mlog_hex("welcome: sceMsgDialogTerminate at", (uint64_t)(uintptr_t)p_mdTerm);
    g_gr_api_ok = p_mdInit && p_mdOpen && p_mdStatus && p_mdResult && p_mdTerm;
    mlog_dec("welcome: dialog functions found:", g_gr_api_ok);
    return g_gr_api_ok;
}

static void seconds_text(char *out, size_t cap, uint32_t ms) {
    size_t i = 0;
    if (ms % 1000 == 0) {
        i = put_n(out, 0, cap, ms / 1000, false);
        i = put_s(out, i, cap, ms == 1000 ? " second" : " seconds");
    } else {
        char t[16];
        fmt_tenths(t, sizeof(t), ms);
        i = put_s(out, 0, cap, t);
        i = put_s(out, i, cap, " seconds");
    }
    out[i] = 0;
}

static void greet_text(int page) {
    char c[64], hold[32];
    combo_name(g_cfg.combo_mask, c, sizeof(c));
    seconds_text(hold, sizeof(hold), g_cfg.hold_play_us / 1000u);
    size_t i = 0;
    char *o = g_gr_text;
    size_t cap = sizeof(g_gr_text);
    switch (page) {
    case 1:
        i = put_s(o, 0, cap, "Welcome to Macro Plugin!\n\n");
        i = put_s(o, i, cap, "Record your controller inputs and replay them in a loop, in any game.\n\n");
        i = put_s(o, i, cap, "Want a quick tour of the controls?");
        break;
    case 2:
        i = put_s(o, 0, cap, "THE BASICS\n\n");
        i = put_s(o, i, cap, "Tap "); i = put_s(o, i, cap, c); i = put_s(o, i, cap, ": start recording (light bar red).\n");
        i = put_s(o, i, cap, "Tap it again: stop and loop your macro (green).\n");
        i = put_s(o, i, cap, "Tap it once more: stop.\n\n");
        i = put_s(o, i, cap, "Hold "); i = put_s(o, i, cap, c); i = put_s(o, i, cap, " for ");
        i = put_s(o, i, cap, hold); i = put_s(o, i, cap, ": play your saved macro.");
        break;
    case 3:
        i = put_s(o, 0, cap, "WHILE HOLDING "); i = put_s(o, i, cap, c); i = put_s(o, i, cap, "\n\n");
        i = put_s(o, i, cap, "UP / DOWN: playback speed\n");
        i = put_s(o, i, cap, "LEFT / RIGHT: switch macro slot\n");
        i = put_s(o, i, cap, "TRIANGLE: pause / resume\n");
        i = put_s(o, i, cap, "CIRCLE: reload config.json\n");
        i = put_s(o, i, cap, "SQUARE: show this guide\n\n");
        i = put_s(o, i, cap, "These buttons never reach the game while you hold the combo.");
        break;
    default:
        i = put_s(o, 0, cap, "ALL SET\n\n");
        i = put_s(o, i, cap, "Settings: /data/macro_plugin/config.json\n\n");
        i = put_s(o, i, cap, "Two players: the controller that starts a macro is the one that controls it.\n\n");
        i = put_s(o, i, cap, "Tip: only use macros in offline play.");
        break;
    }
    o[i] = 0;
}

static bool greet_open(int page) {
    greet_text(page);
    memset(&g_md_param, 0, sizeof(g_md_param));
    memset(&g_md_user, 0, sizeof(g_md_user));
    memset(&g_md_btns, 0, sizeof(g_md_btns));
    g_md_param.base.size = sizeof(MD_Base);
    g_md_param.base.magic = (uint32_t)(MD_MAGIC + (uint64_t)(uintptr_t)&g_md_param.base);
    g_md_param.size = sizeof(MD_Param);
    g_md_param.mode = MD_MODE_USER_MSG;
    g_md_param.userMsg = &g_md_user;
    g_md_user.msg = g_gr_text;
    if (page >= GREET_LAST_PAGE) {
        g_md_user.buttonType = MD_BTN_OK;
    } else {
        g_md_user.buttonType = MD_BTN_TWO;
        g_md_user.buttons = &g_md_btns;
        g_md_btns.msg1 = (page == 1) ? "Yes" : "Next";
        g_md_btns.msg2 = (page == 1) ? "Skip" : "Done";
    }
    // an "already initialized" answer from init is harmless, so only open's answer decides
    s32 r = p_mdInit();
    mlog_hex("welcome: dialog init returned", (uint32_t)r);
    r = p_mdOpen(&g_md_param);
    mlog_hex("welcome: dialog open returned", (uint32_t)r);
    if (r < 0) { p_mdTerm(); return false; }
    mlog_dec("welcome: showing page", page);
    return true;
}

static void greet_finish(void) {
    g_gr_stage = GR_DONE;
    if (!g_gr_manual) guard_write(GREETING_FILE, 'D');
    mlog("welcome: finished");
}

// Fallback if the dialog can't be used: the same info as three popups
static void greet_fallback(void) {
    char c[64];
    combo_name(g_cfg.combo_mask, c, sizeof(c));
    size_t i = put_s(g_pop, 0, sizeof(g_pop), "Macro Plugin: tap ");
    i = put_s(g_pop, i, sizeof(g_pop), c);
    i = put_s(g_pop, i, sizeof(g_pop), " to record, again to loop, again to stop.");
    g_pop[i] = 0;
    notify_now(g_pop, ICON_READY);
    i = put_s(g_pop, 0, sizeof(g_pop), "Hold ");
    i = put_s(g_pop, i, sizeof(g_pop), c);
    i = put_s(g_pop, i, sizeof(g_pop), " to play your saved macro. While holding: UP/DOWN speed, LEFT/RIGHT slot.");
    g_pop[i] = 0;
    notify_now(g_pop, ICON_READY);
    i = put_s(g_pop, 0, sizeof(g_pop), "While holding ");
    i = put_s(g_pop, i, sizeof(g_pop), c);
    i = put_s(g_pop, i, sizeof(g_pop), ": TRIANGLE pause, CIRCLE reload, SQUARE this guide. Settings: /data/macro_plugin/config.json");
    g_pop[i] = 0;
    notify_now(g_pop, ICON_READY);
    greet_finish();
}

// manual = opened with combo + SQUARE (starts at the basics page, leaves greeting.txt alone)
static void greet_begin(bool manual) {
    g_gr_manual = manual;
    g_ready_done = true; // no "ready" popup on top of the guide
    if (!manual) guard_write(GREETING_FILE, 'P'); // if the game dies while the dialog is up, we never show it again
    if (!greet_load_api()) {
        mlog("welcome: dialog functions not available, using popups instead");
        greet_fallback();
        return;
    }
    if (p_cdInit) mlog_hex("welcome: common dialog init returned", (uint32_t)p_cdInit());
    g_gr_stage = GR_RUNNING;
    g_gr_page = manual ? 2 : 1;
    g_gr_wait = 0;
    if (!greet_open(g_gr_page)) {
        mlog("welcome: could not open the dialog, using popups instead");
        greet_fallback();
    }
}

static void greet_collect(void) {
    memset(g_md_result_buf, 0, sizeof(g_md_result_buf));
    MD_Result *res = (MD_Result *)g_md_result_buf;
    s32 rr = p_mdResult(res);
    p_mdTerm();
    bool ok = (rr == 0 && res->result == 0);
    mlog_dec("welcome: page answered:", g_gr_page);
    mlog_dec("welcome: button id:", (uint32_t)res->buttonId);
    if (ok && res->buttonId == 1 && g_gr_page < GREET_LAST_PAGE) {
        g_gr_page++;
        g_gr_wait = 20; // give the system a moment before the next page
    } else {
        greet_finish();
    }
}

static void greet_tick(uint64_t now) {
    if (g_gr_stage == GR_PENDING) {
        if (g_first_read_t && now - g_first_read_t >= GREET_AFTER_US && g_state == ST_IDLE) greet_begin(false);
        return;
    }
    if (g_gr_stage != GR_RUNNING) return;
    if (g_gr_wait) {
        if (--g_gr_wait == 0 && !greet_open(g_gr_page)) { mlog("welcome: next page failed, stopping"); greet_finish(); }
        return;
    }
    s32 st = p_mdStatus();
    if (st == MD_STATUS_FINISHED) greet_collect();
    else if (st < 0) { mlog_hex("welcome: dialog status error", (uint32_t)st); greet_finish(); }
}

/* ------------------------------------------------------------------ */
/* Controllers                                                         */
/* ------------------------------------------------------------------ */

static PadCtx *pad_ctx(s32 handle) {
    for (int i = 0; i < MAX_PADS; i++) if (g_pads[i].used && g_pads[i].handle == handle) return &g_pads[i];
    for (int i = 0; i < MAX_PADS; i++) {
        if (!g_pads[i].used) {
            memset(&g_pads[i], 0, sizeof(g_pads[i]));
            g_pads[i].used = true;
            g_pads[i].handle = handle;
            g_pad_count++;
            mlog_dec("new controller handle seen:", handle);
            mlog_dec("controllers seen so far:", g_pad_count);
            return &g_pads[i];
        }
    }
    return &g_pads[MAX_PADS - 1]; // more handles than expected: share the last slot
}

/* ------------------------------------------------------------------ */
/* Per-read processing                                                 */
/* ------------------------------------------------------------------ */

static void process_pad(s32 handle, PadHead *d, int from_read) {
    uint64_t now = sceKernelGetProcessTime();
    PadCtx *c = pad_ctx(handle);

    // one-time proof-of-life lines
    if (from_read) {
        if (!g_seen_read)  { g_seen_read = true;  mlog_dec("game uses scePadRead, handle:", handle); }
    } else {
        if (!g_seen_state) { g_seen_state = true; mlog_dec("game uses scePadReadState, handle:", handle); }
    }
    g_total_reads++;
    if (g_first_read_t == 0) g_first_read_t = now;
    if (g_total_reads == 600 || g_total_reads == 6000 || g_total_reads == 60000)
        mlog_dec("pad reads so far:", g_total_reads);

    notify_flush(now);

    // the welcome screen takes over while it is showing
    if (g_gr_stage != GR_DONE) {
        greet_tick(now);
        if (g_gr_stage == GR_RUNNING) return;
    }
    if (!g_ready_done && now - g_first_read_t >= READY_AFTER_US) {
        g_ready_done = true;
        if (g_cfg.ready_popup && g_gr_stage == GR_DONE) notify(g_msg_ready, ICON_READY);
    }

    MacroSample real = snapshot(d); // always judge the combo from REAL input
    bool combo = combo_down(&real);

    uint32_t cmd_mask = CMD_ALL & ~g_cfg.combo_mask;
    uint32_t cmd_now = effective_buttons(&real) & cmd_mask;
    uint32_t cmd_new = cmd_now & ~c->cmd_prev; // control buttons pressed since this controller's last read
    c->cmd_prev = cmd_now;

    if (g_state != ST_IDLE) {
        if (handle != g_owner) {
            // another player's controller: never touched while the owner's macro is recording / playing
            if (combo) c->consumed = true; // so letting go of it later is not mistaken for a tap
            c->combo_prev = combo;
            if (g_owner_last && now - g_owner_last > OWNER_TIMEOUT_US) {
                mlog("macro owner's controller went silent, stopping the macro");
                enter_state(ST_IDLE);
                notify(MSG_END, ICON_END);
            }
            return;
        }
        g_owner_last = now;
    }

    if (combo) {
        if (!c->combo_prev) {
            c->press_t = now;
            c->consumed = false;
            mlog_dec("combo pressed, controller handle:", handle);
        } else if (!c->consumed && g_state == ST_IDLE && g_have_macro &&
                   (now - c->press_t) >= g_cfg.hold_play_us) {
            mlog("combo held: playing saved macro");
            g_owner = handle;
            g_owner_last = now;
            start_play(now); // long hold = play saved macro
            c->consumed = true;
        }
        if (cmd_new) {
            c->consumed = true; // using a control means this was not a tap
            handle_commands(cmd_new);
        }
    } else if (c->combo_prev && !c->consumed) {
        on_tap(now, c, handle); // tap = acted on release
    }
    c->combo_prev = combo;

    if (combo) {
        strip_combo(d);
        d->buttons &= ~cmd_mask; // the control buttons never reach the game while the combo is held
    }

    if (g_state == ST_RECORD) {
        if (!combo) record_sample(&real, now);
    } else if (g_state == ST_PLAY) {
        apply_playback(d, now);
    }

    if (g_state != ST_IDLE && (++g_read_count % BAR_REAPPLY_EVERY) == 0) apply_state_bar();
}

/* ------------------------------------------------------------------ */
/* Hooks                                                               */
/* ------------------------------------------------------------------ */

s32 scePadReadState_hook(s32 handle, void *data) {
    s32 ret = HOOK_CONTINUE(scePadReadState, s32 (*)(s32, void *), handle, data);
    if (ret == 0 && data) process_pad(handle, (PadHead *)data, 0);
    return ret;
}

s32 scePadRead_hook(s32 handle, void *data, s32 count) {
    s32 ret = HOOK_CONTINUE(scePadRead, s32 (*)(s32, void *, s32), handle, data, count);
    // scePadRead returns newest-first; process oldest-first to keep state/timing in order
    if (data) {
        uint8_t *base = (uint8_t *)data;
        for (s32 i = ret - 1; i >= 0; i--) process_pad(handle, (PadHead *)(base + (size_t)i * PAD_STRIDE), 1);
    }
    return ret;
}

/* ------------------------------------------------------------------ */
/* Plugin entry points                                                 */
/* ------------------------------------------------------------------ */

s32 attr_public plugin_load(s32 argc, const char* argv[]) {
    log_reset(); // creates /data/macro_plugin and starts a fresh log.txt
    mlog("=== macro_plugin loaded ===");
    mlog_hex("plugin version:", g_pluginVersion);
    mlog_dec("pad data stride:", PAD_STRIDE);
    if (PAD_STRIDE_FALLBACK) mlog("WARNING: orbis/Pad.h not found, using fallback stride");

    final_printf("[GoldHEN] <%s\\Ver.0x%08x> %s\n", g_pluginName, g_pluginVersion, __func__);
    boot_ver();

    cfg_load();       // read /data/macro_plugin/config.json (creates / upgrades it when needed)
    g_speed_pct = g_cfg.speed_pct;
    g_slot = 1;
    load_macro();
    build_messages(); // popup text that mentions your combo

    if (guard_read(BAR_STATE_FILE) == 'P') {
        g_bar_enabled = false;
        mlog("LIGHT BAR DISABLED: the last run crashed inside a light bar call.");
        mlog("Delete /data/macro_plugin/lightbar_state.txt to try the light bar again.");
    }
    if (guard_read(NOTIFY_STATE_FILE) == 'P') {
        g_notify_enabled = false;
        mlog("NOTIFICATIONS DISABLED: the last run crashed inside a notification call.");
        mlog("Delete /data/macro_plugin/notify_state.txt to try them again.");
    }

    char gs = guard_read(GREETING_FILE);
    if (!g_cfg.welcome) {
        g_gr_stage = GR_DONE;
    } else if (gs == 'D') {
        g_gr_stage = GR_DONE;
    } else if (gs == 'P') {
        g_gr_stage = GR_DONE;
        mlog("welcome: the last attempt did not finish, not showing it again.");
        mlog("Delete /data/macro_plugin/greeting.txt to see the welcome screen again.");
    } else {
        g_gr_stage = GR_PENDING;
        mlog("welcome: first run, will show the welcome screen shortly");
    }

    s32 h = 0;
    s32 r = sys_dynlib_load_prx("libScePad.sprx", &h);
    mlog_hex("load libScePad result:", (uint32_t)r);
    mlog_hex("libScePad handle:", (uint32_t)h);
    sys_dynlib_dlsym(h, "scePadReadState", &scePadReadState);
    sys_dynlib_dlsym(h, "scePadRead", &scePadRead);
    sys_dynlib_dlsym(h, "scePadSetLightBar", &scePadSetLightBar);
    sys_dynlib_dlsym(h, "scePadResetLightBar", &scePadResetLightBar);

    // notification function lives in libkernel; try the usual module names, then the well-known handle
    {
        s32 hk = 0;
        sys_dynlib_load_prx("libkernel.sprx", &hk);
        sys_dynlib_dlsym(hk, "sceKernelSendNotificationRequest", &p_sendNotification);
        if (!p_sendNotification) {
            sys_dynlib_load_prx("libkernel_sys.sprx", &hk);
            sys_dynlib_dlsym(hk, "sceKernelSendNotificationRequest", &p_sendNotification);
        }
        if (!p_sendNotification) sys_dynlib_dlsym(0x2001, "sceKernelSendNotificationRequest", &p_sendNotification);
        mlog_hex("found sceKernelSendNotificationRequest at:", (uint64_t)(uintptr_t)p_sendNotification);
    }

    mlog_hex("found scePadReadState at:", (uint64_t)(uintptr_t)scePadReadState);
    mlog_hex("found scePadRead at:", (uint64_t)(uintptr_t)scePadRead);
    mlog_hex("found scePadSetLightBar at (not hooked):", (uint64_t)(uintptr_t)scePadSetLightBar);
    mlog_hex("found scePadResetLightBar at:", (uint64_t)(uintptr_t)scePadResetLightBar);

    if (scePadReadState)   { HOOK32(scePadReadState);   mlog("hooked scePadReadState"); }
    if (scePadRead)        { HOOK32(scePadRead);        mlog("hooked scePadRead"); }

    mlog("plugin_load finished, waiting for pad reads");
    mlog_dec("light bar feedback enabled:", USE_LIGHTBAR && g_bar_enabled && g_cfg.bar_on);
    mlog_dec("notifications enabled:", USE_NOTIFY && g_notify_enabled && g_cfg.notify_on && p_sendNotification != 0);
    return 0;
}

s32 attr_public plugin_unload(s32 argc, const char* argv[]) {
    mlog("plugin_unload");
    g_state = ST_IDLE;
    if (scePadReadState)   { UNHOOK(scePadReadState); }
    if (scePadRead)        { UNHOOK(scePadRead); }
    return 0;
}

s32 attr_module_hidden module_start(s64 argc, const void *args)
{
    return 0;
}

s32 attr_module_hidden module_stop(s64 argc, const void *args)
{
    return 0;
}
