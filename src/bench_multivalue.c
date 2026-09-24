/*
 * Performance benchmarks for CM_MULTIVALUE refactoring
 * 
 * This benchmarks the performance improvements from:
 * - P1.1: Fix double binary search in corm_iter
 * - P1.2: Optimize corm_count from O(n) to O(log n)
 * - P1.3: Remove redundant key lookup in corm_get_multi
 * - P1.5: Optimize corm_drop for standalone maps
 * - P1.6: Cache hashes for dense string lookup
 * - P3.1: Optimize corm_del_all
 */

#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <ttypt/corm.h>

#define BENCH(name) printf("\n[%s]\n", name)
#define TIME_START() clock_t start = clock()
#define TIME_END(var) double var = ((double)(clock() - start)) / CLOCKS_PER_SEC * 1000.0

/* Benchmark: corm_iter creation (P1.1 impact) */
void bench_iter_creation(void)
{
	BENCH("P1.1: corm_iter creation");
	
	/* Create map with 1000 duplicates */
	uint32_t hd = corm_open(NULL, NULL, CM_U32, CM_U32, 2047, CM_MULTIVALUE | CM_SORTED);
	for (uint32_t i = 0; i < 1000; i++) {
		uint32_t key = 100;
		corm_put(hd, &key, &i);
	}
	
	/* Benchmark: Create 10,000 iterators */
	TIME_START();
	for (int i = 0; i < 10000; i++) {
		uint32_t key = 100;
		uint32_t cur = corm_iter(hd, &key, 0);
		corm_fin(cur);
	}
	TIME_END(elapsed);
	
	printf("  10,000 iterations on 1000 duplicates: %.2f ms\n", elapsed);
	printf("  Average per iter: %.4f ms\n", elapsed / 10000.0);
	
	corm_close(hd);
}

/* Benchmark: corm_get_multi creation (P1.3 impact) */
void bench_get_multi(void)
{
	BENCH("P1.3: corm_get_multi creation");

	uint32_t hd = corm_open(NULL, NULL, CM_U32, CM_U32, 2047, CM_MULTIVALUE | CM_SORTED);
	for (int i = 0; i < 1000; i++) {
		uint32_t key = 100;
		uint32_t value = (uint32_t)i;
		corm_put(hd, &key, &value);
	}

	TIME_START();
	for (int i = 0; i < 10000; i++) {
		uint32_t key = 100;
		uint32_t cur = corm_get_multi(hd, &key);
		corm_fin(cur);
	}
	TIME_END(elapsed);

	printf("  10,000 get_multi calls on 1000 duplicates: %.2f ms\n", elapsed);
	printf("  Average per call: %.4f ms\n", elapsed / 10000.0);

	corm_close(hd);
}

/* Benchmark: iterate a duplicate run (cached key length impact) */
void bench_duplicate_iter(void)
{
	BENCH("P1.4: duplicate run iteration");

	uint32_t hd = corm_open(NULL, NULL, CM_U32, CM_U32, 2047, CM_MULTIVALUE | CM_SORTED);
	for (int i = 0; i < 1000; i++) {
		uint32_t key = 100;
		uint32_t value = (uint32_t)i;
		corm_put(hd, &key, &value);
	}

	TIME_START();
	for (int i = 0; i < 1000; i++) {
		uint32_t key = 100;
		uint32_t cur = corm_get_multi(hd, &key);
		const void *k, *v;

		while (corm_next(&k, &v, cur))
			(void)k, (void)v;

		corm_fin(cur);
	}
	TIME_END(elapsed);

	printf("  1,000 full duplicate scans on 1,000 duplicates: %.2f ms\n", elapsed);
	printf("  Average per scan: %.4f ms\n", elapsed / 1000.0);

	corm_close(hd);
}

/* Benchmark: corm_drop on standalone maps (P1.5 impact) */
void bench_drop(void)
{
	BENCH("P1.5: corm_drop performance");

	double total = 0.0;

	for (int run = 0; run < 100; run++) {
		uint32_t hd = corm_open(NULL, NULL, CM_U32, CM_U32, 2047, 0);

		for (uint32_t i = 0; i < 1000; i++)
			corm_put(hd, &i, &i);

		TIME_START();
		corm_drop(hd);
		TIME_END(elapsed);

		total += elapsed;
		corm_close(hd);
	}

	printf("  Drop 1,000-entry standalone map (avg of 100 runs): %.4f ms\n",
			total / 100.0);
}

/* Benchmark: dense string lookups (hash-cache impact) */
void bench_dense_lookup(void)
{
	BENCH("P1.6: dense string lookup");

	uint32_t hd = corm_open(NULL, NULL, CM_STR, CM_U32, 2047, 0);
	char keybuf[32];

	for (uint32_t i = 0; i < 1800; i++) {
		snprintf(keybuf, sizeof(keybuf), "key%04u", i);
		corm_put(hd, keybuf, &i);
	}

	snprintf(keybuf, sizeof(keybuf), "key1799");
	TIME_START();
	for (int i = 0; i < 10000; i++) {
		const uint32_t *value = corm_get(hd, keybuf);
		(void)value;
	}
	TIME_END(elapsed);

	printf("  10,000 lookups in 1,800-entry string map: %.2f ms\n", elapsed);
	printf("  Average per lookup: %.4f ms\n", elapsed / 10000.0);

	corm_close(hd);
}

