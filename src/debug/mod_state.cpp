// mod_state.cpp -- see mod_state.h.

#include "mod_state.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <cstdlib>
#include <fstream>
#include <limits>

#if defined(_WIN32)
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace gbarecomp::debug {
namespace {

constexpr uint32_t kModsFormat = 1;
constexpr uint32_t kSidecarVersion = 1;
constexpr std::size_t kMaxIdLength = 64;
constexpr std::size_t kMaxIdentityLength = 256;
constexpr std::size_t kMaxSidecarPayload = 16 * 1024 * 1024;
constexpr std::array<uint8_t, 8> kSidecarMagic = {'G','B','A','M','O','D','S','1'};

void set_error(std::string* error, const std::string& value) {
    if (error) *error = value;
}

bool valid_provider_id(const char* id) {
    if (!id || !id[0]) return false;
    const std::size_t n = std::strlen(id);
    if (n > kMaxIdLength ||
        (!std::islower(static_cast<unsigned char>(id[0])) &&
         !std::isdigit(static_cast<unsigned char>(id[0])))) return false;
    for (std::size_t i = 0; i < n; ++i) {
        const unsigned char c = static_cast<unsigned char>(id[i]);
        if (!(std::islower(c) || std::isdigit(c) || c == '.' || c == '_' || c == '-')) {
            return false;
        }
    }
    return true;
}

void write_string(SnapshotWriter& out, const std::string& value) {
    out.u16(static_cast<uint16_t>(value.size()));
    out.bytes(value.data(), value.size());
}

bool read_string(SnapshotReader& in, std::string* value, std::size_t maximum) {
    const std::size_t size = in.u16();
    if (!in.ok() || size > maximum || size > in.remaining()) return false;
    value->resize(size);
    in.bytes(value->data(), size);
    return in.ok();
}

uint32_t crc32(const uint8_t* data, std::size_t size) {
    uint32_t crc = 0xFFFFFFFFu;
    for (std::size_t i = 0; i < size; ++i) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ (0xEDB88320u & static_cast<uint32_t>(-(crc & 1u)));
    }
    return ~crc;
}

void append_u32(std::vector<uint8_t>& out, uint32_t value) {
    for (unsigned i = 0; i != 4; ++i) out.push_back(static_cast<uint8_t>(value >> (i * 8)));
}

uint32_t load_u32(const uint8_t* bytes) {
    return static_cast<uint32_t>(bytes[0]) |
           (static_cast<uint32_t>(bytes[1]) << 8) |
           (static_cast<uint32_t>(bytes[2]) << 16) |
           (static_cast<uint32_t>(bytes[3]) << 24);
}

bool flush_file(const std::filesystem::path& path) {
#if defined(_WIN32)
    HANDLE handle = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ,
                                nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) return false;
    const bool ok = FlushFileBuffers(handle) != 0;
    CloseHandle(handle);
    return ok;
#else
    const int fd = open(path.c_str(), O_RDONLY);
    if (fd < 0) return false;
    const bool ok = fsync(fd) == 0;
    close(fd);
    return ok;
#endif
}

bool is_drive_relative_path(const std::filesystem::path& path) {
    // std::filesystem on MSYS/Windows accepts "F:slot" as a path relative to
    // the current directory on drive F.  Never let that spelling create a
    // literal F: directory below the game's sidecar root.
    const std::string text = path.string();
    return text.size() >= 2 && std::isalpha(static_cast<unsigned char>(text[0])) &&
           text[1] == ':' && (text.size() == 2 ||
           (text[2] != '/' && text[2] != '\\'));
}

enum class ReadStatus { Ok, Missing, Corrupt, IdentityMismatch };

ReadStatus read_sidecar_file(const std::filesystem::path& path,
                             const std::string& expected_identity,
                             std::vector<uint8_t>* payload) {
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) return ReadStatus::Missing;
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) return ReadStatus::Corrupt;
    const std::streamoff end = file.tellg();
    if (end < 24 || static_cast<uint64_t>(end) > kMaxSidecarPayload + 280ull)
        return ReadStatus::Corrupt;
    std::vector<uint8_t> bytes(static_cast<std::size_t>(end));
    file.seekg(0);
    file.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!file || !std::equal(kSidecarMagic.begin(), kSidecarMagic.end(), bytes.begin()))
        return ReadStatus::Corrupt;
    const uint32_t version = load_u32(bytes.data() + 8);
    const uint32_t identity_size = load_u32(bytes.data() + 12);
    const uint32_t payload_size = load_u32(bytes.data() + 16);
    const uint32_t stored_crc = load_u32(bytes.data() + 20);
    if (version != kSidecarVersion || identity_size > kMaxIdentityLength ||
        payload_size > kMaxSidecarPayload || bytes.size() != 24ull + identity_size + payload_size)
        return ReadStatus::Corrupt;
    std::vector<uint8_t> crc_bytes = bytes;
    std::fill(crc_bytes.begin() + 20, crc_bytes.begin() + 24, 0);
    if (crc32(crc_bytes.data(), crc_bytes.size()) != stored_crc) return ReadStatus::Corrupt;
    const char* identity = reinterpret_cast<const char*>(bytes.data() + 24);
    if (std::string(identity, identity_size) != expected_identity) return ReadStatus::IdentityMismatch;
    payload->assign(bytes.begin() + 24 + identity_size, bytes.end());
    return ReadStatus::Ok;
}

}  // namespace

