/* The CodeWarrior runtime's integer helpers (64-bit and 32-bit division, shifts), NitroSDK MTX
 * routines written in assembly, the SDK's quicksort, and the GX FIFO copy helpers. */
typedef unsigned long u32;
typedef long s32;
typedef long long s64;
typedef unsigned long long u64;
typedef s32 fx32;

/* _u32_div_f (Math_DivMod) and _s32_div_f return the quotient in r0 and the remainder in r1,
 * which callers read as one 64-bit value; ARM's divide-by-zero gives 0 here. */
u64 Math_DivMod(u32 a, u32 b)
{
    if (!b) return 0;
    return (u64)(a / b) | ((u64)(a % b) << 32);
}

u64 func_02020400(s32 a, s32 b)
{
    if (!b) return 0;
    if (a == (s32)0x80000000 && b == -1) return (u32)a;
    return (u64)(u32)(a / b) | ((u64)(u32)(a % b) << 32);
}

s64 func_020201b8(s64 a, s64 b) { return b ? (a == (s64)0x8000000000000000ll && b == -1 ? a : a / b) : 0; }  /* _ll_sdiv */
s64 func_020201a8(s64 a, s64 b) { return b ? (b == -1 ? 0 : a % b) : 0; }                                  /* _ll_mod */
u64 func_02020368(u64 a, u64 b) { return b ? a / b : 0; }                                                  /* _ll_udiv */
u64 func_02020374(u64 a, u64 b) { return b ? a % b : 0; }                                                  /* _ull_mod */
u64 func_020203d0(u64 v, int s) { return s >= 64 ? 0 : v << s; }                                           /* _ll_shl */
s64 _ll_mul(s64 a, s64 b) { return a * b; }

/* MTX: row-major fixed-point matrices, row vectors (v' = v * M), translation in the last row. */
#define FX32_ONE 0x1000

void MTX_Identity22_(fx32 *m) { m[0] = FX32_ONE; m[1] = 0; m[2] = 0; m[3] = FX32_ONE; }

void MTX_Identity33_(fx32 *m)
{
    for (int i = 0; i < 9; ++i) m[i] = (i % 4 == 0) ? FX32_ONE : 0;
}

void MTX_Identity43_(fx32 *m)
{
    for (int i = 0; i < 12; ++i) m[i] = 0;
    m[0] = m[4] = m[8] = FX32_ONE;
}

void MTX_Identity44_(fx32 *m)
{
    for (int i = 0; i < 16; ++i) m[i] = (i % 5 == 0) ? FX32_ONE : 0;
}

void MTX_Copy43To44_(const fx32 *s, fx32 *d)
{
    for (int r = 0; r < 4; ++r) {
        d[r * 4 + 0] = s[r * 3 + 0];
        d[r * 4 + 1] = s[r * 3 + 1];
        d[r * 4 + 2] = s[r * 3 + 2];
        d[r * 4 + 3] = r == 3 ? FX32_ONE : 0;
    }
}

void MTX_Copy44To43_(const fx32 *s, fx32 *d)
{
    for (int r = 0; r < 4; ++r) {
        d[r * 3 + 0] = s[r * 4 + 0];
        d[r * 3 + 1] = s[r * 4 + 1];
        d[r * 3 + 2] = s[r * 4 + 2];
    }
}

static void Rot3(fx32 *m, int stride, int axis, fx32 s, fx32 c)
{
    fx32 v[9];
    switch (axis) {
    case 0: v[0] = FX32_ONE; v[1] = 0; v[2] = 0; v[3] = 0; v[4] = c; v[5] = s; v[6] = 0; v[7] = -s; v[8] = c; break;
    case 1: v[0] = c; v[1] = 0; v[2] = -s; v[3] = 0; v[4] = FX32_ONE; v[5] = 0; v[6] = s; v[7] = 0; v[8] = c; break;
    default: v[0] = c; v[1] = s; v[2] = 0; v[3] = -s; v[4] = c; v[5] = 0; v[6] = 0; v[7] = 0; v[8] = FX32_ONE; break;
    }
    for (int r = 0; r < 3; ++r)
        for (int k = 0; k < 3; ++k) m[r * stride + k] = v[r * 3 + k];
}

void MTX_RotY33_(fx32 *m, fx32 s, fx32 c) { Rot3(m, 3, 1, s, c); }
void MTX_RotZ33_(fx32 *m, fx32 s, fx32 c) { Rot3(m, 3, 2, s, c); }
void MTX_RotX43_(fx32 *m, fx32 s, fx32 c) { Rot3(m, 3, 0, s, c); m[9] = m[10] = m[11] = 0; }
void MTX_RotY43_(fx32 *m, fx32 s, fx32 c) { Rot3(m, 3, 1, s, c); m[9] = m[10] = m[11] = 0; }

/* MATH_QSort with the caller's work buffer; an insertion sort keeps it simple and stable enough
 * for the handful of elements the game sorts (mission results). */
typedef s32 (*CompareFunc)(void *, void *);
void Util_QuickSortWithWork(void *head, u32 num, u32 width, CompareFunc comp, void *work)
{
    unsigned char *base = head, *tmp = work;
    for (u32 i = 1; i < num; ++i) {
        u32 j = i;
        for (u32 b = 0; b < width; ++b) tmp[b] = base[i * width + b];
        while (j > 0 && comp(base + (j - 1) * width, tmp) > 0) {
            for (u32 b = 0; b < width; ++b) base[j * width + b] = base[(j - 1) * width + b];
            --j;
        }
        for (u32 b = 0; b < width; ++b) base[j * width + b] = tmp[b];
    }
}

/* Geometry FIFO helpers: every word goes to the same FIFO register. */
void GX_SendFifo48B(const void *src, volatile void *fifo)
{
    const u32 *s = src;
    volatile u32 *d = fifo;
    for (int i = 0; i < 12; ++i) *d = s[i];
}

void GXi_NopClearFifo128_(volatile void *fifo)
{
    volatile u32 *d = fifo;
    for (int i = 0; i < 128; ++i) *d = 0;
}

/* BIOS CpuSet: bit 24 fills with the first source unit, bit 26 moves 32-bit units instead of
 * 16-bit ones, the low 21 bits count units. */
void CpuSet(const void *src, void *dst, u32 ctrl)
{
    u32 n = ctrl & 0x1fffff;
    int fill = (ctrl >> 24) & 1;
    if ((ctrl >> 26) & 1) {
        const u32 *s = src;
        u32 *d = dst;
        for (u32 i = 0; i < n; ++i) d[i] = fill ? s[0] : s[i];
    } else {
        const unsigned short *s = src;
        unsigned short *d = dst;
        for (u32 i = 0; i < n; ++i) d[i] = fill ? s[0] : s[i];
    }
}
