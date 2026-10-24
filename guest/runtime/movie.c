/* MobiClip movies, played without a picture (exclude.txt drops the decomp's versions of these
 * four): the decoder's core is still ARM assembly the port cannot run. The scenes around a movie
 * depend on it playing for its real length, though: the opening's script steps its timeline in
 * time with the movie's frames, and it needs the request's frame callback once per vertical
 * blank (Ov024_MobiClip_PresentOnVBlank calls it on the DS) to clear the flag it waits on. So a
 * movie here opens, counts its frames up at its own frame rate from the vertical blanks, calls
 * the callback every blank, and ends when the real one would; START skips it as on the DS.
 * Drawing the frames is a later step (FFmpeg's MobiClip decoder on the host side). */
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
extern void OS_WaitVBlankIntr(void);
extern void FS_InitFile(void *pFile);
extern int FS_OpenFile(void *pFile, const char *pszPath);
extern int FS_ReadFile(void *pFile, void *pDest, int nLength);
extern int FS_CloseFile(void *pFile);
extern const char *Msg_BuildLangPath(const char *pszPath);

/* The DS's vertical blank rate. */
#define KHVR_VBLANK_HZ_X1000 59826

static void (*sFrameReady)(void);
static int sOpen;
static unsigned int sVBlanks;     /* vertical blanks since the movie opened */
static unsigned int sFrameCount;  /* the movie's length in frames */
static unsigned int sFps24;       /* its frame rate, frames per second << 24 */

/* Frames the movie has shown by now, at its own frame rate. */
static unsigned int ElapsedFrames(void)
{
    unsigned long long f = (unsigned long long)sVBlanks * sFps24 * 1000ull /
                           ((unsigned long long)KHVR_VBLANK_HZ_X1000 << 24);
    return f > sFrameCount ? sFrameCount : (unsigned int)f;
}

static void PresentNothingOnVBlank(void)
{
    if (sVBlanks++ == 0) khvr_host_print("movie: playing (no picture yet)");
    if (sFrameReady) sFrameReady();
}

static unsigned int Le32(const unsigned char *p)
{
    return p[0] | p[1] << 8 | p[2] << 16 | (unsigned int)p[3] << 24;
}

/* Reads a .mods header: "MODSN" and a version, then the frame count at 8 and the frame rate at
 * 0x14 (frames per second << 24). Falls back to an instant movie if it cannot. */
static void ReadLength(const char *pszPath)
{
    static unsigned int file[0x48 / 4];
    unsigned char header[0x20];
    sFrameCount = 1;
    sFps24 = 30u << 24;
    if (!pszPath) return;
    FS_InitFile(file);
    if (!FS_OpenFile(file, Msg_BuildLangPath(pszPath))) return;
    if (FS_ReadFile(file, header, sizeof header) == (int)sizeof header && header[0] == 'M' &&
        header[1] == 'O' && header[2] == 'D' && header[3] == 'S') {
        unsigned int frames = Le32(header + 8), fps = Le32(header + 0x14);
        if (frames > 0 && frames < 100000) sFrameCount = frames;
        if (fps >= (1u << 24) && fps <= (120u << 24)) sFps24 = fps;
    }
    FS_CloseFile(file);
}

int Ov024_MobiClip_OpenStreams(struct MobiClipOpenRequest *pRequest)
{
    const char *pszPath = pRequest->pszStream0 ? pRequest->pszStream0
                          : pRequest->pszStream1 ? pRequest->pszStream1
                                                 : pRequest->pszStream2;
    khvr_host_print("movie (MobiClip video is not drawn yet):");
    if (pszPath) khvr_host_print(pszPath);
    ReadLength(pszPath);
    sFrameReady = (void (*)(void))pRequest->pfnFrameReady;
    sVBlanks = 0;
    sOpen = 1;
    RegisterNamedTask(1, gOv024MobiclipIntrName, (void *)&PresentNothingOnVBlank);
    return 1;
}

/* Nonzero once the movie has played to its end. Waits for the next vertical blank first: the
 * opening polls this in a tight loop, which on the DS the frame interrupt breaks up. */
unsigned int Ov024_TickStreamSlots(void)
{
    if (!sOpen || ElapsedFrames() >= sFrameCount) return 1;
    OS_WaitVBlankIntr();
    return ElapsedFrames() >= sFrameCount;
}

int Ov024_MobiClip_BufferedFrameCount(void)
{
    return sOpen ? (int)ElapsedFrames() : -1;
}

void Ov024_MobiClip_StopPlayback(void)
{
    VBlank_UnregisterCallback(1, gOv024MobiclipIntrName);
    sFrameReady = 0;
    sOpen = 0;
    khvr_host_print("movie: stopped");
}
