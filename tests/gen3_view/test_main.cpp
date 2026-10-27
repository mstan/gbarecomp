#include "field_view.h"
#include "object_view.h"
#include "ui_view.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <vector>
#include <utility>

static void check(bool ok, const char* message) {
    if (!ok) { std::fprintf(stderr, "%s: %s\n", gen3::game_profile().name, message); std::exit(1); }
}
struct Fixture {
    const gen3::Profile& p = gen3::game_profile();
    std::vector<std::uint8_t> ew = std::vector<std::uint8_t>(0x40000);
    std::vector<std::uint8_t> iw = std::vector<std::uint8_t>(0x8000);
    std::vector<std::uint8_t> rom = std::vector<std::uint8_t>(0x600000);
    std::vector<std::uint8_t> vr = std::vector<std::uint8_t>(0x18000);
    std::vector<std::uint8_t> io = std::vector<std::uint8_t>(0x400);
    std::vector<std::uint8_t> oa = std::vector<std::uint8_t>(0x400);
    std::vector<std::uint8_t> pa = std::vector<std::uint8_t>(0x400);
    gen3::ViewMemory memory() const {
        return {ew.data(),iw.data(),rom.data(),rom.size(),vr.data(),io.data(),oa.data(),pa.data(),&p};
    }
    void word(std::uint32_t addr, unsigned value) {
        auto* out = const_cast<std::uint8_t*>(memory().bytes(addr,2));
        check(out, "invalid fixture address"); out[0]=value; out[1]=value>>8;
    }
    void dword(std::uint32_t addr, unsigned value) { word(addr,value); word(addr+2,value>>16); }
    void reg(int addr, unsigned value) { io[addr]=value; io[addr+1]=value>>8; }
    void tile(int bg, int x, int y, unsigned value) {
        const auto at = bg*0x800+((y&31)*32+(x&31))*2;
        vr[at]=value;vr[at+1]=value>>8;
    }
    unsigned id(int x, int y) const { return (x*31+y*53)%1000; }
    unsigned entry(int bg, int x, int y) const {
        const unsigned n=id(x/2,y/2), q=(x&1)+2*(y&1);
        const unsigned bottom=(n*8+q)%1000+1, top=(n*8+4+q)%1000+1;
        const unsigned values[3][3]={{top,bottom,0x3014},{0,top,bottom},{top,0,bottom}};
        // Physical SetBgTilemapBuffer bindings, independent of the misleading
        // gBGTilemapBuffers1/2 names in the FireRed reference.
        return values[n%3][bg-1];
    }
    Fixture() {
        dword(p.main+4,p.overworld|1);
        if(p.indirect_save) dword(p.save_block,0x02010000);
        const auto save=memory().save(); word(save,10);word(save+2,10);
        dword(p.map_header,0x08000100);
        dword(0x08000100,50);dword(0x08000104,50);
        dword(0x08000108,0x08000200);dword(0x0800010C,0x08010000);
        dword(0x08000110,0x08000300);dword(0x08000114,0x08000320);
        word(0x08000118,0x0304); // FRLG 4x3 authored border
        for(int i=0;i<12;++i)word(0x08000200+i*2,i+1);
        const unsigned primary=p.frlg?640:512;
        for(unsigned n=0;n<1024;++n) {
            const unsigned set=n>=primary, index=set?n-primary:n;
            const unsigned tiles=0x08001000+set*0x3000, attrs=0x08008000+set*0x1000;
            dword(0x0800030C+set*32,tiles);
            dword(0x08000300+set*32+(p.frlg?20:16),attrs);
            for(unsigned q=0;q<8;++q)word(tiles+index*16+q*2,(n*8+q)%1000+1);
            if(p.frlg)dword(attrs+index*4,(n%3)<<29);else word(attrs+index*2,(n%3)<<12);
        }
        dword(p.backup_map,64);dword(p.backup_map+4,64);dword(p.backup_map+8,0x02000000);
        for(int y=0;y<64;++y)for(int x=0;x<64;++x)word(0x02000000+(y*64+x)*2,id(x,y));
        reg(0,0x1F40);
        for(int bg=0;bg<4;++bg) reg(8+bg*2,bg*0x100);
        publish();
    }
    void publish() {
        for(int bg=1;bg<=3;++bg)for(int y=0;y<32;++y)for(int x=0;x<32;++x)
            tile(bg,x,y,entry(bg,20+x,20+y));
    }
};

