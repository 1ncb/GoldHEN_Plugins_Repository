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
//   /data/macro_plugin/log.txt    - plugin log, overwritten each time the plugin loads
//   /data/macro_plugin/macro.bin  - the saved macro

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

#define MAX_SAMPLES          16384          // 16 bytes each = 256 KB
#define MIN_MACRO_US         200000         // ignore recordings shorter than 0.2 s
#define HOLD_PLAY_SAVED_US   1000000        // hold combo this long to play saved macro
#define R2_ANALOG_THRESHOLD  200            // treat R2 as pressed above this (0-255)
#define BAR_REAPPLY_EVERY    120            // re-apply light bar every N pad reads

// Light bar feedback (red = recording, green = playing).
// Guarded: if the game ever crashes inside a light bar call, the plugin notices on the
// next launch and turns the light bar off by itself (see lightbar_state.txt).
#define USE_LIGHTBAR         1
#define BAR_STATE_FILE       "/data/macro_plugin/lightbar_state.txt"

// Standard PS4 button bits. To change the combo, edit these two lines.
#define BTN_L2   0x00000100u
#define BTN_R2   0x00000200u
#define BTN_L1   0x00000400u
#define COMBO_BTN_A  BTN_L1
#define COMBO_BTN_B  BTN_R2

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

static void bar_state_write(char c) {
    int fd = sceKernelOpen(BAR_STATE_FILE, SCE_KERNEL_O_WRONLY | SCE_KERNEL_O_CREAT | SCE_KERNEL_O_TRUNC, 0777);
    if (fd < 0) return;
    sceKernelWrite(fd, &c, 1);
    sceKernelClose(fd);
}

static char bar_state_read(void) {
    char c = 0;
    int fd = sceKernelOpen(BAR_STATE_FILE, SCE_KERNEL_O_RDONLY, 0);
    if (fd < 0) return 0;
    if (sceKernelRead(fd, &c, 1) != 1) c = 0;
    sceKernelClose(fd);
    return c;
}

static void set_bar(uint8_t r, uint8_t g, uint8_t b) {
#if USE_LIGHTBAR
    if (!g_bar_enabled || g_handle < 0 || !scePadSetLightBar) return;
    MacroColor c = { r, g, b, 0 };
    if (!g_bar_verified) { bar_state_write('P'); mlog("light bar: first call (guarded)"); }
    s32 rr = scePadSetLightBar(g_handle, &c); // direct call, no hook on this function
    if (!g_bar_verified) {
        bar_state_write('K');
        g_bar_verified = true;
        mlog_hex("light bar: first call OK, returned", (uint32_t)rr);
    }
#else
    (void)r; (void)g; (void)b;
#endif
}

static void apply_state_bar(void) {
    switch (g_state) {
    case ST_RECORD: set_bar(255, 0, 0); break;
    case ST_PLAY:   set_bar(0, 255, 0); break;
    default:
#if USE_LIGHTBAR
        if (g_bar_enabled && g_bar_verified && g_handle >= 0 && scePadResetLightBar) {
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

static bool combo_down(const MacroSample *s) {
    bool a = (s->buttons & COMBO_BTN_A) != 0;
    bool b = (s->buttons & COMBO_BTN_B) != 0 || s->r2 >= R2_ANALOG_THRESHOLD;
    return a && b;
}

static void strip_combo(PadHead *d) {
    d->buttons &= ~(uint32_t)(COMBO_BTN_A | COMBO_BTN_B);
    d->r2 = 0;
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
}

static void start_play(uint64_t now) {
    g_t0 = now;
    g_idx = 0;
    enter_state(ST_PLAY);
    mlog("STATE: playing (bar should be green)");
}

// end_t = moment the stop combo was first pressed, so the combo isn't part of the macro
static void stop_record_and_play(uint64_t end_t, uint64_t now) {
    uint64_t dur = (end_t > g_t0) ? (end_t - g_t0) : 0;
    mlog_dec("recording ended, samples:", g_len);
    mlog_dec("recording ended, duration ms:", dur / 1000);
    if (g_len == 0 || dur < MIN_MACRO_US || dur > 0xFFFFFFFFull) {
        mlog("recording too short or empty, discarded");
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
    case ST_PLAY:   enter_state(ST_IDLE); mlog("STATE: stopped"); break;
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
                   (now - g_combo_press_t) >= HOLD_PLAY_SAVED_US) {
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

    load_macro();

    if (bar_state_read() == 'P') {
        g_bar_enabled = false;
        mlog("LIGHT BAR DISABLED: the last run crashed inside a light bar call.");
        mlog("Delete /data/macro_plugin/lightbar_state.txt to try the light bar again.");
    }

    s32 h = 0;
    s32 r = sys_dynlib_load_prx("libScePad.sprx", &h);
    mlog_hex("load libScePad result:", (uint32_t)r);
    mlog_hex("libScePad handle:", (uint32_t)h);
    sys_dynlib_dlsym(h, "scePadReadState", &scePadReadState);
    sys_dynlib_dlsym(h, "scePadRead", &scePadRead);
    sys_dynlib_dlsym(h, "scePadSetLightBar", &scePadSetLightBar);
    sys_dynlib_dlsym(h, "scePadResetLightBar", &scePadResetLightBar);

    mlog_hex("found scePadReadState at:", (uint64_t)(uintptr_t)scePadReadState);
    mlog_hex("found scePadRead at:", (uint64_t)(uintptr_t)scePadRead);
    mlog_hex("found scePadSetLightBar at (not hooked):", (uint64_t)(uintptr_t)scePadSetLightBar);
    mlog_hex("found scePadResetLightBar at:", (uint64_t)(uintptr_t)scePadResetLightBar);

    if (scePadReadState)   { HOOK32(scePadReadState);   mlog("hooked scePadReadState"); }
    if (scePadRead)        { HOOK32(scePadRead);        mlog("hooked scePadRead"); }

    mlog("plugin_load finished, waiting for pad reads");
    mlog_dec("light bar feedback enabled:", USE_LIGHTBAR && g_bar_enabled);
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
