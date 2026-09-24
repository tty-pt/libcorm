#include "./../include/ttypt/corm.h"

#include <stdio.h>
#include <string.h>

#include <ttypt/qsys.h>

#define DB_MASK 0x3
#define MAX_LEN (BUFSIZ * 2)

char *good = "✅";
char *bad = "❌";

unsigned errors = 0;

typedef unsigned dbtype_t[2];

enum dbtype {
	UTOS,
	STOU,
	UTOU,
	STOS,
};

dbtype_t dbtypes[] = {
	{ CM_HNDL, CM_STR },
	{ CM_STR, CM_HNDL },
	{ CM_HNDL, CM_HNDL },
	{ CM_STR, CM_STR },
};

typedef int print_t(const void *key);
typedef int cmp_t(const void *a, const void *b);

typedef struct {
	print_t *print;
	cmp_t *cmp;
} type_meta_t;

int s_print(const void *key) {
	return printf("%s", (char *) key);
}

int u_print(const void *key) {
	return printf("%u", * (unsigned *) key);
}

int s_cmp(const void *a, const void *b) {
	return strcmp(a, b);
}

int u_cmp(const void *a, const void *b) {
	return (* (unsigned *) a)
		- (* (unsigned *) b);
}

type_meta_t type_meta[] = {
	{ .print = s_print, .cmp = s_cmp, },
	{ .print = u_print, .cmp = u_cmp, },
	{ .print = s_print, .cmp = s_cmp, },
};

enum corm_mbr {
	CM_KEY = 0,
	CM_VALUE = 1,
};

enum meta_flags {
	CM_REVERSE = 16,
};

typedef struct {
	unsigned type, flags;
} hd_meta_t;

hd_meta_t hd_meta[32];

static inline int
rmbr_get(unsigned hd, unsigned mbr)
{
	hd_meta_t *meta = &hd_meta[hd];
	unsigned rmbr = meta->flags & CM_REVERSE
		? !mbr : mbr;
	return rmbr;
}

static int
type_print(unsigned hd, unsigned mbr, const void *key)
{
	hd_meta_t *meta = &hd_meta[hd];
	unsigned rmbr = rmbr_get(hd, mbr);
	unsigned type = dbtypes[meta->type][rmbr];
	return type_meta[type].print(key);
}

static int
type_cmp(unsigned hd, unsigned mbr, const void *a, void *b)
{
	hd_meta_t *meta = &hd_meta[hd];
	unsigned rmbr = rmbr_get(hd, mbr);
	unsigned type = dbtypes[meta->type][rmbr];
	return type_meta[type].cmp(a, b);
}

unsigned gen_open(enum dbtype type, unsigned flags) {
	dbtype_t *dbtype = &dbtypes[type];

	unsigned hd = corm_open(NULL, NULL,
			(*dbtype)[CM_KEY],
			(*dbtype)[CM_VALUE],
			DB_MASK, flags);

	hd_meta[hd].type = type;
	hd_meta[hd].flags = flags;

	if (flags & CM_MIRROR) {
		unsigned rhd = hd + 1;
		hd_meta[rhd].type = type;
		hd_meta[rhd].flags = flags | CM_REVERSE;
	}

	return hd;
}

static int
_gen_get(unsigned hd, void *key, const void *value, void *expects, int reverse)
{
	char *mark, *rgood = good, *rbad = bad;
	const void *svalue;

	if (reverse) {
		rgood = bad;
		rbad = good;
	}

	printf("gen_get_test(");
	type_print(hd, CM_KEY, key);
	printf(", ");
	type_print(hd, CM_VALUE, value);
	printf(") = ");

	svalue = corm_get(hd, key);
	if (!svalue) {
		printf("-1 %s\n", rbad);
		return !reverse;
	}

	mark = type_cmp(hd, CM_VALUE, svalue, expects)
		? rbad : rgood;

	type_print(hd, CM_VALUE, svalue);

	printf(" %s\n", mark);
	return reverse;
}

static inline int
gen_get(unsigned hd, void *key, void *expects)
{
	int ret = _gen_get(hd, key, expects, expects, 0);
	errors += ret;
	return ret;
}

static inline unsigned
gen_put(unsigned hd, void *key, void *value)
{
	static unsigned akeys[8];
	static unsigned akey;

	akey = corm_put(hd, key, value);
	if (!key) {
		akeys[akey] = akey;
		key = &akeys[akey];
	}

	gen_get(hd, key, value);
	return akey;
}

