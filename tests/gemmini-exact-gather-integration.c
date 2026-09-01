/*
 * Directed Gemmini frontend integration test for the YOLOv8 Exact Gather
 * lockup.  This deliberately tests command transitions rather than sustained
 * Gather throughput.  In particular, an RTL-SiLU layer sends 32 LUT config
 * commands immediately before the first Gather tile of a 1x1 convolution.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <include/gemmini.h>

#if !defined(HAS_EXACT_GATHER)
int main(void)
{
	printf("[FAIL] HAS_EXACT_GATHER is not enabled\n");
	return 1;
}
#elif !defined(HAS_SILU_LUT)
int main(void)
{
	printf("[FAIL] HAS_SILU_LUT is not enabled\n");
	return 1;
}
#elif !defined(HAS_EXACT_RESADD)
int main(void)
{
	printf("[FAIL] HAS_EXACT_RESADD is not enabled\n");
	return 1;
}
#else

#define MAX_ROWS 256
#define MAX_BRANCH_WIDTH 32
#define MAX_COLS (4 * MAX_BRANCH_WIDTH)
#define MAX_SOURCE_STRIDE (2 * MAX_BRANCH_WIDTH)

#define SMALL_ROWS 16
#define WS_I 32
#define WS_J 32
#define WS_K 32
#define CONV_IN_DIM 18
#define CONV_OUT_DIM 16
#define CONV_CHANNELS 16

/*
 * Match run_native_conv_3x3_ws(): one merged spatial tile, two output-channel
 * blocks and both LoopConv accumulator halves.  Keeping pocols/pochs larger
 * than DIM also forces the same four-mvout batch used by production.
 */
#define NATIVE_IN_ROWS 3
#define NATIVE_IN_COLS 34
#define NATIVE_POSITIONS 32
#define NATIVE_CHANNELS 32

static elem_t gather_sources[4][MAX_ROWS * MAX_SOURCE_STRIDE + 64]
	__attribute__((aligned(4096)));
static elem_t gather_output[MAX_ROWS * MAX_COLS]
	__attribute__((aligned(4096)));
static elem_t gather_expected[MAX_ROWS * MAX_COLS]
	__attribute__((aligned(4096)));
static elem_t silu_lut[256] __attribute__((aligned(64)));

static elem_t ws_a[WS_I * WS_K] __attribute__((aligned(64)));
static elem_t ws_b[WS_K * WS_J] __attribute__((aligned(64)));
static elem_t ws_c[WS_I * WS_J] __attribute__((aligned(64)));

static elem_t conv_input[CONV_IN_DIM * CONV_IN_DIM * CONV_CHANNELS]
	__attribute__((aligned(64)));
static elem_t conv_weights[3 * 3 * CONV_CHANNELS * CONV_CHANNELS]
	__attribute__((aligned(64)));
static acc_t conv_bias[CONV_CHANNELS] __attribute__((aligned(64)));
static elem_t conv_output[CONV_OUT_DIM * CONV_OUT_DIM * CONV_CHANNELS]
	__attribute__((aligned(64)));

static elem_t native_input[NATIVE_IN_ROWS * NATIVE_IN_COLS *
	NATIVE_CHANNELS] __attribute__((aligned(4096)));
static elem_t native_weights[3 * 3 * NATIVE_CHANNELS * NATIVE_CHANNELS]
	__attribute__((aligned(4096)));
static acc_t native_bias[NATIVE_CHANNELS] __attribute__((aligned(64)));
static elem_t native_retire[NATIVE_POSITIONS * NATIVE_CHANNELS]
	__attribute__((aligned(4096)));
static elem_t native_twoscale_output[2][NATIVE_POSITIONS * NATIVE_CHANNELS]
	__attribute__((aligned(4096)));

static elem_t resadd_a[DIM * DIM] row_align(MAX_BYTES);
static elem_t resadd_b[DIM * DIM] row_align(MAX_BYTES);
static elem_t resadd_c[DIM * DIM] row_align(MAX_BYTES);