bool ModStateRegistry::register_provider(const ModStateProvider& provider, std::string* error) {
    if (frozen_) { set_error(error, "mod state: registration is closed after first snapshot"); return false; }
    if (providers_.size() == kMaxProviders) { set_error(error, "mod state: provider limit reached"); return false; }
    if (!valid_provider_id(provider.id) || !provider.save || !provider.preflight || !provider.restore) {
        set_error(error, "mod state: invalid provider registration"); return false;
    }
    for (const auto& existing : providers_) {
        if (existing.id == provider.id) { set_error(error, "mod state: duplicate provider id"); return false; }
    }
    Entry entry{provider, provider.id};
    // The ID is catalog metadata, not callback input.  Do not retain the
    // caller's pointer (it may have pointed into a temporary std::string).
    entry.callbacks.id = nullptr;
    providers_.push_back(std::move(entry));
    return true;
}

bool ModStateRegistry::serialize(SnapshotWriter& out, std::string* error) const {
    frozen_ = true;
    out.u32(kModsFormat);
    out.u32(static_cast<uint32_t>(providers_.size()));
    for (const auto& entry : providers_) {
        const auto& provider = entry.callbacks;
        SnapshotWriter payload;
        if (!provider.save(provider.user, payload, error)) return false;
        if (payload.size() > kMaxProviderPayload) { set_error(error, "mod state: provider payload too large"); return false; }
        write_string(out, entry.id);
        out.u32(provider.schema);
        out.u32(static_cast<uint32_t>(payload.size()));
        out.bytes(payload.buffer().data(), payload.size());
    }
    return true;
}

bool parse_catalog(const ModStateRegistry& registry, SnapshotReader& in,
                   bool invoke_preflight, bool invoke_restore, std::string* error) {
    if (in.u32() != kModsFormat || in.u32() != registry.size() || !in.ok()) {
        set_error(error, "mod state: catalog format or provider count mismatch"); return false;
    }
    for (std::size_t index = 0; index < registry.size(); ++index) {
        std::string id;
        if (!read_string(in, &id, kMaxIdLength)) { set_error(error, "mod state: invalid provider id in snapshot"); return false; }
        const uint32_t schema = in.u32();
        const std::size_t size = in.u32();
        if (!in.ok() || size > ModStateRegistry::kMaxProviderPayload || size > in.remaining()) {
            set_error(error, "mod state: invalid provider payload length"); return false;
        }
        const auto& provider = registry.provider_at(index);
        if (id != registry.provider_id_at(index) || schema != provider.schema) {
            set_error(error, "mod state: provider catalog/schema mismatch"); return false;
        }
        // SnapshotReader has no public raw cursor, so copy the bounded payload.
        std::vector<uint8_t> bytes(size);
        in.bytes(bytes.data(), size);
        SnapshotReader provider_reader(bytes.data(), bytes.size());
        bool ok = false;
        if (invoke_preflight) ok = provider.preflight(provider.user, provider_reader, error);
        else if (invoke_restore) { provider.restore(provider.user, provider_reader); ok = true; }
        if (!ok || !provider_reader.ok() || provider_reader.remaining() != 0) {
            if (!error || error->empty()) set_error(error, "mod state: provider payload rejected");
            if (invoke_restore) std::abort();
            return false;
        }
    }
    if (!in.ok() || in.remaining() != 0) { set_error(error, "mod state: trailing data in MODS section"); return false; }
    return true;
}

bool ModStateRegistry::preflight(SnapshotReader& in, std::string* error) const {
    frozen_ = true;
    return parse_catalog(*this, in, true, false, error);
}

ModStateRegistry& global_mod_state_registry() {
    static ModStateRegistry registry;
    return registry;
}

bool register_mod_state_provider(const ModStateProvider& provider, std::string* error) {
    return global_mod_state_registry().register_provider(provider, error);
}

