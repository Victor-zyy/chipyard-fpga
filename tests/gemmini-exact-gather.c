/* Verilator/board directed test for native RTL Exact Gather-Requant DMA. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#if defined(__linux__) && !defined(GEMMINI_EXACT_GATHER_USE_CSR_TIMER)
#include <time.h>
#endif

#include <include/gemmini.h>

#ifndef HAS_EXACT_GATHER
int main(void)
{
	printf("[FAIL] HAS_EXACT_GATHER is not enabled\n");
	return 1;
}
#else

#define MAX_ROWS 256
#define MAX_STRIDE 80
#define MAX_COLS 160
#define GUARD_BYTES 37

#define YOLO_STRESS_ROWS 256
#define YOLO_STRESS_COLS 128
#define YOLO_STRESS_BRANCHES 4
#define YOLO_STRESS_BRANCH_COLS 32
#define YOLO_STRESS_REPEATS 19
#define YOLO_STRESS_MATMUL_COLS 64

#if defined(__linux__) && !defined(GEMMINI_EXACT_GATHER_USE_CSR_TIMER)
#define GATHER_TIMER_UNIT "ns"

static inline uint64_t read_target_ticks(void)
{
	struct timespec timestamp;

	if (clock_gettime(CLOCK_MONOTONIC_RAW, &timestamp) != 0)
		return 0;
	return (uint64_t)timestamp.tv_sec * UINT64_C(1000000000) +
		(uint64_t)timestamp.tv_nsec;
}
#else
#define GATHER_TIMER_UNIT "cycles"

static inline uint64_t read_target_ticks(void)
{
	uint64_t cycles;

	__asm__ volatile ("rdcycle %0" : "=r" (cycles));
	return cycles;
}
#endif

struct gather_case {
	const char *name;
	uint16_t rows;
	uint8_t branch_count;
	uint16_t channels[4];
	uint16_t strides[4];
	uint16_t source_offsets[4];
	int32_t multipliers[4];
	uint8_t shifts[4];
};

static const struct gather_case cases[] = {
	{
		"two-branch-strided", 17, 2,
		{7, 19, 0, 0}, {11, 23, 0, 0}, {0, 7, 0, 0},
		{1, 7885, 0, 0}, {0, 13, 0, 0}
	},
	{
		"three-branch-mixed", 19, 3,
		{5, 16, 9, 0}, {13, 21, 17, 0}, {3, 11, 19, 0},
		{3809, 6691, 2, 0}, {12, 13, 1, 0}
	},
	{
		"four-branch-multipage", 97, 4,
		{7, 11, 5, 14}, {31, 43, 29, 61}, {1, 9, 23, 31},
		{2491, 26111, 1851, -1499}, {12, 15, 10, 10}
	},
};

static elem_t sources[4][MAX_ROWS * MAX_STRIDE + GUARD_BYTES]
	__attribute__((aligned(4096)));
static elem_t output[MAX_ROWS * MAX_COLS + GUARD_BYTES]
	__attribute__((aligned(4096)));
static elem_t expected[MAX_ROWS * MAX_COLS + GUARD_BYTES]
	__attribute__((aligned(4096)));
static elem_t stress_weights[YOLO_STRESS_COLS * YOLO_STRESS_MATMUL_COLS]
	__attribute__((aligned(4096)));
static elem_t stress_matmul_output[
	YOLO_STRESS_ROWS * YOLO_STRESS_MATMUL_COLS]
	__attribute__((aligned(4096)));
static elem_t stress_matmul_expected[
	YOLO_STRESS_ROWS * YOLO_STRESS_MATMUL_COLS]
	__attribute__((aligned(4096)));

static uint64_t run_gather_chunks(uintptr_t dst, uint16_t rows,
	uint16_t cols, uint16_t dst_stride,
	const struct gemmini_exact_gather_branch *branches,
	uint8_t branch_count)
{
	uint32_t fragments_per_row = 0;
	uint32_t rows_per_command;
	uint64_t total_ticks = 0;

	for (uint8_t branch = 0; branch < branch_count; branch++)
		fragments_per_row += (branches[branch].channels + DIM - 1) / DIM;
	rows_per_command = GEMMINI_EXACT_GATHER_MAX_FRAGMENTS_PER_COMMAND /
		fragments_per_row;
	if (rows_per_command == 0)
		rows_per_command = 1;

	for (uint32_t row_base = 0; row_base < rows;) {
		const uint32_t rows_left = (uint32_t)rows - row_base;
		const uint16_t chunk_rows = (uint16_t)(
			rows_left < rows_per_command ? rows_left : rows_per_command);
		const uintptr_t chunk_dst = dst +
			(uintptr_t)row_base * dst_stride * sizeof(elem_t);
		uint64_t start_ticks;
		uint64_t end_ticks;

		printf("[CHUNK-BEGIN] rows=%lu..%lu dst_page_off=%lu src_page_off=",
			(unsigned long)row_base,
			(unsigned long)(row_base + chunk_rows - 1),
			(unsigned long)(chunk_dst & 4095));
		for (uint8_t branch = 0; branch < branch_count; branch++) {
			const uintptr_t chunk_src = (uintptr_t)(branches[branch].src +
				(size_t)row_base * branches[branch].src_stride);
			printf("%s%lu", branch == 0 ? "[" : ",",
				(unsigned long)(chunk_src & 4095));
		}
		printf("]\n");

		start_ticks = read_target_ticks();
		gemmini_exact_gather_issue_rows(dst, (uint16_t)row_base,
			chunk_rows, cols, dst_stride, branches, branch_count, false);
		gemmini_fence();
		end_ticks = read_target_ticks();
		total_ticks += end_ticks - start_ticks;
		printf("[CHUNK-DONE ] rows=%lu..%lu %s=%lu\n",
			(unsigned long)row_base,
			(unsigned long)(row_base + chunk_rows - 1),
			GATHER_TIMER_UNIT,
			(unsigned long)(end_ticks - start_ticks));
		row_base += chunk_rows;
	}

	return total_ticks;
}

static elem_t input_value(size_t branch, size_t row, size_t channel)
{
	const unsigned value = (unsigned)(branch * 71 + row * 29 +
		channel * 17);

	if (value % 31 == 0)
		return (elem_t)-128;
	if (value % 37 == 0)
		return (elem_t)127;
	return (elem_t)(int8_t)((value % 255) - 127);
}

static elem_t exact_requant(elem_t input, int32_t multiplier, uint8_t shift)
{
	const int64_t product = (int64_t)input * multiplier;
	int64_t result;

	if (shift == 0) {
		result = product;
	} else {
		const uint64_t magnitude = product < 0 ?
			(uint64_t)-product : (uint64_t)product;
		const uint64_t rounded =
			(magnitude + (UINT64_C(1) << (shift - 1))) >> shift;
		result = product < 0 ? -(int64_t)rounded : (int64_t)rounded;
	}
	if (result < -127)
		return (elem_t)-127;
	if (result > 127)
		return (elem_t)127;
	return (elem_t)result;
}

static int run_case(const struct gather_case *test, size_t *checked)
{
	struct gemmini_exact_gather_branch branches[4] = {0};
	size_t total_cols = 0;
	uint16_t dst_stride;
	uint64_t start_ticks;
	uint64_t end_ticks;
	int mismatches = 0;

	for (size_t branch = 0; branch < test->branch_count; branch++)
		total_cols += test->channels[branch];
	dst_stride = (uint16_t)(total_cols + 7);

	memset(sources, 0x5a, sizeof(sources));
	memset(output, 0x33, sizeof(output));
	memset(expected, 0x33, sizeof(expected));
	total_cols = 0;
	for (size_t branch = 0; branch < test->branch_count; branch++) {
		elem_t *source = &sources[branch][test->source_offsets[branch]];

		branches[branch].src = source;
		branches[branch].src_stride = test->strides[branch];
		branches[branch].channels = test->channels[branch];
		branches[branch].dst_offset = (uint16_t)total_cols;
		branches[branch].multiplier = test->multipliers[branch];
		branches[branch].shift = test->shifts[branch];
		for (size_t row = 0; row < test->rows; row++) {
			for (size_t channel = 0;
			     channel < test->channels[branch]; channel++) {
				const elem_t value = input_value(branch, row, channel);

				source[row * test->strides[branch] + channel] = value;
				expected[row * dst_stride + total_cols + channel] =
					exact_requant(value, test->multipliers[branch],
						test->shifts[branch]);
			}
		}
		total_cols += test->channels[branch];
	}

	printf("[STAGE] native gather %-24s branches=%u rows=%u cols=%lu\n",
		test->name, test->branch_count, test->rows,
		(unsigned long)total_cols);
	start_ticks = read_target_ticks();
	end_ticks = start_ticks + run_gather_chunks((uintptr_t)output, test->rows,
		(uint16_t)total_cols, dst_stride, branches,
		test->branch_count);

	for (size_t row = 0; row < test->rows; row++) {
		for (size_t column = 0; column < dst_stride; column++) {
			const size_t index = row * dst_stride + column;

			if (output[index] == expected[index])
				continue;
			if (mismatches < 12)
				printf("%s mismatch row=%lu col=%lu got=%d expected=%d%s\n",
					test->name, (unsigned long)row,
					(unsigned long)column, output[index],
					expected[index],
					column >= total_cols ? " (padding)" : "");
			mismatches++;
		}
	}
	*checked += (size_t)test->rows * total_cols;
	printf("[%s] RTL gather %-24s branches=%u shape=%ux%lu %s=%lu\n",
		mismatches ? "FAIL" : "PASS", test->name,
		test->branch_count, test->rows, (unsigned long)total_cols,
		GATHER_TIMER_UNIT,
		(unsigned long)(end_ticks - start_ticks));
	return mismatches;
}

/*
 * Reproduce model.4.cv2's live C2f matrix exactly: 256 position rows,
 * 128 contiguous output columns and four 32-channel branches. The first two
 * branches are channel slices of one 64-channel NHWC tensor, just as they are
 * in YOLOv8 C2f; the remaining branches are independent 32-channel tensors.
 */
