// touch_input.h — engine-owned touch model: events, gestures, always-on rings.
//
// Host input sources (SDL fingers, desktop mouse emulation, TCP scripts) all
// submit TouchEvents here. A pure GestureRecognizer turns them into Gestures.
// Once per guest frame (VBlank start) the runtime drains the frame's touches
// and hands them to an opted-in game's input policy, which may answer with a
// synthesized KEYINPUT mask. Every event, gesture and per-frame key
// composition is retained in bounded always-on rings that the debug TCP
// server can query at any time (no arm-then-capture).
//
// Coordinates: "drawable" is physical window pixels (origin top-left);
// "view" is the logical guest view INCLUDING any extended margins (origin at
// the logical framebuffer's top-left, so the native 240x160 area starts at
// (extra_left, extra_top)).

#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

#include "presentation_layout.h"

namespace gbarecomp {

enum class TouchPhase : std::uint8_t { Down = 0, Move = 1, Up = 2, Cancel = 3 };
enum class TouchSource : std::uint8_t { Finger = 0, Mouse = 1, Tcp = 2 };

struct TouchEvent {
    std::uint64_t seq = 0;          // assigned by TouchHub, monotonic
    std::uint32_t t_ms = 0;         // host (or scripted) milliseconds
    std::uint64_t guest_frame = 0;  // guest frame that consumed the event
    std::int32_t  pointer = 0;
    TouchPhase    phase = TouchPhase::Down;
    TouchSource   source = TouchSource::Finger;
    float drawable_x = 0.0f, drawable_y = 0.0f;
    float view_x = 0.0f, view_y = 0.0f;
    bool  in_view = false;          // inside the presented logical image
};

enum class GestureKind : std::uint8_t {
    Tap = 0,
    LongPress,       // fired while the finger is still down
    DragBegin,       // finger left the slop radius (start point = origin)
    DragMove,
    DragEnd,
    Swipe,           // emitted after DragEnd for a fast, axis-dominant fling
    TwoFingerTap,
    ThreeFingerTap,
    Back,            // platform back (Android Back key, desktop Escape map)
    Cancel,          // an in-flight single-finger gesture was abandoned
};

enum class SwipeDirection : std::uint8_t { None = 0, Left, Right, Up, Down };

struct Gesture {
    std::uint64_t seq = 0;
    GestureKind   kind = GestureKind::Tap;
    std::int32_t  pointer = 0;
    std::uint32_t t_ms = 0;
    std::uint64_t guest_frame = 0;
    float view_x = 0.0f, view_y = 0.0f;              // current / final point
    float start_view_x = 0.0f, start_view_y = 0.0f;  // gesture origin
    float drawable_x = 0.0f, drawable_y = 0.0f;
    float start_drawable_x = 0.0f, start_drawable_y = 0.0f;
    float velocity_x = 0.0f, velocity_y = 0.0f;      // view px / second
    std::uint32_t duration_ms = 0;
    SwipeDirection swipe = SwipeDirection::None;
    std::uint8_t  fingers = 1;
    bool start_in_view = false;
    TouchSource source = TouchSource::Finger;
};

struct TouchPointSnapshot {
    std::int32_t pointer = 0;
    float view_x = 0.0f, view_y = 0.0f;
    float drawable_x = 0.0f, drawable_y = 0.0f;
    float start_view_x = 0.0f, start_view_y = 0.0f;
    std::uint32_t down_ms = 0;
    bool moved = false;       // left the slop radius
    bool long_pressed = false;
};

// Gesture ownership. A claimed gesture is the game's; the engine performs no
// built-in action for it (e.g. an unclaimed LongPress opens runtime settings).
enum TouchClaim : std::uint32_t {
    kTouchClaimTap          = 1u << 0,
    kTouchClaimLongPress    = 1u << 1,
    kTouchClaimDrag         = 1u << 2,
    kTouchClaimSwipe        = 1u << 3,
    kTouchClaimTwoFingerTap = 1u << 4,
    kTouchClaimBack         = 1u << 5,
};

struct GestureConfig {
    float drawable_px_per_mm = 6.0f;   // physical density
    float view_px_per_drawable_px = 1.0f / 4.0f;
    float slop_mm = 2.5f;
    std::uint32_t tap_max_ms = 350;
    std::uint32_t long_press_ms = 450;
    float swipe_min_mm = 8.0f;
    float swipe_min_mm_per_s = 120.0f;
    std::uint32_t multi_finger_window_ms = 200;
    std::uint32_t multi_finger_tap_max_ms = 450;
};

// Pure, deterministic recognizer. Time comes only from event timestamps and
// advance(); it never reads a clock, so it is unit-testable and scriptable.
class GestureRecognizer {
public:
    void configure(const GestureConfig& config) { config_ = config; }
    const GestureConfig& config() const { return config_; }

