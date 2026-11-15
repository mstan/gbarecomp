#include "launcher_seam.h"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>
#if !defined(RECOMP_LAUNCHER_HAS_NATIVE_SAVE_PATH_PROVIDER)
int main() { return 77; }
#else
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr,"line %d: %s\n",__LINE__,#x); std::exit(1); } } while(0)
static bool enabled;
static int feature_count(void*) { return 1; }
static int feature_get(void*,int index,RecompLauncherCModFeature* out) {
    CHECK(index==0); *out={}; std::strcpy(out->package_id,"fixture.definitive");
    std::strcpy(out->id,"definitive");out->enabled=enabled;return 1;
}
static std::string resolve(std::vector<std::string> args,const gbarecomp::RunOptions& options) {
    std::vector<char*> argv;for(auto& arg:args)argv.push_back(arg.data());
    std::string path,error;
    CHECK(gbarecomp::resolve_native_save_override(argv.size(),argv.data(),options,&path,&error));
    return path;
}
int main(int argc,char** argv) {
    CHECK(argc==2);
    const auto fixture=std::filesystem::absolute(argv[1])/
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    CHECK(!std::filesystem::exists(fixture));std::filesystem::create_directories(fixture/"config");
    const auto config=fixture/"game.toml";
    {std::ofstream file(config);file<<"[game]\ndefault_region = \"usa\"\n[save]\npath = \"base.sav\"\n";}
    {std::ofstream file(fixture/"config/usa.toml");file<<"[save]\npath = \"regional.sav\"\n";}
    gbarecomp::RunOptions options;
    options.builtin_game_name="fixture";
    options.launcher_save_path="declared.sav";
    const std::vector<std::string> args={"fixture.exe","--config",config.string()};
    const auto regional = resolve(args,options);
    CHECK(std::filesystem::u8path(regional).lexically_normal()==
          (fixture/"config/regional.sav").lexically_normal());
    auto cli=args;cli.insert(cli.end(),{"--save","first.sav","--save-path","chosen.sav"});
    CHECK(resolve(cli,options)=="chosen.sav");
    // ROM and save values must not be mistaken for positional config files.
    cli={"fixture.exe","--rom","not-a-config.toml","--save","chosen.sav"};
    CHECK(resolve(cli,options)=="chosen.sav");
    cli={"fixture.exe","--config",(fixture/"missing.toml").string()};
    CHECK(resolve(cli,options)=="declared.sav");
    options.launcher_save_path=nullptr;
    CHECK(resolve(cli,options).empty());
    const gbarecomp::ModNativeProgramProfile profile={"hash",nullptr,"definitive-1-4",
        "fixture.definitive","definitive"};
    options.mod_native_programs=&profile;options.mod_native_program_count=1;
    RecompLauncherCModProvider provider{};provider.feature_count=feature_count;provider.feature_get=feature_get;
    gbarecomp_seam::NativeSaveContext context{&options,&provider,{},{}};
    const auto rom=(fixture/"cart.gba").string();
    enabled=false;CHECK(gbarecomp_seam::native_save_path(&context,rom.c_str())==(fixture/"cart.sav").string());
    enabled=true;CHECK(gbarecomp_seam::native_save_path(&context,rom.c_str())==(fixture/"cart.definitive-1-4.sav").string());
    const auto other=(fixture/"other.gba").string();
    CHECK(gbarecomp_seam::native_save_path(&context,other.c_str())==(fixture/"other.definitive-1-4.sav").string());
    context.explicit_path=resolve(args,options);
    CHECK(gbarecomp_seam::native_save_path(&context,rom.c_str())==context.explicit_path);
    context.explicit_path=resolve({"fixture.exe","--save","player-choice.sav"},options);
    CHECK(std::string(gbarecomp_seam::native_save_path(&context,rom.c_str()))=="player-choice.sav");
    enabled=false;CHECK(std::string(gbarecomp_seam::native_save_path(&context,rom.c_str()))=="player-choice.sav");
    // Remove only the exact files/directories created by this fixture.
    CHECK(std::filesystem::remove(fixture/"config/usa.toml"));CHECK(std::filesystem::remove(fixture/"config"));
    CHECK(std::filesystem::remove(config));CHECK(std::filesystem::remove(fixture));
    std::puts("Production native-save context: staged profile/ROM changes and config/region/CLI overrides PASS");
}
#endif