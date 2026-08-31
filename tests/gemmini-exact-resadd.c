#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <include/gemmini.h>

struct fixed_edge {
	int32_t multiplier;
	uint32_t shift;
};

struct edge_pair {
	struct fixed_edge a;
	struct fixed_edge b;
};

static const struct edge_pair cases[] = {
	{{2, 1}, {7885, 13}},
	{{3809, 12}, {6691, 13}},
	{{2491, 12}, {26111, 15}},
	{{2, 1}, {1851, 10}},
	{{2, 1}, {2671, 11}},
	{{2, 1}, {1499, 10}},
};

static elem_t a[DIM][DIM] row_align(MAX_BYTES);
static elem_t b[DIM][DIM] row_align(MAX_BYTES);
static elem_t got[DIM][DIM] row_align(MAX_BYTES);

static int32_t requant(elem_t value, const struct fixed_edge *edge)
{
	const int64_t product = (int64_t)value * edge->multiplier;
	uint64_t magnitude;
	uint64_t rounded;

	if (!edge->shift)
		return (int32_t)product;
	magnitude = product < 0 ? (uint64_t)-product : (uint64_t)product;
	rounded = (magnitude + (UINT64_C(1) << (edge->shift - 1))) >>
		edge->shift;
	return product < 0 ? -(int32_t)rounded : (int32_t)rounded;
}

static elem_t reference(elem_t av, elem_t bv, const struct edge_pair *pair)
{
	int32_t sum = requant(av, &pair->a) + requant(bv, &pair->b);

	if (sum > 127)
		sum = 127;
	if (sum < -127)
		sum = -127;
	return (elem_t)sum;
}

int main(void)
{
	int mismatches = 0;

#ifndef HAS_EXACT_RESADD
	printf("[FAIL] HAS_EXACT_RESADD is not enabled\n");
	return 1;
#endif

	for (size_t row = 0; row < DIM; row++) {
		for (size_t col = 0; col < DIM; col++) {
			const unsigned index = row * DIM + col;
			a[row][col] = (elem_t)(int8_t)index;
			b[row][col] = (elem_t)(int8_t)(255U - index);
		}
	}

	for (size_t test = 0; test < sizeof(cases) / sizeof(cases[0]); test++) {
		memset(got, 0, sizeof(got));
		gemmini_extended_config_st_exact_resadd(DIM * sizeof(elem_t));
		gemmini_config_ex(WS, 0, 0);
		gemmini_extended4_config_ld_exact(DIM * sizeof(elem_t),
			cases[test].a.multiplier, cases[test].a.shift, DIM, 0);
		gemmini_extended4_config_ld_exact(DIM * sizeof(elem_t),
			cases[test].b.multiplier, cases[test].b.shift, DIM, 1);
		gemmini_loop_ws_exact_resadd(1, 1, 0, 0, a, b, got,
			DIM, DIM, DIM);
		gemmini_fence();

		for (size_t row = 0; row < DIM; row++) {
			for (size_t col = 0; col < DIM; col++) {
				const elem_t expected = reference(a[row][col],
					b[row][col], &cases[test]);
				if (got[row][col] != expected) {
					if (mismatches < 16)
						printf("case=%zu row=%zu col=%zu got=%d expected=%d\n",
							test, row, col, got[row][col], expected);
					mismatches++;
				}
			}
		}
	}

	if (mismatches) {
		printf("[FAIL] Gemmini exact ResAdd mismatches=%d\n", mismatches);
		return 1;
	}
	printf("[PASS] Gemmini exact ResAdd: 6 C2f edge pairs, %d/%d values exact\n",
		(int)(sizeof(cases) / sizeof(cases[0]) * DIM * DIM),
		(int)(sizeof(cases) / sizeof(cases[0]) * DIM * DIM));
	return 0;
}
