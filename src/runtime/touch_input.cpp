// touch_input.cpp — gesture recognizer, touch hub rings, TCP touch commands.

#include "touch_input.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>

#include "mini_json.h"

namespace gbarecomp {

const char* gesture_kind_name(GestureKind kind) {
    switch (kind) {
        case GestureKind::Tap: return "tap";
        case GestureKind::LongPress: return "long_press";
        case GestureKind::DragBegin: return "drag_begin";
        case GestureKind::DragMove: return "drag_move";
        case GestureKind::DragEnd: return "drag_end";
        case GestureKind::Swipe: return "swipe";
        case GestureKind::TwoFingerTap: return "two_finger_tap";
        case GestureKind::ThreeFingerTap: return "three_finger_tap";
        case GestureKind::Back: return "back";
        case GestureKind::Cancel: return "cancel";
    }
    return "unknown";
}

const char* touch_phase_name(TouchPhase phase) {
    switch (phase) {
        case TouchPhase::Down: return "down";
        case TouchPhase::Move: return "move";
        case TouchPhase::Up: return "up";
        case TouchPhase::Cancel: return "cancel";
    }
    return "unknown";
}

namespace {

const char* swipe_name(SwipeDirection d) {
    switch (d) {
        case SwipeDirection::None: return "none";
        case SwipeDirection::Left: return "left";
        case SwipeDirection::Right: return "right";
        case SwipeDirection::Up: return "up";
        case SwipeDirection::Down: return "down";
    }
    return "none";
}

const char* source_name(TouchSource s) {
    switch (s) {
        case TouchSource::Finger: return "finger";
        case TouchSource::Mouse: return "mouse";
        case TouchSource::Tcp: return "tcp";
    }
    return "unknown";
}

bool parse_phase(const std::string& text, TouchPhase* phase) {
    if (text == "down") *phase = TouchPhase::Down;
    else if (text == "move") *phase = TouchPhase::Move;
    else if (text == "up") *phase = TouchPhase::Up;
    else if (text == "cancel") *phase = TouchPhase::Cancel;
    else return false;
    return true;
}

}  // namespace

// ── GestureRecognizer ──────────────────────────────────────────────────────

GestureRecognizer::Pointer* GestureRecognizer::find(std::int32_t id) {
    for (auto& p : pointers_)
        if (p.active && p.id == id) return &p;
    return nullptr;
}

GestureRecognizer::Pointer* GestureRecognizer::allocate(std::int32_t id) {
    if (Pointer* existing = find(id)) return existing;
    for (auto& p : pointers_) {
        if (!p.active) {
            p = Pointer{};
            p.active = true;
            p.id = id;
            return &p;
        }
    }
    return nullptr;
}

std::size_t GestureRecognizer::active_count() const {
    std::size_t n = 0;
    for (const auto& p : pointers_)
        if (p.active) ++n;
    return n;
}

void GestureRecognizer::snapshot(std::vector<TouchPointSnapshot>& out) const {
    out.clear();
    for (const auto& p : pointers_) {
        if (!p.active) continue;
        TouchPointSnapshot s;
        s.pointer = p.id;
        s.view_x = p.vx;
        s.view_y = p.vy;
        s.drawable_x = p.dx;
        s.drawable_y = p.dy;
        s.start_view_x = p.start_vx;
        s.start_view_y = p.start_vy;
        s.down_ms = p.start_ms;
        s.moved = p.moved;
        s.long_pressed = p.long_fired;
        out.push_back(s);
    }
}

Gesture GestureRecognizer::make(const Pointer& p, GestureKind kind,
                                std::uint32_t t) const {
    Gesture g;
    g.kind = kind;
    g.pointer = p.id;
    g.t_ms = t;
    g.view_x = p.vx;
    g.view_y = p.vy;
    g.start_view_x = p.start_vx;
    g.start_view_y = p.start_vy;
    g.drawable_x = p.dx;
    g.drawable_y = p.dy;
    g.start_drawable_x = p.start_dx;
    g.start_drawable_y = p.start_dy;
    g.duration_ms = t - p.start_ms;
    g.start_in_view = p.start_in_view;
    g.source = p.source;
    return g;
}

void GestureRecognizer::on_event(const TouchEvent& e, std::vector<Gesture>& out) {
    switch (e.phase) {
        case TouchPhase::Down: {
            Pointer* p = allocate(e.pointer);
            if (!p) return;  // more than ten fingers: ignore the extra
            p->source = e.source;
            p->start_dx = p->dx = e.drawable_x;
            p->start_dy = p->dy = e.drawable_y;
            p->start_vx = p->vx = e.view_x;
            p->start_vy = p->vy = e.view_y;
            p->start_in_view = e.in_view;
            p->start_ms = p->last_ms = e.t_ms;
            p->hist_n = 1;
            p->hist_head = 0;
            p->hist_vx[0] = e.drawable_x;
            p->hist_vy[0] = e.drawable_y;
            p->hist_t[0] = e.t_ms;
            const int active = static_cast<int>(active_count());
            if (!session_active_) {
                session_active_ = true;
                session_start_ms_ = e.t_ms;
                session_max_fingers_ = 1;
                session_any_moved_ = false;
                session_multi_ = false;
            } else if (active >= 2) {
                if (!session_multi_) {
                    // The single-finger interpretation of the first finger is
                    // abandoned. Tell the consumer about anything it saw start.
                    for (auto& other : pointers_) {
                        if (!other.active || other.id == e.pointer) continue;
                        if (other.dragging || other.long_fired)
                            out.push_back(make(other, GestureKind::Cancel, e.t_ms));
                        other.dragging = false;
                    }
                }
                session_multi_ = true;
                if (e.t_ms - session_start_ms_ > config_.multi_finger_window_ms)
                    session_any_moved_ = true;  // too late to be a multi-tap
            }
            session_max_fingers_ = std::max(session_max_fingers_, active);
            break;
        }
        case TouchPhase::Move:
        case TouchPhase::Up: {
            Pointer* p = find(e.pointer);
            if (!p) return;
            p->dx = e.drawable_x;
            p->dy = e.drawable_y;
            p->vx = e.view_x;
            p->vy = e.view_y;
            p->last_ms = e.t_ms;
            p->hist_head = (p->hist_head + 1) % 6;
            p->hist_vx[p->hist_head] = e.drawable_x;
            p->hist_vy[p->hist_head] = e.drawable_y;
            p->hist_t[p->hist_head] = e.t_ms;
            p->hist_n = std::min(6, p->hist_n + 1);
            const float ddx = p->dx - p->start_dx;
            const float ddy = p->dy - p->start_dy;
            if (!p->moved && ddx * ddx + ddy * ddy > slop_px() * slop_px()) {
                p->moved = true;
                session_any_moved_ = true;
            }
            if (!session_multi_ && p->moved && !p->long_fired) {
                if (!p->dragging) {
                    p->dragging = true;
                    out.push_back(make(*p, GestureKind::DragBegin, e.t_ms));
                } else if (e.phase == TouchPhase::Move) {
                    out.push_back(make(*p, GestureKind::DragMove, e.t_ms));
                }
            }
            if (e.phase == TouchPhase::Move) break;

            // Release.
            if (!session_multi_) {
                if (p->dragging) {
                    Gesture end = make(*p, GestureKind::DragEnd, e.t_ms);
                    // Velocity over the most recent ~100 ms of samples.
                    int oldest = p->hist_head;
                    for (int k = 1; k < p->hist_n; ++k) {
                        const int idx = (p->hist_head - k + 6) % 6;
                        if (e.t_ms - p->hist_t[idx] > 100) break;
                        oldest = idx;
                    }
                    const std::uint32_t dt = e.t_ms - p->hist_t[oldest];
                    float vx_d = 0.0f, vy_d = 0.0f;
                    if (dt > 0) {
                        vx_d = (p->dx - p->hist_vx[oldest]) * 1000.0f / dt;
                        vy_d = (p->dy - p->hist_vy[oldest]) * 1000.0f / dt;
                    }
                    end.velocity_x = vx_d * config_.view_px_per_drawable_px;
                    end.velocity_y = vy_d * config_.view_px_per_drawable_px;
                    out.push_back(end);
                    const float dist = std::sqrt(ddx * ddx + ddy * ddy);
                    const float speed = std::sqrt(vx_d * vx_d + vy_d * vy_d);
                    const float min_dist = config_.swipe_min_mm * config_.drawable_px_per_mm;
                    const float min_speed =
                        config_.swipe_min_mm_per_s * config_.drawable_px_per_mm;
                    const float ax = std::fabs(ddx), ay = std::fabs(ddy);
                    if (dist >= min_dist && speed >= min_speed &&
                        (ax >= 2.0f * ay || ay >= 2.0f * ax)) {
                        Gesture swipe = end;
                        swipe.kind = GestureKind::Swipe;
                        swipe.swipe = ax > ay
                            ? (ddx < 0 ? SwipeDirection::Left : SwipeDirection::Right)
                            : (ddy < 0 ? SwipeDirection::Up : SwipeDirection::Down);
                        out.push_back(swipe);
                    }
                } else if (!p->moved && !p->long_fired) {
                    out.push_back(make(*p, GestureKind::Tap, e.t_ms));
                }
            }
            const Pointer released = *p;
            p->active = false;
            if (active_count() == 0) {
                const std::uint32_t span = e.t_ms - session_start_ms_;
                if (session_multi_ && !session_any_moved_ &&
                    span <= config_.multi_finger_tap_max_ms) {
                    Gesture g = make(released, session_max_fingers_ >= 3
                                                   ? GestureKind::ThreeFingerTap
                                                   : GestureKind::TwoFingerTap,
                                     e.t_ms);
                    g.fingers = static_cast<std::uint8_t>(session_max_fingers_);
                    g.duration_ms = span;
                    out.push_back(g);
                }
                session_active_ = false;
                session_multi_ = false;
            }
            break;
        }
        case TouchPhase::Cancel: {
            Pointer* p = find(e.pointer);
            if (!p) return;
            if (!session_multi_ && (p->dragging || p->long_fired))
                out.push_back(make(*p, GestureKind::Cancel, e.t_ms));
            p->active = false;
            if (active_count() == 0) {
                session_active_ = false;
                session_multi_ = false;
            }
            break;
        }
    }
}

void GestureRecognizer::advance(std::uint32_t now_ms, std::vector<Gesture>& out) {
    if (session_multi_) return;
    for (auto& p : pointers_) {
        if (!p.active || p.moved || p.long_fired) continue;
        if (now_ms - p.start_ms >= config_.long_press_ms &&
            static_cast<std::int32_t>(now_ms - p.start_ms) >= 0) {
            p.long_fired = true;
            out.push_back(make(p, GestureKind::LongPress, now_ms));
        }
    }
}

void GestureRecognizer::cancel_all(std::uint32_t now_ms, std::vector<Gesture>& out) {
    for (auto& p : pointers_) {
        if (!p.active) continue;
        if (!session_multi_ && (p.dragging || p.long_fired))
            out.push_back(make(p, GestureKind::Cancel, now_ms));
        p.active = false;
    }
    session_active_ = false;
    session_multi_ = false;
}

// ── TouchHub ───────────────────────────────────────────────────────────────

TouchHub& TouchHub::instance() {
    static TouchHub hub;
    return hub;
}

TouchHub::TouchHub() {
    event_ring_.resize(kEventRing);
    gesture_ring_.resize(kGestureRing);
    synth_ring_.resize(kSynthRing);
    update_config_locked();
}

void TouchHub::update_config_locked() {
    GestureConfig c = recognizer_.config();
    c.drawable_px_per_mm = std::max(0.5f, presentation_.drawable_px_per_mm);
    c.view_px_per_drawable_px = presentation_.layout.width > 0
        ? static_cast<float>(presentation_.view_width) /
              static_cast<float>(presentation_.layout.width)
        : 0.25f;
    recognizer_.configure(c);
}

void TouchHub::set_presentation(const TouchPresentation& p) {
    std::lock_guard<std::mutex> lk(m_);
    presentation_ = p;
    update_config_locked();
}

TouchPresentation TouchHub::presentation() const {
    std::lock_guard<std::mutex> lk(m_);
    return presentation_;
}

void TouchHub::set_claims(std::uint32_t claims) {
    std::lock_guard<std::mutex> lk(m_);
    claims_ = claims;
}

std::uint32_t TouchHub::claims() const {
    std::lock_guard<std::mutex> lk(m_);
    return claims_;
}

void TouchHub::set_timing(std::uint32_t tap_max_ms, std::uint32_t long_press_ms) {
    std::lock_guard<std::mutex> lk(m_);
    GestureConfig c = recognizer_.config();
    c.tap_max_ms = tap_max_ms;
    c.long_press_ms = long_press_ms;
    recognizer_.configure(c);
    update_config_locked();
}

GestureConfig TouchHub::gesture_config() const {
    std::lock_guard<std::mutex> lk(m_);
    return recognizer_.config();
}

std::vector<TouchPointSnapshot> TouchHub::points() const {
    std::lock_guard<std::mutex> lk(m_);
    std::vector<TouchPointSnapshot> out;
    recognizer_.snapshot(out);
    return out;
}

bool TouchHub::drawable_to_view(float dx, float dy, float* vx, float* vy) const {
    std::lock_guard<std::mutex> lk(m_);
    return presentation_point_to_logical(presentation_.layout, presentation_.view_width,
                                         presentation_.view_height, dx, dy, vx, vy);
}

void TouchHub::view_to_drawable(float vx, float vy, float* dx, float* dy) const {
    std::lock_guard<std::mutex> lk(m_);
    logical_point_to_presentation(presentation_.layout, presentation_.view_width,
                                  presentation_.view_height, vx, vy, dx, dy);
}

void TouchHub::push_gestures_locked(std::vector<Gesture>& gestures) {
    for (auto& g : gestures) {
        g.seq = next_gesture_seq_++;
        gesture_ring_[(g.seq - 1) % kGestureRing] = g;
        ++gesture_total_;
        pending_gestures_.push_back(g);
        const bool host_action =
            g.kind == GestureKind::ThreeFingerTap ||
            (g.kind == GestureKind::LongPress && !(claims_ & kTouchClaimLongPress)) ||
            (g.kind == GestureKind::Back && !(claims_ & kTouchClaimBack));
        if (host_action) host_actions_.push_back(g);
    }
    // Bound unconsumed queues so a game that never drains (TCP paused, etc.)
    // cannot grow memory without limit; the rings keep the history anyway.
    constexpr std::size_t kPendingCap = 4096;
    if (pending_gestures_.size() > kPendingCap)
        pending_gestures_.erase(pending_gestures_.begin(),
                                pending_gestures_.end() - kPendingCap);
    if (host_actions_.size() > 64)
        host_actions_.erase(host_actions_.begin(), host_actions_.end() - 64);
}

void TouchHub::submit_locked(TouchEvent event) {
    event.seq = next_event_seq_++;
    event_ring_[(event.seq - 1) % kEventRing] = event;
    ++event_total_;
    pending_events_.push_back(event);
    constexpr std::size_t kPendingCap = 8192;
    if (pending_events_.size() > kPendingCap)
        pending_events_.erase(pending_events_.begin(),
                              pending_events_.end() - kPendingCap);
    std::vector<Gesture> gestures;
    recognizer_.on_event(event, gestures);
    push_gestures_locked(gestures);
}

void TouchHub::submit(TouchEvent event) {
    std::lock_guard<std::mutex> lk(m_);
    submit_locked(event);
}

void TouchHub::submit_gesture(Gesture gesture) {
    std::lock_guard<std::mutex> lk(m_);
    std::vector<Gesture> one{gesture};
    push_gestures_locked(one);
}

void TouchHub::advance(std::uint32_t now_ms) {
    std::lock_guard<std::mutex> lk(m_);
    if (script_clock_active_) return;  // the script owns time while it runs
    std::vector<Gesture> gestures;
    recognizer_.advance(now_ms, gestures);
    push_gestures_locked(gestures);
}

void TouchHub::cancel_all(std::uint32_t now_ms) {
    std::lock_guard<std::mutex> lk(m_);
    std::vector<Gesture> gestures;
    recognizer_.cancel_all(now_ms, gestures);
    push_gestures_locked(gestures);
}

std::vector<Gesture> TouchHub::take_host_actions() {
    std::lock_guard<std::mutex> lk(m_);
    std::vector<Gesture> out;
    out.swap(host_actions_);
    return out;
}

void TouchHub::queue_script(std::vector<TouchScriptStep> steps) {
    std::stable_sort(steps.begin(), steps.end(),
                     [](const TouchScriptStep& a, const TouchScriptStep& b) {
                         return a.frame_offset < b.frame_offset;
                     });
    std::lock_guard<std::mutex> lk(m_);
    script_.assign(steps.begin(), steps.end());
    script_started_ = false;
}

bool TouchHub::script_active() const {
    std::lock_guard<std::mutex> lk(m_);
    return !script_.empty() || script_clock_active_;
}

TouchHub::FrameBatch TouchHub::drain(std::uint64_t guest_frame, std::uint32_t now_ms) {
    std::lock_guard<std::mutex> lk(m_);
    FrameBatch batch;
    std::uint32_t clock = now_ms;
    if (!script_.empty()) {
        if (!script_started_) {
            script_started_ = true;
            script_base_frame_ = guest_frame;
            script_base_ms_ = script_clock_active_ ? script_clock_ms_ : now_ms;
            script_clock_active_ = true;
        }
        const std::uint64_t elapsed = guest_frame - script_base_frame_;
        clock = script_base_ms_ +
                static_cast<std::uint32_t>(std::llround(elapsed * 16.7427));
        script_clock_ms_ = clock;
        while (!script_.empty() && script_.front().frame_offset <= elapsed) {
            const TouchScriptStep step = script_.front();
            script_.pop_front();
            TouchEvent e;
            e.t_ms = clock;
            e.pointer = step.pointer;
            e.phase = step.phase;
            e.source = TouchSource::Tcp;
            if (step.drawable_space) {
                e.drawable_x = step.x;
                e.drawable_y = step.y;
                e.in_view = presentation_point_to_logical(
                    presentation_.layout, presentation_.view_width,
                    presentation_.view_height, e.drawable_x, e.drawable_y,
                    &e.view_x, &e.view_y);
            } else {
                e.view_x = step.x;
                e.view_y = step.y;
                logical_point_to_presentation(
                    presentation_.layout, presentation_.view_width,
                    presentation_.view_height, e.view_x, e.view_y,
                    &e.drawable_x, &e.drawable_y);
                e.in_view = e.view_x >= 0 && e.view_y >= 0 &&
                            e.view_x < presentation_.view_width &&
                            e.view_y < presentation_.view_height;
            }
            submit_locked(e);
        }
    } else if (script_clock_active_) {
        // Keep the deterministic clock running one frame at a time until every
        // scripted finger is released, then hand time back to the host clock.
        script_clock_ms_ += 17;
        clock = script_clock_ms_;
        if (recognizer_.active_count() == 0) script_clock_active_ = false;
    }
    std::vector<Gesture> gestures;
    recognizer_.advance(clock, gestures);
    push_gestures_locked(gestures);

    for (auto& e : pending_events_) {
        e.guest_frame = guest_frame;
        TouchEvent& slot = event_ring_[(e.seq - 1) % kEventRing];
        if (slot.seq == e.seq) slot.guest_frame = guest_frame;
    }
    for (auto& g : pending_gestures_) {
        g.guest_frame = guest_frame;
        Gesture& slot = gesture_ring_[(g.seq - 1) % kGestureRing];
        if (slot.seq == g.seq) slot.guest_frame = guest_frame;
    }
    batch.events.swap(pending_events_);
    batch.gestures.swap(pending_gestures_);
    recognizer_.snapshot(batch.points);
    batch.now_ms = clock;
    return batch;
}

void TouchHub::record_key_synth(const KeySynthSample& sample) {
    std::lock_guard<std::mutex> lk(m_);
    synth_ring_[synth_total_ % kSynthRing] = sample;
    ++synth_total_;
}

std::string TouchHub::events_json(std::uint64_t since_seq, std::size_t limit) const {
    std::lock_guard<std::mutex> lk(m_);
    const std::uint64_t newest = next_event_seq_ - 1;
    const std::uint64_t oldest = newest >= kEventRing ? newest - kEventRing + 1 : 1;
    std::uint64_t first = std::max<std::uint64_t>(since_seq + 1, oldest);
    if (limit == 0) limit = 512;
    std::string out;
    char head[192];
    std::snprintf(head, sizeof(head),
                  "{\"ok\":true,\"total\":%llu,\"oldest\":%llu,\"newest\":%llu,\"events\":[",
                  static_cast<unsigned long long>(event_total_),
                  static_cast<unsigned long long>(newest ? oldest : 0),
                  static_cast<unsigned long long>(newest));
    out = head;
    std::size_t n = 0;
    for (std::uint64_t s = first; s <= newest && n < limit; ++s, ++n) {
        const TouchEvent& e = event_ring_[(s - 1) % kEventRing];
        char item[320];
        std::snprintf(item, sizeof(item),
                      "%s{\"seq\":%llu,\"t_ms\":%u,\"frame\":%llu,\"pointer\":%d,"
                      "\"phase\":\"%s\",\"source\":\"%s\",\"dx\":%.1f,\"dy\":%.1f,"
                      "\"vx\":%.2f,\"vy\":%.2f,\"in_view\":%s}",
                      n ? "," : "", static_cast<unsigned long long>(e.seq), e.t_ms,
                      static_cast<unsigned long long>(e.guest_frame), e.pointer,
                      touch_phase_name(e.phase), source_name(e.source), e.drawable_x,
                      e.drawable_y, e.view_x, e.view_y, e.in_view ? "true" : "false");
        out += item;
    }
    out += "]}";
    return out;
}

std::string TouchHub::gestures_json(std::uint64_t since_seq, std::size_t limit) const {
    std::lock_guard<std::mutex> lk(m_);
    const std::uint64_t newest = next_gesture_seq_ - 1;
    const std::uint64_t oldest = newest >= kGestureRing ? newest - kGestureRing + 1 : 1;
    std::uint64_t first = std::max<std::uint64_t>(since_seq + 1, oldest);
    if (limit == 0) limit = 512;
    std::string out;
    char head[192];
    std::snprintf(head, sizeof(head),
                  "{\"ok\":true,\"total\":%llu,\"oldest\":%llu,\"newest\":%llu,\"gestures\":[",
                  static_cast<unsigned long long>(gesture_total_),
                  static_cast<unsigned long long>(newest ? oldest : 0),
                  static_cast<unsigned long long>(newest));
    out = head;
    std::size_t n = 0;
    for (std::uint64_t s = first; s <= newest && n < limit; ++s, ++n) {
        const Gesture& g = gesture_ring_[(s - 1) % kGestureRing];
        char item[448];
        std::snprintf(item, sizeof(item),
                      "%s{\"seq\":%llu,\"kind\":\"%s\",\"t_ms\":%u,\"frame\":%llu,"
                      "\"pointer\":%d,\"x\":%.2f,\"y\":%.2f,\"start_x\":%.2f,"
                      "\"start_y\":%.2f,\"velocity_x\":%.1f,\"velocity_y\":%.1f,"
                      "\"duration_ms\":%u,\"swipe\":\"%s\",\"fingers\":%u,"
                      "\"start_in_view\":%s,\"source\":\"%s\"}",
                      n ? "," : "", static_cast<unsigned long long>(g.seq),
                      gesture_kind_name(g.kind), g.t_ms,
                      static_cast<unsigned long long>(g.guest_frame), g.pointer,
                      g.view_x, g.view_y, g.start_view_x, g.start_view_y,
                      g.velocity_x, g.velocity_y, g.duration_ms, swipe_name(g.swipe),
                      static_cast<unsigned>(g.fingers),
                      g.start_in_view ? "true" : "false", source_name(g.source));
        out += item;
    }
    out += "]}";
    return out;
}

std::string TouchHub::key_synth_json(std::uint64_t since_frame, std::size_t limit) const {
    std::lock_guard<std::mutex> lk(m_);
    const std::uint64_t count = std::min<std::uint64_t>(synth_total_, kSynthRing);
    if (limit == 0) limit = 1024;
    std::string out;
    char head[128];
    std::snprintf(head, sizeof(head), "{\"ok\":true,\"total\":%llu,\"samples\":[",
                  static_cast<unsigned long long>(synth_total_));
    out = head;
    std::size_t n = 0;
    for (std::uint64_t i = synth_total_ - count; i < synth_total_ && n < limit; ++i) {
        const KeySynthSample& s = synth_ring_[i % kSynthRing];
        if (s.frame < since_frame) continue;
        char item[128];
        std::snprintf(item, sizeof(item),
                      "%s{\"frame\":%llu,\"host\":%u,\"synth\":%u,\"composed\":%u}",
                      n ? "," : "", static_cast<unsigned long long>(s.frame),
                      s.host, s.synth, s.composed);
        out += item;
        ++n;
    }
    out += "]}";
    return out;
}

std::string TouchHub::status_json() const {
    std::lock_guard<std::mutex> lk(m_);
    char buf[640];
    std::snprintf(buf, sizeof(buf),
                  "{\"ok\":true,\"claims\":%u,\"active_pointers\":%zu,"
                  "\"script_pending\":%zu,\"script_clock\":%s,\"events\":%llu,"
                  "\"gestures\":%llu,\"synth_frames\":%llu,\"drawable\":[%d,%d],"
                  "\"layout\":[%d,%d,%d,%d],\"view\":[%d,%d],\"margins\":[%u,%u,%u,%u],"
                  "\"px_per_mm\":%.3f,\"insets\":[%d,%d,%d,%d]}",
                  claims_, recognizer_.active_count(), script_.size(),
                  script_clock_active_ ? "true" : "false",
                  static_cast<unsigned long long>(event_total_),
                  static_cast<unsigned long long>(gesture_total_),
                  static_cast<unsigned long long>(synth_total_),
                  presentation_.drawable_width, presentation_.drawable_height,
                  presentation_.layout.x, presentation_.layout.y,
                  presentation_.layout.width, presentation_.layout.height,
                  presentation_.view_width, presentation_.view_height,
                  presentation_.extra_left, presentation_.extra_right,
                  presentation_.extra_top, presentation_.extra_bottom,
                  presentation_.drawable_px_per_mm, presentation_.safe_insets.left,
                  presentation_.safe_insets.top, presentation_.safe_insets.right,
                  presentation_.safe_insets.bottom);
    return buf;
}

void TouchHub::reset_for_tests() {
    std::lock_guard<std::mutex> lk(m_);
    std::vector<Gesture> discard;
    recognizer_.cancel_all(0, discard);
    next_event_seq_ = next_gesture_seq_ = 1;
    event_total_ = gesture_total_ = synth_total_ = 0;
    std::fill(event_ring_.begin(), event_ring_.end(), TouchEvent{});
    std::fill(gesture_ring_.begin(), gesture_ring_.end(), Gesture{});
    std::fill(synth_ring_.begin(), synth_ring_.end(), KeySynthSample{});
    pending_events_.clear();
    pending_gestures_.clear();
    host_actions_.clear();
    script_.clear();
    script_started_ = false;
    script_clock_active_ = false;
    claims_ = 0;
}

// ── TCP ────────────────────────────────────────────────────────────────────

std::uint32_t touch_clock_ms() {
    using namespace std::chrono;
    static const auto t0 = steady_clock::now();
    return static_cast<std::uint32_t>(
        duration_cast<milliseconds>(steady_clock::now() - t0).count());
}

namespace {
std::uint32_t steady_ms() { return touch_clock_ms(); }
}  // namespace

bool touch_tcp_command(const std::string& request, std::string& out) {
    // Cheap prefilter so ordinary commands never pay for a JSON parse.
    if (request.find("\"touch_") == std::string::npos &&
        request.find("\"key_synth\"") == std::string::npos)
        return false;
    json::Value req;
    std::string error;
    if (!json::parse(request, req, &error) || !req.is_object()) return false;
    const std::string cmd = req.str("cmd");
    if (cmd.rfind("touch_", 0) != 0 && cmd != "key_synth") return false;

    TouchHub& hub = TouchHub::instance();
    const bool drawable = req.str("space", "view") == "drawable";
    const std::int32_t pointer = static_cast<std::int32_t>(req.integer("pointer", 0));

    auto script_reply = [&](std::size_t steps) {
        char buf[96];
        std::snprintf(buf, sizeof(buf), "{\"ok\":true,\"queued_steps\":%zu}", steps);
        out = buf;
    };

    if (cmd == "touch_status") { out = hub.status_json(); return true; }
    if (cmd == "touch_events") {
        out = hub.events_json(static_cast<std::uint64_t>(req.integer("since", 0)),
                              static_cast<std::size_t>(req.integer("limit", 512)));
        return true;
    }
    if (cmd == "touch_gestures") {
        out = hub.gestures_json(static_cast<std::uint64_t>(req.integer("since", 0)),
                                static_cast<std::size_t>(req.integer("limit", 512)));
        return true;
    }
    if (cmd == "key_synth") {
        out = hub.key_synth_json(static_cast<std::uint64_t>(req.integer("since_frame", 0)),
                                 static_cast<std::size_t>(req.integer("limit", 1024)));
        return true;
    }
    if (cmd == "touch_back") {
        Gesture g;
        g.kind = GestureKind::Back;
        g.t_ms = steady_ms();
        g.source = TouchSource::Tcp;
        hub.submit_gesture(g);
        out = "{\"ok\":true}";
        return true;
    }
    if (cmd == "touch_down" || cmd == "touch_move" || cmd == "touch_up" ||
        cmd == "touch_cancel") {
        // Immediate events on the host clock (interactive poking).
        TouchEvent e;
        e.t_ms = steady_ms();
        e.pointer = pointer;
        e.source = TouchSource::Tcp;
        e.phase = cmd == "touch_down" ? TouchPhase::Down
                : cmd == "touch_move" ? TouchPhase::Move
                : cmd == "touch_up"   ? TouchPhase::Up : TouchPhase::Cancel;
        const float x = static_cast<float>(req.num("x", 0.0));
        const float y = static_cast<float>(req.num("y", 0.0));
        if (drawable) {
            e.drawable_x = x;
            e.drawable_y = y;
            e.in_view = hub.drawable_to_view(x, y, &e.view_x, &e.view_y);
        } else {
            e.view_x = x;
            e.view_y = y;
            hub.view_to_drawable(x, y, &e.drawable_x, &e.drawable_y);
            const TouchPresentation p = hub.presentation();
            e.in_view = x >= 0 && y >= 0 && x < p.view_width && y < p.view_height;
        }
        hub.submit(e);
        out = "{\"ok\":true}";
        return true;
    }

    std::vector<TouchScriptStep> steps;
    auto step = [&](std::uint32_t f, TouchPhase ph, std::int32_t id, float x, float y) {
        TouchScriptStep s;
        s.frame_offset = f;
        s.phase = ph;
        s.pointer = id;
        s.x = x;
        s.y = y;
        s.drawable_space = drawable;
        steps.push_back(s);
    };
    const float x = static_cast<float>(req.num("x", 0.0));
    const float y = static_cast<float>(req.num("y", 0.0));

    if (cmd == "touch_tap") {
        const auto hold = static_cast<std::uint32_t>(req.integer("hold_frames", 4));
        step(0, TouchPhase::Down, pointer, x, y);
        step(std::max<std::uint32_t>(1, hold), TouchPhase::Up, pointer, x, y);
        hub.queue_script(std::move(steps));
        script_reply(2);
        return true;
    }
    if (cmd == "touch_long_press") {
        const auto hold = static_cast<std::uint32_t>(req.integer("hold_frames", 40));
        step(0, TouchPhase::Down, pointer, x, y);
        step(std::max<std::uint32_t>(1, hold), TouchPhase::Up, pointer, x, y);
        hub.queue_script(std::move(steps));
        script_reply(2);
        return true;
    }
    if (cmd == "touch_two_finger_tap" || cmd == "touch_three_finger_tap") {
        const int fingers = cmd == "touch_two_finger_tap" ? 2 : 3;
        for (int k = 0; k < fingers; ++k)
            step(0, TouchPhase::Down, 100 + k, x + 12.0f * k, y);
        for (int k = 0; k < fingers; ++k)
            step(4, TouchPhase::Up, 100 + k, x + 12.0f * k, y);
        const std::size_t n = steps.size();
        hub.queue_script(std::move(steps));
        script_reply(n);
        return true;
    }
    if (cmd == "touch_drag") {
        const json::Value* points = req.get("points");
        if (!points || !points->is_array() || points->array.size() < 2) {
            out = json::error_reply("touch_drag needs points:[[x,y],...] (>=2)");
            return true;
        }
        const auto per = std::max<std::uint32_t>(
            1, static_cast<std::uint32_t>(req.integer("frames_per_point", 2)));
        const auto settle = static_cast<std::uint32_t>(req.integer("hold_end_frames", 1));
        std::uint32_t f = 0;
        float lx = 0, ly = 0;
        for (std::size_t k = 0; k < points->array.size(); ++k) {
            const json::Value& pt = points->array[k];
            if (!pt.is_array() || pt.array.size() < 2 || !pt.array[0].is_number() ||
                !pt.array[1].is_number()) {
                out = json::error_reply("touch_drag point must be [x,y]");
                return true;
            }
            lx = static_cast<float>(pt.array[0].number);
            ly = static_cast<float>(pt.array[1].number);
            step(f, k == 0 ? TouchPhase::Down : TouchPhase::Move, pointer, lx, ly);
            f += per;
        }
        step(f - per + std::max<std::uint32_t>(1, settle), TouchPhase::Up, pointer, lx, ly);
        const std::size_t n = steps.size();
        hub.queue_script(std::move(steps));
        script_reply(n);
        return true;
    }
    if (cmd == "touch_script") {
        const json::Value* list = req.get("steps");
        if (!list || !list->is_array()) {
            out = json::error_reply("touch_script needs steps:[{f,phase,pointer,x,y}]");
            return true;
        }
        for (const json::Value& s : list->array) {
            TouchPhase ph;
            if (!s.is_object() || !parse_phase(s.str("phase"), &ph)) {
                out = json::error_reply("touch_script step needs phase down|move|up|cancel");
                return true;
            }
            step(static_cast<std::uint32_t>(s.integer("f", 0)), ph,
                 static_cast<std::int32_t>(s.integer("pointer", 0)),
                 static_cast<float>(s.num("x", 0.0)), static_cast<float>(s.num("y", 0.0)));
        }
        const std::size_t n = steps.size();
        hub.queue_script(std::move(steps));
        script_reply(n);
        return true;
    }
    out = json::error_reply("unknown touch command");
    return true;
}

}  // namespace gbarecomp
