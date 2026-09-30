#include "multiplayer_view.h"
#include "runtime.h"
#include <algorithm>
#include <stdexcept>
#include <tuple>

extern "C" unsigned g_ws_extra, g_ws_extra_left, g_ws_extra_right, g_ws_view_width;

namespace gbarecomp {
GbaNetplayView parse_gba_netplay_view(std::string_view name) {
    if (name == "native") return GbaNetplayView::Native;
    if (name == "16:9") return GbaNetplayView::Wide16x9;
    if (name == "21:9") return GbaNetplayView::Wide21x9;
    if (name == "32:9") return GbaNetplayView::Wide32x9;
    if (name == "adaptive") return GbaNetplayView::Adaptive;
    throw std::invalid_argument("netplay view must be native, 16:9, 21:9, 32:9 or adaptive");
}
namespace {
// The game's validated ceiling, bounded by what the engine can store.
std::uint32_t netplay_max_width(std::uint32_t game_max) {
    return std::clamp<std::uint32_t>(game_max, 240, gba::GbaPpu::kMaxRenderWidth);
}
}
void validate_gba_netplay_view(GbaNetplayView view, const GbaNetplayViewPolicy& policy) {
    if (view > GbaNetplayView::Adaptive ||
        (view != GbaNetplayView::Native && !policy.supported) ||
        (view == GbaNetplayView::Adaptive && !policy.adaptive_supported))
        throw std::invalid_argument("this game does not authorize that netplay view");
    if (policy.max_width < 240 || policy.max_width > gba::GbaPpu::kMaxRenderWidth)
        throw std::invalid_argument("netplay view policy maximum width is out of range");
    if (view != GbaNetplayView::Adaptive && gba_netplay_view_width(view) > policy.max_width)
        throw std::invalid_argument("this game does not authorize a netplay view that wide");
}
unsigned gba_netplay_view_width(GbaNetplayView view, int w, int h, std::uint32_t max_width) {
    switch (view) {
    case GbaNetplayView::Native: return 240;
    case GbaNetplayView::Wide16x9: return 284;
    case GbaNetplayView::Wide21x9: return 373;
    case GbaNetplayView::Wide32x9: return 569;
    case GbaNetplayView::Adaptive: {
        const std::uint32_t maximum = netplay_max_width(max_width);
        if (w <= 0 || h <= 0) return std::min<std::uint32_t>(284, maximum);
        return static_cast<unsigned>(std::clamp<std::int64_t>(
            (std::int64_t(w) * 160 + h / 2) / h, 240, maximum));
    }
    }
    throw std::invalid_argument("invalid netplay view");
}

namespace {
// Legacy game render providers use globals. Scope them to the host mirror so
// they cannot affect the canonical renderer or the other local machine.
#define VIEW_GLOBALS(X) \
    X(gba::g_ws_tilemap_provider) X(gba::g_ws_bg_x_provider) \
    X(gba::g_ws_bg_x_provider_layers) X(gba::g_ws_bg_xy_provider) \
    X(gba::g_ws_bg_xy_provider_layers) X(gba::g_ws_affine_filter_enabled) \
    X(gba::g_ws_affine_filter_provider) X(gba::g_ws_authored_margin_layers) \
    X(gba::g_ws_pillarbox) X(gba::g_ws_pillarbox_left) X(gba::g_ws_pillarbox_right) \
    X(gba::g_ws_obj_x_provider) X(gba::g_ws_obj_attr_x_provider) \
    X(gba::g_ws_obj_native_clip) X(gba::g_ws_obj_margin_provider) \
    X(g_ws_extra) X(g_ws_extra_left) X(g_ws_extra_right) X(g_ws_view_width)
// std::tuple avoids duplicating the provider signatures or aliases.
auto capture_globals() {
#define VALUE(name) name,
    return std::tuple{VIEW_GLOBALS(VALUE) 0};
#undef VALUE
}
using Globals = decltype(capture_globals());
void apply_globals(const Globals& state) {
#define REF(name) name,
    int unused = 0;
    std::tie(VIEW_GLOBALS(REF) unused) = state;
#undef REF
}
#undef VIEW_GLOBALS
struct GlobalScope {
    Globals previous = capture_globals();
    Globals& owned;
    explicit GlobalScope(Globals& state) : owned(state) { apply_globals(state); }
    ~GlobalScope() { owned = capture_globals(); apply_globals(previous); }
};
}
struct GbaNetplayPresentation::Impl {
    gba::GbaPpu mirror;
    GbaNetplayViewPolicy policy;
    Globals globals{};
    unsigned requested = 240, latched_width = 240, next_line = 0;
    std::uint64_t epoch = 1;
    bool initialized = false, reset_pending = false, complete = false;
    explicit Impl(GbaNetplayViewPolicy p, bool filter) : policy(p) {
        GlobalScope scope(globals);
        gba::g_ws_bg_x_provider_layers = gba::g_ws_bg_xy_provider_layers = 0xF;
        gba::g_ws_affine_filter_enabled = filter;
    }
};
GbaNetplayPresentation::GbaNetplayPresentation(GbaNetplayViewPolicy p, bool filter)
    : impl_(std::make_unique<Impl>(p, filter)) {}
GbaNetplayPresentation::~GbaNetplayPresentation() {
    GlobalScope scope(impl_->globals);
    if (impl_->initialized && impl_->policy.reset) impl_->policy.reset();
}
void GbaNetplayPresentation::request_width(unsigned width) {
    if (width < 240 || width > netplay_max_width(impl_->policy.max_width) ||
        (width > 240 && !impl_->policy.supported))
        throw std::invalid_argument("unsupported netplay presentation width");
    impl_->requested = width; // applied at scanline zero, never mid-frame
}
const std::uint8_t* GbaNetplayPresentation::pixels() const {
    return impl_->complete ? impl_->mirror.latched_framebuffer() : nullptr;
}
unsigned GbaNetplayPresentation::width() const { return impl_->latched_width; }
void GbaNetplayPresentation::restored() noexcept {
    ++impl_->epoch;
    impl_->reset_pending = true;
    impl_->complete = false;
    impl_->next_line = 0;
}
void GbaNetplayPresentation::scanline(const gba::GbaPpu& source, std::uint32_t y,
    std::uint16_t dispcnt, const std::uint8_t* io, const std::uint8_t* vram,
    const std::uint8_t* oam, const std::uint8_t* pal) {
    auto& s = *impl_;
    // A restored snapshot may start mid-frame. Wait for a complete raster.
    if (y != 0 && y != s.next_line) return;
    GlobalScope scope(s.globals);
    if (y == 0) {
        s.next_line = 0;
        if (s.reset_pending) {
            if (s.initialized && s.policy.reset) s.policy.reset();
            s.initialized = false;
            s.reset_pending = false;
        }
        const unsigned extra = s.requested - 240;
        s.mirror.set_view_margins(extra / 2, extra - extra / 2, 0, 0);
        g_ws_extra = extra / 2;
        g_ws_extra_left = extra / 2; g_ws_extra_right = extra - extra / 2;
        g_ws_view_width = s.requested;
        if (extra && !s.initialized) {
            if (s.policy.init) s.policy.init(g_ws_extra_left, g_ws_extra_right);
            s.initialized = true;
        }
        if (s.initialized && s.policy.frame) {
            ExtendedViewFrameInfo frame;
            frame.frame_count = source.frame_count(); frame.state_epoch = s.epoch;
            frame.view_width = s.requested;
            frame.extra_left = g_ws_extra_left; frame.extra_right = g_ws_extra_right;
            frame.io = io; frame.io_size = 0x400;
            s.policy.frame(&frame);
        }
    }
    s.mirror.render_presentation_scanline(source, y, dispcnt, io, vram, oam, pal);
    s.next_line = y + 1;
}
void GbaNetplayPresentation::frame_ready() {
    auto& s = *impl_;
    if (s.next_line != 160) return;
    s.mirror.mark_framebuffer_latched();
    s.latched_width = s.mirror.render_width();
    s.complete = true;
    s.next_line = 0;
}
}
