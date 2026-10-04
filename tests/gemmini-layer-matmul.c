/* Same source for Spike, bare-metal RTL and (later) Linux validation.
 * No rdcycle: this is a correctness test, not a timing benchmark.
 * Run only with the new layer-capable extension/bitstream.
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <math.h>
#include <include/gemmini.h>

#ifndef GEMMINI_LAYER_TEST_TRACE
#define GEMMINI_LAYER_TEST_TRACE 0
#endif
#if GEMMINI_LAYER_TEST_TRACE != 0 && GEMMINI_LAYER_TEST_TRACE != 1
#error "GEMMINI_LAYER_TEST_TRACE must be 0 or 1"
#endif

/* Target-side progress only: no cycle CSR, extra accelerator fence or RTL
 * change. HTIF/stdio can change timing; this executable is not a benchmark.
 * The default regression path has NO printing between command submission
 * and output checking. Trace mode is explicitly diagnostic, not acceptance.
 */
#if GEMMINI_LAYER_TEST_TRACE
static void diag_stage(const char *stage)
{
    __asm__ volatile("" ::: "memory");
    printf("[LAYER-DIAG] %s\n", stage);
    fflush(stdout);
    __asm__ volatile("" ::: "memory");
}

static void diag_command(const char *stage, unsigned funct)
{
    printf("[LAYER-DIAG] %s funct=%u\n", stage, funct);
    fflush(stdout);
    __asm__ volatile("" ::: "memory");
}

/* Instrument the REAL inline layer helper as it is parsed, not a copied
 * descriptor implementation. gemmini.h (including legacy matmul and LUT
 * helpers) has already been parsed with the original macros above.
 */
#undef ROCC_INSTRUCTION_RS1_RS2
#define ROCC_INSTRUCTION_RS1_RS2(x, rs1, rs2, funct) do { \
    diag_command("COMMAND_BEGIN", (unsigned)(funct)); \
    ROCC_INSTRUCTION_0_R_R(x, rs1, rs2, funct); \
    diag_command("COMMAND_ISSUED", (unsigned)(funct)); \
} while (0)
#undef gemmini_fence
#define gemmini_fence() do { \
    diag_stage("LAYER_FENCE_BEGIN"); \
    __asm__ volatile("fence" ::: "memory"); \
    diag_stage("LAYER_FENCE_DONE"); \
} while (0)
#include <include/gemmini_layer.h>
#undef ROCC_INSTRUCTION_RS1_RS2
#define ROCC_INSTRUCTION_RS1_RS2(x, rs1, rs2, funct) \
    ROCC_INSTRUCTION_0_R_R(x, rs1, rs2, funct)
#undef gemmini_fence
#define gemmini_fence() asm volatile("fence")
#else
#define diag_stage(stage) ((void)0)
#include <include/gemmini_layer.h>
#endif

#ifndef HAS_LAYER_MATMUL
#error "Build against a layer-enabled configuration (not the baseline bitstream)"
#endif
#define CAP (256 * 512 + 4096)
static elem_t aa[CAP] __attribute__((aligned(4096)));
static elem_t bb[CAP] __attribute__((aligned(4096)));
static acc_t dd[512] __attribute__((aligned(4096)));
static acc_t cc[CAP] __attribute__((aligned(4096)));
static elem_t lut[256];

static void diag_load_lut(void)
{
#if GEMMINI_LAYER_TEST_TRACE
    /* Same sequence as gemmini_load_silu_lut: one leading fence, then
     * exactly 32 CONFIG_NORM writes; no trailing fence is added.
     */
    diag_stage("LUT_FENCE_BEGIN");
    __asm__ volatile("fence" ::: "memory");
    diag_stage("LUT_FENCE_DONE");
    for (unsigned chunk = 0; chunk < 32; chunk++) {
        uint64_t packed = 0;
        for (unsigned i = 0; i < 8; i++)
            packed |= (uint64_t)(uint8_t)lut[chunk * 8 + i] << (i * 8);
        printf("[LAYER-DIAG] LUT_CHUNK_BEGIN chunk=%u\n", chunk);
        fflush(stdout);
        gemmini_config_silu_lut8(chunk, packed);
        printf("[LAYER-DIAG] LUT_CHUNK_ISSUED chunk=%u\n", chunk);
        fflush(stdout);
    }
    diag_stage("LUT_LOAD_ISSUED");
#else
    gemmini_load_silu_lut(lut);
#endif
}

