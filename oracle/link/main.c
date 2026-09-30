/* Original test frontend to the upstream mGBA API, not an SIO implementation.
 * The reference emulator stays in this separate executable.
 *
 *   gba_link_oracle <rom> [--players N] [--normal | --immediate] [--timing]
 *
 * --players N  2..4 cores on one lockstep coordinator (default 2).
 * --normal     exercise normal-mode serial instead of multiplayer.
 * --immediate  any other extra argument: skip the startup settle delay.
 * --timing     multiplayer only: print one "T,baud,players,span,finish,skew.."
 *              line per baud (see report_timing). Data lines are unchanged. */
#include <mgba/flags.h>
#include <mgba/core/core.h>
#include <mgba/core/log.h>
#include <mgba/core/timing.h>
#include <mgba/gba/core.h>
#include <mgba/internal/arm/isa-inlines.h>
#include <mgba/internal/gba/gba.h>
#include <mgba/internal/gba/sio.h>
#include <mgba/internal/gba/sio/lockstep.h>
#include <mgba-util/vfs.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_PLAYERS 4
struct TestUser { struct mLockstepUser api; bool sleeping; int port; };
static void sleep_node(struct mLockstepUser* p) { ((struct TestUser*)p)->sleeping = true; }
static void wake_node(struct mLockstepUser* p) { ((struct TestUser*)p)->sleeping = false; }
static int requested_id(struct mLockstepUser* p) { return ((struct TestUser*)p)->port; }
static void log_error(struct mLogger* log, int category, enum mLogLevel level, const char* fmt, va_list args) {
    (void)log; (void)category;
    if (getenv("GBA_LINK_ORACLE_VERBOSE") || level == mLOG_FATAL || level == mLOG_ERROR) { vfprintf(stderr,fmt,args); fputc('\n',stderr); }
}

/* Timing probe: wraps the master's driver writeSIOCNT to note the exact core
 * time of the start write, and polls every core's busy bit after each runLoop
 * to read the (exact) scheduled completion time of its SIO event. */
static struct GBASIOLockstepDriver g_drivers[MAX_PLAYERS];
static uint16_t (*g_write_siocnt[MAX_PLAYERS])(struct GBASIODriver*, uint16_t);
static bool g_master_started;
static int32_t g_master_start;
static uint16_t timing_write_siocnt(struct GBASIODriver* driver, uint16_t value) {
    unsigned port = (unsigned)((struct GBASIOLockstepDriver*)driver - g_drivers);
    struct GBASIO* sio = driver->p;
    if (port == 0 && !g_master_started && sio->mode == GBA_SIO_MULTI &&
        (value & 0x80) && !(sio->siocnt & 0x80)) {
        g_master_start = mTimingCurrentTime(&sio->p->timing);
        g_master_started = true;
    }
    return g_write_siocnt[port](driver, value);
}

int main(int argc, char** argv) {
    if (argc < 2) return 2;
    bool normal=false, immediate=false, timing=false;
    unsigned players=2;
    for (int i=2; i<argc; ++i) {
        if (strcmp(argv[i],"--normal")==0) normal=true;
        else if (strcmp(argv[i],"--timing")==0) timing=true;
        else if (strcmp(argv[i],"--players")==0) {
            if (++i>=argc) return 2;
            players=(unsigned)atoi(argv[i]);
            if (players<2 || players>MAX_PLAYERS) return 2;
        } else immediate=true;
    }
    if (timing && normal) return 2;
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
        struct mCore* cores[MAX_PLAYERS];
        struct TestUser users[MAX_PLAYERS] = {0};
        struct GBASIOLockstepCoordinator coordinator;
        int32_t finish[MAX_PLAYERS] = {0};
        bool finish_seen[MAX_PLAYERS] = {false};
        memset(g_drivers,0,sizeof(g_drivers));
        g_master_started = false;
        GBASIOLockstepCoordinatorInit(&coordinator);
        for (unsigned port=0; port<players; ++port) {
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
            /* Normal serial: r0 bit 13 selects the external-clock role in the fixture,
             * so every port after the clock owner sets it (0x4444 would not). */
            cpu->gprs[0] = (port+1)*0x1111 | ((normal && port) ? 0x2000 : 0);
            cpu->gprs[1] = baud;
            cpu->gprs[8] = immediate && !normal ? 0 : 4096;
            cpu->gprs[9] = normal;
            users[port].port = port;
            users[port].api.sleep = sleep_node;
            users[port].api.wake = wake_node;
            users[port].api.requestedId = requested_id;
            GBASIOLockstepDriverCreate(&g_drivers[port],&users[port].api);
            if (timing) {
                g_write_siocnt[port] = g_drivers[port].d.writeSIOCNT;
                g_drivers[port].d.writeSIOCNT = timing_write_siocnt;
            }
            GBASIOLockstepCoordinatorAttach(&coordinator,&g_drivers[port]);
            GBASIOSetDriver(&((struct GBA*)core->board)->sio,&g_drivers[port].d);
        }
        bool done = false;
        for (unsigned step=0; step<2000000; ++step) {
            bool awake = false;
            for (unsigned port=0; port<players; ++port) {
                if (!users[port].sleeping) { cores[port]->runLoop(cores[port]); awake=true; }
                if (timing && !finish_seen[port]) {
                    struct GBASIO* sio = &((struct GBA*)cores[port]->board)->sio;
                    if (sio->mode == GBA_SIO_MULTI && (cores[port]->busRead16(cores[port],0x04000128) & 0x80)) {
                        finish[port] = (int32_t)sio->completeEvent.when;
                        finish_seen[port] = true;
                    }
                }
            }
            if (!awake) { fprintf(stderr,"oracle coordinator deadlock\n"); return 4; }
            done = true;
            for (unsigned port=0; port<players; ++port)
                if (cores[port]->busRead32(cores[port],0x02000010) != 0x42) { done=false; break; }
            if (done) break;
        }
        if (!done) { fprintf(stderr,"oracle fixture did not finish at baud %u\n",baud); return 5; }
        for (unsigned port=0; port<players; ++port) {
            struct mCore* core = cores[port];
            printf("%u,%u,%08x,%08x,%04x,%04x\n",baud,port,
                core->busRead32(core,0x02000000),core->busRead32(core,0x02000004),
                core->busRead32(core,0x02000008),core->busRead32(core,0x0200000c)&0x80);
        }
        if (timing) {
            /* T,baud,players,span,finish,skew1..: span = master completion event
             * time minus the core time of the master's start write (exact, from
             * the driver hook); finish = master completion time in that core's
             * timeline; skewN = port N completion time minus master's. */
            if (!g_master_started || !finish_seen[0]) { fprintf(stderr,"oracle timing probe missed the transfer\n"); return 6; }
            printf("T,%u,%u,%d,%d",baud,players,(int)(finish[0]-g_master_start),(int)finish[0]);
            for (unsigned port=1; port<players; ++port) {
                if (!finish_seen[port]) { fprintf(stderr,"oracle timing probe missed port %u\n",port); return 6; }
                printf(",%d",(int)(finish[port]-finish[0]));
            }
            putchar('\n');
        }
        for (unsigned port=0; port<players; ++port) {
            GBASIOSetDriver(&((struct GBA*)cores[port]->board)->sio,NULL);
            GBASIOLockstepCoordinatorDetach(&coordinator,&g_drivers[port]);
            mCoreConfigDeinit(&cores[port]->config);
            cores[port]->deinit(cores[port]);
        }
        GBASIOLockstepCoordinatorDeinit(&coordinator);
    }
    free(rom);
    return 0;
}
