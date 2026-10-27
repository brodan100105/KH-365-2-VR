/* Port version of NNS_G2dFontFindGlyphIndex (NitroSystem g2d_Font.c): the same lookup, with the
 * font, its resource and each code map checked to lie in guest memory first. A HUD panel's text
 * at the first mission's start reached here with a font whose pointers lead outside memory (a
 * trap on the headset); with the check that glyph is reported missing instead, and the first
 * few bad fonts are logged with the pointer that failed, so the cause can be found. */

#include "nitro/types.h"
#include "nitro/os.h"
#include "nnsys/g2d.h"
#include "khvr_host.h"

extern u16 GetGlyphIndex (const NNSG2dFontCodeMap * pMap, u16 c);

/* The guest's memory is 256 MB; anything below 0x1000 is not a real object either. */
#define KHVR_VALID(p) ((unsigned long)(p) >= 0x1000 && (unsigned long)(p) < 0x10000000 - 0x20)

static int sBadFonts;

static u16 BadFont(const char *what, const void *pFont, const void *p)
{
    if (sBadFonts < 8) {
        sBadFonts++;
        khvr_host_trace(what, (unsigned long)pFont, (unsigned long)p);
    }
    return NNS_G2D_GLYPH_INDEX_NOT_FOUND;
}

u16 NNS_G2dFontFindGlyphIndex (const NNSG2dFont * pFont, u16 c)
{
    const NNSG2dFontCodeMap * pMap;
    int n = 0;

    if (!KHVR_VALID(pFont)) return BadFont("font: bad font pointer", pFont, 0);
    if (!KHVR_VALID(pFont->pRes)) return BadFont("font: bad resource (font -> pRes)", pFont, pFont->pRes);
    pMap = pFont->pRes->pMap;

    while (pMap != NULL) {
        if (!KHVR_VALID(pMap) || ++n > 64) return BadFont("font: bad code map (font -> map)", pFont, pMap);
        if ((pMap->ccodeBegin <= c) && (c <= pMap->ccodeEnd)) {
            return GetGlyphIndex(pMap, c);
        }

        pMap = pMap->pNext;
    }

    return NNS_G2D_GLYPH_INDEX_NOT_FOUND;
}