bool ModStateRegistry::restore(SnapshotReader& in, std::string* error) const {
    return parse_catalog(*this, in, false, true, error);
}

bool canonical_mod_slot_key(const std::string& input, std::string* output, std::string* error) {
    if (!output || input.empty() || input.size() > 48) { set_error(error, "mod state: invalid slot key"); return false; }
    std::string key;
    key.reserve(input.size());
    for (const unsigned char c : input) {
        if (std::isupper(c)) key.push_back(static_cast<char>(std::tolower(c)));
        else if (std::islower(c) || std::isdigit(c) || c == '_' || c == '-') key.push_back(static_cast<char>(c));
        else { set_error(error, "mod state: slot key contains unsafe characters"); return false; }
    }
    if (!std::islower(static_cast<unsigned char>(key[0])) && !std::isdigit(static_cast<unsigned char>(key[0]))) {
        set_error(error, "mod state: slot key must start alphanumeric"); return false;
    }
    *output = std::move(key);
    return true;
}

bool write_mod_state_slot(const std::filesystem::path& directory, const std::string& slot_key,
                          const std::string& identity, const std::vector<uint8_t>& payload,
                          std::string* error) {
    std::string key;
    if (!canonical_mod_slot_key(slot_key, &key, error)) return false;
    if (is_drive_relative_path(directory)) {
        set_error(error, "mod state: drive-relative sidecar directory rejected"); return false;
    }
    if (identity.size() > kMaxIdentityLength || payload.size() > kMaxSidecarPayload) {
        set_error(error, "mod state: sidecar identity or payload too large"); return false;
    }
    std::error_code ec;
    std::filesystem::create_directories(directory, ec);
    if (ec) { set_error(error, "mod state: cannot create sidecar directory"); return false; }
    const auto primary = directory / (key + ".mods");
    const auto temporary = directory / (key + ".tmp");
    const auto backup = directory / (key + ".bak");
    std::vector<uint8_t> bytes(kSidecarMagic.begin(), kSidecarMagic.end());
    append_u32(bytes, kSidecarVersion); append_u32(bytes, static_cast<uint32_t>(identity.size()));
    append_u32(bytes, static_cast<uint32_t>(payload.size())); append_u32(bytes, 0);
    bytes.insert(bytes.end(), identity.begin(), identity.end());
    bytes.insert(bytes.end(), payload.begin(), payload.end());
    const uint32_t checksum = crc32(bytes.data(), bytes.size());
    for (unsigned i = 0; i != 4; ++i) bytes[20 + i] = static_cast<uint8_t>(checksum >> (i * 8));
    { std::ofstream file(temporary, std::ios::binary | std::ios::trunc); file.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size())); if (!file) { set_error(error, "mod state: temporary sidecar write failed"); return false; } }
    if (!flush_file(temporary)) { std::filesystem::remove(temporary, ec); set_error(error, "mod state: temporary sidecar flush failed"); return false; }
    if (std::filesystem::exists(primary, ec)) {
        std::filesystem::remove(backup, ec);
        std::filesystem::rename(primary, backup, ec);
        if (ec) { std::filesystem::remove(temporary, ec); set_error(error, "mod state: cannot make sidecar backup"); return false; }
    }
    std::filesystem::rename(temporary, primary, ec);
    if (ec) { set_error(error, "mod state: atomic sidecar replacement failed (backup retained)"); return false; }
    return true;
}

bool read_mod_state_slot(const std::filesystem::path& directory, const std::string& slot_key,
                         const std::string& expected_identity, std::vector<uint8_t>* payload,
                         std::string* error) {
    if (!payload) { set_error(error, "mod state: null payload output"); return false; }
    std::string key;
    if (!canonical_mod_slot_key(slot_key, &key, error)) return false;
    if (is_drive_relative_path(directory)) {
        set_error(error, "mod state: drive-relative sidecar directory rejected"); return false;
    }
    const auto primary = directory / (key + ".mods");
    const auto backup = directory / (key + ".bak");
    const ReadStatus primary_status = read_sidecar_file(primary, expected_identity, payload);
    if (primary_status == ReadStatus::Ok) return true;
    if (primary_status == ReadStatus::IdentityMismatch) { set_error(error, "mod state: sidecar identity mismatch"); return false; }
    const ReadStatus backup_status = read_sidecar_file(backup, expected_identity, payload);
    if (backup_status == ReadStatus::Ok) return true;
    set_error(error, backup_status == ReadStatus::IdentityMismatch ? "mod state: backup identity mismatch" : "mod state: no valid sidecar slot");
    return false;
}

}  // namespace gbarecomp::debug