static void stage(const char *name)
{
	printf("[STAGE] %s\n", name);
	fflush(stdout);
}

static elem_t input_value(unsigned branch, unsigned row, unsigned channel,
			  unsigned epoch)
{
	const unsigned value = branch * 71 + row * 29 + channel * 17 +
		epoch * 43;

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
	if (result > 127)
		return (elem_t)127;
	if (result < -127)
		return (elem_t)-127;
	return (elem_t)result;
}

static uint16_t prepare_gather(
	struct gemmini_exact_gather_branch branches[4], uint16_t rows,
	uint8_t branch_count, uint16_t branch_width, unsigned epoch)
{
	static const int32_t multipliers[4] = {7885, 6691, 3809, 2491};
	static const uint8_t shifts[4] = {13, 13, 12, 12};
	const uint16_t cols = branch_count * branch_width;

	memset(gather_sources, 0x5a, sizeof(gather_sources));
	memset(gather_output, 0x33, sizeof(gather_output));
	memset(gather_expected, 0x33, sizeof(gather_expected));
	for (uint8_t branch = 0; branch < branch_count; branch++) {
		elem_t *source;
		uint16_t source_stride;

		if (branch < 2) {
			source = &gather_sources[0][branch * branch_width];
			source_stride = 2 * branch_width;
		} else {
			source = &gather_sources[branch][0];
			source_stride = branch_width;
		}
		branches[branch].src = source;
		branches[branch].src_stride = source_stride;
		branches[branch].channels = branch_width;
		branches[branch].dst_offset = branch * branch_width;
		branches[branch].multiplier = multipliers[branch];
		branches[branch].shift = shifts[branch];

		for (uint16_t row = 0; row < rows; row++) {
			for (uint16_t channel = 0; channel < branch_width;
			     channel++) {
				const elem_t value = input_value(branch, row, channel,
					epoch);
				const size_t dst = (size_t)row * cols +
					(size_t)branch * branch_width + channel;

				source[(size_t)row * source_stride + channel] =
					value;
				gather_expected[dst] = exact_requant(value,
					multipliers[branch], shifts[branch]);
			}
		}
	}
	return cols;
}

static void issue_gather_traced(uint16_t rows, uint16_t cols,
	const struct gemmini_exact_gather_branch branches[4],
	uint8_t branch_count)
{
	stage("gather bounds begin");
	gemmini_config_exact_gather_bounds((uintptr_t)gather_output, rows, cols,
		cols, branch_count, false);
	stage("gather bounds done");
	for (uint8_t branch = 0; branch < branch_count; branch++) {
		printf("[STAGE] gather branch=%u geometry begin\n", branch);
		gemmini_config_exact_gather_branch(branch, branches[branch].src,
			branches[branch].src_stride, branches[branch].channels,
			branches[branch].dst_offset);
		printf("[STAGE] gather branch=%u geometry done\n", branch);
		printf("[STAGE] gather branch=%u scale begin\n", branch);
		gemmini_config_exact_gather_scale(branch,
			branches[branch].multiplier, branches[branch].shift);
		printf("[STAGE] gather branch=%u scale done\n", branch);
	}
	stage("gather execute begin");
	gemmini_exact_gather_execute();
	stage("gather execute done");
	stage("gather fence begin");
	gemmini_fence();
	stage("gather fence done");
}

static int check_gather(uint16_t rows, uint16_t cols, const char *name)
{
	int mismatches = 0;

	for (size_t index = 0; index < (size_t)rows * cols; index++) {
		if (gather_output[index] == gather_expected[index])
			continue;
		if (mismatches < 12)
			printf("%s mismatch row=%lu col=%lu got=%d expected=%d\n",
				name, (unsigned long)(index / cols),
				(unsigned long)(index % cols), gather_output[index],
				gather_expected[index]);
		mismatches++;
	}
	printf("[%s] %s rows=%u cols=%u\n",
		mismatches ? "FAIL" : "PASS", name, rows, cols);
	return mismatches;
}

