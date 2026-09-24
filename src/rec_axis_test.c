/* rec_axis_test.c — unit tests for the axis registry + query engine
 * (rec_axis.c). Uses mock axes only; no concrete axis library is loaded. */

#include "./../include/ttypt/rec.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#define ARRAY_LEN(a) (sizeof(a) / sizeof((a)[0]))

unsigned errors = 0;

#define PASS() printf("  OK\n")
#define FAIL(msg) do { printf("  FAIL: %s [line %d]\n", msg, __LINE__); errors++; } while(0)
#define ASSERT(cond, msg) do { if (!(cond)) FAIL(msg); else PASS(); } while(0)

/* ── mock axes ─────────────────────────────────────────────────────── */

/* mockA: fixed-list fill (params override) + rank (ref/100). */
struct mock_list { const rec_ref_t *v; size_t n; };
static const rec_ref_t mockA_default[] = { 1, 2, 3, 4, 5 };
static int mockA_fill_calls;
static void *mockA_ctx_seen, *mockA_params_seen;

static int
mockA_fill(void *ctx, void *params, rec_set_t *out)
{
	const struct mock_list *p = params;
	size_t n = p ? p->n : ARRAY_LEN(mockA_default);
	const rec_ref_t *v = p ? p->v : mockA_default;
	for (size_t i = 0; i < n; i++)
		rec_set_push(out, v[i]);
	rec_set_seal(out);
	mockA_fill_calls++;
	mockA_ctx_seen = ctx;
	mockA_params_seen = params;
	return 0;
}

static int
mockA_rank(void *ctx, void *params, rec_ref_t ref, float *score)
{
	(void)ctx; (void)params;
	*score = (float)ref / 100.0f;
	return 0;
}

/* mockB: interval fill (params), no rank. */
struct mock_iv { rec_ref_t lo, hi; };

static int
mockB_fill(void *ctx, void *params, rec_set_t *out)
{
	const struct mock_iv *p = params;
	(void)ctx;
	for (rec_ref_t r = p->lo; r <= p->hi; r++)
		rec_set_push(out, r);
	rec_set_seal(out);
	return 0;
}

/* mockC: fixed approx fill {2,4,6}, borrowed recall 0.8. */
static int
mockC_fill(void *ctx, void *params, rec_set_t *out)
{
	(void)ctx; (void)params;
	rec_set_push(out, 2);
	rec_set_push(out, 4);
	rec_set_push(out, 6);
	rec_set_seal(out);
	rec_set_set_approx(out, REC_SET_APPROX, 0.8f);
	return 0;
}

/* mockD: rank-only axis (no fill), constant score. */
static int
mockD_rank(void *ctx, void *params, rec_ref_t ref, float *score)
{
	(void)ctx; (void)params; (void)ref;
	*score = 1.0f;
	return 0;
}

/* mockE: fill that always fails. */
static int
mockE_fill(void *ctx, void *params, rec_set_t *out)
{
	(void)ctx; (void)params; (void)out;
	return -1;
}

/* mockF: decode-only axis. */
static char mockF_buf[64];

static void *
mockF_decode(const char *s)
{
	snprintf(mockF_buf, sizeof(mockF_buf), "decoded:%s", s);
	return mockF_buf;
}

/* mockG: second rank-less fixed fill {10,11}. */
static int
mockG_fill(void *ctx, void *params, rec_set_t *out)
{
	(void)ctx; (void)params;
	rec_set_push(out, 10);
	rec_set_push(out, 11);
	rec_set_seal(out);
	return 0;
}

/* mockH: fill used only to fill the registry at the end. */
static int
mockH_fill(void *ctx, void *params, rec_set_t *out)
{
	(void)ctx; (void)params;
	rec_set_push(out, 1);
	rec_set_seal(out);
	return 0;
}

/* Consumer that weights two axes' scores (missing/rankless count zero) and
 * records the engine-fed rec_axis_score_t array for post-run assertions. */
struct mock_weight { float wa, wd; };
static int consumer_seen_n;
static rec_axis_score_t consumer_seen[REC_QUERY_MAX_AXES];