static int check(int m, int n, int k, int gap, int full, int bias, int legacy, int seed)
{
#if GEMMINI_LAYER_TEST_TRACE
    printf("[LAYER-DIAG] CASE_BEGIN m=%d n=%d k=%d full=%d bias=%d legacy=%d seed=%d\n",
        m,n,k,full,bias,legacy,seed);
    fflush(stdout);
#endif
    const int sa = k + gap, sb = k + gap + 1, sc = n + gap;
    elem_t *a = aa + 3, *b = bb + 7;
    void *c = (void *)(cc + 1);
    diag_stage("INIT_A_BEGIN");
    memset(aa, 0, sizeof aa);
    diag_stage("INIT_A_DONE");
    diag_stage("INIT_B_BEGIN");
    memset(bb, 0, sizeof bb);
    diag_stage("INIT_B_DONE");
    diag_stage("INIT_C_BEGIN");
    memset(cc, 0x5a, sizeof cc);
    diag_stage("INIT_C_DONE");
    diag_stage("INIT_D_BEGIN");
    memset(dd, 0, sizeof dd);
    diag_stage("INIT_D_DONE");
    diag_stage("INPUT_FILL_BEGIN");
    for (int i = 0; i < m; i++)
        for (int x = 0; x < k; x++) a[i*sa+x] = (i*7+x*3+seed)%17-8;
    for (int j = 0; j < n; j++) {
        dd[j] = (j*19+seed)%257-128;
        for (int x = 0; x < k; x++) b[j*sb+x] = (j*11+x*5+seed)%13-6;
    }
    diag_stage("INPUT_FILL_DONE");
    // Explicit pre-write all DMA buffers before flushing translation state.
    __asm__ volatile("" ::: "memory");
    diag_stage("FLUSH_ISSUE_BEGIN");
    gemmini_flush(0);
    diag_stage("FLUSH_ISSUED");
    diag_stage("LUT_BUILD_BEGIN");
    for (int raw = 0; raw < 256; raw++) {
        const int q = raw < 128 ? raw : raw-256;
        lut[raw] = (elem_t)(q / 2 + (seed & 7));
    }
    diag_stage("LUT_BUILD_DONE");
    diag_load_lut();
    const int act = full ? NO_ACTIVATION : SILU;
    const float scale = full ? 1.0f : 0.125f;
    const acc_t *d = bias ? dd : NULL;
#if GEMMINI_LAYER_TEST_TRACE
    printf("[STAGE] layer m=%d n=%d k=%d full=%d bias=%d legacy=%d\n",
        m,n,k,full,bias,legacy);
    fflush(stdout);
#endif
    // Save failures and print only AFTER all output/guard reads. Printing
    // the first mismatch here would give unfinished DMA time to catch up.
    struct { int row, col, got, expected; } samples[4];
    int sample_count = 0;
    if (legacy) {
        /* The existing legacy helper contains its own final fence. Keep it
         * unmodified; CALL_RETURNED means that internal fence also returned.
         */
        diag_stage("LEGACY_CALL_BEGIN");
        tiled_matmul_auto(m,n,k,a,b,d,c,sa,sb,n,sc,
            MVIN_SCALE_IDENTITY,MVIN_SCALE_IDENTITY,MVIN_SCALE_IDENTITY,
            act,scale,0,true,false,true,full,false,0,WS);
        diag_stage("LEGACY_CALL_RETURNED");
    } else {
        diag_stage("LAYER_CALL_BEGIN");
        if (gemmini_layer_matmul(m,n,k,a,b,d,c,sa,sb,sc,full,act,scale)) {
            puts("[FAIL] descriptor rejected");
            fflush(stdout);
            return 1;
        }
        diag_stage("LAYER_CALL_RETURNED");
    }
    __asm__ volatile("" ::: "memory");
    diag_stage("CPU_REFERENCE_BEGIN");
    int errors = 0;
    for (int i = 0; i < m; i++) {
#if GEMMINI_LAYER_TEST_TRACE
        if (i % 16 == 0) {
            printf("[LAYER-DIAG] CPU_REFERENCE_ROW row=%d total=%d\n", i, m);
            fflush(stdout);
        }
#endif
        for (int j = 0; j < sc; j++) {
            // Read hardware output BEFORE computing this element's golden
            // dot product: reference computation must not serve as a wait.
            int got = full ? ((volatile acc_t *)c)[i*sc+j] :
                             ((volatile elem_t *)c)[i*sc+j];
            int expected = full ? 0x5a5a5a5a : 0x5a;
            if (j < n) {
                int sum = bias ? dd[j] : 0;
                for (int x = 0; x < k; x++) sum += a[i*sa+x] * b[j*sb+x];
                if (full) expected = sum;
                else {
                    int q = (int)nearbyintf((float)sum * scale);
                    if (q > 127) q = 127;
                    if (q < -128) q = -128;
                    expected = lut[(uint8_t)(int8_t)q];
                }
            }
            if (got != expected) {
                errors++;
                if (sample_count < 4) {
                    samples[sample_count].row = i;
                    samples[sample_count].col = j;
                    samples[sample_count].got = got;
                    samples[sample_count].expected = expected;
                    sample_count++;
                }
            }
        }
    }
    diag_stage("CPU_REFERENCE_DONE");
    // Check guards outside the destination, not only row padding.
    if (cc[0] != 0x5a5a5a5a) errors++;
    const unsigned char *end = (unsigned char *)c + (size_t)m*sc*(full ? 4 : 1);
    for (int i = 0; i < 64; i++) if (end[i] != 0x5a) errors++;
    for (int i = 0; i < sample_count; i++)
        printf("mismatch row=%d col=%d got=%d expected=%d\n",
            samples[i].row, samples[i].col, samples[i].got, samples[i].expected);
    printf("[%s] layer m=%d n=%d k=%d full=%d bias=%d legacy=%d seed=%d errors=%d\n",
        errors ? "FAIL" : "PASS",m,n,k,full,bias,legacy,seed,errors);
    fflush(stdout);
    return errors != 0;
}

