/*
 * fixture_id_join.c — disposable proof-of-concept, NOT production code.
 *
 * Plan: site/mm-plan/README.md §5 (phase 1, ref-type retype).
 *
 * Proves the target architecture end-to-end with REAL (non-mock) stores:
 *
 *   - One PRIMARY value map (its own corm file, here in-memory) holds the
 *     actual content (a string) and is the id authority: corm_put(hd,
 *     NULL, value) with CM_AINDEX hands back the id.
 *   - libjoint keeps its OWN file-backed store (here in-memory too --
 *     "having their own corms if need be") and is populated using that
 *     SAME id: joint_start(jd, ts, id) / joint_stop(jd, ts, id).
 *   - libsepal keeps its OWN store and is populated using the SAME id:
 *     sepal_put(vs, (rec_ref_t)id, vec, dim).
 *   - Query = interval set I (time window, via
 *     rec_axis_fill_interval / libjoint) INTERSECTED with semantic set S
 *     (ANN, via sepal_fill_approx / libsepal) -- exactly the "S ∩ T ->
 *     original string" model the mm design converged on.
 *   - The winning id(s) are looked back up in the PRIMARY map to
 *     materialize the original string ("we want the original string
 *     too").
 *
 * Writes are explicit multi-call sequences (put primary, then joint,
 * then sepal) -- no fan-out orchestrator, per the plan.
 *
 * Three seed memories:
 *   alpha @ t=1000  "alpha memory about water"   vec ~ water
 *   beta  @ t=2000  "beta memory about fire"     vec ~ fire (orthogonal)
 *   gamma @ t=3000  "gamma memory about water"   vec ~ water (near alpha)
 *
 * Query: time window [1500, 3500) (excludes alpha) ∩ semantic ~ water
 * (excludes beta) --> expect exactly {gamma}, proving the conjunction
 * picks the memory that is BOTH recent AND on-topic, not just one or the
 * other.
 */
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <ttypt/corm.h>
#include <ttypt/rec.h>
#include <ttypt/joint.h>
#include <ttypt/sepal.h>

static int failures;
static int total;

#define CHECK(cond, name)                                                     \
	do {                                                                  \
		total++;                                                      \
		if (!(cond)) {                                                \
			failures++;                                           \
			printf("FAIL: %s (line %d)\n", name, __LINE__);       \
		}                                                             \
	} while (0)

struct seed {
	const char *text;
	time_t ts;
	float vec[4];
};

static int rec_has(const rec_set_t *s, rec_ref_t r)
{
	const rec_ref_t *at = rec_set_at(s);
	size_t n = rec_set_count(s);
	size_t i;

	for (i = 0; i < n; i++)
		if (at[i] == r)
			return 1;
	return 0;
}

int main(void)
{
	static const struct seed seeds[3] = {
		{ "alpha memory about water", 1000, { 1.0f, 0.0f, 0.0f, 0.0f } },
		{ "beta memory about fire",   2000, { 0.0f, 1.0f, 0.0f, 0.0f } },
		{ "gamma memory about water", 3000, { 0.9f, 0.1f, 0.0f, 0.0f } },
	};
	uint32_t mem_hd;
	unsigned jd;
	sepal_vecstore_t *vs;
	int err = 0;
	uint32_t ids[3];
	size_t i;
	rec_set_t *tset, *sset, *inter;
	sepal_hit_t hits[3];
	size_t nhit;
	float q[4] = { 1.0f, 0.0f, 0.0f, 0.0f };

	/* --- three independent stores, own corm-backed state each --- */
	mem_hd = corm_open(NULL, NULL, CM_HNDL, CM_STR, 0xFF, CM_AINDEX);
	CHECK(mem_hd != 0, "primary map opened");

	jd = joint_init(NULL);
	/* jd is an idm-assigned slot -- 0 is a perfectly valid handle, so
	 * there is nothing to assert here beyond "it was assigned"; the
	 * real proof is that fill_interval below returns the right ids. */

	vs = sepal_open(NULL, &err);
	CHECK(vs != NULL && err == 0, "sepal store opened");

	/* --- writes: explicit multi-call sequences, same id everywhere --- */
	for (i = 0; i < 3; i++) {
		ids[i] = corm_put(mem_hd, NULL, seeds[i].text);
		joint_start(jd, seeds[i].ts, ids[i]);
		joint_stop(jd, seeds[i].ts + 1, ids[i]);
		sepal_put(vs, (rec_ref_t)ids[i], seeds[i].vec, 4);
	}
	CHECK(ids[0] != ids[1] && ids[1] != ids[2] && ids[0] != ids[2],
	      "auto-index ids unique");

	/* Primary map round-trips the original strings. */
	for (i = 0; i < 3; i++) {
		const char *got = corm_get(mem_hd, &ids[i]);
		CHECK(got && !strcmp(got, seeds[i].text),
		      "primary map returns original string");
	}

	/* --- I: interval set, time window [1500, 3500) --- */
	tset = rec_set_new();
	CHECK(rec_axis_fill_interval(jd, 1500, 3500, tset) == 0,
	      "joint fill_interval ok");
	CHECK(!rec_has(tset, ids[0]), "alpha (t=1000) excluded from window");
	CHECK(rec_has(tset, ids[1]), "beta (t=2000) inside window");
	CHECK(rec_has(tset, ids[2]), "gamma (t=3000) inside window");

	/* --- S: semantic set, ANN over the "water" query vector ---
	 * sepal_search runs both ANN stages (Hamming prefilter + exact
	 * cosine rerank) and applies min_sim itself -- unlike
	 * sepal_fill_approx, which is stage-1-only (a Hamming candidate
	 * pool, not filtered by cosine/min_sim at all; with m >= n it
	 * would return every ref in the store). Build S from the hits. */
	nhit = sepal_search(vs, q, 4, 3, 0.5f, 3, hits);
	sset = rec_set_new();
	for (i = 0; i < nhit; i++)
		rec_set_push(sset, hits[i].ref);
	rec_set_seal(sset);
	CHECK(rec_has(sset, ids[0]), "alpha (water) matches semantic query");
	CHECK(!rec_has(sset, ids[1]), "beta (fire) excluded by min_sim");
	CHECK(rec_has(sset, ids[2]), "gamma (water) matches semantic query");

	/* --- S ∩ I: exactly gamma (recent AND on-topic) --- */
	inter = rec_set_new();
	CHECK(rec_set_intersect(inter, sset, tset) == 0, "rec_set_intersect ok");
	CHECK(rec_set_count(inter) == 1 && rec_has(inter, ids[2]),
	      "S ∩ I == {gamma} exactly");

	/* --- materialize the original string for the winning id --- */
	if (rec_set_count(inter) == 1) {
		rec_ref_t winner = rec_set_at(inter)[0];
		uint32_t winner_key = (uint32_t)winner;
		const char *text = corm_get(mem_hd, &winner_key);

		CHECK(text && !strcmp(text, "gamma memory about water"),
		      "winning id resolves to the original string via the "
		      "primary map");
		if (text)
			printf("winner id=%llu -> \"%s\"\n",
			       (unsigned long long)winner, text);
	}

	rec_set_free(tset);
	rec_set_free(sset);
	rec_set_free(inter);
	sepal_close(vs);
	joint_close(jd);
	corm_close(mem_hd);

	printf("Results: %d/%d passed", total - failures, total);
	if (failures)
		printf(", %d FAILED", failures);
	printf("\n");
	return failures ? 1 : 0;
}