static void prepare_yolo_stress(
	struct gemmini_exact_gather_branch branches[YOLO_STRESS_BRANCHES],
	unsigned epoch)
{
	static const int32_t multipliers[YOLO_STRESS_BRANCHES] = {
		7885, 6691, 3809, 2491
	};
	static const uint8_t shifts[YOLO_STRESS_BRANCHES] = {
		13, 13, 12, 12
	};

	memset(sources, 0x5a, sizeof(sources));
	memset(expected, 0x33, sizeof(expected));
	for (size_t branch = 0; branch < YOLO_STRESS_BRANCHES; branch++) {
		elem_t *source;
		uint16_t stride;

		if (branch < 2) {
			source = &sources[0][branch * YOLO_STRESS_BRANCH_COLS];
			stride = 2 * YOLO_STRESS_BRANCH_COLS;
		} else {
			source = &sources[branch][0];
			stride = YOLO_STRESS_BRANCH_COLS;
		}
		branches[branch].src = source;
		branches[branch].src_stride = stride;
		branches[branch].channels = YOLO_STRESS_BRANCH_COLS;
		branches[branch].dst_offset =
			branch * YOLO_STRESS_BRANCH_COLS;
		branches[branch].multiplier = multipliers[branch];
		branches[branch].shift = shifts[branch];

		for (size_t row = 0; row < YOLO_STRESS_ROWS; row++) {
			for (size_t channel = 0;
			     channel < YOLO_STRESS_BRANCH_COLS; channel++) {
				const elem_t value = input_value(branch,
					row + epoch * 13, channel + epoch * 7);
				const size_t output_index =
					row * YOLO_STRESS_COLS +
					branch * YOLO_STRESS_BRANCH_COLS + channel;

				source[row * stride + channel] = value;
				expected[output_index] = exact_requant(value,
					multipliers[branch], shifts[branch]);
			}
		}
	}
}

