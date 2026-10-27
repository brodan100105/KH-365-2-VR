/* Data the game keeps inside its ITCM code block, which the port does not load (the code is
 * compiled, the block's bytes are not): the NitroSystem texture-matrix builders that
 * NNSi_G3dSendTexMtxMode0 / Mode2 call through fixed ITCM addresses. Each table has 8 entries
 * indexed by the material's flag bits (1: scale is one, 2: no rotation, 4: no translation); the
 * all-flags entry only sets the identity. Mode 0 is Maya's (NitroSystem maya.c); mode 2 is the
 * second set (the decomp's "_2" copies). The platform calls this once at boot. */
typedef void (*TexMtxBuilder)(void *m, const void *anm);

extern void texmtxCalc_flag_(void *, const void *);
extern void texmtxCalc_flagS_(void *, const void *);
extern void texmtxCalc_flagR_(void *, const void *);
extern void texmtxCalc_flagRS_(void *, const void *);
extern void texmtxCalc_flagT_(void *, const void *);
extern void texmtxCalc_flagTS_(void *, const void *);
extern void texmtxCalc_flagTR_(void *, const void *);
extern void Mtx44_SetIdentity2D(void *);
extern void texmtxCalc_flag__2(void *, const void *);
extern void texmtxCalc_flagS__2(void *, const void *);
extern void texmtxCalc_flagR__2(void *, const void *);
extern void texmtxCalc_flagRS__2(void *, const void *);
extern void texmtxCalc_flagT__2(void *, const void *);
extern void texmtxCalc_flagTS__2(void *, const void *);
extern void texmtxCalc_flagTR__2(void *, const void *);
extern void Mtx44_SetIdentity2D_2(void *);

void khvr_init_itcm_tables(void)
{
    TexMtxBuilder *mode0 = (TexMtxBuilder *)0x01ffa1f8;
    TexMtxBuilder *mode2 = (TexMtxBuilder *)0x01ffa598;
    mode0[0] = texmtxCalc_flag_;
    mode0[1] = texmtxCalc_flagS_;
    mode0[2] = texmtxCalc_flagR_;
    mode0[3] = texmtxCalc_flagRS_;
    mode0[4] = texmtxCalc_flagT_;
    mode0[5] = texmtxCalc_flagTS_;
    mode0[6] = texmtxCalc_flagTR_;
    mode0[7] = (TexMtxBuilder)Mtx44_SetIdentity2D;
    mode2[0] = texmtxCalc_flag__2;
    mode2[1] = texmtxCalc_flagS__2;
    mode2[2] = texmtxCalc_flagR__2;
    mode2[3] = texmtxCalc_flagRS__2;
    mode2[4] = texmtxCalc_flagT__2;
    mode2[5] = texmtxCalc_flagTS__2;
    mode2[6] = texmtxCalc_flagTR__2;
    mode2[7] = (TexMtxBuilder)Mtx44_SetIdentity2D_2;
}
