/* librec_axis_plain.c — minimal string-only axis plugin for the 2B-4
 * write-fan-out gate.  Registers ONE axis named "plain" (fill + rank).
 * Unlike librec_axis_fold, this plugin exports the string
 * `rec_axis_store` but NO `rec_axis_store_typed` — the CLI's typed
 * dispatch (D12) must therefore fall back to the string symbol on binary
 * primaries (text-only axis).  The stash is file-backed
 * (`<dir>/<primary>-plain.wr`, HNDL→string), so the gate's dlopen-based verifier
 * can read back cross-process what the CLI fan-out stored. */
#include <ttypt/rec.h>
#include <ttypt/corm.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint32_t
plain_mask(void)
{
	const char *e = getenv("CORM_MASK");
	if (e && *e) {
		unsigned long v = strtoul(e, NULL, 10);
		if (v != 0 && (v & (v + 1)) == 0)
			return (uint32_t) v;
	}
	return 4096 - 1;
}

static int
plain_fill(void *ctx, void *params, rec_set_t *out)
{
	(void) params;
	if (!ctx)
		return -1;
	rec_set_fill_corm_iter(out, (uint32_t)(uintptr_t) ctx);
	rec_set_seal(out);
	return 0;
}

static int
plain_rank(void *ctx, void *params, rec_ref_t ref, float *score)
{
	(void) ctx;
	(void) params;
	*score = (float) ref;
	return 0;
}

int
rec_axis_store(void *ctx, const char *spec, rec_ref_t ref, const char *value)
{
	uint32_t stash = (uint32_t)(uintptr_t) ctx;
	(void) spec;
	if (!ctx || !value)
		return -1;
	corm_put(stash, &ref, value);
	return 0;
}

int
rec_axis_unstore(void *ctx, rec_ref_t ref)
{
	uint32_t stash = (uint32_t)(uintptr_t) ctx;
	if (!ctx)
		return -1;
	corm_del(stash, &ref);  /* idempotent: absent ref is a no-op */
	return 0;
}

int
rec_axis_readback(void *ctx, rec_ref_t ref, char **blob_out, size_t *n_out)
{
	uint32_t stash = (uint32_t)(uintptr_t) ctx;
	const char *v;

	if (blob_out)
		*blob_out = NULL;
	if (n_out)
		*n_out = 0;
	if (!ctx)
		return -1;
	v = corm_get(stash, &ref);
	if (!v)
		return 0;
	if (blob_out) {
		*blob_out = strdup(v);
		if (!*blob_out)
			return -1;
	}
	if (n_out)
		*n_out = strlen(v);
	return 0;
}

static int plain_slot = -1;

__attribute__((constructor))
static void plain_init(void)
{
	static const rec_axis_t a =
		{ "plain", plain_fill, plain_rank, NULL, NULL };
	plain_slot = rec_axis_register(&a);
	(void) plain_slot;
}

/* rec_axis_open convention: alongside-default spec
 * <primary-dir>/<primary>-plain; the stash is the same base with .wr. "hd"
 * database + CLI mask (corm namespaces by dbid = XXH32(database)). */
void *
rec_axis_open(const char *spec)
{
	char wr[BUFSIZ];
	size_t n;
	uint32_t stash;

	if (!spec)
		return NULL;
	n = strlen(spec);
	if (n >= 3 && !strcmp(spec + n - 3, ".db"))
		snprintf(wr, sizeof(wr), "%.*s.wr", (int)(n - 3), spec);
	else
		snprintf(wr, sizeof(wr), "%s.wr", spec);
	stash = corm_open(wr, "hd", CM_HNDL, CM_STR, plain_mask(), 0);
	return (void *)(uintptr_t) stash;
}
