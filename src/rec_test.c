#include "./../include/ttypt/rec.h"
#include "./../include/ttypt/corm.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#define ARRAY_LEN(a) (sizeof(a) / sizeof((a)[0]))

unsigned errors = 0;

#define PASS() printf("  OK\n")
#define FAIL(msg) do { printf("  FAIL: %s [line %d]\n", msg, __LINE__); errors++; } while(0)
#define ASSERT(cond, msg) do { if (!(cond)) FAIL(msg); else PASS(); } while(0)

static int
expect_set(const rec_set_t *s, const rec_ref_t *want, size_t n)
{
	if (rec_set_count(s) != n) {
		printf("  count %zu != %zu\n", rec_set_count(s), n);
		return 0;
	}
	const rec_ref_t *got = rec_set_at(s);
	for (size_t i = 0; i < n; i++) {
		if (got[i] != want[i]) {
			printf("  [%zu] %llu != %llu\n", i,
			       (unsigned long long)got[i], (unsigned long long)want[i]);
			return 0;
		}
	}
	return 1;
}

#define EXPECT_SET(s, ...) do { \
	rec_ref_t _w[] = { __VA_ARGS__ }; \
	if (expect_set((s), _w, ARRAY_LEN(_w))) PASS(); else FAIL("set mismatch"); \
} while(0)

static void
fill(rec_set_t *s, const rec_ref_t *v, size_t n)
{
	for (size_t i = 0; i < n; i++)
		rec_set_push(s, v[i]);
	rec_set_seal(s);
}

/* ── 1A: sets, joins, arena ─────────────────────────────────────────── */

static void
test_basic_count(void)
{
	printf("=== 1A basic count/unsealed ===\n");
	rec_set_t *s = rec_set_new();
	ASSERT(s, "new set");
	ASSERT(rec_set_count(s) == 0, "empty count 0");
	rec_set_push(s, 42);
	ASSERT(rec_set_count(s) == 1, "count after one push");
	rec_set_free(s);
}

static void
test_seal_sorts(void)
{
	printf("=== 1A seal sorts ===\n");
	rec_set_t *s = rec_set_new();
	const rec_ref_t in[] = { 5, 1, 4, 1, 3 };
	fill(s, in, ARRAY_LEN(in));
	EXPECT_SET(s, 1, 3, 4, 5);
	rec_set_free(s);
}

static void
test_seal_dedups(void)
{
	printf("=== 1A seal dedups ===\n");
	rec_set_t *s = rec_set_new();
	const rec_ref_t in[] = { 1, 1, 2, 2, 2, 1 };
	fill(s, in, ARRAY_LEN(in));
	EXPECT_SET(s, 1, 2);
	rec_set_free(s);
}

static void
test_push_unseals(void)
{
	printf("=== 1A push unseals ===\n");
	rec_set_t *s = rec_set_new();
	rec_set_push(s, 2);
	rec_set_push(s, 1);
	rec_set_seal(s);
	EXPECT_SET(s, 1, 2);
	rec_set_push(s, 0);            /* after seal → unsealed */
	rec_set_seal(s);
	EXPECT_SET(s, 0, 1, 2);
	rec_set_free(s);
}

static void
test_intersect(void)
{
	printf("=== 1A intersect ===\n");
	rec_set_t *a = rec_set_new(), *b = rec_set_new(), *d = rec_set_new();
	const rec_ref_t av[] = { 1, 2, 3, 4, 5 }, bv[] = { 3, 5, 7 };
	fill(a, av, 5); fill(b, bv, 3);

	ASSERT(rec_set_intersect(d, a, b) == 0, "intersect ok");
	EXPECT_SET(d, 3, 5);
	rec_set_free(d);

	d = rec_set_new();
	b = rec_set_new();
	const rec_ref_t bv2[] = { 90, 91 };
	fill(b, bv2, 2);
	rec_set_intersect(d, a, b);
	ASSERT(rec_set_count(d) == 0, "disjoint → empty");
	rec_set_free(d);

	rec_set_free(a); rec_set_free(b);
}

