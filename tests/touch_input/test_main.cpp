// touch_input_tests — gesture recognition, touch hub drain/scripts/rings,
// key synthesis edge rules, KEYINPUT composition, presentation inverse and
// the mini JSON reader.

#include "gba_io.h"
#include "input_synth.h"
#include "mini_json.h"
#include "presentation_layout.h"
#include "touch_input.h"
#include "view_config.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

using namespace gbarecomp;

namespace {

int g_failures = 0;

void check(bool ok, const char* what) {
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++g_failures;
    }
}

bool near(float a, float b, float eps = 0.01f) { return std::fabs(a - b) <= eps; }

TouchEvent ev(TouchPhase phase, int id, float x, float y, std::uint32_t t) {
    TouchEvent e;
    e.phase = phase;
    e.pointer = id;
    e.drawable_x = x;
    e.drawable_y = y;
    e.view_x = x / 4.0f;
    e.view_y = y / 4.0f;
    e.in_view = true;
    e.t_ms = t;
    return e;
}

GestureRecognizer make_recognizer() {
    GestureRecognizer r;
    GestureConfig c;
    c.drawable_px_per_mm = 10.0f;  // slop 2.5 mm = 25 px
    c.view_px_per_drawable_px = 0.25f;
    c.long_press_ms = 450;
    r.configure(c);
    return r;
}

std::vector<GestureKind> kinds(const std::vector<Gesture>& g) {
    std::vector<GestureKind> k;
    for (const auto& x : g) k.push_back(x.kind);
    return k;
}

void test_presentation_inverse() {
    struct Case { int dw, dh, lw, lh; };
    const Case cases[] = {
        {2400, 1080, 533, 240}, {1080, 2400, 240, 533}, {2560, 1600, 400, 250},
        {1000, 700, 240, 160}, {960, 540, 284, 160}, {3088, 1440, 569, 160},
    };
    for (const Case& c : cases) {
        const auto layout = compute_adaptive_presentation_layout(c.dw, c.dh, c.lw, c.lh);
        for (float lx : {0.5f, 17.25f, c.lw * 0.5f, c.lw - 0.5f}) {
            for (float ly : {0.5f, 9.75f, c.lh - 0.5f}) {
                float dx, dy, bx, by;
                logical_point_to_presentation(layout, c.lw, c.lh, lx, ly, &dx, &dy);
                const bool inside =
                    presentation_point_to_logical(layout, c.lw, c.lh, dx, dy, &bx, &by);
                check(inside, "round-trip point inside");
                check(near(lx, bx, 0.02f) && near(ly, by, 0.02f), "round-trip exact");
            }
        }
        float ox, oy;
        check(!presentation_point_to_logical(layout, c.lw, c.lh, -5.0f, -5.0f, &ox, &oy),
              "outside point reported outside");
    }
}

void test_tap_and_slop() {
    GestureRecognizer r = make_recognizer();
    std::vector<Gesture> out;
    r.on_event(ev(TouchPhase::Down, 1, 100, 100, 0), out);
    r.on_event(ev(TouchPhase::Move, 1, 110, 105, 40), out);  // inside slop
    r.on_event(ev(TouchPhase::Up, 1, 110, 105, 120), out);
    check(out.size() == 1 && out[0].kind == GestureKind::Tap, "tap within slop");
    check(near(out[0].view_x, 27.5f), "tap reports view coordinates");
}

void test_long_press() {
    GestureRecognizer r = make_recognizer();
    std::vector<Gesture> out;
    r.on_event(ev(TouchPhase::Down, 1, 100, 100, 1000), out);
    r.advance(1400, out);
    check(out.empty(), "no long press before threshold");
    r.advance(1450, out);
    check(out.size() == 1 && out[0].kind == GestureKind::LongPress, "long press fires");
    r.on_event(ev(TouchPhase::Up, 1, 100, 100, 1600), out);
    check(out.size() == 1, "no tap after long press");
}

