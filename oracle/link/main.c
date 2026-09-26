/* Original test frontend to the upstream mGBA API, not an SIO implementation.
 * The reference emulator stays in this separate executable. */
#include <mgba/flags.h>
#include <mgba/core/core.h>
#include <mgba/core/log.h>
#include <mgba/gba/core.h>
#include <mgba/internal/arm/isa-inlines.h>
#include <mgba/internal/gba/gba.h>
#include <mgba/internal/gba/sio/lockstep.h>
#include <mgba-util/vfs.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct TestUser { struct mLockstepUser api; bool sleeping; int port; };
static void sleep_node(struct mLockstepUser* p) { ((struct TestUser*)p)->sleeping = true; }
static void wake_node(struct mLockstepUser* p) { ((struct TestUser*)p)->sleeping = false; }
static int requested_id(struct mLockstepUser* p) { return ((struct TestUser*)p)->port; }
static void log_error(struct mLogger* log, int category, enum mLogLevel level, const char* fmt, va_list args) {
    (void)log; (void)category;
    if (getenv("GBA_LINK_ORACLE_VERBOSE") || level == mLOG_FATAL || level == mLOG_ERROR) { vfprintf(stderr,fmt,args); fputc('\n',stderr); }
}
int main(int argc, char** argv) {
    if (argc != 2 && argc != 3) return 2;
    const bool normal=argc==3 && strcmp(argv[2],"--normal")==0;
    FILE* file = fopen(argv[1],"rb");
    if (!file) return 2;
    fseek(file,0,SEEK_END); long size = ftell(file); rewind(file);
    if (size <= 0 || size > 1024*1024) return 2;
    void* rom = malloc(size);
    if (!rom || fread(rom,1,size,file) != (size_t)size) return 2;
    fclose(file);
    struct mLogger logger = {.log=log_error};
    mLogSetDefaultLogger(&logger);
    for (unsigned baud=0; baud<4; ++baud) {
        struct mCore* cores[2];
        struct TestUser users[2] = {0};
        struct GBASIOLockstepDriver drivers[2] = {0};
        struct GBASIOLockstepCoordinator coordinator;
        GBASIOLockstepCoordinatorInit(&coordinator);
        for (unsigned port=0; port<2; ++port) {
            cores[port] = GBACoreCreate();
            struct mCore* core = cores[port];
            if (!core || !core->init(core)) return 3;
            mCoreInitConfig(core,NULL);
            if (!core->loadROM(core,VFileFromConstMemory(rom,size))) return 3;
            core->reset(core);
            GBASkipBIOS(core->board);
            struct ARMCore* cpu = core->cpu;
            cpu->gprs[ARM_PC] = 0x08000000;
            ARMWritePC(cpu);
            cpu->gprs[0] = (port+1)*0x1111;
            cpu->gprs[1] = baud;
            cpu->gprs[8] = argc==3 && !normal ? 0 : 4096;
            cpu->gprs[9] = normal;
            users[port].port = port;
            users[port].api.sleep = sleep_node;
            users[port].api.wake = wake_node;
            users[port].api.requestedId = requested_id;
            GBASIOLockstepDriverCreate(&drivers[port],&users[port].api);
            GBASIOLockstepCoordinatorAttach(&coordinator,&drivers[port]);
            GBASIOSetDriver(&((struct GBA*)core->board)->sio,&drivers[port].d);
        }
        bool done = false;
        for (unsigned step=0; step<2000000; ++step) {
            bool awake = false;
            for (unsigned port=0; port<2; ++port) {
                if (!users[port].sleeping) { cores[port]->runLoop(cores[port]); awake=true; }
            }
            if (!awake) { fprintf(stderr,"oracle coordinator deadlock\n"); return 4; }
            if (cores[0]->busRead32(cores[0],0x02000010) == 0x42 &&
                cores[1]->busRead32(cores[1],0x02000010) == 0x42) { done=true; break; }
        }
        if (!done) { fprintf(stderr,"oracle fixture did not finish at baud %u\n",baud); return 5; }
        for (unsigned port=0; port<2; ++port) {
            struct mCore* core = cores[port];
            printf("%u,%u,%08x,%08x,%04x,%04x\n",baud,port,
                core->busRead32(core,0x02000000),core->busRead32(core,0x02000004),
                core->busRead32(core,0x02000008),core->busRead32(core,0x0200000c)&0x80);
        }
        for (unsigned port=0; port<2; ++port) {
            GBASIOSetDriver(&((struct GBA*)cores[port]->board)->sio,NULL);
            GBASIOLockstepCoordinatorDetach(&coordinator,&drivers[port]);
            mCoreConfigDeinit(&cores[port]->config);
            cores[port]->deinit(cores[port]);
        }
        GBASIOLockstepCoordinatorDeinit(&coordinator);
    }
    free(rom);
    return 0;
}
