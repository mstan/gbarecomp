// Inspect a paused TCP capture without executing guest code. Usage:
// gen3_view_capture_check <capture-dir> <ROM> [width] [--height N]
// [--require-visible-objects] [--require-dormant-objects] [--expect-native]
// Files: ewram.bin, iwram.bin, vram.bin, io.bin, pal.bin, oam.bin.
#include "field_view.h"
#include "object_view.h"
#include "gba_ppu.h"
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string_view>
#include <vector>

namespace {
gen3::FieldView view;
gen3::ObjectView objects;
const gba::WsMarginObjPixel* object_provider(int y, int* left, int* width) {
    return objects.row(y, left, width);
}
int provider(int bg, int x, int y, std::uint16_t* out) {
    return view.tile(bg, x, y, out) ? gba::kWsTilemapReplace : gba::kWsTilemapUnavailable;
}
std::vector<std::uint8_t> load(const std::filesystem::path& path, std::size_t size) {
    std::ifstream file(path, std::ios::binary);
    std::vector<std::uint8_t> data(size);
    if (!file.read(reinterpret_cast<char*>(data.data()), size) || file.peek() != EOF) {
        std::cerr << "Invalid capture size: " << path << '\n'; std::exit(1);
    }
    return data;
}
}

int main(int argc, char** argv) {
    if (argc < 3) { std::cerr << "capture directory and ROM required\n"; return 1; }
    const std::filesystem::path root(argv[1]);
    const int width = argc > 3 ? std::atoi(argv[3]) : 569;
    bool require_visible_objects = false, require_dormant_objects = false, expect_native = false;
    int height = 160;
    for (int i = 4; i < argc; ++i) {
        if (std::string_view(argv[i]) == "--require-visible-objects") require_visible_objects = true;
        else if (std::string_view(argv[i]) == "--require-dormant-objects") require_dormant_objects = true;
        else if (std::string_view(argv[i]) == "--expect-native") expect_native = true;
        else if (std::string_view(argv[i]) == "--height" && i + 1 < argc) height = std::atoi(argv[++i]);
        else return 1;
    }
    if (width < 240 || width > gen3::kMaxViewWidth || height < 160 || height > gen3::kMaxViewHeight) return 1;
    auto ewram = load(root / "ewram.bin", 0x40000), iwram = load(root / "iwram.bin", 0x8000);
    auto vram = load(root / "vram.bin", 0x18000), io = load(root / "io.bin", 0x400);
    auto pal = load(root / "pal.bin", 0x400), oam = load(root / "oam.bin", 0x400);
    auto rom = load(argv[2], 0x1000000);
    gen3::ViewMemory memory{ewram.data(), iwram.data(), rom.data(), rom.size(), vram.data(), io.data(), oam.data(), pal.data(), &gen3::game_profile()};
    const auto status = view.prepare(memory, width, height);
    std::cout << gen3::view_status_name(status) << " native tiles=" << view.matched() << '/' << view.compared() << '\n';
    const bool object_ready = objects.prepare(memory, view, width, height);
    std::cout << "objects=" << objects.objects() << " OAM-parts=" << objects.verified_parts() << " ready=" << object_ready << '\n';
    std::cout << "dormant objects=" << objects.dormant_objects() << '\n';
    int object_pixels = 0;
    const int top = (height - 160) / 2;
    for (int y=-top; y<height-top; ++y) {
        int left=0, count=0;
        if (const auto* row=objects.row(y,&left,&count))
            for (int x=0; x<count; ++x) object_pixels += (row[x].color & 0x8000) == 0;
    }
    std::cout << "authored OBJ margin pixels=" << object_pixels << '\n';
    gba::GbaPpu ppu;
    std::vector<std::uint8_t> native(240*160*3), expanded(width*height*3);
    const std::uint16_t dispcnt = io[0] | (unsigned(io[1]) << 8);
    ppu.render(native.data(), dispcnt, io.data(), vram.data(), oam.data(), pal.data());
    ppu.set_view_margins((width-240)/2, (width-240+1)/2, top, height-160-top);
    gba::g_ws_tilemap_provider = provider;
    gba::g_ws_authored_margin_layers = 1;
    gba::g_ws_obj_native_clip = 1;
    gba::g_ws_pillarbox = status == gen3::ViewStatus::Ready ? 0 : 1;
    ppu.render(expanded.data(), dispcnt, io.data(), vram.data(), oam.data(), pal.data());
    const auto scenery = expanded;
    gba::g_ws_obj_margin_provider = object_provider;
    ppu.render(expanded.data(), dispcnt, io.data(), vram.data(), oam.data(), pal.data());
    int visible_object_pixels = 0;
    for (int i=0; i<width*height; ++i)
        visible_object_pixels += scenery[i*3] != expanded[i*3] || scenery[i*3+1] != expanded[i*3+1] || scenery[i*3+2] != expanded[i*3+2];
    std::cout << "visible OBJ margin pixels=" << visible_object_pixels << '\n';
    int differences = 0;
    for (int y=0; y<160; ++y) for (int x=0; x<240*3; ++x)
        differences += native[y*240*3+x] != expanded[((y+top)*width+(width-240)/2)*3+x];
    std::cout << "native center differing channels=" << differences << '\n';
    int margin_pixels = 0;
    for (int y=0; y<height; ++y) for (int x=0; x<width; ++x) {
        if (x >= (width-240)/2 && x < (width-240)/2+240 && y >= top && y < top+160) continue;
        const int offset = (y*width+x)*3;
        margin_pixels += expanded[offset] || expanded[offset+1] || expanded[offset+2];
    }
    std::cout << "nonblack margin pixels=" << margin_pixels << '\n';
    std::ofstream file(root / ("render-"+std::to_string(width)+"x"+std::to_string(height)+".rgb"), std::ios::binary);
    file.write(reinterpret_cast<const char*>(expanded.data()), expanded.size());
    if (expect_native) return status == gen3::ViewStatus::Ready || differences != 0 || margin_pixels != 0;
    return status != gen3::ViewStatus::Ready || !object_ready || differences != 0 ||
        (require_dormant_objects && objects.dormant_objects() == 0) ||
        (require_visible_objects && visible_object_pixels == 0);
}
