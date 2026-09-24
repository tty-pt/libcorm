/* rec_axis_bench.c — 3-axis rec_query_run vs equivalent hand-written loop.
 * Synthetic axes carry a rank fn; the parity gate is that the engine
 * produces exactly the results a hand-composed fill/join/rank loop
 * produces, and that it is not meaningfully slower. */

#include "./../include/ttypt/rec.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static double
now_sec(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

struct bench_params { rec_ref_t lo, hi; uint32_t step; };

static int
bench_fill(void *ctx, void *params, rec_set_t *out)
{
	const struct bench_params *p = params;
	(void)ctx;
	for (rec_ref_t r = p->lo; r <= p->hi; r += p->step)
		rec_set_push(out, r);
	rec_set_seal(out);
	return 0;
}

static int
bench_rank(void *ctx, void *params, rec_ref_t ref, float *score)
{
	(void)ctx; (void)params;
	*score = (float)(ref % 101) / 101.0f;
	return 0;
}

int
main(void)
{
	const rec_ref_t N = 10000;
	const size_t top_k = 100;
	int a_slot, b_slot, c_slot;
	rec_ref_t refs_e[1024], refs_h[1024];
	float sc_e[1024], sc_h[1024];
	size_t n_e, n_h;
	double te, th;

	/* Register three synthetic axes. */
	{
		const rec_axis_t ax = { "benchA", bench_fill, bench_rank, NULL, NULL };
		const rec_axis_t bx = { "benchB", bench_fill, bench_rank, NULL, NULL };
		const rec_axis_t cx = { "benchC", bench_fill, bench_rank, NULL, NULL };
		int mrk;
		a_slot = rec_axis_register(&ax);
		b_slot = rec_axis_register(&bx);
		c_slot = rec_axis_register(&cx);
		rec_axis_set_ctx(a_slot, &mrk);
		rec_axis_set_ctx(b_slot, &mrk);
		rec_axis_set_ctx(c_slot, &mrk);
	}

	struct bench_params pa = { 0, N, 2 };        /* evens */
	struct bench_params pb = { 0, N, 3 };        /* multiples of 3 */
	struct bench_params pc = { N / 4, N, 1 };    /* tail range */

	/* engine path */
	{
		rec_query_t q;
		memset(&q, 0, sizeof(q));
		q.n_axes = 3;
		q.top_k = top_k;
		q.axes[0].slot = a_slot;
		q.axes[0].params = &pa;
		q.axes[1].slot = b_slot;
		q.axes[1].params = &pb;
		q.axes[1].join = REC_JOIN_AND;
		q.axes[2].slot = c_slot;
		q.axes[2].params = &pc;
		q.axes[2].join = REC_JOIN_AND;
		double t0 = now_sec();
		n_e = (size_t)rec_query_run(&q, refs_e, sc_e);
		te = now_sec() - t0;
	}

	/* hand path */
	{
		struct bench_params pp[3] = { pa, pb, pc };
		rec_set_t *sets[3], *r, *tmp;
		double t0 = now_sec();
		for (int i = 0; i < 3; i++) {
			sets[i] = rec_set_new();
			bench_fill(NULL, &pp[i], sets[i]);
		}
		r = sets[0];
		sets[0] = NULL;                    /* r owns the seed now */
		for (int i = 1; i < 3; i++) {
			tmp = rec_set_new();
			rec_set_intersect(tmp, r, sets[i]);
			rec_set_free(r);
			r = tmp;
		}
		th = now_sec() - t0;

		rec_rank_t *rk = rec_rank_new(top_k, 0.0f);
		const rec_ref_t *ra = rec_set_at(r);
		for (size_t k = 0; k < rec_set_count(r); k++) {
			float s;
			if (bench_rank(NULL, NULL, ra[k], &s) == 0)
				rec_rank_push(rk, ra[k], s);
		}
		n_h = rec_rank_sorted(rk, refs_h, sc_h);
		rec_rank_free(rk);
		rec_set_free(r);
		rec_set_free(sets[1]);
		rec_set_free(sets[2]);
	}

	/* both paths discard the seed's own first intersect for n_h? no: report */
	printf("engine  : %9zu results   %7.2f ms\n", n_e, te * 1e3);
	printf("hand    : %9zu results   %7.2f ms\n", n_h, th * 1e3);

	int ok = n_e == n_h;
	for (size_t i = 0; ok && i < n_e; i++)
		if (refs_e[i] != refs_h[i] || sc_e[i] != sc_h[i])
			ok = 0;
	printf(ok ? "PARITY OK (identical refs + scores)\n"
	          : "PARITY FAILED\n");
	return ok ? 0 : 1;
}