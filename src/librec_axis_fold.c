/* librec_axis_fold.c — functional axis plugin for the 2B-3/-X/-g . test
 * matrix (alpha/beta/pure over real corm a:u stores) and the 2B-4 write-
 * fan-out gate.  Registers three axes in one constructor:
 *   alpha  { fill, rank }    → refs from alpha fill keys ∪ alpha stash
 *   beta   { fill, rank }    → refs from beta fill keys ∪ beta stash
 *   pure   { fill, NULL }    → refs from pure fill keys ∪ pure stash
 *
 * Each axis owns TWO alongside stores (fold_ctx_t): `fill` (read-only,
 * the `<dir>/<primary>-<name>` a:u store the CLI seeds — the -X fill
 * source) and `stash` (`<dir>/<primary>-<name>.wr`, HNDL→string, the
 * 2B-4 write-fan-out target).
 * fill = union of both handles' refs; rank = score = ref; decode = NULL
 * (whole-string VALUE forwarded).  Same multi-axis-one-so pattern as
 * librec_axis_mock.c (works around the shared mk LIB-obj-y aggregation
 * bug; see that file's header comment).
 *
 * 2B-4 write contract carried by each axis (mm-plan/PHASE-2-CLI.md 2A/D12):
 *   rec_axis_store(ctx, spec, ref, value) — stores the whole `value`
 *     string verbatim for `ref` (same-ref put replaces in place).
 *   rec_axis_store_typed(ctx, spec, ref, blob, len, qtype) — stores the
 *     binary payload as a "T:<decimal>" tag for fixed 4-byte built-ins
 *     (CM_HNDL/CM_U32) so the CLI gate can tell which symbol ran; CM_STR
 *     is stored raw; anything else → -1 (EINVAL).
 *   rec_axis_unstore(ctx, ref) — removes the stash entry (idempotent —
 *     absent ref → 0).
 *   rec_axis_readback(ctx, ref, blob_out, n_out) — malloc'd copy of the
 *     stored entry display string (absent → NULL/0, still 0).
 *
 * CORM_AXIS_LIBS loads it; -X expr names pull axes by name. */
#include <ttypt/rec.h>
#include <ttypt/corm.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
	uint32_t fill;   /* alongside <dir>/<primary>-<name> : read-only a:u ref set */
	uint32_t stash;  /* alongside <dir>/<primary>-<name>.wr : HNDL→string write stash */
	char     name[16]; /* axis name parsed from the alongside spec (side-channel) */
} fold_ctx_t;

/* ── D14 axis-contributed CLI options — dlsym'd by the corm CLI like
 *    rec_axis_env_config (never declared or called by libcorm): the CLI
 *    broadcasts every collected --name=value to each bound .so that
 *    declares the name via rec_axis_cli_options(); this plugin adopts
 *    them into fold-level state so the -X fill path (and the test
 *    side-channel) can observe them. ── */
static const char *fold_cli_query;
static int fold_cli_query_set;
static int fold_cli_verbose;

/* Default hash mask (D11): power-of-two-minus-one bucket hint, auto-grow
 * (corm.h:172). CORM_MASK env overrides (benches); same derivation as the
 * CLI's gen_open so co-opened files always match. */
static uint32_t
fold_mask(void)
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
fold_fill(void *ctx, void *params, rec_set_t *out)
{
	fold_ctx_t *c = ctx;
	(void) params;
	if (!c)
		return -1;
	/* D14 test side-channel: echoes the broadcast --query so test-cli.sh
	 * can assert delivery + per-axis reachability on stderr without
	 * disturbing the composed stdout. Silently absent when no plugin
	 * option was passed. */
	if (fold_cli_query_set && c->name[0])
		fprintf(stderr, "fold %s query=%s verbose=%d\n",
				c->name, fold_cli_query, fold_cli_verbose);
	/* Scoped-flags side-channel: echoes the leaf spec string delivered to
	 * this fold instance (params = the raw leaf value; decode is NULL).
	 * Absent when the leaf carries no value — existing broadcast tests
	 * keep their exact stderr. */
	if (params && ((const char *)params)[0])
		fprintf(stderr, "fold %s leaf=%s\n", c->name,
				(const char *)params);
	rec_set_fill_corm_iter(out, c->fill);
	rec_set_fill_corm_iter(out, c->stash);
	rec_set_seal(out);
	return 0;
}

static int
fold_rank(void *ctx, void *params, rec_ref_t ref, float *score)
{
	(void) ctx;
	(void) params;
	*score = (float) ref;
	return 0;
}

/* ── 2B-4 store/unstore/readback conventional exports (dlsym'd by the
 *    CLI like rec_axis_open — never declared or called by libcorm). ── */

int
rec_axis_store(void *ctx, const char *spec, rec_ref_t ref, const char *value)
{
	fold_ctx_t *c = ctx;
	(void) spec;
	if (!c || !value)
		return -1;
	corm_put(c->stash, &ref, value);
	return 0;
}