static int check_yolo_gather_output(const char *phase, unsigned iteration)
{
	int mismatches = 0;

	for (size_t index = 0;
	     index < YOLO_STRESS_ROWS * YOLO_STRESS_COLS; index++) {
		if (output[index] == expected[index])
			continue;
		if (mismatches < 12)
			printf("%s iteration=%u mismatch row=%lu col=%lu got=%d expected=%d\n",
				phase, iteration,
				(unsigned long)(index / YOLO_STRESS_COLS),
				(unsigned long)(index % YOLO_STRESS_COLS),
				output[index], expected[index]);
		mismatches++;
	}
	return mismatches;
}

static int run_yolo_gather_stress(size_t *checked)
{
	struct gemmini_exact_gather_branch branches[YOLO_STRESS_BRANCHES] = {0};
	int mismatches = 0;

	printf("[STAGE] YOLO gather-only stress rows=%u cols=%u branches=%u repeats=%u fragments=2048\n",
		YOLO_STRESS_ROWS, YOLO_STRESS_COLS, YOLO_STRESS_BRANCHES,
		YOLO_STRESS_REPEATS);
	for (unsigned iteration = 0; iteration < YOLO_STRESS_REPEATS;
	     iteration++) {
		prepare_yolo_stress(branches, iteration);
		memset(output, 0x33, sizeof(output));
		printf("[STRESS-GATHER-BEGIN] iteration=%u/%u\n",
			iteration + 1, YOLO_STRESS_REPEATS);
		gemmini_exact_gather_issue_rows((uintptr_t)output, 0,
			YOLO_STRESS_ROWS, YOLO_STRESS_COLS, YOLO_STRESS_COLS,
			branches, YOLO_STRESS_BRANCHES, false);
		printf("[STRESS-GATHER-ISSUE-DONE] iteration=%u/%u\n",
			iteration + 1, YOLO_STRESS_REPEATS);
		gemmini_fence();
		printf("[STRESS-GATHER-FENCE-DONE] iteration=%u/%u\n",
			iteration + 1, YOLO_STRESS_REPEATS);
		mismatches += check_yolo_gather_output("gather-only",
			iteration);
		*checked += YOLO_STRESS_ROWS * YOLO_STRESS_COLS;
		if (mismatches)
			break;
	}
	printf("[%s] YOLO gather-only stress repeats=%u\n",
		mismatches ? "FAIL" : "PASS", YOLO_STRESS_REPEATS);
	return mismatches;
}

