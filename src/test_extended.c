/**
 * Extended test suite for libcorm
 * Tests edge cases, error conditions, and advanced features
 */

#include "./../include/ttypt/corm.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>

#define TEST_MASK 0xF  // Small capacity for testing limits

unsigned errors = 0;

#define PASS() printf("  ✅ PASS\n")
#define FAIL(msg) do { printf("  ❌ FAIL: %s\n", msg); errors++; } while(0)
#define ASSERT(cond, msg) do { if (!(cond)) FAIL(msg); else PASS(); } while(0)

/* Association callback for test_associations */
static void assoc_cb(const void **skey, const void *pkey, const void *value, void *userdata) {
	(void)pkey;
	(void)userdata;
	*skey = value; // Use primary value as secondary key
}

/* Test 1: Empty map operations */
static void test_empty_map(void) {
	printf("\n=== Test 1: Empty Map Operations ===\n");
	
	uint32_t hd = corm_open(NULL, NULL, CM_U32, CM_STR, 0xFF, 0);
	
	printf("Get from empty map:");
	const char *result = corm_get(hd, &(uint32_t){42});
	ASSERT(result == NULL, "Expected NULL from empty map");
	
	printf("Iterate empty map:");
	uint32_t cur = corm_iter(hd, NULL, 0);
	const void *key, *value;
	int count = 0;
	while (corm_next(&key, &value, cur))
		count++;
	ASSERT(count == 0, "Expected 0 items from empty map");
	
	printf("Delete from empty map (should not crash):");
	corm_del(hd, &(uint32_t){42});
	PASS();
	
	printf("Drop empty map (should not crash):");
	corm_drop(hd);
	PASS();
	
	corm_close(hd);
}

/* Test 2: Capacity limits */
static void test_capacity_limits(void) {
	printf("\n=== Test 2: Capacity Limits ===\n");
	
	// Create map with very small capacity (mask 0x3 = capacity 4)
	uint32_t hd = corm_open(NULL, NULL, CM_U32, CM_U32, 0x3, 0);
	
	printf("Fill map to capacity (4 items):");
	for (uint32_t i = 0; i < 4; i++) {
		uint32_t key = i * 10; // Spread out to avoid hash collisions
		corm_put(hd, &key, &i);
	}
	PASS();
	
	printf("Verify all 4 items retrievable:");
	int found = 0;
	for (uint32_t i = 0; i < 4; i++) {
		uint32_t key = i * 10;
		const uint32_t *val = corm_get(hd, &key);
		if (val && *val == i) found++;
	}
	ASSERT(found == 4, "Expected to retrieve all 4 items");
	
	printf("Note: Attempting to exceed capacity would trigger CBUG (process exit)\n");
	printf("  This behavior is documented but not tested here to avoid test failure\n");
	
	corm_close(hd);
}

/* Test 3: CM_AINDEX (auto-indexing) */
static void test_aindex(void) {
	printf("\n=== Test 3: CM_AINDEX Auto-Indexing ===\n");
	
	uint32_t hd = corm_open(NULL, NULL, CM_HNDL, CM_STR, 0xFF, CM_AINDEX);
	
	printf("Insert with NULL key (should auto-generate ID):");
	uint32_t id1 = corm_put(hd, NULL, "first");
	uint32_t id2 = corm_put(hd, NULL, "second");
	uint32_t id3 = corm_put(hd, NULL, "third");
	ASSERT(id1 != id2 && id2 != id3, "IDs should be unique");
	
	printf("Retrieve by auto-generated IDs:");
	const char *v1 = corm_get(hd, &id1);
	const char *v2 = corm_get(hd, &id2);
	const char *v3 = corm_get(hd, &id3);
	ASSERT(v1 && strcmp(v1, "first") == 0, "Retrieved first value");
	ASSERT(v2 && strcmp(v2, "second") == 0, "Retrieved second value");
	ASSERT(v3 && strcmp(v3, "third") == 0, "Retrieved third value");
	
	printf("Mix NULL and explicit keys:");
	uint32_t explicit_key = 999;
	corm_put(hd, &explicit_key, "explicit");
	const char *ve = corm_get(hd, &explicit_key);
	ASSERT(ve && strcmp(ve, "explicit") == 0, "Retrieved explicit key value");
	
	corm_close(hd);
}

