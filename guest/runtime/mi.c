/* NitroSDK MI copy/fill routines and the streaming LZ decoder, which the SDK writes in assembly.
 * Semantics follow the SDK's documentation: sizes in bytes, 16/32-bit variants move whole units. */
typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned long u32;
typedef long s32;

void MI_CpuCopy8(const void *src, void *dst, u32 size)
{
    const u8 *s = src;
    u8 *d = dst;
    while (size--) *d++ = *s++;
}

void MIi_CpuCopy16(const void *src, void *dst, u32 size)
{
    const u16 *s = src;
    u16 *d = dst;
    for (size >>= 1; size; --size) *d++ = *s++;
}

void MIi_CpuCopy32(const void *src, void *dst, u32 size)
{
    const u32 *s = src;
    u32 *d = dst;
    for (size >>= 2; size; --size) *d++ = *s++;
}

void MIi_CpuCopyFast(const void *src, void *dst, u32 size) { MIi_CpuCopy32(src, dst, size); }

void MI_CpuFill8(void *dst, u8 data, u32 size)
{
    u8 *d = dst;
    while (size--) *d++ = data;
}

void MIi_CpuClear16(u16 data, void *dst, u32 size)
{
    u16 *d = dst;
    for (size >>= 1; size; --size) *d++ = data;
}

void MIi_CpuClearFast(u32 data, void *dst, u32 size)
{
    u32 *d = dst;
    for (size >>= 2; size; --size) *d++ = data;
}

void INITi_CpuClear32_0x01ff86fc(u32 data, void *dst, u32 size) { MIi_CpuClearFast(data, dst, size); }

/* Writes every word of `src` to the same address (a FIFO register). */
void MIi_CpuSend32(const void *src, volatile void *dst, u32 size)
{
    const u32 *s = src;
    volatile u32 *d = dst;
    for (size >>= 2; size; --size) *d = *s++;
}

void MI_Copy36B(const void *src, void *dst) { MIi_CpuCopy32(src, dst, 36); }
void MI_Copy48B(const void *src, void *dst) { MIi_CpuCopy32(src, dst, 48); }
void MI_Copy64B(const void *src, void *dst) { MIi_CpuCopy32(src, dst, 64); }
void MI_Zero36B(void *dst) { MIi_CpuClearFast(0, dst, 36); }

u32 MI_SwapWord(u32 data, volatile u32 *dst)
{
    u32 old = *dst;
    *dst = data;
    return old;
}

/* Overlays are linked into the port, never decompressed from the ROM. */
void MIi_UncompressBackward(void *bottom) { (void)bottom; }

/* MI_ReadUncompLZ8 (func_02004484): feeds `len` more compressed bytes to a streaming LZ77
 * (type 0x10, optionally the extended 0x11 format) decode. Returns the bytes still to produce.
 * A line-by-line translation of the SDK's assembly. */
typedef struct {
    u8 *destp;
    s32 destCount;
    u32 length;
    u16 destTmp;
    u8 destTmpCnt;
    u8 flags;
    u8 flagIndex;
    u8 lengthFlg;
    u8 exFormat;
    u8 _padding[1];
} MIUncompContextLZ;

s32 func_02004484(MIUncompContextLZ *ctx, const u8 *data, u32 len)
{
    u8 *dst = ctx->destp;
    s32 count = ctx->destCount;
    u32 flags = ctx->flags;
    u32 flagIndex = ctx->flagIndex;
    u32 length = ctx->length;
    u32 lengthFlg = ctx->lengthFlg;
    u32 ex = ctx->exFormat;

    while (count > 0) {
        if (flagIndex == 0) {
            if (len == 0) goto done;
            flags = *data++;
            flagIndex = 8;
            --len;
            continue;
        }
        if (len == 0) goto done;
        if (!(flags & 0x80)) {
            *dst++ = *data++;
            --count;
            --len;
        } else {
            if (lengthFlg != 0) {
                /* Reading the length bytes of a reference (state kept across calls). */
                for (;;) {
                    if (ex == 1) {
                        --lengthFlg;
                        if (lengthFlg == 0) {
                            length += *data++;
                        } else if (lengthFlg == 1) {
                            length += (u32)*data++ << 8;
                            if (--len == 0) goto done;
                            continue;
                        } else {
                            length = *data++;
                            if (length & 0xe0) {
                                length += 0x10;
                                lengthFlg = 0;
                            } else {
                                u32 base = 0x110;
                                if (length & 0x10) {
                                    length &= 0xf;
                                    base += 0x1000;
                                    length = base + (length << 16);
                                } else {
                                    length &= 0xf;
                                    length = base + (length << 8);
                                    lengthFlg = 1;
                                }
                                if (--len == 0) goto done;
                                continue;
                            }
                        }
                    } else {
                        length = (u32)*data++ + 0x30;
                        lengthFlg = 0;
                    }
                    break;
                }
                if (--len == 0) goto done;
            }
            {
                u32 disp = ((length & 0xf) << 8) | *data++;
                s32 n = (s32)length >> 4;
                lengthFlg = 3;
                --len;
                ++disp;
                while (n-- > 0) {
                    *dst = dst[-(s32)disp];
                    ++dst;
                    --count;
                }
            }
        }
        if (count == 0) goto done;
        flags <<= 1;
        --flagIndex;
    }
done:
    ctx->destp = dst;
    ctx->destCount = count;
    ctx->flags = (u8)flags;
    ctx->flagIndex = (u8)flagIndex;
    ctx->length = length;
    ctx->lengthFlg = (u8)lengthFlg;
    ctx->exFormat = (u8)ex;
    return count;
}