static void load_lut32(unsigned epoch)
{
	for (unsigned index = 0; index < 256; index++)
		silu_lut[index] = (elem_t)(int8_t)(index + epoch * 17);
	stage("SiLU LUT32 begin");
	gemmini_load_silu_lut(silu_lut);
	stage("SiLU LUT32 done");
}

static int run_lut_gather_case(const char *name, uint16_t rows,
	uint8_t branch_count, uint16_t branch_width, unsigned epoch)
{
	struct gemmini_exact_gather_branch branches[4] = {0};
	uint16_t cols;

	printf("[CASE-BEGIN] %s\n", name);
	cols = prepare_gather(branches, rows, branch_count, branch_width,
		epoch);
	load_lut32(epoch);
	issue_gather_traced(rows, cols, branches, branch_count);
	return check_gather(rows, cols, name);
}

static void initialize_ws(void)
{
	for (size_t index = 0; index < sizeof(ws_a); index++)
		ws_a[index] = (elem_t)(int8_t)(index * 13 + 1);
	memset(ws_b, 0, sizeof(ws_b));
	for (size_t index = 0; index < WS_J; index++)
		ws_b[index * WS_J + index] = 1;
	memset(ws_c, 0, sizeof(ws_c));
}

static void issue_ws_no_fence(void)
{
	stage("WS issue begin");
	tiled_matmul_auto(WS_I, WS_J, WS_K,
		ws_a, ws_b, NULL, ws_c,
		WS_K, WS_J, WS_J, WS_J,
		MVIN_SCALE_IDENTITY, MVIN_SCALE_IDENTITY,
		MVIN_SCALE_IDENTITY, NO_ACTIVATION, ACC_SCALE_IDENTITY, 0,
		false, false, false, false, false, 0, WS);
	stage("WS issue done (no fence)");
}

static void issue_tlb_flush(void)
{
	/*
	 * Production grows the matmul workspace immediately before model.4.cv2.
	 * That path emits FLUSH, then 32 LUT configs and the four-branch Gather.
	 */
	stage("TLB flush issue begin");
	gemmini_flush(0);
	stage("TLB flush issue done");
}

static void initialize_conv(void)
{
	for (size_t index = 0; index < sizeof(conv_input); index++)
		conv_input[index] = (elem_t)(int8_t)(index * 7 + 3);
	for (size_t index = 0; index < sizeof(conv_weights); index++)
		conv_weights[index] = (elem_t)(int8_t)((index % 5) - 2);
	memset(conv_bias, 0, sizeof(conv_bias));
	memset(conv_output, 0, sizeof(conv_output));
}

static void issue_loopconv_no_fence(void)
{
	stage("LoopConv issue begin");
	tiled_conv_auto(1, CONV_IN_DIM, CONV_IN_DIM, CONV_CHANNELS,
		CONV_CHANNELS, CONV_OUT_DIM, CONV_OUT_DIM,
		1, 1, 1, 0, 3,
		false, false, false, false, false,
		conv_input, conv_weights, conv_bias, conv_output,
		NO_ACTIVATION, ACC_SCALE_IDENTITY, 0, 0, 0, WS);
	stage("LoopConv issue done (no fence)");
}

static void initialize_native_twoscale(void)
{
	for (size_t index = 0; index < sizeof(native_input); index++)
		native_input[index] = (elem_t)(int8_t)(index * 11 + 7);
	for (size_t index = 0; index < sizeof(native_weights); index++)
		native_weights[index] = (elem_t)(int8_t)((index % 7) - 3);
	for (size_t index = 0; index < NATIVE_CHANNELS; index++)
		native_bias[index] = (acc_t)((int)index - 16);
	memset(native_retire, 0x44, sizeof(native_retire));
	memset(native_twoscale_output, 0x55,
		sizeof(native_twoscale_output));
}

