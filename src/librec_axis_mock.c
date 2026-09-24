/* librec_axis_mock.c — mock rec_axis plugin (retired with the old
 * recall-query CLI mode in 2B-3; kept built but unused by test-cli.sh, which now exercises the
 * functional librec_axis_fold plugin instead). Registers TWO axes in one
 * constructor — "mock_a" (fill+rank+decode) and "mock_b" (fill-only, no
 * rank, no decode) — so a single dlopen brings in both halves of an
 * AND/OR/NOT composition test.
 *
 * (Deliberately one plugin with two axes, not two plugins: `dlopen` caches
 * by path, so two independently-`dlopen`'d *different* .so files would
 * each need their own Makefile "lib" target — but ../mk/include.mk's
 * LIB-obj-y aggregation has a latent bug with 3+ concurrent "lib*" targets
 * in one Makefile ($(LIB:%=%-obj-y) expands to a multi-word string, and
 * GNU Make's $($(X)) double-dereference only works for a single-word X,
 * so libcorm's own extra objects — idm.o/rec.o/rec_axis.o — silently drop
 * out of the link line). Rather than touch that shared, cross-repo build
 * system, this test stays a single "lib" target and gets its two
 * independent axes from one constructor instead.)
 *
 * Built as a loadable .so (Makefile's `all` list, name starts with "lib" so
 * the shared LIB build rule picks it up) but NEVER linked into corm/libcorm
 * themselves — dlopen'd only by axis consumers via CORM_AXIS_LIBS. Mirrors
 * the shape a real axis library's registration constructor would have
 * (see the recall-query plan doc §3's four sibling axis libraries) except
 * for the two-axes-in-one-constructor deviation explained above, which is
 * purely a test-plugin convenience, not a convention real axis libs follow.
 */
#include <ttypt/rec.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Store/unstore/get backing store: per-axis in-memory ref → value entries.
 * Mirrors the shape a real axis's store would have (ref-indexed, idempotent
 * unstore, malloc'd read-back on get) — the mock proves the plugin
 * convention (2A-mock) without any real axis library. */
typedef struct {
	rec_ref_t ref;
	char     *value;
} mock_entry_t;

typedef struct {
	mock_entry_t *entries;
	size_t        n, cap;
} mock_store_t;

typedef struct {
	rec_ref_t *refs;
	size_t n;
	mock_store_t store;
} mock_ctx_t;

static int
store_append(mock_store_t *st, rec_ref_t ref, const char *value)
{
	mock_entry_t *e;
	size_t i;

	if (st->n == st->cap) {
		size_t ncap = st->cap ? st->cap * 2 : 4;
		mock_entry_t *grown = realloc(st->entries, ncap * sizeof(*st->entries));
		if (!grown)
			return -1;
		st->entries = grown;
		st->cap = ncap;
	}
	/* Same-ref put replaces in place (matches sepal / corm semantics). */
	for (i = 0; i < st->n; i++)
		if (st->entries[i].ref == ref) {
			char *copy = strdup(value);
			if (!copy)
				return -1;
			free(st->entries[i].value);
			st->entries[i].value = copy;
			return 0;
		}
	e = &st->entries[st->n++];
	e->ref = ref;
	e->value = strdup(value);
	return e->value ? 0 : -1;
}

static int
store_remove(mock_store_t *st, rec_ref_t ref)
{
	size_t i;

	for (i = 0; i < st->n; i++)
		if (st->entries[i].ref == ref) {
			free(st->entries[i].value);
			st->entries[i] = st->entries[--st->n];
			return 0;
		}
	return 0;
}

static const char *
store_find(mock_store_t *st, rec_ref_t ref)
{
	size_t i;

	for (i = 0; i < st->n; i++)
		if (st->entries[i].ref == ref)
			return st->entries[i].value;
	return NULL;
}

/* ── rec_axis_store / rec_axis_unstore / rec_axis_readback conventional
 *    exports (2A-mock / PHASE-2-CLI.md §2A contract; optional, CLI-specific,
 *    dlsym'd like rec_axis_open — never declared or called by libcorm). The
 *    mock stores the whole `value` string verbatim per (axis, ref). Read-back
 *    is rec_axis_readback: rec_axis_get(int) is the kernel registry lookup. */

int
rec_axis_store(void *ctx, const char *spec, rec_ref_t ref, const char *value)
{
	mock_ctx_t *c = ctx;
	(void) spec;
	if (!c || !value)
		return -1;
	return store_append(&c->store, ref, value);
}

int
rec_axis_unstore(void *ctx, rec_ref_t ref)
{
	mock_ctx_t *c = ctx;
	if (!c)
		return -1;
	return store_remove(&c->store, ref);
}

