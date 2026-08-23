/*
 * Verilator-directed RTL test for Gemmini's programmable Two-Scale SiLU path.
 *
 * This test intentionally exercises the real RoCC/store/accumulator-scale RTL;
 * it is not a unit test of the C lookup helper.
 *
 * Coverage:
 *   1. legacy full-INT32/INT8 accumulator mvout before and after SiLU;
 *   2. legacy WS full-INT32 matmul before and after SiLU;
 *   3. legacy WS 3x3 LoopConv before and after SiLU;
 *   4. all 256 signed INT8 q_mid bit patterns;
 *   5. signed LUT output and ordered LUT replacement;
 *   6. non-identity FP32 scaling, RNE and INT8 saturation.
 */

#include <limits.h>
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "include/gemmini.h"

#ifndef GEMMINI_COMPAT_ENABLE_SILU
#define GEMMINI_COMPAT_ENABLE_SILU 1
#endif

#if GEMMINI_COMPAT_ENABLE_SILU && !defined(HAS_SILU_LUT)
#error "Rebuild the Verilator simulator with Ku5pGemminiConfigs.has_silu_lut enabled"
#endif

#define TEST_SCALE_TO_MID 0.2f
#define TEST_MID_SCALE 0.125f
#define TEST_OUTPUT_SCALE 0.0625f
#define MAX_REPORTED_MISMATCHES 16

#define MAT_I (DIM + 1)
#define MAT_J (DIM + 3)
#define MAT_K (DIM + 2)

#define CONV_BATCH 1
#define CONV_IN_H 7
#define CONV_IN_W 7
#define CONV_IN_C DIM
#define CONV_OUT_C (2 * DIM)
#define CONV_KERNEL 3
#define CONV_PADDING 1
#define CONV_STRIDE 1
#define CONV_OUT_H CONV_IN_H
#define CONV_OUT_W CONV_IN_W
#define CONV_PATCH (CONV_KERNEL * CONV_KERNEL * CONV_IN_C)
#define CONV_POINTS (CONV_BATCH * CONV_OUT_H * CONV_OUT_W)

#define MP_MAT_I 32
#define MP_MAT_J 32
#define MP_MAT_K (9 * DIM)

#define CONV1_OUT_C DIM
#define CONV1_POINTS (CONV_IN_H * CONV_IN_W)

#define SMALL_CONV_IN_C 4
#define SMALL_CONV_OUT_C 8
#define SMALL_CONV_PATCH (CONV_KERNEL * CONV_KERNEL * SMALL_CONV_IN_C)
#define SMALL_CONV_PAD0_OUT_H (CONV_IN_H - CONV_KERNEL + 1)
#define SMALL_CONV_PAD0_OUT_W (CONV_IN_W - CONV_KERNEL + 1)
#define SMALL_CONV_PAD0_POINTS \
  (SMALL_CONV_PAD0_OUT_H * SMALL_CONV_PAD0_OUT_W)
#define SMALL_CONV_PAD1_POINTS (CONV_IN_H * CONV_IN_W)

static acc_t identity_input[DIM][DIM] row_align_acc(1);
static acc_t scaled_input[DIM][DIM] row_align_acc(1);

static acc_t legacy_full_before[DIM][DIM] row_align_acc(1);
static acc_t legacy_full_after[DIM][DIM] row_align_acc(1);
static elem_t legacy_int8_before[DIM][DIM] row_align(1);
static elem_t legacy_int8_after[DIM][DIM] row_align(1);

static elem_t matmul_a[MAT_I][MAT_K] row_align(1);
static elem_t matmul_b[MAT_K][MAT_J] row_align(1);
static acc_t matmul_gold[MAT_I][MAT_J] row_align_acc(1);
static acc_t matmul_before[MAT_I][MAT_J] row_align_acc(1);
static acc_t matmul_after[MAT_I][MAT_J] row_align_acc(1);

/* The 4608-byte A and B operands force Linux/Gemmini DMA across 4KB pages. */
static elem_t mp_matmul_a[MP_MAT_I][MP_MAT_K] __attribute__((aligned(4096)));
static elem_t mp_matmul_b[MP_MAT_K][MP_MAT_J] __attribute__((aligned(4096)));
static acc_t mp_matmul_gold[MP_MAT_I][MP_MAT_J]
    __attribute__((aligned(4096)));
static acc_t mp_matmul_output[MP_MAT_I][MP_MAT_J]
    __attribute__((aligned(4096)));

static elem_t conv_input[CONV_BATCH][CONV_IN_H][CONV_IN_W][CONV_IN_C]
    row_align(1);
static elem_t conv_weights[CONV_PATCH][CONV_OUT_C] row_align(1);
static elem_t conv_weights_aligned[CONV_PATCH][CONV_OUT_C]
    __attribute__((aligned(4096)));
static acc_t conv_bias[CONV_OUT_C] row_align_acc(1);
static acc_t conv_full_gold[CONV_POINTS][CONV_OUT_C] row_align_acc(1);
static acc_t conv_full_output[CONV_POINTS][CONV_OUT_C]
    __attribute__((aligned(4096)));
