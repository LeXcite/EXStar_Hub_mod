/* fan_blob_check.c - offline validator for the --scanner-fan A64 blob.
 * Mirrors the encoders/constants in src\manager.c (fanBuildPidBlob is
 * textually synced from it by tools\sync_fan_check.ps1); prints the
 * loop words and checks them against the known-good factory facts:
 *   - the collaborator's status.cgi tags his pid hook as file bytes
 *     "c66a0014" at 151088 == LE encoding of `b 260424`  -> our b must
 *     encode to exactly that word;
 *   - factory hook word 0x7100a43f decodes as `cmp w1,#41`.
 * Build & run:
 *   cl /W4 /Fe:build\fanblob.exe tools\fan_blob_check.c && build\fanblob.exe
 */
#include <windows.h>
#include <stdio.h>

/* --- encoders (keep identical to manager.c) ------------------------ */
static DWORD a64Movz(int rd, DWORD imm) { return 0x52800000u | ((imm & 0xFFFF) << 5) | (rd & 31); }
static DWORD a64CmpW1(int rn, DWORD imm)
    { return 0x7100001Fu | ((imm & 0xFFF) << 10) | ((rn & 31) << 5); }
static DWORD a64Sub(int rd, int rn, DWORD imm)
    { return 0x51000000u | ((imm & 0xFFF) << 10) | ((rn & 31) << 5) | (rd & 31); }
static DWORD a64Madd(int rd, int rn, int rm, int ra)
    { return 0x1B000000u | ((rm & 31) << 16) | ((ra & 31) << 10) | ((rn & 31) << 5) | (rd & 31); }
static DWORD a64Csel(int rd, int rn, int rm, int cond)
    { return 0x1A800000u | ((rm & 31) << 16) | ((cond & 15) << 12) | ((rn & 31) << 5) | (rd & 31); }
static DWORD a64LdrW(int rt, int rn, DWORD imm12)
    { return 0xB9400000u | ((imm12 & 0xFFF) << 10) | ((rn & 31) << 5) | (rt & 31); }
static DWORD a64StrW(int rt, int rn, DWORD imm12)
    { return 0xB9000000u | ((imm12 & 0xFFF) << 10) | ((rn & 31) << 5) | (rt & 31); }
static DWORD a64Asr(int rd, int rn, int amt)
    { return 0x13000800u | ((amt & 63) << 16) | 0x7C00u | ((rn & 31) << 5) | (rd & 31); }
static DWORD a64B(DWORD from, DWORD to)
    { return 0x14000000u | (((to - from) / 4) & 0x03FFFFFFu); }
static DWORD a64Bcond(DWORD from, DWORD to, int cond)
    { return 0x54000000u | ((((to - from) / 4) & 0x7FFFF) << 5) | (cond & 15); }
static DWORD a64AddImm(int rd, int rn, DWORD imm12)
    { return 0x11000000u | ((imm12 & 0xFFF) << 10) | ((rn & 31) << 5) | (rd & 31); }
static DWORD a64Adrp(int rd, DWORD pc, DWORD target)
{
    long long d = (long long) (target & ~0xFFFu) - (long long) (pc & ~0xFFFu);
    DWORD imm = (DWORD) (d >> 12);
    return 0x90000000u | ((imm & 3) << 29) | (((imm >> 2) & 0x7FFFF) << 5) | (rd & 31);
}
#define FAN_COND_LE 13
#define FAN_COND_GE 10
#define FAN_F_HOOK     151088u
#define FAN_F_HOOK_W0  0x7100A43Fu
#define FAN_F_TEMP     151216u
#define FAN_F_OUT      260512u
#define FAN_F_CAVE     260424u
#define FAN_C_CODE     128
#define FAN_C_INTEG    (FAN_F_CAVE + 128)
#define FAN_C_STATE    (FAN_F_CAVE + 152)
#define FAN_LOAD_REG   12

/* --- builder: SYNCED from manager.c, do not edit here -------------- */
/*START*/
static int fanBuildPidBlob(DWORD *w, DWORD off, DWORD minV, DWORD maxV,
                           DWORD targetDeci)
{
    const int X = 9, T = 10, DUTY = 11, ERR = 12, KP = 13, TMP = 14;
    DWORD n = 0, cmpImm;
    /* the temp global holds milli-deg C; >>10 puts T in units of
     * 1024 mC (~1 C), so target [deci] -> (deci*100)>>10. PROBE item. */
    cmpImm = (targetDeci * 25) / 256;
    if (cmpImm > 0xFFF || minV > 0xFFF || maxV > 0xFFF) return -1;
    w[n++] = a64Adrp(X,   off +  0, FAN_F_TEMP);
    w[n++] = a64LdrW(T,   X, FAN_F_TEMP & 0xFFF);   /* raw temp global */
    w[n++] = a64Asr (T,   T, 10);                   /* T (scale: PROBE)*/
    w[n++] = a64Adrp(X,   off + 12, FAN_C_INTEG);
    w[n++] = a64AddImm(X, X, FAN_C_INTEG & 0xFFF);  /* x9 = &duty      */
    w[n++] = a64LdrW(DUTY, X, 0);                   /* duty = integral */
    w[n++] = a64CmpW1(T, cmpImm);
    w[n++] = a64Bcond(off + 28, off + 92, FAN_COND_LE); /* T<=t: keep  */
    w[n++] = a64Sub(ERR, T, cmpImm);                /* err = T - target*/
    w[n++] = a64LdrW(KP,  X, 8);                    /* Kp word         */
    w[n++] = a64Madd(DUTY, ERR, KP, DUTY);          /* duty += Kp*err  */
    w[n++] = a64CmpW1(DUTY, minV);
    w[n++] = a64Movz(TMP, minV);
    w[n++] = a64Csel(DUTY, DUTY, TMP, FAN_COND_GE);
    w[n++] = a64CmpW1(DUTY, maxV);
    w[n++] = a64Movz(TMP, maxV);
    w[n++] = a64Csel(DUTY, DUTY, TMP, FAN_COND_LE);
    w[n++] = a64StrW(DUTY, X, 0);                   /* store clamped   */
    w[n++] = a64Adrp(X,   off + 72, FAN_F_OUT);
    w[n++] = a64StrW(DUTY, X, FAN_F_OUT & 0xFFF);   /* -> vendor PWM   */
    w[n++] = a64Adrp(X,   off + 80, FAN_C_STATE);
    w[n++] = a64StrW(DUTY, X, FAN_C_STATE & 0xFFF); /* status word     */
    w[n++] = a64B   (off + 88, off + 92);           /* -> reload       */
    w[n++] = a64Movz(FAN_LOAD_REG, 0);              /* reload (PROBE)  */
    w[n++] = a64B   (off + 96, FAN_F_HOOK + 4);     /* back to the app */
    if (n > FAN_C_CODE / 4) return -1;              /* code budget     */
    return (int) n;
}
/*END*/