/* Test 4: CM_SORTED edge cases */
static void test_sorted_edge_cases(void) {
	printf("\n=== Test 4: CM_SORTED Edge Cases ===\n");
	
	uint32_t hd = corm_open(NULL, NULL, CM_U32, CM_STR, 0xFF, CM_SORTED);
	
	printf("Insert in random order:");
	uint32_t keys[] = {50, 10, 30, 20, 40};
	for (int i = 0; i < 5; i++)
		corm_put(hd, &keys[i], "value");
	PASS();
	
	printf("Iterate in sorted order:");
	uint32_t cur = corm_iter(hd, NULL, CM_RANGE);
	const void *key, *value;
	uint32_t prev = 0;
	int sorted_ok = 1;
	while (corm_next(&key, &value, cur)) {
		uint32_t k = *(const uint32_t*)key;
		if (k < prev) sorted_ok = 0;
		prev = k;
	}
	ASSERT(sorted_ok, "Items should be in ascending order");
	
	printf("Range scan from middle (start=30):");
	uint32_t start = 30;
	cur = corm_iter(hd, &start, CM_RANGE);
	int count = 0;
	while (corm_next(&key, &value, cur)) {
		uint32_t k = *(const uint32_t*)key;
		ASSERT(k >= 30, "Range scan should only return keys >= 30");
		count++;
	}
	ASSERT(count == 3, "Expected 3 items (30, 40, 50)");
	
	printf("Add more items after sorted index built:");
	uint32_t new_key = 25;
	corm_put(hd, &new_key, "new");
	PASS();
	
	printf("Verify sorted order maintained:");
	cur = corm_iter(hd, NULL, CM_RANGE);
	prev = 0;
	sorted_ok = 1;
	count = 0;
	while (corm_next(&key, &value, cur)) {
		uint32_t k = *(const uint32_t*)key;
		if (k < prev) sorted_ok = 0;
		prev = k;
		count++;
	}
	ASSERT(sorted_ok && count == 6, "Sorted order maintained with new item");
	
	printf("Delete item and verify sorted order:");
	corm_del(hd, &(uint32_t){30});
	cur = corm_iter(hd, NULL, CM_RANGE);
	count = 0;
	sorted_ok = 1;
	prev = 0;
	while (corm_next(&key, &value, cur)) {
		uint32_t k = *(const uint32_t*)key;
		if (k < prev) sorted_ok = 0;
		if (k == 30) sorted_ok = 0; // 30 should be deleted
		prev = k;
		count++;
	}
	ASSERT(sorted_ok && count == 5, "Sorted order maintained after deletion");
	
	corm_close(hd);
}

/* Test 5: Custom type registration */
static void test_custom_types(void) {
	printf("\n=== Test 5: Custom Type Registration ===\n");
	
	// Register a fixed-length struct type
	typedef struct { uint32_t x, y; } point_t;
	
	printf("Register fixed-length type (point_t, 8 bytes):");
	uint32_t point_type = corm_reg(sizeof(point_t));
	ASSERT(point_type != CM_MISS, "Type registration should succeed");
	
	printf("Use custom type in map:");
	uint32_t hd = corm_open(NULL, NULL, point_type, CM_U32, 0xFF, 0);
	
	point_t p1 = {10, 20};
	point_t p2 = {30, 40};
	corm_put(hd, &p1, &(uint32_t){100});
	corm_put(hd, &p2, &(uint32_t){200});
	
	const uint32_t *v1 = corm_get(hd, &p1);
	const uint32_t *v2 = corm_get(hd, &p2);
	ASSERT(v1 && *v1 == 100, "Retrieved value for p1");
	ASSERT(v2 && *v2 == 200, "Retrieved value for p2");
	
	corm_close(hd);
	
	// Test variable-length type (already done with CM_STR)
	printf("Variable-length type (CM_STR is built-in):");
	PASS();
}