static elem_t conv_gold[CONV_POINTS][CONV_OUT_C] row_align(1);
static elem_t conv_full_retire[CONV_POINTS][CONV_OUT_C] row_align(1);
static elem_t conv_before[CONV_POINTS][CONV_OUT_C] row_align(1);
static elem_t conv_after[CONV_POINTS][CONV_OUT_C] row_align(1);
static elem_t conv_fenced_output[CONV_POINTS][CONV_OUT_C] row_align(1);
static elem_t conv_aligned_output[CONV_POINTS][CONV_OUT_C]
    __attribute__((aligned(4096)));

static elem_t conv1_weights[CONV_IN_C][CONV1_OUT_C] row_align(1);
static acc_t conv1_bias[CONV1_OUT_C] row_align_acc(1);
static elem_t conv1_gold[CONV1_POINTS][CONV1_OUT_C] row_align(1);
static elem_t conv1_output[CONV1_POINTS][CONV1_OUT_C] row_align(1);

static elem_t small_conv_input[CONV_IN_H][CONV_IN_W][SMALL_CONV_IN_C]
    row_align(1);
static elem_t small_conv_weights[SMALL_CONV_PATCH][SMALL_CONV_OUT_C]
    row_align(1);
static acc_t small_conv_bias[SMALL_CONV_OUT_C] row_align_acc(1);
static elem_t small_conv_pad0_gold[SMALL_CONV_PAD0_POINTS][SMALL_CONV_OUT_C]
    row_align(1);
static elem_t small_conv_pad0_output[SMALL_CONV_PAD0_POINTS][SMALL_CONV_OUT_C]
    row_align(1);
static elem_t small_conv_pad1_gold[SMALL_CONV_PAD1_POINTS][SMALL_CONV_OUT_C]
    row_align(1);
static elem_t small_conv_pad1_output[SMALL_CONV_PAD1_POINTS][SMALL_CONV_OUT_C]
    row_align(1);

static elem_t identity_silu_output[DIM][DIM] row_align(1);
static elem_t identity_signature_output[DIM][DIM] row_align(1);
static elem_t scaled_silu_output[DIM][DIM] row_align(1);

static elem_t silu_lut[256];
static elem_t signature_lut[256];

/*
 * Linux may initially map untouched BSS through a shared read-only zero page.
 * Gemmini DMA must not become the first writer, because the accelerator cannot
 * service the CPU page fault needed to create a private writable page. Touch
 * the first byte of every page and the final byte through a volatile pointer.
 */
static void pretouch_dma_output(void *buffer, size_t bytes)
{
  volatile uint8_t *data = (volatile uint8_t *)buffer;
  const size_t page_bytes = 4096;
  size_t offset;

  for (offset = 0; offset < bytes; offset += page_bytes)
    data[offset] = 0;
  if (bytes != 0)
    data[bytes - 1] = 0;
}

static void report_stage(const char *name)
{
  printf("[STAGE] %s\n", name);
  fflush(stdout);
}

static int32_t round_near_even(float x)
{
  const int32_t base = (int32_t)x;
  const int32_t next = x < 0.0f ? base - 1 : base + 1;
  float remainder = x - (float)base;

  if (remainder < 0.0f)
    remainder = -remainder;
  if (remainder < 0.5f)
    return base;
  if (remainder > 0.5f)
    return next;
  return (base & 1) == 0 ? base : next;
}

static elem_t saturate_int8(int32_t x)
{
  if (x > INT8_MAX)
    return INT8_MAX;
  if (x < INT8_MIN)
    return INT8_MIN;
  return (elem_t)x;
}

static elem_t scale_to_mid_reference(acc_t x)
{
  return saturate_int8(
      round_near_even((float)x * TEST_SCALE_TO_MID));
}

static void build_conv_reference(
    const elem_t *input, int in_h, int in_w, int in_c,
    const elem_t *weights, const acc_t *bias, int out_c,
    int kernel, int padding, int out_h, int out_w,
    elem_t *output)
{
  int out_row;

  for (out_row = 0; out_row < out_h; out_row++) {
    int out_col;

    for (out_col = 0; out_col < out_w; out_col++) {
      int out_channel;

      for (out_channel = 0; out_channel < out_c; out_channel++) {
        acc_t sum = bias == NULL ? 0 : bias[out_channel];
        int kernel_row;

        for (kernel_row = 0; kernel_row < kernel; kernel_row++) {
          int kernel_col;

          for (kernel_col = 0; kernel_col < kernel; kernel_col++) {
            const int input_row = out_row + kernel_row - padding;
            const int input_col = out_col + kernel_col - padding;
            int in_channel;

            if (input_row < 0 || input_row >= in_h ||
                input_col < 0 || input_col >= in_w)
              continue;
            for (in_channel = 0; in_channel < in_c; in_channel++) {
              const size_t input_index =
                  ((size_t)input_row * in_w + input_col) * in_c + in_channel;
              const size_t weight_index =
                  (((size_t)kernel_row * kernel + kernel_col) * in_c +
                   in_channel) * out_c + out_channel;

              sum += (acc_t)input[input_index] * weights[weight_index];
            }
          }
        }
        output[((size_t)out_row * out_w + out_col) * out_c + out_channel] =
            saturate_int8(sum);
      }
    }
  }
}

