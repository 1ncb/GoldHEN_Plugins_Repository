// macro_plugin - single-slot controller macro recorder / looper for GoldHEN (PS4)
//
// Combo = L1 + R2 (hidden from the game; actions fire when you RELEASE the combo)
//   IDLE      --tap-->  RECORDING   (light bar RED)
//   RECORDING --tap-->  PLAYING     (light bar GREEN, loops; macro saved to disk)
//   PLAYING   --tap-->  IDLE
//   IDLE + hold combo ~1 s --> play the saved macro without re-recording
//
// Timing is timestamp based (microseconds), so playback is independent of game FPS.
//
// Files (folder is created automatically):
//   /data/macro_plugin/config.json - settings (combo, light bar colors, popups); a default
//                                    one is written on first run
//   /data/macro_plugin/log.txt     - plugin log, overwritten each time the plugin loads
//   /data/macro_plugin/macro.bin   - the saved macro

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
attr_public u32 g_pluginVersion = 0x00000100; // 1.00

/* ------------------------------------------------------------------ */
/* Config                                                              */
/* ------------------------------------------------------------------ */

#define PLUGIN_DIR           "/data/macro_plugin"
#define LOG_FILE             "/data/macro_plugin/log.txt"
#define MACRO_FILE           "/data/macro_plugin/macro.bin"
#define CONFIG_FILE          "/data/macro_plugin/config.json"
#define CFG_MAX              4096           // config.json bigger than this is cut off

#define MAX_SAMPLES          16384          // 16 bytes each = 256 KB
#define MIN_MACRO_US         200000         // ignore recordings shorter than 0.2 s
#define R2_ANALOG_THRESHOLD  200            // treat R2 as pressed above this (0-255)
#define BAR_REAPPLY_EVERY    120            // re-apply light bar every N pad reads

// Light bar feedback (red = recording, green = playing).
// Guarded: if the game ever crashes inside a light bar call, the plugin notices on the
// next launch and turns the light bar off by itself (see lightbar_state.txt).
#define USE_LIGHTBAR         1
#define BAR_STATE_FILE       "/data/macro_plugin/lightbar_state.txt"

// Popup notifications (top-left of the screen): ready / recording / playing / stopped.
// Guarded like the light bar: if the game ever crashes inside a notification call, the
// plugin turns notifications off on the next launch (see notify_state.txt).
#define USE_NOTIFY           1
#define NOTIFY_STATE_FILE    "/data/macro_plugin/notify_state.txt"
#define NOTIFY_TARGET_ID     (-1)           // if no popup ever shows, try 0 here
#define READY_NOTIFY_AT_READ 300            // "ready" popup after ~5 s of pad reads

// Built-in system icons for now (custom icon is the next experiment)
#define ICON_READY "cxml://psnotification/tex_default_icon_tips"
#define ICON_REC   "cxml://psnotification/tex_icon_live_start"
#define ICON_PLAY  "cxml://psnotification/tex_icon_loading"
#define ICON_END   "cxml://psnotification/tex_icon_ban"

#define MSG_END    "Macro stopped"
#define MSG_SHORT  "Macro: recording too short, discarded"

// Standard PS4 button bits (the combo itself is chosen in config.json)
#define BTN_L2   0x00000100u
#define BTN_R2   0x00000200u
#define BTN_L1   0x00000400u

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
    char b[200];
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
    bool     bar_on;
    uint8_t  col_rec[3];
    uint8_t  col_play[3];
    bool     notify_on;
    bool     ready_popup;
    char     icon[128];      // "" = built-in icons; otherwise this URI is used for every popup
} Config;

static Config g_cfg;
static char   g_msg_ready[128];
static char   g_msg_rec[128];
static char   g_msg_play[128];

