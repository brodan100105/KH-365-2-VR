/* MobiClip movies, skipped (exclude.txt drops the decomp's versions of these four): the decoder's
 * core is still ARM assembly the port cannot run, and a movie that fails to open leaves the
 * opening scene waiting forever. Instead every movie opens, reports all of its frames played and
 * ends on the first tick, so the opening scene and the cutscene player both move straight on.
 *
 * The real player also calls the request's frame callback once per vertical blank, from the task
 * Ov024_MobiClip_PresentOnVBlank; the scenes depend on it (the opening's runs its task queue and
 * clears the flag its script waits on), so the skipped movie keeps that task, minus the frames. */
#include "khvr_host.h"

struct MobiClipOpenRequest {
    const char *pszStream0;
    const char *pszStream1;
    const char *pszStream2;
    void *pfnFrameReady;
};

extern char gOv024MobiclipIntrName[];
extern void RegisterNamedTask(int nSlot, void *pTable, void *pfn);
extern void VBlank_UnregisterCallback(int nSlot, const char *pszName);

static void (*sFrameReady)(void);

static int sPresented;

static void PresentNothingOnVBlank(void)
{
    if (sPresented++ == 0) khvr_host_print("movie: frame callback running");
    if (sFrameReady) sFrameReady();
}

/* Far past any movie's length, small enough that the callers' arithmetic on it cannot overflow. */
#define KHVR_MOVIE_DONE_FRAMES 0x00ffffff

static void Name(const char *path) {
    if (path) khvr_host_print(path);
}

int Ov024_MobiClip_OpenStreams(struct MobiClipOpenRequest *pRequest)
{
    khvr_host_print("movie skipped (MobiClip playback is not in the port yet):");
    Name(pRequest->pszStream0);
    Name(pRequest->pszStream1);
    Name(pRequest->pszStream2);
    sFrameReady = (void (*)(void))pRequest->pfnFrameReady;
    sPresented = 0;
    RegisterNamedTask(1, gOv024MobiclipIntrName, (void *)&PresentNothingOnVBlank);
    return 1;
}

unsigned int Ov024_TickStreamSlots(void) { return 1; } /* nonzero: the streams have ended */

int Ov024_MobiClip_BufferedFrameCount(void) { return KHVR_MOVIE_DONE_FRAMES; }

void Ov024_MobiClip_StopPlayback(void)
{
    VBlank_UnregisterCallback(1, gOv024MobiclipIntrName);
    sFrameReady = 0;
    khvr_host_print("movie: stopped");
}