static void build_test_tables(void)
{
  for (int raw = 0; raw < 256; raw++) {
    const int q_mid = raw < 128 ? raw : raw - 256;
    const float x = (float)q_mid * TEST_MID_SCALE;
    const float y = x / (1.0f + expf(-x));

    silu_lut[raw] =
        saturate_int8(round_near_even(y / TEST_OUTPUT_SCALE));

    /*
     * This permutation deliberately produces both positive and negative
     * output bytes and makes adjacent indices unrelated. It catches index
     * truncation, signed-index mistakes, byte-lane reversal and stale chunks.
     */
    signature_lut[raw] = (elem_t)(uint8_t)(raw * 73u + 19u);
  }
}

static void build_inputs(void)
{
  for (size_t row = 0; row < DIM; row++) {
    for (size_t col = 0; col < DIM; col++) {
      const int index = (int)(row * DIM + col);
      const int q_mid = index - 128;

      /* With acc_scale=1.0, these cover q_mid=-128...127 exactly once. */
      identity_input[row][col] = (acc_t)q_mid;

      /*
       * A nonuniform wider range exercises FP32 scaling, RNE ties and both
       * saturation limits before the LUT lookup.
       */
      scaled_input[row][col] =
          (acc_t)(q_mid * 7 + ((index % 5) - 2));

      identity_silu_output[row][col] = 0;
      identity_signature_output[row][col] = 0;
      scaled_silu_output[row][col] = 0;
    }
  }
}

static void build_legacy_references(void)
{
  size_t row;

  for (row = 0; row < MAT_I; row++) {
    size_t k;

    for (k = 0; k < MAT_K; k++)
      matmul_a[row][k] = (elem_t)(((row * 3 + k * 5) % 7) - 3);
  }
  for (row = 0; row < MAT_K; row++) {
    size_t col;

    for (col = 0; col < MAT_J; col++)
      matmul_b[row][col] = (elem_t)(((row * 2 + col * 3) % 5) - 2);
  }
  for (row = 0; row < MAT_I; row++) {
    size_t col;

    for (col = 0; col < MAT_J; col++) {
      acc_t sum = 0;
      size_t k;

      for (k = 0; k < MAT_K; k++)
        sum += (acc_t)matmul_a[row][k] * (acc_t)matmul_b[k][col];
      matmul_gold[row][col] = sum;
      matmul_before[row][col] = 0;
      matmul_after[row][col] = 0;
    }
  }

  for (row = 0; row < CONV_IN_H; row++) {
    size_t col;

    for (col = 0; col < CONV_IN_W; col++) {
      size_t channel;

      for (channel = 0; channel < CONV_IN_C; channel++)
        conv_input[0][row][col][channel] =
            (elem_t)(((row * 5 + col * 3 + channel * 2) % 7) - 3);
    }
  }
  for (row = 0; row < CONV_PATCH; row++) {
    size_t channel;

    for (channel = 0; channel < CONV_OUT_C; channel++)
      conv_weights[row][channel] =
          (elem_t)(((row * 3 + channel * 5) % 5) - 2);
    for (channel = 0; channel < CONV_OUT_C; channel++)
      conv_weights_aligned[row][channel] = conv_weights[row][channel];
  }
  for (row = 0; row < CONV_OUT_C; row++)
    conv_bias[row] = (acc_t)((row % 7) - 3);

  for (row = 0; row < CONV_OUT_H; row++) {
    size_t col;

    for (col = 0; col < CONV_OUT_W; col++) {
      size_t out_channel;
      const size_t point = row * CONV_OUT_W + col;

      for (out_channel = 0; out_channel < CONV_OUT_C; out_channel++) {
        acc_t sum = conv_bias[out_channel];
        size_t kernel_row;

        for (kernel_row = 0; kernel_row < CONV_KERNEL; kernel_row++) {
          size_t kernel_col;

          for (kernel_col = 0; kernel_col < CONV_KERNEL; kernel_col++) {
            const int input_row = (int)row + (int)kernel_row - CONV_PADDING;
            const int input_col = (int)col + (int)kernel_col - CONV_PADDING;
            size_t in_channel;

            if (input_row < 0 || input_row >= CONV_IN_H ||
                input_col < 0 || input_col >= CONV_IN_W)
              continue;
            for (in_channel = 0; in_channel < CONV_IN_C; in_channel++) {
              const size_t patch =
                  (kernel_row * CONV_KERNEL + kernel_col) * CONV_IN_C +
                  in_channel;
              sum += (acc_t)conv_input[0][input_row][input_col][in_channel] *
                     (acc_t)conv_weights[patch][out_channel];
            }
          }
        }
        conv_full_gold[point][out_channel] = sum;
        conv_gold[point][out_channel] = saturate_int8(sum);
        conv_full_output[point][out_channel] = 0;
        conv_full_retire[point][out_channel] = 0;
        conv_before[point][out_channel] = 0;
        conv_after[point][out_channel] = 0;
        conv_fenced_output[point][out_channel] = 0;
        conv_aligned_output[point][out_channel] = 0;
      }
    }
  }
}