static void initialize_stress_weights(void)
{
	memset(stress_weights, 0, sizeof(stress_weights));
	for (size_t index = 0; index < YOLO_STRESS_MATMUL_COLS; index++)
		stress_weights[index * YOLO_STRESS_MATMUL_COLS + index] = 1;
}

static void snapshot_stress_matmul_expected(void)
{
	for (size_t row = 0; row < YOLO_STRESS_ROWS; row++)
		memcpy(&stress_matmul_expected[
			row * YOLO_STRESS_MATMUL_COLS],
			&output[row * YOLO_STRESS_COLS],
			YOLO_STRESS_MATMUL_COLS * sizeof(elem_t));
}

static int check_stress_matmul_output(unsigned iteration)
{
	int mismatches = 0;

	for (size_t index = 0;
	     index < YOLO_STRESS_ROWS * YOLO_STRESS_MATMUL_COLS; index++) {
		if (stress_matmul_output[index] ==
		    stress_matmul_expected[index])
			continue;
		if (mismatches < 12)
			printf("interleave-matmul iteration=%u mismatch row=%lu col=%lu got=%d expected=%d\n",
				iteration,
				(unsigned long)(index / YOLO_STRESS_MATMUL_COLS),
				(unsigned long)(index % YOLO_STRESS_MATMUL_COLS),
				stress_matmul_output[index],
				stress_matmul_expected[index]);
		mismatches++;
	}
	return mismatches;
}

/*
 * Match the production scheduling hazard: WS consumes the shared A matrix,
 * then the next four-branch Gather rewrites that same matrix with no fence in
 * between. One final fence waits for both engines, exactly as the current
 * YOLO replacement path does.
 */