static void issue_native_twoscale_mvout(unsigned acc_half,
	unsigned output_index)
{
	const uint32_t acc_base =
		(UINT32_C(3) << (ADDR_LEN - 2)) +
		acc_half * (ACC_ROWS / 2);

	printf("[STAGE] native twoscale config half=%u begin\n", acc_half);
	fflush(stdout);
	gemmini_extended_config_st_twoscale_silu_loaded(
		NATIVE_CHANNELS * sizeof(elem_t), ACC_SCALE_IDENTITY);
	printf("[STAGE] native twoscale config half=%u done\n", acc_half);
	fflush(stdout);
	for (unsigned local_oc = 0; local_oc < NATIVE_CHANNELS;
	     local_oc += DIM) {
		for (unsigned pos = 0; pos < NATIVE_POSITIONS; pos += DIM) {
			const uint32_t local_addr = acc_base +
				(local_oc / DIM) * NATIVE_POSITIONS + pos;
			elem_t *dram = native_twoscale_output[output_index] +
				pos * NATIVE_CHANNELS + local_oc;

			gemmini_extended_mvout(dram, local_addr, DIM, DIM);
		}
	}
	printf("[STAGE] native twoscale mvout half=%u done (no fence)\n",
		acc_half);
	fflush(stdout);
}

static void issue_native_twoscale_deferred_pair(void)
{
	/* The first LUT belongs to the native convolution being retired. */
	load_lut32(19);
	stage("native config-ex begin");
	gemmini_extended3_config_ex(WEIGHT_STATIONARY, 0, 0, 0, 1, 1,
		false, false, false);
	stage("native config-ex done");

	for (unsigned acc_half = 0; acc_half < 2; acc_half++) {
		printf("[STAGE] native loopconv half=%u begin\n", acc_half);
		fflush(stdout);
		gemmini_extended_config_st(
			NATIVE_CHANNELS * sizeof(elem_t), NO_ACTIVATION,
			ACC_SCALE_IDENTITY);
		sp_tiled_conv(
			1, NATIVE_IN_ROWS, NATIVE_IN_COLS, NATIVE_CHANNELS,
			NATIVE_CHANNELS, NATIVE_IN_ROWS, NATIVE_POSITIONS,
			NATIVE_IN_ROWS, NATIVE_POSITIONS,
			1, 1, 3, 1,
			NATIVE_CHANNELS, NATIVE_CHANNELS, NATIVE_CHANNELS,
			1, 1, 0,
			1, 1, NATIVE_POSITIONS, NATIVE_CHANNELS,
			3, 3, NATIVE_CHANNELS,
			1, 0, 1, 0,
			0, 0, 0, 0,
			native_input, native_weights, native_retire,
			native_bias, NO_ACTIVATION, ACC_SCALE_IDENTITY,
			false, false, false, false, false,
			false, true, false, false, false, 0, 0);
		printf("[STAGE] native loopconv half=%u issue done\n", acc_half);
		printf("[STAGE] native dependency fence half=%u begin\n",
			acc_half);
		fflush(stdout);
		gemmini_fence();
		printf("[STAGE] native dependency fence half=%u done\n",
			acc_half);
		fflush(stdout);
		issue_native_twoscale_mvout(acc_half, acc_half);
	}
	/* Deliberately no fence: run_prefixed_case() next loads a new LUT. */
	stage("native deferred pair done (final mvout outstanding)");
}

static int check_native_twoscale_pair(void)
{
	int mismatches = 0;
	int writes = 0;

	for (size_t index = 0;
	     index < NATIVE_POSITIONS * NATIVE_CHANNELS; index++) {
		if ((uint8_t)native_twoscale_output[0][index] != UINT8_C(0x55))
			writes++;
		if (native_twoscale_output[0][index] ==
		    native_twoscale_output[1][index])
			continue;
		if (mismatches < 12)
			printf("native half mismatch index=%lu half0=%d half1=%d\n",
				(unsigned long)index,
				native_twoscale_output[0][index],
				native_twoscale_output[1][index]);
		mismatches++;
	}
	if (writes == 0) {
		printf("native twoscale output was not written\n");
		mismatches++;
	}
	printf("[%s] native3x3 twoscale acc-half pair writes=%d/%u "
	       "mismatches=%d\n", mismatches ? "FAIL" : "PASS", writes,
		NATIVE_POSITIONS * NATIVE_CHANNELS, mismatches);
	return mismatches;
}

