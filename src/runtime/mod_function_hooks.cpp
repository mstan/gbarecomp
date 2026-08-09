// mod_function_hooks.cpp -- always-on registry for trusted entry hooks.

#include "mod_function_hooks.h"

#include <atomic>
#include <cstring>

namespace {

constexpr int kMaxFunctionHooks = 32;

struct FunctionHook {
    const char* id = nullptr;
    uint32_t addr = 0;
    int thumb = 0;
    std::atomic<int> enabled{0};
    GBAModFunctionEntryCallback callback = nullptr;
};

FunctionHook g_hooks[kMaxFunctionHooks];
int g_count = 0;
std::atomic<int> g_active{0};
std::atomic<uint64_t> g_hits{0};

}  // namespace

extern "C" int gba_mod_register_function_entry_plugin(
    const char* id, uint32_t addr, int thumb, GBAModFunctionEntryCallback cb) {
    if (!id || !*id || !cb || (thumb != 0 && thumb != 1) ||
        (thumb ? (addr & 1u) != 0 : (addr & 3u) != 0)) return 0;
    for (int i = 0; i < g_count; ++i) {
        const FunctionHook& existing = g_hooks[i];
        if (std::strcmp(existing.id, id) == 0 && existing.addr == addr &&
            existing.thumb == thumb && existing.callback == cb)
            return 1;  // Idempotent registration of this exact hook.
    }
    if (g_count == kMaxFunctionHooks) return 0;
    FunctionHook& hook = g_hooks[g_count++];
    hook.id = id;
    hook.addr = addr;
    hook.thumb = thumb;
    hook.enabled.store(0, std::memory_order_relaxed);
    hook.callback = cb;
    return 1;
}

extern "C" int gba_mod_set_function_hook_enabled(const char* id, int enabled) {
    if (!id) return 0;
    int found = 0;
    for (int i = 0; i < g_count; ++i) {
        if (std::strcmp(g_hooks[i].id, id) != 0) continue;
        found = 1;
        const int want = enabled ? 1 : 0;
        if (g_hooks[i].enabled.exchange(want, std::memory_order_acq_rel) !=
            want) {
            if (want) g_active.fetch_add(1, std::memory_order_release);
            else g_active.fetch_sub(1, std::memory_order_release);
        }
    }
    return found;
}

extern "C" void gba_mod_disable_all_function_hooks(void) {
    for (int i = 0; i < g_count; ++i)
        g_hooks[i].enabled.store(0, std::memory_order_release);
    g_active.store(0, std::memory_order_release);
}

extern "C" int gba_mod_function_hook_enabled(const char* id) {
    if (!id) return 0;
    for (int i = 0; i < g_count; ++i)
        if (std::strcmp(g_hooks[i].id, id) == 0)
            if (g_hooks[i].enabled.load(std::memory_order_acquire)) return 1;
    return 0;
}

extern "C" int gba_mod_function_entry(uint32_t addr, int thumb,
                                        ArmCpuState* cpu) {
    if (g_active.load(std::memory_order_acquire) == 0 || !cpu) return 0;
    const uint32_t pc = addr & ~1u;
    const int mode = thumb ? 1 : 0;
    for (int i = 0; i < g_count; ++i) {
        const FunctionHook& hook = g_hooks[i];
        if (!hook.enabled.load(std::memory_order_acquire) ||
            hook.addr != pc || hook.thumb != mode) continue;
        // A decline is observational: speculative callback writes cannot leak
        // into a later hook or the original guest body.
        const ArmCpuState before = *cpu;
        if (hook.callback(pc, mode, cpu)) {
            g_hits.fetch_add(1, std::memory_order_relaxed);
            return 1;
        }
        *cpu = before;
    }
    return 0;
}

extern "C" uint64_t gba_mod_function_hook_hits(void) {
    return g_hits.load(std::memory_order_relaxed);
}
