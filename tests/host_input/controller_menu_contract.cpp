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
        const SDL_GameControllerButton defaults[] = {
            SDL_CONTROLLER_BUTTON_DPAD_UP, SDL_CONTROLLER_BUTTON_DPAD_DOWN,
            SDL_CONTROLLER_BUTTON_DPAD_LEFT, SDL_CONTROLLER_BUTTON_DPAD_RIGHT,
            SDL_CONTROLLER_BUTTON_A, SDL_CONTROLLER_BUTTON_B,
            SDL_CONTROLLER_BUTTON_LEFTSHOULDER, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER,
            SDL_CONTROLLER_BUTTON_START, SDL_CONTROLLER_BUTTON_BACK};
        for (int i = 0; i < 10; ++i) {
            check((button(defaults[i], 1).keyinput & (1u << kRuiGbaKeyBits[i])) == 0,
                  "every default controller button reaches its GBA input");
            button(defaults[i], 0);
        }
        RecompLauncherCGameInfo game{};launcher_profile_apply_gba(&game);
        check(game.has_integer_scale && game.has_scanlines,
              "GBA profile exposes implemented presentation controls");
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
#if defined(RECOMP_LAUNCHER_HAS_LAUNCH_OPTIONS)
        {
            gbarecomp::RunOptions launch_opts;
            launch_opts.launcher_game_version = "v0.0.0-fixture";
            std::vector<std::string> arguments{fixture_dir + "/fixture.exe", "--no-launcher"};
            check(gbarecomp_launcher_preboot(arguments, launch_opts) == 0,
                  "one-shot skip boots without opening a launcher or picker");
            check(std::find(arguments.begin(), arguments.end(), "--no-launcher") == arguments.end() &&
                  std::find(arguments.begin(), arguments.end(), "73") != arguments.end(),
                  "one-shot skip consumes switch and honors saved settings");
            check(std::string(recomp_launcher_get_game_version()) == "v0.0.0-fixture",
                  "title version reaches shared launcher metadata");
            gbarecomp_seam::seam_config_load(config_path, &loaded_profile);
            check(loaded_profile.skip_launcher == 0, "one-shot skip preserves saved launcher preference");
            check(recomp_launcher_cli_should_skip(RECOMP_LAUNCHER_CLI_SHOW, 1) == 0 &&
                  recomp_launcher_cli_should_skip(RECOMP_LAUNCHER_CLI_DEFAULT, 1) == 1,
                  "force-show and saved-skip remain independent");
        }
#endif
        launcher_binds_load(model,config_path.c_str(),keys_path.c_str());
        launcher_binds_set_pad_button(model,1,5,LNG_PADBIND_BUTTON,SDL_CONTROLLER_BUTTON_X,0);
        check(std::string(model->pad_binds[0][5])=="x","launcher capture displays Xbox X");
        window.load_input_config(fixture_dir.c_str());
        check((button(SDL_CONTROLLER_BUTTON_X,1).keyinput&2u)==0,"captured Xbox X drives GBA B");
        check((button(SDL_CONTROLLER_BUTTON_X,0).keyinput&2u)!=0,"Xbox X release releases GBA B");
        check((button(SDL_CONTROLLER_BUTTON_B,1).keyinput&2u)!=0,"former Xbox B mapping is removed");button(SDL_CONTROLLER_BUTTON_B,0);
        launcher_binds_set_pad_button(model,1,5,LNG_PADBIND_AXIS,SDL_CONTROLLER_AXIS_LEFTX,1);
        window.load_input_config(fixture_dir.c_str());
        SDL_JoystickSetVirtualAxis(pad,SDL_CONTROLLER_AXIS_LEFTX,3000);pump();
        check((pump().keyinput&2u)!=0,"axis remap honors deadzone");
        SDL_JoystickSetVirtualAxis(pad,SDL_CONTROLLER_AXIS_LEFTX,24000);pump();
        check((pump().keyinput&2u)==0,"positive axis remap drives GBA B");
        SDL_JoystickSetVirtualAxis(pad,SDL_CONTROLLER_AXIS_LEFTX,-24000);pump();
        check((pump().keyinput&2u)!=0,"opposite axis direction does not drive GBA B");
        SDL_JoystickSetVirtualAxis(pad,SDL_CONTROLLER_AXIS_LEFTX,0);pump();
        launcher_binds_set_pad_button(model,1,5,LNG_PADBIND_NONE,0,0);
        window.load_input_config(fixture_dir.c_str());
        check((button(SDL_CONTROLLER_BUTTON_X,1).keyinput&2u)!=0,"unbinding removes former mapped source");
        button(SDL_CONTROLLER_BUTTON_X,0);
        launcher_binds_set_pad_button(model,1,5,LNG_PADBIND_BUTTON,SDL_CONTROLLER_BUTTON_X,0);
