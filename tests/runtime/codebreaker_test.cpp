// codebreaker_test.cpp -- strict CodeBreaker Advance parser + interpreter.
//
// Semantics are pinned against mGBA's CodeBreaker implementation (the oracle
// named in src/runtime/codebreaker.h); the Buu's Fury lines are the curated
// codes from the NovaPowers fork that motivated the engine.
#include "codebreaker.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace cb = gbarecomp::codebreaker;

namespace {

int g_failures = 0;

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__,     \
                         #cond);                                             \
            ++g_failures;                                                    \
        }                                                                    \
    } while (0)

struct FakeGba {
    std::vector<uint8_t> ewram = std::vector<uint8_t>(256 * 1024, 0);
    std::vector<uint8_t> iwram = std::vector<uint8_t>(32 * 1024, 0);
    uint16_t keyinput = 0x03FF;  // nothing held
    uint64_t stray_accesses = 0;  // anything outside RAM/KEYINPUT

    uint8_t* byte(uint32_t a) {
        if (a >= 0x02000000u && a < 0x02040000u) return &ewram[a - 0x02000000u];
        if (a >= 0x03000000u && a < 0x03008000u) return &iwram[a - 0x03000000u];
        ++stray_accesses;
        return nullptr;
    }
    uint16_t get16(uint32_t a) {
        if (a == cb::kKeyInputAddress) return keyinput;
        uint8_t* p = byte(a);
        return p ? static_cast<uint16_t>(p[0] | (p[1] << 8)) : 0;
    }
    void set16(uint32_t a, uint16_t v) {
        if (uint8_t* p = byte(a)) {
            p[0] = static_cast<uint8_t>(v);
            p[1] = static_cast<uint8_t>(v >> 8);
        }
    }
    uint32_t get32(uint32_t a) { return get16(a) | (uint32_t{get16(a + 2)} << 16); }

    cb::Memory memory() {
        cb::Memory m;
        m.ctx = this;
        m.read16 = [](void* c, uint32_t a) { return static_cast<FakeGba*>(c)->get16(a); };
        m.write8 = [](void* c, uint32_t a, uint8_t v) {
            if (uint8_t* p = static_cast<FakeGba*>(c)->byte(a)) *p = v;
        };
        m.write16 = [](void* c, uint32_t a, uint16_t v) {
            static_cast<FakeGba*>(c)->set16(a, v);
        };
        return m;
    }
};

cb::Program must_parse(const std::vector<std::string>& lines) {
    cb::Program program;
    std::string error;
    if (!cb::parse(lines, &program, &error)) {
        std::fprintf(stderr, "FAIL unexpected parse error: %s\n", error.c_str());
        ++g_failures;
    }
    return program;
}

bool rejects(const std::vector<std::string>& lines, const char* needle) {
    cb::Program program;
    std::string error;
    const bool ok = cb::parse(lines, &program, &error);
    if (ok || error.find(needle) == std::string::npos || !program.ops.empty()) {
        std::fprintf(stderr, "FAIL expected rejection containing '%s', got %s '%s'\n",
                     needle, ok ? "success" : "error", error.c_str());
        return false;
    }
    return true;
}