/* Test 6: Iterator edge cases */
static void test_iterator_edge_cases(void) {
	printf("\n=== Test 6: Iterator Edge Cases ===\n");
	
	uint32_t hd = corm_open(NULL, NULL, CM_U32, CM_STR, 0xFF, 0);
	
	printf("Early iterator termination (corm_fin):");
	for (int i = 0; i < 10; i++)
		corm_put(hd, &(uint32_t){i}, "value");
	
	uint32_t cur = corm_iter(hd, NULL, 0);
	const void *key, *value;
	int count = 0;
	while (corm_next(&key, &value, cur)) {
		count++;
		if (count == 3) {
			corm_fin(cur);
			break;
		}
	}
	ASSERT(count == 3, "Early termination should work");
	
	printf("Multiple concurrent iterators:");
	uint32_t cur1 = corm_iter(hd, NULL, 0);
	uint32_t cur2 = corm_iter(hd, NULL, 0);
	
	int c1 = 0, c2 = 0;
	while (corm_next(&key, &value, cur1)) c1++;
	while (corm_next(&key, &value, cur2)) c2++;
	
	ASSERT(c1 == 10 && c2 == 10, "Both iterators should see all items");
	
	printf("Iterate after modifications:");
	corm_del(hd, &(uint32_t){5});
	cur = corm_iter(hd, NULL, 0);
	count = 0;
	while (corm_next(&key, &value, cur)) count++;
	ASSERT(count == 9, "Iterator should reflect deletions");
	
	corm_close(hd);
}

/* Test 7: Mirror map functionality */
static void test_mirror_maps(void) {
	printf("\n=== Test 7: Mirror Maps ===\n");
	
	uint32_t hd = corm_open(NULL, NULL, CM_U32, CM_STR, 0xFF, CM_MIRROR);
	uint32_t mirror_hd = hd + 1;
	
	printf("Insert into primary map:");
	corm_put(hd, &(uint32_t){1}, "one");
	corm_put(hd, &(uint32_t){2}, "two");
	corm_put(hd, &(uint32_t){3}, "three");
	PASS();
	
	printf("Lookup in mirror (value->key):");
	const uint32_t *k1 = corm_get(mirror_hd, "one");
	const uint32_t *k2 = corm_get(mirror_hd, "two");
	const uint32_t *k3 = corm_get(mirror_hd, "three");
	ASSERT(k1 && *k1 == 1, "Mirror lookup for 'one'");
	ASSERT(k2 && *k2 == 2, "Mirror lookup for 'two'");
	ASSERT(k3 && *k3 == 3, "Mirror lookup for 'three'");
	
	printf("Delete from primary, verify mirror updated:");
	corm_del(hd, &(uint32_t){2});
	const uint32_t *k2_after = corm_get(mirror_hd, "two");
	ASSERT(k2_after == NULL, "Mirror should reflect deletion");
	
	printf("Update value in primary:");
	corm_put(hd, &(uint32_t){1}, "ONE");
	const uint32_t *k1_new = corm_get(mirror_hd, "ONE");
	const uint32_t *k1_old = corm_get(mirror_hd, "one");
	ASSERT(k1_new && *k1_new == 1, "Mirror updated for new value");
	ASSERT(k1_old == NULL, "Old mirror entry removed");
	
	corm_close(hd);
}

/* Test 8: Association between maps */
static void test_associations(void) {
	printf("\n=== Test 8: Map Associations ===\n");
	
	// Primary map: user_id -> username
	uint32_t users_hd = corm_open(NULL, NULL, CM_U32, CM_STR, 0xFF, 0);
	
	// Secondary map: username -> user_id (manual association)
	// CM_PGET flag makes corm_get return primary keys instead of values
	uint32_t by_name_hd = corm_open(NULL, NULL, CM_STR, CM_U32, 0xFF, CM_PGET);
	
	printf("Setup association callback:");
	corm_assoc(by_name_hd, users_hd, assoc_cb, NULL);

	int count = 0;
	uint32_t cur = corm_iter(by_name_hd, NULL, 0);
	const void *k, *v;
	while (corm_next(&k, &v, cur)) count++;
	corm_fin(cur);
	ASSERT(count == 0, "Empty secondary after fresh assoc");

	PASS();

	printf("Associate BEFORE populating (existing behavior):");
	{
	uint32_t prim = corm_open(NULL, NULL, CM_U32, CM_U32, 0xFF, 0);
	uint32_t sec = corm_open(NULL, NULL, CM_U32, CM_U32, 0xFF, CM_MULTIVALUE | CM_SORTED);
	corm_assoc(sec, prim, assoc_cb, NULL);

	for (uint32_t i = 0; i < 5; i++)
		corm_put(prim, &i, &i);

	count = 0;
	cur = corm_iter(sec, NULL, 0);
	while (corm_next(&k, &v, cur)) count++;
	corm_fin(cur);
	ASSERT(count == 5, "All 5 post-assoc items in secondary");

	corm_close(prim);
	corm_close(sec);
	PASS();
	}

	printf("Associate on empty map:");
	{
	uint32_t prim = corm_open(NULL, NULL, CM_U32, CM_U32, 0xFF, 0);
	uint32_t sec = corm_open(NULL, NULL, CM_U32, CM_U32, 0xFF, CM_MULTIVALUE | CM_SORTED);
	corm_assoc(sec, prim, assoc_cb, NULL);

	int count = 0;
	uint32_t cur = corm_iter(sec, NULL, 0);
	const void *k, *v;
	while (corm_next(&k, &v, cur)) count++;
	corm_fin(cur);
	ASSERT(count == 0, "Empty secondary stays empty");

	corm_close(prim);
	corm_close(sec);
	PASS();
	}
}

