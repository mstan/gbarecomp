#define SDL_MAIN_HANDLED
#include <SDL.h>
#include "host_window.h"
#include "recomp_runtime_ui_internal.h"
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include "consoles/gba/gba_pad_binds.h"
#include "launcher_binds.h"
#include "launcher_profile.h"
#include "launcher_seam.h"

#if !SDL_VERSION_ATLEAST(2,24,0)
int main() {return 77;}
#else

int failures = 0;
void check(bool yes, const char* what) { if (!yes) { std::fprintf(stderr,"FAIL: %s\n",what); ++failures; } }
int main(int argc,char** argv) {
    const std::string fixture_dir=argc>1?argv[1]:"controller-contract-fixture";
    std::filesystem::create_directories(fixture_dir);
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
    desc.vendor_id=0xfeed; desc.product_id=0xbeef; desc.name="Virtual Xbox Series X GBA test";
    desc.button_mask=(1u<<SDL_CONTROLLER_BUTTON_MAX)-1u; desc.axis_mask=(1u<<SDL_CONTROLLER_AXIS_MAX)-1u;
    int device=SDL_JoystickAttachVirtualEx(&desc); check(device>=0,"virtual controller attached");
    SDL_Joystick* pad=SDL_JoystickOpen(device); check(pad!=nullptr,"virtual joystick open");
    RecompRuntimeUiItem items[5]{};
    const char* names[]={"One","Two","Three","Four","Five"};
    for (int i=0;i<5;++i) { items[i].key=names[i];items[i].label=names[i];items[i].section=names[i];items[i].type=RECOMP_RUNTIME_UI_ACTION; }
    RecompRuntimeUiConfig config{}; config.title="Menu contract"; config.items=items;config.item_count=5;
    auto* ui=recomp_runtime_ui_create(&config); check(ui!=nullptr,"menu model created");
    if (!ui || !pad || device < 0) return 1;
    {
        gbarecomp::HostWindow window;
        check(window.open(1),"dummy host window open"); window.set_runtime_ui(ui);
        auto pump=[&] { SDL_JoystickUpdate(); SDL_PumpEvents(); return window.pump(); };
        auto button=[&](int b,int down) { check(SDL_JoystickSetVirtualButton(pad,b,down)==0,"virtual button set");return pump(); };
        const std::string bindings=fixture_dir+"/input.ini";
        char guid[40];SDL_JoystickGetGUIDString(SDL_JoystickGetGUID(pad),guid,sizeof(guid));
        rui_pad_binds_reset(&kRuiGbaPadSpec,bindings.c_str(),guid);
        window.load_input_config(fixture_dir.c_str());
        check((button(SDL_CONTROLLER_BUTTON_A,1).keyinput&1u)==0,"default Xbox A remains GBA A");button(SDL_CONTROLLER_BUTTON_A,0);
        check((button(SDL_CONTROLLER_BUTTON_B,1).keyinput&2u)==0,"default Xbox B remains GBA B");button(SDL_CONTROLLER_BUTTON_B,0);
        RecompLauncherCGameInfo game{};launcher_profile_apply_gba(&game);
        RecompLauncherCSettings settings{};settings.player_src[0]=2;
        std::snprintf(settings.player_gamepad_guid[0],sizeof(settings.player_gamepad_guid[0]),"%s",guid);
        auto* model=new LauncherModel{};
        launcher_model_init(model,&settings,&game,nullptr);
        const std::string config_path=fixture_dir+"/config.ini",keys_path=fixture_dir+"/keybinds.ini";
        gbarecomp_seam::SeamConfig saved_profile;
        saved_profile.player_source=2;saved_profile.player_guid=guid;saved_profile.volume=73;
        gbarecomp_seam::seam_config_save(config_path,saved_profile);
        gbarecomp_seam::SeamConfig loaded_profile;
        gbarecomp_seam::seam_config_load(config_path,&loaded_profile);
        check(loaded_profile.player_source==2 && loaded_profile.player_guid==guid &&
              loaded_profile.volume==73,"launcher profile preserves device and setting");
        launcher_binds_load(model,config_path.c_str(),keys_path.c_str());
        launcher_binds_set_pad_button(model,1,5,LNG_PADBIND_BUTTON,SDL_CONTROLLER_BUTTON_X,0);
        check(std::string(model->pad_binds[0][5])=="x","launcher capture displays Xbox X");
        delete model;
        window.load_input_config(fixture_dir.c_str());
        check((button(SDL_CONTROLLER_BUTTON_X,1).keyinput&2u)==0,"captured Xbox X drives GBA B");
        check((button(SDL_CONTROLLER_BUTTON_X,0).keyinput&2u)!=0,"Xbox X release releases GBA B");
        check((button(SDL_CONTROLLER_BUTTON_B,1).keyinput&2u)!=0,"former Xbox B mapping is removed");button(SDL_CONTROLLER_BUTTON_B,0);
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
    {
        gbarecomp::HostWindow reopened;check(reopened.open(1),"reopened native window");
        reopened.load_input_config(fixture_dir.c_str());
        SDL_JoystickSetVirtualButton(pad,SDL_CONTROLLER_BUTTON_X,1);SDL_JoystickUpdate();SDL_PumpEvents();
        check((reopened.pump().keyinput&2u)==0,"GBA B to Xbox X persists after reopening");
        SDL_JoystickSetVirtualButton(pad,SDL_CONTROLLER_BUTTON_X,0);SDL_JoystickUpdate();SDL_PumpEvents();
        check((reopened.pump().keyinput&2u)!=0,"reopened mapping releases cleanly");
    }
    recomp_runtime_ui_destroy(ui);SDL_JoystickClose(pad);SDL_JoystickDetachVirtual(device);SDL_Quit();
    std::printf("controller menu contract: %d failure(s)\n",failures);return failures?1:0;
}
#endif