    void on_event(const TouchEvent& event, std::vector<Gesture>& out);
    void advance(std::uint32_t now_ms, std::vector<Gesture>& out);
    void cancel_all(std::uint32_t now_ms, std::vector<Gesture>& out);

    std::size_t active_count() const;
    void snapshot(std::vector<TouchPointSnapshot>& out) const;

private:
    struct Pointer {
        bool active = false;
        std::int32_t id = 0;
        TouchSource source = TouchSource::Finger;
        float start_dx = 0, start_dy = 0, start_vx = 0, start_vy = 0;
        float dx = 0, dy = 0, vx = 0, vy = 0;
        bool start_in_view = false;
        std::uint32_t start_ms = 0, last_ms = 0;
        bool moved = false;
        bool dragging = false;
        bool long_fired = false;
        // Short velocity history (drawable px, ms) for release velocity.
        float hist_vx[6] = {}, hist_vy[6] = {};
        std::uint32_t hist_t[6] = {};
        int hist_n = 0, hist_head = 0;
    };
    GestureConfig config_{};
    Pointer pointers_[10]{};
    // Multi-finger session (from first down until all fingers are up).
    bool session_active_ = false;
    std::uint32_t session_start_ms_ = 0;
    int session_max_fingers_ = 0;
    bool session_any_moved_ = false;
    bool session_multi_ = false;
    float session_vx_ = 0, session_vy_ = 0, session_dx_ = 0, session_dy_ = 0;

    Pointer* find(std::int32_t id);
    Pointer* allocate(std::int32_t id);
    Gesture make(const Pointer& p, GestureKind kind, std::uint32_t t) const;
    float slop_px() const { return config_.slop_mm * config_.drawable_px_per_mm; }
};

// Per-guest-frame input handed to a game's input policy.
struct TouchFrameInfo {
    std::uint64_t frame_count = 0;
    std::uint32_t host_ms = 0;
    std::uint16_t host_keyinput = 0x03FF;   // active-low, before synthesis
    std::uint32_t view_width = 240, view_height = 160;
    std::uint32_t extra_left = 0, extra_right = 0, extra_top = 0, extra_bottom = 0;
    int drawable_width = 0, drawable_height = 0;
    PresentationLayout game_rect{};          // where the view is presented
    float drawable_px_per_mm = 6.0f;
    float view_px_per_mm = 1.5f;
    struct Insets { int left = 0, top = 0, right = 0, bottom = 0; } safe_insets;
    bool pad_visible = false;
    const TouchEvent* events = nullptr;
    std::size_t event_count = 0;
    const Gesture* gestures = nullptr;
    std::size_t gesture_count = 0;
    const TouchPointSnapshot* points = nullptr;
    std::size_t point_count = 0;
};

// Presentation facts the hub needs to map between drawable and view space.
struct TouchPresentation {
    int drawable_width = 960, drawable_height = 640;
    PresentationLayout layout{0, 0, 960, 640, 4};
    int view_width = 240, view_height = 160;
    std::uint32_t extra_left = 0, extra_right = 0, extra_top = 0, extra_bottom = 0;
    float drawable_px_per_mm = 6.0f;
    TouchFrameInfo::Insets safe_insets{};
};

struct KeySynthSample {
    std::uint64_t frame = 0;
    std::uint16_t host = 0x03FF;
    std::uint16_t synth = 0x03FF;
    std::uint16_t composed = 0x03FF;
};

// Scripted touch step (TCP). frame_offset counts guest frames from the frame
// that begins executing the script; view coordinates are logical pixels.
struct TouchScriptStep {
    std::uint32_t frame_offset = 0;
    TouchPhase phase = TouchPhase::Down;
    std::int32_t pointer = 0;
    float x = 0.0f, y = 0.0f;
    bool drawable_space = false;
};

class TouchHub {
public:
    static TouchHub& instance();