static void cfg_defaults(void) {
    memset(&g_cfg, 0, sizeof(g_cfg));
    g_cfg.combo_mask = BTN_L1 | BTN_R2;
    g_cfg.hold_play_us = 1000000;
    g_cfg.bar_on = true;
    g_cfg.col_rec[0] = 255;  g_cfg.col_rec[1] = 0;    g_cfg.col_rec[2] = 0;
    g_cfg.col_play[0] = 0;   g_cfg.col_play[1] = 255; g_cfg.col_play[2] = 0;
    g_cfg.notify_on = true;
    g_cfg.ready_popup = true;
}

typedef struct { const char *name; uint32_t bit; } BtnDef;
// canonical names first; the first name listed for a bit is the one shown in popups
static const BtnDef g_btns[] = {
    {"L1", 0x400},   {"L2", 0x100},    {"L3", 0x2},
    {"R1", 0x800},   {"R2", 0x200},    {"R3", 0x4},
    {"CROSS", 0x4000}, {"X", 0x4000},
    {"CIRCLE", 0x2000}, {"O", 0x2000},
    {"SQUARE", 0x8000}, {"TRIANGLE", 0x1000},
    {"UP", 0x10},    {"DOWN", 0x40},   {"LEFT", 0x80},  {"RIGHT", 0x20},
    {"OPTIONS", 0x8},
    {"TOUCHPAD", 0x100000}, {"TOUCH", 0x100000},
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
    char buf[200];
    size_t i = 0;
    i = put_s(buf, i, sizeof(buf), a);
    i = put_s(buf, i, sizeof(buf), " ");
    i = put_s(buf, i, sizeof(buf), b);
    buf[i] = 0;
    mlog(buf);
}

static void compose3(char *out, size_t cap, const char *a, const char *b, const char *c) {
    size_t i = 0;
    i = put_s(out, i, cap, a);
    i = put_s(out, i, cap, b);
    i = put_s(out, i, cap, c);
    out[i] = 0;
}