static void build_linux_diagnostic_references(void)
{
  size_t row;

  for (row = 0; row < MP_MAT_I; row++) {
    size_t k;

    for (k = 0; k < MP_MAT_K; k++)
      mp_matmul_a[row][k] =
          (elem_t)(((row * 11 + k * 7) % 9) - 4);
  }
  for (row = 0; row < MP_MAT_K; row++) {
    size_t col;

    for (col = 0; col < MP_MAT_J; col++)
      mp_matmul_b[row][col] =
          (elem_t)(((row * 5 + col * 13) % 7) - 3);
  }
  for (row = 0; row < MP_MAT_I; row++) {
    size_t col;

    for (col = 0; col < MP_MAT_J; col++) {
      acc_t sum = 0;
      size_t k;

      for (k = 0; k < MP_MAT_K; k++)
        sum += (acc_t)mp_matmul_a[row][k] * mp_matmul_b[k][col];
      mp_matmul_gold[row][col] = sum;
      mp_matmul_output[row][col] = 0;
    }
  }

  for (row = 0; row < CONV_IN_C; row++) {
    size_t out_channel;

    for (out_channel = 0; out_channel < CONV1_OUT_C; out_channel++)
      conv1_weights[row][out_channel] =
          (elem_t)(((row * 7 + out_channel * 3) % 7) - 3);
  }
  for (row = 0; row < CONV1_OUT_C; row++)
    conv1_bias[row] = (acc_t)((row % 5) - 2);
  build_conv_reference(
      &conv_input[0][0][0][0], CONV_IN_H, CONV_IN_W, CONV_IN_C,
      &conv1_weights[0][0], conv1_bias, CONV1_OUT_C,
      1, 0, CONV_IN_H, CONV_IN_W, &conv1_gold[0][0]);

  for (row = 0; row < CONV_IN_H; row++) {
    size_t col;

    for (col = 0; col < CONV_IN_W; col++) {
      size_t channel;

      for (channel = 0; channel < SMALL_CONV_IN_C; channel++)
        small_conv_input[row][col][channel] =
            (elem_t)(((row * 3 + col * 5 + channel * 7) % 9) - 4);
    }
  }
  for (row = 0; row < SMALL_CONV_PATCH; row++) {
    size_t out_channel;

    for (out_channel = 0; out_channel < SMALL_CONV_OUT_C; out_channel++)
      small_conv_weights[row][out_channel] =
          (elem_t)(((row * 5 + out_channel * 3) % 7) - 3);
  }
  for (row = 0; row < SMALL_CONV_OUT_C; row++)
    small_conv_bias[row] = (acc_t)((row % 5) - 2);

  build_conv_reference(
      &small_conv_input[0][0][0], CONV_IN_H, CONV_IN_W, SMALL_CONV_IN_C,
      &small_conv_weights[0][0], small_conv_bias, SMALL_CONV_OUT_C,
      CONV_KERNEL, 0, SMALL_CONV_PAD0_OUT_H, SMALL_CONV_PAD0_OUT_W,
      &small_conv_pad0_gold[0][0]);
  build_conv_reference(
      &small_conv_input[0][0][0], CONV_IN_H, CONV_IN_W, SMALL_CONV_IN_C,
      &small_conv_weights[0][0], small_conv_bias, SMALL_CONV_OUT_C,
      CONV_KERNEL, 1, CONV_IN_H, CONV_IN_W,
      &small_conv_pad1_gold[0][0]);
}

static int check_acc_values(const char *name, const acc_t *actual,
                            const acc_t *expected, size_t count)
{
  int mismatches = 0;
  size_t i;

  for (i = 0; i < count; i++) {
    if (actual[i] != expected[i]) {
      if (mismatches < MAX_REPORTED_MISMATCHES)
        printf("%s mismatch[%u]: got=%d expected=%d\n", name,
               (unsigned)i, (int)actual[i], (int)expected[i]);
      mismatches++;
    }
  }
  return mismatches;
}

static int check_elem_values(const char *name, const elem_t *actual,
                             const elem_t *expected, size_t count)
{
  int mismatches = 0;
  size_t i;

  for (i = 0; i < count; i++) {
    if (actual[i] != expected[i]) {
      if (mismatches < MAX_REPORTED_MISMATCHES)
        printf("%s mismatch[%u]: got=%d expected=%d\n", name,
               (unsigned)i, (int)actual[i], (int)expected[i]);
      mismatches++;
    }
  }
  return mismatches;
}

static int check_identity_output(
    const char *name,
    elem_t output[DIM][DIM],
    const elem_t lut[256])
{
  int mismatches = 0;

  for (size_t row = 0; row < DIM; row++) {
    for (size_t col = 0; col < DIM; col++) {
      const int q_mid = (int)(row * DIM + col) - 128;
      const elem_t expected = lut[(uint8_t)q_mid];
      const elem_t actual = output[row][col];

      if (actual != expected) {
        if (mismatches < MAX_REPORTED_MISMATCHES) {
          printf("%s mismatch[%u,%u]: q_mid=%d raw=0x%02x got=%d expected=%d\n",
              name, (unsigned)row, (unsigned)col, q_mid,
              (unsigned)(uint8_t)q_mid, actual, expected);
        }
        mismatches++;
      }
    }
  }

  return mismatches;
}