static void
test_subtract(void)
{
	printf("=== 1A subtract ===\n");
	rec_set_t *a = rec_set_new(), *b = rec_set_new(), *d = rec_set_new();
	const rec_ref_t av[] = { 1, 2, 3, 4, 5 }, bv[] = { 2, 4 };
	fill(a, av, 5); fill(b, bv, 2);
	rec_set_subtract(d, a, b);
	EXPECT_SET(d, 1, 3, 5);
	rec_set_free(d);

	d = rec_set_new();
	b = rec_set_new();             /* b empty → d == a */
	rec_set_seal(b);
	rec_set_subtract(d, a, b);
	EXPECT_SET(d, 1, 2, 3, 4, 5);
	rec_set_free(d);
	rec_set_free(a); rec_set_free(b);
}

static void
test_union(void)
{
	printf("=== 1A union ===\n");
	rec_set_t *a = rec_set_new(), *b = rec_set_new(), *d = rec_set_new();
	const rec_ref_t av[] = { 1, 2, 3 }, bv[] = { 3, 4 };
	fill(a, av, 3); fill(b, bv, 2);
	rec_set_union(d, a, b);
	EXPECT_SET(d, 1, 2, 3, 4);
	rec_set_free(d);
	rec_set_free(a); rec_set_free(b);
}

static void
test_joins_require_sealed(void)
{
	printf("=== 1A joins require sealed ===\n");
	rec_set_t *a = rec_set_new(), *b = rec_set_new(), *d = rec_set_new();
	rec_set_push(a, 1);            /* unsealed */
	rec_set_push(b, 1);
	fill(d, (rec_ref_t[]){ 1 }, 1);
	ASSERT(rec_set_intersect(d, a, b) != 0, "intersect rejects unsealed");
	rec_set_seal(a); rec_set_seal(b);
	ASSERT(rec_set_intersect(d, a, b) == 0, "intersect sealed ok");
	ASSERT(rec_set_subtract(d, a, b) == 0, "subtract sealed ok");
	ASSERT(rec_set_union(d, a, b) == 0, "union sealed ok");
	rec_set_free(a); rec_set_free(b); rec_set_free(d);
}

static void
test_fill_corm_iter(void)
{
	printf("=== 1A fill_corm_iter (u32 keys) ===\n");
	uint32_t kt = corm_reg(sizeof(uint32_t));
	uint32_t hd = corm_open(NULL, NULL, kt, kt, 0xFF, CM_SORTED);
	for (uint32_t i = 0; i < 50; i++)
		corm_put(hd, &i, &i);
	rec_set_t *s = rec_set_new();
	ASSERT(rec_set_fill_corm_iter(s, hd) == 0, "fill ok");
	rec_set_seal(s);
	ASSERT(rec_set_count(s) == 50, "50 keys drained");
	const rec_ref_t *got = rec_set_at(s);
	ASSERT(got[0] == 0 && got[1] == 1 && got[2] == 2 && got[49] == 49,
	       "drained keys sorted 0..49");
	rec_set_free(s);
	corm_close(hd);
}

static void
test_fill_corm_iter_multivalue(void)
{
	printf("=== 1A fill_corm_iter duplicates ===\n");
	uint32_t kt = corm_reg(sizeof(uint32_t));
	uint32_t hd = corm_open(NULL, NULL, kt, kt, 0xFF, CM_SORTED | CM_MULTIVALUE);
	rec_ref_t r = 7;
	corm_put(hd, &r, &r);
	corm_put(hd, &r, &r);
	r = 3;
	corm_put(hd, &r, &r);
	rec_set_t *s = rec_set_new();
	rec_set_fill_corm_iter(s, hd);
	rec_set_seal(s);
	EXPECT_SET(s, 3, 7);           /* dedup after fill */
	rec_set_free(s);
	corm_close(hd);
}

static void
test_fill_corm_iter_rejects_variable(void)
{
	printf("=== 1A fill_corm_iter variable keys ===\n");
	uint32_t hd = corm_open(NULL, NULL, CM_STR, CM_STR, 0xFF, 0);
	corm_put(hd, "k", "v");
	rec_set_t *s = rec_set_new();
	ASSERT(rec_set_fill_corm_iter(s, hd) != 0, "variable keys rejected");
	rec_set_free(s);
	corm_close(hd);
}