static inline int
gen_del(unsigned hd, void *key, void *value)
{
	int ret;
	corm_del(hd, key);
	ret = _gen_get(hd, key, value, value, 1);
	errors += ret;
	return ret;
}

static int
gen_iter_check(unsigned hd, const void *key_start, unsigned flags,
			   int n_expected, const void *expected_keys[])
{
	unsigned cur_id;
	const void *key, *value;
	int i = 0;
	int iter_errors = 0;

	printf("gen_iter_check(hd=%u, start_key=", hd);
	if (key_start) type_print(hd, CM_KEY, key_start);
	else printf("NULL");
	printf(", flags=0x%x)\n", flags);

	cur_id = corm_iter(hd, key_start, flags);
	while (corm_next(&key, &value, cur_id)) {
		printf("  > iter %d: key=", i);
		type_print(hd, CM_KEY, key);

		if (i >= n_expected) {
			printf(" %s (got more than %d expected)\n", bad, n_expected);
			iter_errors++;
		} else if (type_cmp(hd, CM_KEY, (void*)key, (void*)expected_keys[i]) != 0) {
			printf(" %s (expected '", bad);
			type_print(hd, CM_KEY, expected_keys[i]);
			printf("')\n");
			iter_errors++;
		} else {
			printf(" %s\n", good);
		}
		i++;
	}

	if (i != n_expected) {
		printf("  > iter end: %s (got %d items, expected %d)\n", bad, i, n_expected);
		iter_errors++;
	}

	if (iter_errors > 0) {
		printf("  > RESULT: %d iteration errors %s\n", iter_errors, bad);
		errors += iter_errors;
		return 1; // failure
	}

	printf("  > RESULT: all %d items correct %s\n", n_expected, good);
	return 0; // success
}

static inline void
test_first(void)
{
	unsigned hd = gen_open(UTOS, 0);

	unsigned keys[] = { 3, 5 };

	gen_put(hd, &keys[0], "hello");
	gen_put(hd, &keys[1], "hi");

	gen_del(hd, &keys[0], "hello");

	corm_close(hd);
}

static inline void
test_second(void)
{
	unsigned hd = gen_open(STOU, 0);
	unsigned values[] = { 9, 7 };

	gen_put(hd, "hello", &values[0]);
	gen_put(hd, "hi", &values[1]);

	corm_close(hd);
}

static inline void
iter_print(unsigned hd,
		const void *key,
		const void *value)
{
	printf("ITER '");
	type_print(hd, CM_KEY, key);
	printf("' - '");
	type_print(hd, CM_VALUE, value);
	printf("'\n");
}

static inline void
test_third(void)
{
	unsigned hd = gen_open(UTOU, 0), cur_id;
	unsigned keys[] = { 3, 9 };
	unsigned values[] = { 5, 7 };
	const void *key, *value;

	gen_put(hd, &keys[0], &values[0]);
	gen_put(hd, &keys[1], &values[1]);

	cur_id = corm_iter(hd, NULL, 0);
	while (corm_next(&key, &value, cur_id))
		iter_print(hd, key, value);

	corm_close(hd);
}

static inline void
test_fourth(void)
{
	unsigned hd = gen_open(UTOS, CM_MIRROR),
		 rhd = hd + 1;
	unsigned keys[] = { 3, 9 };

	gen_put(hd, &keys[0], "hello");
	gen_put(hd, &keys[1], "hi");

	gen_get(rhd, "hello", &keys[0]);
	gen_get(rhd, "hi", &keys[1]);

	corm_close(hd);
}

static inline void
test_fifth(void)
{
	unsigned hd = gen_open(STOU, CM_MIRROR),
		 rhd = hd + 1;
	unsigned values[] = { 3, 9 };

	gen_put(hd, "hello", &values[0]);
	gen_put(hd, "hi", &values[1]);
	printf("fifth two-way\n");

	gen_get(rhd, &values[0], "hello");
	gen_get(rhd, &values[1], "hi");

	corm_close(hd);
}

static inline void
test_sixth(void)
{
	unsigned hd = gen_open(STOS, CM_MIRROR),
		 rhd = hd + 1;

	gen_put(hd, "hello", "hellov");
	gen_put(hd, "hi", "hiv");

	gen_get(rhd, "hellov", "hello");
	gen_get(rhd, "hiv", "hi");

	corm_close(hd);
}