void test_rejections() {
    CHECK(rejects({}, "no codes"));
    CHECK(rejects({"8300155C"}, "hex words"));
    CHECK(rejects({"8300155C E0FF trailing"}, "hex words"));
    CHECK(rejects({"8300155G E0FF"}, "hex words"));
    CHECK(rejects({""}, "hex words"));
    CHECK(rejects({"9A3B5C7D 1234"}, "encrypted"));
    CHECK(rejects({"D0000010 0001", "83000000 0001"}, "D0000020"));
    CHECK(rejects({"84000000 0001"}, "outside EWRAM/IWRAM"));   // IO
    CHECK(rejects({"88000000 0001"}, "outside EWRAM/IWRAM"));   // ROM
    CHECK(rejects({"86000000 0001"}, "outside EWRAM/IWRAM"));   // VRAM
    CHECK(rejects({"83000001 0001"}, "misaligned"));
    CHECK(rejects({"83007FFE 0001", "8303FFFE 0001"}, "line 2"));
    CHECK(rejects({"32040000 0001"}, "outside EWRAM/IWRAM"));
    CHECK(rejects({"73000000 0001"}, "not followed"));
    CHECK(rejects({"43000000 0001"}, "continuation"));
    CHECK(rejects({"43000000 0001", "00000002 0003"}, "even"));
    CHECK(rejects({"43007FF0 0001", "00000010 0002"}, "leaves"));
    CHECK(rejects({"53000000 0004", "11223344 5566"}, "missing data"));
    CHECK(rejects({"53000000 0000"}, "no halfwords"));
    CHECK(rejects({"53007FFE 0002", "11223344 5566"}, "leaves"));
    CHECK(rejects({"00000000 0001", "10000000 0002"}, "no code has an effect"));
    // A failed parse never hands back a partial program.
    cb::Program program;
    program.ops.resize(3);
    std::string error;
    CHECK(!cb::parse({"83000000 0001", "bad"}, &program, &error));
    CHECK(program.ops.empty());
}

void test_writes_and_alu() {
    FakeGba g;
    g.set16(0x02000010, 0x00F0);
    g.set16(0x02000012, 0xFF0F);
    g.set16(0x02000014, 0xFFFF);
    const cb::Program p = must_parse({
        "00000000 1234",  // master codes are accepted and inert
        "1000ABCD 0000",
        "32000001 00AB",  // 8-bit: low byte only
        "83000002 BEEF",
        "22000010 0F00",  // OR
        "62000012 00FF",  // AND
        "E2000014 0002",  // ADD wraps at 16 bits
    });
    CHECK(p.ops.size() == 5);
    cb::run(p, g.memory());
    CHECK(g.ewram[1] == 0xAB && g.ewram[0] == 0x00 && g.ewram[2] == 0x00);
    CHECK(g.get16(0x03000002) == 0xBEEF);
    CHECK(g.get16(0x02000010) == 0x0FF0);
    CHECK(g.get16(0x02000012) == 0x000F);
    CHECK(g.get16(0x02000014) == 0x0001);
    CHECK(g.stray_accesses == 0);
}

void test_buus_fury_presets() {
    // NovaPowers' curated BG3E codes: Max Zenie and Infinite HP (all slots).
    FakeGba g;
    cb::run(must_parse({"8300155C E0FF", "8300155E 05F5"}), g.memory());
    CHECK(g.get32(0x0300155C) == 99999999u);

    const cb::Program hp = must_parse({
        "43001576 270F", "00000008 0068",
        "43001578 270F", "00000008 0068",
    });
    CHECK(hp.ops.size() == 2);
    cb::run(hp, g.memory());
    for (uint32_t slot = 0; slot < 8; ++slot) {
        CHECK(g.get16(0x03001576 + slot * 0x68) == 9999);
        CHECK(g.get16(0x03001578 + slot * 0x68) == 9999);
    }
    CHECK(g.get16(0x03001576 + 8 * 0x68) == 0);  // exactly eight slots
    CHECK(g.get16(0x03001574) == 0);
    CHECK(g.stray_accesses == 0);
}

void test_slide_operand_step_and_fill_list() {
    FakeGba g;
    cb::run(must_parse({"42000100 0010", "00050004 0004"}), g.memory());
    for (uint32_t k = 0; k < 4; ++k)
        CHECK(g.get16(0x02000100 + k * 4) == 0x0010 + k * 5);
    CHECK(g.get16(0x02000110) == 0);

    // Fill list: halfwords arrive three per line and are byte-swapped.
    cb::run(must_parse({"52000200 0004", "11223344 5566", "77880000 0000"}),
            g.memory());
    CHECK(g.get16(0x02000200) == 0x2211);
    CHECK(g.get16(0x02000202) == 0x4433);
    CHECK(g.get16(0x02000204) == 0x6655);
    CHECK(g.get16(0x02000206) == 0x8877);
    CHECK(g.get16(0x02000208) == 0);
    CHECK(g.stray_accesses == 0);
}

