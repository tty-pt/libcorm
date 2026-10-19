/**
 * @file cormr_test.c
 * @brief Unit tests for the modernized cormr_* API and shortened record aliases.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

/* Prove that <ttypt/corm.h> alone provides both map and cormr_* symbols */
#include "./../include/ttypt/corm.h"

struct test_person {
	uint32_t age;
	char name[32];
};

static void
test_cormr_sets(void)
{
	printf("=== Cormr Candidate Sets ===\n");
	cormr_set_t *s1 = cormr_set_new();
	assert(s1 != NULL);
	assert(cormr_set_count(s1) == 0);

	cormr_set_push(s1, 42);
	cormr_set_push(s1, 10);
	cormr_set_push(s1, 42); /* duplicate */
	cormr_set_push(s1, 5);
	assert(cormr_set_count(s1) == 4);

	cormr_set_seal(s1);
	assert(cormr_set_count(s1) == 3);
	const cormr_ref_t *refs = cormr_set_at(s1);
	assert(refs[0] == 5);
	assert(refs[1] == 10);
	assert(refs[2] == 42);

	cormr_set_t *s2 = cormr_set_new();
	cormr_set_push(s2, 10);
	cormr_set_push(s2, 99);
	cormr_set_seal(s2);

	cormr_set_t *inter = cormr_set_new();
	assert(cormr_set_intersect(inter, s1, s2) == 0);
	assert(cormr_set_count(inter) == 1);
	assert(cormr_set_at(inter)[0] == 10);

	/* Approximation bounds */
	assert(cormr_set_is_approx(s1) == 0);
	assert(cormr_set_bound(s1) == 1.0f);
	assert(cormr_set_approx(s1, CORMR_SET_APPROX, 0.85f) == 0);
	assert(cormr_set_is_approx(s1) == 1);
	assert(cormr_set_bound(s1) == 0.85f);

	cormr_set_free(s1);
	cormr_set_free(s2);
	cormr_set_free(inter);
	printf("  OK\n");
}

static void
test_cormr_rank(void)
{
	printf("=== Cormr Ranking Min-Heap ===\n");
	cormr_rank_t *rk = cormr_rank_new(2, 0.0f);
	assert(rk != NULL);

	cormr_rank_push(rk, 1, 0.2f);
	cormr_rank_push(rk, 2, 0.9f);
	cormr_rank_push(rk, 3, 0.5f);

	cormr_ref_t best_refs[2];
	float best_scores[2];
	size_t n = cormr_rank_sorted(rk, best_refs, best_scores);
	assert(n == 2);
	assert(best_refs[0] == 2 && best_scores[0] == 0.9f);
	assert(best_refs[1] == 3 && best_scores[1] == 0.5f);

	cormr_rank_free(rk);
	printf("  OK\n");
}

static void
test_corm_record_shortened_aliases(void)
{
	printf("=== Shortened Corm Record API Aliases ===\n");

	corm_field_t fields[] = {
		{ "age", 0, offsetof(struct test_person, age), sizeof(uint32_t), 0, 0, NULL },
		{ "name", 0, offsetof(struct test_person, name), 32, 0, 0, NULL }
	};

	/* Test corm_rec_reg alias */
	uint32_t rec_id = corm_rec_reg("person", sizeof(struct test_person), fields, 2);
	assert(rec_id > 0);

	/* Test corm_rec_type alias */
	uint32_t type_id = corm_rec_type(rec_id);
	assert(type_id != CM_MISS);

	/* Test corm_field_target alias */
	corm_field_target(rec_id, "name", 1234);

	printf("  OK\n");
}

static void
test_cormr_cli_helpers(void)
{
	printf("=== Cormr Spec & CLI Helpers ===\n");
	const char *spec = "field='title and subtitle' count=42";
	const char *cur = spec;
	const char *key, *val;
	size_t klen, vlen;
	int quoted;

	assert(cormr_spec_scan(&cur, &key, &klen, &val, &vlen, &quoted) == 1);
	assert(cormr_key_eq(key, klen, "field"));
	assert(quoted == 1);
	assert(vlen == strlen("title and subtitle"));

	assert(cormr_spec_scan(&cur, &key, &klen, &val, &vlen, &quoted) == 1);
	assert(cormr_key_eq(key, klen, "count"));
	assert(quoted == 0);
	int cnt = 0;
	assert(cormr_cli_int_b(val, val + vlen, &cnt) == 0);
	assert(cnt == 42);

	assert(cormr_spec_scan(&cur, &key, &klen, &val, &vlen, &quoted) == 0);
	printf("  OK\n");
}

int
main(void)
{
	test_cormr_sets();
	test_cormr_rank();
	test_corm_record_shortened_aliases();
	test_cormr_cli_helpers();
	printf("\nALL CORMR TESTS PASSED\n");
	return 0;
}