static int
weighted_consumer(void *ud, rec_ref_t ref, const rec_axis_score_t *as,
                  int n_as, float *out_score)
{
	const struct mock_weight *w = ud;
	(void)ref;
	*out_score = 0.0f;
	consumer_seen_n = n_as;
	for (int i = 0; i < n_as; i++) {
		consumer_seen[i] = as[i];
		if (as[i].valid)
			*out_score += as[i].score *
				(as[i].slot == 0 ? w->wa : w->wd);
	}
	return 0;
}

static void
set_ctx_if(const char *name, int slot)
{
	if (rec_axis_set_ctx(slot, (void *)(uintptr_t)(slot + 1)) != 0)
		printf("  FAIL: set_ctx %s\n", name);
}

/* Capture slots as the registrations happen, so the suite is robust. */
static int slotA, slotB, slotC, slotD, slotE, slotF, slotG;
static int ctx_marker;

static void
init_pool(void)
{
	const rec_axis_t a = { "mockA", mockA_fill, mockA_rank, NULL, NULL };
	const rec_axis_t b = { "mockB", mockB_fill, NULL, NULL, NULL };
	const rec_axis_t c = { "mockC", mockC_fill, NULL, NULL, NULL };
	const rec_axis_t d = { "mockD", NULL, mockD_rank, NULL, NULL };
	const rec_axis_t e = { "mockE", mockE_fill, NULL, NULL, NULL };
	const rec_axis_t f = { "mockF", NULL, NULL, NULL, mockF_decode };
	slotA = rec_axis_register(&a);
	slotB = rec_axis_register(&b);
	slotC = rec_axis_register(&c);
	slotD = rec_axis_register(&d);
	slotE = rec_axis_register(&e);
	slotF = rec_axis_register(&f);
	set_ctx_if("a", slotA);
	set_ctx_if("b", slotB);
	set_ctx_if("c", slotC);
	set_ctx_if("d", slotD);
	set_ctx_if("e", slotE);
}

static void
reset_query(rec_query_t *q)
{
	memset(q, 0, sizeof(*q));
}

/* ── 2A: registry ──────────────────────────────────────────────────── */

static void
test_registry(void)
{
	printf("=== 2A registry: register/get/set_ctx/count/decode ===\n");
	ASSERT(slotA == 0 && slotB == 1 && slotC == 2 && slotD == 3 &&
	       slotE == 4 && slotF == 5, "pool registered at slots 0..5");
	ASSERT(rec_axis_count() == 6, "six pool axes");
	ASSERT(strcmp(rec_axis_get(0)->name, "mockA") == 0, "slot 0 name");
	ASSERT(strcmp(rec_axis_get(5)->name, "mockF") == 0, "slot 5 name");
	ASSERT(rec_axis_get(6) == NULL, "slot 6 unregistered");
	ASSERT(rec_axis_get(-1) == NULL, "negative slot NULL");
	ASSERT(rec_axis_get(99) == NULL, "out-of-range slot NULL");

	{
		const rec_axis_t gax = { "mockG", mockG_fill, NULL, NULL, NULL };
		slotG = rec_axis_register(&gax);
	}
	ASSERT(slotG == 6, "next slot 6");
	ASSERT(rec_axis_count() == 7, "count 7");
	ASSERT(strcmp(rec_axis_get(slotG)->name, "mockG") == 0, "name copied");
	rec_axis_set_ctx(slotG, &ctx_marker);

	ASSERT(rec_axis_register(NULL) == -1, "NULL register rejected");
	ASSERT(rec_axis_count() == 7, "count unchanged after NULL");

	ASSERT(rec_axis_get(0)->ctx == (void *)(uintptr_t)1, "ctx set at init");
	ASSERT(rec_axis_set_ctx(0, NULL) == 0, "set_ctx to NULL ok");
	ASSERT(rec_axis_get(0)->ctx == NULL, "ctx now NULL");
	ASSERT(rec_axis_set_ctx(0, &ctx_marker) == 0, "set_ctx new value");
	ASSERT(rec_axis_get(0)->ctx == &ctx_marker, "ctx visible via get");
	ASSERT(rec_axis_set_ctx(99, NULL) == -1, "set_ctx invalid slot");

	ASSERT(rec_axis_decode(slotF, "hello") != NULL, "decode present");
	ASSERT(strcmp((char *)rec_axis_decode(slotF, "hello"),
	              "decoded:hello") == 0, "decode result");
	ASSERT(rec_axis_decode(slotF, NULL) == NULL, "decode NULL string");
	ASSERT(rec_axis_decode(slotG, "s") == NULL, "decode absent → NULL");
}