static inline void
test_seventh(void)
{
	unsigned hd = gen_open(STOS, CM_MIRROR),
		 rhd = hd + 1;
	unsigned cur_id;
	const void *key, *value;

	gen_put(hd, "hello", "olleh");
	gen_put(hd, "hi", "ih");
	gen_put(hd, "ola", "alo");

	cur_id = corm_iter(hd, NULL, 0);
	while (corm_next(&key, &value, cur_id))
		iter_print(hd, key, value);

	printf("keyed iter\n");
	cur_id = corm_iter(hd, "hello", 0);
	while (corm_next(&key, &value, cur_id))
		iter_print(hd, key, value);

	gen_del(rhd, "alo", NULL);
	gen_get(rhd, "olleh", "hello");
	gen_get(rhd, "ih", "hi");

	printf("reverse iter\n");
	cur_id = corm_iter(rhd, NULL, 0);
	while (corm_next(&key, &value, cur_id))
		iter_print(rhd, key, value);

	printf("reverse keyed iter\n");
	cur_id = corm_iter(rhd, "ih", 0);
	while (corm_next(&key, &value, cur_id))
		iter_print(rhd, key, value);

	printf("final iter\n");
	cur_id = corm_iter(hd, NULL, 0);
	while (corm_next(&key, &value, cur_id))
		iter_print(hd, key, value);

	corm_close(hd);
}

static inline
void test_eighth(void)
{
	unsigned cur_id;
	unsigned hd = gen_open(UTOS, CM_AINDEX | CM_MIRROR),
		 rhd = hd + 1;
	const void *key, *value;

	gen_put(hd, NULL, "hello");
	gen_put(hd, NULL, "hi");
	gen_put(hd, NULL, "ola");

	cur_id = corm_iter(hd, NULL, 0);
	while (corm_next(&key, &value, cur_id))
		iter_print(hd, key, value);

	printf("reversed\n");
	cur_id = corm_iter(rhd, NULL, 0);
	while (corm_next(&key, &value, cur_id))
		iter_print(rhd, key, value);

	corm_close(hd);
}

static inline
void test_nineth(void)
{
	unsigned cur_id;
	const void *key, *value;
	unsigned hd = gen_open(UTOS, CM_MULTIVALUE | CM_SORTED);
	unsigned keys[] = { 3, 3, 2 };

	gen_put(hd, &keys[0], "hello");
	corm_put(hd, &keys[1], "hi");
	/* errors += _gen_get(hd, &keys[1], "hi", "hello", 0); */
	gen_put(hd, &keys[2], "ola");

	cur_id = corm_iter(hd, NULL, 0);
	while (corm_next(&key, &value, cur_id))
		printf("ITER '%u' - '%s'\n", *(unsigned *)key, (char *)value);

	printf("Keyed iter\n");
	cur_id = corm_iter(hd, &keys[0], 0);
	while (corm_next(&key, &value, cur_id))
		printf("ITER '%u' - '%s'\n", *(unsigned *)key, (char *)value);

	gen_del(hd, &keys[0], NULL);
	printf("After del keyed iter\n");
	cur_id = corm_iter(hd, &keys[0], 0);
	while (corm_next(&key, &value, cur_id))
		printf("ITER '%u' - '%s'\n", *(unsigned *)key, (char *)value);
	printf("After del unkeyed\n");
	cur_id = corm_iter(hd, NULL, 0);
	while (corm_next(&key, &value, cur_id))
		printf("ITER '%u' - '%s'\n", *(unsigned *)key, (char *)value);

	corm_close(hd);
}

static inline
void test_tenth(void)
{
	unsigned cur_id;
	unsigned hd = gen_open(UTOS, CM_AINDEX);
	unsigned keys[] = { 3, 3, 2 };
	const void *key, *value;

	keys[0] = gen_put(hd, NULL, "hello");
	gen_put(hd, &keys[0], "hi");
	gen_put(hd, NULL, "ola");

	cur_id = corm_iter(hd, NULL, 0);
	while (corm_next(&key, &value, cur_id))
		iter_print(hd, key, value);

	printf("Keyed iter\n");
	cur_id = corm_iter(hd, &keys[0], 0);
	while (corm_next(&key, &value, cur_id))
		iter_print(hd, key, value);

	corm_close(hd);
}

static inline
void test_eleventh(void)
{
	unsigned cur_id;
	unsigned hd = gen_open(UTOU, CM_AINDEX);
	unsigned keys[] = { 3, 6, 2 };
	unsigned values[] = { 2, 4, 3 };
	const void *key, *value;

	keys[0] = gen_put(hd, &keys[0], &values[0]);
	gen_put(hd, &keys[0], &values[1]);
	gen_put(hd, NULL, &values[2]);

	cur_id = corm_iter(hd, NULL, 0);
	while (corm_next(&key, &value, cur_id))
		iter_print(hd, key, value);

	printf("Keyed iter\n");
	cur_id = corm_iter(hd, &keys[0], 0);
	while (corm_next(&key, &value, cur_id))
		iter_print(hd, key, value);

	corm_close(hd);
}