static void build_messages(void) {
    char combo[64];
    combo_name(g_cfg.combo_mask, combo, sizeof(combo));
    compose3(g_msg_ready, sizeof(g_msg_ready), "Macro ready: tap ", combo, " to record");
    compose3(g_msg_rec,   sizeof(g_msg_rec),   "Macro RECORDING - tap ", combo, " to stop and loop");
    compose3(g_msg_play,  sizeof(g_msg_play),  "Macro PLAYING (looping) - tap ", combo, " to stop");
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

static void cfg_leaf(const char *path, int kind, const char *s, int32_t n) {
    bool b;
    if (path[0] == '_') return; // "_help" and friends are comments

    if (path_eq(path, "combo") || after_prefix(path, "combo.")) {
        if (kind != K_STR) { mlog2("config: combo entries must be button names in quotes:", path); t_combo_bad = true; return; }
        t_combo_seen = true;
        combo_add(s);
        return;
    }
    if (path_eq(path, "hold_play_ms")) {
        if (kind != K_NUM) { mlog2("config: expected a number:", path); return; }
        if (n < 300) n = 300;
        if (n > 10000) n = 10000;
        g_cfg.hold_play_us = (uint32_t)n * 1000u;
        return;
    }
    if (path_eq(path, "light_bar.enabled")) {
        if (want_bool(kind, n, &b)) g_cfg.bar_on = b; else mlog2("config: expected true/false:", path);
        return;
    }
    if (color_key(path, "light_bar.recording", g_cfg.col_rec, kind, s, n)) return;
    if (color_key(path, "light_bar.playing", g_cfg.col_play, kind, s, n)) return;
    if (path_eq(path, "notifications.enabled")) {
        if (want_bool(kind, n, &b)) g_cfg.notify_on = b; else mlog2("config: expected true/false:", path);
        return;
    }
    if (path_eq(path, "notifications.ready_popup")) {
        if (want_bool(kind, n, &b)) g_cfg.ready_popup = b; else mlog2("config: expected true/false:", path);
        return;
    }
    if (path_eq(path, "notifications.icon")) {
        if (kind == K_STR) {
            size_t i = 0;
            while (s[i] && i + 1 < sizeof(g_cfg.icon)) { g_cfg.icon[i] = s[i]; i++; }
            g_cfg.icon[i] = 0;
        } else mlog2("config: icon must be text in quotes:", path);
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
        cfg_leaf(path, K_STR, val, 0);
        return true;
    }
    if (jp_lit(j, "true"))  { cfg_leaf(path, K_BOOL, 0, 1); return true; }
    if (jp_lit(j, "false")) { cfg_leaf(path, K_BOOL, 0, 0); return true; }
    if (jp_lit(j, "null"))  { cfg_leaf(path, K_NULL, 0, 0); return true; }
    if (c == '-' || (c >= '0' && c <= '9')) {
        bool neg = (c == '-');
        if (neg) j->p++;
        int32_t v = 0;
        while (j->p < j->end && *j->p >= '0' && *j->p <= '9') {
            if (v < 100000000) v = v * 10 + (*j->p - '0');
            j->p++;
        }
        while (j->p < j->end && (*j->p == '.' || *j->p == 'e' || *j->p == 'E' || *j->p == '+' || *j->p == '-' ||
                                 (*j->p >= '0' && *j->p <= '9'))) j->p++; // skip fraction/exponent
        cfg_leaf(path, K_NUM, 0, neg ? -v : v);
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
// >>>PURE-END

static const char DEFAULT_CONFIG[] =
"{\n"
"  \"_help\": \"combo: 1-4 buttons from L1 R1 L2 R2 L3 R3 CROSS CIRCLE SQUARE TRIANGLE UP DOWN LEFT RIGHT OPTIONS TOUCHPAD. Colors: [red,green,blue] 0-255 or #RRGGBB. icon: leave empty for the built-in icons. Changes apply the next time a game launches.\",\n"
"  \"combo\": [\"L1\", \"R2\"],\n"
"  \"hold_play_ms\": 1000,\n"
"  \"light_bar\": {\n"
"    \"enabled\": true,\n"
"    \"recording\": [255, 0, 0],\n"
"    \"playing\": [0, 255, 0]\n"
"  },\n"
"  \"notifications\": {\n"
"    \"enabled\": true,\n"
"    \"ready_popup\": true,\n"
"    \"icon\": \"\"\n"
"  }\n"
"}\n";

static char g_cfgbuf[CFG_MAX + 1];

static void cfg_write_default(void) {
    sceKernelMkdir(PLUGIN_DIR, 0777);
    int fd = sceKernelOpen(CONFIG_FILE, SCE_KERNEL_O_WRONLY | SCE_KERNEL_O_CREAT | SCE_KERNEL_O_TRUNC, 0777);
    if (fd < 0) { mlog_hex("config: could not write default config.json, error:", (uint32_t)fd); return; }
    size_t n = 0;
    while (DEFAULT_CONFIG[n]) n++;
    sceKernelWrite(fd, DEFAULT_CONFIG, n);
    sceKernelClose(fd);
    mlog("config: wrote a default config.json");
}

static void cfg_load(void) {
    cfg_defaults();
    int fd = sceKernelOpen(CONFIG_FILE, SCE_KERNEL_O_RDONLY, 0);
    if (fd < 0) {
        mlog("config: config.json not found, using defaults");
        cfg_write_default();
    } else {
        size_t len = 0;
        for (;;) {
            if (len >= CFG_MAX) { mlog("config: config.json too big, extra text ignored"); break; }
            int r = sceKernelRead(fd, g_cfgbuf + len, CFG_MAX - len);
            if (r <= 0) break;
            len += (size_t)r;
        }
        sceKernelClose(fd);
        g_cfgbuf[len] = 0;
        if (cfg_parse(g_cfgbuf, len)) mlog("config: loaded config.json");
        else mlog("config: problems in config.json, see messages above (settings before the error still apply)");
    }

    char combo[64];
    combo_name(g_cfg.combo_mask, combo, sizeof(combo));
    mlog2("config: combo =", combo);
    mlog_dec("config: hold_play_ms =", g_cfg.hold_play_us / 1000u);
    mlog_dec("config: light bar on =", g_cfg.bar_on);
    mlog_hex("config: recording color rgb =", ((uint32_t)g_cfg.col_rec[0] << 16) | ((uint32_t)g_cfg.col_rec[1] << 8) | g_cfg.col_rec[2]);
    mlog_hex("config: playing color rgb =", ((uint32_t)g_cfg.col_play[0] << 16) | ((uint32_t)g_cfg.col_play[1] << 8) | g_cfg.col_play[2]);
    mlog_dec("config: popups on =", g_cfg.notify_on);
    mlog_dec("config: ready popup on =", g_cfg.ready_popup);
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
static uint64_t    g_t0 = 0;
static bool        g_have_macro = false;

static bool        g_combo_prev = false;
static bool        g_combo_consumed = false;
static uint64_t    g_combo_press_t = 0;

static s32         g_handle = -1;
static uint32_t    g_read_count = 0;
static uint32_t    g_total_reads = 0;
static bool        g_seen_state = false;
static bool        g_seen_read = false;

static bool        g_bar_enabled = true;   // false if a previous run crashed in a light bar call
static bool        g_bar_verified = false; // true after the first successful light bar call this run

static bool        g_notify_enabled = true;   // false if a previous run crashed in a notification call
static bool        g_notify_verified = false; // true after the first successful notification this run


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
/* Light bar                                                           */
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

/* ------------------------------------------------------------------ */
/* Popup notifications                                                 */
/* ------------------------------------------------------------------ */

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

static void notify(const char *msg, const char *icon) {
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

/* ------------------------------------------------------------------ */
/* Light bar                                                           */
/* ------------------------------------------------------------------ */

static void set_bar(uint8_t r, uint8_t g, uint8_t b) {
#if USE_LIGHTBAR
    if (!g_cfg.bar_on || !g_bar_enabled || g_handle < 0 || !scePadSetLightBar) return;
    MacroColor c = { r, g, b, 0 };
    if (!g_bar_verified) { guard_write(BAR_STATE_FILE, 'P'); mlog("light bar: first call (guarded)"); }
    s32 rr = scePadSetLightBar(g_handle, &c); // direct call, no hook on this function
    if (!g_bar_verified) {
        guard_write(BAR_STATE_FILE, 'K');
        g_bar_verified = true;
        mlog_hex("light bar: first call OK, returned", (uint32_t)rr);
    }
#else
    (void)r; (void)g; (void)b;
#endif
}

static void apply_state_bar(void) {
    switch (g_state) {
    case ST_RECORD: set_bar(g_cfg.col_rec[0], g_cfg.col_rec[1], g_cfg.col_rec[2]); break;
    case ST_PLAY:   set_bar(g_cfg.col_play[0], g_cfg.col_play[1], g_cfg.col_play[2]); break;
    default:
#if USE_LIGHTBAR
        if (g_cfg.bar_on && g_bar_enabled && g_bar_verified && g_handle >= 0 && scePadResetLightBar) {
            scePadResetLightBar(g_handle); // back to the controller's normal color
        }
#endif
        break;
    }
}

static void enter_state(MacroState s) {
    g_state = s;
    mlog_dec("enter_state:", (uint32_t)s);
    apply_state_bar();
}

/* ------------------------------------------------------------------ */
/* Persistence                                                         */
/* ------------------------------------------------------------------ */

static bool load_macro(void) {
    g_have_macro = false;
    int fd = sceKernelOpen(MACRO_FILE, SCE_KERNEL_O_RDONLY, 0);
    if (fd < 0) { mlog("no saved macro file (ok on first run)"); return false; }

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
        mlog_dec("loaded macro, samples:", g_len);
        mlog_dec("loaded macro, duration us:", g_duration);
    } else {
        mlog("macro file present but invalid, ignored");
    }
    return ok;
}

static void save_macro(void) {
    sceKernelMkdir(PLUGIN_DIR, 0777);
    int fd = sceKernelOpen(MACRO_FILE, SCE_KERNEL_O_WRONLY | SCE_KERNEL_O_CREAT | SCE_KERNEL_O_TRUNC, 0777);
    if (fd < 0) { mlog_hex("SAVE FAILED, open error code:", (uint32_t)fd); return; }
    MacroHeader h = { MACRO_MAGIC, MACRO_VERSION, g_len, g_duration };
    sceKernelWrite(fd, &h, sizeof(h));
    sceKernelWrite(fd, g_buf, g_len * sizeof(MacroSample));
    sceKernelClose(fd);
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

static void write_sample(PadHead *d, const MacroSample *s) {
    d->buttons = s->buttons;
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
    if (s->l2 >= R2_ANALOG_THRESHOLD) b |= BTN_L2; // analog trigger pulls count as presses
    if (s->r2 >= R2_ANALOG_THRESHOLD) b |= BTN_R2;
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
    g_t0 = now;
    g_idx = 0;
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
        load_macro(); // restore previous macro from disk, if any
        enter_state(ST_IDLE);
        return;
    }
    g_duration = (uint32_t)dur;
    g_have_macro = true;
    save_macro();
    start_play(now);
}

static void on_tap(uint64_t now) {
    mlog("combo tapped");
    switch (g_state) {
    case ST_IDLE:   start_record(now); break;
    case ST_RECORD: stop_record_and_play(g_combo_press_t, now); break;
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

static void apply_playback(PadHead *d, uint64_t now) {
    if (g_len == 0 || g_duration == 0) { enter_state(ST_IDLE); return; }

    uint64_t el = now - g_t0;
    if (el >= g_duration) { // loop
        g_t0 += (el / g_duration) * (uint64_t)g_duration;
        el = now - g_t0;
        g_idx = 0;
    }
    while (g_idx + 1 < g_len && g_buf[g_idx + 1].t_us <= el) g_idx++;
    write_sample(d, &g_buf[g_idx]);
}

static void process_pad(s32 handle, PadHead *d, int from_read) {
    g_handle = handle;

    // one-time proof-of-life lines
    if (from_read) {
        if (!g_seen_read)  { g_seen_read = true;  mlog_dec("game uses scePadRead, handle:", handle); }
    } else {
        if (!g_seen_state) { g_seen_state = true; mlog_dec("game uses scePadReadState, handle:", handle); }
    }
    g_total_reads++;
    if (g_cfg.ready_popup && g_total_reads == READY_NOTIFY_AT_READ) notify(g_msg_ready, ICON_READY);
    if (g_total_reads == 600 || g_total_reads == 6000 || g_total_reads == 60000)
        mlog_dec("pad reads so far:", g_total_reads);

    uint64_t now = sceKernelGetProcessTime();

    MacroSample real = snapshot(d); // always judge the combo from REAL input
    bool combo = combo_down(&real);

    if (combo) {
        if (!g_combo_prev) {
            g_combo_press_t = now;
            g_combo_consumed = false;
            mlog("combo pressed (L1+R2 seen)");
        } else if (!g_combo_consumed && g_state == ST_IDLE && g_have_macro &&
                   (now - g_combo_press_t) >= g_cfg.hold_play_us) {
            mlog("combo held: playing saved macro");
            start_play(now); // long hold = play saved macro
            g_combo_consumed = true;
        }
    } else if (g_combo_prev && !g_combo_consumed) {
        on_tap(now); // tap = acted on release
    }
    g_combo_prev = combo;

    if (combo) strip_combo(d);

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

    cfg_load();       // read /data/macro_plugin/config.json (writes a default one if missing)
    build_messages(); // popup text that mentions your combo

    load_macro();

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
    mlog_dec("light bar feedback enabled:", USE_LIGHTBAR && g_bar_enabled);
    mlog_dec("notifications enabled:", USE_NOTIFY && g_notify_enabled && p_sendNotification != 0);
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