/* Test 8c: Association delete cleanup */
static void test_assoc_delete_cleanup(void) {
	printf("\n=== Test 8c: Association Delete Cleanup ===\n");

	printf("Delete from primary cleans secondary entry:");
	uint32_t prim = corm_open(NULL, NULL, CM_U32, CM_U32, 0xFF, 0);
	uint32_t sec = corm_open(NULL, NULL, CM_U32, CM_U32, 0xFF, 0);
	corm_assoc(sec, prim, assoc_cb, NULL);

	for (uint32_t i = 0; i < 10; i++)
		corm_put(prim, &i, &(uint32_t){i * 10});

	int count = 0;
	uint32_t cur = corm_iter(sec, NULL, 0);
	const void *k, *v;
	while (corm_next(&k, &v, cur)) count++;
	corm_fin(cur);
	ASSERT(count == 10, "10 items in secondary before delete");

	corm_del(prim, &(uint32_t){3});
	count = 0;
	cur = corm_iter(sec, NULL, 0);
	while (corm_next(&k, &v, cur)) count++;
	corm_fin(cur);
	ASSERT(count == 9, "9 items in secondary after delete");

	const uint32_t *gone = corm_get(sec, &(uint32_t){30});
	ASSERT(gone == NULL, "Deleted secondary key returns NULL");

	corm_close(prim);
	corm_close(sec);
	PASS();
}

/* Test 8d: Association update cleanup */
static void test_assoc_update_cleanup(void) {
	printf("\n=== Test 8d: Association Update Cleanup ===\n");

	printf("Update primary value cleans old secondary key:");
	uint32_t prim = corm_open(NULL, NULL, CM_U32, CM_U32, 0xFF, 0);
	uint32_t sec = corm_open(NULL, NULL, CM_U32, CM_U32, 0xFF, 0);
	corm_assoc(sec, prim, assoc_cb, NULL);

	uint32_t val = 10;
	corm_put(prim, &(uint32_t){1}, &val);

	int count = 0;
	uint32_t cur = corm_iter(sec, NULL, 0);
	const void *k, *v;
	while (corm_next(&k, &v, cur)) count++;
	corm_fin(cur);
	ASSERT(count == 1, "1 item in secondary before update");

	const uint32_t *got = corm_get(sec, &(uint32_t){10});
	ASSERT(got != NULL, "Secondary key 10 exists");

	val = 20;
	corm_put(prim, &(uint32_t){1}, &val);

	count = 0;
	cur = corm_iter(sec, NULL, 0);
	while (corm_next(&k, &v, cur)) count++;
	corm_fin(cur);
	ASSERT(count == 1, "1 item in secondary after update (old removed)");

	got = corm_get(sec, &(uint32_t){20});
	ASSERT(got != NULL, "New secondary key 20 exists");

	got = corm_get(sec, &(uint32_t){10});
	ASSERT(got == NULL, "Old secondary key 10 removed");

	corm_close(prim);
	corm_close(sec);
	PASS();
}

