// diagnostic_replay.h — always-on, bounded diagnostic replay capture and
// offline playback for player sessions.
//
// Normal windowed play records, at every guest frame boundary (the
// present-in-place VBlank hook), every host input that can reach the guest
// plus fingerprints of the machine state that frame produced. Files rotate in
// segments of kSegmentFrames frames; each segment starts with a full machine
// checkpoint so any retained segment replays on its own. A player attaches the
// diagnostics/replays files; a developer (or an agent) replays them offline
// with the same build and their own ROM/BIOS, and playback stops at the FIRST
// frame whose state differs from the recording, saving that machine.
//
// Cost model (the reason this exists in this shape): the simulation thread
// only serializes a checkpoint at segment boundaries/discontinuities and
// memcpy's the hashed regions into a fixed queue. Hashing, file I/O, sidecar
// copies and eviction run on a background worker (debug/replay_writer.h).
// When storage falls behind, the current segment is abandoned and capture
// resumes with a new checkpoint once the queue drains — gameplay never waits.
//
// Record format (text, one record per line; see docs/DIAGNOSTIC_REPLAY.md):
//   GBARECOMP-REPLAY 1
//   BUILD <engine revision> <executable hash> <title>
//   ROM <sha1>
//   SEGMENT <n> <unix time>
//   S <offset> <size> <hash> <kind>   checkpoint in <file>.state
//   T <scheduling context>            host timing at that checkpoint
//   N <heal key>...                   complete self-healed native set
//   O <heal key>...                   native code adopted at this boundary
//                                      kind C = live continued in place,
//                                           R = live restored this state
//   I <frame> <keyinput> <gyro> <solar> <rtc> <w> <h> <l> <r> <t> <b> <site>
//   H <frame> <region hash>... <save hash>     after that frame executed
//   X <frame>     that frame was cut short by a restore (no fingerprint)
//   G             frames lost to a storage gap (the next checkpoint resyncs)
//   E <frame>     clean exit before that input's frame ran (absent: the
//                 final frame hung, crashed or was killed)

#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <string>
#include <vector>
#include "replay_writer.h"

namespace gbarecomp {

// Every host-originated value that reaches the guest during one frame.
struct DiagnosticFrameInput {
    std::uint16_t keyinput = 0x03FFu;  // composed KEYINPUT, active-low
    std::int32_t gyro = 0;             // gyro sample offset
    std::int32_t solar = -1;           // brightness 0..255, -1 none
    std::int64_t rtc = std::numeric_limits<std::int64_t>::min();  // none
    std::uint16_t width = 240, height = 160;
    std::uint16_t extra_left = 0, extra_right = 0;
    std::uint16_t extra_top = 0, extra_bottom = 0;
};

// One fingerprinted memory range, named in divergence reports. Cartridge save
// memory is supplied separately (always the last fingerprint) so unchanged
// save memory is never copied per frame.
struct DiagnosticRegion {
    const char* name = "";
    const void* data = nullptr;
    std::size_t size = 0;
    // Host bookkeeping that mirrors guest execution but is not guest state
    // (the runtime's call-return stack, trimmed when playback resynchronizes
    // by redispatching). A mismatch here is reported, never a divergence.
    bool advisory = false;
};

struct DiagnosticReplayHost {
    std::function<std::uint64_t()> frame;
    std::function<std::uint64_t()> state_epoch;
    std::function<bool(std::vector<std::uint8_t>&, std::string&)> save_state;
    std::function<bool(const std::vector<std::uint8_t>&, std::string&)> load_state;
    // Host scheduling context that the machine snapshot does not carry
    // (pending cycle debt, event budget, VBlank yield counters), as one
    // whitespace-free token stream. Stored with every checkpoint ("T" record)
    // and reinstated after loading it.
    std::function<std::string()> timing_capture;
    std::function<bool(const std::string&)> timing_restore;
    std::function<void(std::vector<DiagnosticRegion>&)> regions;
    // Cartridge save memory (SRAM/flash/EEPROM bytes; empty when none).
    std::function<void(std::vector<std::uint8_t>&)> save_memory;
    std::function<std::uint64_t()> save_write_seq;
    std::function<void(DiagnosticFrameInput&)> capture_input;
    std::function<void(const DiagnosticFrameInput&)> apply_input;
    // Unwind the guest to the runner after this frame-boundary hook returns
    // and redispatch the published PC (playback's initial synchronization).
    std::function<void()> request_unwind;
    // Self-heal adoption (which uncovered functions run natively instead of
    // through the interpreter bridge) as space-separated hex keys. Optional:
    // hosts without self-heal leave them empty.
    //   capture:  heal_adopt() installs parked native code now and returns its
    //             keys; heal_active() lists everything native; heal_release()
    //             returns the loader to immediate adoption when capture ends.
    //   playback: heal_set_active(keys) makes exactly that set native.
    std::function<std::string()> heal_adopt;
    std::function<std::string()> heal_active;
    std::function<void()> heal_release;
    std::function<bool(const std::string&, std::string&)> heal_set_active;
};

struct DiagnosticReplayConfig {
    std::string directory;   // empty: diagnostics/replays beside the cwd
    std::string tag;         // file name prefix, e.g. "MegaManZero2Recomp"
    std::string rom_sha1;
    std::string engine_revision;
    std::string executable_hash;
    std::string title;
    // Small files copied beside every segment: {source path, suffix}.
    std::vector<std::pair<std::string, std::string>> sidecars;
    // Small texts written beside every segment: {body, suffix} (the
    // session's effective command line, for example).
    std::vector<std::pair<std::string, std::string>> inline_sidecars;
    std::size_t history_bytes = 256u * 1024u * 1024u;
};

class DiagnosticReplay {
public:
    static constexpr unsigned kSegmentFrames = 600;     // 10 s per segment
    static constexpr unsigned kSegments = 360;          // one hour retained
    static constexpr std::size_t kSegmentStateLimit = 16u * 1024u * 1024u;