#if defined(RUI_GBA_PAD_BIND_SLOTS) && RUI_GBA_PAD_BIND_SLOTS >= 2
        launcher_binds_set_pad_button_slot(model,1,5,1,LNG_PADBIND_BUTTON,SDL_CONTROLLER_BUTTON_Y,0);
        check(std::string(model->pad_binds[0][5])=="x" &&
              std::string(model->pad_binds_alt[0][5])=="y","primary and alternate labels are independent");
        window.load_input_config(fixture_dir.c_str());
        check((button(SDL_CONTROLLER_BUTTON_Y,1).keyinput&2u)==0,"alternate button drives GBA B");
        button(SDL_CONTROLLER_BUTTON_X,1);
        check((button(SDL_CONTROLLER_BUTTON_Y,0).keyinput&2u)==0,"releasing alternate keeps held primary active");
        check((button(SDL_CONTROLLER_BUTTON_X,0).keyinput&2u)!=0,"both buttons released releases GBA B");
        button(SDL_CONTROLLER_BUTTON_Y,1);button(SDL_CONTROLLER_BUTTON_X,1);
        check((button(SDL_CONTROLLER_BUTTON_X,0).keyinput&2u)==0,"releasing primary keeps held alternate active");
        button(SDL_CONTROLLER_BUTTON_Y,0);
        launcher_binds_set_pad_button_slot(model,1,5,1,LNG_PADBIND_AXIS,SDL_CONTROLLER_AXIS_TRIGGERLEFT,1);
        check(std::string(model->pad_binds_alt[0][5])=="lefttrigger+","trigger alternative is captured");
        rui_pad_binds_init(&kRuiGbaPadSpec,bindings.c_str());
        launcher_binds_refresh(model);
        check(std::string(model->pad_binds_alt[0][5])=="lefttrigger+","trigger alternative persists after store reload");
        window.load_input_config(fixture_dir.c_str());
        // SDL maps the virtual joystick's signed trigger axis to [0,32767].
        SDL_JoystickSetVirtualAxis(pad,SDL_CONTROLLER_AXIS_TRIGGERLEFT,-28000);pump();
        check((pump().keyinput&2u)!=0,"trigger alternative honors deadzone");
        SDL_JoystickSetVirtualAxis(pad,SDL_CONTROLLER_AXIS_TRIGGERLEFT,24000);pump();
        check((pump().keyinput&2u)==0,"trigger alternative drives GBA B");
        button(SDL_CONTROLLER_BUTTON_X,1);
        SDL_JoystickSetVirtualAxis(pad,SDL_CONTROLLER_AXIS_TRIGGERLEFT,-32768);pump();
        check((pump().keyinput&2u)==0,"trigger release preserves held button");
        check((button(SDL_CONTROLLER_BUTTON_X,0).keyinput&2u)!=0,"trigger and button both released");
        launcher_binds_set_pad_button_slot(model,1,5,1,LNG_PADBIND_NONE,0,0);
        window.load_input_config(fixture_dir.c_str());
        SDL_JoystickSetVirtualAxis(pad,SDL_CONTROLLER_AXIS_TRIGGERLEFT,24000);pump();
        check((pump().keyinput&2u)!=0,"clearing alternate removes trigger without changing primary");
        SDL_JoystickSetVirtualAxis(pad,SDL_CONTROLLER_AXIS_TRIGGERLEFT,-32768);pump();
        launcher_binds_set_pad_button_slot(model,1,2,1,LNG_PADBIND_AXIS,SDL_CONTROLLER_AXIS_LEFTX,-1);
        launcher_binds_set_pad_button_slot(model,1,3,1,LNG_PADBIND_AXIS,SDL_CONTROLLER_AXIS_LEFTX,1);
        window.load_input_config(fixture_dir.c_str());
        SDL_JoystickSetVirtualAxis(pad,SDL_CONTROLLER_AXIS_LEFTX,-24000);pump();
        check((pump().keyinput&0x30u)==0x10u,"negative stick drives left without right");
        SDL_JoystickSetVirtualAxis(pad,SDL_CONTROLLER_AXIS_LEFTX,24000);pump();
        check((pump().keyinput&0x30u)==0x20u,"positive stick drives right without left");
        SDL_JoystickSetVirtualAxis(pad,SDL_CONTROLLER_AXIS_LEFTX,0);pump();
        check((pump().keyinput&0x30u)==0x30u,"stick center releases both directions");
#endif
        window.load_input_config(fixture_dir.c_str());
        delete model;
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