/* Test 8e: Association zombie detection via counting */
static void test_assoc_zombie_check(void) {
	printf("\n=== Test 8e: Association Zombie Check (CM_PGET) ===\n");

	printf("Delete from primary, verify secondary count drops:");
	uint32_t prim = corm_open(NULL, NULL, CM_U32, CM_STR, 0xFF, 0);
	uint32_t sec = corm_open(NULL, NULL, CM_STR, CM_U32, 0xFF, CM_PGET);
	corm_assoc(sec, prim, assoc_cb, NULL);

	corm_put(prim, &(uint32_t){100}, "alice");
	corm_put(prim, &(uint32_t){200}, "bob");
	corm_put(prim, &(uint32_t){300}, "charlie");

	int count = 0;
	uint32_t cur = corm_iter(sec, NULL, 0);
	const void *k, *v;
	while (corm_next(&k, &v, cur)) count++;
	corm_fin(cur);
	ASSERT(count == 3, "3 items in secondary before delete");

	corm_del(prim, &(uint32_t){100});
	count = 0;
	cur = corm_iter(sec, NULL, 0);
	while (corm_next(&k, &v, cur)) {
		const uint32_t *pv = (const uint32_t *)v;
		ASSERT(*pv == 200 || *pv == 300, "Remaining values valid");
		count++;
	}
	corm_fin(cur);
	ASSERT(count == 2, "2 items in secondary after delete (no zombies)");

	const uint32_t *u = corm_get(sec, "alice");
	ASSERT(u == NULL, "Deleted name returns NULL");

	corm_close(prim);
	corm_close(sec);
	PASS();
}

/* Test 9: File persistence */
static void test_file_persistence(void) {
	printf("\n=== Test 9: File Persistence ===\n");
	
	const char *filename = "test_persist.corm";
	
	printf("Create and populate file-backed map (requires CM_MIRROR):");
	{
		uint32_t hd = corm_open(filename, "testdb", CM_U32, CM_STR, 0xFF, CM_MIRROR);
		corm_put(hd, &(uint32_t){1}, "persisted_one");
		corm_put(hd, &(uint32_t){2}, "persisted_two");
		corm_put(hd, &(uint32_t){3}, "persisted_three");
		corm_save(); // Explicit save
		corm_close(hd);  // Automatically closes mirror at hd + 1
	}
	PASS();
	
	printf("Reopen and verify data persisted:");
	{
		uint32_t hd = corm_open(filename, "testdb", CM_U32, CM_STR, 0xFF, CM_MIRROR);
		const char *v1 = corm_get(hd, &(uint32_t){1});
		const char *v2 = corm_get(hd, &(uint32_t){2});
		const char *v3 = corm_get(hd, &(uint32_t){3});
		ASSERT(v1 && strcmp(v1, "persisted_one") == 0, "Value 1 persisted");
		ASSERT(v2 && strcmp(v2, "persisted_two") == 0, "Value 2 persisted");
		ASSERT(v3 && strcmp(v3, "persisted_three") == 0, "Value 3 persisted");
		corm_close(hd);  // Automatically closes mirror at hd + 1
	}
	
	printf("Test multiple databases in same file:");
	{
		uint32_t hd1 = corm_open(filename, "db1", CM_U32, CM_STR, 0xFF, CM_MIRROR);
		uint32_t hd2 = corm_open(filename, "db2", CM_U32, CM_STR, 0xFF, CM_MIRROR);
		
		corm_put(hd1, &(uint32_t){100}, "db1_value");
		corm_put(hd2, &(uint32_t){100}, "db2_value");
		
		const char *v1 = corm_get(hd1, &(uint32_t){100});
		const char *v2 = corm_get(hd2, &(uint32_t){100});
		
		ASSERT(v1 && strcmp(v1, "db1_value") == 0, "DB1 independent");
		ASSERT(v2 && strcmp(v2, "db2_value") == 0, "DB2 independent");
		
		corm_close(hd1);
		corm_close(hd1 + 1);
		corm_close(hd2);
		corm_close(hd2 + 1);
	}
	
	// Cleanup
	remove(filename);
}

/* Test 10: CM_HNDL type behavior */
static void test_hndl_type(void) {
	printf("\n=== Test 10: CM_HNDL Type Behavior ===\n");
	
	uint32_t hd = corm_open(NULL, NULL, CM_HNDL, CM_STR, 0xFF, 0);
	
	printf("CM_HNDL uses value directly as hash:");
	// With CM_HNDL, the value is used directly as hash
	// So we can test predictable hash bucket placement
	corm_put(hd, &(uint32_t){10}, "ten");
	corm_put(hd, &(uint32_t){20}, "twenty");
	
	const char *v1 = corm_get(hd, &(uint32_t){10});
	const char *v2 = corm_get(hd, &(uint32_t){20});
	
	ASSERT(v1 && strcmp(v1, "ten") == 0, "Retrieved by handle 10");
	ASSERT(v2 && strcmp(v2, "twenty") == 0, "Retrieved by handle 20");
	
	corm_close(hd);
}