static void initialize_resadd(void)
{
	for (size_t index = 0; index < DIM * DIM; index++) {
		resadd_a[index] = (elem_t)(int8_t)(index * 3 + 1);
		resadd_b[index] = (elem_t)(int8_t)(255 - index * 5);
		resadd_c[index] = 0;
	}
}

static void issue_resadd_no_fence(void)
{
	stage("Exact ResAdd issue begin");
	gemmini_extended_config_st_exact_resadd(DIM * sizeof(elem_t));
	gemmini_config_ex(WS, 0, 0);
	gemmini_extended4_config_ld_exact(DIM * sizeof(elem_t),
		3809, 12, DIM, 0);
	gemmini_extended4_config_ld_exact(DIM * sizeof(elem_t),
		6691, 13, DIM, 1);
	gemmini_loop_ws_exact_resadd(1, 1, 0, 0,
		resadd_a, resadd_b, resadd_c, DIM, DIM, DIM);
	stage("Exact ResAdd issue done (no fence)");
}

static int run_prefixed_case(const char *name, void (*prefix)(void),
	unsigned epoch)
{
	struct gemmini_exact_gather_branch branches[4] = {0};
	const uint16_t cols = prepare_gather(branches, SMALL_ROWS, 4,
		MAX_BRANCH_WIDTH, epoch);

	printf("[CASE-BEGIN] %s\n", name);
	prefix();
	load_lut32(epoch);
	issue_gather_traced(SMALL_ROWS, cols, branches, 4);
	return check_gather(SMALL_ROWS, cols, name);
}

int main(void)
{
	int mismatches = 0;

	gemmini_flush(0);
	initialize_ws();
	initialize_conv();
	initialize_native_twoscale();
	initialize_resadd();

	/* model.2-equivalent transition: 32 LUT configs then 3 branches. */
	mismatches += run_lut_gather_case("lut32-to-gather3", SMALL_ROWS,
		3, 16, 1);
	/* model.4 transition with the fourth branch, but a fast row count. */
	if (!mismatches)
		mismatches += run_lut_gather_case("lut32-to-gather4-small",
			SMALL_ROWS, 4, MAX_BRANCH_WIDTH, 2);
	if (!mismatches)
		mismatches += run_prefixed_case("ws-lut32-gather4",
			issue_ws_no_fence, 3);
	if (!mismatches)
		mismatches += run_prefixed_case("flush-lut32-gather4",
			issue_tlb_flush, 4);
	/*
	 * Exact production transition: native LoopConv accumulator halves 0/1,
	 * Two-Scale SiLU batch mvout with the fence deferred, next-layer LUT32,
	 * then C2f Gather4.
	 */
	if (!mismatches) {
		mismatches += run_prefixed_case(
			"native3x3-twoscale-deferred-lut32-gather4",
			issue_native_twoscale_deferred_pair, 5);
		if (!mismatches)
			mismatches += check_native_twoscale_pair();
	}
	if (!mismatches)
		mismatches += run_prefixed_case("loopconv-lut32-gather4",
			issue_loopconv_no_fence, 6);
	if (!mismatches)
		mismatches += run_prefixed_case("resadd-lut32-gather4",
			issue_resadd_no_fence, 7);
	/* Exact board failure geometry runs once, after fast transition tests. */
	if (!mismatches)
		mismatches += run_lut_gather_case("lut32-to-gather4-yolo",
			MAX_ROWS, 4, MAX_BRANCH_WIDTH, 8);

	if (mismatches) {
		printf("[FAIL] Exact Gather integration mismatches=%d\n",
			mismatches);
		return 1;
	}
	printf("[PASS] Exact Gather YOLO frontend integration sequence\n");
	return 0;
}
#endif
