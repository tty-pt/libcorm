/* rec.c — recall kernel (candidate sets + generic ranking loop).
 * Part of libcorm. See rec.h for the public contract. */

#include "./../include/ttypt/rec.h"
#include "./../include/ttypt/corm.h"

#include <stdlib.h>
#include <string.h>

#define REC_MIN_CAP 1024
#define REC_RANK_STACK_PAIRS 1024

struct rec_set {
	rec_ref_t *a;
	size_t n, cap;
	int sealed;
	int approx;         /* REC_SET_EXACT / REC_SET_APPROX */
	float recall_bound; /* owed recall@k (exact → 1.0) */
	rec_ref_t inline_a[];
};

struct rec_rank {
	rec_ref_t *refs;      /* 1..n heap */
	float *sc;
	size_t n, cap, top_k;
	float min;
};

static int
ref_cmp(const void *x, const void *y)
{
	rec_ref_t a = *(const rec_ref_t *)x, b = *(const rec_ref_t *)y;
	return a < b ? -1 : a > b ? 1 : 0;
}

static int
grow(rec_set_t *s)
{
	size_t cap = s->cap ? s->cap * 2 : REC_MIN_CAP;
	rec_ref_t *a;

	if (s->a == s->inline_a) {
		a = malloc(cap * sizeof(*a));
		if (!a)
			return -1;
		memcpy(a, s->inline_a, s->n * sizeof(*a));
	} else {
		a = realloc(s->a, cap * sizeof(*a));
		if (!a)
			return -1;
	}
	s->a = a;
	s->cap = cap;
	return 0;
}

rec_set_t *
rec_set_new(void)
{
	rec_set_t *s = calloc(1, sizeof(*s) + REC_MIN_CAP * sizeof(*s->a));
	if (!s)
		return NULL;
	s->a = s->inline_a;
	s->cap = REC_MIN_CAP;
	s->approx = REC_SET_EXACT;
	s->recall_bound = 1.0f;
	return s;
}

void
rec_set_push(rec_set_t *s, rec_ref_t r)
{
	if (s->n == s->cap && grow(s))
		return;
	s->a[s->n++] = r;
	s->sealed = 0;
}

void
rec_set_seal(rec_set_t *s)
{
	if (s->sealed)
		return;
	if (s->n > 1) {
		qsort(s->a, s->n, sizeof(*s->a), ref_cmp);
		size_t keep = 1;
		for (size_t i = 1; i < s->n; i++)
			if (s->a[i] != s->a[keep - 1])
				s->a[keep++] = s->a[i];
		s->n = keep;
	}
	s->sealed = 1;
}

int
rec_set_fill_corm_iter(rec_set_t *s, uint32_t hd)
{
	uint32_t kt = corm_get_ktype(hd);
	size_t kl = corm_type_len(kt);
	if (kl == 0 || kl > sizeof(rec_ref_t))
		return -1;

	uint32_t cur = corm_iter(hd, NULL, 0);
	const void *key, *value;
	rec_ref_t r;
	while (corm_next(&key, &value, cur)) {
		memset(&r, 0, sizeof(r));
		memcpy(&r, key, kl);
		rec_set_push(s, r);
	}
	corm_fin(cur);
	return 0;
}

static int
check_sealed(const rec_set_t *a, const rec_set_t *b)
{
	return a->sealed && b->sealed ? 0 : -1;
}

static void
join_exactness(rec_set_t *dst, const rec_set_t *a, const rec_set_t *b)
{
	/* intersect/union: result approximates as soon as either operand does;
	 * owed recall is bounded by the weaker (min) operand. */
	dst->approx = (a->approx || b->approx) ? REC_SET_APPROX : REC_SET_EXACT;
	dst->recall_bound = a->recall_bound < b->recall_bound
		? a->recall_bound : b->recall_bound;
}

int
rec_set_intersect(rec_set_t *dst, const rec_set_t *a, const rec_set_t *b)
{
	if (check_sealed(a, b))
		return -1;
	dst->n = 0;
	dst->sealed = 0;
	size_t i = 0, j = 0;
	while (i < a->n && j < b->n) {
		if (a->a[i] < b->a[j])
			i++;
		else if (a->a[i] > b->a[j])
			j++;
		else {
			rec_set_push(dst, a->a[i]);
			i++;
			j++;
		}
	}
	dst->sealed = 1;
	join_exactness(dst, a, b);
	return 0;
}

int
rec_set_subtract(rec_set_t *dst, const rec_set_t *a, const rec_set_t *b)
{
	if (check_sealed(a, b))
		return -1;
	dst->n = 0;
	dst->sealed = 0;
	size_t i = 0, j = 0;
	while (i < a->n) {
		while (j < b->n && b->a[j] < a->a[i])
			j++;
		if (j < b->n && b->a[j] == a->a[i])
			i++;
		else
			rec_set_push(dst, a->a[i++]);
	}
	dst->sealed = 1;
	dst->approx = a->approx;       /* result ⊆ a: keeps a's exactness */
	dst->recall_bound = a->recall_bound;
	return 0;
}