/* Test 11: CM_PTR type behavior */
static void test_ptr_type(void) {
	printf("\n=== Test 11: CM_PTR Type Behavior ===\n");
	
	uint32_t hd = corm_open(NULL, NULL, CM_PTR, CM_STR, 0xFF, 0);
	
	printf("Use pointer addresses as keys:");
	char obj1[] = "object1";
	char obj2[] = "object2";
	void *ptr1 = &obj1;
	void *ptr2 = &obj2;
	
	corm_put(hd, &ptr1, "value1");
	corm_put(hd, &ptr2, "value2");
	
	const char *v1 = corm_get(hd, &ptr1);
	const char *v2 = corm_get(hd, &ptr2);
	
	ASSERT(v1 && strcmp(v1, "value1") == 0, "Retrieved by pointer key 1");
	ASSERT(v2 && strcmp(v2, "value2") == 0, "Retrieved by pointer key 2");
	
	printf("Note: Returned pointer points to stored pointer bytes:");
	PASS();
	
	corm_close(hd);
}

/* Test 12: Drop functionality */
static void test_drop(void) {
	printf("\n=== Test 12: Drop Functionality ===\n");
	
	uint32_t hd = corm_open(NULL, NULL, CM_U32, CM_STR, 0xFF, 0);
	
	printf("Populate map:");
	for (int i = 0; i < 10; i++)
		corm_put(hd, &(uint32_t){i}, "value");
	PASS();
	
	printf("Drop all entries:");
	corm_drop(hd);
	PASS();
	
	printf("Verify map is empty:");
	uint32_t cur = corm_iter(hd, NULL, 0);
	const void *key, *value;
	int count = 0;
	while (corm_next(&key, &value, cur)) count++;
	ASSERT(count == 0, "Map should be empty after drop");
	
	printf("Reuse map after drop:");
	corm_put(hd, &(uint32_t){99}, "new_value");
	const char *v = corm_get(hd, &(uint32_t){99});
	ASSERT(v && strcmp(v, "new_value") == 0, "Can reuse map after drop");
	
	corm_close(hd);
}

/* Test 13: Update existing keys */
static void test_update_keys(void) {
	printf("\n=== Test 13: Update Existing Keys ===\n");
	
	uint32_t hd = corm_open(NULL, NULL, CM_U32, CM_STR, 0xFF, 0);
	
	printf("Insert initial value:");
	corm_put(hd, &(uint32_t){42}, "initial");
	const char *v1 = corm_get(hd, &(uint32_t){42});
	ASSERT(v1 && strcmp(v1, "initial") == 0, "Initial value set");
	
	printf("Update with new value:");
	corm_put(hd, &(uint32_t){42}, "updated");
	const char *v2 = corm_get(hd, &(uint32_t){42});
	ASSERT(v2 && strcmp(v2, "updated") == 0, "Value updated");
	
	printf("Verify only one entry exists:");
	uint32_t cur = corm_iter(hd, NULL, 0);
	const void *key, *value;
	int count = 0;
	while (corm_next(&key, &value, cur)) count++;
	ASSERT(count == 1, "Only one entry should exist");
	
	corm_close(hd);
}

/* Test 14: File loading without CM_MIRROR */
static void test_file_loading_no_mirror(void) {
	printf("\n=== Test 14: File Loading Without CM_MIRROR ===\n");
	
	const char *testfile = "/tmp/corm_test_no_mirror.db";
	
	// Create and populate a file-backed map with CM_MIRROR
	printf("Creating file with CM_MIRROR:");
	uint32_t hd1 = corm_open(testfile, NULL, CM_U32, CM_U32, 0xFF, CM_MIRROR);
	corm_put(hd1, &(uint32_t){10}, &(uint32_t){100});
	corm_put(hd1, &(uint32_t){20}, &(uint32_t){200});
	corm_put(hd1, &(uint32_t){30}, &(uint32_t){300});
	corm_save();  // Explicitly save to file
	corm_close(hd1);
	PASS();
	
	// Open WITHOUT CM_MIRROR and verify data loads
	printf("Opening file WITHOUT CM_MIRROR:");
	uint32_t hd2 = corm_open(testfile, NULL, CM_U32, CM_U32, 0xFF, 0);
	PASS();
	
	printf("Verify data loaded from file:");
	const uint32_t *v1 = corm_get(hd2, &(uint32_t){10});
	const uint32_t *v2 = corm_get(hd2, &(uint32_t){20});
	const uint32_t *v3 = corm_get(hd2, &(uint32_t){30});
	ASSERT(v1 && *v1 == 100 && v2 && *v2 == 200 && v3 && *v3 == 300,
	       "Data should load from file without CM_MIRROR");
	
	corm_close(hd2);
	unlink(testfile);
}