static inline
void test_twelfth(void)
{
	printf("Testing CM_SORTED + CM_RANGE (STOS map)\n");
	// Note: Must pass CM_SORTED flag
	unsigned hd = gen_open(STOS, CM_SORTED);

	// 1. Insert out of order
	gen_put(hd, "delta", "d");
	gen_put(hd, "alpha", "a");
	gen_put(hd, "gamma", "g");
	gen_put(hd, "beta", "b");

	// 2. Test full sorted iteration
	const void *full_order[] = { "alpha", "beta", "delta", "gamma" };
	gen_iter_check(hd, NULL, CM_RANGE, 4, full_order);

	// 3. Test range start (exact key)
	const void *range_beta[] = { "beta", "delta", "gamma" };
	gen_iter_check(hd, "beta", CM_RANGE, 3, range_beta);

	// 4. Test range start (non-exact key)
	const void *range_charlie[] = { "delta", "gamma" };
	gen_iter_check(hd, "charlie", CM_RANGE, 2, range_charlie);

	// 5. Test end-of-range
	gen_iter_check(hd, "zulu", CM_RANGE, 0, NULL);

	// 6. Test "dirty" rebuild (after put/del)
	printf("Testing dirty rebuild (del/put)\n");
	gen_del(hd, "gamma", "g"); // del
	gen_put(hd, "epsilon", "e"); // add
	gen_put(hd, "bravo", "b2");  // add

	const void *final_order[] = { "alpha", "beta", "bravo", "delta", "epsilon" };
	gen_iter_check(hd, NULL, CM_RANGE, 5, final_order);

	corm_close(hd);
}

static inline
void test_thirteenth(void)
{
	printf("Testing CM_SORTED + CM_RANGE (UTOS map)\n");
	unsigned hd = gen_open(UTOS, CM_SORTED);

	unsigned keys[] = { 50, 10, 30, 20 };

	gen_put(hd, &keys[0], "fifty");
	gen_put(hd, &keys[1], "ten");
	gen_put(hd, &keys[2], "thirty");
	gen_put(hd, &keys[3], "twenty");

	// Expected keys in ascending order (10, 20, 30, 50)
	const void *full_order[] = { &keys[1], &keys[3], &keys[2], &keys[0] };
	gen_iter_check(hd, NULL, CM_RANGE, 4, full_order);

	// Test range start (key=30)
	unsigned key_30 = 30;
	const void *range_start[] = { &keys[2], &keys[0] }; // 30, 50
	gen_iter_check(hd, &key_30, CM_RANGE, 2, range_start);

	corm_close(hd);
}

static inline
void test_fourteenth(void)
{
	printf("Testing CM_RANGE fallback (non-SORTED map)\n");
	unsigned hd = gen_open(STOS, 0); // No CM_SORTED

	gen_put(hd, "delta", "d");
	gen_put(hd, "alpha", "a");
	gen_put(hd, "gamma", "g");

	// We can't guarantee order, so we just print
	// We expect to see "delta", "alpha", "gamma" in HASH order, not alphabetical
	printf("Full iteration (should be hash/insert order, NOT alphabetical):\n");
	unsigned cur_id = corm_iter(hd, NULL, CM_RANGE);
	const void *key, *value;
	while(corm_next(&key, &value, cur_id))
		iter_print(hd, key, value);

	// Test the filtering part of the fallback
	printf("Range iteration from 'c' (should be 'delta', 'gamma' in any order):\n");
	cur_id = corm_iter(hd, "c", CM_RANGE);
	while(corm_next(&key, &value, cur_id))
		iter_print(hd, key, value);

	corm_close(hd);
}

int main(void) {
	printf("first\n");
	test_first();
	printf("second\n");
	test_second();
	printf("third\n");
	test_third();
	printf("fourth\n");
	test_fourth();
	printf("fifth\n");
	test_fifth();
	printf("sixth\n");
	test_sixth();
	printf("seventh\n");
	test_seventh();
	printf("eighth\n");
	test_eighth();
#if 0
	printf("nineth\n");
	test_nineth();
#endif
	printf("tenth\n");
	test_tenth();
	printf("eleventh\n");
	test_eleventh();
	printf("twelfth\n");
	test_twelfth();
	printf("thirteenth\n");
	test_thirteenth();
	printf("fourteenth\n");
	test_fourteenth();

	return -errors;
}
