/* bench_rec.c — kernel join vs hand-rolled baseline (parity gate).
 * Two sealed rec_sets joined via the kernel sorted merge vs a hand-rolled
 * corm join (iterate A, corm_get membership in B). Asserts identical match
 * counts and records wall time for both paths. */

#include "./../include/ttypt/rec.h"
#include "./../include/ttypt/corm.h"

#include <stdio.h>
#include <stdlib.h>
#include <time.h>

static double
now_sec(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

static void
build(rec_set_t *s, uint64_t start, uint64_t step, uint64_t n)
{
	for (uint64_t i = 0; i < n; i++)
		rec_set_push(s, start + i * step);
	rec_set_seal(s);
}

int
main(void)
{
	const uint64_t N = 10000u;
	rec_set_t *a = rec_set_new(), *b = rec_set_new();
	build(a, 0, 1, N);          /* 0..9999 */
	build(b, N / 2, 2, N);      /* 5000..29998 step 2 → evens in 5000..9999 */

	/* kernel path: sealed merge-join, no corm involvement */
	double t0 = now_sec();
	rec_set_t *k = rec_set_new();
	rec_set_intersect(k, a, b);
	double tk = now_sec() - t0;

	/* hand-rolled path: CM_SORTED maps, iterate A + corm_get in B */
	uint32_t kt = corm_reg(sizeof(rec_ref_t));
	uint32_t hda = corm_open(NULL, NULL, kt, kt, 0xFFFF, CM_SORTED);
	uint32_t hdb = corm_open(NULL, NULL, kt, kt, 0xFFFF, CM_SORTED);
	const rec_ref_t *aa = rec_set_at(a), *ba = rec_set_at(b);
	for (size_t i = 0; i < rec_set_count(a); i++)
		corm_put(hda, &aa[i], &aa[i]);
	for (size_t i = 0; i < rec_set_count(b); i++)
		corm_put(hdb, &ba[i], &ba[i]);

	double t1 = now_sec();
	uint32_t cur = corm_iter(hda, NULL, 0);
	const void *key, *value;
	size_t qnext = 0, matches = 0;
	while (corm_next(&key, &value, cur)) {
		qnext++;
		if (corm_get(hdb, key) != NULL)
			matches++;
	}
	corm_fin(cur);
	double th = now_sec() - t1;

	size_t kernel_n = rec_set_count(k);
	printf("kernel join      : %9zu matches   %7.2f ms\n", kernel_n, tk * 1e3);
	printf("hand-rolled join : %9zu matches   %7.2f ms (corm_next %zu, corm_get %zu)\n",
	       matches, th * 1e3, qnext, qnext);
	printf("corm_next count  : kernel %zu (merge, zero scans) vs hand-rolled %zu\n",
	       kernel_n, qnext);

	int ok = kernel_n == matches;
	printf(ok ? "PARITY OK (identical match counts)\n"
	          : "PARITY FAILED\n");
	return ok ? 0 : 1;
}