/* ── fill dispatch ─────────────────────────────────────────────────── */

static void
test_fill_dispatch(void)
{
	rec_query_t q;
	rec_ref_t refs[16];

	printf("=== 2A fill dispatch (params + ctx forwarded) ===\n");
	reset_query(&q);
	q.n_axes = 1;
	q.top_k = 100;
	q.axes[0].slot = slotA;
	mockA_fill_calls = 0;
	int n = rec_query_run(&q, refs, NULL);
	ASSERT(n == 5, "default list of 5");
	/* slotA has rank (mockA_rank): refs ranked desc by ref/100 */
	ASSERT(refs[0] == 5 && refs[4] == 1, "fill dispatched, ranked");
	ASSERT(mockA_fill_calls == 1, "fill called once");

	{
		rec_ref_t custom[] = { 7, 9 };
		struct mock_list ml = { custom, 2 };
		reset_query(&q);
		q.n_axes = 1;
		q.top_k = 100;
		q.axes[0].slot = slotA;
		q.axes[0].params = &ml;
		mockA_fill_calls = 0;
		n = rec_query_run(&q, refs, NULL);
		ASSERT(n == 2 && refs[0] == 9 && refs[1] == 7,
		       "params list used, ranked");
		ASSERT(mockA_ctx_seen != NULL, "ctx forwarded");
		ASSERT(mockA_params_seen == &ml, "params forwarded");
	}
}

/* ── single-axis rank ──────────────────────────────────────────────── */

static void
test_single_axis_rank(void)
{
	rec_query_t q;
	rec_ref_t refs[16];
	float sc[16];

	printf("=== 2A single-axis rank (top_k + min_score) ===\n");
	reset_query(&q);
	q.n_axes = 1;
	q.top_k = 16;
	q.min_score = 0.03f;         /* ref/100 ≥ 0.03 → refs 3,4,5 */
	q.axes[0].slot = slotA;
	int n = rec_query_run(&q, refs, sc);
	ASSERT(n == 3, "three above 0.03");
	ASSERT(refs[0] == 5 && refs[1] == 4 && refs[2] == 3, "desc by score");
	ASSERT(sc[0] == 0.05f && sc[1] == 0.04f && sc[2] == 0.03f, "scores aligned");
}

/* ── joins ─────────────────────────────────────────────────────────── */

static void
test_intersect_2(void)
{
	rec_query_t q;

	printf("=== 2A 2-axis intersect ===\n");
	reset_query(&q);
	q.n_axes = 2;
	q.top_k = 64;
	q.axes[0].slot = slotB;                     /* {1..10} */
	q.axes[1].slot = slotB;                     /* {5..20} */
	{
		struct mock_iv i0 = { 1, 10 };
		struct mock_iv i1 = { 5, 20 };
		rec_ref_t refs[16];
		q.axes[0].params = &i0;
		q.axes[1].params = &i1;
		int n = rec_query_run(&q, refs, NULL);
		ASSERT(n == 6, "{1..10} ∩ {5..20} → 6");
		ASSERT(refs[0] == 5 && refs[5] == 10, "5..10");
	}
}

static void
test_intersect_3(void)
{
	rec_query_t q;

	printf("=== 2A 3-axis intersect ===\n");
	reset_query(&q);
	q.n_axes = 3;
	q.top_k = 64;
	q.axes[0].slot = slotB;                     /* {1..10} */
	q.axes[1].slot = slotB;                     /* {5..20} ∩ → {5..10} */
	q.axes[2].slot = slotG;                     /* {10,11} ∩ → {10} */
	q.axes[0].join = q.axes[1].join = q.axes[2].join = 0;
	{
		struct mock_iv i0 = { 1, 10 };
		struct mock_iv i1 = { 5, 20 };
		rec_ref_t refs[16];
		q.axes[0].params = &i0;
		q.axes[1].params = &i1;
		int n = rec_query_run(&q, refs, NULL);
		ASSERT(n == 1 && refs[0] == 10, "{1..10} ∩ {5..20} ∩ {10,11} → {10}");
	}
}