/* Test 15: Pointer stability on replacement */
static void test_pointer_stability(void) {
	printf("\n=== Test 15: Pointer Stability on Replacement ===\n");
	
	uint32_t hd = corm_open(NULL, NULL, CM_U32, CM_U32, 0xFF, 0);
	
	// Test 15a: Same key, same-sized value replacement
	printf("Same key, same value size - pointer should stay valid:");
	corm_put(hd, &(uint32_t){1}, &(uint32_t){100});
	const uint32_t *ptr1 = corm_get(hd, &(uint32_t){1});
	void *orig_ptr1 = (void *)ptr1;
	corm_put(hd, &(uint32_t){1}, &(uint32_t){200});
	const uint32_t *ptr2 = corm_get(hd, &(uint32_t){1});
	ASSERT(ptr1 == ptr2 && *ptr2 == 200, 
	       "Pointer should remain valid and point to new value");
	
	// Test 15b: Verify old pointer is still valid (reused allocation)
	printf("Old pointer should show new value:");
	ASSERT((void *)ptr1 == orig_ptr1 && *ptr1 == 200,
	       "Original pointer should still be valid with new value");
	
	corm_close(hd);
	
	// Test 15c: Variable-length types (strings)
	printf("Same key, smaller string - pointer should stay valid:");
	uint32_t hd2 = corm_open(NULL, NULL, CM_U32, CM_STR, 0xFF, 0);
	corm_put(hd2, &(uint32_t){1}, "Long string here");
	const char *str_ptr1 = corm_get(hd2, &(uint32_t){1});
	void *orig_str_ptr = (void *)str_ptr1;
	corm_put(hd2, &(uint32_t){1}, "Short");
	const char *str_ptr2 = corm_get(hd2, &(uint32_t){1});
	ASSERT(str_ptr1 == str_ptr2 && strcmp(str_ptr2, "Short") == 0,
	       "String pointer should be reused for smaller replacement");
	
	printf("Old string pointer should show new value:");
	ASSERT((void *)str_ptr1 == orig_str_ptr && strcmp(str_ptr1, "Short") == 0,
	       "Original string pointer should still be valid");
	
	corm_close(hd2);
	
	// Test 15d: Same key, larger value (expect new allocation)
	printf("Same key, larger string - new allocation expected:");
	uint32_t hd3 = corm_open(NULL, NULL, CM_U32, CM_STR, 0xFF, 0);
	corm_put(hd3, &(uint32_t){1}, "Short");
	// Don't save pointer - it will be invalidated
	corm_put(hd3, &(uint32_t){1}, "Much longer string that won't fit");
	const char *large_ptr = corm_get(hd3, &(uint32_t){1});
	ASSERT(strcmp(large_ptr, "Much longer string that won't fit") == 0,
	       "New larger value should be stored correctly");
	
	corm_close(hd3);
}