void test_drag_and_swipe() {
    GestureRecognizer r = make_recognizer();
    std::vector<Gesture> out;
    r.on_event(ev(TouchPhase::Down, 1, 100, 100, 0), out);
    r.on_event(ev(TouchPhase::Move, 1, 140, 100, 20), out);  // leaves slop
    r.on_event(ev(TouchPhase::Move, 1, 200, 102, 40), out);
    r.on_event(ev(TouchPhase::Move, 1, 300, 104, 60), out);
    r.on_event(ev(TouchPhase::Up, 1, 400, 104, 80), out);
    const auto k = kinds(out);
    check(k.size() == 5, "drag produces begin, 2 moves, end, swipe");
    if (k.size() == 5) {
        check(k[0] == GestureKind::DragBegin, "drag begin first");
        check(k[1] == GestureKind::DragMove && k[2] == GestureKind::DragMove, "drag moves");
        check(k[3] == GestureKind::DragEnd, "drag end");
        check(k[4] == GestureKind::Swipe && out[4].swipe == SwipeDirection::Right,
              "fast fling is a right swipe");
        check(near(out[0].start_view_x, 25.0f), "drag begin keeps origin");
    }
    // Slow drag: no swipe.
    out.clear();
    r.on_event(ev(TouchPhase::Down, 2, 100, 100, 1000), out);
    std::uint32_t t = 1000;
    for (int x = 110; x <= 300; x += 10) {
        t += 100;
        r.on_event(ev(TouchPhase::Move, 2, static_cast<float>(x), 100, t), out);
    }
    r.on_event(ev(TouchPhase::Up, 2, 300, 100, t + 100), out);
    check(!out.empty() && out.back().kind == GestureKind::DragEnd, "slow drag ends without swipe");
}

void test_multi_finger() {
    GestureRecognizer r = make_recognizer();
    std::vector<Gesture> out;
    r.on_event(ev(TouchPhase::Down, 1, 100, 100, 0), out);
    r.on_event(ev(TouchPhase::Down, 2, 300, 100, 60), out);
    r.on_event(ev(TouchPhase::Up, 1, 100, 100, 150), out);
    r.on_event(ev(TouchPhase::Up, 2, 300, 100, 170), out);
    check(out.size() == 1 && out[0].kind == GestureKind::TwoFingerTap &&
              out[0].fingers == 2,
          "two-finger tap");

    out.clear();
    r.on_event(ev(TouchPhase::Down, 1, 100, 100, 1000), out);
    r.on_event(ev(TouchPhase::Down, 2, 200, 100, 1010), out);
    r.on_event(ev(TouchPhase::Down, 3, 300, 100, 1020), out);
    r.on_event(ev(TouchPhase::Up, 1, 100, 100, 1100), out);
    r.on_event(ev(TouchPhase::Up, 2, 200, 100, 1110), out);
    r.on_event(ev(TouchPhase::Up, 3, 300, 100, 1120), out);
    check(out.size() == 1 && out[0].kind == GestureKind::ThreeFingerTap, "three-finger tap");

    // A second finger arriving mid-drag cancels the drag, no tap afterwards.
    out.clear();
    r.on_event(ev(TouchPhase::Down, 1, 100, 100, 2000), out);
    r.on_event(ev(TouchPhase::Move, 1, 200, 100, 2050), out);
    r.on_event(ev(TouchPhase::Down, 2, 400, 100, 2400), out);
    r.on_event(ev(TouchPhase::Up, 2, 400, 100, 2450), out);
    r.on_event(ev(TouchPhase::Up, 1, 200, 100, 2500), out);
    const auto k = kinds(out);
    check(k.size() == 2 && k[0] == GestureKind::DragBegin && k[1] == GestureKind::Cancel,
          "late second finger cancels drag");
    check(r.active_count() == 0, "all pointers released");
}

void test_key_synth() {
    KeySynth s;
    s.tap(kGbaKeyA);
    check(s.frame() == (0x03FFu & ~kGbaKeyA), "tap presses on first frame");
    check(s.frame() == 0x03FFu, "tap releases on next frame");
    check(s.idle(), "idle after tap");

    // Tapping a key that is currently held forces a released frame first.
    s.hold(kGbaKeyB);
    check(s.frame() == (0x03FFu & ~kGbaKeyB), "hold B");
    s.tap(kGbaKeyB);
    check(s.frame() == 0x03FFu, "pre-release before re-press");
    check(s.frame() == (0x03FFu & ~kGbaKeyB), "re-press edge (held again)");
    s.release(kGbaKeyB);
    check(s.frame() == 0x03FFu, "released");

    // Destructive / impossible combinations never escape.
    s.set_held(kGbaKeyA | kGbaKeyB | kGbaKeyStart | kGbaKeySelect);
    const std::uint16_t reset = s.frame();
    check((reset & kGbaKeySelect) != 0, "soft-reset combo loses Select");
    s.set_held(kGbaKeyLeft | kGbaKeyRight | kGbaKeyUp);
    const std::uint16_t dirs = s.frame();
    check((dirs & kGbaKeyLeft) && (dirs & kGbaKeyRight) && !(dirs & kGbaKeyUp),
          "opposing directions removed, others kept");
    s.clear();

    // wait() produces idle frames without pressing anything.
    s.wait(2);
    s.tap(kGbaKeyStart, 2, 1);
    check(s.frame() == 0x03FFu && s.frame() == 0x03FFu, "wait frames");
    check(s.frame() == (0x03FFu & ~kGbaKeyStart) &&
              s.frame() == (0x03FFu & ~kGbaKeyStart),
          "two-frame press");
    check(s.frame() == 0x03FFu && s.idle(), "release after press");
}

