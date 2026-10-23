/* NitroSDK OS functions the SDK writes in assembly: CPU mode and interrupt flags, lock ids,
 * thread contexts, caches and the protection unit. The guest runs on one host thread at a time
 * (khvr_host.h), so "disabling interrupts" only has to keep the flag the callers read back. */
#include "khvr_host.h"

typedef unsigned short u16;
typedef unsigned long u32;
typedef long s32;

/* The CPSR's I (0x80) and F (0x40) bits; the platform reads them to know when an interrupt may
 * be taken (platform/src/hle_thread.cpp). */
u32 khvr_cpsr_i;
#define sCpsrI khvr_cpsr_i

/* Re-enabling interrupts is where the hardware would take a pending one. */
#define IRQ_WINDOW(old) do { if (((old) & 0x80) && !(sCpsrI & 0x80)) khvr_host_irq_window(); } while (0)

u32 OS_DisableInterrupts(void) { u32 o = sCpsrI & 0x80; sCpsrI |= 0x80; return o; }
u32 OS_EnableInterrupts(void) { u32 o = sCpsrI & 0x80; sCpsrI &= ~0x80u; IRQ_WINDOW(o); return o; }
u32 OS_RestoreInterrupts(u32 s) { u32 o = sCpsrI & 0x80; sCpsrI = (sCpsrI & ~0x80u) | (s & 0x80); IRQ_WINDOW(o); return o; }
u32 OS_DisableInterrupts_IrqAndFiq(void) { u32 o = sCpsrI & 0xc0; sCpsrI |= 0xc0; return o; }
u32 OS_RestoreInterrupts_IrqAndFiq(u32 s) { u32 o = sCpsrI & 0xc0; sCpsrI = (sCpsrI & ~0xc0u) | (s & 0xc0); IRQ_WINDOW(o); return o; }
u32 OS_GetCpsrIrq(void) { return sCpsrI & 0x80; }
/* Always system mode: the port never runs guest code "inside" an IRQ exception. */
u32 OS_GetProcMode(void) { return 0x1f; }

void OS_SpinWait(u32 cycles) { (void)cycles; }
void WaitByLoop(s32 count) { (void)count; }
void OS_Halt(void) { khvr_host_halt(); }
void OS_ResetSystem(u32 param) { (void)param; khvr_host_reset(); }

extern char khvr_dtcm[];
u32 OS_GetDTCMAddress(void) { return (u32)khvr_dtcm; }

/* Lock ids: two words of free-id bits in the ARM7-shared area (OS_InitLock seeds them); the
 * highest free bit is taken. */
s32 OS_GetLockID(void)
{
    volatile u32 *w = (volatile u32 *)0x027fffb0;
    for (int i = 0; i < 2; ++i, ++w) {
        u32 v = *w;
        if (v) {
            int n = __builtin_clz(v);
            *w = v & ~(0x80000000u >> n);
            return (i ? 0x60 : 0x40) + n;
        }
    }
    return (s32)0xfffffffd;  /* OS_LOCK_ID_ERROR */
}

void OS_ReleaseLockID(u16 id)
{
    volatile u32 *w = (volatile u32 *)0x027fffb0;
    if (id < 0x40 || id >= 0x80) return;
    id -= 0x40;
    w[id >> 5] |= 0x80000000u >> (id & 31);
}

int OsCountZeroBits(u32 v) { return v ? __builtin_clz(v) : 32; }

/* OSContext: cpsr, r0-r12, sp, lr, pc+4, sp_svc, then the divider's CPContext. */
typedef struct {
    u32 cpsr;
    u32 r[13];
    u32 sp;
    u32 lr;
    u32 pc_plus4;
    u32 sp_svc;
} OSContextHead;

void OS_InitContext(OSContextHead *c, u32 newpc, u32 newsp)
{
    int i;
    c->cpsr = 0x1f;
    for (i = 0; i < 13; ++i) c->r[i] = 0;
    c->sp = newsp - 4;  /* the SDK leaves one word for the stack check */
    c->lr = 0;
    c->pc_plus4 = newpc + 4;
    c->sp_svc = 0;
}

int OS_SaveContext(void *c) { return khvr_host_save_context(c); }
void OS_LoadContext(void *c) { khvr_host_load_context(c); }

/* The divider and square-root unit keep no state outside their registers, which the platform
 * emulates; saving the context means reading them back, restoring means writing them again. */
typedef struct {
    unsigned long long divNumer, divDenom, sqrtParam;
    u16 divMode, sqrtMode;
} CPContext;

void CP_SaveContext(CPContext *c)
{
    c->divNumer = *(volatile unsigned long long *)0x04000290;
    c->divDenom = *(volatile unsigned long long *)0x04000298;
    c->sqrtParam = *(volatile unsigned long long *)0x040002b8;
    c->divMode = *(volatile u16 *)0x04000280 & 3;
    c->sqrtMode = *(volatile u16 *)0x040002b0 & 1;
}

void CPi_RestoreContext(const CPContext *c)
{
    *(volatile u16 *)0x04000280 = c->divMode;
    *(volatile u16 *)0x040002b0 = c->sqrtMode;
    *(volatile unsigned long long *)0x04000290 = c->divNumer;
    *(volatile unsigned long long *)0x04000298 = c->divDenom;
    *(volatile unsigned long long *)0x040002b8 = c->sqrtParam;
}

/* No caches, no MPU, no exception vectors, no cartridge slot. */
void DC_FlushAll(void) {}
void DC_FlushRange(const void *p, u32 n) { (void)p; (void)n; }
void DC_InvalidateAll(void) {}
void DC_InvalidateRange(void *p, u32 n) { (void)p; (void)n; }
void DC_StoreAll(void) {}
void DC_StoreRange(const void *p, u32 n) { (void)p; (void)n; }
void DC_WaitWriteBufferEmpty(void) {}
void IC_InvalidateAll(void) {}
void IC_InvalidateRange(void *p, u32 n) { (void)p; (void)n; }
void OS_SetProtectionRegion1(u32 v) { (void)v; }
void OS_SetProtectionRegion2(u32 v) { (void)v; }
void OS_SetDPermissionsForProtectionRegion(u32 m, u32 v) { (void)m; (void)v; }
void OS_EnableProtectionUnit(void) {}
void OS_DisableProtectionUnit(void) {}
void OS_UnLockCartridge(void) {}
void OSi_CancelDma0(void) {}
void OSi_ExceptionHandler(void) {}
void RtcWaitBusy(void) {}

extern void OSi_ArrangeTimer(void);
void OSi_AlarmHandler(void) { OSi_ArrangeTimer(); }