static int run_yolo_interleave_stress(size_t *checked)
{
	struct gemmini_exact_gather_branch branches[YOLO_STRESS_BRANCHES] = {0};
	int mismatches = 0;

	printf("[STAGE] YOLO WS/Gather interleave rows=%u K=%u J=%u repeats=%u\n",
		YOLO_STRESS_ROWS, YOLO_STRESS_COLS,
		YOLO_STRESS_MATMUL_COLS, YOLO_STRESS_REPEATS);
	initialize_stress_weights();
	prepare_yolo_stress(branches, 0);
	memset(output, 0x33, sizeof(output));
	gemmini_exact_gather_issue_rows((uintptr_t)output, 0,
		YOLO_STRESS_ROWS, YOLO_STRESS_COLS, YOLO_STRESS_COLS,
		branches, YOLO_STRESS_BRANCHES, false);
	gemmini_fence();

	for (unsigned iteration = 0; iteration < YOLO_STRESS_REPEATS;
	     iteration++) {
		snapshot_stress_matmul_expected();
		memset(stress_matmul_output, 0x33,
			sizeof(stress_matmul_output));
		printf("[INTERLEAVE-WS-BEGIN] iteration=%u/%u\n",
			iteration + 1, YOLO_STRESS_REPEATS);
		tiled_matmul_auto(YOLO_STRESS_ROWS,
			YOLO_STRESS_MATMUL_COLS, YOLO_STRESS_COLS,
			output, stress_weights, NULL, stress_matmul_output,
			YOLO_STRESS_COLS, YOLO_STRESS_MATMUL_COLS,
			YOLO_STRESS_MATMUL_COLS, YOLO_STRESS_MATMUL_COLS,
			MVIN_SCALE_IDENTITY, MVIN_SCALE_IDENTITY,
			MVIN_SCALE_IDENTITY, NO_ACTIVATION,
			ACC_SCALE_IDENTITY, 0, false, false, false,
			false, false, 0, WS);
		printf("[INTERLEAVE-WS-ISSUE-DONE] iteration=%u/%u\n",
			iteration + 1, YOLO_STRESS_REPEATS);

		prepare_yolo_stress(branches, iteration + 1);
		printf("[INTERLEAVE-GATHER-BEGIN] iteration=%u/%u\n",
			iteration + 1, YOLO_STRESS_REPEATS);
		gemmini_exact_gather_issue_rows((uintptr_t)output, 0,
			YOLO_STRESS_ROWS, YOLO_STRESS_COLS, YOLO_STRESS_COLS,
			branches, YOLO_STRESS_BRANCHES, false);
		printf("[INTERLEAVE-GATHER-ISSUE-DONE] iteration=%u/%u\n",
			iteration + 1, YOLO_STRESS_REPEATS);
		gemmini_fence();
		printf("[INTERLEAVE-FENCE-DONE] iteration=%u/%u\n",
			iteration + 1, YOLO_STRESS_REPEATS);

		mismatches += check_stress_matmul_output(iteration);
		mismatches += check_yolo_gather_output("interleave-gather",
			iteration);
		*checked += YOLO_STRESS_ROWS *
			(YOLO_STRESS_MATMUL_COLS + YOLO_STRESS_COLS);
		if (mismatches)
			break;
	}
	printf("[%s] YOLO WS/Gather interleave repeats=%u\n",
		mismatches ? "FAIL" : "PASS", YOLO_STRESS_REPEATS);
	return mismatches;
}

int main(void)
{
	int mismatches = 0;
	size_t checked = 0;

	gemmini_flush(0);
	for (size_t index = 0; index < sizeof(cases) / sizeof(cases[0]); index++)
		mismatches += run_case(&cases[index], &checked);
	if (!mismatches)
		mismatches += run_yolo_gather_stress(&checked);
	if (!mismatches)
		mismatches += run_yolo_interleave_stress(&checked);
	if (mismatches) {
		printf("[FAIL] Gemmini RTL Exact Gather mismatches=%d\n", mismatches);
		return 1;
	}
	printf("[PASS] Gemmini native RTL Exact Gather: %lu/%lu values exact\n",
		(unsigned long)checked, (unsigned long)checked);
	printf("       direct DMA, 2/3/4 branches, strided and multi-page paths passed\n");
	return 0;
}
#endif
