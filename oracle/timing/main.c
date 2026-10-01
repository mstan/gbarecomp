/* Original test frontend to the upstream mGBA API for the CPU timing fixture
 * (tests/timing/timing_fixture.S). The reference emulator stays in this
 * separate executable; nothing here is linked into the native build.
 *
 *   gba_timing_oracle <rom>
 *   gba_timing_oracle <rom> --swi-log <bios> <frames>
 *
 * Fixture mode boots like the link oracle (skip BIOS, PC = 08000000h), runs
 * frames until the fixture writes its done marker, and prints one
 * "index,cycles" line per measurement, the format tests/timing/timing_test
 * prints for the recompiled fixture.
 *
 * --swi-log boots any cartridge through the real BIOS for <frames> frames
 * and prints "seq,cycles,imm" for every SWI, stamped at the start of the SWI
 * instruction in cycles since reset: the reference for bios_smoke's
 * GBARECOMP_SWI_LOG (oracle/timing/frame_budget.py). */
#include <mgba/flags.h>
#include <mgba/core/core.h>
#include <mgba/core/log.h>
#include <mgba/gba/core.h>
#include <mgba/internal/arm/isa-inlines.h>
#include <mgba/internal/gba/gba.h>
#include <mgba/core/timing.h>
#include <mgba-util/vfs.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RESULTS 0x02020000u
#define MARKER 0x0201FFF0u
#define MARKER_DONE 0x600Du
#define MAX_RESULTS 4096u

static void log_error(struct mLogger* log, int category, enum mLogLevel level, const char* fmt, va_list args) {
    (void)log; (void)category;
    if (getenv("GBA_TIMING_ORACLE_VERBOSE") || level == mLOG_FATAL) { vfprintf(stderr,fmt,args); fputc('\n',stderr); }
}

static void (*g_swi16)(struct ARMCore*, int);
static void (*g_swi32)(struct ARMCore*, int);
static struct mCore* g_core;
static unsigned long g_swi_seq;
static void log_swi(struct ARMCore* cpu, int imm) {
    (void)cpu;
    struct GBA* gba = g_core->board;
    printf("%lu,%u,%d\n", g_swi_seq++, (unsigned)mTimingCurrentTime(&gba->timing), imm);
}
static void swi16_hook(struct ARMCore* cpu, int imm) { log_swi(cpu, imm); g_swi16(cpu, imm); }
static void swi32_hook(struct ARMCore* cpu, int imm) { log_swi(cpu, imm); g_swi32(cpu, imm); }

static int swi_log(struct mCore* core, const char* bios_path, unsigned frames) {
    struct VFile* bios = VFileOpen(bios_path, O_RDONLY);
    if (!bios || !core->loadBIOS(core, bios, 0)) return 3;
    core->reset(core);
    g_core = core;
    struct ARMCore* cpu = core->cpu;
    g_swi16 = cpu->irqh.swi16; g_swi32 = cpu->irqh.swi32;
    cpu->irqh.swi16 = swi16_hook; cpu->irqh.swi32 = swi32_hook;
    printf("seq,cycles,imm\n");
    for (unsigned f = 0; f < frames; ++f) core->runFrame(core);
    return 0;
}

int main(int argc, char** argv) {
    const int swi_mode = argc == 5 && strcmp(argv[2], "--swi-log") == 0;
    if (argc != 2 && !swi_mode) return 2;
    FILE* file = fopen(argv[1],"rb");
    if (!file) return 2;
    fseek(file,0,SEEK_END); long size = ftell(file); rewind(file);
    if (size <= 0 || size > 32*1024*1024) return 2;
    void* rom = malloc(size);
    if (!rom || fread(rom,1,size,file) != (size_t)size) return 2;
    fclose(file);
    struct mLogger logger = {.log=log_error};
    mLogSetDefaultLogger(&logger);
    struct mCore* core = GBACoreCreate();
    if (!core || !core->init(core)) return 3;
    mCoreInitConfig(core,NULL);
    if (!core->loadROM(core,VFileFromConstMemory(rom,size))) return 3;
    if (swi_mode) return swi_log(core, argv[3], (unsigned)atoi(argv[4]));
    core->reset(core);
    GBASkipBIOS(core->board);
    struct ARMCore* cpu = core->cpu;
    cpu->gprs[ARM_PC] = 0x08000000;
    ARMWritePC(cpu);
    unsigned frames = 0;
    while (core->busRead32(core,MARKER) != MARKER_DONE) {
        if (++frames > 600) { fprintf(stderr,"oracle: timing fixture did not finish\n"); return 5; }
        core->runFrame(core);
    }
    /* Results run until the first zero word after the last pass. */
    for (unsigned i = 0; i < MAX_RESULTS; ++i) {
        uint32_t v = core->busRead32(core,RESULTS + 4u*i);
        if (!v) break;
        printf("%u,%u\n",i,v);
    }
    mCoreConfigDeinit(&core->config);
    core->deinit(core);
    free(rom);
    return 0;
}
