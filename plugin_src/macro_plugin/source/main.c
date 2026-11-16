// macro_plugin - single-slot controller macro recorder / looper for GoldHEN (PS4)
//
// Combo = L1 + R2 (hidden from the game; actions fire when you RELEASE the combo)
//   IDLE      --tap-->  RECORDING   (light bar RED)
//   RECORDING --tap-->  PLAYING     (light bar GREEN, loops; macro saved to disk)
//   PLAYING   --tap-->  IDLE
//   IDLE + hold combo ~1 s --> play the saved macro without re-recording
//
// Timing is timestamp based (microseconds), so playback is independent of game FPS.
// Saved macro: /data/GoldHEN/macros/macro.bin

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
#else
#define PAD_STRIDE 0x80 // fallback guess for sizeof(OrbisPadData); verify if scePadRead misbehaves
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

#define MACRO_DIR            "/data/GoldHEN/macros"
#define MACRO_FILE           "/data/GoldHEN/macros/macro.bin"

#define MAX_SAMPLES          16384          // 16 bytes each = 256 KB
#define MIN_MACRO_US         200000         // ignore recordings shorter than 0.2 s
#define HOLD_PLAY_SAVED_US   1000000        // hold combo this long to play saved macro
#define R2_ANALOG_THRESHOLD  200            // treat R2 as pressed above this (0-255)
#define BAR_REAPPLY_EVERY    120            // re-apply light bar every N pad reads

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
#ifndef SCE_KERNEL_O_CREAT
#define SCE_KERNEL_O_CREAT  0x0200
#endif
#ifndef SCE_KERNEL_O_TRUNC
#define SCE_KERNEL_O_TRUNC  0x0400
#endif

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
static bool        g_logged_first = false;

static bool        g_self_call = false;
static bool        g_have_game_bar = false;
static MacroColor  g_game_bar;

/* ------------------------------------------------------------------ */
/* Function pointers + hooks (names must match the library functions)  */
/* ------------------------------------------------------------------ */

s32 (*scePadReadState)(s32 handle, void *data);
s32 (*scePadRead)(s32 handle, void *data, s32 count);
s32 (*scePadSetLightBar)(s32 handle, const MacroColor *color);
s32 (*scePadResetLightBar)(s32 handle);

HOOK_INIT(scePadReadState);
HOOK_INIT(scePadRead);
HOOK_INIT(scePadSetLightBar);

/* ------------------------------------------------------------------ */
/* Light bar                                                           */
/* ------------------------------------------------------------------ */

static void set_bar(uint8_t r, uint8_t g, uint8_t b) {
    if (g_handle < 0 || !scePadSetLightBar) return;
    MacroColor c = { r, g, b, 0 };
    g_self_call = true; // tell our own hook not to swallow this
    scePadSetLightBar(g_handle, &c);
    g_self_call = false;
}

static void apply_state_bar(void) {
    switch (g_state) {
    case ST_RECORD: set_bar(255, 0, 0); break;
    case ST_PLAY:   set_bar(0, 255, 0); break;
    default:
        if (g_have_game_bar) {
            set_bar(g_game_bar.r, g_game_bar.g, g_game_bar.b);
        } else if (g_handle >= 0 && scePadResetLightBar) {
            scePadResetLightBar(g_handle);
        }
        break;
    }
}

static void enter_state(MacroState s) {
    g_state = s;
    apply_state_bar();
}

/* ------------------------------------------------------------------ */
/* Persistence                                                         */
/* ------------------------------------------------------------------ */

static bool load_macro(void) {
    g_have_macro = false;
    int fd = sceKernelOpen(MACRO_FILE, SCE_KERNEL_O_RDONLY, 0);
    if (fd < 0) return false;

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
    if (ok) final_printf("[macro] loaded %u samples, %u us\n", (unsigned)g_len, (unsigned)g_duration);
    return ok;
}

static void save_macro(void) {
    sceKernelMkdir(MACRO_DIR, 0777);
    int fd = sceKernelOpen(MACRO_FILE, SCE_KERNEL_O_WRONLY | SCE_KERNEL_O_CREAT | SCE_KERNEL_O_TRUNC, 0777);
    if (fd < 0) { final_printf("[macro] save failed: open\n"); return; }
    MacroHeader h = { MACRO_MAGIC, MACRO_VERSION, g_len, g_duration };
    sceKernelWrite(fd, &h, sizeof(h));
    sceKernelWrite(fd, g_buf, g_len * sizeof(MacroSample));
    sceKernelClose(fd);
    final_printf("[macro] saved %u samples\n", (unsigned)g_len);
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
    g_len = 0;
    g_duration = 0;
    g_t0 = now;
    enter_state(ST_RECORD);
    final_printf("[macro] recording\n");
}

