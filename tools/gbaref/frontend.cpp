/* gbaref — minimal SDL2 libretro frontend: a known-good GBA emulator used as
 * the differential oracle for the gbarecomp recompiler. Loads a libretro GBA
 * core (e.g. mgba_libretro.dll), plays a ROM with recomp-matched keyboard
 * input, and logs per-frame WRAM changes (IWRAM 0x03000000-0x03007FFF and
 * EWRAM 0x02000000-0x0203FFFF) as JSONL — the SAME shape the gbarecomp runtime
 * emits with GBARECOMP_WRAM_TRACE and that oracle/ref_diff.py diffs.
 *
 *   gbaref.exe <core.dll> <rom.gba>
 *
 * This is the GBA sibling of snesref / mdref. Why a separate libretro tool and
 * not the in-tree interpreter: the gbarecomp interpreter (bios_smoke) shares
 * src/gba/* device models with the recompiled runtime, so it is NOT an
 * independent oracle for device/bus/timing bugs — both diverge from hardware
 * identically. A libretro core (mGBA) is the independent, hardware-faithful
 * reference. The human plays BOTH to the same scene (no input scripting, no
 * savestate alignment); the traces are diffed by value/order, not frame.
 *
 * Keys (match the gbarecomp host window): arrows=D-pad, Z=A, X=B, S=L, A=R,
 *   Enter=Start, RShift=Select.   F1..F9 = load state slot, Shift+Fn = save.
 *   Backspace = clear trace (fresh capture window)   Esc = quit.
 *
 * Env: GBAREF_TRACE (out path, default gbaref_trace.jsonl);
 *      GBAREF_WATCH_LO / GBAREF_WATCH_HI (restrict traced GBA addr range);
 *      GBAREF_QUIT_FRAMES (headless cap).
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#define SDL_MAIN_HANDLED
#include <SDL.h>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <vector>
#include "libretro.h"

// ---- core function pointers ----
static HMODULE g_core;
#define LR(sym) static decltype(&sym) p_##sym;
LR(retro_init) LR(retro_deinit) LR(retro_api_version)
LR(retro_get_system_info) LR(retro_get_system_av_info)
LR(retro_set_environment) LR(retro_set_video_refresh)
LR(retro_set_audio_sample) LR(retro_set_audio_sample_batch)
LR(retro_set_input_poll) LR(retro_set_input_state)
LR(retro_set_controller_port_device)
LR(retro_load_game) LR(retro_unload_game) LR(retro_run)
LR(retro_serialize_size) LR(retro_serialize) LR(retro_unserialize)
LR(retro_get_memory_data) LR(retro_get_memory_size)
#undef LR

template<class T> static void bind(T& fn, const char* name) {
    fn = (T)GetProcAddress(g_core, name);
    if (!fn) { fprintf(stderr, "missing core symbol: %s\n", name); exit(2); }
}

// ---- video ----
static SDL_Window*   g_win;
static SDL_Renderer* g_ren;
static SDL_Texture*  g_tex;
static int g_tex_w = 0, g_tex_h = 0;
static retro_pixel_format g_fmt = RETRO_PIXEL_FORMAT_0RGB1555;
static SDL_GameController* g_pad = nullptr;
static std::vector<uint8_t> g_last_rgb;
static unsigned g_last_w = 0, g_last_h = 0;
static bool g_demo_campaign = false;
static uint32_t g_input_preroll = 1;
static uint32_t g_quit_frame_cap = 0;

static void open_first_pad() {
    if (g_pad) return;
    for (int i = 0; i < SDL_NumJoysticks(); i++)
        if (SDL_IsGameController(i)) {
            g_pad = SDL_GameControllerOpen(i);
            if (g_pad) { printf("[controller: %s]\n", SDL_GameControllerName(g_pad)); fflush(stdout); return; }
        }
}

// ---- WRAM trace ----
// Each traced GBA RAM region: a host pointer (from the core's memory map),
// the GBA-absolute base address, length, and a shadow of the previous frame so
// only changed bytes are emitted. mGBA reports IWRAM (0x03000000) and EWRAM
// (0x02000000) via RETRO_ENVIRONMENT_SET_MEMORY_MAPS; we keep the ones that
// overlap the GBA work-RAM ranges and clamp to [GBAREF_WATCH_LO, _HI] if set.
struct Region {
    const uint8_t* ptr = nullptr;
    uint32_t gba_base = 0;
    uint32_t len = 0;
    std::vector<uint8_t> prev;
};
static std::vector<Region> g_regions;
static uint32_t g_watch_lo = 0x02000000u, g_watch_hi = 0x03FFFFFFu;
static FILE* g_log;
static const char* g_log_path = "gbaref_trace.jsonl";
static bool g_primed = false;
static uint32_t g_frame = 0;
static bool g_trace_enabled = true;

static void add_region_if_ram(const void* ptr, uint32_t base, uint32_t len) {
    if (!ptr || !len) return;
    // Every WRITABLE GBA region: EWRAM(0x02) IWRAM(0x03) IO(0x04) PAL(0x05)
    // VRAM(0x06) OAM(0x07). Skip const BIOS(0x00) and ROM(0x08+) — they can't
    // diverge. Matches the recomp's GBARECOMP_WRAM_TRACE region set for parity.
    if (base < 0x02000000u || base >= 0x08000000u) return;
    Region r;
    r.ptr = (const uint8_t*)ptr;
    r.gba_base = base;
    r.len = len;
    r.prev.assign(len, 0);
    g_regions.push_back(std::move(r));
    printf("[trace region: 0x%08X..0x%08X (%u KB)]\n", base, base + len, len / 1024);
    fflush(stdout);
}

static void emit(uint32_t addr, uint8_t o, uint8_t n) {
    if (!g_log) { g_log = fopen(g_log_path, "a"); if (!g_log) return; }
    fprintf(g_log, "{\"f\":%u,\"adr\":\"0x%08x\",\"old\":\"0x%02x\",\"val\":\"0x%02x\"}\n",
            g_frame, addr, o, n);
}

static void trace_tick() {
    if (g_regions.empty()) return;
    for (auto& r : g_regions) {
        if (!g_primed) { std::memcpy(r.prev.data(), r.ptr, r.len); continue; }
        for (uint32_t i = 0; i < r.len; i++) {
            uint8_t v = r.ptr[i];
            if (v != r.prev[i]) {
                uint32_t a = r.gba_base + i;
                if (a >= g_watch_lo && a <= g_watch_hi) emit(a, r.prev[i], v);
                r.prev[i] = v;
            }
        }
    }
    if (!g_primed) { g_primed = true; return; }
    if (g_log && (g_frame % 30) == 0) fflush(g_log);
}

static void clear_trace() {
    if (g_log) { fclose(g_log); g_log = nullptr; }
    FILE* f = fopen(g_log_path, "w"); if (f) fclose(f);
    g_primed = false;
    printf("[trace cleared: %s]\n", g_log_path); fflush(stdout);
}

// ---- point-in-time RAM dump (D key) ----
// gbaref has no on-demand memory read; this dumps the live IWRAM (0x03000000)
// and EWRAM (0x02000000) to frame-numbered files so a recomp-vs-oracle field
// comparison (e.g. find an entity by a known ROM ptr at obj+0x48, read its
// animId/frame-ptr) can be done offline. Press D a few times across a scene
// transition to capture the transient object setup.
static void dump_ram() {
    for (auto& r : g_regions) {
        const char* name = r.gba_base == 0x03000000u ? "iwram"
                         : r.gba_base == 0x02000000u ? "ewram" : nullptr;
        if (!name) continue;
        char path[96];
        snprintf(path, sizeof path, "gbaref_%s_f%u.bin", name, g_frame);
        FILE* f = fopen(path, "wb");
        if (f) {
            fwrite(r.ptr, 1, r.len, f);
            fclose(f);
            printf("[dump %s @f%u -> %s (%u bytes)]\n", name, g_frame, path, r.len);
            fflush(stdout);
        }
    }
}

// ---- libretro callbacks ----
static bool cb_environment(unsigned cmd, void* data) {
    switch (cmd) {
        case RETRO_ENVIRONMENT_GET_CAN_DUPE: *(bool*)data = true; return true;
        case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT: g_fmt = *(const retro_pixel_format*)data; return true;
        case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY: *(const char**)data = "."; return true;
        case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY:   *(const char**)data = "."; return true;
        case RETRO_ENVIRONMENT_SET_PERFORMANCE_LEVEL: return true;
        case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE: if (data) *(bool*)data = false; return true;
        case RETRO_ENVIRONMENT_SET_MEMORY_MAPS: {
            const retro_memory_map* mm = (const retro_memory_map*)data;
            for (unsigned i = 0; i < mm->num_descriptors; i++) {
                const retro_memory_descriptor& d = mm->descriptors[i];
                printf("[memmap] start=0x%08X len=0x%-8X flags=0x%llx ptr=%p\n",
                       (uint32_t)d.start, (uint32_t)d.len,
                       (unsigned long long)d.flags, d.ptr);
                add_region_if_ram(d.ptr, (uint32_t)d.start, (uint32_t)d.len);
            }
            return true;
        }
        default: return false;
    }
}
static void ensure_texture(unsigned w, unsigned h) {
    if ((int)w == g_tex_w && (int)h == g_tex_h && g_tex) return;
    if (g_tex) SDL_DestroyTexture(g_tex);
    Uint32 sf = (g_fmt == RETRO_PIXEL_FORMAT_XRGB8888) ? SDL_PIXELFORMAT_ARGB8888
              : (g_fmt == RETRO_PIXEL_FORMAT_RGB565)   ? SDL_PIXELFORMAT_RGB565
              :                                          SDL_PIXELFORMAT_ARGB1555;
    g_tex = SDL_CreateTexture(g_ren, sf, SDL_TEXTUREACCESS_STREAMING, w, h);
    g_tex_w = w; g_tex_h = h;
}
static void cb_video(const void* data, unsigned w, unsigned h, size_t pitch) {
    // A deterministic headless oracle only needs the terminal framebuffer.
    // Avoid converting/uploading 15,000 throwaway intermediate frames.
    if (g_quit_frame_cap && g_frame + 1 < g_quit_frame_cap) return;
    if (data && w && h) {
        ensure_texture(w, h);
        if (g_tex) SDL_UpdateTexture(g_tex, nullptr, data, (int)pitch);
        g_last_w = w; g_last_h = h;
        g_last_rgb.assign((size_t)w * h * 3, 0);
        auto expand5 = [](unsigned v) -> uint8_t { return (uint8_t)((v << 3) | (v >> 2)); };
        for (unsigned y = 0; y < h; ++y) {
            const uint8_t* row = (const uint8_t*)data + y * pitch;
            for (unsigned x = 0; x < w; ++x) {
                uint8_t r, g, b;
                if (g_fmt == RETRO_PIXEL_FORMAT_XRGB8888) {
                    uint32_t c; std::memcpy(&c, row + x * 4, sizeof c);
                    r = (uint8_t)(c >> 16); g = (uint8_t)(c >> 8); b = (uint8_t)c;
                } else {
                    uint16_t c; std::memcpy(&c, row + x * 2, sizeof c);
                    if (g_fmt == RETRO_PIXEL_FORMAT_RGB565) {
                        // Normalize the core's host RGB565 back to logical GBA
                        // RGB555. mGBA stores the 5-bit GBA green as an even
                        // 6-bit host channel; expanding all six bits would add
                        // a frontend-format artifact to oracle comparisons.
                        r = expand5((c >> 11) & 31); g = expand5((c >> 6) & 31); b = expand5(c & 31);
                    } else {
                        r = expand5((c >> 10) & 31); g = expand5((c >> 5) & 31); b = expand5(c & 31);
                    }
                }
                size_t p = ((size_t)y * w + x) * 3;
                g_last_rgb[p] = r; g_last_rgb[p + 1] = g; g_last_rgb[p + 2] = b;
            }
        }
    }
    if (g_ren) {
        SDL_RenderClear(g_ren);
        if (g_tex) SDL_RenderCopy(g_ren, g_tex, nullptr, nullptr);
        SDL_RenderPresent(g_ren);
    }
}
static void cb_audio_sample(int16_t, int16_t) {}
static size_t cb_audio_batch(const int16_t*, size_t frames) { return frames; }
static void cb_input_poll(void) {}

// GBA button (libretro joypad id) <- keyboard key, matching gbarecomp host_window:
//   A=Z, B=X, Select=RShift, Start=Enter, dpad=arrows, L=S, R=A.
static int16_t cb_input_state(unsigned port, unsigned device, unsigned, unsigned id) {
    if (port != 0 || device != RETRO_DEVICE_JOYPAD) return 0;
    if (g_demo_campaign) {
        if (g_frame < g_input_preroll) return 0;
        const uint32_t frame = g_frame - g_input_preroll;
        uint16_t pressed = 0;
        if (frame >= 9000) {
            static const uint16_t dirs[] = { 0x10, 0x80, 0x20, 0x40 };
            const uint32_t walk = frame - 9000;
            pressed = dirs[(walk / 180) % 4];
            if ((walk % 60) < 2) pressed |= 0x01;
        } else {
            static const uint16_t buttons[] = {
                0x08, 0x01, 0x01, 0x80, 0x01, 0x10,
                0x02, 0x01, 0x20, 0x01, 0x40, 0x02,
            };
            if (((frame / 6) & 1u) == 0)
                pressed = buttons[(frame / 12) % (sizeof buttons / sizeof buttons[0])];
        }
        uint16_t mask = 0;
        switch (id) {
            case RETRO_DEVICE_ID_JOYPAD_A:     mask = 0x01; break;
            case RETRO_DEVICE_ID_JOYPAD_B:     mask = 0x02; break;
            case RETRO_DEVICE_ID_JOYPAD_START: mask = 0x08; break;
            case RETRO_DEVICE_ID_JOYPAD_RIGHT: mask = 0x10; break;
            case RETRO_DEVICE_ID_JOYPAD_LEFT:  mask = 0x20; break;
            case RETRO_DEVICE_ID_JOYPAD_UP:    mask = 0x40; break;
            case RETRO_DEVICE_ID_JOYPAD_DOWN:  mask = 0x80; break;
            default: return 0;
        }
        return (pressed & mask) != 0;
    }
    const Uint8* ks = SDL_GetKeyboardState(nullptr);
    SDL_Scancode sc; SDL_GameControllerButton gb;
    switch (id) {
        case RETRO_DEVICE_ID_JOYPAD_A:      sc=SDL_SCANCODE_Z;      gb=SDL_CONTROLLER_BUTTON_A; break;
        case RETRO_DEVICE_ID_JOYPAD_B:      sc=SDL_SCANCODE_X;      gb=SDL_CONTROLLER_BUTTON_B; break;
        case RETRO_DEVICE_ID_JOYPAD_SELECT: sc=SDL_SCANCODE_RSHIFT; gb=SDL_CONTROLLER_BUTTON_BACK; break;
        case RETRO_DEVICE_ID_JOYPAD_START:  sc=SDL_SCANCODE_RETURN; gb=SDL_CONTROLLER_BUTTON_START; break;
        case RETRO_DEVICE_ID_JOYPAD_L:      sc=SDL_SCANCODE_S;      gb=SDL_CONTROLLER_BUTTON_LEFTSHOULDER; break;
        case RETRO_DEVICE_ID_JOYPAD_R:      sc=SDL_SCANCODE_A;      gb=SDL_CONTROLLER_BUTTON_RIGHTSHOULDER; break;
        case RETRO_DEVICE_ID_JOYPAD_UP:     sc=SDL_SCANCODE_UP;     gb=SDL_CONTROLLER_BUTTON_DPAD_UP; break;
        case RETRO_DEVICE_ID_JOYPAD_DOWN:   sc=SDL_SCANCODE_DOWN;   gb=SDL_CONTROLLER_BUTTON_DPAD_DOWN; break;
        case RETRO_DEVICE_ID_JOYPAD_LEFT:   sc=SDL_SCANCODE_LEFT;   gb=SDL_CONTROLLER_BUTTON_DPAD_LEFT; break;
        case RETRO_DEVICE_ID_JOYPAD_RIGHT:  sc=SDL_SCANCODE_RIGHT;  gb=SDL_CONTROLLER_BUTTON_DPAD_RIGHT; break;
        default: return 0;
    }
    if (ks[sc]) return 1;
    if (g_pad && SDL_GameControllerGetButton(g_pad, gb)) return 1;
    if (g_pad) {
        const int DZ = 16000;
        if (id==RETRO_DEVICE_ID_JOYPAD_LEFT  && SDL_GameControllerGetAxis(g_pad,SDL_CONTROLLER_AXIS_LEFTX) < -DZ) return 1;
        if (id==RETRO_DEVICE_ID_JOYPAD_RIGHT && SDL_GameControllerGetAxis(g_pad,SDL_CONTROLLER_AXIS_LEFTX) >  DZ) return 1;
        if (id==RETRO_DEVICE_ID_JOYPAD_UP    && SDL_GameControllerGetAxis(g_pad,SDL_CONTROLLER_AXIS_LEFTY) < -DZ) return 1;
        if (id==RETRO_DEVICE_ID_JOYPAD_DOWN  && SDL_GameControllerGetAxis(g_pad,SDL_CONTROLLER_AXIS_LEFTY) >  DZ) return 1;
    }
    return 0;
}

// ---- save state: Shift+Fn = save slot n, Fn = load slot n ----
static void slot_path(int slot, char* out, size_t n) { snprintf(out, n, "gbaref_state_%d.bin", slot); }
static void save_state(int slot) {
    size_t n = p_retro_serialize_size(); if (!n) return;
    std::vector<uint8_t> buf(n);
    if (p_retro_serialize(buf.data(), n)) {
        char path[64]; slot_path(slot, path, sizeof path);
        FILE* f = fopen(path, "wb"); if (f) { fwrite(buf.data(),1,n,f); fclose(f); printf("[slot %d SAVED %zu]\n",slot,n); fflush(stdout); }
    }
}
static void load_state(int slot) {
    char path[64]; slot_path(slot, path, sizeof path);
    FILE* f = fopen(path,"rb"); if (!f) { printf("[slot %d empty]\n",slot); fflush(stdout); return; }
    fseek(f,0,SEEK_END); long fn=ftell(f); fseek(f,0,SEEK_SET);
    size_t need = p_retro_serialize_size();
    if (fn <= 0) { fclose(f); return; }
    size_t bn = ((size_t)fn > need) ? (size_t)fn : need;
    std::vector<uint8_t> buf(bn, 0);
    fread(buf.data(),1,(size_t)fn,f); fclose(f);
    bool ok = p_retro_unserialize(buf.data(), need);
    printf("[slot %d %s]\n", slot, ok ? "LOADED" : "unserialize FALSE"); fflush(stdout);
}

int main(int argc, char** argv) {
    if (argc < 3) { fprintf(stderr, "usage: gbaref <core.dll> <rom.gba>\n"); return 1; }
    const char* corePath = argv[1];
    const char* romPath  = argv[2];
    if (const char* p = getenv("GBAREF_TRACE")) if (p[0]) g_log_path = p;
    if (const char* p = getenv("GBAREF_WATCH_LO")) if (p[0]) g_watch_lo = (uint32_t)strtoul(p,nullptr,0);
    if (const char* p = getenv("GBAREF_WATCH_HI")) if (p[0]) g_watch_hi = (uint32_t)strtoul(p,nullptr,0);
    if (const char* p = getenv("GBAREF_DEMO_INPUT")) g_demo_campaign = std::strcmp(p, "campaign") == 0;
    if (const char* p = getenv("GBAREF_INPUT_PREROLL")) if (p[0]) g_input_preroll = (uint32_t)strtoul(p,nullptr,0);
    if (const char* p = getenv("GBAREF_NO_TRACE")) g_trace_enabled = !(p[0] && std::strcmp(p, "0") != 0);

    g_core = LoadLibraryA(corePath);
    if (!g_core) { fprintf(stderr,"LoadLibrary failed: %s (err %lu)\n", corePath, GetLastError()); return 2; }
    bind(p_retro_init,"retro_init"); bind(p_retro_deinit,"retro_deinit");
    bind(p_retro_api_version,"retro_api_version");
    bind(p_retro_get_system_info,"retro_get_system_info");
    bind(p_retro_get_system_av_info,"retro_get_system_av_info");
    bind(p_retro_set_environment,"retro_set_environment");
    bind(p_retro_set_video_refresh,"retro_set_video_refresh");
    bind(p_retro_set_audio_sample,"retro_set_audio_sample");
    bind(p_retro_set_audio_sample_batch,"retro_set_audio_sample_batch");
    bind(p_retro_set_input_poll,"retro_set_input_poll");
    bind(p_retro_set_input_state,"retro_set_input_state");
    bind(p_retro_set_controller_port_device,"retro_set_controller_port_device");
    bind(p_retro_load_game,"retro_load_game"); bind(p_retro_unload_game,"retro_unload_game");
    bind(p_retro_run,"retro_run");
    bind(p_retro_serialize_size,"retro_serialize_size");
    bind(p_retro_serialize,"retro_serialize"); bind(p_retro_unserialize,"retro_unserialize");
    bind(p_retro_get_memory_data,"retro_get_memory_data");
    bind(p_retro_get_memory_size,"retro_get_memory_size");

    p_retro_set_environment(cb_environment);
    p_retro_init();

    retro_system_info si; memset(&si,0,sizeof si); p_retro_get_system_info(&si);
    printf("core: %s %s  need_fullpath=%d\n", si.library_name?si.library_name:"?",
           si.library_version?si.library_version:"?", si.need_fullpath);

    retro_game_info gi; memset(&gi,0,sizeof gi); gi.path=romPath;
    std::vector<uint8_t> rom;
    if (!si.need_fullpath) {
        FILE* f=fopen(romPath,"rb"); if(!f){ fprintf(stderr,"cannot open rom %s\n",romPath); return 3; }
        fseek(f,0,SEEK_END); long n=ftell(f); fseek(f,0,SEEK_SET);
        rom.resize(n); fread(rom.data(),1,n,f); fclose(f);
        gi.data=rom.data(); gi.size=rom.size();
    }
    p_retro_set_video_refresh(cb_video);
    p_retro_set_audio_sample(cb_audio_sample);
    p_retro_set_audio_sample_batch(cb_audio_batch);
    p_retro_set_input_poll(cb_input_poll);
    p_retro_set_input_state(cb_input_state);
    if (!p_retro_load_game(&gi)) { fprintf(stderr,"retro_load_game failed\n"); return 4; }
    p_retro_set_controller_port_device(0, RETRO_DEVICE_JOYPAD);

    // Fallback if the core never sent a memory map: use SYSTEM_RAM with a base
    // the caller declares (GBAREF_SYSRAM_BASE, default IWRAM 0x03000000).
    if (g_regions.empty()) {
        void* ram = p_retro_get_memory_data(RETRO_MEMORY_SYSTEM_RAM);
        size_t sz = p_retro_get_memory_size(RETRO_MEMORY_SYSTEM_RAM);
        uint32_t base = 0x03000000u;
        if (const char* p = getenv("GBAREF_SYSRAM_BASE")) if (p[0]) base = (uint32_t)strtoul(p,nullptr,0);
        if (ram && sz) { add_region_if_ram(ram, base, (uint32_t)sz);
            if (g_regions.empty()) { // base didn't match a known RAM window; force it
                Region r; r.ptr=(const uint8_t*)ram; r.gba_base=base; r.len=(uint32_t)sz; r.prev.assign(sz,0);
                g_regions.push_back(std::move(r));
                printf("[trace SYSTEM_RAM @0x%08X (%zu KB) — forced]\n", base, sz/1024); fflush(stdout);
            } }
    }
    if (g_regions.empty()) fprintf(stderr,"[warn] no RAM regions to trace\n");

    retro_system_av_info av; memset(&av,0,sizeof av); p_retro_get_system_av_info(&av);
    int vw=(int)av.geometry.base_width, vh=(int)av.geometry.base_height;
    if(vw<=0)vw=240; if(vh<=0)vh=160;
    printf("core timing: fps=%.4f  trace=%s  watch=[0x%08X..0x%08X]\n",
           av.timing.fps, g_log_path, g_watch_lo, g_watch_hi);

    long quit_frames = 0;
    if (const char* qf = getenv("GBAREF_QUIT_FRAMES")) if (qf[0]) quit_frames = atol(qf);
    if (quit_frames > 0) g_quit_frame_cap = (uint32_t)quit_frames;

    SDL_SetMainReady();
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER | SDL_INIT_JOYSTICK) != 0) { fprintf(stderr,"SDL_Init: %s\n",SDL_GetError()); return 5; }
    open_first_pad();
    g_win = SDL_CreateWindow("gbaref (libretro GBA oracle) — Fn load / Shift+Fn save / Backspace clear-trace",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, vw*3, vh*3,
        SDL_WINDOW_RESIZABLE | (quit_frames > 0 ? SDL_WINDOW_HIDDEN : 0));
    g_ren = SDL_CreateRenderer(g_win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!g_ren) g_ren = SDL_CreateRenderer(g_win, -1, SDL_RENDERER_SOFTWARE);
    if (g_ren) SDL_RenderSetLogicalSize(g_ren, vw, vh);

    printf("RUN. KB: arrows=DPad Z=A X=B S=L A=R Enter=Start RShift=Select | "
           "F1-F12=LOAD slot, Shift+F1-F12=SAVE | D=dump IWRAM+EWRAM | "
           "Backspace=clear trace | Esc=quit\n");
    fflush(stdout);

    bool running=true;
    Uint64 freq=SDL_GetPerformanceFrequency(), prev=SDL_GetPerformanceCounter();
    const double target = (double)freq / (av.timing.fps>0 ? av.timing.fps : 59.7275);
    while (running) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type==SDL_QUIT) running=false;
            else if (e.type==SDL_CONTROLLERDEVICEADDED) open_first_pad();
            else if (e.type==SDL_CONTROLLERDEVICEREMOVED) { if(g_pad){ SDL_GameControllerClose(g_pad); g_pad=nullptr; } open_first_pad(); }
            else if (e.type==SDL_KEYDOWN && e.key.repeat==0) {
                SDL_Scancode s = e.key.keysym.scancode;
                if (s==SDL_SCANCODE_ESCAPE) running=false;
                else if (s==SDL_SCANCODE_BACKSPACE) clear_trace();
                else if (s==SDL_SCANCODE_D) dump_ram();   // dump IWRAM+EWRAM now
                else if (s>=SDL_SCANCODE_F1 && s<=SDL_SCANCODE_F12) {
                    int slot = (int)(s - SDL_SCANCODE_F1) + 1;  // F1..F12 -> 1..12
                    if (e.key.keysym.mod & KMOD_SHIFT) save_state(slot); else load_state(slot);
                }
            }
        }
        p_retro_run();
        g_frame++;
        if (g_trace_enabled) trace_tick();
        if (quit_frames > 0 && g_frame >= (uint32_t)quit_frames) running = false;
        for (; quit_frames <= 0;) {
            Uint64 now=SDL_GetPerformanceCounter();
            double el=(double)(now-prev);
            if (el>=target) { prev=now; break; }
            double rem_ms=(target-el)*1000.0/(double)freq;
            if (rem_ms>1.5) SDL_Delay((Uint32)(rem_ms-1.0));
        }
    }
    if (const char* path = getenv("GBAREF_DUMP_PPM")) {
        if (path[0] && !g_last_rgb.empty()) {
            FILE* f = fopen(path, "wb");
            if (f) {
                fprintf(f, "P6\n%u %u\n255\n", g_last_w, g_last_h);
                fwrite(g_last_rgb.data(), 1, g_last_rgb.size(), f);
                fclose(f);
                printf("[framebuffer f%u -> %s]\n", g_frame, path);
            }
        }
    }
    if (g_log) fflush(g_log);
    p_retro_unload_game(); p_retro_deinit();
    SDL_Quit(); FreeLibrary(g_core);
    return 0;
}