static void
test_arena_growth(void)
{
	printf("=== 1A arena growth (200k) ===\n");
	rec_set_t *s = rec_set_new();
	for (uint32_t i = 0; i < 200000; i++)
		rec_set_push(s, i % 100);
	ASSERT(rec_set_count(s) == 200000, "all pushed");
	rec_set_seal(s);
	ASSERT(rec_set_count(s) == 100, "dedup to 100");
	const rec_ref_t *got = rec_set_at(s);
	int sorted = 1;
	for (uint32_t i = 1; i < 100; i++)
		if (got[i - 1] >= got[i])
			sorted = 0;
	ASSERT(sorted, "sealed sorted");
	rec_set_free(s);
}

static void
test_big_merges(void)
{
	printf("=== 1A big merges (evens ∩ 3s) ===\n");
	rec_set_t *a = rec_set_new(), *b = rec_set_new(), *d = rec_set_new();
	for (uint32_t i = 0; i < 20000; i += 2)
		rec_set_push(a, i);
	for (uint32_t i = 0; i < 60000; i += 3)
		rec_set_push(b, i);
	rec_set_seal(a); rec_set_seal(b);
	rec_set_intersect(d, a, b);    /* multiples of lcm 6 → 3334 */
	ASSERT(rec_set_count(d) == 3334, "multiples of 6 count");
	const rec_ref_t *got = rec_set_at(d);
	ASSERT(got[0] == 0 && got[1] == 6 && got[3333] == 19998,
	       "intersection contents 0,6,...,19998");
	rec_set_free(d);
	rec_set_free(a); rec_set_free(b);
}

/* ── 1B: ranking loop ───────────────────────────────────────────────── */

static void
test_rank_min_score(void)
{
	printf("=== 1B min_score filter ===\n");
	rec_rank_t *rk = rec_rank_new(16, 0.5f);
	rec_rank_push(rk, 1, 0.1f);
	rec_rank_push(rk, 2, 0.9f);
	rec_rank_push(rk, 3, 0.5f);
	rec_ref_t refs[16]; float sc[16];
	size_t n = rec_rank_sorted(rk, refs, sc);
	ASSERT(n == 2, "two above threshold");
	ASSERT(refs[0] == 2 && refs[1] == 3, "best first");
	ASSERT(sc[0] == 0.9f && sc[1] == 0.5f, "scores preserved");
	rec_rank_free(rk);
}

static void
test_rank_topk(void)
{
	printf("=== 1B top_k bound ===\n");
	rec_rank_t *rk = rec_rank_new(3, 0.0f);
	for (uint32_t i = 0; i < 100; i++)
		rec_rank_push(rk, i, (float)i);
	rec_ref_t refs[16]; float sc[16];
	size_t n = rec_rank_sorted(rk, refs, sc);
	ASSERT(n == 3, "kept top 3");
	ASSERT(refs[0] == 99 && refs[1] == 98 && refs[2] == 97, "best three, desc");
	ASSERT(sc[0] == 99.0f, "score matches");
	rec_rank_free(rk);
}

static void
test_rank_ties(void)
{
	printf("=== 1B tie → asc ref ===\n");
	rec_rank_t *rk = rec_rank_new(8, 0.0f);
	rec_rank_push(rk, 5, 1.0f);
	rec_rank_push(rk, 2, 1.0f);
	rec_rank_push(rk, 9, 1.0f);
	rec_ref_t refs[16]; float sc[16];
	size_t n = rec_rank_sorted(rk, refs, sc);
	ASSERT(n == 3, "all retained");
	ASSERT(refs[0] == 2 && refs[1] == 5 && refs[2] == 9, "ties by asc ref");
	rec_rank_free(rk);
}