    DiagnosticReplay();
    ~DiagnosticReplay();

    bool start_recording(const DiagnosticReplayConfig& config,
                         DiagnosticReplayHost host);
    // `path` is any segment (<stem>-NNNNNN.replay); playback continues
    // through the following consecutive segments that exist.
    bool start_playback(const std::string& path, const DiagnosticReplayConfig& config,
                        DiagnosticReplayHost host);

    // Frame boundary, before host input is sampled: the previous frame has
    // executed. Records/compares its fingerprints.
    void frame_end();
    // Frame boundary, after host input was sampled (recording) or in place of
    // host input (playback): opens segments, writes/loads checkpoints, and
    // records/applies this frame's inputs.
    // `site` names the boundary: 'h' present-in-place hook (guest suspended
    // mid-dispatch), 'l' runner loop (guest unwound). Playback must reach
    // every frame at the same kind of boundary as the recording.
    void frame_begin(char site);

    // True at a boundary where no guest frame ran since the previous one
    // (the runner noticing a restore the hook already handled). Host input
    // polled there must wait for the next real boundary.
    bool repeated_boundary() const;
    bool recording() const { return recording_; }
    bool playing() const { return playing_; }
    bool finished() const { return finished_; }
    bool failed() const { return failed_; }
    std::uint64_t matched_frames() const { return matched_; }
    const std::string& session_stem() const { return stem_; }

    // Drain and join the writer (call before process exit).
    void shutdown();

private:
    struct Playback;
    bool start_segment();
    bool write_checkpoint(char kind);
    bool evict_oldest();
    void note_writer_failure();
    void stop_recording(const char* why);
    void fail_playback(const std::string& why);
    bool playback_open_segment(unsigned segment);
    bool playback_checkpoint(const std::string& line);
    void playback_begin(char site);
    void playback_end();
    void dump_frame_state();

    DiagnosticReplayConfig config_;
    DiagnosticReplayHost host_;
    std::unique_ptr<debug::ReplayWriter> writer_;
    std::unique_ptr<Playback> playback_;

    bool recording_ = false;
    bool playing_ = false;
    bool finished_ = false;
    bool failed_ = false;
    bool segment_open_ = false;
    bool pending_end_ = false;
    bool gap_ = false;
    bool gap_marker_pending_ = false;
    bool full_save_next_ = true;
    bool have_boundary_ = false;
    bool skip_begin_ = false;
    std::uint64_t boundary_frame_ = 0;
    std::string stem_;
    std::string directory_;
    unsigned segment_ = 0;
    unsigned oldest_segment_ = 1;
    unsigned rows_ = 0;
    std::uint64_t next_frame_ = 0;
    std::uint64_t epoch_ = 0;
    std::uint64_t save_seq_ = 0;
    std::uint64_t gaps_ = 0;
    std::uint64_t matched_ = 0;
    std::uint64_t advisory_mismatches_ = 0;
    std::size_t segment_state_bytes_ = 0;
    std::size_t history_bytes_ = 0;
    std::vector<std::size_t> history_sizes_;
    std::vector<std::uint8_t> snapshot_;
    std::vector<std::uint8_t> save_bytes_;
};

// Identity stamped into every segment header and checked on playback.
// 16 hex digits of the running executable's content hash, or "unknown".
std::string diagnostic_executable_hash();
// The engine revision this runtime was configured from (CMake), or "unknown".
const char* diagnostic_engine_revision();
// config.ini [Diagnostics] replay_capture=0|1; `fallback` when absent.
bool diagnostic_replay_setting(const std::string& config_ini, bool fallback);

}  // namespace gbarecomp