static int check_scaled_output(void)
{
  int mismatches = 0;

  for (size_t row = 0; row < DIM; row++) {
    for (size_t col = 0; col < DIM; col++) {
      const elem_t q_mid = scale_to_mid_reference(scaled_input[row][col]);
      const elem_t expected = silu_lut[(uint8_t)q_mid];
      const elem_t actual = scaled_silu_output[row][col];

      if (actual != expected) {
        if (mismatches < MAX_REPORTED_MISMATCHES) {
          printf("scaled mismatch[%u,%u]: acc=%d q_mid=%d raw=0x%02x got=%d expected=%d\n",
              (unsigned)row, (unsigned)col, scaled_input[row][col],
              q_mid, (unsigned)(uint8_t)q_mid, actual, expected);
        }
        mismatches++;
      }
    }
  }

  return mismatches;
}

static void run_legacy_direct(acc_t full_output[DIM][DIM],
                              elem_t int8_output[DIM][DIM])
{
  const uint32_t acc_addr = (uint32_t)1 << (ADDR_LEN - 1);
  const uint32_t full_acc_addr =
      acc_addr | ((uint32_t)1 << (ADDR_LEN - 3));

  gemmini_config_ld(DIM * sizeof(acc_t));
  gemmini_mvin(identity_input, acc_addr);
  gemmini_extended_config_st(DIM * sizeof(acc_t),
      NO_ACTIVATION, ACC_SCALE_IDENTITY);
  gemmini_mvout(full_output, full_acc_addr);
  gemmini_extended_config_st(DIM * sizeof(elem_t),
      NO_ACTIVATION, ACC_SCALE_IDENTITY);
  gemmini_mvout(int8_output, acc_addr);
  gemmini_fence();
}

static int check_legacy_direct(const char *phase,
                               acc_t full_output[DIM][DIM],
                               elem_t int8_output[DIM][DIM])
{
  int mismatches;
  size_t row;

  mismatches = check_acc_values(phase, &full_output[0][0],
      &identity_input[0][0], DIM * DIM);
  for (row = 0; row < DIM; row++) {
    size_t col;

    for (col = 0; col < DIM; col++) {
      const elem_t expected = (elem_t)identity_input[row][col];

      if (int8_output[row][col] != expected) {
        if (mismatches < MAX_REPORTED_MISMATCHES)
          printf("%s-int8 mismatch[%u,%u]: got=%d expected=%d\n", phase,
                 (unsigned)row, (unsigned)col,
                 (int)int8_output[row][col], (int)expected);
        mismatches++;
      }
    }
  }
  return mismatches;
}

static void run_legacy_matmul(acc_t output[MAT_I][MAT_J])
{
  tiled_matmul_auto(MAT_I, MAT_J, MAT_K,
      (const elem_t *)matmul_a, (const elem_t *)matmul_b,
      NULL, output,
      MAT_K, MAT_J, MAT_J, MAT_J,
      MVIN_SCALE_IDENTITY, MVIN_SCALE_IDENTITY, MVIN_SCALE_IDENTITY,
      NO_ACTIVATION, ACC_SCALE_IDENTITY, 0, false,
      false, false, true, false, 0, WS);
  gemmini_fence();
}

static void run_multipage_matmul(void)
{
  __asm__ volatile("fence rw,rw" ::: "memory");
  tiled_matmul_auto(MP_MAT_I, MP_MAT_J, MP_MAT_K,
      (const elem_t *)mp_matmul_a, (const elem_t *)mp_matmul_b,
      NULL, mp_matmul_output,
      MP_MAT_K, MP_MAT_J, MP_MAT_J, MP_MAT_J,
      MVIN_SCALE_IDENTITY, MVIN_SCALE_IDENTITY, MVIN_SCALE_IDENTITY,
      NO_ACTIVATION, ACC_SCALE_IDENTITY, 0, false,
      false, false, true, false, 0, WS);
  gemmini_fence();
  __asm__ volatile("" ::: "memory");
}

static void run_conv_case(
    const elem_t *input, int in_h, int in_w, int in_c,
    const elem_t *weights, const acc_t *bias, int out_c,
    int kernel, int padding, int out_h, int out_w,
    elem_t *output)
{
  __asm__ volatile("fence rw,rw" ::: "memory");
  tiled_conv_auto(
      1, in_h, in_w, in_c,
      out_c, out_h, out_w,
      1, 1, 1, padding, kernel,
      false, false, false, false, false,
      input, weights, bias, output,
      NO_ACTIVATION, ACC_SCALE_IDENTITY, 0, 0, 0, WS);
  gemmini_fence();
  __asm__ volatile("" ::: "memory");
}

static void report_linux_dma_layout(void)
{
  printf("[INFO] DMA page offsets: mp_A=%lu mp_B=%lu "
         "conv_W=%lu conv_W_4k=%lu\n",
         (unsigned long)((uintptr_t)mp_matmul_a & 4095u),
         (unsigned long)((uintptr_t)mp_matmul_b & 4095u),
         (unsigned long)((uintptr_t)conv_weights & 4095u),
         (unsigned long)((uintptr_t)conv_weights_aligned & 4095u));
  printf("[INFO] DMA operand bytes: mp_A=%lu mp_B=%lu "
         "conv_W=%lu\n",
         (unsigned long)sizeof(mp_matmul_a),
         (unsigned long)sizeof(mp_matmul_b),
         (unsigned long)sizeof(conv_weights));
  fflush(stdout);
}

/*
 * Issue one complete LoopConv tile into the first accumulator half, retire its
 * normal INT8 output, then read the same accumulator rows back as full INT32.
 * This mirrors YOLO's native-3x3 path and separates LoopConv computation from
 * the scaled INT8 StoreController/AccumulatorScale path.
 *
 * This must be the first sp_tiled_conv() call in the process because that
 * helper rotates its private C accumulator base after every retiring call.
 */