    void set_presentation(const TouchPresentation& p);
    TouchPresentation presentation() const;
    void set_claims(std::uint32_t claims);
    std::uint32_t claims() const;
    bool claimed(std::uint32_t claim) const { return (claims() & claim) != 0; }

    // Timing policy (e.g. a longer engine-owned long-press when no game
    // policy claims it). Density fields are overwritten from presentation.
    void set_timing(std::uint32_t tap_max_ms, std::uint32_t long_press_ms);
    GestureConfig gesture_config() const;
    std::vector<TouchPointSnapshot> points() const;

    // Map helpers using the last published presentation.
    bool drawable_to_view(float dx, float dy, float* vx, float* vy) const;
    void view_to_drawable(float vx, float vy, float* dx, float* dy) const;

    // Host side (any thread). Coordinates must be filled for both spaces.
    void submit(TouchEvent event);
    void submit_gesture(Gesture gesture);
    void advance(std::uint32_t now_ms);
    void cancel_all(std::uint32_t now_ms);

    // Gestures the engine itself must act on (settings menu etc.). Drained by
    // the host window each pump.
    std::vector<Gesture> take_host_actions();

    // Scripted input (TCP). Replaces any unfinished script.
    void queue_script(std::vector<TouchScriptStep> steps);
    bool script_active() const;

    // Guest-frame drain (game thread, VBlank). Releases due script steps,
    // advances timers, and returns everything that arrived since the last
    // drain. `now_ms` is the host clock; a running script substitutes its own
    // deterministic frame clock (16.743 ms/frame).
    struct FrameBatch {
        std::vector<TouchEvent> events;
        std::vector<Gesture> gestures;
        std::vector<TouchPointSnapshot> points;
        std::uint32_t now_ms = 0;
    };
    FrameBatch drain(std::uint64_t guest_frame, std::uint32_t now_ms);

    void record_key_synth(const KeySynthSample& sample);

    // JSON views over the always-on rings (TCP).
    std::string events_json(std::uint64_t since_seq, std::size_t limit) const;
    std::string gestures_json(std::uint64_t since_seq, std::size_t limit) const;
    std::string key_synth_json(std::uint64_t since_frame, std::size_t limit) const;
    std::string status_json() const;
    // Every retained entry of every ring plus status, as one JSON object
    // (session diagnostics persisted on background/exit).
    std::string diagnostics_json() const;

    // Test support: clear rings, script, recognizer and queues.
    void reset_for_tests();

    static constexpr std::size_t kEventRing = 8192;
    static constexpr std::size_t kGestureRing = 4096;
    static constexpr std::size_t kSynthRing = 16384;

private:
    TouchHub();
    void submit_locked(TouchEvent event);
    void push_gestures_locked(std::vector<Gesture>& gestures);
    void update_config_locked();

    mutable std::mutex m_;
    GestureRecognizer recognizer_;
    TouchPresentation presentation_{};
    std::uint32_t claims_ = 0;
    std::uint64_t next_event_seq_ = 1;
    std::uint64_t next_gesture_seq_ = 1;
    std::uint64_t event_total_ = 0, gesture_total_ = 0, synth_total_ = 0;
    std::vector<TouchEvent> event_ring_;
    std::vector<Gesture> gesture_ring_;
    std::vector<KeySynthSample> synth_ring_;
    std::vector<TouchEvent> pending_events_;
    std::vector<Gesture> pending_gestures_;
    std::vector<Gesture> host_actions_;
    // Script state.
    std::deque<TouchScriptStep> script_;
    bool script_started_ = false;
    std::uint64_t script_base_frame_ = 0;
    std::uint32_t script_base_ms_ = 0;
    bool script_clock_active_ = false;
    std::uint32_t script_clock_ms_ = 0;
};

// Monotonic milliseconds shared by every touch source and the frame drain.
std::uint32_t touch_clock_ms();

// Debug TCP extension for touch commands. Returns true when `request` named a
// touch command (reply written to `out`), false to let other handlers run.
bool touch_tcp_command(const std::string& request, std::string& out);

const char* gesture_kind_name(GestureKind kind);
const char* touch_phase_name(TouchPhase phase);

}  // namespace gbarecomp
