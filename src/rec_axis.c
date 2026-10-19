/* rec_axis.c — pluggable axis registry + multi-axis query engine.
 * Part of libcorm. See cormr.h for the public contract.
 *
 * The registry is a fixed table of opaque axes (filled by dlopen'd plugin
 * constructors or by direct calls); libcorm itself knows nothing about any
 * concrete axis. cormr_query_run fills each axis, folds the sealed sets into
 * a running set with the kernel merge-joins (AND → intersect, OR → union,
 * NOT → subtract), then ranks via cormr_rank. */

#include "./../include/ttypt/cormr.h"

#include <string.h>

static cormr_axis_t axes[CORMR_QUERY_MAX_AXES];
static int          axes_n;

int
cormr_axis_register(const cormr_axis_t *axis)
{
	if (!axis || axes_n >= CORMR_QUERY_MAX_AXES)
		return -1;
	axes[axes_n] = *axis;
	return axes_n++;
}

const cormr_axis_t *
cormr_axis_get(int slot)
{
	if (slot < 0 || slot >= axes_n)
		return NULL;
	return &axes[slot];
}

void *
cormr_axis_decode(int slot, const char *s)
{
	const cormr_axis_t *a = cormr_axis_get(slot);
	if (!a || !a->decode || !s)
		return NULL;
	return a->decode(s);
}

int
cormr_axis_set_ctx(int slot, void *ctx)
{
	if (slot < 0 || slot >= axes_n)
		return -1;
	axes[slot].ctx = ctx;
	return 0;
}

int
cormr_axis_count(void)
{
	return axes_n;
}

static int
query_join(cormr_join_t jm, cormr_set_t *dst, const cormr_set_t *a, const cormr_set_t *b)
{
	switch (jm) {
	case CORMR_JOIN_AND:
		return cormr_set_intersect(dst, a, b);
	case CORMR_JOIN_OR:
		return cormr_set_union(dst, a, b);
	case CORMR_JOIN_NOT:
		return cormr_set_subtract(dst, a, b);
	}
	return -1;
}

int
cormr_query_run(const cormr_query_t *q, cormr_ref_t *refs, float *scores)
{
	cormr_set_t *set[CORMR_QUERY_MAX_AXES];
	cormr_set_t *r = NULL;
	int n_sets = 0, seed = 0, i, ret = 0;

	if (q == NULL || q->top_k == 0)
		return -1;
	if (q->n_axes == 0)
		return 0;
	if (q->n_axes < 0 || q->n_axes > CORMR_QUERY_MAX_AXES)
		return -1;

	for (i = 0; i < CORMR_QUERY_MAX_AXES; i++)
		set[i] = NULL;

	/* Fill phase: one sealed set per axis that fills; an axis may refuse
	 * (NULL ctx, fill error) and is simply skipped. */
	for (i = 0; i < q->n_axes; i++) {
		const cormr_axis_t *axis = cormr_axis_get(q->axes[i].slot);
		cormr_set_t *s;

		if (!axis || !axis->fill || !axis->ctx)
			continue;
		s = cormr_set_new();
		if (!s) {
			ret = -1;
			goto out;
		}
		if (axis->fill(axis->ctx, q->axes[i].params, s) != 0) {
			cormr_set_free(s);
			continue;
		}
		set[i] = s;
		n_sets++;
	}

	if (n_sets == 0)
		goto out;                /* nothing filled → 0 results */

	/* Join phase: left-to-right running set r. The first filled axis seeds
	 * r (its own join is ignored); each subsequent set joins with the mode
	 * from its axis, overridden by q->combine when that differs from AND. */
	for (i = 0; i < q->n_axes; i++)
		if (set[i]) {
			seed = i;
			break;
		}

	r = set[seed];
	set[seed] = NULL;            /* r owns the seed now */
	for (i = seed + 1; i < q->n_axes; i++) {
		cormr_set_t *tmp;
		cormr_join_t jm;

		if (!set[i])
			continue;
		tmp = cormr_set_new();
		if (!tmp) {
			ret = -1;
			goto out;
		}
		jm = q->axes[i].join;
		if (q->combine != CORMR_JOIN_AND)
			jm = q->combine;
		if (query_join(jm, tmp, r, set[i]) != 0) {
			cormr_set_free(tmp);
			ret = -1;
			goto out;
		}
		cormr_set_free(r);
		r = tmp;
	}

	/* Rank phase. */
	{
		size_t n = cormr_set_count(r);
		const cormr_ref_t *ra = cormr_set_at(r);
		const cormr_axis_t *ax[CORMR_QUERY_MAX_AXES];
		int have_consumer = q->consumer_score != NULL;
		int have_rank = 0;
		size_t k;

		for (i = 0; i < q->n_axes; i++) {
			const cormr_axis_t *axis = cormr_axis_get(q->axes[i].slot);
			ax[i] = axis;
			if (axis && axis->rank && axis->ctx)
				have_rank = 1;
		}

		if (!have_consumer && !have_rank) {
			/* Pure-filter mode: copy the refs as-is (sorted, deduped),
			 * scores untouched. */
			if (refs)
				memcpy(refs, ra, n * sizeof(*refs));
			ret = (int)n;
			goto out;
		}

		{
			cormr_rank_t *rk = cormr_rank_new(q->top_k, q->min_score);
			if (!rk) {
				ret = -1;
				goto out;
			}
			for (k = 0; k < n; k++) {
				cormr_ref_t ref = ra[k];
				float s;
				if (have_consumer) {
					cormr_axis_score_t as[CORMR_QUERY_MAX_AXES];
					int n_as = 0;
					for (i = 0; i < q->n_axes; i++) {
						const cormr_axis_t *axis = ax[i];
						as[n_as].slot = q->axes[i].slot;
						as[n_as].valid = 0;
						as[n_as].score = 0.0f;
						if (axis && axis->rank && axis->ctx &&
						    axis->rank(axis->ctx, q->axes[i].params,
						               ref, &as[n_as].score) == 0)
							as[n_as].valid = 1;
						n_as++;
					}
					if (q->consumer_score(q->consumer_ud, ref,
					                      as, n_as, &s) == 0)
						cormr_rank_push(rk, ref, s);
				} else {
					/* Fallback: first axis (query order) that has a
					 * rank fn + ctx. */
					for (i = 0; i < q->n_axes; i++) {
						const cormr_axis_t *axis = ax[i];
						if (axis && axis->rank && axis->ctx) {
							if (axis->rank(axis->ctx,
							              q->axes[i].params,
							              ref, &s) == 0)
								cormr_rank_push(rk, ref, s);
							break;
						}
					}
				}
			}
			ret = (int)cormr_rank_sorted(rk, refs, scores);
			cormr_rank_free(rk);
		}
	}

out:
	if (r)
		cormr_set_free(r);
	for (i = 0; i < q->n_axes; i++)
		if (set[i])
			cormr_set_free(set[i]);
	return ret;
}

/* ── Backward compatibility wrappers for rec_axis_* symbols ────────── */

int rec_axis_register(const rec_axis_t *axis) { return cormr_axis_register(axis); }
const rec_axis_t *rec_axis_get(int slot) { return cormr_axis_get(slot); }
void *rec_axis_decode(int slot, const char *s) { return cormr_axis_decode(slot, s); }
int rec_axis_set_ctx(int slot, void *ctx) { return cormr_axis_set_ctx(slot, ctx); }
int rec_axis_count(void) { return cormr_axis_count(); }
int rec_query_run(const rec_query_t *q, rec_ref_t *refs, float *scores)
{
	return cormr_query_run(q, refs, scores);
}