static void
test_rank_edge(void)
{
	printf("=== 1B edges ===\n");
	rec_rank_t *rk = rec_rank_new(4, 0.0f);
	rec_ref_t refs[4]; float sc[4];
	ASSERT(rec_rank_sorted(rk, refs, sc) == 0, "empty → 0");
	rec_rank_push(rk, 1, 3.0f);
	ASSERT(rec_rank_sorted(rk, refs, sc) == 1, "single");
	rec_rank_free(rk);
	ASSERT(rec_rank_new(0, 0.0f) == NULL, "top_k 0 → NULL");
}

static void
test_rank_streaming_sequence(void)
{
	printf("=== 1B streaming sequence ===\n");
	/* result order independent of push order for equal scores */
	rec_rank_t *rk = rec_rank_new(4, 0.0f);
	rec_rank_push(rk, 7, 0.2f);
	rec_rank_push(rk, 3, 0.9f);
	rec_rank_push(rk, 1, 0.5f);
	rec_rank_push(rk, 9, 0.9f);
	rec_ref_t refs[4]; float sc[4];
	size_t n = rec_rank_sorted(rk, refs, sc);
	ASSERT(n == 4, "all kept");
	ASSERT(refs[0] == 3 && refs[1] == 9 && refs[2] == 1 && refs[3] == 7,
	       "desc, ties asc ref");
	rec_rank_free(rk);
}

/* ── 1C: approximate-fill exactness flag ────────────────────────────── */

static void
test_exactness_defaults(void)
{
	printf("=== 1C exactness defaults ===\n");
	rec_set_t *s = rec_set_new();
	ASSERT(s, "new set");
	ASSERT(rec_set_approx(s) == REC_SET_EXACT, "fresh set is exact");
	ASSERT(rec_set_recall_bound(s) == 1.0f, "fresh set bound 1.0");
	rec_set_push(s, 1);
	ASSERT(rec_set_approx(s) == REC_SET_EXACT, "push keeps exact");
	rec_set_seal(s);
	ASSERT(rec_set_approx(s) == REC_SET_EXACT, "seal keeps exact");
	rec_set_free(s);
}

static void
test_exactness_set_clear(void)
{
	printf("=== 1C exactness set/clear ===\n");
	rec_set_t *s = rec_set_new();
	ASSERT(rec_set_set_approx(s, REC_SET_APPROX, 0.9f) == 0, "set approx ok");
	ASSERT(rec_set_approx(s) == REC_SET_APPROX, "now approximate");
	ASSERT(rec_set_recall_bound(s) == 0.9f, "bound stored");
	ASSERT(rec_set_set_approx(s, REC_SET_EXACT, 1.0f) == 0, "set exact ok");
	ASSERT(rec_set_approx(s) == REC_SET_EXACT, "back to exact");
	ASSERT(rec_set_recall_bound(s) == 1.0f, "bound reset to 1.0");
	rec_set_free(s);
}

static void
test_exactness_bound_validation(void)
{
	printf("=== 1C exactness bound validation ===\n");
	rec_set_t *s = rec_set_new();
	ASSERT(rec_set_set_approx(s, REC_SET_APPROX, 0.0f) == -1, "bound 0 rejected");
	ASSERT(rec_set_set_approx(s, REC_SET_APPROX, -1.0f) == -1, "bound <0 rejected");
	ASSERT(rec_set_set_approx(s, REC_SET_APPROX, 1.5f) == -1, "bound >1 rejected");
	ASSERT(rec_set_approx(s) == REC_SET_EXACT, "rejected leaves exact");
	ASSERT(rec_set_set_approx(s, REC_SET_APPROX, 1.0f) == 0, "bound 1.0 accepted");
	ASSERT(rec_set_approx(s) == REC_SET_APPROX, "approx accepted");
	rec_set_free(s);
	ASSERT(rec_set_set_approx(NULL, REC_SET_APPROX, 0.5f) == -1, "NULL rejected");
}

