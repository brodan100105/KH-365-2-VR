/* The C library functions the decomp calls but does not define (MSL's are assembly or use mwcc
 * lvalue casts). Byte loops: every store still goes through the I/O and VRAM traps, so copies into
 * hardware registers or VRAM behave as on the DS. */
typedef unsigned long size_t;

void *memcpy(void *dst, const void *src, size_t n)
{
    unsigned char *d = dst;
    const unsigned char *s = src;
    if ((((unsigned long)d | (unsigned long)s | n) & 3) == 0) {
        unsigned long *dw = (unsigned long *)d;
        const unsigned long *sw = (const unsigned long *)s;
        for (n >>= 2; n; --n) *dw++ = *sw++;
        return dst;
    }
    while (n--) *d++ = *s++;
    return dst;
}

void *memmove(void *dst, const void *src, size_t n)
{
    unsigned char *d = dst;
    const unsigned char *s = src;
    if (d == s || n == 0) return dst;
    if (d < s) {
        while (n--) *d++ = *s++;
    } else {
        d += n;
        s += n;
        while (n--) *--d = *--s;
    }
    return dst;
}

void *memset(void *dst, int c, size_t n)
{
    unsigned char *d = dst;
    if ((((unsigned long)d | n) & 3) == 0) {
        unsigned long w = (unsigned char)c * 0x01010101u;
        unsigned long *dw = (unsigned long *)d;
        for (n >>= 2; n; --n) *dw++ = w;
        return dst;
    }
    while (n--) *d++ = (unsigned char)c;
    return dst;
}

int memcmp(const void *a, const void *b, size_t n)
{
    const unsigned char *x = a, *y = b;
    for (; n; --n, ++x, ++y)
        if (*x != *y) return *x - *y;
    return 0;
}

int strcmp(const char *a, const char *b)
{
    while (*a && *a == *b) {
        ++a;
        ++b;
    }
    return (unsigned char)*a - (unsigned char)*b;
}

char *strcpy(char *dst, const char *src)
{
    char *d = dst;
    while ((*d++ = *src++) != 0) {
    }
    return dst;
}

size_t strlen(const char *s)
{
    const char *p = s;
    while (*p) ++p;
    return (size_t)(p - s);
}