void test_keyinput_composition() {
    gba::GbaIo io;
    io.set_keyinput(0x03FEu);             // host: A
    io.set_synthesized_keyinput(0x03FDu); // policy: B
    check(io.composed_keyinput() == 0x03FCu, "host AND synth");
    check(io.read16(0x130) == 0x03FCu, "register reads composed value");
    io.set_synthesized_keyinput(0x03FFu);
    check(io.read16(0x130) == 0x03FEu, "clearing synth restores host keys");
}

void test_hub_script_and_rings() {
    TouchHub& hub = TouchHub::instance();
    hub.reset_for_tests();
    TouchPresentation p;
    p.drawable_width = 960;
    p.drawable_height = 640;
    p.layout = {0, 0, 960, 640, 4};
    p.view_width = 240;
    p.view_height = 160;
    p.drawable_px_per_mm = 16.0f;
    hub.set_presentation(p);
    hub.set_claims(kTouchClaimLongPress | kTouchClaimTap);

    std::string reply;
    check(touch_tcp_command("{\"cmd\":\"touch_tap\",\"x\":120,\"y\":80,\"hold_frames\":3}", reply),
          "touch_tap handled");
    check(reply.find("\"queued_steps\":2") != std::string::npos, "tap queues two steps");
    std::vector<Gesture> all;
    for (std::uint64_t f = 100; f < 108; ++f) {
        auto batch = hub.drain(f, 5000);
        for (auto& g : batch.gestures) all.push_back(g);
        if (f == 100) {
            check(batch.events.size() == 1 && batch.events[0].phase == TouchPhase::Down,
                  "down released on first frame");
            check(near(batch.events[0].drawable_x, 480.0f), "view->drawable mapping");
        }
    }
    check(all.size() == 1 && all[0].kind == GestureKind::Tap && all[0].guest_frame == 103,
          "scripted tap recognized on its release frame");
    check(!hub.script_active(), "script finished");

    // Long press through a script: deterministic frame clock (16.74 ms/frame).
    touch_tcp_command("{\"cmd\":\"touch_long_press\",\"x\":10,\"y\":10,\"hold_frames\":40}", reply);
    std::vector<Gesture> lp;
    for (std::uint64_t f = 200; f < 245; ++f) {
        auto batch = hub.drain(f, 9000);
        for (auto& g : batch.gestures) lp.push_back(g);
    }
    check(lp.size() == 1 && lp[0].kind == GestureKind::LongPress &&
              lp[0].guest_frame == 227,
          "scripted long press fires at 27 frames (>=450 ms)");
    check(hub.take_host_actions().empty(), "claimed long press is not a host action");

    // Unclaimed Back becomes a host action.
    hub.set_claims(0);
    touch_tcp_command("{\"cmd\":\"touch_back\"}", reply);
    check(hub.take_host_actions().size() == 1, "unclaimed back -> host action");
    {
        auto batch = hub.drain(250, 11000);
        check(batch.gestures.size() == 1 && batch.gestures[0].kind == GestureKind::Back,
              "back is also delivered to the game policy");
    }

    // Drag script.
    touch_tcp_command(
        "{\"cmd\":\"touch_drag\",\"points\":[[20,20],[60,20],[100,20]],\"frames_per_point\":2}",
        reply);
    std::vector<Gesture> drag;
    for (std::uint64_t f = 300; f < 310; ++f) {
        auto batch = hub.drain(f, 12000);
        for (auto& g : batch.gestures) drag.push_back(g);
    }
    if (drag.size() < 3 || drag.front().kind != GestureKind::DragBegin) {
        for (const auto& g : drag)
            std::fprintf(stderr, "  drag gesture %s frame=%llu\n", gesture_kind_name(g.kind),
                         static_cast<unsigned long long>(g.guest_frame));
    }
    check(drag.size() >= 3 && drag.front().kind == GestureKind::DragBegin,
          "scripted drag begins");
    bool ended = false;
    for (auto& g : drag) ended |= g.kind == GestureKind::DragEnd;
    check(ended, "scripted drag ends");

    // Rings are queryable and bounded.
    const std::string events = hub.events_json(0, 4);
    check(events.find("\"events\":[{") != std::string::npos, "events ring JSON");
    const std::string gestures = hub.gestures_json(0, 0);
    check(gestures.find("\"kind\":\"tap\"") != std::string::npos, "gesture ring has tap");
    KeySynthSample s;
    s.frame = 42;
    s.host = 0x3FF;
    s.synth = 0x3FE;
    s.composed = 0x3FE;
    hub.record_key_synth(s);
    check(hub.key_synth_json(40, 0).find("\"frame\":42") != std::string::npos,
          "key synth ring JSON");
    check(touch_tcp_command("{\"cmd\":\"touch_status\"}", reply) &&
              reply.find("\"view\":[240,160]") != std::string::npos,
          "status reports presentation");
    check(!touch_tcp_command("{\"cmd\":\"ping\"}", reply), "non-touch command ignored");
    hub.reset_for_tests();
}

