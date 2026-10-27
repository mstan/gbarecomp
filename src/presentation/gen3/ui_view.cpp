#include "ui_view.h"
#include <algorithm>
#include "gba_ppu.h"
#include <memory>
#include <cstring>

namespace gen3 {
namespace {
unsigned u16(const std::uint8_t* p) { return p[0] | (unsigned(p[1]) << 8); }
int signed_scroll(unsigned n) { return int((n + 256) & 511) - 256; }
}
void UiView::prepare(const ViewMemory& m, const FieldView& field, int width, int height) {
    count_ = 0;
    composed_ = false;
    active_ = m.profile && field.status() == ViewStatus::Ready && m.io && m.vram;
    if (!active_) return;
    const unsigned cnt = u16(m.io + 8);
    if (cnt & 0xC080) { active_ = false; return; }
    const int left = (width - 240) / 2, right = width - 240 - left;
    const int top = (height - 160) / 2, bottom = height - 160 - top;
    const int scroll_x = signed_scroll(u16(m.io + 0x10));
    const int scroll_y = signed_scroll(u16(m.io + 0x12));
    const unsigned screen = ((cnt >> 8) & 31) * 0x800;
    if (!m.profile->frlg) {
        // Ruby/Sapphire draw directly into BG0. Recognize complete published
        // frame borders, then move their live pixels (text and cursors included).
        const auto tile = [&](int x, int y) { return u16(m.vram + screen + (y*32+x)*2); };
        const auto add = [&](int x, int y, int w, int h) {
            if (count_ == int(windows_.size())) return;
            const bool al = x <= 2, ar = x+w >= 28, at = y <= 2, ab = y+h >= 19;
            const int dx = al == ar ? 0 : ar ? right : -left;
            int dy = at ? -top : ab ? bottom : 0;
            if (ar && !al && at && top > 0) dy = 80 + (bottom-top)/2 - (y*8+h*4);
            if (dx || dy) windows_[count_++] = {x*8-scroll_x,y*8-scroll_y,w*8,h*8,dx,dy};
        };
        const unsigned frame = m.u16(m.profile->standard_frame) & 1023;
        if (frame) for (int y = 0; y < 19; ++y) for (int x = 0; x < 29; ++x) {
            const unsigned corner = tile(x,y);
            if ((corner & 1023) != frame) continue;
            for (int r=x+2; r<30; ++r) {
                if (tile(r,y) != corner+2) continue;
                bool top_ok=true;
                for (int tx=x+1; tx<r; ++tx) top_ok &= tile(tx,y)==corner+1;
                if (!top_ok) continue;
                for (int b=y+2; b<20; ++b) {
                    if (tile(x,b)!=corner+6 || tile(r,b)!=corner+8) continue;
                    bool valid=true;
                    for (int tx=x+1; tx<r; ++tx) valid &= tile(tx,b)==corner+7;
                    for (int ty=y+1; ty<b; ++ty) valid &= tile(x,ty)==corner+3 && tile(r,ty)==corner+5;
                    if (valid) add(x,y,r-x+1,b-y+1);
                }
            }
        }
        // The authored dialogue frame spans all 30 columns at rows 14..19.
        // Its published corners identify visibility independently of text state.
        const unsigned dlg=m.u16(m.profile->dialogue_frame) & 1023;
        if (dlg && (tile(0,14)&0xFFF)==dlg+1 && (tile(29,14)&0xFFF)==dlg+6 &&
            (tile(0,19)&0xFFF)==((dlg+1)|0x800) &&
            (tile(29,19)&0xFFF)==((dlg+6)|0x800)) add(0,14,30,6);
        if (count_ && m.oam && m.pal) compose(m, cnt);
        return;
    }
    for (int i = 0; i < 32; ++i) {
        const auto* w = m.bytes(m.profile->windows + i * 12, 12);
        if (!w || w[0] != 0 || !w[3] || !w[4] || w[1] + w[3] > 30 ||
            w[2] + w[4] > 20 || !m.u32(m.profile->windows + i * 12 + 8)) continue;
        // Allocated but hidden windows share BG0 with active ones. Require
        // a published interior tile before assigning an anchor rectangle.
        const auto base = u16(w + 6);
        bool visible = false;
        for (int ty = 0; ty < w[4] && !visible; ++ty) for (int tx = 0; tx < w[3]; ++tx) {
            const auto entry = u16(m.vram + screen + ((w[2] + ty) * 32 + w[1] + tx) * 2);
            if ((entry & 1023) == base + ty * w[3] + tx && (entry >> 12) == w[5]) {
                visible = true; break;
            }
        }
        if (!visible) continue;
        const bool at_left = w[1] <= 2, at_right = w[1] + w[3] >= 28;
        const bool at_top = w[2] <= 2, at_bottom = w[2] + w[4] >= 19;
        const int dx = at_left == at_right ? 0 : at_right ? right : -left;
        int dy = at_top ? -top : at_bottom ? bottom : 0;
        if (at_right && !at_left && at_top && top > 0) {
            // Right-edge menus that open from the top (the field Start menu)
            // are centred vertically in the view instead of pinned to its top
            // edge: on a tall phone the top corner is out of thumb reach.
            const int window_center = int(w[2]) * 8 + int(w[4]) * 4;
            const int view_center = 80 + (bottom - top) / 2;  // native coords
            dy = view_center - window_center;
        }
        if (!dx && !dy) continue;
        // Standard frames occupy one tile column left of the window; dialogue
        // frames (menu.c WindowFunc_DrawDialogueFrame and its custom-tile
        // variants) occupy two (tilemapLeft - 2 and - 1). Their outer column
        // repeats one vertical-edge tile down every interior row, while window
        // interiors never repeat a tile, so the repeat identifies the frame.
        int frame_left = 1;
        if (w[1] >= 2) {
            const auto edge = u16(m.vram + screen + (w[2] * 32 + w[1] - 2) * 2);
            bool repeated = (edge & 1023) != 0;
            for (int ty = 1; ty < w[4] && repeated; ++ty)
                repeated = u16(m.vram + screen + ((w[2] + ty) * 32 + w[1] - 2) * 2) == edge;
            if (repeated) frame_left = 2;
        }
        const int x = std::max(0, (int(w[1]) - frame_left) * 8);
        const int y = std::max(0, (int(w[2]) - 1) * 8);
        windows_[count_++] = {x - scroll_x, y - scroll_y,
            std::min(240, (w[1] + w[3] + 1) * 8) - x,
            std::min(160, (w[2] + w[4] + 1) * 8) - y, dx, dy};
    }
}

int UiView::sample(int bg, int x, int y, int* sx, int* sy) const {
    if (!active_ || bg != 0) return 0;
    const auto inside = [](const Rect& r, int px, int py) {
        return px >= r.x && px < r.x + r.w && py >= r.y && py < r.y + r.h;
    };
    // Destination rectangles take precedence when movement overlaps source.
    for (int i = count_ - 1; i >= 0; --i) {
        const auto& r = windows_[i];
        if (inside(r, x - r.dx, y - r.dy)) { *sx = x - r.dx; *sy = y - r.dy; return 1; }
    }
    for (int i = 0; i < count_; ++i) if (inside(windows_[i], x, y)) return -1;
    return x < 0 || x >= 240 || y < 0 || y >= 160 ? -1 : 0;
}

void UiView::compose(const ViewMemory& m, unsigned cnt) {
    // R/S uses normal and OBJ-window sprites for the outline cursor. Sample
    // the faithful native compositor so both sprites and their stencil move
    // with the live window, including in portrait. Only host buffers change.
    auto ppu = std::make_unique<gba::GbaPpu>();
    const unsigned dispcnt = u16(m.io);
    ppu->render(native_ui_.data(), dispcnt, m.io, m.vram, m.oam, m.pal);
    std::array<std::uint8_t,1024> oam;
    std::copy_n(m.oam, oam.size(), oam.begin());
    const auto* ids = m.bytes(m.profile->cursor_sprites, 2);
    if (ids) for (int i=0;i<2;++i) {
        if (ids[i]>=64) continue;
        const auto address=m.profile->sprites+ids[i]*68;
        const auto* sprite=m.bytes(address,68);
        if (!sprite || !(sprite[62]&1)) continue;
        const auto tag=m.u16(m.u32(address+20));
        if (tag!=0xFFF0 && tag!=0xFFF1) continue;
        const unsigned base=m.u16(address+64), palette=u16(sprite+4)>>12;
        for (int n=0;n<128;++n) {
            const auto a2=u16(oam.data()+n*8+4);
            if ((a2>>12)==palette && (a2&1023)>=base && (a2&1023)<base+14)
                oam[n*8+1]|=2; // disabled non-affine cursor part
        }
    }
    ppu->render(native_world_.data(), dispcnt & ~0x8100u, m.io, m.vram, oam.data(), m.pal);
    const unsigned screen=((cnt>>8)&31)*0x800, chars=((cnt>>2)&3)*0x4000;
    const unsigned hofs=u16(m.io+0x10), vofs=u16(m.io+0x12);
    for (unsigned y=0;y<160;++y) for (unsigned x=0;x<240;++x) {
        const unsigned tx=(x+hofs)&255,ty=(y+vofs)&255;
        const auto entry=u16(m.vram+screen+((ty/8)*32+tx/8)*2);
        const unsigned px=(entry&0x400)?7-(tx&7):tx&7, py=(entry&0x800)?7-(ty&7):ty&7;
        const unsigned offset=chars+(entry&1023)*32+py*4+px/2;
        opaque_[y*240+x]=offset<0x10000 && ((m.vram[offset]>>((px&1)*4))&15)!=0;
    }
    composed_=true;
}

int UiView::pixel(int x,int y,std::uint16_t* color) const {
    if (!composed_ || !color) return 0;
    int sx=x,sy=y;
    const int action=sample(0,x,y,&sx,&sy);
    const std::uint8_t* rgb=nullptr;
    if (action==1 && sx>=0 && sx<240 && sy>=0 && sy<160 && opaque_[sy*240+sx])
        rgb=native_ui_.data()+(sy*240+sx)*3;
    if (action<0 && x>=0 && x<240 && y>=0 && y<160) {
        for (int i=0;i<count_;++i) {
            const auto& r=windows_[i];
            if (x>=r.x && x<r.x+r.w && y>=r.y && y<r.y+r.h)
                rgb=native_world_.data()+(y*240+x)*3;
        }
    }
    if (!rgb) return 0;
    *color=(rgb[0]>>3)|((rgb[1]>>3)<<5)|((rgb[2]>>3)<<10);
    return 1;
}
}
