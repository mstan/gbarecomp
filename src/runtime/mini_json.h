// mini_json.h — small, dependency-free JSON reader/writer for host tooling.
//
// The debug TCP protocol historically matched substrings of each request.
// Structured commands (touch scripts, ring queries, game-owned extensions)
// need real arrays and nested objects, so they parse with this reader instead.
// It is deliberately small: UTF-8 passthrough, doubles for numbers, no
// comments, depth-limited recursion. Never used on a guest-execution path.

#pragma once

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace gbarecomp::json {

struct Value {
    enum class Type { Null, Bool, Number, String, Array, Object };
    Type type = Type::Null;
    bool boolean = false;
    double number = 0.0;
    std::string string;
    std::vector<Value> array;
    std::map<std::string, Value> object;

    bool is_null() const { return type == Type::Null; }
    bool is_number() const { return type == Type::Number; }
    bool is_string() const { return type == Type::String; }
    bool is_array() const { return type == Type::Array; }
    bool is_object() const { return type == Type::Object; }

    const Value* get(const char* key) const {
        if (type != Type::Object) return nullptr;
        auto it = object.find(key);
        return it == object.end() ? nullptr : &it->second;
    }
    double num(const char* key, double fallback) const {
        const Value* v = get(key);
        return v && v->type == Type::Number ? v->number : fallback;
    }
    long long integer(const char* key, long long fallback) const {
        const Value* v = get(key);
        if (!v) return fallback;
        if (v->type == Type::Number) return static_cast<long long>(v->number);
        if (v->type == Type::String) {
            // Allow "0x1234" strings for addresses and masks.
            const char* s = v->string.c_str();
            char* end = nullptr;
            const long long parsed = std::strtoll(s, &end, 0);
            if (end && end != s) return parsed;
        }
        return fallback;
    }
    std::string str(const char* key, const std::string& fallback = {}) const {
        const Value* v = get(key);
        return v && v->type == Type::String ? v->string : fallback;
    }
    bool flag(const char* key, bool fallback) const {
        const Value* v = get(key);
        if (!v) return fallback;
        if (v->type == Type::Bool) return v->boolean;
        if (v->type == Type::Number) return v->number != 0.0;
        return fallback;
    }
};

namespace detail {

class Parser {
public:
    explicit Parser(std::string_view text) : s_(text) {}

    bool parse(Value& out, std::string* error) {
        skip_ws();
        if (!value(out, 0)) {
            if (error) *error = error_.empty() ? "invalid JSON" : error_;
            return false;
        }
        skip_ws();
        if (i_ != s_.size()) {
            if (error) *error = "trailing characters after JSON value";
            return false;
        }
        return true;
    }

private:
    std::string_view s_;
    std::size_t i_ = 0;
    std::string error_;

    void skip_ws() {
        while (i_ < s_.size() &&
               (s_[i_] == ' ' || s_[i_] == '\t' || s_[i_] == '\n' || s_[i_] == '\r'))
            ++i_;
    }
    bool fail(const char* why) {
        if (error_.empty()) {
            char buf[96];
            std::snprintf(buf, sizeof(buf), "%s at offset %zu", why, i_);
            error_ = buf;
        }
        return false;
    }
    bool literal(const char* word) {
        std::size_t n = 0;
        while (word[n]) ++n;
        if (s_.substr(i_, n) != std::string_view(word, n)) return fail("bad literal");
        i_ += n;
        return true;
    }
    bool value(Value& out, int depth) {
        if (depth > 64) return fail("nesting too deep");
        skip_ws();
        if (i_ >= s_.size()) return fail("unexpected end");
        const char c = s_[i_];
        if (c == '{') return object(out, depth);
        if (c == '[') return array(out, depth);
        if (c == '"') { out.type = Value::Type::String; return string(out.string); }
        if (c == 't') { out.type = Value::Type::Bool; out.boolean = true; return literal("true"); }
        if (c == 'f') { out.type = Value::Type::Bool; out.boolean = false; return literal("false"); }
        if (c == 'n') { out.type = Value::Type::Null; return literal("null"); }
        return number(out);
    }
    bool number(Value& out) {
        const std::size_t start = i_;
        if (i_ < s_.size() && (s_[i_] == '-' || s_[i_] == '+')) ++i_;
        while (i_ < s_.size() &&
               ((s_[i_] >= '0' && s_[i_] <= '9') || s_[i_] == '.' ||
                s_[i_] == 'e' || s_[i_] == 'E' || s_[i_] == '-' || s_[i_] == '+'))
            ++i_;
        if (i_ == start) return fail("expected value");
        const std::string text(s_.substr(start, i_ - start));
        char* end = nullptr;
        out.type = Value::Type::Number;
        out.number = std::strtod(text.c_str(), &end);
        if (!end || *end != '\0') return fail("bad number");
        return true;
    }
    static void append_utf8(std::string& out, unsigned cp) {
        if (cp < 0x80) {
            out += static_cast<char>(cp);
        } else if (cp < 0x800) {
            out += static_cast<char>(0xC0 | (cp >> 6));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            out += static_cast<char>(0xE0 | (cp >> 12));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else {
            out += static_cast<char>(0xF0 | (cp >> 18));
            out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        }
    }
    bool hex4(unsigned& cp) {
        if (i_ + 4 > s_.size()) return fail("short unicode escape");
        cp = 0;
        for (int k = 0; k < 4; ++k) {
            const char h = s_[i_++];
            cp <<= 4;
            if (h >= '0' && h <= '9') cp |= static_cast<unsigned>(h - '0');
            else if (h >= 'a' && h <= 'f') cp |= static_cast<unsigned>(h - 'a' + 10);
            else if (h >= 'A' && h <= 'F') cp |= static_cast<unsigned>(h - 'A' + 10);
            else return fail("bad unicode escape");
        }
        return true;
    }
    bool string(std::string& out) {
        ++i_;  // opening quote
        out.clear();
        while (i_ < s_.size()) {
            const char c = s_[i_++];
            if (c == '"') return true;
            if (c != '\\') { out += c; continue; }
            if (i_ >= s_.size()) break;
            const char e = s_[i_++];
            switch (e) {
                case '"': out += '"'; break;
                case '\\': out += '\\'; break;
                case '/': out += '/'; break;
                case 'b': out += '\b'; break;
                case 'f': out += '\f'; break;
                case 'n': out += '\n'; break;
                case 'r': out += '\r'; break;
                case 't': out += '\t'; break;
                case 'u': {
                    unsigned cp = 0;
                    if (!hex4(cp)) return false;
                    if (cp >= 0xD800 && cp <= 0xDBFF && i_ + 6 <= s_.size() &&
                        s_[i_] == '\\' && s_[i_ + 1] == 'u') {
                        i_ += 2;
                        unsigned lo = 0;
                        if (!hex4(lo)) return false;
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    }
                    append_utf8(out, cp);
                    break;
                }
                default: return fail("bad escape");
            }
        }
        return fail("unterminated string");
    }
    bool array(Value& out, int depth) {
        ++i_;
        out.type = Value::Type::Array;
        skip_ws();
        if (i_ < s_.size() && s_[i_] == ']') { ++i_; return true; }
        for (;;) {
            Value item;
            if (!value(item, depth + 1)) return false;
            out.array.push_back(std::move(item));
            skip_ws();
            if (i_ < s_.size() && s_[i_] == ',') { ++i_; continue; }
            if (i_ < s_.size() && s_[i_] == ']') { ++i_; return true; }
            return fail("expected , or ]");
        }
    }
    bool object(Value& out, int depth) {
        ++i_;
        out.type = Value::Type::Object;
        skip_ws();
        if (i_ < s_.size() && s_[i_] == '}') { ++i_; return true; }
        for (;;) {
            skip_ws();
            if (i_ >= s_.size() || s_[i_] != '"') return fail("expected key");
            std::string key;
            if (!string(key)) return false;
            skip_ws();
            if (i_ >= s_.size() || s_[i_] != ':') return fail("expected :");
            ++i_;
            Value item;
            if (!value(item, depth + 1)) return false;
            out.object[std::move(key)] = std::move(item);
            skip_ws();
            if (i_ < s_.size() && s_[i_] == ',') { ++i_; continue; }
            if (i_ < s_.size() && s_[i_] == '}') { ++i_; return true; }
            return fail("expected , or }");
        }
    }
};

}  // namespace detail

inline bool parse(std::string_view text, Value& out, std::string* error = nullptr) {
    return detail::Parser(text).parse(out, error);
}

// Append `text` as a quoted JSON string.
inline void append_quoted(std::string& out, std::string_view text) {
    out += '"';
    for (const char c : text) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x",
                                  static_cast<unsigned>(static_cast<unsigned char>(c)));
                    out += buf;
                } else {
                    out += c;
                }
        }
    }
    out += '"';
}

inline std::string error_reply(std::string_view why) {
    std::string out = "{\"ok\":false,\"error\":";
    append_quoted(out, why);
    out += '}';
    return out;
}

}  // namespace gbarecomp::json