static void
test_union_or(void)
{
	rec_query_t q;

	printf("=== 2A OR union (dedup) ===\n");
	reset_query(&q);
	q.n_axes = 2;
	q.top_k = 64;
	q.axes[0].slot = slotB;                     /* {1..3} */
	q.axes[1].slot = slotB;                     /* {3..5} ∪ */
	q.axes[1].join = REC_JOIN_OR;
	{
		struct mock_iv i0 = { 1, 3 };
		struct mock_iv i1 = { 3, 5 };
		rec_ref_t refs[16];
		q.axes[0].params = &i0;
		q.axes[1].params = &i1;
		int n = rec_query_run(&q, refs, NULL);
		ASSERT(n == 5, "{1..3} ∪ {3..5} → 5");
		ASSERT(refs[0] == 1 && refs[4] == 5, "1,2,3,4,5 deduped");
	}
}

static void
test_not_subtract(void)
{
	rec_query_t q;

	printf("=== 2A NOT subtract ===\n");
	reset_query(&q);
	q.n_axes = 2;
	q.top_k = 64;
	q.axes[0].slot = slotB;                     /* {1..10} */
	q.axes[1].slot = slotB;                     /* \ {3..5} */
	q.axes[1].join = REC_JOIN_NOT;
	{
		struct mock_iv i0 = { 1, 10 };
		struct mock_iv i1 = { 3, 5 };
		rec_ref_t refs[16];
		q.axes[0].params = &i0;
		q.axes[1].params = &i1;
		int n = rec_query_run(&q, refs, NULL);
		ASSERT(n == 7, "{1..10} \\ {3..5} → 7");
		ASSERT(refs[0] == 1 && refs[2] == 6 && refs[3] == 7 && refs[6] == 10,
		       "1,2,6,7,8,9,10");
	}
}

static void
test_mixed_chain(void)
{
	rec_query_t q;

	printf("=== 2A mixed chain: AND → OR → NOT (left-to-right) ===\n");
	reset_query(&q);
	q.n_axes = 4;
	q.top_k = 64;
	q.axes[0].slot = slotB;                     /* {1..10}   seed */
	q.axes[1].slot = slotB;                     /* {5..20}   AND → {5..10} */
	q.axes[2].slot = slotG;                     /* {10,11}   OR  → {5..11} */
	q.axes[3].slot = slotB;                     /* {8..9}    NOT → {5,6,7,10,11} */
	q.axes[1].join = REC_JOIN_AND;
	q.axes[2].join = REC_JOIN_OR;
	q.axes[3].join = REC_JOIN_NOT;
	{
		struct mock_iv i0 = { 1, 10 };
		struct mock_iv i1 = { 5, 20 };
		struct mock_iv i3 = { 8, 9 };
		rec_ref_t refs[16];
		q.axes[0].params = &i0;
		q.axes[1].params = &i1;
		q.axes[3].params = &i3;
		int n = rec_query_run(&q, refs, NULL);
		ASSERT(n == 5, "hand-computed chain → 5");
		ASSERT(refs[0] == 5 && refs[1] == 6 && refs[2] == 7 &&
		       refs[3] == 10 && refs[4] == 11, "5,6,7,10,11");
	}
}

static void
test_global_combine(void)
{
	rec_query_t q;

	printf("=== 2A global combine override ===\n");
	/* OR shorthand: per-axis AND ignored, union instead. */
	reset_query(&q);
	q.n_axes = 2;
	q.top_k = 64;
	q.combine = REC_JOIN_OR;
	q.axes[0].slot = slotB;                     /* {1..3}    seed */
	q.axes[1].slot = slotG;                     /* {10,11}   AND → but combine → ∪ */
	q.axes[1].join = REC_JOIN_AND;
	{
		struct mock_iv i0 = { 1, 3 };
		rec_ref_t refs[16];
		q.axes[0].params = &i0;
		int n = rec_query_run(&q, refs, NULL);
		ASSERT(n == 5, "combine=OR unions despite per-axis AND");
		ASSERT(refs[4] == 11, "refs 1,2,3,10,11");
	}
	/* Default combine=AND: per-axis join honored. */
	reset_query(&q);
	q.n_axes = 2;
	q.top_k = 64;
	q.axes[0].slot = slotB;                     /* {1..3}    seed */
	q.axes[1].slot = slotG;                     /* {10,11}   AND → empty */
	q.axes[1].join = REC_JOIN_AND;
	{
		struct mock_iv i0 = { 1, 3 };
		rec_ref_t refs[16];
		q.axes[0].params = &i0;
		int n = rec_query_run(&q, refs, NULL);
		ASSERT(n == 0, "default AND honors per-axis join (empty)");
	}
}

