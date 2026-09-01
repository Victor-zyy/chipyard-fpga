/* Verilator/board directed test for native RTL Exact Gather-Requant DMA. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <include/gemmini.h>

#ifndef HAS_EXACT_GATHER
int main(void)
{
	printf("[FAIL] HAS_EXACT_GATHER is not enabled\n");
	return 1;
}
#else

#define MAX_ROWS 97
#define MAX_STRIDE 80
#define MAX_COLS 64
#define GUARD_BYTES 37

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
	gemmini_loop_exact_gather((uintptr_t)output, test->rows,
		(uint16_t)total_cols, dst_stride, branches,
		test->branch_count, false);
	gemmini_fence();

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
	printf("[%s] RTL gather %-24s branches=%u shape=%ux%lu\n",
		mismatches ? "FAIL" : "PASS", test->name,
		test->branch_count, test->rows, (unsigned long)total_cols);
	return mismatches;
}

int main(void)
{
	int mismatches = 0;
	size_t checked = 0;

	gemmini_flush(0);
	for (size_t index = 0; index < sizeof(cases) / sizeof(cases[0]); index++)
		mismatches += run_case(&cases[index], &checked);
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
