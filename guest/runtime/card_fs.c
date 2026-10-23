/* Card ROM reads and overlay start-up, replaced at the SDK's level (exclude.txt drops the
 * decomp's CARDi_ReadRom.c and FS_StartOverlay.c):
 *  - the ROM is a file the platform reads, not a card on a bus, so a read is one copy and the
 *    completion callback runs right away;
 *  - overlays are linked into the port, so "starting" one only gives it fresh .bss and
 *    initialized data, as loading it from the ROM did. */
#include "khvr_host.h"

typedef unsigned long u32;
typedef void (*CARDCallback)(void *argument);

/* The ROM offset the SDK adds to every read (non-zero only for download-play children). */
extern u32 data_02046b00;

void CARDi_ReadRom(u32 dma, const void *source, void *destination, u32 length,
                   CARDCallback callback, void *callbackArgument, int asynchronous)
{
    (void)dma;
    (void)asynchronous;
    khvr_host_read_rom((u32)source + data_02046b00, destination, length);
    if (callback) callback(callbackArgument);
}

typedef struct {
    u32 id;               /* FSOverlayInfoHeader: the overlay id comes first */
    u32 rest[7];
} FSOverlayInfoHead;

void FS_StartOverlay(FSOverlayInfoHead *ovi) { khvr_host_start_overlay(ovi->id); }
