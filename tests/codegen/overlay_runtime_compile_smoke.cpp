#include "overlay_runtime_arm.h"

const GbaOverlayCallbacks* g_ovl = nullptr;

extern "C" int overlay_runtime_trace_gate_compile_smoke(
    const GbaOverlayCallbacks* callbacks) {
    g_ovl = callbacks;
    return runtime_trace_enabled();
}
