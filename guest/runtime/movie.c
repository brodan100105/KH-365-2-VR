/* MobiClip movies, skipped (exclude.txt drops the decomp's versions of these four): the decoder's
 * core is still ARM assembly the port cannot run, and a movie that fails to open leaves the
 * opening scene waiting forever. Instead every movie opens, reports all of its frames played and
 * ends on the first tick, so the opening scene and the cutscene player both move straight on. */
#include "khvr_host.h"

struct MobiClipOpenRequest {
    const char *pszStream0;
    const char *pszStream1;
    const char *pszStream2;
    void *pfnFrameReady;
};

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
    return 1;
}

unsigned int Ov024_TickStreamSlots(void) { return 1; } /* nonzero: the streams have ended */

int Ov024_MobiClip_BufferedFrameCount(void) { return KHVR_MOVIE_DONE_FRAMES; }

void Ov024_MobiClip_StopPlayback(void) {}