static void start_play(uint64_t now) {
    g_t0 = now;
    g_idx = 0;
    enter_state(ST_PLAY);
    final_printf("[macro] playing\n");
}

// end_t = moment the stop combo was first pressed, so the combo isn't part of the macro
static void stop_record_and_play(uint64_t end_t, uint64_t now) {
    uint64_t dur = (end_t > g_t0) ? (end_t - g_t0) : 0;
    if (g_len == 0 || dur < MIN_MACRO_US || dur > 0xFFFFFFFFull) {
        final_printf("[macro] recording too short, discarded\n");
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
    switch (g_state) {
    case ST_IDLE:   start_record(now); break;
    case ST_RECORD: stop_record_and_play(g_combo_press_t, now); break;
    case ST_PLAY:   enter_state(ST_IDLE); final_printf("[macro] stopped\n"); break;
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

static void process_pad(s32 handle, PadHead *d) {
    g_handle = handle;
    if (!g_logged_first) {
        g_logged_first = true;
        final_printf("[macro] pad hook active (handle %d)\n", (int)handle);
    }
    uint64_t now = sceKernelGetProcessTime();

    MacroSample real = snapshot(d); // always judge the combo from REAL input
    bool combo = combo_down(&real);

    if (combo) {
        if (!g_combo_prev) {
            g_combo_press_t = now;
            g_combo_consumed = false;
        } else if (!g_combo_consumed && g_state == ST_IDLE && g_have_macro &&
                   (now - g_combo_press_t) >= HOLD_PLAY_SAVED_US) {
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
    if (ret == 0 && data) process_pad(handle, (PadHead *)data);
    return ret;
}

s32 scePadRead_hook(s32 handle, void *data, s32 count) {
    s32 ret = HOOK_CONTINUE(scePadRead, s32 (*)(s32, void *, s32), handle, data, count);
    // scePadRead returns newest-first; process oldest-first to keep state/timing in order
    if (data) {
        uint8_t *base = (uint8_t *)data;
        for (s32 i = ret - 1; i >= 0; i--) process_pad(handle, (PadHead *)(base + (size_t)i * PAD_STRIDE));
    }
    return ret;
}

s32 scePadSetLightBar_hook(s32 handle, const MacroColor *color) {
    if (!g_self_call && color) {
        g_game_bar = *color; // remember what the game wants
        g_have_game_bar = true;
        if (g_state != ST_IDLE) return 0; // swallow while recording/playing
    }
    return HOOK_CONTINUE(scePadSetLightBar, s32 (*)(s32, const MacroColor *), handle, color);
}

/* ------------------------------------------------------------------ */
/* Plugin entry points                                                 */
/* ------------------------------------------------------------------ */

s32 attr_public plugin_load(s32 argc, const char* argv[]) {
    final_printf("[GoldHEN] <%s\\Ver.0x%08x> %s\n", g_pluginName, g_pluginVersion, __func__);
    final_printf("[GoldHEN] Plugin Author(s): %s\n", g_pluginAuth);
    boot_ver();

    load_macro();

    s32 h = 0;
    sys_dynlib_load_prx("libScePad.sprx", &h);
    sys_dynlib_dlsym(h, "scePadReadState", &scePadReadState);
    sys_dynlib_dlsym(h, "scePadRead", &scePadRead);
    sys_dynlib_dlsym(h, "scePadSetLightBar", &scePadSetLightBar);
    sys_dynlib_dlsym(h, "scePadResetLightBar", &scePadResetLightBar);

    final_printf("[macro] resolved: ReadState=%p Read=%p SetLightBar=%p\n",
                 (void *)scePadReadState, (void *)scePadRead, (void *)scePadSetLightBar);

    if (scePadReadState)   { HOOK32(scePadReadState); }
    if (scePadRead)        { HOOK32(scePadRead); }
    if (scePadSetLightBar) { HOOK32(scePadSetLightBar); }
    return 0;
}

s32 attr_public plugin_unload(s32 argc, const char* argv[]) {
    final_printf("[GoldHEN] <%s\\Ver.0x%08x> %s\n", g_pluginName, g_pluginVersion, __func__);
    g_state = ST_IDLE;
    if (scePadReadState)   { UNHOOK(scePadReadState); }
    if (scePadRead)        { UNHOOK(scePadRead); }
    if (scePadSetLightBar) { UNHOOK(scePadSetLightBar); }
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