int
rec_axis_store_typed(void *ctx, const char *spec, rec_ref_t ref,
		const void *blob, size_t len, uint32_t qtype)
{
	fold_ctx_t *c = ctx;
	(void) spec;
	if (!c || !blob)
		return -1;
	/* Fixed 4-byte built-ins: tag-decimal (lets the gate distinguish the
	 * typed symbol from the string one). CM_STR: stored raw. */
	if ((qtype == CM_HNDL || qtype == CM_U32)
			&& len == sizeof(uint32_t)) {
		uint32_t v;
		char tag[16];
		memcpy(&v, blob, sizeof(v));
		snprintf(tag, sizeof(tag), "T:%u", v);
		corm_put(c->stash, &ref, tag);
		return 0;
	}
	if (qtype == CM_STR && len == corm_type_len(CM_STR)) {
		corm_put(c->stash, &ref, blob);
		return 0;
	}
	errno = EINVAL;
	return -1;
}

int
rec_axis_unstore(void *ctx, rec_ref_t ref)
{
	fold_ctx_t *c = ctx;
	if (!c)
		return -1;
	corm_del(c->stash, &ref);  /* idempotent: absent ref is a no-op */
	return 0;
}

int
rec_axis_readback(void *ctx, rec_ref_t ref, char **blob_out, size_t *n_out)
{
	fold_ctx_t *c = ctx;
	const char *v;

	if (blob_out)
		*blob_out = NULL;
	if (n_out)
		*n_out = 0;
	if (!c)
		return -1;
	v = corm_get(c->stash, &ref);
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

/* ── D14 CLI-option convention: declared surface + per-option delivery.
 *    Same optional dlsym pattern as rec_axis_env_config — the corm CLI is
 *    axis-agnostic and never declares/calls these itself. The option
 *    struct ABI is kernel-owned in <ttypt/rec.h>. ── */

const struct rec_axis_cli_option *
rec_axis_cli_options(void)
{
	static const struct rec_axis_cli_option opts[] = {
		{ "query",   1, "broadcast full-text/embed query" },
		{ "verbose", 0, "bare flag (broadcast)" },
		{ NULL, 0, NULL }
	};
	return opts;
}

int
rec_axis_config_arg(const char *name, const char *value)
{
	if (!name)
		return -1;
	if (!strcmp(name, "query")) {
		if (!value)
			return -1;
		fold_cli_query = value;
		fold_cli_query_set = 1;
		return 0;
	}
	if (!strcmp(name, "verbose")) {
		fold_cli_verbose = 1;
		fold_cli_query_set = 1;  /* observable side-channel (query still NULL) */
		return 0;
	}
	return -1;
}

static int fold_alpha_slot, fold_beta_slot, fold_pure_slot;

__attribute__((constructor))
static void fold_init(void)
{
	static const rec_axis_t alpha =
		{ "alpha", fold_fill, fold_rank, NULL, NULL };
	static const rec_axis_t beta =
		{ "beta",  fold_fill, fold_rank, NULL, NULL };
	static const rec_axis_t pure =
		{ "pure",  fold_fill, NULL,     NULL, NULL };
	fold_alpha_slot = rec_axis_register(&alpha);
	fold_beta_slot  = rec_axis_register(&beta);
	fold_pure_slot  = rec_axis_register(&pure);
	(void) fold_alpha_slot;
	(void) fold_beta_slot;
	(void) fold_pure_slot;
}

/* rec_axis_open convention: alongside-default spec <primary-dir>/<primary>-<name>,
 * corm a:u store (fill side, CLI mask derivation). The stash
 * <primary-dir>/<primary>-<name>.wr is derived beside it.
 *
 * Database name MUST be "hd" with the CLI mask: corm files namespace
 * records by dbid = XXH32(database) and the CLI seeds axis stores via
 * gen_open(..., "hd", ...). Any other name loads nothing (and the
 * exit-time save would truncate the file). */
void *
rec_axis_open(const char *spec)
{
	fold_ctx_t *c;
	char wr[BUFSIZ];
	size_t n;

	if (!spec)
		return NULL;
	c = calloc(1, sizeof(*c));
	if (!c)
		return NULL;
	c->fill = corm_open(spec, "hd", CM_HNDL, CM_U32,
			fold_mask(), CM_AINDEX);

	/* Axis name = the alongside spec suffix after the last '-' (e.g.
	 * "demo.db-alpha" → "alpha") for the fold_ctx side-channel. */
	{
		const char *last = strrchr(spec, '-');
		if (last && last[1])
			snprintf(c->name, sizeof(c->name), "%s", last + 1);
	}

	/* <dir>/<primary>-<name> → <dir>/<primary>-<name>.wr (same base).
	 * The ends-in-.db spelling is legacy (pre-per-primary stores). */
	n = strlen(spec);
	if (n >= 3 && !strcmp(spec + n - 3, ".db"))
		snprintf(wr, sizeof(wr), "%.*s.wr", (int)(n - 3), spec);
	else
		snprintf(wr, sizeof(wr), "%s.wr", spec);
	c->stash = corm_open(wr, "hd", CM_HNDL, CM_STR,
			fold_mask(), 0);
	return c;
}
