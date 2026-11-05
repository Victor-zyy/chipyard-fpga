/* Small full-SoC regression: actual RoCC -> DMA -> ACC -> DMA -> CPU.
 * Build once with ORDER_EXACT=0 and once with ORDER_EXACT=1. The first load
 * is deliberately A, so the optional first-response skew targets A, not setup.
 * No model, Linux, time CSR, or prints within capture windows are required.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <include/gemmini.h>

#ifndef ORDER_EXACT
#define ORDER_EXACT 0
#endif
// Attribution runs one case per fresh simulation, so the one-shot response
// injector targets A in BOTH the overlap and fenced-control experiments.
// Default 0 preserves the original three-case regression.
#ifndef ORDER_ONLY_CASE
#define ORDER_ONLY_CASE 0
#endif
#if ORDER_ONLY_CASE < 0 || ORDER_ONLY_CASE > 2
#error "ORDER_ONLY_CASE must be 0 (all), 1 (overlap), or 2 (fenced control)"
#endif
#if DIM != 16 || ACC_ROWS != 1024
#error "Ordering test targets ZU19EG DIM16 ACC64"
#endif
#define N (DIM * DIM)
#define AF UINT32_C(0x80000000)
#define ADD UINT32_C(0x40000000)
#define FULL UINT32_C(0x20000000)
#define MARK INT32_C(0x5a5a5a5a)
static acc_t a32[DIM][DIM] __attribute__((aligned(4096)));
static acc_t b32[DIM][DIM] __attribute__((aligned(4096)));
static elem_t a8[DIM][DIM] __attribute__((aligned(4096)));
static elem_t b8[DIM][DIM] __attribute__((aligned(4096)));
static struct { uint8_t pre[64]; acc_t data[DIM][DIM]; uint8_t post[64]; }
    dst[2] __attribute__((aligned(4096)));
static volatile acc_t saved[4][DIM][DIM];
static acc_t gold[DIM][DIM];
static unsigned errors, early_errors, stable_errors;
static uint32_t exact_tag;

static inline void barrier(void) { __asm__ volatile("fence rw,rw" ::: "memory"); }
static inline __attribute__((always_inline)) acc_t fence_first(const acc_t *p)
{
    acc_t v;
    __asm__ volatile("fence rw,rw\n\tlw %0,0(%1)" : "=r"(v) : "r"(p) : "memory");
    return v;
}
static void delay(void)
{
    for (unsigned i = 0; i < 8192; i++) __asm__ volatile("nop" ::: "memory");
}
static void snapshot(unsigned phase, unsigned dest)
{
    const volatile acc_t (*src)[DIM] = dst[dest].data;
    for (unsigned r = 0; r < DIM; r++) for (unsigned c = 0; c < DIM; c++)
        saved[phase][r][c] = src[r][c];
}
static int32_t requant(int v, int mul, unsigned shift)
{
    int64_t p = (int64_t)v * mul;
    int64_t q = ((p < 0 ? -p : p) + (INT64_C(1) << (shift-1))) >> shift;
    return (int32_t)(p < 0 ? -q : q);
}
static void load(int second, uint32_t row, int add)
{
    if (ORDER_EXACT) {
        gemmini_extended4_config_ld_exact(DIM, second ? 6691 : 3809,
            second ? 13 : 12, DIM, second);
        const uint32_t addr = AF | (add ? ADD : 0) | exact_tag | row;
        if (second) { gemmini_extended_mvin2(b8, addr, DIM, DIM); }
        else { gemmini_extended_mvin(a8, addr, DIM, DIM); }
    } else {
        gemmini_extended4_config_ld(DIM*sizeof(acc_t), MVIN_SCALE_IDENTITY,
            false, DIM, second);
        const uint32_t addr = AF | (add ? ADD : 0) | row;
        if (second) { gemmini_extended_mvin2(b32, addr, DIM, DIM); }
        else { gemmini_extended_mvin(a32, addr, DIM, DIM); }
    }
}

static void run(const char *name, uint32_t row, int fenced, int do_add)
{
    for (unsigned d = 0; d < 2; d++) {
        memset(&dst[d], 0xd3, sizeof(dst[d]));
        for (unsigned r = 0; r < DIM; r++) for (unsigned c = 0; c < DIM; c++)
            dst[d].data[r][c] = MARK;
    }
    for (unsigned r = 0; r < DIM; r++) for (unsigned c = 0; c < DIM; c++)
        gold[r][c] = ORDER_EXACT ? requant(a8[r][c],3809,12) +
            (do_add ? requant(b8[r][c],6691,13) : 0) :
            a32[r][c] + (do_add ? b32[r][c] : 0);
    barrier();
    printf("[ORDER-CASE] %s exact=%d row=%u fenced=%d\n", name, ORDER_EXACT, row, fenced);
    printf("[ORDER-ADDR] A=0x%lx B=0x%lx C=0x%lx D=0x%lx\n",
        (unsigned long)(ORDER_EXACT ? (void *)a8 : (void *)a32),
        (unsigned long)(ORDER_EXACT ? (void *)b8 : (void *)b32),
        (unsigned long)dst[0].data, (unsigned long)dst[1].data);
    fflush(stdout);
    load(0, row, 0);
    if (fenced) barrier();
    if (do_add) load(1, row, 1);
    barrier();
    gemmini_extended_config_st(DIM*sizeof(acc_t), NO_ACTIVATION, ACC_SCALE_IDENTITY);
#if ORDER_EXACT
    // Preserve the Exact regression's normalization-state reset. RAW full-INT32
    // attribution must also run on NoExt, which does not decode CONFIG_NORM.
    // Gate on the test mode, not HAS_*: all six configs use the same RAW ELF.
    gemmini_config_norm(0, 0, 0, 0, 0, 0, 0);
#endif
    gemmini_extended_mvout(dst[0].data, AF|FULL|row, DIM, DIM);
    const acc_t first = fence_first(&dst[0].data[0][0]);
    snapshot(0,0); saved[0][0][0] = first;
    delay(); snapshot(1,0);
    gemmini_extended_mvout(dst[1].data, AF|FULL|row, DIM, DIM);
    const acc_t repeated_first = fence_first(&dst[1].data[0][0]);
    snapshot(2,1); saved[2][0][0] = repeated_first;
    delay(); snapshot(3,1);
    const char *phases[] = {"NOW", "LATE_NO_CMD", "RE_MVOUT_NOW", "RE_MVOUT_LATE"};
    for (unsigned p = 0; p < 4; p++) {
        unsigned bad = 0;
        for (unsigned r = 0; r < DIM; r++) for (unsigned c = 0; c < DIM; c++) {
            if (saved[p][r][c] != gold[r][c]) {
                if (bad < 4) printf("[ORDER-DIFF] %s %s r=%u c=%u got=%d want=%d\n",
                    name, phases[p], r, c, (int)saved[p][r][c], (int)gold[r][c]);
                bad++;
            }
        }
        errors += bad;
        if (p & 1) stable_errors += bad; else early_errors += bad;
        printf("[ORDER-CHECK] %s %s bad=%u/%u\n", name, phases[p], bad, N);
    }
    unsigned guard = 0;
    for (unsigned d = 0; d < 2; d++) for (unsigned i = 0; i < 64; i++)
        guard += (dst[d].pre[i] != 0xd3) + (dst[d].post[i] != 0xd3);
    errors += guard;
    printf("[ORDER-GUARD] bad=%u\n", guard);
}

int main(void)
{
    puts("[INFO] ordering-v1: full RoCC path, no Linux/MMU coverage, NOT a benchmark");
    printf("[INFO] DIM=%d ACC_ROWS=%d exact=%d\n", DIM, ACC_ROWS, ORDER_EXACT);
#if ORDER_ONLY_CASE
    printf("[ATTR] ordering-attribution-v1 single_case=%d exact=%d\n",
        ORDER_ONLY_CASE, ORDER_EXACT);
#endif
    unsigned rows = BANK_NUM * BANK_ROWS > ACC_ROWS ? BANK_NUM * BANK_ROWS : ACC_ROWS;
    unsigned bits = 0;
    while ((1U << bits) < rows) bits++;
    exact_tag = 1U << (bits + 1);
    for (unsigned r = 0; r < DIM; r++) for (unsigned c = 0; c < DIM; c++) {
        unsigned i = r*DIM+c;
        a32[r][c] = (int)(i*37%601)-300;
        b32[r][c] = (int)(i*19%257)-128;
        a8[r][c] = (int8_t)(i < 128 ? (int)i : (int)i-256);
        unsigned j = 255-i;
        b8[r][c] = (int8_t)(j < 128 ? (int)j : (int)j-256);
    }
    barrier(); gemmini_flush(0); barrier();
    gemmini_config_ex(WS, NO_ACTIVATION, 0);
    // First Get must belong to the unfenced A overwrite, for one-shot skew.
#if ORDER_ONLY_CASE == 0 || ORDER_ONLY_CASE == 1
    run("OVERLAP", ACC_ROWS/2, 0, 1);
#endif
#if ORDER_ONLY_CASE == 0 || ORDER_ONLY_CASE == 2
    run("FENCED_CONTROL", ACC_ROWS/2, 1, 1);
#endif
#if ORDER_ONLY_CASE == 0
    run("MVOUT_ONLY", 0, 1, 0);
#endif
    barrier();
    printf("[%s] ordering-v1 early_errors=%u stable_errors=%u total=%u\n",
        errors ? "FAIL" : "PASS", early_errors, stable_errors, errors);
    return errors ? 1 : 0;
}