static void run_loopconv_full_int32(void)
{
  const uint32_t full_acc_base =
      (UINT32_C(3) << (ADDR_LEN - 2)) |
      (UINT32_C(1) << (ADDR_LEN - 3));
  size_t out_channel;

  __asm__ volatile("fence rw,rw" ::: "memory");
  gemmini_extended_config_st(
      CONV_OUT_C * sizeof(elem_t), NO_ACTIVATION, ACC_SCALE_IDENTITY);
  gemmini_extended3_config_ex(
      WEIGHT_STATIONARY, 0, 0, 0, 1, 1, false, false, false);
  sp_tiled_conv(
      CONV_BATCH, CONV_IN_H, CONV_IN_W, CONV_IN_C,
      CONV_OUT_C, CONV_OUT_H, CONV_OUT_W, CONV_OUT_H, CONV_OUT_W,
      CONV_STRIDE, CONV_PADDING, CONV_KERNEL, 1,
      CONV_IN_C, CONV_OUT_C, CONV_OUT_C,
      1, 1, 0,
      CONV_BATCH, CONV_OUT_H, CONV_OUT_W, CONV_OUT_C,
      CONV_KERNEL, CONV_KERNEL, CONV_IN_C,
      CONV_PADDING, CONV_PADDING, CONV_PADDING, CONV_PADDING,
      0, 0, 0, 0,
      &conv_input[0][0][0][0], &conv_weights[0][0],
      &conv_full_retire[0][0], conv_bias,
      NO_ACTIVATION, ACC_SCALE_IDENTITY,
      false, false, false, false, false,
      false, true, false, false, false, 1, 1);
  gemmini_fence();
  __asm__ volatile("" ::: "memory");

  gemmini_extended_config_st(
      CONV_OUT_C * sizeof(acc_t), NO_ACTIVATION, ACC_SCALE_IDENTITY);
  for (out_channel = 0; out_channel < CONV_OUT_C; out_channel += DIM) {
    size_t position;
    const size_t cols =
        CONV_OUT_C - out_channel < DIM ? CONV_OUT_C - out_channel : DIM;

    for (position = 0; position < CONV_POINTS; position += DIM) {
      const size_t rows =
          CONV_POINTS - position < DIM ? CONV_POINTS - position : DIM;
      const uint32_t local_addr = full_acc_base +
          (uint32_t)(out_channel / DIM) * CONV_POINTS + (uint32_t)position;
      acc_t *dram = &conv_full_output[position][out_channel];

      gemmini_extended_mvout(dram, local_addr, cols, rows);
    }
  }
  gemmini_fence();
  __asm__ volatile("" ::: "memory");
}

static void run_legacy_conv(elem_t output[CONV_POINTS][CONV_OUT_C])
{
  tiled_conv_auto(
      CONV_BATCH, CONV_IN_H, CONV_IN_W, CONV_IN_C,
      CONV_OUT_C, CONV_OUT_H, CONV_OUT_W,
      CONV_STRIDE, 1, 1, CONV_PADDING, CONV_KERNEL,
      false, false, false, false, false,
      (const elem_t *)conv_input,
      (const elem_t *)conv_weights,
      (const acc_t *)conv_bias,
      (elem_t *)output,
      NO_ACTIVATION, ACC_SCALE_IDENTITY, 0, 0, 0, WS);
  gemmini_fence();
}

static void run_silu_suite(void)
{
  const uint32_t acc_addr = (uint32_t)1 << (ADDR_LEN - 1);

  gemmini_config_ld(DIM * sizeof(acc_t));
  gemmini_mvin(identity_input, acc_addr);

  /* The public LUT-load helper fences before each table replacement. */
  gemmini_extended_config_st_twoscale_silu(
      DIM * sizeof(elem_t), 1.0f, silu_lut);
  gemmini_mvout(identity_silu_output, acc_addr);
  gemmini_extended_config_st_twoscale_silu(
      DIM * sizeof(elem_t), 1.0f, signature_lut);
  gemmini_mvout(identity_signature_output, acc_addr);

  gemmini_mvin(scaled_input, acc_addr);
  gemmini_extended_config_st_twoscale_silu(
      DIM * sizeof(elem_t), TEST_SCALE_TO_MID, silu_lut);
  gemmini_mvout(scaled_silu_output, acc_addr);
  gemmini_fence();
}