static void
test_intersect_propagates_approx(void)
{
	printf("=== 1C intersect propagates approx ===\n");
	rec_set_t *a = rec_set_new(), *b = rec_set_new(), *d = rec_set_new();
	const rec_ref_t av[] = { 1, 2, 3, 4, 5 }, bv[] = { 2, 4, 6 };
	fill(a, av, 5); fill(b, bv, 3);

	rec_set_set_approx(b, REC_SET_APPROX, 0.8f);
	rec_set_intersect(d, a, b);
	EXPECT_SET(d, 2, 4);
	ASSERT(rec_set_approx(d) == REC_SET_APPROX, "exact∩approx → approx");
	ASSERT(rec_set_recall_bound(d) == 0.8f, "bound = min(1.0, 0.8)");

	rec_set_set_approx(a, REC_SET_APPROX, 0.5f);
	rec_set_intersect(d, a, b);
	ASSERT(rec_set_approx(d) == REC_SET_APPROX, "approx∩approx → approx");
	ASSERT(rec_set_recall_bound(d) == 0.5f, "bound = min(0.5, 0.8)");
	rec_set_free(d);

	d = rec_set_new();
	rec_set_set_approx(d, REC_SET_APPROX, 0.9f); /* dst old flag discarded */
	rec_set_intersect(d, a, b);
	ASSERT(rec_set_approx(d) == REC_SET_APPROX &&
	       rec_set_recall_bound(d) == 0.5f, "dst overwritten, bound min");
	rec_set_free(d);

	rec_set_free(a); rec_set_free(b);
}

static void
test_union_propagates_approx(void)
{
	printf("=== 1C union propagates approx ===\n");
	rec_set_t *a = rec_set_new(), *b = rec_set_new(), *d = rec_set_new();
	fill(a, (rec_ref_t[]){ 1, 2 }, 2);
	fill(b, (rec_ref_t[]){ 2, 3 }, 2);
	rec_set_union(d, a, b);
	ASSERT(rec_set_approx(d) == REC_SET_EXACT, "exact∪exact → exact");

	rec_set_set_approx(b, REC_SET_APPROX, 0.7f);
	rec_set_union(d, a, b);
	ASSERT(rec_set_approx(d) == REC_SET_APPROX, "exact∪approx → approx");
	ASSERT(rec_set_recall_bound(d) == 0.7f, "bound = min");
	rec_set_free(a); rec_set_free(b); rec_set_free(d);
}

static void
test_subtract_keeps_left_flag(void)
{
	printf("=== 1C subtract keeps left operand's exactness ===\n");
	rec_set_t *a = rec_set_new(), *b = rec_set_new(), *d = rec_set_new();
	fill(a, (rec_ref_t[]){ 1, 2, 3 }, 3);
	fill(b, (rec_ref_t[]){ 2 }, 1);

	rec_set_set_approx(a, REC_SET_APPROX, 0.6f);
	rec_set_subtract(d, a, b);
	EXPECT_SET(d, 1, 3);
	ASSERT(rec_set_approx(d) == REC_SET_APPROX, "keeps a's approx");
	ASSERT(rec_set_recall_bound(d) == 0.6f, "keeps a's bound");

	rec_set_set_approx(a, REC_SET_EXACT, 1.0f);
	rec_set_set_approx(b, REC_SET_APPROX, 0.3f);
	rec_set_subtract(d, a, b);
	ASSERT(rec_set_approx(d) == REC_SET_EXACT, "exact−approx → exact (subset of a)");
	rec_set_free(a); rec_set_free(b); rec_set_free(d);
}

int
main(void)
{
	test_basic_count();
	test_seal_sorts();
	test_seal_dedups();
	test_push_unseals();
	test_intersect();
	test_subtract();
	test_union();
	test_joins_require_sealed();
	test_fill_corm_iter();
	test_fill_corm_iter_multivalue();
	test_fill_corm_iter_rejects_variable();
	test_arena_growth();
	test_big_merges();

	test_rank_min_score();
	test_rank_topk();
	test_rank_ties();
	test_rank_edge();
	test_rank_streaming_sequence();

	test_exactness_defaults();
	test_exactness_set_clear();
	test_exactness_bound_validation();
	test_intersect_propagates_approx();
	test_union_propagates_approx();
	test_subtract_keeps_left_flag();

	printf("\n");
	if (errors == 0)
		printf("ALL REC TESTS PASSED\n");
	else
		printf("%u REC TEST(S) FAILED\n", errors);

	return errors == 0 ? 0 : 1;
}