int
rec_set_union(rec_set_t *dst, const rec_set_t *a, const rec_set_t *b)
{
	if (check_sealed(a, b))
		return -1;
	dst->n = 0;
	dst->sealed = 0;
	size_t i = 0, j = 0;
	while (i < a->n || j < b->n) {
		if (j == b->n || (i < a->n && a->a[i] < b->a[j]))
			rec_set_push(dst, a->a[i++]);
		else if (i == a->n || b->a[j] < a->a[i])
			rec_set_push(dst, b->a[j++]);
		else {
			rec_set_push(dst, a->a[i++]);
			j++;
		}
	}
	dst->sealed = 1;
	join_exactness(dst, a, b);
	return 0;
}

size_t
rec_set_count(const rec_set_t *s)
{
	return s->n;
}

const rec_ref_t *
rec_set_at(const rec_set_t *s)
{
	return s->a;
}

int
rec_set_set_approx(rec_set_t *s, int approx, float recall_bound)
{
	if (!s)
		return -1;
	if (approx != REC_SET_EXACT && approx != REC_SET_APPROX)
		return -1;
	if (recall_bound <= 0.0f || recall_bound > 1.0f)
		return -1;
	s->approx = approx;
	s->recall_bound = recall_bound;
	return 0;
}

int
rec_set_approx(const rec_set_t *s)
{
	return s ? s->approx : REC_SET_EXACT;
}

float
rec_set_recall_bound(const rec_set_t *s)
{
	return s ? s->recall_bound : 1.0f;
}

void
rec_set_free(rec_set_t *s)
{
	if (!s)
		return;
	if (s->a != s->inline_a)
		free(s->a);
	free(s);
}

/* ── ranking loop ───────────────────────────────────────────────────── */

/* heap ordering: MIN-heap on rank; root = the WORST kept candidate.
 * better(a,b) → a outranks b: higher score; ties → lower ref.
 * Rank: higher score is better/min-worse; equal score → lower ref better. */

static int
better(rec_ref_t ar, float as, rec_ref_t br, float bs)
{
	if (as != bs)
		return as > bs;
	return ar < br;
}

static void
swp(rec_rank_t *rk, size_t i, size_t j)
{
	rec_ref_t tr = rk->refs[i]; float ts = rk->sc[i];
	rk->refs[i] = rk->refs[j]; rk->sc[i] = rk->sc[j];
	rk->refs[j] = tr; rk->sc[j] = ts;
}

static void
sift_up(rec_rank_t *rk, size_t i)
{
	while (i > 1) {
		size_t p = i / 2;
		if (better(rk->refs[p], rk->sc[p], rk->refs[i], rk->sc[i])) {
			swp(rk, i, p);
			i = p;
		} else
			break;
	}
}

static void
sift_down(rec_rank_t *rk, size_t i)
{
	for (;;) {
		size_t l = i * 2, r = l + 1, m = i;
		if (l <= rk->n &&
		    better(rk->refs[m], rk->sc[m], rk->refs[l], rk->sc[l]))
			m = l;
		if (r <= rk->n &&
		    better(rk->refs[m], rk->sc[m], rk->refs[r], rk->sc[r]))
			m = r;
		if (m == i)
			break;
		swp(rk, i, m);
		i = m;
	}
}

rec_rank_t *
rec_rank_new(size_t top_k, float min_score)
{
	rec_rank_t *rk;
	size_t n;

	if (top_k == 0)
		return NULL;
	n = top_k + 1;
	rk = malloc(sizeof(*rk) + n * sizeof(*rk->refs) + n * sizeof(*rk->sc));
	if (!rk)
		return NULL;
	rk->refs = (rec_ref_t *)((char *)rk + sizeof(*rk));
	rk->sc = (float *)((char *)rk->refs + n * sizeof(*rk->refs));
	rk->n = 0;
	rk->cap = top_k;
	rk->top_k = top_k;
	rk->min = min_score;
	return rk;
}

void
rec_rank_push(rec_rank_t *rk, rec_ref_t r, float score)
{
	if (score < rk->min)
		return;
	if (rk->n < rk->cap) {
		rk->refs[++rk->n] = r;
		rk->sc[rk->n] = score;
		sift_up(rk, rk->n);
	} else if (better(r, score, rk->refs[1], rk->sc[1])) {
		rk->refs[1] = r;
		rk->sc[1] = score;
		sift_down(rk, 1);
	}
}

typedef struct { rec_ref_t r; float s; } pair_t;

static int
pair_cmp(const void *x, const void *y)
{
	const pair_t *a = x, *b = y;
	if (a->s != b->s)
		return a->s > b->s ? -1 : 1;
	return a->r < b->r ? -1 : a->r > b->r ? 1 : 0;
}

size_t
rec_rank_sorted(const rec_rank_t *rk, rec_ref_t *refs, float *scores)
{
	pair_t stack[REC_RANK_STACK_PAIRS];
	pair_t *pairs;
	size_t i;

	if (rk->n == 0)
		return 0;
	if (rk->n <= REC_RANK_STACK_PAIRS) {
		pairs = stack;
	} else {
		pairs = malloc(rk->n * sizeof(*pairs));
		if (!pairs)
			return 0;
	}
	for (i = 0; i < rk->n; i++) {
		pairs[i].r = rk->refs[i + 1];
		pairs[i].s = rk->sc[i + 1];
	}
	qsort(pairs, rk->n, sizeof(*pairs), pair_cmp);
	for (i = 0; i < rk->n; i++) {
		refs[i] = pairs[i].r;
		if (scores)
			scores[i] = pairs[i].s;
	}
	if (pairs != stack)
		free(pairs);
	return rk->n;
}

void
rec_rank_free(rec_rank_t *rk)
{
	free(rk);
}