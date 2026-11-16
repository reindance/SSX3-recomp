/* Nintendo Switch start-up and exit (docs/SWITCH.md).
 *
 * Layout on the SD card (sdmc:, the default device):
 *
 *   /switch/ssx3/ssx3_recomp.nro   this program (hbmenu lists it)
 *   /switch/ssx3/game/             your rip: default.xbe + data/
 *   /switch/ssx3/env.txt           optional RECOMP_* settings, one KEY=VALUE per line
 *   /switch/ssx3/ssx3.log          the session log (stderr), rewritten every run
 *   /switch/ssx3/saves/, system/   saves (UDATA/TDATA) and the emulated hard disk
 *
 * Start it from hbmenu in full-memory (title) mode -- hold R while starting
 * a game -- not from the album applet, which leaves too little memory.
 */
#ifdef __SWITCH__
#include <switch.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define NX_DIR "/switch/ssx3"

extern Result ssx3NxHeapBudgetResult;
extern size_t ssx3NxHeapOriginalSize, ssx3NxHeapActualSize;

/* Every exit -- the title's own (HalReturnToFirmware -> ExitProcess ->
 * exit) and main returning -- ends the process here, like closing the
 * application from the HOME menu. Returning to hbmenu instead would mean
 * stopping every guest, device and timer thread and unmapping the guest
 * arena under them first; the kernel reclaims all of it on process exit. */
static void nx_exit(void)
{
    fprintf(stderr, "[SWITCH] exiting\n");
    fflush(stdout);
    fflush(stderr);
    svcExitProcess();
}

/* RECOMP_* settings without a launcher: KEY=VALUE lines, # comments. They
 * override the defaults below. */
static void load_env_file(const char *path)
{
    char line[512];
    FILE *f = fopen(path, "r");
    int n = 0;

    if (!f)
        return;
    while (fgets(line, sizeof line, f)) {
        char *eq, *end = line + strlen(line);
        while (end > line && (end[-1] == '\n' || end[-1] == '\r' || end[-1] == ' '))
            *--end = 0;
        if (line[0] == '#' || !(eq = strchr(line, '=')) || eq == line)
            continue;
        *eq = 0;
        setenv(line, eq + 1, 1);
        fprintf(stderr, "[SWITCH] env.txt: %s=%s\n", line, eq + 1);
        n++;
    }
    fclose(f);
    if (!n)
        fprintf(stderr, "[SWITCH] env.txt: no settings\n");
}

void ssx3_SwitchSetup(void)
{
    mkdir(NX_DIR, 0777);
    mkdir(NX_DIR "/saves", 0777);
    mkdir(NX_DIR "/system", 0777);
    freopen(NX_DIR "/ssx3_stdout.log", "w", stdout);
    freopen(NX_DIR "/ssx3.log", "w", stderr);
    setvbuf(stderr, NULL, _IONBF, 0);   /* nothing lost in a crash */

    fprintf(stderr, "[SWITCH] heap budget 0x%08X: %zu MB (loader gave %zu MB)\n",
            (unsigned)ssx3NxHeapBudgetResult, ssx3NxHeapActualSize >> 20,
            ssx3NxHeapOriginalSize >> 20);
    {
        u64 total = 0, used = 0;
        svcGetInfo(&total, InfoType_TotalMemorySize, CUR_PROCESS_HANDLE, 0);
        svcGetInfo(&used, InfoType_UsedMemorySize, CUR_PROCESS_HANDLE, 0);
        fprintf(stderr, "[SWITCH] process memory %llu MB, %llu MB used%s\n",
                (unsigned long long)(total >> 20), (unsigned long long)(used >> 20),
                total < (2048ull << 20)
                    ? " -- applet mode? start from a game with R held" : "");
    }

    /* The address-space regions the guest arena (4 GB, toolkit
     * platform/switch_memory.inc) must fit between. */
    {
        static const struct { const char *name; u32 addr, size; } regions[] = {
            { "aslr",  InfoType_AslrRegionAddress,  InfoType_AslrRegionSize },
            { "heap",  InfoType_HeapRegionAddress,  InfoType_HeapRegionSize },
            { "alias", InfoType_AliasRegionAddress, InfoType_AliasRegionSize },
            { "stack", InfoType_StackRegionAddress, InfoType_StackRegionSize },
        };
        for (unsigned i = 0; i < sizeof regions / sizeof regions[0]; i++) {
            u64 a = 0, s = 0;
            svcGetInfo(&a, regions[i].addr, CUR_PROCESS_HANDLE, 0);
            svcGetInfo(&s, regions[i].size, CUR_PROCESS_HANDLE, 0);
            fprintf(stderr, "[SWITCH] %-5s region 0x%010llX-0x%010llX (%llu MB)\n", regions[i].name,
                    (unsigned long long)a, (unsigned long long)(a + s), (unsigned long long)(s >> 20));
        }
    }

    load_env_file(NX_DIR "/env.txt");
    setenv("RECOMP_GAME_DIR", NX_DIR "/game", 0);
    setenv("RECOMP_SAVE_DIR", NX_DIR "/saves", 0);
    setenv("RECOMP_SYSTEM_DIR", NX_DIR "/system", 0);
    /* No renderer backend yet: the toolkit's software rasteriser at the
     * original 640x480 4:3 (a widened camera would be squeezed). */
    setenv("RECOMP_GPU", "sw", 0);
    setenv("RECOMP_RES", "480", 0);
    setenv("RECOMP_WIDESCREEN", "0", 0);

    atexit(nx_exit);
}

/* Run the game on a thread with a deep stack: the guest's call chains are
 * native call chains, and the loader's main thread stack is small. */
struct nx_run { int (*fn)(void); int rc; };

static void *nx_run_thread(void *p)
{
    struct nx_run *r = p;
    r->rc = r->fn();
    return NULL;
}

int ssx3_SwitchRun(int (*fn)(void))
{
    struct nx_run r = { fn, 1 };
    pthread_attr_t attr;
    pthread_t th;

    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 32u << 20);
    if (pthread_create(&th, &attr, nx_run_thread, &r) != 0) {
        fprintf(stderr, "[SWITCH] could not start the game thread; running on the main thread\n");
        return fn();
    }
    pthread_attr_destroy(&attr);
    pthread_join(th, NULL);
    return r.rc;
}
#endif
