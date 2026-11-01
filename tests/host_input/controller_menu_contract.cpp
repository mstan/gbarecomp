#define SDL_MAIN_HANDLED
#include <SDL.h>
#include "host_window.h"
#include "recomp_runtime_ui_internal.h"
#include <cstdio>
#include <cstdlib>

int failures = 0;
void check(bool yes, const char* what) { if (!yes) { std::fprintf(stderr,"FAIL: %s\n",what); ++failures; } }
int main() {
    SDL_setenv("SDL_VIDEODRIVER","dummy",1);
    SDL_setenv("SDL_AUDIODRIVER","dummy",1);
    SDL_SetHint(SDL_HINT_RENDER_DRIVER,"software");
    SDL_SetHint(SDL_HINT_GAMECONTROLLER_IGNORE_DEVICES_EXCEPT,"0xfeed/0xbeef");
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS,"1");
    SDL_SetMainReady();
    check(SDL_Init(SDL_INIT_GAMECONTROLLER)==0,"SDL init");
    SDL_VirtualJoystickDesc desc{};
    desc.version=SDL_VIRTUAL_JOYSTICK_DESC_VERSION; desc.type=SDL_JOYSTICK_TYPE_GAMECONTROLLER;
    desc.naxes=SDL_CONTROLLER_AXIS_MAX; desc.nbuttons=SDL_CONTROLLER_BUTTON_MAX;
    desc.vendor_id=0xfeed; desc.product_id=0xbeef; desc.name="GBA menu test";
    desc.button_mask=(1u<<SDL_CONTROLLER_BUTTON_MAX)-1u; desc.axis_mask=(1u<<SDL_CONTROLLER_AXIS_MAX)-1u;
    int device=SDL_JoystickAttachVirtualEx(&desc); check(device>=0,"virtual controller attached");
    SDL_Joystick* pad=SDL_JoystickOpen(device); check(pad!=nullptr,"virtual joystick open");
    RecompRuntimeUiItem items[5]{};
    const char* names[]={"One","Two","Three","Four","Five"};
    for (int i=0;i<5;++i) { items[i].key=names[i];items[i].label=names[i];items[i].section=names[i];items[i].type=RECOMP_RUNTIME_UI_ACTION; }
    RecompRuntimeUiConfig config{}; config.title="Menu contract"; config.items=items;config.item_count=5;
    auto* ui=recomp_runtime_ui_create(&config); check(ui!=nullptr,"menu model created");
    {
        gbarecomp::HostWindow window;
        check(window.open(1),"dummy host window open"); window.set_runtime_ui(ui);
        auto pump=[&] { SDL_JoystickUpdate(); SDL_PumpEvents(); return window.pump(); };
        auto button=[&](int b,int down) { check(SDL_JoystickSetVirtualButton(pad,b,down)==0,"virtual button set");return pump(); };
        pump();
        button(SDL_CONTROLLER_BUTTON_LEFTSTICK,1); check(recomp_runtime_ui_is_open(ui),"L3 opens");button(SDL_CONTROLLER_BUTTON_LEFTSTICK,0);
        button(SDL_CONTROLLER_BUTTON_DPAD_DOWN,1);check(ui->section_index==1,"D-pad navigates");
        SDL_Delay(400);pump();check(ui->section_index==2,"held direction repeats");
        button(SDL_CONTROLLER_BUTTON_DPAD_DOWN,0);auto stopped=ui->section_index;
        SDL_Delay(120);pump();check(ui->section_index==stopped,"release stops repeat");
        SDL_JoystickSetVirtualAxis(pad,SDL_CONTROLLER_AXIS_LEFTY,32767);pump();check(ui->section_index==3,"left stick navigates");
        SDL_JoystickSetVirtualAxis(pad,SDL_CONTROLLER_AXIS_LEFTY,0);pump();
        button(SDL_CONTROLLER_BUTTON_RIGHTSTICK,1);check(!recomp_runtime_ui_is_open(ui),"R3 closes");button(SDL_CONTROLLER_BUTTON_RIGHTSTICK,0);
        button(SDL_CONTROLLER_BUTTON_BACK,1);button(SDL_CONTROLLER_BUTTON_START,1);check(recomp_runtime_ui_is_open(ui),"Select+Start opens");
        button(SDL_CONTROLLER_BUTTON_START,0);button(SDL_CONTROLLER_BUTTON_LEFTSTICK,1);
        check(!recomp_runtime_ui_is_open(ui),"L3 closes while chord held");
        check((pump().keyinput & 0x0cu)==0x0cu,"chord stays withheld after menu closes");
        button(SDL_CONTROLLER_BUTTON_LEFTSTICK,0);button(SDL_CONTROLLER_BUTTON_BACK,0);
        check((button(SDL_CONTROLLER_BUTTON_START,1).keyinput & 8u)==0,"ordinary Start restored after chord release");button(SDL_CONTROLLER_BUTTON_START,0);
        button(SDL_CONTROLLER_BUTTON_LEFTSTICK,1);button(SDL_CONTROLLER_BUTTON_LEFTSTICK,0);
        button(SDL_CONTROLLER_BUTTON_DPAD_DOWN,1);auto editing_row=ui->section_index; ui->editing_text=1;
        SDL_Delay(400);pump();check(ui->section_index==editing_row,"text editing suppresses held navigation");
        button(SDL_CONTROLLER_BUTTON_DPAD_DOWN,0);
        button(SDL_CONTROLLER_BUTTON_LEFTSTICK,1);check(!recomp_runtime_ui_is_open(ui),"L3 closes while text editing");
        ui->editing_text=0;
        window.set_runtime_ui(nullptr);
    }
    recomp_runtime_ui_destroy(ui);SDL_JoystickClose(pad);SDL_JoystickDetachVirtual(device);SDL_Quit();
    std::printf("controller menu contract: %d failure(s)\n",failures);return failures?1:0;
}