void test_density_geometry() {
    auto geo = [](int dw, int dh, float ppmm, float zoom) {
        return density_driven_view_geometry(dw, dh, ppmm, 0.30f, zoom, 569, 854, 576, 864);
    };
    auto aspect_ok = [](const ViewGeometry& g, int dw, int dh) {
        const double view = static_cast<double>(g.width) / g.height;
        const double screen = static_cast<double>(dw) / dh;
        return std::fabs(view / screen - 1.0) < 0.02;
    };
    // Galaxy S22 Ultra (~500 dpi): portrait keeps ~native width, much taller.
    const auto phone_p = geo(1440, 3088, 19.7f, 1.0f);
    check(phone_p.width >= 240 && phone_p.width <= 250 && phone_p.height >= 500 &&
              phone_p.height <= 540 && aspect_ok(phone_p, 1440, 3088),
          "phone portrait ~244x523");
    check(phone_p.extra_left + phone_p.extra_right + 240 == phone_p.width &&
              phone_p.extra_top + phone_p.extra_bottom + 160 == phone_p.height,
          "margins sum to view");
    const auto phone_l = geo(3088, 1440, 19.7f, 1.0f);
    check(phone_l.height >= 240 && phone_l.height <= 250 && phone_l.width >= 500 &&
              aspect_ok(phone_l, 3088, 1440),
          "phone landscape ~523x244");
    // 270 dpi tablet: both axes grow well beyond native.
    const auto tab_l = geo(2560, 1600, 10.6f, 1.0f);
    check(tab_l.width == 569 && tab_l.height >= 340 && aspect_ok(tab_l, 2560, 1600),
          "tablet landscape fills to max width with far more rows");
    const auto tab_p = geo(1600, 2560, 10.6f, 1.0f);
    check(tab_p.width > 450 && tab_p.height > 700 && tab_p.height <= 854 &&
              aspect_ok(tab_p, 1600, 2560),
          "tablet portrait shows much more world on both axes");
    // Zoom never shrinks the view below native.
    const auto zoomed = geo(1440, 3088, 19.7f, 2.0f);
    check(zoomed.width == 240 && zoomed.height >= 160, "zoomed phone clamps to native width");
    // Low-density desktop window keeps at least native size.
    const auto small = geo(300, 200, 3.78f, 1.0f);
    check(small.width >= 240 && small.height >= 160, "tiny window at least native");
    check(geo(0, 100, 10.0f, 1.0f).width == 240, "invalid drawable -> native");
}

void test_json() {
    json::Value v;
    std::string err;
    check(json::parse("{\"a\":[1,2.5,-3e2],\"b\":{\"c\":\"x\\\"y\\u00e9\"},\"t\":true,\"n\":null}",
                      v, &err),
          "parse nested JSON");
    check(v.get("a") && v.get("a")->array.size() == 3 &&
              near(static_cast<float>(v.get("a")->array[2].number), -300.0f),
          "array numbers");
    check(v.get("b")->str("c") == "x\"y\xc3\xa9", "string escapes + unicode");
    check(v.flag("t", false) && v.get("n")->is_null(), "bool and null");
    check(!json::parse("{\"a\":", v, &err), "truncated JSON rejected");
    json::Value hex;
    json::parse("{\"addr\":\"0x02037590\"}", hex);
    check(hex.integer("addr", 0) == 0x02037590, "hex string integers");
}

}  // namespace

int main() {
    test_presentation_inverse();
    test_tap_and_slop();
    test_long_press();
    test_drag_and_swipe();
    test_multi_finger();
    test_key_synth();
    test_keyinput_composition();
    test_hub_script_and_rings();
    test_density_geometry();
    test_json();
    if (g_failures) {
        std::fprintf(stderr, "touch_input_tests: %d failure(s)\n", g_failures);
        return 1;
    }
    std::puts("touch_input_tests: PASS");
    return 0;
}