void test_conditionals() {
    FakeGba g;
    g.set16(0x02000000, 5);
    // True conditions run the next code; false ones skip exactly one code,
    // including every continuation line of a slide.
    cb::run(must_parse({
        "72000000 0005", "82000010 0001",   // == true
        "72000000 0006", "82000012 0001",   // == false
        "A2000000 0006", "82000014 0001",   // != true
        "B2000000 0004", "82000016 0001",   // > true
        "B2000000 0005", "82000018 0001",   // > false
        "C2000000 0006", "8200001A 0001",   // < true
        "F2000000 0004", "8200001C 0001",   // & true
        "F2000000 0002", "8200001E 0001",   // & false
        "72000000 0000", "42000020 0007", "00000002 0002",  // skips whole slide
        "82000030 0001",                    // runs: the skip consumed one code
    }), g.memory());
    CHECK(g.get16(0x02000010) == 1);
    CHECK(g.get16(0x02000012) == 0);
    CHECK(g.get16(0x02000014) == 1);
    CHECK(g.get16(0x02000016) == 1);
    CHECK(g.get16(0x02000018) == 0);
    CHECK(g.get16(0x0200001A) == 1);
    CHECK(g.get16(0x0200001C) == 1);
    CHECK(g.get16(0x0200001E) == 0);
    CHECK(g.get16(0x02000020) == 0 && g.get16(0x02000022) == 0);
    CHECK(g.get16(0x02000030) == 1);

    // Comparisons are unsigned 16-bit, as in mGBA (both sides are 0..FFFF).
    g.set16(0x02000040, 0x8000);
    cb::run(must_parse({"C2000040 0001", "82000042 0001"}), g.memory());
    CHECK(g.get16(0x02000042) == 0);

    // Nested: a false outer conditional skips the inner conditional, so the
    // code after it runs unconditionally (one code per skip).
    cb::run(must_parse({"72000000 0009", "72000000 0005", "82000050 0001",
                        "82000052 0001"}), g.memory());
    CHECK(g.get16(0x02000050) == 1);
    CHECK(g.get16(0x02000052) == 1);
}

void test_button_conditional() {
    const cb::Program p = must_parse({"D0000020 0003", "83000000 0001"});
    FakeGba g;
    cb::run(p, g.memory());              // nothing held
    CHECK(g.get16(0x03000000) == 0);
    g.keyinput = 0x03FE;                 // A only: B still released
    cb::run(p, g.memory());
    CHECK(g.get16(0x03000000) == 0);
    g.keyinput = 0x03FC;                 // A + B held (active low)
    cb::run(p, g.memory());
    CHECK(g.get16(0x03000000) == 1);
    CHECK(g.stray_accesses == 0);
}

void test_rerun_is_stable() {
    // The runner re-asserts every frame; an idempotent code reaches the same
    // memory image no matter how many frames it has run.
    const cb::Program p = must_parse({"83001840 466D", "32000000 0001"});
    FakeGba once, many;
    cb::run(p, once.memory());
    for (int i = 0; i < 60; ++i) cb::run(p, many.memory());
    CHECK(once.ewram == many.ewram && once.iwram == many.iwram);
}

}  // namespace

int main() {
    test_rejections();
    test_writes_and_alu();
    test_buus_fury_presets();
    test_slide_operand_step_and_fill_list();
    test_conditionals();
    test_button_conditional();
    test_rerun_is_stable();
    if (g_failures) {
        std::fprintf(stderr, "codebreaker_tests: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("codebreaker_tests: OK\n");
    return 0;
}
