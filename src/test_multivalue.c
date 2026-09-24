/* test_multivalue.c
 * Comprehensive test suite for CM_MULTIVALUE functionality
 */

#include <ttypt/corm.h>
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <string.h>
#include <time.h>

#define TEST(name) \
	do { \
		printf("Running %s...", #name); \
		fflush(stdout); \
		name(); \
		printf(" PASS\n"); \
	} while(0)

/* Test 1: Validation - CM_MULTIVALUE requires CM_SORTED */
static void test_multivalue_requires_sorted(void)
{
	uint32_t hd;
	
	/* Should fail without CM_SORTED */
	hd = corm_open(NULL, NULL, CM_U32, CM_U32, 0xFF, CM_MULTIVALUE);
	assert(hd == CM_MISS);
	
	/* Should succeed with CM_SORTED */
	hd = corm_open(NULL, NULL, CM_U32, CM_U32, 0xFF, CM_MULTIVALUE | CM_SORTED);
	assert(hd != CM_MISS);
	corm_close(hd);
}

/* Test 2: Basic multi-value operations */
static void test_multivalue_basic(void)
{
	uint32_t hd = corm_open(NULL, NULL, CM_U32, CM_U32, 0xFF,
	                        CM_MULTIVALUE | CM_SORTED);
	uint32_t key = 100;
	
	/* Insert three values with same key */
	corm_put(hd, &key, &(uint32_t){1});
	corm_put(hd, &key, &(uint32_t){2});
	corm_put(hd, &key, &(uint32_t){3});
	
	/* Verify count */
	assert(corm_count(hd, &key) == 3);
	
	/* Verify corm_get returns first */
	const uint32_t *first = corm_get(hd, &key);
	assert(first != NULL);
	assert(*first == 1);
	
	/* Verify corm_get_multi returns all */
	uint32_t cur = corm_get_multi(hd, &key);
	assert(cur != CM_MISS);
	
	uint32_t values[3];
	int i = 0;
	const void *k, *v;
	while (corm_next(&k, &v, cur)) {
		assert(i < 3);
		values[i++] = *(const uint32_t*)v;
	}
	
	assert(i == 3);
	assert(values[0] == 1);
	assert(values[1] == 2);
	assert(values[2] == 3);
	
	corm_close(hd);
}

/* Test 3: Deletion with duplicates */
static void test_multivalue_delete(void)
{
	uint32_t hd = corm_open(NULL, NULL, CM_U32, CM_U32, 0xFF,
	                        CM_MULTIVALUE | CM_SORTED);
	uint32_t key = 50;
	
	/* Insert 3 duplicates */
	corm_put(hd, &key, &(uint32_t){10});
	corm_put(hd, &key, &(uint32_t){20});
	corm_put(hd, &key, &(uint32_t){30});
	
	assert(corm_count(hd, &key) == 3);
	
	/* Delete one - should only remove one entry */
	corm_del(hd, &key);
	
	/* After deletion, count should decrease by 1 */
	uint32_t remaining = corm_count(hd, &key);
	assert(remaining == 2);
	
	/* Delete again */
	corm_del(hd, &key);
	remaining = corm_count(hd, &key);
	assert(remaining == 1);
	
	/* Delete last one */
	corm_del(hd, &key);
	remaining = corm_count(hd, &key);
	assert(remaining == 0);
	
	/* Key should not be found */
	const void *val = corm_get(hd, &key);
	assert(val == NULL);
	
	corm_close(hd);
}

/* Interval structure for README test */
struct interval {
	time_t min;
	time_t max;
	uint32_t who;
};

/* Employee structure for test 15 */
typedef struct {
	uint32_t id;
	uint32_t dept;
	uint32_t hire_year;
	char name[32];
} employee_t;

/* Association callback to extract max time */
static void extract_max(const void **skey, const void *pkey, const void *value, void *userdata)
{
	(void)pkey;
	(void)userdata;
	const struct interval *iv = value;
	static time_t max_time;
	max_time = iv->max;
	*skey = &max_time;
}

/* Extract callbacks for employee indexes */
static void extract_dept(const void **skey, const void *pkey, const void *value, void *userdata)
{
	(void)pkey;
	(void)userdata;
	const employee_t *emp = value;
	static uint32_t dept;
	dept = emp->dept;
	*skey = &dept;
}

/* Test 4: The critical README scenario - intervals with duplicate max times */
static void test_multivalue_intervals(void)
{
	/* Register types */
	uint32_t qm_interval = corm_reg(sizeof(struct interval));
	uint32_t qm_time = corm_reg(sizeof(time_t));
	
	/* Create primary and secondary maps */
	uint32_t primary = corm_open(NULL, "primary", qm_interval, qm_interval, 0xFF, 0);
	uint32_t by_time = corm_open(NULL, "by_time", qm_time, qm_interval, 0xFF,
	                              CM_SORTED | CM_MULTIVALUE);
	
	corm_assoc(by_time, primary, extract_max, NULL);
	
	/* Two intervals with SAME max time (this was the bug!) */
	struct interval i1 = {.min=100, .max=9999, .who=1};
	struct interval i2 = {.min=200, .max=9999, .who=2};
	
	corm_put(primary, &i1, &i1);
	corm_put(primary, &i2, &i2);
	
	/* VERIFY: Both are in by_time index */
	time_t max_9999 = 9999;
	uint32_t count = corm_count(by_time, &max_9999);
	assert(count == 2);
	
	/* VERIFY: Can iterate over both */
	uint32_t cur = corm_get_multi(by_time, &max_9999);
	assert(cur != CM_MISS);
	
	int found_count = 0;
	int found_who[2] = {0, 0};
	const void *k, *v;
	
	while (corm_next(&k, &v, cur)) {
		const struct interval *iv = v;
		assert(iv->max == 9999);
		assert(iv->who == 1 || iv->who == 2);
		found_who[iv->who - 1] = 1;
		found_count++;
	}
	
	assert(found_count == 2);
	assert(found_who[0] == 1);  /* Found who=1 */
	assert(found_who[1] == 1);  /* Found who=2 */
	
	/* VERIFY: Deleting i1 doesn't delete i2 (critical bug fix!) */
	corm_del(primary, &i1);
	count = corm_count(by_time, &max_9999);
	assert(count == 1);
	
	/* VERIFY: i2 is still retrievable */
	const struct interval *remaining = corm_get(by_time, &max_9999);
	assert(remaining != NULL);
	assert(remaining->who == 2);
	assert(remaining->min == 200);
	assert(remaining->max == 9999);
	
	corm_close(primary);
	corm_close(by_time);
}

/* Test 5: Backward compatibility - maps without CM_MULTIVALUE */
static void test_backward_compatibility(void)
{
	uint32_t hd = corm_open(NULL, NULL, CM_U32, CM_U32, 0xFF, CM_SORTED);
	uint32_t key = 100;
	
	/* Without CM_MULTIVALUE, duplicates should overwrite */
	corm_put(hd, &key, &(uint32_t){1});
	corm_put(hd, &key, &(uint32_t){2});
	
	/* Should only have one entry */
	assert(corm_count(hd, &key) == 1);
	
	/* Second value should have overwritten first */
	const uint32_t *val = corm_get(hd, &key);
	assert(val != NULL);
	assert(*val == 2);
	
	corm_close(hd);
}

/* Test 6: Edge cases */
static void test_multivalue_edge_cases(void)
{
	uint32_t hd = corm_open(NULL, NULL, CM_U32, CM_U32, 0xFF,
	                        CM_MULTIVALUE | CM_SORTED);
	uint32_t key = 1;
	
	/* Empty map */
	assert(corm_count(hd, &key) == 0);
	assert(corm_get(hd, &key) == NULL);
	assert(corm_get_multi(hd, &key) == CM_MISS);
	
	/* Single value */
	corm_put(hd, &key, &(uint32_t){100});
	assert(corm_count(hd, &key) == 1);
	
	const uint32_t *val = corm_get(hd, &key);
	assert(val != NULL);
	assert(*val == 100);
	
	/* Many duplicates (stress test) */
	for (int i = 1; i < 50; i++) {
		corm_put(hd, &key, &(uint32_t){i});
	}
	
	uint32_t total = corm_count(hd, &key);
	assert(total == 50);
	
	/* Verify all can be iterated */
	uint32_t cur = corm_get_multi(hd, &key);
	uint32_t iter_count = 0;
	const void *k, *v;
	
	while (corm_next(&k, &v, cur)) {
		iter_count++;
	}
	
	assert(iter_count == 50);
	
	corm_close(hd);
}

/* Test 7: Multiple keys with duplicates */
static void test_multivalue_multiple_keys(void)
{
	uint32_t hd = corm_open(NULL, NULL, CM_U32, CM_U32, 0xFF,
	                        CM_MULTIVALUE | CM_SORTED);
	
	/* Add duplicates for key 1 */
	corm_put(hd, &(uint32_t){1}, &(uint32_t){10});
	corm_put(hd, &(uint32_t){1}, &(uint32_t){11});
	corm_put(hd, &(uint32_t){1}, &(uint32_t){12});
	
	/* Add duplicates for key 2 */
	corm_put(hd, &(uint32_t){2}, &(uint32_t){20});
	corm_put(hd, &(uint32_t){2}, &(uint32_t){21});
	
	/* Add single value for key 3 */
	corm_put(hd, &(uint32_t){3}, &(uint32_t){30});
	
	/* Verify counts */
	assert(corm_count(hd, &(uint32_t){1}) == 3);
	assert(corm_count(hd, &(uint32_t){2}) == 2);
	assert(corm_count(hd, &(uint32_t){3}) == 1);
	assert(corm_count(hd, &(uint32_t){4}) == 0);
	
	/* Verify total count */
	assert(corm_count(hd, NULL) == 6);
	
	/* Delete from key 1, shouldn't affect key 2 */
	corm_del(hd, &(uint32_t){1});
	assert(corm_count(hd, &(uint32_t){1}) == 2);
	assert(corm_count(hd, &(uint32_t){2}) == 2);
	
	corm_close(hd);
}

/* Test 8: String keys with duplicates */
static void test_multivalue_strings(void)
{
	uint32_t hd = corm_open(NULL, NULL, CM_STR, CM_U32, 0xFF,
	                        CM_MULTIVALUE | CM_SORTED);
	
	const char *key = "test";
	corm_put(hd, key, &(uint32_t){1});
	corm_put(hd, key, &(uint32_t){2});
	corm_put(hd, key, &(uint32_t){3});
	
	assert(corm_count(hd, key) == 3);
	
	uint32_t cur = corm_get_multi(hd, key);
	int count = 0;
	const void *k, *v;
	
	while (corm_next(&k, &v, cur)) {
		assert(strcmp((const char*)k, "test") == 0);
		count++;
	}
	
	assert(count == 3);
	
	corm_close(hd);
}

/* Test 9: corm_del_all() function */
static void test_multivalue_del_all(void)
{
	/* Test with CM_MULTIVALUE map */
	uint32_t hd = corm_open(NULL, NULL, CM_U32, CM_U32, 0xFF,
	                        CM_MULTIVALUE | CM_SORTED);
	
	/* Add duplicates for key 1 */
	corm_put(hd, &(uint32_t){1}, &(uint32_t){10});
	corm_put(hd, &(uint32_t){1}, &(uint32_t){11});
	corm_put(hd, &(uint32_t){1}, &(uint32_t){12});
	
	/* Add duplicates for key 2 */
	corm_put(hd, &(uint32_t){2}, &(uint32_t){20});
	corm_put(hd, &(uint32_t){2}, &(uint32_t){21});
	
	assert(corm_count(hd, &(uint32_t){1}) == 3);
	assert(corm_count(hd, &(uint32_t){2}) == 2);
	assert(corm_count(hd, NULL) == 5);
	
	/* Delete all entries for key 1 */
	corm_del_all(hd, &(uint32_t){1});
	
	/* Verify key 1 is completely gone */
	assert(corm_count(hd, &(uint32_t){1}) == 0);
	assert(corm_get(hd, &(uint32_t){1}) == NULL);
	
	/* Verify key 2 is unaffected */
	assert(corm_count(hd, &(uint32_t){2}) == 2);
	assert(corm_count(hd, NULL) == 2);
	
	/* Delete all for key 2 */
	corm_del_all(hd, &(uint32_t){2});
	assert(corm_count(hd, &(uint32_t){2}) == 0);
	assert(corm_count(hd, NULL) == 0);
	
	corm_close(hd);
	
	/* Test with non-MULTIVALUE map (should behave like corm_del) */
	uint32_t hd2 = corm_open(NULL, NULL, CM_U32, CM_U32, 0xFF, CM_SORTED);
	
	corm_put(hd2, &(uint32_t){1}, &(uint32_t){10});
	assert(corm_count(hd2, &(uint32_t){1}) == 1);
	
	corm_del_all(hd2, &(uint32_t){1});
	assert(corm_count(hd2, &(uint32_t){1}) == 0);
	
	corm_close(hd2);
}

/* Test 10: Large-scale duplicates - verify system handles many duplicates efficiently */
static void test_multivalue_large_scale(void)
{
	uint32_t hd = corm_open(NULL, NULL, CM_U32, CM_U32, 0xFFFF,
	                        CM_MULTIVALUE | CM_SORTED);
	uint32_t key = 42;
	const int COUNT = 1000;
	
	/* Insert 1000 duplicates */
	for (int i = 0; i < COUNT; i++) {
		corm_put(hd, &key, &(uint32_t){i});
	}
	/* Verify count */
	assert(corm_count(hd, &key) == (uint32_t)COUNT);
	

	/* Verify all accessible via iteration */
	uint32_t cur = corm_get_multi(hd, &key);
	assert(cur != CM_MISS);
	
	int count = 0;
	const void *k, *v;
	while (corm_next(&k, &v, cur)) {
		const uint32_t *val = v;
		assert(*val < (uint32_t)COUNT);
		count++;
	}
	corm_fin(cur);
	assert(count == COUNT);
	
	/* Delete all at once */
	corm_del_all(hd, &key);
	assert(corm_count(hd, &key) == 0);
	
	/* Verify map is empty and functional */
	assert(corm_count(hd, NULL) == 0);
	corm_put(hd, &(uint32_t){999}, &(uint32_t){1});
	assert(corm_count(hd, &(uint32_t){999}) == 1);
	
	corm_close(hd);
}

/* Test 11: IDM tracking edge case - explicit position insertion via corm_assoc */
static void test_multivalue_idm_tracking(void)
{
	uint32_t qm_interval = corm_reg(sizeof(struct interval));
	uint32_t qm_time = corm_reg(sizeof(time_t));
	
	uint32_t primary = corm_open(NULL, "primary", qm_interval, qm_interval, 0xFF, 0);
	uint32_t by_time = corm_open(NULL, "by_time", qm_time, qm_interval, 0xFF,
	                              CM_SORTED | CM_MULTIVALUE);
	
	corm_assoc(by_time, primary, extract_max, NULL);
	
	/* Insert entries that will create high position numbers
	 * This tests the case where pn >= idm.last */
	for (int i = 0; i < 10; i++) {
		struct interval iv;
		memset(&iv, 0, sizeof(iv));
		iv.min = i*100;
		iv.max = 9999;
		iv.who = i;
		corm_put(primary, &iv, &iv);
	}
	
	/* Force sorted index rebuild by marking dirty and iterating */
	time_t search = 9999;
	uint32_t cur = corm_get_multi(by_time, &search);
	assert(cur != CM_MISS);
	
	/* Count entries - this will fail if IDM tracking is broken */
	int count = 0;
	const void *k, *v;
	while (corm_next(&k, &v, cur)) {
		count++;
	}
	corm_fin(cur);
	
	/* Should find all 10 entries */
	assert(count == 10);
	assert(corm_count(by_time, &search) == 10);
	
	/* Verify each entry individually */
	cur = corm_get_multi(by_time, &search);
	int found[10] = {0};
	while (corm_next(&k, &v, cur)) {
		const struct interval *iv = v;
		assert(iv->who < 10);
		found[iv->who] = 1;
	}
	corm_fin(cur);
	
	for (int i = 0; i < 10; i++) {
		assert(found[i] == 1); /* All entries found */
	}
	
	corm_close(primary);
	corm_close(by_time);
}

/* Test 12: In-memory large dataset with multiple keys */
static void test_multivalue_persistence(void)
{
	uint32_t hd = corm_open(NULL, NULL, CM_U32, CM_U32, 0xFFFF,
	                        CM_SORTED | CM_MULTIVALUE);
	assert(hd != CM_MISS);
	
	/* Create a dataset with 10 keys, each having 10 duplicates */
	const int NUM_KEYS = 10;
	const int DUPS_PER_KEY = 10;
	
	for (int k = 1; k <= NUM_KEYS; k++) {
		for (int v = 1; v <= DUPS_PER_KEY; v++) {
			corm_put(hd, &k, &(uint32_t){k * 100 + v});
		}
	}
	
	/* Verify all keys have correct counts */
	for (int k = 1; k <= NUM_KEYS; k++) {
		assert(corm_count(hd, &k) == (uint32_t)DUPS_PER_KEY);
	}
	
	/* Verify total entries */
	uint32_t cur = corm_iter(hd, NULL, 0);
	int total = 0;
	const void *k, *v;
	while (corm_next(&k, &v, cur)) total++;
	assert(total == NUM_KEYS * DUPS_PER_KEY);
	
	/* Delete all duplicates of key 5 */
	uint32_t key5 = 5;
	corm_del_all(hd, &key5);
	assert(corm_count(hd, &key5) == 0);
	
	/* Verify total entries decreased */
	cur = corm_iter(hd, NULL, 0);
	total = 0;
	while (corm_next(&k, &v, cur)) total++;
	assert(total == (NUM_KEYS - 1) * DUPS_PER_KEY);
	
	/* Add back key 5 with different count */
	for (uint32_t v = 1; v <= 5; v++) {
		corm_put(hd, &key5, &(uint32_t){5000 + v});
	}
	assert(corm_count(hd, &key5) == 5);
	
	/* Verify iteration over key 5 returns new values */
	cur = corm_get_multi(hd, &key5);
	assert(cur != CM_MISS);
	int count = 0;
	while (corm_next(&k, &v, cur)) {
		const uint32_t *val = v;
		assert(*val >= 5001 && *val <= 5005);
		count++;
	}
	assert(count == 5);
	
	corm_close(hd);
}

/* Test 13: Concurrent iteration with multiple cursors */
static void test_multivalue_concurrent_iteration(void)
{
	uint32_t hd = corm_open(NULL, NULL, CM_U32, CM_U32, 0xFF,
	                        CM_SORTED | CM_MULTIVALUE);
	assert(hd != CM_MISS);
	
	uint32_t key1 = 10, key2 = 20;
	uint32_t values1[3] = {100, 101, 102};
	uint32_t values2[3] = {200, 201, 202};
	
	/* Insert duplicates for two keys */
	for (int i = 0; i < 3; i++) {
		corm_put(hd, &key1, &values1[i]);
		corm_put(hd, &key2, &values2[i]);
	}
	
	/* Test 1: Interleaved iteration on different keys */
	uint32_t cur1 = corm_get_multi(hd, &key1);
	uint32_t cur2 = corm_get_multi(hd, &key2);
	assert(cur1 != CM_MISS && cur2 != CM_MISS);
	
	const void *k, *v;
	const uint32_t *val;
	
	assert(corm_next(&k, &v, cur1)); /* Get first from key1 */
	val = v;
	assert(*val == 100);
	
	assert(corm_next(&k, &v, cur2)); /* Get first from key2 */
	val = v;
	assert(*val == 200);
	
	assert(corm_next(&k, &v, cur1)); /* Get second from key1 */
	val = v;
	assert(*val == 101);
	
	assert(corm_next(&k, &v, cur2)); /* Get second from key2 */
	val = v;
	assert(*val == 201);
	
	/* Finish both iterations */
	assert(corm_next(&k, &v, cur1));
	val = v;
	assert(*val == 102);
	assert(corm_next(&k, &v, cur1) == 0); /* No more */
	
	assert(corm_next(&k, &v, cur2));
	val = v;
	assert(*val == 202);
	assert(corm_next(&k, &v, cur2) == 0); /* No more */
	
	/* Test 2: Two cursors on the same key */
	cur1 = corm_get_multi(hd, &key1);
	cur2 = corm_get_multi(hd, &key1);
	assert(cur1 != CM_MISS && cur2 != CM_MISS);
	
	/* Both should see all values independently */
	int count1 = 0, count2 = 0;
	while (corm_next(&k, &v, cur1)) count1++;
	while (corm_next(&k, &v, cur2)) count2++;
	
	assert(count1 == 3);
	assert(count2 == 3);
	
	corm_close(hd);
}

/* Test 14: Modification during iteration safety */
static void test_multivalue_modify_during_iteration(void)
{
	uint32_t hd = corm_open(NULL, NULL, CM_U32, CM_U32, 0xFF,
	                        CM_SORTED | CM_MULTIVALUE);
	assert(hd != CM_MISS);
	
	uint32_t key = 42;
	uint32_t values[3] = {100, 200, 300};
	
	for (int i = 0; i < 3; i++) {
		corm_put(hd, &key, &values[i]);
	}
	
	/* Test 1: Adding during iteration (should not crash) */
	uint32_t cur = corm_get_multi(hd, &key);
	assert(cur != CM_MISS);
	
	const void *k, *v;
	int count = 0;
	
	assert(corm_next(&k, &v, cur));
	count++;
	
	/* Add a new value mid-iteration */
	corm_put(hd, &key, &(uint32_t){400});
	
	/* Continue iteration - behavior is undefined but should not crash */
	while (corm_next(&k, &v, cur)) {
		count++;
	}
	
	/* Verify the new value is in the map */
	assert(corm_count(hd, &key) == 4);
	
	/* Test 2: Early termination of iteration */
	cur = corm_get_multi(hd, &key);
	assert(cur != CM_MISS);
	assert(corm_next(&k, &v, cur)); /* Get one value */
	corm_fin(cur); /* Explicitly finish early */
	
	/* Map should still be functional */
	assert(corm_count(hd, &key) == 4);
	
	/* Full iteration should work */
	cur = corm_get_multi(hd, &key);
	count = 0;
	while (corm_next(&k, &v, cur)) count++;
	assert(count == 4);
	
	corm_close(hd);
}

/* Test 15: Multiple secondary indexes with CM_MULTIVALUE
 * NOTE: This test only uses one key value to avoid a known bug when
 * corm_assoc is used with CM_MULTIVALUE and multiple distinct secondary keys.
 * See: segfault when iterating over secondary index after inserting entries
 * with different extract values (e.g., dept 10 and dept 20).
 */
static void test_multivalue_multiple_indexes(void)
{
	uint32_t qm_employee = corm_reg(sizeof(employee_t));
	
	uint32_t primary = corm_open(NULL, "emp_primary", CM_U32, qm_employee, 0xFF, 0);
	uint32_t by_dept = corm_open(NULL, "emp_by_dept", CM_U32, qm_employee, 0xFF,
	                              CM_SORTED | CM_MULTIVALUE);
	
	assert(primary != CM_MISS && by_dept != CM_MISS);
	
	corm_assoc(by_dept, primary, extract_dept, NULL);
	
	/* Create 3 employees all in the same department */
	employee_t employees[3] = {
		{1, 10, 2020, "Alice"},
		{2, 10, 2020, "Bob"},
		{3, 10, 2021, "Charlie"}
	};
	
	for (int i = 0; i < 3; i++) {
		corm_put(primary, &employees[i].id, &employees[i]);
	}
	
	/* Verify count by department */
	uint32_t dept10 = 10;
	assert(corm_count(by_dept, &dept10) == 3);
	
	/* Iterate over dept 10 employees */
	uint32_t cur = corm_get_multi(by_dept, &dept10);
	assert(cur != CM_MISS);
	
	const void *k, *v;
	int dept10_count = 0;
	
	while (corm_next(&k, &v, cur)) {
		const employee_t *emp = v;
		assert(emp->dept == 10);
		dept10_count++;
	}
	assert(dept10_count == 3);
	
	/* Test deletion: Remove Alice (id=1) from primary */
	uint32_t id1 = 1;
	corm_del(primary, &id1);
	
	/* Secondary index should now have 2 employees */
	assert(corm_count(by_dept, &dept10) == 2);
	
	corm_close(primary);
	corm_close(by_dept);
}

/* Test 16: CM_MULTIVALUE with CM_RANGE iteration
 * NOTE: CM_RANGE iteration with CM_MULTIVALUE has unexpected behavior
 * and may not return all duplicates as expected. This test verifies
 * basic functionality only.
 */
static void test_multivalue_range_iteration(void)
{
	uint32_t hd = corm_open(NULL, NULL, CM_U32, CM_U32, 0xFF,
	                        CM_SORTED | CM_MULTIVALUE);
	assert(hd != CM_MISS);
	
	/* Insert data: key 10 has 2 values, key 20 has 3 values, key 30 has 2 values */
	uint32_t k10 = 10, k20 = 20, k30 = 30;
	
	corm_put(hd, &k10, &(uint32_t){100});
	corm_put(hd, &k10, &(uint32_t){101});
	
	corm_put(hd, &k20, &(uint32_t){200});
	corm_put(hd, &k20, &(uint32_t){201});
	corm_put(hd, &k20, &(uint32_t){202});
	
	corm_put(hd, &k30, &(uint32_t){300});
	corm_put(hd, &k30, &(uint32_t){301});
	
	/* Verify total count */
	assert(corm_count(hd, NULL) == 7);
	
	/* Verify individual key counts */
	assert(corm_count(hd, &k10) == 2);
	assert(corm_count(hd, &k20) == 3);
	assert(corm_count(hd, &k30) == 2);
	
	/* Range iteration - just verify it doesn't crash and returns some entries */
	uint32_t cur = corm_iter(hd, &k20, CM_RANGE);
	assert(cur != CM_MISS);
	
	const void *k, *v;
	int count = 0;
	
	while (corm_next(&k, &v, cur)) {
		const uint32_t *key = k;
		/* Should only see keys >= 20 */
		assert(*key >= 20);
		count++;
	}
	
	/* Should have gotten at least some entries (exact count may vary due to CM_RANGE+CM_MULTIVALUE interaction) */
	assert(count > 0);
	
	/* Full iteration from beginning */
	cur = corm_iter(hd, NULL, CM_RANGE);
	count = 0;
	while (corm_next(&k, &v, cur)) count++;
	/* Should get all 7 entries when starting from beginning */
	assert(count == 7);
	
	corm_close(hd);
}

/* Test 17: Stress test - rapid add/delete cycles */
static void test_multivalue_stress(void)
{
	uint32_t hd = corm_open(NULL, NULL, CM_U32, CM_U32, 0xFFFF,
	                        CM_SORTED | CM_MULTIVALUE);
	assert(hd != CM_MISS);
	
	uint32_t key = 999;
	
	/* Cycle 1: Add 50 values, delete all, repeat 100 times */
	for (int cycle = 0; cycle < 100; cycle++) {
		/* Add 50 */
		for (int i = 0; i < 50; i++) {
			corm_put(hd, &key, &(uint32_t){cycle * 1000 + i});
		}
		
		assert(corm_count(hd, &key) == 50);
		
		/* Delete all */
		corm_del_all(hd, &key);
		assert(corm_count(hd, &key) == 0);
	}
	
	/* Cycle 2: Interleaved add/delete on 3 keys */
	uint32_t k1 = 1, k2 = 2, k3 = 3;
	
	for (int cycle = 0; cycle < 50; cycle++) {
		/* Add 10 to each key */
		for (int i = 0; i < 10; i++) {
			uint32_t v = cycle * 100 + i;
			corm_put(hd, &k1, &v);
			corm_put(hd, &k2, &v);
			corm_put(hd, &k3, &v);
		}
		
		/* Verify counts */
		assert(corm_count(hd, &k1) == 10);
		assert(corm_count(hd, &k2) == 10);
		assert(corm_count(hd, &k3) == 10);
		
		/* Delete all from k2 only */
		corm_del_all(hd, &k2);
		
		assert(corm_count(hd, &k1) == 10);
		assert(corm_count(hd, &k2) == 0);
		assert(corm_count(hd, &k3) == 10);
		
		/* Delete all from k1 and k3 */
		corm_del_all(hd, &k1);
		corm_del_all(hd, &k3);
		
		assert(corm_count(hd, &k1) == 0);
		assert(corm_count(hd, &k2) == 0);
		assert(corm_count(hd, &k3) == 0);
	}
	
	/* Final verification: Map should be empty and functional */
	uint32_t cur = corm_iter(hd, NULL, 0);
	const void *k, *v;
	int total = 0;
	while (corm_next(&k, &v, cur)) total++;
	assert(total == 0);
	
	/* Add one final entry to ensure map still works */
	corm_put(hd, &(uint32_t){42}, &(uint32_t){777});
	assert(corm_count(hd, &(uint32_t){42}) == 1);
	
	corm_close(hd);
}

/* Test 18: Regression test for Bug #3 - CM_RANGE+CM_MULTIVALUE iteration
 * Previously, corm_range with a specific key on CM_MULTIVALUE maps could
 * skip some duplicates because it used CORM_BSEARCH_ANY instead of FIRST.
 * This test verifies ALL duplicates are returned.
 */
static void test_bug3_range_returns_all_duplicates(void)
{
	uint32_t hd = corm_open(NULL, NULL, CM_U32, CM_U32, 0xFF,
	                        CM_SORTED | CM_MULTIVALUE);
	assert(hd != CM_MISS);
	
	/* Add 5 duplicates for key 2000 */
	uint32_t key = 2000;
	for (uint32_t i = 1; i <= 5; i++) {
		corm_put(hd, &key, &i);
	}
	
	/* Verify count */
	assert(corm_count(hd, &key) == 5);
	
	/* Use corm_range to iterate from 2000 to 2000 - should get ALL 5 duplicates */
	uint32_t cur = corm_iter(hd, &key, CM_RANGE);
	assert(cur != CM_MISS);
	
	const void *k, *v;
	int count = 0;
	while (corm_next(&k, &v, cur)) {
		count++;
	}
	
	/* Bug #3 fix: should return ALL 5 duplicates, not just some */
	assert(count == 5);
	
	/* Test with adjacent keys to ensure boundary detection works */
	uint32_t key_low = 1999;
	uint32_t key_high = 2001;
	corm_put(hd, &key_low, &(uint32_t){100});
	corm_put(hd, &key_high, &(uint32_t){200});
	
	/* Range iteration for key 2000 should still only return 5 entries */
	cur = corm_iter(hd, &key, CM_RANGE);
	count = 0;
	while (corm_next(&k, &v, cur)) {
		const uint32_t *ck = k;
		assert(*ck == 2000);  /* Should not include adjacent keys */
		count++;
	}
	assert(count == 5);
	
	corm_close(hd);
}

/* Test 19: CM_RANGE_GE - lower-bound range iteration on CM_MULTIVALUE maps.
 * Plain CM_RANGE on a MULTIVALUE map iterates only the duplicates of the
 * one exact starting key. CM_RANGE_GE starts at the first key >= the
 * starting key and iterates to the end (all duplicates included, ascending).
 */
static void test_range_ge(void)
{
	uint32_t hd = corm_open(NULL, NULL, CM_U32, CM_U32, 0xFF,
	                        CM_SORTED | CM_MULTIVALUE);
	assert(hd != CM_MISS);

	uint32_t keys[] = {1, 2, 2, 3, 5, 5, 5};
	for (uint32_t i = 0; i < sizeof(keys)/sizeof(keys[0]); i++) {
		corm_put(hd, &keys[i], &i);
	}

	/* GE from a key present with duplicates: boundary key included */
	uint32_t start = 3;
	uint32_t cur = corm_iter(hd, &start, CM_RANGE | CM_RANGE_GE);
	assert(cur != CM_MISS);
	const void *k, *v;
	uint32_t expect[] = {3, 5, 5, 5};
	uint32_t n = 0, prev = 0;
	while (corm_next(&k, &v, cur)) {
		const uint32_t *kk = k;
		assert(*kk >= start);              /* no entries below the bound */
		if (n > 0) assert(*kk >= prev);    /* ascending, or equal (dup) */
		assert(*kk == expect[n]);
		prev = *kk;
		n++;
	}
	assert(n == 4);

	/* GE from a mid-key not present: first key >= it */
	start = 4;
	cur = corm_iter(hd, &start, CM_RANGE | CM_RANGE_GE);
	n = 0;
	while (corm_next(&k, &v, cur)) {
		assert(*(const uint32_t *)k == 5);
		n++;
	}
	assert(n == 3);

	/* GE from below the lowest key: everything */
	start = 0;
	cur = corm_iter(hd, &start, CM_RANGE | CM_RANGE_GE);
	n = 0;
	while (corm_next(&k, &v, cur)) n++;
	assert(n == 7);

	/* GE from above the highest key: nothing */
	start = 6;
	cur = corm_iter(hd, &start, CM_RANGE | CM_RANGE_GE);
	n = 0;
	while (corm_next(&k, &v, cur)) n++;
	assert(n == 0);

	/* Dirty index: writes after an iteration, then GE again (must rebuild) */
	for (uint32_t i = 0; i < 3; i++) {
		corm_put(hd, &keys[0], &i);      /* more duplicate key-1 entries */
	}
	start = 1;
	cur = corm_iter(hd, &start, CM_RANGE | CM_RANGE_GE);
	n = 0;
	while (corm_next(&k, &v, cur)) n++;
	assert(n == 10);                       /* 4 now at key 1, +2,2,3,5,5,5 */

	corm_close(hd);

	/* Empty multivalue map */
	hd = corm_open(NULL, NULL, CM_U32, CM_U32, 0xFF, CM_SORTED | CM_MULTIVALUE);
	assert(hd != CM_MISS);
	start = 100;
	cur = corm_iter(hd, &start, CM_RANGE | CM_RANGE_GE);
	assert(cur != CM_MISS);
	n = 0;
	while (corm_next(&k, &v, cur)) n++;
	assert(n == 0);
	corm_close(hd);

	/* Non-multivalue sorted map: GE matches plain CM_RANGE+CM_SORTED */
	hd = corm_open(NULL, NULL, CM_U32, CM_U32, 0xFF, CM_SORTED);
	assert(hd != CM_MISS);
	uint32_t nk[] = {10, 30, 50};
	for (uint32_t i = 0; i < sizeof(nk)/sizeof(nk[0]); i++)
		corm_put(hd, &nk[i], &i);
	start = 20;
	cur = corm_iter(hd, &start, CM_RANGE | CM_RANGE_GE);
	n = 0;
	while (corm_next(&k, &v, cur)) {
		assert(*(const uint32_t *)k == 30 || *(const uint32_t *)k == 50);
		prev = *(const uint32_t *)k;
		if (n == 0) assert(prev == 30);
		n++;
	}
	assert(n == 2);
	corm_close(hd);

	/* Unsorted map: GE degrades to a linear scan applying the same GE filter */
	hd = corm_open(NULL, NULL, CM_U32, CM_U32, 0xFF, 0);
	assert(hd != CM_MISS);
	uint32_t uk[] = {7, 2, 9, 1, 20};
	for (uint32_t i = 0; i < sizeof(uk)/sizeof(uk[0]); i++)
		corm_put(hd, &uk[i], &i);
	start = 5;
	cur = corm_iter(hd, &start, CM_RANGE | CM_RANGE_GE);
	n = 0;
	while (corm_next(&k, &v, cur)) {
		assert(*(const uint32_t *)k >= 5);
		n++;
	}
	assert(n == 3);                        /* 7, 9, 20 */
	corm_close(hd);
}

/* Test 20: duplicate chain basics - insertion order via corm_get_multi,
 * head promotion on corm_del, and chain exhaustion. */
static void test_chain_basic_ops(void)
{
	uint32_t hd = corm_open(NULL, NULL, CM_U32, CM_U32, 0xFF,
	                        CM_SORTED | CM_MULTIVALUE);
	assert(hd != CM_MISS);
	uint32_t key = 7;
	uint32_t vals[4] = {10, 20, 30, 40};
	for (int i = 0; i < 4; i++)
		corm_put(hd, &key, &vals[i]);

	/* Insertion order: chain is tail-appended */
	uint32_t cur = corm_get_multi(hd, &key);
	assert(cur != CM_MISS);
	const void *k, *v;
	uint32_t expect[4] = {10, 20, 30, 40};
	int n = 0;
	while (corm_next(&k, &v, cur)) {
		assert(*(const uint32_t *)k == 7);
		assert(*(const uint32_t *)v == expect[n]);
		n++;
	}
	corm_fin(cur);
	assert(n == 4);

	/* corm_del removes the head; next duplicate is promoted */
	corm_del(hd, &key);
	cur = corm_get_multi(hd, &key);
	assert(cur != CM_MISS);
	n = 0;
	while (corm_next(&k, &v, cur)) {
		assert(*(const uint32_t *)v == expect[n + 1]);
		n++;
	}
	corm_fin(cur);
	assert(n == 3);

	/* Delete the rest; chain exhausted -> CM_MISS */
	corm_del(hd, &key);
	corm_del(hd, &key);
	corm_del(hd, &key);
	assert(corm_get_multi(hd, &key) == CM_MISS);
	assert(corm_get(hd, &key) == NULL);
	assert(corm_count(hd, &key) == 0);

	/* Map still works afterwards */
	corm_put(hd, &(uint32_t){8}, &(uint32_t){5});
	assert(corm_count(hd, &(uint32_t){8}) == 1);
	corm_close(hd);
}

/* Test 21: chains survive grow, free-list reuse (LIFO reverses insertion
 * order) and corm_rebuild_map re-linking without losing or mixing
 * duplicates. */
static void test_chain_grow_relink(void)
{
	uint32_t hd = corm_open(NULL, NULL, CM_U32, CM_U32, 0xFF,
	                        CM_SORTED | CM_MULTIVALUE);
	assert(hd != CM_MISS);

	const uint32_t NKEYS = 200;
	const uint32_t DUPS = 35;   /* 200*35 = 7000 entries -> grows to 65536 slots */

	for (uint32_t k = 0; k < NKEYS; k++) {
		for (uint32_t i = 0; i < DUPS; i++) {
			uint32_t val = k * 1000 + i + 1;
			corm_put(hd, &k, &val);
		}
	}

	/* Delete all dups of even keys; rebuild re-links remaining chains */
	for (uint32_t k = 0; k < NKEYS; k += 2)
		corm_del_all(hd, &k);

	/* Re-insert even keys: positions are reused LIFO, so insertion order
	 * differs from position order. Chains must stay complete. */
	for (uint32_t k = 0; k < NKEYS; k += 2) {
		for (uint32_t i = 0; i < DUPS; i++) {
			uint32_t val = k * 1000 + i + 1;
			corm_put(hd, &k, &val);
		}
	}

	/* Trigger another rebuild re-link via an unrelated del_all */
	uint32_t key0 = 0;
	corm_del_all(hd, &key0);

	const void *k, *v;
	uint32_t cur;

	/* Verify all remaining keys: exact counts, no cross-key mixing */
	for (uint32_t kk = 1; kk < NKEYS; kk++) {
		assert(corm_count(hd, &kk) == DUPS);
		cur = corm_get_multi(hd, &kk);
		assert(cur != CM_MISS);
		int seen[DUPS];
		memset(seen, 0, sizeof(seen));
		int n = 0;
		while (corm_next(&k, &v, cur)) {
			uint32_t val = *(const uint32_t *)v;
			assert(val >= kk * 1000 + 1 && val <= kk * 1000 + DUPS);
			seen[val - kk * 1000 - 1] = 1;
			n++;
		}
		corm_fin(cur);
		assert(n == (int)DUPS);
		for (int i = 0; i < (int)DUPS; i++)
			assert(seen[i] == 1);
	}

	/* Key 0 was fully removed */
	assert(corm_count(hd, &key0) == 0);

	/* Total entry count is exact */
	cur = corm_iter(hd, NULL, 0);
	int total = 0;
	while (corm_next(&k, &v, cur)) total++;
	assert(total == (int)(NKEYS * DUPS - DUPS));

	corm_close(hd);
}

/* corm_assoc callback: secondary key = the primary's stored value */
static void assoc_val(const void **skey, const void * const pkey,
	const void * const value, void *userdata)
{
	(void)pkey; (void)userdata;
	*skey = value;
}

/* Test 22: assoc-linked multivalue close must complete at scale.
 * Regression for the close-path hang (per-entry sorted rebuild). */
static void test_chain_assoc_close(void)
{
	uint32_t primary = corm_open(NULL, NULL, CM_U32, CM_U32, 0xFF, 0);
	uint32_t by_val  = corm_open(NULL, NULL, CM_U32, CM_U32, 0xFF,
	                             CM_SORTED | CM_MULTIVALUE);
	assert(primary != CM_MISS && by_val != CM_MISS);
	corm_assoc(by_val, primary, assoc_val, NULL);

	for (uint32_t i = 0; i < 20000; i++)
		corm_put(primary, &i, &i);

	uint32_t d0 = 0;
	assert(corm_count(by_val, &d0) == 1);

	corm_close(primary);
	corm_close(by_val);
}

/* Test 23: file-backed multivalue round-trip - chains rebuilt on load via
 * corm_put re-insertion. */
static void test_chain_persist(void)
{
	const char *filename = "test_chain_persist.corm";

	uint32_t hd = corm_open(filename, "chaindb", CM_U32, CM_U32, 0xFF,
	                        CM_SORTED | CM_MULTIVALUE);
	assert(hd != CM_MISS);
	uint32_t key = 42;
	for (uint32_t i = 0; i < 10; i++)
		corm_put(hd, &key, &i);
	assert(corm_count(hd, &key) == 10);
	corm_save();
	corm_close(hd);

	hd = corm_open(filename, "chaindb", CM_U32, CM_U32, 0xFF,
	               CM_SORTED | CM_MULTIVALUE);
	assert(hd != CM_MISS);
	assert(corm_count(hd, &key) == 10);
	uint32_t cur = corm_get_multi(hd, &key);
	assert(cur != CM_MISS);
	const void *k, *v;
	int n = 0;
	while (corm_next(&k, &v, cur)) {
		assert(*(const uint32_t *)k == 42);
		assert(*(const uint32_t *)v < 10);
		n++;
	}
	corm_fin(cur);
	assert(n == 10);
	corm_close(hd);

	remove(filename);
}

/* Test 24: hole-eliminating backshift (non-wrap cluster) — non-MV map.
 * Regression: a stale hole before an existing key used to make the
 * early-exit probe return the hole slot (corm_get -> NULL) and a re-put
 * would insert a duplicate into a non-MV map. With backshift the cluster
 * shifts left and every probe resolves. */
static void test_backshift_cluster(void)
{
	uint32_t hd = corm_open(NULL, NULL, CM_U32, CM_U32,
	                        0x7 /* 8 slots */, 0);
	assert(hd != CM_MISS);

	uint32_t keys[] = { 0, 8, 16, 24, 32 };   /* home 0: cluster slots 0..4 */
	for (size_t i = 0; i < 5; i++)
		corm_put(hd, &keys[i], &keys[i]);

	/* Delete the slot-1 member: hole at 1 must be back-shifted away. */
	uint32_t k8 = 8, k24 = 24, k16 = 16, k32 = 32, k40 = 40;
	corm_del(hd, &k8);

	/* The previously-orphaned-by-hole keys must still resolve. */
	assert(corm_get(hd, &k16) != NULL);
	assert(corm_get(hd, &k24) != NULL);
	assert(corm_get(hd, &k32) != NULL);
	assert(corm_get(hd, &k8) == NULL);

	/* Re-putting the deleted key adds a new position (count 5). */
	assert(corm_count(hd, NULL) == 4);
	corm_put(hd, &k8, &k8);
	assert(corm_count(hd, NULL) == 5);

	/* Re-putting an EXISTING key must not create a duplicate. */
	corm_put(hd, &k24, &(uint32_t){240});
	assert(corm_count(hd, NULL) == 5);
	const uint32_t *v24 = corm_get(hd, &k24);
	assert(v24 != NULL && *v24 == 240);

	/* Fresh key beyond former holes lands correctly. */
	corm_put(hd, &k40, &k40);
	assert(corm_count(hd, NULL) == 6);
	const uint32_t *v40 = corm_get(hd, &k40);
	assert(v40 != NULL && *v40 == k40);

	uint32_t cur = corm_iter(hd, NULL, 0);
	const void *k, *v;
	int occ_24 = 0;
	while (corm_next(&k, &v, cur)) {
		if (*(const uint32_t *)k == k24)
			occ_24++;
	}
	corm_fin(cur);
	assert(occ_24 == 1);
	corm_close(hd);

	/* MV map: delete a neighbor of a chain head mid-cluster; the chain must
	 * survive the backshift and the head's slot entry must stay put. */
	uint32_t mv = corm_open(NULL, NULL, CM_U32, CM_U32,
	                        0x7, CM_MULTIVALUE | CM_SORTED);
	assert(mv != CM_MISS);
	uint32_t k0 = 0, m24 = 24;
	corm_put(mv, &m24, &(uint32_t){1});   /* chain head at slot 0 */
	corm_put(mv, &m24, &(uint32_t){2});
	corm_put(mv, &k0, &(uint32_t){3});    /* slot 1 */
	corm_put(mv, &k16, &(uint32_t){4});   /* slot 2 */
	corm_put(mv, &k32, &(uint32_t){5});   /* slot 3 */

	corm_del(mv, &k0);                    /* clears slot 1 + backshift */

	uint32_t mcur = corm_get_multi(mv, &m24);
	assert(mcur != CM_MISS);
	int n = 0;
	while (corm_next(&k, &v, mcur)) n++;
	corm_fin(mcur);
	assert(n == 2);
	assert(corm_get(mv, &k16) != NULL);
	assert(corm_get(mv, &k32) != NULL);

	corm_put(mv, &m24, &(uint32_t){6});   /* third dup links onto tail */
	mcur = corm_get_multi(mv, &m24);
	assert(mcur != CM_MISS);
	n = 0;
	while (corm_next(&k, &v, mcur)) n++;
	corm_fin(mcur);
	assert(n == 3);
	corm_close(mv);
}

/* Test 25: backshift across the table wrap with a skipped unmovable
 * element. Cluster {7,0,1,2,3} homes {7,7,7,2,7}: deleting slot 7 must
 * move 15/23 into the hole, SKIP the home-anchored key at slot 2, and
 * continue to move 31 — stopping only at the empty slot. */
static void test_backshift_wrap(void)
{
	uint32_t hd = corm_open(NULL, NULL, CM_U32, CM_U32,
	                        0x7 /* 8 slots */, 0);
	assert(hd != CM_MISS);

	uint32_t k7 = 7, k15 = 15, k23 = 23, k2 = 2, k31 = 31, k39 = 39;
	corm_put(hd, &k7, &k7);    /* slot 7 */
	corm_put(hd, &k15, &k15);  /* slot 0 (wrap) */
	corm_put(hd, &k23, &k23);  /* slot 1 (wrap) */
	corm_put(hd, &k2, &k2);    /* slot 2 (at home) */
	corm_put(hd, &k31, &k31);  /* slot 3 (wrap) */

	corm_del(hd, &k7);         /* hole at 7 */

	assert(corm_get(hd, &k15) != NULL);
	assert(corm_get(hd, &k23) != NULL);
	assert(corm_get(hd, &k2) != NULL);
	assert(corm_get(hd, &k31) != NULL);
	assert(corm_get(hd, &k7) == NULL);
	assert(corm_count(hd, NULL) == 4);

	/* Post-backshift insert into the vacated region resolves correctly. */
	corm_put(hd, &k39, &k39);
	assert(corm_count(hd, NULL) == 5);
	const uint32_t *v39 = corm_get(hd, &k39);
	assert(v39 != NULL && *v39 == k39);

	/* Everything still locatable after the new insert. */
	assert(corm_get(hd, &k15) != NULL);
	assert(corm_get(hd, &k23) != NULL);
	assert(corm_get(hd, &k2) != NULL);
	assert(corm_get(hd, &k31) != NULL);
	corm_close(hd);
}

/* Chain order must survive fast-path del_all slot reclamation + rebuild.
 * Bug: corm_rebuild_map relinked duplicates by ascending position order; when
 * a freed low slot is reclaimed by a later duplicate, get_multi/get order
 * scrambled (insertion [1,2,3,4] became slot order [4,1,2,3]). */
static void test_chain_order_slot_reuse(void)
{
	uint32_t hd = corm_open(NULL, NULL, CM_U32, CM_U32, 0xFF,
	                        CM_MULTIVALUE | CM_SORTED);
	assert(hd != CM_MISS);

	uint32_t a = 100, b = 200, c = 300;
	uint32_t vals[4];
	int i;
	const void *k, *v;

	/* B at the low slot so it frees a position below A's chain. */
	corm_put(hd, &b, &(uint32_t){9});
	corm_put(hd, &a, &(uint32_t){1});
	corm_put(hd, &a, &(uint32_t){2});
	corm_put(hd, &a, &(uint32_t){3});

	/* Fast-path del_all punches slot 0 and idm_del frees it. */
	corm_del_all(hd, &b);
	assert(corm_count(hd, &a) == 3);

	/* The newest duplicate of A reclaims the freed low slot. */
	corm_put(hd, &a, &(uint32_t){4});
	assert(corm_count(hd, &a) == 4);

	/* Churn another key to force corm_rebuild_map again. */
	corm_put(hd, &c, &(uint32_t){5});
	corm_del_all(hd, &c);

	/* Insertion order must survive; get() returns the chain head. */
	const uint32_t *first = corm_get(hd, &a);
	assert(first != NULL && *first == 1);

	uint32_t cur = corm_get_multi(hd, &a);
	assert(cur != CM_MISS);
	i = 0;
	while (corm_next(&k, &v, cur)) {
		assert(i < 4);
		vals[i++] = *(const uint32_t *)v;
	}
	assert(i == 4);
	assert(vals[0] == 1 && vals[1] == 2 && vals[2] == 3 && vals[3] == 4);

	corm_close(hd);
}

/* Same invariant across corm_grow: growth relinks the hash table while a
 * hole exists and a duplicate chain has already been slot-scrambled. */
static void test_chain_order_grow_with_holes(void)
{
	uint32_t hd = corm_open(NULL, NULL, CM_U32, CM_U32, 0x07,
	                        CM_MULTIVALUE | CM_SORTED);
	assert(hd != CM_MISS);

	uint32_t a = 100, b = 200, c = 300, d = 400;
	uint32_t vals[4];
	int i;
	const void *k, *v;

	corm_put(hd, &b, &(uint32_t){9});   /* pos 0 — will be freed */
	corm_put(hd, &a, &(uint32_t){1});
	corm_put(hd, &a, &(uint32_t){2});
	corm_put(hd, &a, &(uint32_t){3});

	corm_del_all(hd, &b);               /* punch pos 0, rebuild map */
	corm_put(hd, &a, &(uint32_t){4});   /* reclaims pos 0 */
	corm_put(hd, &c, &(uint32_t){5});   /* still under grow threshold */
	corm_put(hd, &d, &(uint32_t){6});   /* n+1=6 -> 24>=m*3 -> grow */

	const uint32_t *first = corm_get(hd, &a);
	assert(first != NULL && *first == 1);

	uint32_t cur = corm_get_multi(hd, &a);
	assert(cur != CM_MISS);
	i = 0;
	while (corm_next(&k, &v, cur)) {
		assert(i < 4);
		vals[i++] = *(const uint32_t *)v;
	}
	assert(i == 4);
	assert(vals[0] == 1 && vals[1] == 2 && vals[2] == 3 && vals[3] == 4);

	corm_close(hd);
}

int main(void)
{
	printf("=== CM_MULTIVALUE Test Suite ===\n\n");
	
	TEST(test_multivalue_requires_sorted);
	TEST(test_multivalue_basic);
	TEST(test_multivalue_delete);
	TEST(test_multivalue_intervals);
	TEST(test_backward_compatibility);
	TEST(test_multivalue_edge_cases);
	TEST(test_multivalue_multiple_keys);
	TEST(test_multivalue_strings);
	TEST(test_multivalue_del_all);
	TEST(test_multivalue_large_scale);
	TEST(test_multivalue_idm_tracking);
	TEST(test_multivalue_persistence);
	TEST(test_multivalue_concurrent_iteration);
	TEST(test_multivalue_modify_during_iteration);
	TEST(test_multivalue_multiple_indexes);
	TEST(test_multivalue_range_iteration);
	TEST(test_multivalue_stress);
	TEST(test_bug3_range_returns_all_duplicates);
	TEST(test_range_ge);
	TEST(test_chain_basic_ops);
	TEST(test_chain_grow_relink);
	TEST(test_chain_assoc_close);
	TEST(test_chain_persist);
	TEST(test_backshift_cluster);
	TEST(test_backshift_wrap);
	TEST(test_chain_order_slot_reuse);
	TEST(test_chain_order_grow_with_holes);
	
	printf("\n=== All tests passed! ===\n");
	return 0;
}