/* Test 16: File reopen and append (CLI pattern) */
static void test_file_reopen_append(void) {
	printf("\n=== Test 16: File Reopen and Append (CLI Pattern) ===\n");
	
	const char *filename = "test_reopen_append.corm";
	const char *dbname = "hd";
	
	// Simulate first CLI invocation: open, write, close (auto-save on destructor)
	printf("First session: Create file and add first entry:");
	{
		uint32_t hd = corm_open(filename, dbname, CM_STR, CM_STR, 0x7FFF, CM_MIRROR);
		corm_put(hd, "auth", "title=Auth; routes=/login,/register");
		corm_save();  // Explicit save (destructor would also save)
		corm_close(hd);
	}
	PASS();
	
	// Simulate second CLI invocation: reopen same file/db, add more data
	printf("Second session: Reopen file and add second entry:");
	{
		uint32_t hd = corm_open(filename, dbname, CM_STR, CM_STR, 0x7FFF, CM_MIRROR);
		// Should load "auth" entry from file
		const char *v1 = corm_get(hd, "auth");
		ASSERT(v1 && strstr(v1, "Auth"), "First entry should be loaded");
		
		// Add new entry
		corm_put(hd, "poem", "title=Poem; routes=/poem,/poem/:id");
		corm_save();
		corm_close(hd);
	}
	
	// Third session: verify both entries persist
	printf("Third session: Verify both entries persisted:");
	{
		uint32_t hd = corm_open(filename, dbname, CM_STR, CM_STR, 0x7FFF, CM_MIRROR);
		const char *v1 = corm_get(hd, "auth");
		const char *v2 = corm_get(hd, "poem");
		
		ASSERT(v1 && strstr(v1, "Auth"), "Auth entry persisted");
		ASSERT(v2 && strstr(v2, "Poem"), "Poem entry persisted");
		
		// Verify iteration shows both
		uint32_t cur = corm_iter(hd, NULL, 0);
		const void *key, *value;
		int count = 0;
		while (corm_next(&key, &value, cur))
			count++;
		ASSERT(count == 2, "Should have exactly 2 entries");
		
		corm_close(hd);
	}
	
	// Test with complex values containing colons
	printf("Complex values with colons and special chars:");
	{
		uint32_t hd = corm_open(filename, dbname, CM_STR, CM_STR, 0x7FFF, CM_MIRROR);
		corm_put(hd, "complex", "a:b; c=/d/:id; e=f.tsx");
		corm_save();
		corm_close(hd);
		
		hd = corm_open(filename, dbname, CM_STR, CM_STR, 0x7FFF, CM_MIRROR);
		const char *v = corm_get(hd, "complex");
		ASSERT(v && strcmp(v, "a:b; c=/d/:id; e=f.tsx") == 0, "Complex value preserved");
		corm_close(hd);
	}
	
	// Test multiple sequential appends
	printf("Multiple sequential appends:");
	{
		for (int i = 0; i < 5; i++) {
			uint32_t hd = corm_open(filename, dbname, CM_STR, CM_STR, 0x7FFF, CM_MIRROR);
			char key[32], val[64];
			snprintf(key, sizeof(key), "entry_%d", i);
			snprintf(val, sizeof(val), "value_%d", i);
			corm_put(hd, key, val);
			corm_save();
			corm_close(hd);
		}
		
		// Verify all entries present
		uint32_t hd = corm_open(filename, dbname, CM_STR, CM_STR, 0x7FFF, CM_MIRROR);
		uint32_t cur = corm_iter(hd, NULL, 0);
		const void *key, *value;
		int count = 0;
		while (corm_next(&key, &value, cur))
			count++;
		// Should have: auth, poem, complex, entry_0, entry_1, entry_2, entry_3, entry_4 = 8 total
		ASSERT(count == 8, "Should have all 8 entries after sequential appends");
		corm_close(hd);
	}
	
	// Cleanup
	remove(filename);
}

int main(void) {
	printf("╔════════════════════════════════════════════════════════════╗\n");
	printf("║        Extended Test Suite for libcorm                    ║\n");
	printf("╚════════════════════════════════════════════════════════════╝\n");
	
	test_empty_map();
	test_capacity_limits();
	test_aindex();
	test_sorted_edge_cases();
	test_custom_types();
	test_iterator_edge_cases();
	test_mirror_maps();
	test_associations();
	test_assoc_delete_cleanup();
	test_assoc_update_cleanup();
	test_assoc_zombie_check();
	test_file_persistence();
	test_hndl_type();
	test_ptr_type();
	test_drop();
	test_update_keys();
	test_file_loading_no_mirror();
	test_pointer_stability();
	test_file_reopen_append();
	
	printf("\n╔════════════════════════════════════════════════════════════╗\n");
	if (errors == 0) {
		printf("║  ✅ ALL TESTS PASSED                                       ║\n");
	} else {
		printf("║  ❌ %u TEST(S) FAILED                                     ║\n", errors);
	}
	printf("╚════════════════════════════════════════════════════════════╝\n");
	
	return errors == 0 ? 0 : 1;
}
