#define SDL_MAIN_HANDLED
#include <SDL.h>
#include "host_window.h"
#include "recomp_runtime_ui_internal.h"
#include <cstdio>
#include <algorithm>
#include <fstream>
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
    // The same resolved config used by the launcher must reach the runtime,
    // including skip-launcher boots. Host frequency never selects a guest rate.
    const auto audio_config=std::filesystem::path(fixture_dir)/"audio-selection.ini";
    for (const int rate : {32000,32040,44100,48000,32768}) {
        { std::ofstream file(audio_config);file<<"[Sound]\nAudioFreq="<<rate<<"\nKeep=1\n[Launcher]\nvolume=73\n"; }
        gbarecomp_seam::SeamConfig selected;
        gbarecomp_seam::seam_config_load(audio_config.string(),&selected);
        gbarecomp::RunOptions options;
        std::vector<std::string> args;
        gbarecomp_seam::seam_append_setting_args(args,selected,options);
        auto found=std::find(args.begin(),args.end(),"--audio-rate");
        check(found!=args.end() && found+1!=args.end() && std::atoi((found+1)->c_str())==rate,
              "saved host output rate reaches runtime settings transport");
        gbarecomp_seam::seam_config_save(audio_config.string(),selected);
        std::ifstream file(audio_config);std::string saved((std::istreambuf_iterator<char>(file)),{});
        check(saved.find("AudioFreq="+std::to_string(rate))!=std::string::npos &&
              saved.find("Keep=1")!=std::string::npos,
              "saving launcher settings preserves explicit Sound preferences");
    }
    { std::ofstream file(audio_config);file<<"[Sound]\nAudioFreq=0\n"; }
    gbarecomp_seam::SeamConfig fresh;
    gbarecomp_seam::seam_config_load(audio_config.string(),&fresh);
    std::vector<std::string> fresh_args;
    gbarecomp_seam::seam_append_setting_args(fresh_args,fresh,gbarecomp::RunOptions{});
    auto rate_arg=std::find(fresh_args.begin(),fresh_args.end(),"--audio-rate");
    check(rate_arg!=fresh_args.end() && rate_arg+1!=fresh_args.end() && *(rate_arg+1)=="48000",
          "invalid or absent preference uses a supported launcher rate");
    check(std::filesystem::remove(audio_config),"remove own audio setting fixture");
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
        gbarecomp::RunOptions shortcut_profile;
        shortcut_profile.expose_assist_tools = argc > 2 && std::atoi(argv[2]) != 0;
        gbarecomp_seam::seam_assist_metadata(shortcut_profile, game);
        check(game.has_assist_tools == (shortcut_profile.expose_assist_tools ? 1 : 0),
              "shortcut metadata preserves the title Assist capability");
        check(game.assist_binding_count == 2 && game.assist_binding_labels &&
              std::string(game.assist_binding_labels[0]) == "Rewind" &&
              std::string(game.assist_binding_labels[1]) == "Fast-forward",
              "both host shortcut controls are exposed for the actual title profile");
        const int* assist_keys = game.assist_default_key_bind;
        const int* assist_pads = game.assist_default_pad_bind;
        check(game.has_integer_scale && game.has_scanlines,
              "GBA profile exposes implemented presentation controls");
        RecompLauncherCSettings settings{};settings.player_src[0]=2;
        std::snprintf(settings.player_gamepad_guid[0],sizeof(settings.player_gamepad_guid[0]),"%s",guid);
        auto* model=new LauncherModel{};
        launcher_model_init(model,&settings,&game,nullptr);
        check(model->assist_binding_count == 2 && model->assist_binding_labels &&
              model->has_assist_tools == game.has_assist_tools,
              "controller model exposes host controls without enabling optional Assist");
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
            namespace fs = std::filesystem;
            const auto install = fs::absolute(fixture_dir);
            const auto user = install / "player state with spaces";
            const auto other_cwd = install / "different-working-directory";
            fs::create_directories(user / "variants");
            fs::create_directories(other_cwd);
            gbarecomp_seam::SeamConfig alternate;
            alternate.volume = 41;
            gbarecomp_seam::seam_config_save((user / "config.ini").string(), alternate);
            alternate.volume = 52;
            gbarecomp_seam::seam_config_save((user / "variants/player.ini").string(), alternate);
            alternate.volume = 63;
            gbarecomp_seam::seam_config_save((install / "absolute-player.ini").string(), alternate);
            alternate.volume = 95;
            gbarecomp_seam::seam_config_save((other_cwd / "config.ini").string(), alternate);
            const auto sound = [](const fs::path& path, int rate) {
                std::ofstream file(path,std::ios::app);file<<"\n[Sound]\nAudioFreq="<<rate<<"\n";
            };
            sound(user / "config.ini",32000);
            sound(user / "variants/player.ini",32040);
            sound(install / "absolute-player.ini",44100);
            sound(install / "config.ini",48000);
            sound(other_cwd / "config.ini",8000);
            const auto original_cwd = fs::current_path();
            fs::current_path(other_cwd);
            gbarecomp::RunOptions paths;
            const std::string user_text = user.string();
            const std::string absolute_file = (install / "absolute-player.ini").string();
            const auto boots_with_volume = [&](const char* root, const char* filename, int volume, int rate) {
                paths.config_dir = root;
                paths.launcher_config_filename = filename;
                std::vector<std::string> args{(install / "fixture.exe").string(), "--no-launcher"};
                if (gbarecomp_launcher_preboot(args, paths) != 0) return false;
                const auto audio = std::find(args.begin(),args.end(),"--audio-rate");
                return std::find(args.begin(),args.end(),std::to_string(volume)) != args.end() &&
                    audio != args.end() && audio+1 != args.end() && *(audio+1) == std::to_string(rate);
            };
            check(boots_with_volume(user_text.c_str(), nullptr, 41, 32000),
                  "launcher reads explicit player root instead of install or cwd");
            check(boots_with_volume("../player state with spaces", "variants/player.ini", 52, 32040),
                  "relative state root and filename resolve together");
            check(boots_with_volume(user_text.c_str(), absolute_file.c_str(), 63, 44100),
                  "absolute filename override wins over player root");
            check(boots_with_volume(nullptr, nullptr, 73, 48000) && boots_with_volume("", nullptr, 73, 48000),
                  "null and empty roots preserve executable settings from a different cwd");
            fs::current_path(original_cwd);
        }
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
        // Both triggers can replace GBA shoulders while Assist is explicitly
        // unbound. Exercise the same config -> launcher -> runtime transfer.
        launcher_binds_set_pad_button(model,1,6,LNG_PADBIND_AXIS,SDL_CONTROLLER_AXIS_TRIGGERLEFT,1);
        launcher_binds_set_pad_button(model,1,7,LNG_PADBIND_AXIS,SDL_CONTROLLER_AXIS_TRIGGERRIGHT,1);
        auto persist_assist=[&] {
            launcher_model_commit(model,&settings);
            gbarecomp_seam::seam_assist_from_launcher(settings,saved_profile);
            gbarecomp_seam::seam_config_save(config_path,saved_profile);
            gbarecomp_seam::seam_config_load(config_path,&loaded_profile);
            gbarecomp_seam::seam_assist_to_launcher(loaded_profile,settings);
            launcher_model_init(model,&settings,&game,nullptr);
            window.load_input_config(fixture_dir.c_str());
        };
        auto clear_assist=[&] {
            for(int action=0;action<2;++action) {
                launcher_model_begin_assist_capture(model,action,true);
                launcher_model_set_captured_pad(model,0);launcher_model_cancel_capture(model);
                launcher_model_begin_assist_capture(model,action,false);
                launcher_model_set_captured_key(model,0);launcher_model_cancel_capture(model);
            }
        };
        clear_assist();persist_assist();
        check(model->s.assist_pad_bind[0]<=0 && model->s.assist_pad_bind[1]<=0 &&
              model->s.assist_key_bind[0]<=0 && model->s.assist_key_bind[1]<=0,
              "cleared Assist bindings remain unbound after config and launcher reload");
        persist_assist();
        check(loaded_profile.assist_rewind_pad==0 && loaded_profile.assist_fast_pad==0 &&
              loaded_profile.assist_rewind_key==0 && loaded_profile.assist_fast_key==0,
              "reopening and saving retains canonical unbound config values");
        auto trigger=[&](int axis,int value) {
            check(SDL_JoystickSetVirtualAxis(pad,axis,value)==0,"virtual trigger set");
            const auto first=pump();auto event=pump();
            event.rewind=event.rewind || first.rewind;return event;
        };
        trigger(SDL_CONTROLLER_AXIS_TRIGGERRIGHT,-32768);
        trigger(SDL_CONTROLLER_AXIS_TRIGGERLEFT,-32768);
        auto lt=trigger(SDL_CONTROLLER_AXIS_TRIGGERLEFT,24000);
        check((lt.keyinput&0x300u)==0x100u && !lt.rewind && !lt.fast_forward,
              "left trigger drives only GBA L with Assist cleared");
        trigger(SDL_CONTROLLER_AXIS_TRIGGERLEFT,-32768);
        auto rt=trigger(SDL_CONTROLLER_AXIS_TRIGGERRIGHT,24000);
        check((rt.keyinput&0x300u)==0x200u && !rt.rewind && !rt.fast_forward,
              "right trigger drives only GBA R with Assist cleared");
        trigger(SDL_CONTROLLER_AXIS_TRIGGERRIGHT,-32768);
        check((button(SDL_CONTROLLER_BUTTON_LEFTSHOULDER,1).keyinput&0x300u)==0x300u,
              "old left shoulder no longer drives GBA L");button(SDL_CONTROLLER_BUTTON_LEFTSHOULDER,0);
        check((button(SDL_CONTROLLER_BUTTON_RIGHTSHOULDER,1).keyinput&0x300u)==0x300u,
              "old right shoulder no longer drives GBA R");button(SDL_CONTROLLER_BUTTON_RIGHTSHOULDER,0);
        for(int action=0;action<2;++action) {
            launcher_model_begin_assist_capture(model,action,true);
            launcher_model_set_captured_pad(model,RECOMP_LAUNCHER_PAD_BUTTON(
                action==0?SDL_CONTROLLER_BUTTON_LEFTSHOULDER:SDL_CONTROLLER_BUTTON_RIGHTSHOULDER));
            launcher_model_cancel_capture(model);
        }
        persist_assist();
        check(button(SDL_CONTROLLER_BUTTON_LEFTSHOULDER,1).rewind,"rewind remaps to left shoulder");
        button(SDL_CONTROLLER_BUTTON_LEFTSHOULDER,0);
        check(button(SDL_CONTROLLER_BUTTON_RIGHTSHOULDER,1).fast_forward,"fast-forward remaps to right shoulder");
        button(SDL_CONTROLLER_BUTTON_RIGHTSHOULDER,0);
        lt=trigger(SDL_CONTROLLER_AXIS_TRIGGERLEFT,24000);rt=trigger(SDL_CONTROLLER_AXIS_TRIGGERRIGHT,24000);
        check((rt.keyinput&0x300u)==0 && !rt.fast_forward && !lt.rewind,
              "both gameplay triggers stay independent of remapped Assist");
        trigger(SDL_CONTROLLER_AXIS_TRIGGERLEFT,-32768);trigger(SDL_CONTROLLER_AXIS_TRIGGERRIGHT,-32768);
        launcher_model_reset_assist_bindings(model);
        check(model->s.assist_pad_bind[0]==assist_pads[0] && model->s.assist_pad_bind[1]==assist_pads[1] &&
              model->s.assist_key_bind[0]==assist_keys[0] && model->s.assist_key_bind[1]==assist_keys[1],
              "Reset Assist Controls restores declared defaults after clearing and remapping");
        persist_assist();
        check(trigger(SDL_CONTROLLER_AXIS_TRIGGERLEFT,24000).rewind,
              "reset default left trigger rewinds at runtime");
        trigger(SDL_CONTROLLER_AXIS_TRIGGERLEFT,-32768);
        check(trigger(SDL_CONTROLLER_AXIS_TRIGGERRIGHT,24000).fast_forward,
              "reset default right trigger fast-forwards at runtime");
        trigger(SDL_CONTROLLER_AXIS_TRIGGERRIGHT,-32768);
        clear_assist();persist_assist();
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
        for(int i=0;i<2;++i) {
            const int axis=i?SDL_CONTROLLER_AXIS_TRIGGERRIGHT:SDL_CONTROLLER_AXIS_TRIGGERLEFT;
            SDL_JoystickSetVirtualAxis(pad,axis,24000);SDL_JoystickUpdate();SDL_PumpEvents();reopened.pump();
            const auto event=reopened.pump();
            check((event.keyinput&(i?0x100u:0x200u))==0 && !event.fast_forward && !event.rewind,
                  "reopened host retains trigger gameplay mapping without Assist");
            SDL_JoystickSetVirtualAxis(pad,axis,-32768);SDL_JoystickUpdate();SDL_PumpEvents();reopened.pump();
        }
    }
    recomp_runtime_ui_destroy(ui);SDL_JoystickClose(pad);SDL_JoystickDetachVirtual(device);SDL_Quit();
    std::printf("controller menu contract: %d failure(s)\n",failures);return failures?1:0;
}
#endif