static void
test_not_combine(void)
{
	rec_query_t q;

	printf("=== 2A combine=NOT (subtraction as global mode) ===\n");
	reset_query(&q);
	q.n_axes = 3;
	q.top_k = 64;
	q.combine = REC_JOIN_NOT;
	q.axes[0].slot = slotB;                     /* {1..10}   seed */
	q.axes[1].slot = slotB;                     /* {2..3}    NOT */
	q.axes[2].slot = slotB;                     /* {5..6}    NOT */
	{
		struct mock_iv i0 = { 1, 10 };
		struct mock_iv i1 = { 2, 3 };
		struct mock_iv i2 = { 5, 6 };
		rec_ref_t refs[16];
		q.axes[0].params = &i0;
		q.axes[1].params = &i1;
		q.axes[2].params = &i2;
		int n = rec_query_run(&q, refs, NULL);
		ASSERT(n == 6, "{1..10} \\ {2,3} \\ {5,6} → 6");
		ASSERT(refs[0] == 1 && refs[5] == 10, "1,4,7,8,9,10");
	}
}

/* ── approx fills ──────────────────────────────────────────────────── */

static void
test_approx_propagation(void)
{
	rec_query_t q;

	printf("=== 2A approx fills join correctly (flag via kernel) ===\n");
	/* Engine path: exact seed {1..5} AND approx {2,4,6}(bound 0.8). */
	reset_query(&q);
	q.n_axes = 2;
	q.top_k = 64;
	q.axes[0].slot = slotB;                     /* {1..5}    exact */
	q.axes[1].slot = slotC;                     /* {2,4,6}   approx 0.8 */
	q.axes[1].join = REC_JOIN_AND;
	{
		struct mock_iv i0 = { 1, 5 };
		rec_ref_t refs[16];
		q.axes[0].params = &i0;
		int n = rec_query_run(&q, refs, NULL);
		ASSERT(n == 2 && refs[0] == 2 && refs[1] == 4, "exact ∩ approx refs");
	}
	/* The flag propagation itself lives in the kernel joins the engine
	 * calls; mirror the engine's exact entry conditions here. */
	{
		rec_set_t *a = rec_set_new(), *b = rec_set_new(), *d = rec_set_new();
		struct mock_iv i0 = { 1, 5 };
		mockB_fill(NULL, &i0, a);
		mockC_fill(NULL, NULL, b);
		rec_set_intersect(d, a, b);
		ASSERT(rec_set_approx(d) == REC_SET_APPROX, "intersect → approx");
		ASSERT(rec_set_recall_bound(d) == 0.8f, "bound = min(1.0, 0.8)");
		rec_set_free(d);
		rec_set_free(a); rec_set_free(b);
	}
	/* Union with an approx set. */
	reset_query(&q);
	q.n_axes = 2;
	q.top_k = 64;
	q.axes[0].slot = slotB;                     /* {1..5}    exact */
	q.axes[1].slot = slotC;                     /* {2,4,6}   approx */
	q.axes[1].join = REC_JOIN_OR;
	{
		struct mock_iv i0 = { 1, 5 };
		rec_ref_t refs[16];
		q.axes[0].params = &i0;
		int n = rec_query_run(&q, refs, NULL);
		ASSERT(n == 6 && refs[0] == 1 && refs[5] == 6, "exact ∪ approx refs");
	}
}

/* ── consumer score fn ─────────────────────────────────────────────── */