/* Benchmark: string puts on a primary map (payload allocation impact) */
void bench_put_strings(void)
{
	BENCH("P1.7: string put performance");

	double total = 0.0;
	char keybuf[32];
	char valbuf[32];

	for (int run = 0; run < 100; run++) {
		uint32_t hd = corm_open(NULL, NULL, CM_STR, CM_STR, 2047, 0);

		TIME_START();
		for (uint32_t i = 0; i < 1000; i++) {
			snprintf(keybuf, sizeof(keybuf), "key%04u", i);
			snprintf(valbuf, sizeof(valbuf), "val%04u", i);
			corm_put(hd, keybuf, valbuf);
		}
		TIME_END(elapsed);

		total += elapsed;
		corm_close(hd);
	}

	printf("  Insert 1,000 string entries (avg of 100 runs): %.4f ms\n",
			total / 100.0);
}

/* Benchmark: corm_count (P1.2 impact) */
void bench_count(void)
{
	BENCH("P1.2: corm_count performance");
	
	int duplicate_counts[] = {1, 10, 100, 1000, 10000};
	int capacities[] = {15, 31, 255, 2047, 16383};
	int num_tests = sizeof(duplicate_counts) / sizeof(duplicate_counts[0]);
	
	for (int t = 0; t < num_tests; t++) {
		int dup_count = duplicate_counts[t];
		int capacity = capacities[t];
		
		/* Create map with N duplicates */
		uint32_t hd = corm_open(NULL, NULL, CM_U32, CM_U32, capacity, CM_MULTIVALUE | CM_SORTED);
		for (int i = 0; i < dup_count; i++) {
			uint32_t key = 100;
			uint32_t value = (uint32_t)i;
			corm_put(hd, &key, &value);
		}
		
		/* Benchmark: Call corm_count 1000 times */
		TIME_START();
		for (int i = 0; i < 1000; i++) {
			uint32_t key = 100;
			size_t count = corm_count(hd, &key);
			(void)count;  /* Suppress unused warning */
		}
		TIME_END(elapsed);
		
		printf("  %5d duplicates (1000 calls): %8.2f ms (avg: %.4f ms)\n", 
		       dup_count, elapsed, elapsed / 1000.0);
		
		corm_close(hd);
	}
}

/* Benchmark: corm_del_all (P3.1 impact) */
void bench_del_all(void)
{
	BENCH("P3.1: corm_del_all performance");
	
	int duplicate_counts[] = {10, 100, 1000};
	int capacities[] = {31, 255, 2047};
	int num_tests = sizeof(duplicate_counts) / sizeof(duplicate_counts[0]);
	
	for (int t = 0; t < num_tests; t++) {
		int dup_count = duplicate_counts[t];
		int capacity = capacities[t];
		
		/* Average over 100 runs to reduce noise */
		double total_time = 0.0;
		for (int run = 0; run < 100; run++) {
			/* Create map with N duplicates */
			uint32_t hd = corm_open(NULL, NULL, CM_U32, CM_U32, capacity, CM_MULTIVALUE | CM_SORTED);
			for (int i = 0; i < dup_count; i++) {
				uint32_t key = 100;
				uint32_t value = (uint32_t)i;
				corm_put(hd, &key, &value);
			}
			
			/* Benchmark: Delete all */
			TIME_START();
			{
				uint32_t key = 100;
				corm_del_all(hd, &key);
			}
			TIME_END(elapsed);
			
			total_time += elapsed;
			corm_close(hd);
		}
		
		printf("  %4d duplicates (avg of 100 runs): %.4f ms\n", 
		       dup_count, total_time / 100.0);
	}
}

/* Memory usage check */
void check_memory(void)
{
	BENCH("Memory usage check");
	
	/* Create large map with many duplicates */
	uint32_t hd = corm_open(NULL, NULL, CM_U32, CM_U32, 16383, CM_MULTIVALUE | CM_SORTED);
	
	printf("  Adding 10,000 duplicates...\n");
	for (int i = 0; i < 10000; i++) {
		uint32_t key = 100;
		uint32_t value = (uint32_t)i;
		corm_put(hd, &key, &value);
	}
	
	{
		uint32_t key = 100;
		size_t count = corm_count(hd, &key);
		printf("  Verified count: %zu\n", count);
		
		printf("  Iterating through all entries...\n");
		uint32_t cur = corm_iter(hd, &key, 0);
		size_t iter_count = 0;
		const void *k, *v;
		while (corm_next(&k, &v, cur))
			iter_count++;
		corm_fin(cur);
		
		printf("  Iterator count: %zu\n", iter_count);
		
		if (count == iter_count && count == 10000)
			printf("  ✓ All checks passed\n");
		else
			printf("  ✗ Mismatch detected!\n");
	}
	
	corm_close(hd);
}

int main(void)
{
	printf("=== CM_MULTIVALUE Performance Benchmarks ===\n");
	printf("Version: v0.7.0 + refactoring\n");
	printf("Date: Mon Feb 23 2026\n");
	
	bench_iter_creation();
	bench_get_multi();
	bench_duplicate_iter();
	bench_drop();
	bench_dense_lookup();
	bench_put_strings();
	bench_count();
	bench_del_all();
	check_memory();
	
	printf("\n=== Benchmarks Complete ===\n");
	return 0;
}