int main() {
    Fixture f;
    gen3::FieldView field;
    check(field.prepare(f.memory(),569)==gen3::ViewStatus::Ready,"landscape camera verification");
    check(field.compared()==210 && field.matched()==210,"all native probes must match");
    const auto ew=f.ew,iw=f.iw,rom=f.rom,vr=f.vr;
    for(const auto& size: {std::pair{284,160}, {373,160}, {569,160}, {240,300}, {240,427}, {240,533}, {240,854}, {301,161}}) {
        check(field.prepare(f.memory(),size.first,size.second)==gen3::ViewStatus::Ready,"adaptive size failed");
        for(int bg=1;bg<=3;++bg)for(int y=0;y<160;y+=8)for(int x=0;x<240;x+=8) {
            std::uint16_t actual=0;
            check(field.tile(bg,x,y,&actual) && actual==f.entry(bg,20+x/8,20+y/8),"native map sampling changed");
        }
        std::uint16_t value=0;
        const int left=(size.first-240)/2,top=(size.second-160)/2;
        check(field.tile(3,-left,-top,&value),"missing expanded corner");
        check(!field.tile(3,-left-1,-top,&value),"accepted outside viewport");
    }
    check(f.ew==ew && f.iw==iw && f.rom==rom && f.vr==vr,"presentation wrote guest state");
    // Tall door lower halves consume palette bytes 8..11 in the published
    // covered layer, including the ROM's authored adjacent palette bytes.
    f.word(f.p.doors,f.id(10,10));f.word(f.p.doors+2,0x0100);
    f.dword(f.p.doors+8,0x08000B00);
    f.word(0x08000B04,10);f.word(0x08000B08,11);
    f.tile(1,0,0,0);f.tile(2,0,0,0xB000);f.tile(3,0,0,0xA3FC);
    check(field.prepare(f.memory(),569)==gen3::ViewStatus::Ready,"published tall door rejected");
    std::uint16_t door=0;
    check(field.tile(3,0,0,&door) && door==0xA3FC,"published door tile not retained");
    f.publish();
    f.vr[0x800]^=1;
    check(field.prepare(f.memory(),569)==gen3::ViewStatus::Unverified,"unpublished tiles must fall back");
    f.publish(); f.dword(f.p.main+4,0x08000001);
    check(field.prepare(f.memory(),569)==gen3::ViewStatus::NonField,"battle/menu must stay native");
    f.dword(f.p.main+4,f.p.overworld|1);f.dword(f.p.backup_map+8,0x0203FFFF);
    check(field.prepare(f.memory(),569)==gen3::ViewStatus::Unsupported,"invalid map pointer accepted");
    f.dword(f.p.backup_map+8,0x02000000);

    // Extend beyond the seven-row live connection strip using the neighbor's
    // ROM layout. Live connection cells must still override ROM data.
    f.dword(0x08000104,12);
    f.dword(f.p.map_header+12,0x08000400);
    f.dword(0x08000400,1);f.dword(0x08000404,0x08000420);
    f.dword(0x08000420,1);f.dword(0x08000424,0);f.word(0x08000428,0x0100);
    f.dword(f.p.map_groups,0x08000500);f.dword(0x08000504,0x08000600);
    f.dword(0x08000600,0x08000640);
    f.dword(0x08000640,50);f.dword(0x08000644,30);f.dword(0x0800064C,0x08011000);
    f.dword(0x08000650,0x08000300);f.dword(0x08000654,0x08000320);
    for(int i=0;i<1500;++i)f.word(0x08011000+i*2,700);
    for(int y=32;y<64;++y)for(int x=0;x<64;++x)f.word(0x02000000+(y*64+x)*2,0x3FF);
    check(field.prepare(f.memory(),240,854)==gen3::ViewStatus::Ready && field.map_count()==2,"connected map traversal failed");
    std::uint16_t connected=0;
    check(field.tile(3,0,408,&connected) && connected==((700*8+2)%1000+1),"distant connection substituted border");
    // Undo connection fixture before live-object and UI checks.
    f.dword(0x08000104,50);f.dword(f.p.map_header+12,0);

    // A published software object crossing the left native boundary retains
    // its VRAM/palette pixels; explicit invisibility suppresses it.
    check(field.prepare(f.memory(),569)==gen3::ViewStatus::Ready,"object fixture field");
    auto* obj=const_cast<std::uint8_t*>(f.memory().bytes(f.p.objects,36));
    auto* spr=const_cast<std::uint8_t*>(f.memory().bytes(f.p.sprites,68));
    obj[0]=1;obj[4]=0;spr[62]=1;
    f.word(f.p.sprites,20);f.word(f.p.sprites+2,508);f.word(f.p.sprites+4,0x2000);
    f.word(f.p.sprites+32,0xFFFC);f.word(f.p.sprites+34,20);
    f.oa[0]=20;f.oa[2]=252;f.oa[3]=1;f.oa[5]=32;
    std::fill(f.vr.begin()+0x10000,f.vr.begin()+0x10020,0x11);f.pa[0x242]=31;
    gen3::ObjectView objects;
    check(objects.prepare(f.memory(),field,569) && objects.verified_parts()==1,"live NPC verification failed");
    int left=0,width=0;auto* row=objects.row(20,&left,&width);
    check(row && row[-4-left].color==31,"live margin NPC missing");
    f.dword(f.p.sprites+24,0x08000C00);spr[66]=0x40;
    f.word(0x08000C00,1);f.dword(0x08000C04,0x08000C20);
    if(f.p.frlg) { f.word(0x08000C20,0xFC00);f.word(0x08000C22,0x8000); }
    else { f.word(0x08000C20,0);f.word(0x08000C22,0xFFFC);f.word(0x08000C24,0x8000); }
    f.oa[0]=16;f.oa[5]=0x28;
    check(objects.prepare(f.memory(),field,569) && objects.verified_parts()==1,"family subsprite coordinate layout failed");
    row=objects.row(16,&left,&width);check(row && row[-4-left].color==31,"subsprite margin pixels missing");
    f.dword(0x08000C04,0);f.oa[0]=20;f.oa[5]=0x20;
    check(objects.prepare(f.memory(),field,569) && objects.verified_parts()==1,"null subsprite table must retain full OAM");
    obj[1]=0x20;
    check(objects.prepare(f.memory(),field,569),"hidden object prepare");
    row=objects.row(20,&left,&width);check(row && (row[-4-left].color&0x8000),"script-hidden actor revealed");

    // A dormant neighboring actor uses its ROM idle pose and resident palette;
    // its story flag and live counterpart must suppress that presentation.
    obj[0]=0; f.dword(f.p.map_header+4,0x08000700);f.word(0x08000700,1);
    const auto save=f.memory().save(), templ=save+f.p.templates_offset;
    auto* t=const_cast<std::uint8_t*>(f.memory().bytes(templ,24));
    t[0]=1;t[1]=1;f.word(templ+4,0);f.word(templ+6,10);
    f.dword(f.p.graphics+4,0x08000800);f.word(0x08000802,0x1100);
    f.word(0x08000808,16);f.word(0x0800080A,16);f.word(0x0800080C,2);
    f.dword(f.p.palette_sets,0x08000900);f.word(0x08000904,0x1100);
    f.dword(0x08000818,0x08000920);f.dword(0x08000920,0x08000940);f.word(0x08000940,0);
    f.dword(0x0800081C,0x08000960);f.dword(0x08000960,0x08000A00);f.word(0x08000964,128);
    for(int i=0;i<128;i+=2)f.word(0x08000A00+i,0x1111);
    f.word(f.p.facing,1);f.word(f.p.face_anims,0);f.word(f.p.priorities,2);
    check(field.prepare(f.memory(),569)==gen3::ViewStatus::Ready,"dormant fixture field");
    check(objects.prepare(f.memory(),field,569) && objects.dormant_objects()==1,"unloaded NPC missing");
    f.word(templ+20,8);f.word(save+f.p.flags_offset,0x100);
    check(objects.prepare(f.memory(),field,569) && objects.dormant_objects()==0,"story-hidden dormant actor revealed");

    // Move the live Start menu and dialogue frames using each family's UI.
    obj[0]=0;gen3::UiView ui;
    if(f.p.frlg) {
        auto* w=const_cast<std::uint8_t*>(f.memory().bytes(f.p.windows,12));
        w[0]=0;w[1]=22;w[2]=1;w[3]=7;w[4]=12;w[5]=14;
        f.word(f.p.windows+6,200);f.dword(f.p.windows+8,0x02010000);
        for(int y=0;y<12;++y)for(int x=0;x<7;++x)f.tile(0,x+22,y+1,0xE000+200+y*7+x);
    } else {
        f.word(f.p.standard_frame,200);
        for(int y=0;y<=14;++y)for(int x=22;x<=29;++x) {
            const unsigned n=y==0?(x==22?0:x==29?2:1):y==14?(x==22?6:x==29?8:7):x==22?3:x==29?5:4;
            f.tile(0,x,y,0xE000+200+n);
        }
    }
    ui.prepare(f.memory(),field,569,160);
    int sx=0,sy=0;
    check(ui.sample(0,180+165,16,&sx,&sy)==1 && sx==180 && sy==16,"Start menu not anchored at right edge");
    check(ui.sample(0,180,16,&sx,&sy)==-1,"original UI rectangle not removed");
    if(!f.p.frlg) {
        std::fill(f.vr.begin(),f.vr.begin()+0x800,0);
        f.word(f.p.dialogue_frame,300);
        f.tile(0,0,14,301);f.tile(0,29,14,306);f.tile(0,0,19,0x800|301);f.tile(0,29,19,0x800|306);
        ui.prepare(f.memory(),field,240,427);
        check(ui.sample(0,100,120+134,&sx,&sy)==1 && sy==120,"RS dialogue not anchored at bottom");
    }
    std::printf("%s adaptive terrain, native isolation, invalid input, live NPC and UI checks passed\n",f.p.name);
}