int main(void)
{
  int direct_before_mismatches;
  int matmul_before_mismatches;
  int loopconv_full_mismatches;
  int conv_before_mismatches;
  int mp_matmul_mismatches;
  int conv1_mismatches;
  int small_conv_pad0_mismatches;
  int small_conv_pad1_mismatches;
  int conv_fenced_mismatches;
  int conv_aligned_mismatches;
  int identity_silu_mismatches;
  int identity_signature_mismatches;
  int scaled_silu_mismatches;
  int direct_after_mismatches;
  int matmul_after_mismatches;
  int conv_after_mismatches;
  int total_mismatches;

  build_test_tables();
  build_inputs();
  build_legacy_references();
  build_linux_diagnostic_references();

  pretouch_dma_output(legacy_full_before, sizeof(legacy_full_before));
  pretouch_dma_output(legacy_full_after, sizeof(legacy_full_after));
  pretouch_dma_output(legacy_int8_before, sizeof(legacy_int8_before));
  pretouch_dma_output(legacy_int8_after, sizeof(legacy_int8_after));
  pretouch_dma_output(matmul_before, sizeof(matmul_before));
  pretouch_dma_output(matmul_after, sizeof(matmul_after));
  pretouch_dma_output(conv_before, sizeof(conv_before));
  pretouch_dma_output(conv_after, sizeof(conv_after));
  pretouch_dma_output(conv_full_output, sizeof(conv_full_output));
  pretouch_dma_output(conv_full_retire, sizeof(conv_full_retire));
  pretouch_dma_output(conv_fenced_output, sizeof(conv_fenced_output));
  pretouch_dma_output(conv_aligned_output, sizeof(conv_aligned_output));
  pretouch_dma_output(mp_matmul_output, sizeof(mp_matmul_output));
  pretouch_dma_output(conv1_output, sizeof(conv1_output));
  pretouch_dma_output(small_conv_pad0_output,
                      sizeof(small_conv_pad0_output));
  pretouch_dma_output(small_conv_pad1_output,
                      sizeof(small_conv_pad1_output));
  pretouch_dma_output(identity_silu_output, sizeof(identity_silu_output));
  pretouch_dma_output(identity_signature_output,
                      sizeof(identity_signature_output));
  pretouch_dma_output(scaled_silu_output, sizeof(scaled_silu_output));

  gemmini_flush(0);
  gemmini_config_ex(WEIGHT_STATIONARY, NO_ACTIVATION, 0);
  report_linux_dma_layout();

  report_stage("legacy-before direct-mvout");
  run_legacy_direct(legacy_full_before, legacy_int8_before);
  direct_before_mismatches = check_legacy_direct(
      "legacy-before-direct", legacy_full_before, legacy_int8_before);

  report_stage("legacy-before ws-matmul");
  run_legacy_matmul(matmul_before);
  matmul_before_mismatches = check_acc_values(
      "legacy-before-matmul", &matmul_before[0][0],
      &matmul_gold[0][0], MAT_I * MAT_J);

  report_stage("linux-loopconv full-int32-accumulator");
  run_loopconv_full_int32();
  loopconv_full_mismatches = check_acc_values(
      "loopconv-full-int32", &conv_full_output[0][0],
      &conv_full_gold[0][0], CONV_POINTS * CONV_OUT_C);

  report_stage("legacy-before loopconv-3x3");
  run_legacy_conv(conv_before);
  conv_before_mismatches = check_elem_values(
      "legacy-before-conv", &conv_before[0][0],
      &conv_gold[0][0], CONV_POINTS * CONV_OUT_C);

  report_stage("linux-control multipage-ws-matmul");
  run_multipage_matmul();
  mp_matmul_mismatches = check_acc_values(
      "multipage-matmul", &mp_matmul_output[0][0],
      &mp_matmul_gold[0][0], MP_MAT_I * MP_MAT_J);

  report_stage("linux-loopconv 1x1-pad0-small");
  run_conv_case(
      &conv_input[0][0][0][0], CONV_IN_H, CONV_IN_W, CONV_IN_C,
      &conv1_weights[0][0], conv1_bias, CONV1_OUT_C,
      1, 0, CONV_IN_H, CONV_IN_W, &conv1_output[0][0]);
  conv1_mismatches = check_elem_values(
      "loopconv-1x1", &conv1_output[0][0],
      &conv1_gold[0][0], CONV1_POINTS * CONV1_OUT_C);

  report_stage("linux-loopconv 3x3-pad0-small");
  run_conv_case(
      &small_conv_input[0][0][0], CONV_IN_H, CONV_IN_W, SMALL_CONV_IN_C,
      &small_conv_weights[0][0], small_conv_bias, SMALL_CONV_OUT_C,
      CONV_KERNEL, 0, SMALL_CONV_PAD0_OUT_H, SMALL_CONV_PAD0_OUT_W,
      &small_conv_pad0_output[0][0]);
  small_conv_pad0_mismatches = check_elem_values(
      "loopconv-3x3-pad0", &small_conv_pad0_output[0][0],
      &small_conv_pad0_gold[0][0],
      SMALL_CONV_PAD0_POINTS * SMALL_CONV_OUT_C);

  report_stage("linux-loopconv 3x3-pad1-small");
  run_conv_case(
      &small_conv_input[0][0][0], CONV_IN_H, CONV_IN_W, SMALL_CONV_IN_C,
      &small_conv_weights[0][0], small_conv_bias, SMALL_CONV_OUT_C,
      CONV_KERNEL, 1, CONV_IN_H, CONV_IN_W,
      &small_conv_pad1_output[0][0]);
  small_conv_pad1_mismatches = check_elem_values(
      "loopconv-3x3-pad1", &small_conv_pad1_output[0][0],
      &small_conv_pad1_gold[0][0],
      SMALL_CONV_PAD1_POINTS * SMALL_CONV_OUT_C);

  report_stage("linux-loopconv 3x3-pad1-large-fenced");
  run_conv_case(
      &conv_input[0][0][0][0], CONV_IN_H, CONV_IN_W, CONV_IN_C,
      &conv_weights[0][0], conv_bias, CONV_OUT_C,
      CONV_KERNEL, CONV_PADDING, CONV_OUT_H, CONV_OUT_W,
      &conv_fenced_output[0][0]);
  conv_fenced_mismatches = check_elem_values(
      "loopconv-large-fenced", &conv_fenced_output[0][0],
      &conv_gold[0][0], CONV_POINTS * CONV_OUT_C);

  report_stage("linux-loopconv 3x3-pad1-large-4k-weight");
  run_conv_case(
      &conv_input[0][0][0][0], CONV_IN_H, CONV_IN_W, CONV_IN_C,
      &conv_weights_aligned[0][0], conv_bias, CONV_OUT_C,
      CONV_KERNEL, CONV_PADDING, CONV_OUT_H, CONV_OUT_W,
      &conv_aligned_output[0][0]);
  conv_aligned_mismatches = check_elem_values(
      "loopconv-large-4k", &conv_aligned_output[0][0],
      &conv_gold[0][0], CONV_POINTS * CONV_OUT_C);

#if GEMMINI_COMPAT_ENABLE_SILU
  report_stage("twoscale-silu");
  run_silu_suite();
  identity_silu_mismatches = check_identity_output(
      "identity-silu", identity_silu_output, silu_lut);
  identity_signature_mismatches = check_identity_output(
      "identity-signature", identity_signature_output, signature_lut);
  scaled_silu_mismatches = check_scaled_output();
#else
  identity_silu_mismatches = 0;
  identity_signature_mismatches = 0;
  scaled_silu_mismatches = 0;
#endif

  report_stage("legacy-after direct-mvout");
  run_legacy_direct(legacy_full_after, legacy_int8_after);
  direct_after_mismatches = check_legacy_direct(
      "legacy-after-direct", legacy_full_after, legacy_int8_after);

  report_stage("legacy-after ws-matmul");
  run_legacy_matmul(matmul_after);
  matmul_after_mismatches = check_acc_values(
      "legacy-after-matmul", &matmul_after[0][0],
      &matmul_gold[0][0], MAT_I * MAT_J);

  report_stage("legacy-after loopconv-3x3");
  run_legacy_conv(conv_after);
  conv_after_mismatches = check_elem_values(
      "legacy-after-conv", &conv_after[0][0],
      &conv_gold[0][0], CONV_POINTS * CONV_OUT_C);

  total_mismatches =
      direct_before_mismatches + matmul_before_mismatches +
      loopconv_full_mismatches + conv_before_mismatches +
      mp_matmul_mismatches +
      conv1_mismatches + small_conv_pad0_mismatches +
      small_conv_pad1_mismatches + conv_fenced_mismatches +
      conv_aligned_mismatches + identity_silu_mismatches +
      identity_signature_mismatches + scaled_silu_mismatches +
      direct_after_mismatches + matmul_after_mismatches +
      conv_after_mismatches;

  printf("legacy-before: direct=%d/%d matmul=%d/%d conv=%d/%d\n",
         direct_before_mismatches, 2 * DIM * DIM,
         matmul_before_mismatches, MAT_I * MAT_J,
         conv_before_mismatches, CONV_POINTS * CONV_OUT_C);
#if GEMMINI_COMPAT_ENABLE_SILU
  printf("twoscale-silu: identity=%d/256 signature=%d/256 scaled=%d/256\n",
         identity_silu_mismatches, identity_signature_mismatches,
         scaled_silu_mismatches);
#else
  printf("twoscale-silu: skipped (hardware LUT compiled out)\n");
#endif
  printf("legacy-after : direct=%d/%d matmul=%d/%d conv=%d/%d\n",
         direct_after_mismatches, 2 * DIM * DIM,
         matmul_after_mismatches, MAT_I * MAT_J,
         conv_after_mismatches, CONV_POINTS * CONV_OUT_C);
  printf("linux-control: multipage-matmul=%d/%d\n",
         mp_matmul_mismatches, MP_MAT_I * MP_MAT_J);
  printf("linux-loopconv: full-int32=%d/%d\n",
         loopconv_full_mismatches, CONV_POINTS * CONV_OUT_C);
  printf("linux-loopconv: 1x1=%d/%d 3x3-pad0-small=%d/%d "
         "3x3-pad1-small=%d/%d\n",
         conv1_mismatches, CONV1_POINTS * CONV1_OUT_C,
         small_conv_pad0_mismatches,
         SMALL_CONV_PAD0_POINTS * SMALL_CONV_OUT_C,
         small_conv_pad1_mismatches,
         SMALL_CONV_PAD1_POINTS * SMALL_CONV_OUT_C);
  printf("linux-loopconv: large-fenced=%d/%d large-4k-weight=%d/%d\n",
         conv_fenced_mismatches, CONV_POINTS * CONV_OUT_C,
         conv_aligned_mismatches, CONV_POINTS * CONV_OUT_C);

  if (total_mismatches != 0) {
    printf("[FAIL] Gemmini RTL SiLU backward compatibility: mismatches=%d\n",
           total_mismatches);
    return 1;
  }

  printf("[PASS] Gemmini RTL SiLU backward compatibility\n");
  printf("legacy full-mvout, WS matmul and LoopConv are exact before and after SiLU\n");
  return 0;
}