static void
test_consumer_score(void)
{
	rec_query_t q;
	struct mock_weight w = { 0.5f, 0.5f };

	printf("=== 2A consumer score fn (pre-filled score array) ===\n");
	/* Seed slotA {1..5} (ranked), slotB NOT {2,3} (rankless, valid 0):
	 * R = {1,4,5} → 3. The consumer receives a 2-entry score array;
	 * the rankless NOT axis still appears with valid=0. */
	reset_query(&q);
	q.n_axes = 2;
	q.top_k = 16;
	q.consumer_score = weighted_consumer;
	q.consumer_ud = &w;
	q.axes[0].slot = slotA;                     /* seed {1..5}, rank */
	q.axes[1].slot = slotB;                     /* NOT {2,3}, rankless */
	q.axes[1].join = REC_JOIN_NOT;
	{
		struct mock_iv notr = { 2, 3 };
		rec_ref_t refs[16];
		float sc[16];
		q.axes[1].params = &notr;
		int n = rec_query_run(&q, refs, sc);
		ASSERT(n == 3, "R = {1..5} \\ {2,3} → 3");
		/* axis0 scores as ref/100 weighted 0.5 → 5 > 4 > 1 */
		ASSERT(refs[0] == 5 && refs[1] == 4 && refs[2] == 1,
		       "weighted scores ordered 5,4,1");
		ASSERT(sc[0] > sc[1] && sc[1] > sc[2], "scores strictly decreasing");
		ASSERT(consumer_seen_n == 2, "one score entry per axis");
		ASSERT(consumer_seen[0].slot == slotA && consumer_seen[0].valid == 1,
		       "axis0 (seed) scored+valid");
		ASSERT(consumer_seen[1].slot == slotB && consumer_seen[1].valid == 0,
		       "rankless NOT axis → valid 0");
	}
}

static void
test_rankless_axis(void)
{
	rec_query_t q;
	struct mock_weight w = { 0.5f, 0.5f };

	printf("=== 2A rank-less axis → valid 0 ===\n");
	/* Seed {1..5}, AND {3,4}: R = {3,4} → 2. The intersect axis (mockB)
	 * has no rank fn, so its score entry is valid 0. */
	reset_query(&q);
	q.n_axes = 2;
	q.top_k = 16;
	q.consumer_score = weighted_consumer;
	q.consumer_ud = &w;
	q.axes[0].slot = slotA;                     /* seed {1..5}, rank */
	q.axes[1].slot = slotB;                     /* {3,4}, rankless */
	q.axes[1].join = REC_JOIN_AND;
	{
		struct mock_iv iv = { 3, 4 };
		rec_ref_t refs[16];
		float sc[16];
		q.axes[1].params = &iv;
		int n = rec_query_run(&q, refs, sc);
		ASSERT(n == 2, "{1..5} ∩ {3,4} → 2");
		ASSERT(consumer_seen_n == 2, "two score entries");
		ASSERT(consumer_seen[0].valid == 1, "seed axis scored");
		ASSERT(consumer_seen[1].slot == slotB && consumer_seen[1].valid == 0,
		       "rankless axis → valid 0");
	}
}

/* ── pure-filter mode ──────────────────────────────────────────────── */

static void
test_pure_filter(void)
{
	rec_query_t q;
	rec_ref_t refs[16];

	printf("=== 2A pure-filter mode (no rankable axis) ===\n");
	reset_query(&q);
	q.n_axes = 2;
	q.top_k = 64;
	q.axes[0].slot = slotB;                     /* {1..10} */
	q.axes[1].slot = slotG;                     /* NOT {10,11} */
	q.axes[1].join = REC_JOIN_NOT;
	{
		struct mock_iv i0 = { 1, 10 };
		rec_ref_t refs2[16];
		q.axes[0].params = &i0;
		float *sc = NULL;
		int n = rec_query_run(&q, refs2, sc);
		ASSERT(n == 9, "{1..10} \\ {10,11} → 9");
		ASSERT(refs2[8] == 9, "refs sorted (no rank)");
	}
	/* scores untouched: pass a canary buffer, expect it unchanged. */
	reset_query(&q);
	q.n_axes = 1;
	q.top_k = 64;
	q.axes[0].slot = slotB;
	{
		struct mock_iv i0 = { 1, 3 };
		float canary[4] = { 9.9f, 9.9f, 9.9f, 9.9f };
		q.axes[0].params = &i0;
		int n = rec_query_run(&q, refs, canary);
		ASSERT(n == 3, "filter count");
		ASSERT(canary[0] == 9.9f, "scores untouched in pure filter");
	}
}

/* ── degenerate inputs ─────────────────────────────────────────────── */