int
rec_axis_readback(void *ctx, rec_ref_t ref, char **blob_out, size_t *n_out)
{
	mock_ctx_t *c = ctx;
	const char *v;
	char *copy;

	if (blob_out)
		*blob_out = NULL;
	if (n_out)
		*n_out = 0;
	if (!c)
		return -1;
	v = store_find(&c->store, ref);
	if (!v)
		return 0;
	copy = strdup(v);
	if (!copy)
		return -1;
	if (blob_out)
		*blob_out = copy;
	if (n_out)
		*n_out = strlen(copy);
	return 0;
}

static int mock_a_slot = -1;
static int mock_b_slot = -1;

static int
mock_a_fill(void *ctx, void *params, rec_set_t *out)
{
	(void) params;
	mock_ctx_t *c = ctx;
	if (!c)
		return -1;
	for (size_t i = 0; i < c->n; i++)
		rec_set_push(out, c->refs[i]);
	rec_set_seal(out);
	return 0;
}

static int
mock_a_rank(void *ctx, void *params, rec_ref_t ref, float *score)
{
	(void) ctx;
	(void) params;
	*score = (float) ref;
	return 0;
}

/* No query-time params needed by this mock; pass the raw string straight
 * through so an axis consumer's decode path has something non-NULL to
 * forward (exercises the "decode present, params forwarded" path). */
static void *
mock_a_decode(const char *s)
{
	return (void *) s;
}

static int
mock_b_fill(void *ctx, void *params, rec_set_t *out)
{
	(void) params;
	mock_ctx_t *c = ctx;
	if (!c)
		return -1;
	for (size_t i = 0; i < c->n; i++)
		rec_set_push(out, c->refs[i]);
	rec_set_seal(out);
	return 0;
}

__attribute__((constructor))
static void
mock_init(void)
{
	static const rec_axis_t a = {
		"mock_a", mock_a_fill, mock_a_rank, NULL, mock_a_decode
	};
	static const rec_axis_t b = { "mock_b", mock_b_fill, NULL, NULL, NULL };
	mock_a_slot = rec_axis_register(&a);
	mock_b_slot = rec_axis_register(&b);
}

/* Parses one ref list into a malloc'd mock_ctx_t. "refs=" prefix optional. */
static mock_ctx_t *
parse_refs(const char *s)
{
	if (!s)
		return NULL;

	const char *eq = strchr(s, '=');
	const char *list = eq ? eq + 1 : s;

	size_t cap = 8, n = 0;
	rec_ref_t *refs = malloc(cap * sizeof(*refs));
	if (!refs)
		return NULL;

	char *copy = strdup(list);
	if (!copy) {
		free(refs);
		return NULL;
	}

	char *save = NULL;
	for (char *tok = strtok_r(copy, ",", &save); tok;
			tok = strtok_r(NULL, ",", &save)) {
		if (n == cap) {
			cap *= 2;
			rec_ref_t *grown = realloc(refs, cap * sizeof(*refs));
			if (!grown) {
				free(refs);
				free(copy);
				return NULL;
			}
			refs = grown;
		}
		refs[n++] = (rec_ref_t) strtoull(tok, NULL, 10);
	}
	free(copy);

	mock_ctx_t *ctx = calloc(1, sizeof(*ctx));
	if (!ctx) {
		free(refs);
		return NULL;
	}
	ctx->refs = refs;
	ctx->n = n;
	return ctx;
}

/* rec_axis_open convention: spec is
 * "a=1,2,3:b=2,3,4" — one ref list per axis this plugin owns, separated by
 * ':'. Binds mock_a's ctx directly via rec_axis_set_ctx() (this plugin
 * knows its own slot from mock_init()'s rec_axis_register() return value)
 * and returns mock_b's ctx for the CLI's own rec_axis_set_ctx(new_slot,
 * ...) call to bind (new_slot is always the *last* slot this dlopen
 * registered — mock_b, since it was registered second). Never freed
 * (process lifetime — dlclose is never called on axis plugins, matching
 * real store handles). */
void *
rec_axis_open(const char *spec)
{
	if (!spec)
		return NULL;

	char *copy = strdup(spec);
	if (!copy)
		return NULL;

	char *save = NULL;
	char *part_a = strtok_r(copy, ":", &save);
	char *part_b = strtok_r(NULL, ":", &save);

	mock_ctx_t *ctx_a = parse_refs(part_a);
	mock_ctx_t *ctx_b = parse_refs(part_b);
	free(copy);

	if (mock_a_slot >= 0 && ctx_a)
		rec_axis_set_ctx(mock_a_slot, ctx_a);

	return ctx_b;
}
