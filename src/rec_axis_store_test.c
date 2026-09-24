/* rec_axis_store_test.c — 2A-mock: unit tests for the conventional
 * rec_axis_store / rec_axis_unstore / rec_axis_readback plugin exports,
 * proved against the mock axis plugin via dlopen+dlsym — no CLI involved.
 *
 * Mirrors exactly how a consumer CLI would discover the symbols: qsys_dlopen
 * the .so, qsys_dlsym rec_axis_open, bind the returned ctx to the last
 * registered slot, then call store/unstore/get through the dlsym'd pointers.
 * The loosened-8 fixture below is the same one test-cli.sh uses, so the
 * round-trip numbers here double-check the CLI behaviour from the C side.
 */

#include <ttypt/rec.h>
#include <ttypt/qsys.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

unsigned errors = 0;

#define PASS() printf("  OK\n")
#define FAIL(msg) do { printf("  FAIL: %s [line %d]\n", msg, __LINE__); errors++; } while(0)
#define ASSERT(cond, msg) do { if (!(cond)) FAIL(msg); else PASS(); } while(0)

static void *mock_h;

typedef void *(*open_fn)(const char *);
typedef int (*store_fn)(void *, const char *, rec_ref_t, const char *);
typedef int (*unstore_fn)(void *, rec_ref_t);
typedef int (*get_fn)(void *, rec_ref_t, char **, size_t *);

static open_fn    mock_open;
static store_fn   mock_store;
static unstore_fn mock_unstore;
static get_fn     mock_get;

/* The fixture spec test-cli.sh feeds rec_axis_open. */
#define MOCK_SPEC "a=1,2,3:b=2,3,4"

static void
load_plugin(void)
{
	void *sym;

	mock_h = qsys_dlopen("./lib/librec_axis_mock.so", 0);
	ASSERT(mock_h != NULL, "mock plugin loads");

	sym = qsys_dlsym(mock_h, "rec_axis_open");
	memcpy(&mock_open, &sym, sizeof(mock_open));
	sym = qsys_dlsym(mock_h, "rec_axis_store");
	memcpy(&mock_store, &sym, sizeof(mock_store));
	sym = qsys_dlsym(mock_h, "rec_axis_unstore");
	memcpy(&mock_unstore, &sym, sizeof(mock_unstore));
	sym = qsys_dlsym(mock_h, "rec_axis_readback");
	memcpy(&mock_get, &sym, sizeof(mock_get));

	ASSERT(mock_open != NULL, "rec_axis_open exported");
	ASSERT(mock_store != NULL, "rec_axis_store exported");
	ASSERT(mock_unstore != NULL, "rec_axis_unstore exported");
	ASSERT(mock_get != NULL, "rec_axis_readback exported");
}

/* Mock registers two axes ("mock_a", "mock_b"); rec_axis_open binds mock_a
 * itself and returns mock_b's ctx for the caller to bind. Look up slots by
 * name so the tests are robust. */
static int
slot_by_name(const char *name)
{
	int i, n = rec_axis_count();
	for (i = 0; i < n; i++) {
		const rec_axis_t *a = rec_axis_get(i);
		if (a && strcmp(a->name, name) == 0)
			return i;
	}
	return -1;
}

static void
test_roundtrip(void)
{
	int slot = slot_by_name("mock_b");
	char *blob = NULL;
	size_t n = 0;

	printf("=== store → get → unstore → get zero (mock_b) ===\n");
	ASSERT(slot >= 0, "mock_b registered");
	void *ctx = rec_axis_get(slot)->ctx;
	ASSERT(ctx != NULL, "mock_b ctx bound");

	ASSERT(mock_store(ctx, NULL, 42, "hello") == 0, "store ref 42");
	ASSERT(mock_get(ctx, 42, &blob, &n) == 0, "get ref 42 returns 0");
	ASSERT(blob && strcmp(blob, "hello") == 0, "value read back");
	ASSERT(n == 5, "length 5");
	free(blob); blob = NULL;

	ASSERT(mock_unstore(ctx, 42) == 0, "unstore ref 42");
	ASSERT(mock_get(ctx, 42, &blob, &n) == 0, "get after unstore returns 0");
	ASSERT(blob == NULL, "blob NULL after unstore");
	ASSERT(n == 0, "len 0 after unstore");
}