int main(void)
{
    printf("[INFO] LayerMatmul build=layer-busy-v2 trace=%d\n", GEMMINI_LAYER_TEST_TRACE);
    fflush(stdout);
    diag_stage("MAIN_ENTER build=layer-busy-v2");
#if GEMMINI_LAYER_TEST_TRACE
    printf("[LAYER-DIAG] buffers A=%lu B=%lu C=%lu D=%lu bytes\n",
        (unsigned long)sizeof aa, (unsigned long)sizeof bb,
        (unsigned long)sizeof cc, (unsigned long)sizeof dd);
    fflush(stdout);
#endif
    int failed = 0;
    failed |= check(17,19,37,5,1,1,1,1); // legacy before
    failed |= check(17,30,37,5,1,1,0,2); // all three tails, unaligned DRAM
    failed |= check(33,129,65,7,0,1,0,3); // three J tiles, signed LUT
    failed |= check(97,80,64,9,1,0,0,4); // no bias after a biased layer
    failed |= check(256,128,128,3,0,1,0,5); // representative YOLO tile
    for (int i = 0; i < 3; i++) {
        failed |= check(97,80,64,3,i&1,1,0,6+i); // multi-page/reload stress
        failed |= check(17,19,37,5,1,0,1,8+i); // interleave legacy
    }
    failed |= check(33,64,512,1,0,1,0,12); // full K used by YOLO concat
    puts(failed ? "[FAIL] LayerMatmul correctness" : "[PASS] LayerMatmul correctness");
    fflush(stdout);
    return failed;
}