static void
test_degenerate(void)
{
	rec_query_t q;
	rec_ref_t refs[16];

	printf("=== 2A degenerate inputs ===\n");
	ASSERT(rec_query_run(NULL, refs, NULL) == -1, "NULL q → -1");
	reset_query(&q);
	q.top_k = 0;
	q.n_axes = 1;
	q.axes[0].slot = slotA;
	ASSERT(rec_query_run(&q, refs, NULL) == -1, "top_k 0 → -1");
	reset_query(&q);
	q.top_k = 16;
	q.n_axes = 0;
	ASSERT(rec_query_run(&q, refs, NULL) == 0, "n_axes 0 → 0");
	reset_query(&q);
	q.top_k = 16;
	q.n_axes = -1;
	ASSERT(rec_query_run(&q, refs, NULL) == -1, "negative n_axes → -1");
	reset_query(&q);
	q.top_k = 16;
	q.n_axes = REC_QUERY_MAX_AXES + 1;
	ASSERT(rec_query_run(&q, refs, NULL) == -1, "n_axes > max → -1");
	/* all fills fail */
	reset_query(&q);
	q.top_k = 16;
	q.n_axes = 2;
	q.axes[0].slot = slotE;
	q.axes[1].slot = slotE;
	ASSERT(rec_query_run(&q, refs, NULL) == 0, "all fills fail → 0");
	/* unregistered slot is skipped */
	reset_query(&q);
	q.top_k = 16;
	q.n_axes = 1;
	q.axes[0].slot = 99;
	ASSERT(rec_query_run(&q, refs, NULL) == 0, "unregistered slot → 0");
}

/* ── re-entrancy ───────────────────────────────────────────────────── */

static void
test_reentrant(void)
{
	rec_query_t q;
	rec_ref_t r1[16], r2[16];

	printf("=== 2A re-entrant runs leave no scratch ===\n");
	reset_query(&q);
	q.n_axes = 2;
	q.top_k = 16;
	q.axes[0].slot = slotB;
	q.axes[1].slot = slotB;
	q.axes[1].join = REC_JOIN_AND;
	{
		struct mock_iv i0 = { 1, 4 };
		struct mock_iv i1 = { 2, 6 };
		q.axes[0].params = &i0;
		q.axes[1].params = &i1;
		int n1 = rec_query_run(&q, r1, NULL);
		int n2 = rec_query_run(&q, r2, NULL);
		ASSERT(n1 == n2 && n1 == 3, "stable count across runs");
		ASSERT(r1[0] == r2[0] && r1[1] == r2[1] && r1[2] == r2[2],
		       "identical results");
	}
}

/* ── registry overflow (must run last: fills the final capacity) ───── */

static void
test_overflow(void)
{
	int h;

	printf("=== 2A registry overflow at REC_QUERY_MAX_AXES ===\n");
	{
		const rec_axis_t hax = { "mockH", mockH_fill, NULL, NULL, NULL };
		h = rec_axis_register(&hax);
	}
	ASSERT(h == REC_QUERY_MAX_AXES - 1, "slot 7 (last free)");
	ASSERT(rec_axis_count() == REC_QUERY_MAX_AXES, "count == max");
	rec_axis_set_ctx(h, &ctx_marker);
	{
		const rec_axis_t thorn = { "mockX", mockH_fill, NULL, NULL, NULL };
		ASSERT(rec_axis_register(&thorn) == -1, "9th register → -1");
	}
	/* registered mockH works even after a failed register. */
	{
		rec_query_t q;
		rec_ref_t refs[16];
		reset_query(&q);
		q.top_k = 16;
		q.n_axes = 1;
		q.axes[0].slot = h;
		ASSERT(rec_query_run(&q, refs, NULL) == 1, "mockH still queryable");
	}
}

int
main(void)
{
	init_pool();
	test_registry();
	test_fill_dispatch();
	test_single_axis_rank();
	test_intersect_2();
	test_intersect_3();
	test_union_or();
	test_not_subtract();
	test_mixed_chain();
	test_global_combine();
	test_not_combine();
	test_approx_propagation();
	test_consumer_score();
	test_rankless_axis();
	test_pure_filter();
	test_degenerate();
	test_reentrant();
	test_overflow();

	printf("\n");
	if (errors == 0)
		printf("ALL REC_AXIS TESTS PASSED\n");
	else
		printf("%u REC_AXIS TEST(S) FAILED\n", errors);

	return errors == 0 ? 0 : 1;
}