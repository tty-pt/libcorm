/* librec_axis_probe.c — single-axis probe plugin for the L1 short-circuit
 * efficiency gate (test-shortcircuit.sh): proves corm_expr_eval skips the
 * decode/fill of AND/EXCEPT branches once the running set is empty
 * (AXIS-EFF plan L1).
 *
 * Axis "pe":
 *   - decode(value): returns a heap copy (process-lifetime params, matches
 *     the fold/plain decode convention) and, for the special probe value
 *     "boom", prints a stderr marker "AXIS-BOOM-DECODED\n" — the
 *     black-box observation point the shell test greps for.
 *   - fill(ctx, params, out): pushes the ctx's fixed ref list {1,2,3},
 *     EXCEPT when params == "empty", which returns the empty set.
 *
 * rec_axis_open ignores the alongside spec (this axis stores nothing on
 * disk) and returns a fresh ctx holding refs {1,2,3}. Test-only.
 */
#include <ttypt/rec.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* D14 axis-contributed CLI surface (test-only, probe plugin): the corm
 * CLI broadcasts inline `--query=…` to every bound axis declaring the
 * name; probe adopts it. After Slice 4, bare leaves carry no value, so
 * probe_decode must fall back to the CLI value when s == NULL. */
static char *probe_cli_query;

const struct rec_axis_cli_option *
rec_axis_cli_options(void)
{
	static const struct rec_axis_cli_option opts[] = {
		{ "query", 1, "probe value for testing (empty/boom/raw)" },
		{ NULL, 0, NULL }
	};
	return opts;
}

int
rec_axis_config_arg(const char *name, const char *value)
{
	if (!name || !value)
		return -1;
	if (!strcmp(name, "query"))
		return rec_cli_str_set(&probe_cli_query, value);
	return -1;
}

typedef struct {
	rec_ref_t refs[3];
	size_t     n;
} probe_ctx_t;

static int
probe_fill(void *ctx, void *params, rec_set_t *out)
{
	probe_ctx_t *c = ctx;

	if (!c || !out)
		return -1;
	if (params && !strcmp((const char *)params, "empty")) {
		rec_set_seal(out);
		return 0;
	}
	for (size_t i = 0; i < c->n; i++)
		rec_set_push(out, c->refs[i]);
	rec_set_seal(out);
	return 0;
}

static void *
probe_decode(const char *s)
{
	char *out = NULL;

	/* CLI fallback (post-flip bare leaves deliver NULL). */
	if (!s || !*s)
		s = probe_cli_query;
	if (!s)
		return NULL;

	/* Scoped flags deliver the stoma-style spec "query='value'"; the
	 * read-only scan unwraps the single query key so whole-string
	 * semantics see the raw value ("empty"/"boom"/anything). Probe
	 * declares only `query`, so there is at most one key; a raw value
	 * without a key (leaf `pe:boom`) has no '=' token and stays as-is.
	 * Exactly ONE heap copy on every path — the unwrapped query value
	 * or the whole spec. */
	{
		const char *cur = s, *key, *val;
		size_t klen, vlen;
		int quoted;

		while (rec_spec_scan(&cur, &key, &klen, &val, &vlen,
				     &quoted)) {
			if (rec_key_eq(key, klen, "query")) {
				if (rec_cli_str_dup(val, vlen, quoted,
						    &out) != 0)
					return NULL;
				break;
			}
		}
	}
	if (!out) {
		out = strdup(s);
		if (!out)
			return NULL;
	}
	if (!strcmp(out, "boom"))
		fprintf(stderr, "AXIS-BOOM-DECODED\n");
	return out;
}

__attribute__((constructor))
static void
probe_init(void)
{
	static const rec_axis_t pe = {
		"pe", probe_fill, NULL, NULL, probe_decode
	};

	rec_axis_register(&pe);
}

void *
rec_axis_open(const char *spec)
{
	static const rec_ref_t refs[] = { 1, 2, 3 };
	probe_ctx_t *c = calloc(1, sizeof(*c));

	(void) spec;
	if (!c)
		return NULL;
	memcpy(c->refs, refs, sizeof(refs));
	c->n = 3;
	return c;
}