/* branch helpers: decode imm of b / b.cond, return target or 0 */
static DWORD brTarget(DWORD wv, DWORD from, DWORD lo, DWORD hi)
{
    LONG imm;
    if ((wv & 0x7C000000u) == 0x14000000u) {          /* b */
        imm = (LONG)(wv & 0x03FFFFFFu);
        if (imm & 0x02000000) imm |= (LONG)0xFC000000;
    } else if ((wv & 0x7E000000u) == 0x54000000u) {   /* b.cond */
        imm = (LONG)((wv >> 5) & 0x7FFFF);
        if (imm & 0x40000) imm |= (LONG)0xFFF80000;
    } else return 0;
    return from + (DWORD)(imm * 4);
}

int main(void)
{
    DWORD w[32];
    int n, i, fails = 0;
    DWORD hook;

    printf("== factory facts ==\n");
    if (a64CmpW1(1, 41) != FAN_F_HOOK_W0)
        { printf("FAIL anchor decode %08x\n", a64CmpW1(1, 41)); fails++; }
    else printf("cmp w1,#41        = %08x  OK (factory hook word)\n", FAN_F_HOOK_W0);
    if (a64CmpW1(12, 41) != 0x7100A59Fu)
        { printf("FAIL cmp w12,#41  %08x\n", a64CmpW1(12, 41)); fails++; }
    else printf("cmp w12,#41       = 7100a59f  OK (distinct encoding)\n");
    if (a64Asr(10, 0, 10) != 0x130A7C0Au) { printf("FAIL asr\n"); fails++; }
    else printf("asr w10,w0,#10    = 130a7c0a  OK\n");

    hook = a64B(FAN_F_HOOK, FAN_F_CAVE);
    if (hook != 0x14006AC6u) { printf("FAIL hook b %08x\n", hook); fails++; }
    else printf("hook b cave       = %08x -> file bytes c66a0014"
                "  OK (collaborator's status.cgi pid tag)\n", hook);

    printf("\n== pid blob (45.0 C, clamp 80..255) ==\n");
    n = fanBuildPidBlob(w, FAN_F_CAVE, 80, 255, 450);
    if (n < 0) { printf("FAIL builder rejected params\n"); return 1; }
    printf("words: %d (%d bytes code, budget %d)\n", n, n * 4, FAN_C_CODE);
    for (i = 0; i < n; i++)
        printf("  w%02d @0x%05x %08x\n", i, FAN_F_CAVE + i * 4, w[i]);
    if (n * 4 > FAN_C_CODE) { printf("FAIL code overruns budget\n"); fails++; }
    if (FAN_C_INTEG - FAN_F_CAVE < (unsigned) n * 4)
        { printf("FAIL workspace overlaps code\n"); fails++; }

    for (i = 0; i < n; i++) {
        DWORD from = FAN_C_CODE + 0; /* unused */
        from = FAN_F_CAVE + i * 4;
        if (i == n - 1) continue;   /* the return branch is checked below */
        if (brTarget(w[i], from, 0, 0) &&
            (brTarget(w[i], from, 0, 0) < FAN_F_CAVE ||
             brTarget(w[i], from, 0, 0) >= FAN_F_CAVE + (DWORD)n * 4))
            { printf("FAIL branch escapes blob @w%d -> %08x\n", i,
                     brTarget(w[i], from, 0, 0)); fails++; }
    }
    {   /* final word: b back to the app (hook+4) */
        DWORD to = brTarget(w[n - 1], FAN_F_CAVE + (DWORD)(n - 1) * 4, 0, 0);
        if (to != FAN_F_HOOK + 4)
            { printf("FAIL return branch -> %08x (want %08x)\n", to, FAN_F_HOOK + 4);
              fails++; }
        else printf("return branch -> hook+4 (%u)  OK\n", FAN_F_HOOK + 4);
    }
    /* b.cond skip must land on the reload word (second-to-last) */
    {
        DWORD skipTo = brTarget(w[7], FAN_F_CAVE + 28, 0, 0);
        DWORD reload = FAN_F_CAVE + (DWORD)(n - 2) * 4;
        if (skipTo != reload)
            { printf("FAIL T<=target skip -> %08x (want reload %08x)\n",
                     skipTo, reload); fails++; }
        else printf("skip -> reload word  OK\n");
    }
    printf("\n%s (%d failures)\n", fails ? "FAILED" : "ALL OK", fails);
    return fails;
}