static void
test_idempotent_unstore(void)
{
	int slot = slot_by_name("mock_b");
	void *ctx = rec_axis_get(slot)->ctx;

	printf("=== unstore of absent ref is a no-op (compensation primitive) ===\n");
	ASSERT(mock_unstore(ctx, 999) == 0, "absent unstore → 0");
}

static void
test_absent_get(void)
{
	int slot = slot_by_name("mock_b");
	void *ctx = rec_axis_get(slot)->ctx;
	char *blob = (char *)0x1;   /* poison: must be cleared on miss */
	size_t n = 123;

	printf("=== get of absent ref returns 0 with cleared outputs ===\n");
	ASSERT(mock_get(ctx, 777, &blob, &n) == 0, "absent get → 0");
	ASSERT(blob == NULL, "blob cleared on miss");
	ASSERT(n == 0, "len cleared on miss");
}

static void
test_replace(void)
{
	int slot = slot_by_name("mock_b");
	void *ctx = rec_axis_get(slot)->ctx;
	char *blob = NULL;
	size_t n = 0;

	printf("=== same-ref store replaces in place ===\n");
	ASSERT(mock_store(ctx, NULL, 5, "first") == 0, "store ref 5 first");
	ASSERT(mock_store(ctx, NULL, 5, "second") == 0, "store ref 5 second");
	ASSERT(mock_get(ctx, 5, &blob, &n) == 0, "get ref 5");
	ASSERT(blob && strcmp(blob, "second") == 0, "new value replaces old");
	free(blob); blob = NULL;

	ASSERT(mock_unstore(ctx, 5) == 0, "unstore ref 5 cleans up");
}

static void
test_null_ctx(void)
{
	printf("=== NULL ctx rejected by all three ===\n");
	char *blob = (char *)0x1;
	size_t n = 1;
	ASSERT(mock_store(NULL, NULL, 1, "x") == -1, "store NULL ctx → -1");
	ASSERT(mock_unstore(NULL, 1) == -1, "unstore NULL ctx → -1");
	ASSERT(mock_get(NULL, 1, &blob, &n) == -1, "get NULL ctx → -1");
	ASSERT(blob == NULL && n == 0, "get NULL ctx clears outputs");
}

static void
test_axis_isolation(void)
{
	int a = slot_by_name("mock_a");
	int b = slot_by_name("mock_b");
	void *ctx_a = a >= 0 ? rec_axis_get(a)->ctx : NULL;
	void *ctx_b = b >= 0 ? rec_axis_get(b)->ctx : NULL;
	char *blob = NULL;
	size_t n = 0;

	printf("=== each axis owns an independent store ===\n");
	ASSERT(ctx_a != NULL && ctx_b != NULL, "both ctx bound");
	ASSERT(mock_store(ctx_a, NULL, 42, "in-a") == 0, "store into mock_a");
	ASSERT(mock_get(ctx_b, 42, &blob, &n) == 0, "get from mock_b");
	ASSERT(blob == NULL && n == 0, "mock_b sees nothing from mock_a");
}

int
main(void)
{
	load_plugin();

	/* rec_axis_open binds mock_a internally and returns mock_b's ctx. */
	void *ctx_b = mock_open(MOCK_SPEC);
	ASSERT(ctx_b != NULL, "rec_axis_open returns mock_b ctx");
	{
		int slot_b = slot_by_name("mock_b");
		ASSERT(slot_b >= 0, "mock_b located");
		rec_axis_set_ctx(slot_b, ctx_b);
	}

	/* Detect the registry auto-loaded by the plugin constructor. */
	ASSERT(rec_axis_count() >= 2, "two axes registered after dlopen");

	test_roundtrip();
	test_idempotent_unstore();
	test_absent_get();
	test_replace();
	test_null_ctx();
	test_axis_isolation();

	printf("\n");
	if (errors == 0)
		printf("ALL REC_AXIS_STORE TESTS PASSED\n");
	else
		printf("%u REC_AXIS_STORE TEST(S) FAILED\n", errors);

	return errors == 0 ? 0 : 1;
}