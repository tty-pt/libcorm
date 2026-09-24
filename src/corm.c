/**
 * @page corm corm(1)
 * @brief Corm command-line tool — create, query, and associate persistent maps.
 *
 * Simple CLI for the Corm engine. Replaces the old **qdb** and links directly
 * against libcorm.
 *
 * ## Overview
 *
 * Creates/opens key-value databases in memory or on disk; supports queries,
 * associations, mirrors, and iteration. Supports *uint32_t* and *string*
 * types. **Auto-index is only active if the key type is `a`.**
 *
 * ## Installation
 *
 * See: <https://github.com/tty-pt/ci/blob/main/docs/install.md#install-ttypt-packages>
 *
 * ## Usage
 *
 * ```
 * corm [-qa ARG] [[-rl] [-RpdgmcD ARG] ...] file[[:k]:v]
 * ```
 *
 * Use `.` as the KEY for all keys. Run `corm -?` to show the full help.
 *
 * ## Options
 *
 *  - `-r`  reverse operation.
 *  - `-l`  list all values.
 *  - `-L`  list "missing" values (**requires `-q`** to specify the paired database).
 *  - `-q` *file[:k[:v]]*  db to use for string lookups and printing.
 *  - `-a` *file[:k[:v]]*  db to use for reversed string lookups and printing.
 *  - `-R` *KEY*  get random value of key (empty key for any).
 *  - `-p` *KEY[:VAL]*  put a key/value pair.
 *  - `-d` *KEY[:VAL]*  delete key/value pair (first match for multivalue maps).
 *  - `-D` *KEY[:VAL]*  delete ALL entries with key (multivalue support).
 *  - `-g` *KEY*  get value(s) of a key (first match for multivalue maps; `.` = all).
 *  - `-m` *KEY*  get ALL values for a key (multivalue iteration).
 *  - `-c` *KEY*  count entries for a key.
 *  - `-x`  when printing associations, bail on the first result.
 *  - `-k`  also print keys (for `-g`, `-m`, and `-R`).
 *  - `-X` *EXPR*  composed set query (runs at `-g .` if present, else once after
 *    all ops); leaves **NAME** [label:**NAME**]; operators `( ) AND OR EXCEPT
 *    NOT` (uppercase); `A EXCEPT B` = set minus; `NOT X` = complement; structure
 *    only — parameters go in flags (`--NAME=VALUE` / `--NAME@LABEL=VALUE`).
 *  - `-t` *N*  cap result count (0 = all).
 *  - `-b` *F*  score floor.
 *  - `--NAME=VALUE`  per-axis config; forwarded to every bound axis plugin that
 *    declares **NAME**.
 *  - `--list-axes`  list loaded axes and exit.
 *  - `-?`  display this message.
 *
 * `-q`/`-a` options are processed in order; each entry extends the lookup chain.
 *
 * ## Type specifiers (`k` and `v`)
 *
 * ```
 * u               uint32_t
 * s               string (default for both key and value)
 * a               key only: uint32_t with auto-index
 * 2<base-type>    key only: multivalue support (enables CM_MULTIVALUE|CM_SORTED)
 * ```
 *
 * ## Notes
 *
 * - `-r` is counter-intuitive: when enabled, lookups are done **by primary keys**.
 * - `-q`/`-a` options are processed in order; each entry extends the lookup chain.
 * - Multivalue maps (type `2<base-type>`) automatically enable
 *   `CM_MULTIVALUE|CM_SORTED` flags.
 * - For multivalue maps with non-string keys, use `-r` to look up by the key type.
 *
 * ## Examples
 *
 * ```
 * # Automatic IDs: key 'a', values as strings
 * corm -p Mathew owners.db:a:s              # -> Mathew's ID
 * corm -p cat    pets.db:a:s                # -> cat's ID
 *
 * # Association (no duplicates)
 * corm -p 1:1 assoc.db:u:u                  # owner_id:pet_id
 *
 * # Get Mathew's pet (use -q/-a to resolve names/IDs)
 * corm -q owners.db:a:s -a pets.db:a:s -g Mathew assoc.db:u:u
 *
 * # Random value for a KEY
 * corm -q owners.db:a:s -a pets.db:a:s -R Mathew assoc.db:u:u
 *
 * # Chained lookups (multiple -q/-a in order)
 * corm -q owners.db:a:s -q pets.db:a:s -g Mathew assoc.db:u:u
 *
 * # Multivalue support - duplicate keys
 * corm -p 100:value1 multi.db:2u:s          # Create multivalue map
 * corm -p 100:value2 multi.db:2u:s          # Add duplicate key
 * corm -p 100:value3 multi.db:2u:s          # Add another
 * corm -r -g 100 multi.db:2u:s              # -> value1 (first match)
 * corm -r -m 100 multi.db:2u:s              # -> value1 value2 value3 (all)
 * corm -r -c 100 multi.db:2u:s              # -> 3 (count)
 * corm -r -d 100 multi.db:2u:s              # Delete first only
 * corm -r -D 100 multi.db:2u:s              # Delete all with key 100
 * ```
 *
 * @see corm_handle
 * @see corm_common
 * @see corm_assoc
 * @see corm_iteration
 * @see corm_type
 */

#include <ttypt/corm.h>
#include <ttypt/idm.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <time.h>
#include <unistd.h>
#include <getopt.h>
#include <libgen.h>
#include <sys/stat.h>
#include <ttypt/qsys.h>
#include <ttypt/rec.h>

#define DEBUG_LVL 0
#define DEBUG(lvl, ...) \
	if (DEBUG_LVL > lvl) WARN(__VA_ARGS__)

#define CM_MAX 1024
#define QDBE_MASK (4096 - 1)   /* 4k buckets: only an initial hint —
				* corm_open auto-grows on overflow (D11) */
#define QDBE_QMASK 0xF
/* CLI-specific flags - use high bits to avoid overlap with QM library flags (0x1F) */
#define QH_RDONLY 0x100
/* Mask for Corm library flags (bits 0-4: values 1,2,4,8,16) */
#define CM_FLAGS_MASK 0x1F

/* Effective hash mask (D11): CORM_MASK env overrides the QDBE_MASK
 * default — every co-open of the same file must derive the same value
 * (corm namespaces records by dbid AND table shape, like the 2B-3
 * fold-plugin truncation bug). The CLI's gen_open, the @roster sidecar,
 * and every bound axis's rec_axis_open (fold/plain/stoma) share this
 * derivation. Valid = nonzero 2^n-1 (corm_open requires mask+1 buckets). */
static uint32_t
corm_mask_effective(void)
{
	const char *e = getenv("CORM_MASK");
	if (e && *e) {
		char *end;
		unsigned long v = strtoul(e, &end, 10);
		if (*end != '\0' || v == 0 || (v & (v + 1)) != 0) {
			fprintf(stderr,
				"corm: invalid CORM_MASK '%s' (need 2^n-1)\n",
				e);
			exit(EXIT_FAILURE);
		}
		return (uint32_t) v;
	}
	return QDBE_MASK;
}

typedef struct {
	uint32_t types[2];
} corme_meta_t;

typedef void corme_print_t(const void *data);

typedef struct {
	corme_print_t *print;
} corme_type_t;

enum corme_mbr {
	KEY,
	VALUE,
};

corme_meta_t metas[CM_MAX];
corme_type_t types[8];

uint32_t QH_NOT_NEW = 0x200;  /* CLI flag - high bit to avoid QM library flag overlap */

uint32_t prim_hd, aux_hd;

const void *value_ptr, *key_ptr;
char *col;

typedef struct {
	uint32_t hd, n;
} aq_t;

enum aq {
	AQ_A,
	AQ_Q,
};

aq_t aqs[2];

uint32_t reverse = 0, bail = 0, print_keys = 0;

/* ── 2B-1 roster + axis-load state (PHASE-2-CLI.md "the @ roster") ──
 * Populated by gen_open from `file@roster:k:v`; consumed at the inter-pass
 * point in main() before pass-2 ops. corm_path is always the PRIMARY file
 * (gen_open is called last for the primary). */
#define CORM_AXIS_ROSTER_MAX 16   /* matches rec_axis_t.name width */
static char corm_path[BUFSIZ];
static char roster_names[REC_QUERY_MAX_AXES][CORM_AXIS_ROSTER_MAX];
static int roster_n;
static int roster_explicit;      /* 1 when this invocation carries an @ list */
static int list_axes;

/* ── 2B-3 expression parser + composed get state ──
 * expr_names[] drives the D9 load-set extension: distinct axis names
 * appearing in -X EXPR, deduped in order of first appearance. */
static const char *expr_str;
static struct expr_node *expr_root;
static char expr_names[REC_QUERY_MAX_AXES][CORM_AXIS_ROSTER_MAX];
static int expr_n;
static size_t top_k;
static float min_score;

enum expr_kind { E_LEAF, E_NOTP, E_AND, E_OR, E_SUB, E_REF };

#define LABEL_MAX 31
#define EXPR_LABEL_CAP 64

struct expr_node {
	enum expr_kind kind;
	const char    *name;       /* E_LEAF: axis name (expr_names[]);
	                            * E_REF: borrowed label from expr_str */
	const char    *value;      /* E_LEAF only; NULL = params "" */
	char           label[LABEL_MAX + 1]; /* E_LEAF: instance label, "" = none */
	struct expr_node *ref;     /* E_REF: the defining E_LEAF node */
	int            heapval;    /* E_LEAF: value malloc'd (freed at end) */
	int            kids_n;
	struct expr_node *kids[REC_QUERY_MAX_AXES + 1];
	rec_set_t     *set;      /* memoized eval (sealed); NULL until eval;
	                          * E_REF borrows the target's set, owns none */
};

#define EXPR_ARENA_CAP 512
static struct expr_node expr_arena[EXPR_ARENA_CAP];
static int expr_used;

static struct expr_node *expr_labels[EXPR_LABEL_CAP];
static int expr_labels_n;

/* `--rank=A`: force the labeled instance A to rank the composed output
 * ("" = auto: first rank-capable axis wins, D2). D15C core override. */
static char rank_override[LABEL_MAX + 1];

/* Parser errors and label lookup live below the lexer; prototypes hoisted
 * for the earlier D15 scoped-flag machinery. */
static void corm_expr_error(const char *fmt, ...);
static struct expr_node *corm_expr_label_find(const char *label);

#define EXPR_VAL_MAX 256
static char expr_val_pool[EXPR_ARENA_CAP][EXPR_VAL_MAX];
static int expr_val_used;

/* First leaf axis with a rank fn (preorder) ranks the composed output;
 * NULL when every leaf is filter-only => pure-filter render. */
static const rec_axis_t *ranker_axis;
static void *ranker_params;

#define CLIP_OPT_LIST_AXES 256
#define CLIP_OPT_TOP       257
#define CLIP_OPT_BOTTOM    258
#define CLIP_OPT_PLUGIN    259
#define CLIP_OPT_RANK      260
static const struct option cli_long_opts[] = {
	{ "list-axes", no_argument,       NULL, CLIP_OPT_LIST_AXES },
	{ "top",       required_argument, NULL, CLIP_OPT_TOP },
	{ "bottom",    required_argument, NULL, CLIP_OPT_BOTTOM },
	{ "rank",      required_argument, NULL, CLIP_OPT_RANK },
	{ NULL, 0, NULL, 0 }
};

/* ── axis-contributed CLI options: corm stays axis-agnostic — each bound
 *    plugin may DECLARE extra long options via rec_axis_cli_options() and
 *    receive their inline `--name=value` tokens via rec_axis_config_arg().
 *    Generic names are broadcast to every bound .so that declares them.
 *    An unknown long option would otherwise hit `case '?'` (usage + exit 0),
 *    so pre-pass-1 we collect `--name[=value]` tokens by mirroring getopt's
 *    consumption of the FIXED core surface and feed BOTH getopt passes a
 *    dynamic table (optional_argument, shared val CLIP_OPT_PLUGIN) — libc-
 *    portable, keeps `-?`/`-Z` semantics byte-identical. Values point into
 *    argv (stable for the whole run). struct rec_axis_cli_option is the
 *    kernel-owned ABI in <ttypt/rec.h> — never re-declared here. ── */
#define CORM_CLI_PLUGIN_CAP 16
#define CORM_CLI_PLUGIN_NAME_MAX 63
#define CORM_CLI_PLUGIN_OPTS_MAX 8
static char corm_cli_pname[CORM_CLI_PLUGIN_CAP][CORM_CLI_PLUGIN_NAME_MAX];
static const char *corm_cli_pval[CORM_CLI_PLUGIN_CAP];
static int corm_cli_pn;
static struct option corm_cli_plugin_opts[3 + 1 + CORM_CLI_PLUGIN_CAP + 1];
static const char *corm_cli_prog;

/* ── scoped flags (D15B): `--name@label` / `--name@axis` are collected by
 *    the generic plugin collector (the name literally contains the '@') and
 *    resolved here, after axes load. is_axis=1 targets bare instances of an
 *    axis name; is_axis=0 targets the labeled instance exactly. Values are
 *    injected into the leaf's synth'd decode spec — never via plugin cfg. */
#define CORM_SCOPED_CAP 64
struct corm_scoped {
	char label[LABEL_MAX + 1];
	char base[CORM_CLI_PLUGIN_NAME_MAX + 1];
	const char *value;
	int is_axis;
};
static struct corm_scoped corm_scoped[CORM_SCOPED_CAP];
static int corm_scoped_n;

/* Mirror getopt_long's consumption of the fixed surface on the UNPERMUTED
 * argv: arg-taking shorts (from optstr "kxla:q:p:d:D:g:m:c:rR:L:X:t:b:?": a q
 * p d D g m R X t b), the three core longs (list-axes no-arg; top/bottom take
 * an arg), "--" termination, and bundle rest-of-token args. Everything else
 * that still looks like `--name[=value]` is a plugin candidate. Order = argv
 * order (duplicates resolve last-wins at flush). */
static void
corm_cli_plugin_collect(int argc, char **argv)
{
	const char *need_arg = "aqpdDgmRXtb";

	for (int i = 1; i < argc; i++) {
		char *a = argv[i];
		if (!strcmp(a, "--"))
			break;
		if (a[0] != '-' || a[1] == '\0')
			continue;   /* positional */
		if (a[1] == '-') {
			char *name = a + 2;
			char *eq = strchr(name, '=');
			size_t nl = eq ? (size_t)(eq - name) : strlen(name);
			if (nl == 0 || nl > CORM_CLI_PLUGIN_NAME_MAX)
				continue;
			if (nl == 9 && !strncmp(name, "list-axes", 9))
				continue;                       /* core long, no arg */
			if ((nl == 3 && !strncmp(name, "top", 3))
					|| (nl == 6 && !strncmp(name, "bottom", 6))
					|| (nl == 4 && !strncmp(name, "rank", 4))) {
				i++;                            /* plus its arg */
				continue;
			}
			if (corm_cli_pn >= CORM_CLI_PLUGIN_CAP) {
				fprintf(stderr, "corm: too many plugin options "
						"(max %d)\n", CORM_CLI_PLUGIN_CAP);
				exit(EXIT_FAILURE);
			}
			char *pname = corm_cli_pname[corm_cli_pn];
			memcpy(pname, name, nl);
			pname[nl] = '\0';
			corm_cli_pval[corm_cli_pn] = eq ? eq + 1 : NULL;
			corm_cli_pn++;
			continue;
		}
		/* short bundle: walk chars; first arg-taking char consumes the
		 * rest of the token (in-token arg) or the next token. */
		int j;
		for (j = 1; a[j]; j++)
			if (strchr(need_arg, a[j]))
				break;
		if (a[j] && a[j + 1] == '\0')
			i++;        /* arg-taking char at end: next token is its arg */
	}
}

static const struct option *
corm_cli_plugin_table(void)
{
	int n = 0;
	for (int i = 0; cli_long_opts[i].name; i++)
		corm_cli_plugin_opts[n++] = cli_long_opts[i];
	for (int i = 0; i < corm_cli_pn; i++)
		corm_cli_plugin_opts[n++] = (struct option){
			.name = corm_cli_pname[i],
			.has_arg = optional_argument,
			.flag = NULL,
			.val = CLIP_OPT_PLUGIN };
	corm_cli_plugin_opts[n++] = (struct option){ NULL, 0, NULL, 0 };
	return corm_cli_plugin_opts;
}

uint32_t corm_get_type;
const void **corm_get_ptr;

void corme_print(uint32_t hd, enum corme_mbr t,
		const void *buf)
{
	corme_meta_t *meta = &metas[hd];
	corme_type_t *type = &types[meta->types[t]];
	type->print(buf);
}

void
usage(char *prog)
{
	fprintf(stderr, "Usage: %s [-qa ARG] [[-rl] [-RpdgmcD ARG] ...] file[[:k]:v]\n", prog);
	fprintf(stderr, "    Options:\n");
	fprintf(stderr, "        -r               reverse operation\n");
	fprintf(stderr, "        -l               list all values\n");
	fprintf(stderr, "        -L               list missing values\n");
	fprintf(stderr, "        -q file[:k[:v]]  db to use string lookups and printing\n");
	fprintf(stderr, "        -a file[:k[:v]]  db to use for reversed string lookups and printing\n");
	fprintf(stderr, "        -R KEY           get random value of key (empty key for any)\n");
	fprintf(stderr, "        -p KEY[:VAL]     put a key/value pair\n");
	fprintf(stderr, "        -d KEY[:VAL]     delete key/value pair (first match for multivalue)\n");
	fprintf(stderr, "        -D KEY[:VAL]     delete ALL entries with key (multivalue support)\n");
	fprintf(stderr, "        -g KEY           get value(s) of a key (first match for multivalue)\n");
	fprintf(stderr, "        -m KEY           get ALL values for a key (multivalue iteration)\n");
	fprintf(stderr, "        -c KEY           count entries for a key\n");
	fprintf(stderr, "        -x               when printing associations, bail on first result\n");
	fprintf(stderr, "        -k               also print keys (for get and rand).\n");
	fprintf(stderr, "        -X EXPR          composed set query (runs at -g . if present,\n");
	fprintf(stderr, "                         else once after all ops)\n");
	fprintf(stderr, "                         leaves NAME [label:NAME]; operators ( ) AND OR\n");
	fprintf(stderr, "                         EXCEPT NOT (uppercase); A EXCEPT B = setminus;\n");
	fprintf(stderr, "                         NOT X = complement; structure only — parameters go in\n");
	fprintf(stderr, "                         flags (--NAME=VALUE / --NAME@LABEL=VALUE)\n");
	fprintf(stderr, "        -t N             cap result count (0 = all)\n");
	fprintf(stderr, "        -b F             score floor\n");
	fprintf(stderr, "        --NAME=VALUE     per-axis config; forwarded to every bound\n");
	fprintf(stderr, "                         axis plugin that declares NAME\n");
	fprintf(stderr, "        --list-axes      list loaded axes and exit\n");
	fprintf(stderr, "    'k' and 'v' are key and value types. Supported values:\n");
	fprintf(stderr, "         u               uint32_t\n");
	fprintf(stderr, "         s               string (default for both key and value)\n");
	fprintf(stderr, "         a               key only! uint32_t automatic index\n");
	fprintf(stderr, "         2<base-type>    key only! multivalue support (enables CM_MULTIVALUE|CM_SORTED)\n");
	fprintf(stderr, "\n");
	fprintf(stderr, "Use '.' as the KEY for all keys!\n");
	fprintf(stderr, "-q/-a options are processed in order.\n");
}

static inline uint32_t
corme_type(uint32_t phd, enum corme_mbr t, uint32_t reverse)
{
	corme_meta_t *meta = &metas[phd];
	return meta->types[reverse ? !t : t];
}

static inline const void *rec_query(
		enum aq aq,
		const void *tbuf,
		const void *buf,
		uint32_t tmprev)
{
	tmprev = (aqs[AQ_Q].n & 1) == tmprev;
	uint32_t c2 = corm_iter(aqs[aq].hd, NULL, 0);
	ids_t rqs = ids_init();
	uint32_t aux_hd;
	const void *key, *value, *aux2;

	uint32_t lktype = corme_type(prim_hd,
			KEY, !reverse);

	while (corm_next(&key, &value, c2)) {
		aux_hd = * (uint32_t *) value;

		if (corme_type(aux_hd, VALUE, tmprev)
				!= lktype)
			tmprev = !tmprev;

		if (corme_type(aux_hd, VALUE, tmprev)
			       != lktype)
		{
			// TODO free idml
			ids_drop(&rqs);
			fprintf(stderr, "Invalid query sequence\n");
			corm_fin(c2);
			return NULL;
		}

		lktype = corme_type(aux_hd, KEY, tmprev);
		ids_push(&rqs, aux_hd + tmprev);
	}

	while ((aux_hd = ids_pop(&rqs)) != (uint32_t) -1) {
		value = corm_get(aux_hd, buf);
		if (!value) {
			ids_drop(&rqs);
			return NULL;
		}
		tbuf = value;
		aux2 = buf;
		buf = tbuf;
		tbuf = aux2;
	};

	ids_drop(&rqs);
	return buf;
}

static inline int gen_cond(int is_value) {
	uint32_t aq_hd = aqs[AQ_Q].hd;
	uint32_t c = corm_iter(aq_hd, NULL, 0);
	uint32_t rev = !reverse;
	uint32_t type = corme_type(prim_hd,
			is_value ? KEY : VALUE,
			rev);
	const void *key, *value;

	while (corm_next(&key, &value, c)) {
		rev = !rev;
		uint32_t aux_hd = * (uint32_t *)
			value;
		type = corme_type(aux_hd, KEY, rev);
	}

	return type == CM_STR;
}

inline static const void *
_gen_lookup(const void **buf, uint32_t *uret, char *str,
		enum aq aq, int is_value)
{
	uint32_t cond = gen_cond(is_value);
	const void *ret = NULL;

	if (cond)
		*buf = str;
	else {
		*uret = strtoul(str, NULL, 10);
		*buf = uret;
		/* fprintf(stderr, "_gen_lookup 2 %u\n", ret); */
	}

	if (aqs[aq].n)
		ret = rec_query(aq, *buf, *buf, !reverse);
	else
		ret = *buf;

	return ret;
}

static const void *gen_lookup(char *str) {
	static uint32_t aret, bret;

	if (!str || !strcmp(str, "."))
		return NULL;

	col = strchr(str, ':');
	if (col) {
		*col = '\0';
		col++;
		_gen_lookup(&value_ptr, &aret, col, AQ_A, 1);
		key_ptr = str;
		return _gen_lookup(&key_ptr, &bret, str,
				AQ_Q, 0);
	}

	return _gen_lookup(&value_ptr, &bret, str,
			AQ_Q, 1);
}

/* 2B-4 write fan-out (defined after corm_axes_list, below). */
static int corm_write_ref(const char *operand, uint32_t *ref);
static int corm_fanout_store(uint32_t ref, const void *payload, size_t len,
		uint32_t qtype);
static int corm_fanout_unstore(uint32_t ref);

static inline int gen_del(void) {
	uint32_t ref;
	int composed = roster_n > 0
		&& metas[prim_hd].types[0] == CM_HNDL;
	/* Resolve before gen_lookup (it truncates the operand). */
	int have_ref = composed && corm_write_ref(optarg, &ref);

	gen_lookup(optarg);
	if (have_ref) {
		corm_del(prim_hd, &ref);
		return corm_fanout_unstore(ref)
			? EXIT_FAILURE : EXIT_SUCCESS;
	}
	if (composed) {
		/* Axis writes need a ref (ref-type law); the legacy op would
		 * silently no-op. Reads degrade, writes fail. */
		fprintf(stderr, "corm: write ref '%s': no primary record\n",
				optarg);
		return EXIT_FAILURE;
	}
	corm_del(prim_hd + !reverse, value_ptr);
	return EXIT_SUCCESS;
}

static inline int assoc_exists(const char *key_ptr) {
	if (!aqs[AQ_A].n)
		return 1;

	return !!rec_query(AQ_A, key_ptr,
			key_ptr, 1);
}

static inline void assoc_print(void) {
	const void *alt_ptr;
	const void *buf = reverse ? value_ptr : key_ptr;
	uint32_t aux_hd;
	uint32_t aq_hd = aqs[AQ_A].hd;
	uint32_t c2 = corm_iter(aq_hd, NULL, 0);
	const void *key, *value;

	while (corm_next(&key, &value, c2)) {
		aux_hd = * (uint32_t *)
			value;
		putchar(' ');

		alt_ptr = corm_get(aux_hd, buf);

		if (!alt_ptr) {
			printf("-1");
			continue;
		}

		corme_print(aux_hd, VALUE, alt_ptr);

		if (bail)
			break;
	}
}

static inline void _gen_get(void) {
	if (print_keys) {
		corme_print(prim_hd, KEY, key_ptr);
		putchar(' ');
		corme_print(prim_hd, VALUE,
				value_ptr);
	} else {
		corme_print(prim_hd, corm_get_type,
				*corm_get_ptr);
	}
	assoc_print();
	printf("\n");
}

static inline void gen_rand(void) {
	uint32_t count = 0, randn;
	uint32_t c;
	const void *iter_key = gen_lookup(optarg);

	// why iter by key, if we don't support dupes?
	// should we just get? spoilers: we use the key - or not.

	c = corm_iter(prim_hd + !reverse, iter_key, 0);

	while (corm_next(&key_ptr, &value_ptr, c))
		if (assoc_exists(key_ptr))
			count ++;

	if (count == 0) {
		printf("-1\n");
		return;
	}

	randn = rand() % count;
	c = corm_iter(prim_hd + !reverse, iter_key, 0);

	while (corm_next(&key_ptr, &value_ptr, c))
		if (!assoc_exists(key_ptr))
			continue;
		else if ((--count) <= randn) {
			corm_fin(c);
			break;
		}

	_gen_get();
}

static void gen_get(char *str) {
	const void *iter_key = gen_lookup(str);
	uint32_t c;
	uint32_t nonce = 1, hd;
	const void *key;

	if (reverse) {
		corm_get_ptr = &value_ptr;
		corm_get_type = VALUE;
	} else {
		corm_get_ptr = &key_ptr;
		corm_get_type = KEY;
	}

	if (str && strcmp(str, ".") && !iter_key) {
		printf("-1\n");
		return;
	}

	hd = prim_hd + !reverse;
	c = corm_iter(hd, iter_key, 0);

	while (corm_next(&key, &value_ptr, c)) {
		if (reverse)
			key_ptr = key;
		else {
			key_ptr = value_ptr;
			value_ptr = key;
		}

		if (assoc_exists(key_ptr)) {
			_gen_get();
			nonce = 0;
		}
	}

	if (nonce)
		printf("-1\n");
}

static void gen_list(void) {
	uint32_t c;
	uint32_t cond, aux;

	gen_lookup(NULL);
	cond = gen_cond(1);

	c = corm_iter(prim_hd, NULL, 0);

	corm_get_type = VALUE;
	corm_get_ptr = &key_ptr;
	aux = print_keys;
	print_keys = 1;

	while (corm_next(&key_ptr, &value_ptr, c)) {
		rec_query(AQ_Q, key_ptr, value_ptr, !cond);
		_gen_get();
	}

	print_keys = aux;
}

static inline void gen_multi(void) {
	const void *iter_key = gen_lookup(optarg);
	uint32_t c;

	if (!iter_key) {
		printf("-1\n");
		return;
	}

	c = corm_get_multi(prim_hd + !reverse, iter_key);
	
	if (c == CM_MISS) {
		printf("-1\n");
		return;
	}

	while (corm_next(&key_ptr, &value_ptr, c)) {
		if (reverse)
			key_ptr = value_ptr;
		
		if (print_keys) {
			corme_print(prim_hd, KEY, key_ptr);
			putchar(' ');
		}
		corme_print(prim_hd, VALUE, value_ptr);
		putchar('\n');
	}
}

static inline void gen_count(void) {
	const void *iter_key = gen_lookup(optarg);
	size_t count;

	if (!iter_key) {
		printf("-1\n");
		return;
	}

	count = corm_count(prim_hd + !reverse, iter_key);
	printf("%zu\n", count);
}

static inline int gen_del_all(void) {
	uint32_t ref;
	int composed = roster_n > 0
		&& metas[prim_hd].types[0] == CM_HNDL;
	int have_ref = composed && corm_write_ref(optarg, &ref);

	gen_lookup(optarg);
	if (have_ref) {
		/* -d/-D collapse on axes: both mean unstore-all. */
		corm_del_all(prim_hd, &ref);
		return corm_fanout_unstore(ref)
			? EXIT_FAILURE : EXIT_SUCCESS;
	}
	if (composed) {
		fprintf(stderr, "corm: write ref '%s': no primary record\n",
				optarg);
		return EXIT_FAILURE;
	}
	corm_del_all(prim_hd + !reverse, value_ptr);
	return EXIT_SUCCESS;
}

static inline int gen_put(void) {
	uint32_t id;
	const void *key;

	gen_lookup(optarg);

	key = col ? key_ptr : NULL;
	id = corm_put(prim_hd, key, value_ptr);
	corme_print(prim_hd, KEY,
			key ? key : (char *) &id);
	putchar('\n');

	if (roster_n > 0) {
		/* Composed write: the whole payload fans out as
		 * (ref, stored-payload) to every @-roster target. */
		if (metas[prim_hd].types[0] != CM_HNDL) {
			fprintf(stderr,
				"corm: axis fan-out needs an :a:-type primary "
				"(refs must exist); primary written, axes "
				"skipped\n");
			return EXIT_FAILURE;
		}
		uint32_t ref;
		uint32_t vtype = metas[prim_hd].types[1];
		const void *payload;
		size_t len;

		if (key)
			memcpy(&ref, key, sizeof(ref));
		else
			ref = id;
		payload = corm_get(prim_hd, &ref);
		if (!payload)
			payload = value_ptr;  /* string fallback */
		len = corm_type_len(vtype);
		if (corm_fanout_store(ref, payload, len, vtype) != 0)
			return EXIT_FAILURE;
	}
	return EXIT_SUCCESS;
}

static inline void gen_list_missing(void) {
	uint32_t c;
	gen_lookup(NULL);

	if (!aqs[AQ_Q].n) {
		fprintf(stderr, "list missing needs a corresponding database\n");
		return;
	}

	c = corm_iter(prim_hd + !reverse, NULL, 0);
	while (corm_next(&key_ptr, &value_ptr, c)) {
		uint32_t aqs_hd = aqs[AQ_Q].hd;
		uint32_t c2 = corm_iter(aqs_hd, NULL, 0);
		const void *skey, *sval;

		while (corm_next(&skey, &sval, c2)) {
			uint32_t ahd = (* (uint32_t *)
				sval)
				+ !reverse;
			key_ptr = corm_get(ahd, value_ptr);
			if (key_ptr)
				_gen_get();
		}
	}
}

uint32_t _corme_type(char *which) {
	if (*which == '2' && which[1] != '\0')
		which++;
	if (*which == 's')
		return CM_STR;
	if (*which == 'u')
		return CM_U32;
	fprintf(stderr, "Invalid type specifier: %s\n", which);
	exit(EXIT_FAILURE);
}

/* Split a roster csv ("stub,zed") into roster_names[] (order kept).
 * Resets roster_n first; returns the number of names parsed. */
static int
roster_csv_parse(const char *csv)
{
	roster_n = 0;
	if (!csv || !*csv)
		return 0;

	char copy[BUFSIZ];
	snprintf(copy, sizeof(copy), "%s", csv);
	char *save = NULL;
	for (char *tok = strtok_r(copy, ",", &save); tok;
			tok = strtok_r(NULL, ",", &save)) {
		if (!*tok)
			continue;
		if (roster_n >= REC_QUERY_MAX_AXES) {
			fprintf(stderr, "corm: too many roster axes (max %d)\n",
					REC_QUERY_MAX_AXES);
			exit(EXIT_FAILURE);
		}
		snprintf(roster_names[roster_n], CORM_AXIS_ROSTER_MAX, "%s", tok);
		roster_n++;
	}
	return roster_n;
}

/* Recompose roster_names[] as a csv into out. */
static void
roster_csv_join(char *out, size_t cap)
{
	size_t n = 0;
	out[0] = '\0';
	for (int i = 0; i < roster_n; i++)
		n += snprintf(out + n, cap > n ? cap - n : 0,
				"%s%s", i ? "," : "", roster_names[i]);
}

uint32_t gen_open(char *fname, uint32_t flags) {
	char buf[BUFSIZ];
	uint32_t ktype = CM_STR;
	uint32_t vtype = CM_STR;
	uint32_t hd;

	int written = snprintf(buf, sizeof(buf), "%s", fname);
	if (written < 0 || (size_t) written >= sizeof(buf)) {
		fprintf(stderr, "Filename too long\n");
		exit(EXIT_FAILURE);
	}

	/* `file@roster:k:v` — the first `@` splits the roster (csv of axis
	 * names) from the primary spec; the `:k:v` types still describe the
	 * primary map. A literal `@` in a filename needs quoting — same class
	 * of footgun as `:` (PHASE-2-CLI.md "the @ roster"). */
	char *at = strchr(buf, '@');
	char *frag = buf;
	if (at) {
		*at = '\0';
		frag = at + 1;
	}

	char *first_col = strchr(frag, ':'), *second_col;

	if (first_col) {
		*first_col = '\0';
		first_col++;
		second_col = strchr(first_col, ':');

		if (second_col) {
			*second_col = '\0';
			second_col++;
			vtype = _corme_type(second_col);
		}

	if (!strcmp(first_col, "a")) {
		flags |= CM_AINDEX;
		ktype = CM_HNDL;
	} else if (*first_col == '2') {
		flags |= CM_MULTIVALUE | CM_SORTED;
		first_col++;
		if (*first_col)
			ktype = _corme_type(first_col);
	} else
		ktype = _corme_type(first_col);
	}

	/* Roster csv = `frag` up to the first `:` (truncated above).
	 * A non-empty csv makes this an explicit-@ override invocation. */
	roster_explicit = 0;
	if (at) {
		roster_csv_parse(frag);
		roster_explicit = roster_n > 0;
	} else {
		roster_n = 0;
	}

	snprintf(corm_path, sizeof(corm_path), "%s", buf);

	/* Mask out CLI-specific flags before passing to library */
	hd = corm_open(buf, "hd", ktype, vtype,
			corm_mask_effective(),
			(flags & CM_FLAGS_MASK) | CM_MIRROR);
	metas[hd].types[0] = ktype;
	metas[hd].types[1] = vtype;
	metas[hd + 1].types[0] = vtype;
	metas[hd + 1].types[1] = ktype;
	return hd;
}

/* ========================================================================
 * 2B-1 roster + axis discovery/load — by-name, no per-call load/open
 * flags (D9).
 * Inter-pass (after primary open, before pass-2 ops): reconcile the
 * explicit @ roster / stored sidecar roster, load the load set
 * (@ or stored ∪ CORM_AXIS_LIBS), and bind each axis's ctx via its
 * rec_axis_open on the alongside-default spec <primary-dir>/<primary>-<name>
 * (per-primary, 7-AXIS-NAMESPACE-PLAN.md).
 * ======================================================================== */

/* One dlopen'd plugin: the inclusive registry slot range it contributed,
 * so a name→slot resolve can recover the .so handle for rec_axis_open. */
static struct corm_loaded {
	void *h;
	int lo, hi;
} corm_loaded_slots[REC_QUERY_MAX_AXES];
static int corm_loaded_n;

static void
corm_axes_dlopen_record(const char *path, int quiet)
{
	int before = rec_axis_count();
	void *h = qsys_dlopen(path, 0);
	if (!h) {
		if (!quiet) {
			const char *err = qsys_dlerror();
			fprintf(stderr, "corm: %s: %s\n",
					path, err ? err : "unknown error");
		}
		return;
	}
	int after = rec_axis_count();
	if (after == before)
		return;
	if (corm_loaded_n >= REC_QUERY_MAX_AXES)
		return;
	corm_loaded_slots[corm_loaded_n++] =
			(struct corm_loaded){ h, before, after - 1 };
}

static int
corm_axes_find_slot(const char *name)
{
	int n = rec_axis_count();
	for (int slot = 0; slot < n; slot++) {
		const rec_axis_t *axis = rec_axis_get(slot);
		if (axis && !strcmp(axis->name, name))
			return slot;
	}
	return -1;
}

static void *
corm_axes_handle_for_slot(int slot)
{
	for (int i = 0; i < corm_loaded_n; i++)
		if (slot >= corm_loaded_slots[i].lo
				&& slot <= corm_loaded_slots[i].hi)
			return corm_loaded_slots[i].h;
	return NULL;
}

static int
corm_axes_find_lib(const char *name, char *out, size_t cap)
{
	const char *path_env = getenv("CORM_AXIS_PATH");
	char dirs[BUFSIZ];
	if (path_env && *path_env)
		snprintf(dirs, sizeof(dirs), "%s", path_env);
	else
		snprintf(dirs, sizeof(dirs), "/usr/lib");

	char *save = NULL;
	for (char *dir = strtok_r(dirs, ":", &save); dir;
			dir = strtok_r(NULL, ":", &save)) {
		snprintf(out, cap, "%s/lib%s.so", dir, name);
		struct stat lst;
		if (stat(out, &lst) == 0)
			return 1;
	}
	return 0;
}

/* Per-primary alongside-default spec (7-AXIS-NAMESPACE-PLAN.md A-1): each
 * axis store is keyed to the primary that owns it — <dir>/<basename>-<name>
 * (e.g. "data/garden.db" → "data/garden.db-sepal"), mirroring the
 * <primary>.roster sidecar's per-primary shape. Two databases sharing a
 * directory therefore never share axis state. */
static void
corm_axes_spec(const char *name, char *out, size_t cap)
{
	char dirbuf[BUFSIZ];
	char basebuf[BUFSIZ];
	char *sl;
	snprintf(dirbuf, sizeof(dirbuf), "%s", corm_path);
	snprintf(basebuf, sizeof(basebuf), "%s", corm_path);
	sl = strrchr(basebuf, '/');
	snprintf(out, cap, "%s/%s-%s",
			dirname(dirbuf), sl ? sl + 1 : basebuf, name);
}

/* ── 2B-4 write-fan-out state: per-slot store capability (dlsym'd at
 *    bind time, PHASE-2-CLI.md D1/D12). An axis whose .so misses the store
 *    symbols is read-only: skipped on writes and reported as a partial.
 *    readback is recorded for completeness (no CLI surface — postponed;
 *    query + write cover the mm flow). ── */
struct corm_axis_fx {
	int bound;
	int has_store;      /* rec_axis_store (string whole-payload) */
	int has_typed;      /* rec_axis_store_typed (binary blob,len,qtype) */
	int has_unstore;    /* rec_axis_unstore (idempotent forget) */
	int has_readback;   /* rec_axis_readback (entries-of-ref) */
	int (*store_fn)(void *, const char *, rec_ref_t, const char *);
	int (*typed_fn)(void *, const char *, rec_ref_t, const void *,
			size_t, uint32_t);
	int (*unstore_fn)(void *, rec_ref_t);
	int (*readback_fn)(void *, rec_ref_t, char **, size_t *);
};
static struct corm_axis_fx corm_axis_fx[REC_QUERY_MAX_AXES];

/* Resolve one optional conventional symbol into fn (memcpy keeps the
 * void*→fn-pointer conversion clean on strict-aliasing builds). */
static void
corm_axes_sym(void *h, const char *symname, void *fn, size_t fnsz, int *have)
{
	void *sym = qsys_dlsym(h, symname);
	*have = 0;
	if (!sym)
		return;
	memcpy(fn, &sym, fnsz);
	*have = 1;
}

static void
corm_axes_bind(int slot, const char *name)
{
	void *h = corm_axes_handle_for_slot(slot);
	char spec[BUFSIZ];
	corm_axes_spec(name, spec, sizeof(spec));

	if (!h) {
		fprintf(stderr,
			"corm: axis '%s': no dlopen handle, slot %d left ctx-less\n",
			name, slot);
		return;
	}
	void *sym = qsys_dlsym(h, "rec_axis_open");
	if (!sym) {
		fprintf(stderr,
			"corm: axis '%s': plugin has no rec_axis_open symbol, "
			"slot %d left ctx-less\n", name, slot);
		return;
	}
	void *(*open_fn)(const char *);
	memcpy(&open_fn, &sym, sizeof(open_fn));
	void *ctx = open_fn(spec);
	rec_axis_set_ctx(slot, ctx);

	/* Conventional env-config hook (D8, 2B-2): after binding, invoke the
	 * plugin's optional `rec_axis_env_config` — same dlsym-by-convention
	 * pattern as `rec_axis_open`. Each axis implements whatever config
	 * it needs from its OWN environment; the CLI stays axis-agnostic
	 * (libsepal reads CORM_SEPAL_EMBED_URL/MODEL/KEY). Missing symbol =
	 * no-op for that axis. */
	void *csym = qsys_dlsym(h, "rec_axis_env_config");
	if (csym) {
		int (*cfg_fn)(void);
		memcpy(&cfg_fn, &csym, sizeof(cfg_fn));
		cfg_fn();
	}

	/* 2B-4 write-half capabilities (D1/D12): dlsym the store/unstore/
	 * readback convention the same way. Missing store symbols ⇒ the
	 * axis is read-only (skipped + reported at write time). */
	if (slot >= 0 && slot < REC_QUERY_MAX_AXES) {
		struct corm_axis_fx *fx = &corm_axis_fx[slot];
		corm_axes_sym(h, "rec_axis_store", &fx->store_fn,
				sizeof(fx->store_fn), &fx->has_store);
		corm_axes_sym(h, "rec_axis_store_typed", &fx->typed_fn,
				sizeof(fx->typed_fn), &fx->has_typed);
		corm_axes_sym(h, "rec_axis_unstore", &fx->unstore_fn,
				sizeof(fx->unstore_fn), &fx->has_unstore);
		corm_axes_sym(h, "rec_axis_readback", &fx->readback_fn,
				sizeof(fx->readback_fn), &fx->has_readback);
		fx->bound = 1;
	}
}

/* Extracted CORM_AXIS_LIBS dlopen pass (D9): also used by standalone
 * --list-axes before any primary is opened. */
static void
corm_axes_dlopen_env(int quiet)
{
	const char *env_libs = getenv("CORM_AXIS_LIBS");
	if (!env_libs || !*env_libs)
		return;
	char *copy = strdup(env_libs);
	if (!copy) {
		fprintf(stderr, "corm: out of memory\n");
		exit(EXIT_FAILURE);
	}
	char *save = NULL;
	for (char *tok = strtok_r(copy, ":", &save); tok;
			tok = strtok_r(NULL, ":", &save))
		corm_axes_dlopen_record(tok, quiet);
	free(copy);
}

/* Post-bind broadcast (D14): after every axis is connected, deliver each
 * collected plugin option to every bound .so that DECLARES it (self-
 * describing via rec_axis_cli_options). Runs before pass-2 eval so an
 * axis's decode/fill merges the CLI config. An axis declaring nothing is
 * untouched; an option no bound .so declares is a hard error. */
static void corm_apply_scoped_flags(void);

/* One dlopen'd plugin's D14 surface (dedupe by handle upstream). */
struct corm_cli_plug {
	void *h;
	const rec_axis_cli_option_t *opts;
	int (*cfg)(const char *, const char *);
};

/* Find base across all plugin option tables (the broadcast loop and the
 * D15B declared lookup share it). *has_arg is set on a hit. */
static const rec_axis_cli_option_t *
corm_cli_opt_find(const struct corm_cli_plug *plugs, int nplug,
		  const char *name, int *has_arg)
{
	for (int p = 0; p < nplug; p++) {
		const rec_axis_cli_option_t *o = plugs[p].opts;

		for (int k = 0; o[k].name && k < CORM_CLI_PLUGIN_OPTS_MAX; k++) {
			if (strcmp(o[k].name, name))
				continue;
			if (has_arg)
				*has_arg = o[k].has_arg;
			return &o[k];
		}
	}
	return NULL;
}

/* One-line error paths for the plugin-flag passes. Every message keeps its
 * exact historic text (the shell suites grep stderr); corm_cli_err adds
 * usage + exit(1), corm_cli_fail exits bare. */
static void
vcorm_cli_out(int show_usage, const char *fmt, va_list ap)
{
	fprintf(stderr, "corm: ");
	vfprintf(stderr, fmt, ap);
	fputc('\n', stderr);
	if (show_usage)
		usage((char *)corm_cli_prog);
	exit(EXIT_FAILURE);
}

static void
corm_cli_err(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vcorm_cli_out(1, fmt, ap);
	va_end(ap);
}

static void
corm_cli_fail(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vcorm_cli_out(0, fmt, ap);
	va_end(ap);
}

static void
corm_cli_plugin_flush(void)
{
	struct corm_cli_plug plugs[CORM_CLI_PLUGIN_CAP];
	int nplug = 0;

	if (corm_cli_pn == 0)
		return;

	/* Dedupe by handle: one .so may register several axes. */
	for (int i = 0; i < corm_loaded_n && nplug < CORM_CLI_PLUGIN_CAP; i++) {
		void *h = corm_loaded_slots[i].h;
		int dup = 0;
		for (int j = 0; j < nplug; j++)
			if (plugs[j].h == h) { dup = 1; break; }
		if (dup)
			continue;
		void *osym = qsys_dlsym(h, "rec_axis_cli_options");
		if (!osym)
			continue;
		const rec_axis_cli_option_t *(*opts_fn)(void);
		memcpy(&opts_fn, &osym, sizeof(opts_fn));
		void *cfg_sym = qsys_dlsym(h, "rec_axis_config_arg");
		if (!cfg_sym)
			continue;
		int (*cfg)(const char *, const char *);
		memcpy(&cfg, &cfg_sym, sizeof(cfg));
		plugs[nplug].h = h;
		plugs[nplug].opts = opts_fn();
		plugs[nplug].cfg = cfg;
		nplug++;
	}

	for (int c = 0; c < corm_cli_pn; c++) {
		const char *name = corm_cli_pname[c];
		const char *value = corm_cli_pval[c];
		int has_arg = 0;

		if (strchr(name, '@'))
			continue;   /* scoped: resolved in the D15B pass below */
		if (!corm_cli_opt_find(plugs, nplug, name, &has_arg))
			corm_cli_err("unknown option '--%s'", name);
		if (has_arg && !value)
			corm_cli_err("option '--%s' requires --%s=VALUE",
				     name, name);
		if (!has_arg && value)
			corm_cli_err("option '--%s' takes no value", name);
		for (int p = 0; p < nplug; p++) {
			const rec_axis_cli_option_t *o = plugs[p].opts;

			for (int k = 0; o[k].name
					&& k < CORM_CLI_PLUGIN_OPTS_MAX; k++) {
				if (strcmp(o[k].name, name))
					continue;
				if (plugs[p].cfg(name, value) != 0)
					corm_cli_err(
						"option '--%s' rejected by axis plugin",
						name);
			}
		}
	}

	/* D15B: resolve the collected scoped tokens. ── */
	for (int c = 0; c < corm_cli_pn; c++) {
		const char *name = corm_cli_pname[c];
		const char *value = corm_cli_pval[c];
		const char *at = strchr(name, '@');

		if (!at)
			continue;   /* plain broadcast handled above */

		size_t blen = (size_t)(at - name);
		const char *scope = at + 1;
		if (blen == 0 || !*scope)
			corm_cli_err("unknown option '--%s'", name);

		if (blen == 4 && !strncmp(name, "rank", 4)) {
			/* Core override: --rank@A forces instance A to rank. */
			if (!corm_expr_label_find(scope))
				corm_cli_err("--rank@%s: unknown label '%s'",
					     scope, scope);
			if (strlen(scope) > LABEL_MAX)
				corm_cli_fail("--rank@%s: label too long", scope);
			strcpy(rank_override, scope);
			continue;
		}

		/* base must be a real plugin option taking a value. */
		char base[CORM_CLI_PLUGIN_NAME_MAX + 1];
		if (blen > CORM_CLI_PLUGIN_NAME_MAX)
			corm_cli_err("unknown option '--%s'", name);
		memcpy(base, name, blen);
		base[blen] = '\0';
		int has_arg = 0;
		if (!corm_cli_opt_find(plugs, nplug, base, &has_arg))
			corm_cli_err("unknown option '--%s'", name);
		if (!has_arg)
			corm_cli_err("option '--%s' takes no value", name);
		if (!value)
			corm_cli_err("option '--%s' requires --%s=VALUE",
				     name, name);
		if (corm_scoped_n >= CORM_SCOPED_CAP)
			corm_cli_fail("too many scoped options (max %d)",
				      CORM_SCOPED_CAP);
		struct corm_scoped *sc = &corm_scoped[corm_scoped_n];
		sc->is_axis = 0;
		if (corm_expr_label_find(scope))
			;                       /* exact labeled instance */
		else if (corm_axes_find_slot(scope) >= 0)
			sc->is_axis = 1;       /* every bare instance of the axis */
		else
			corm_cli_err("unknown label or axis '%s'", scope);
		if (strlen(scope) > LABEL_MAX)
			corm_cli_fail("--%s: scope '%s' too long", base, scope);
		strcpy(sc->label, scope);
		strcpy(sc->base, base);
		sc->value = value;
		corm_scoped_n++;
	}

	corm_apply_scoped_flags();
}

/* Length of `v` after single-quoting, escaping '\' and '''. */
static size_t
corm_leaf_quote_len(const char *v)
{
	size_t n = 2;                       /* opening + closing quote */
	for (const char *p = v; *p; p++)
		n += (*p == '\\' || *p == '\'') ? 2 : 1;
	return n;
}

static int
corm_leaf_needs_quote(const char *v)
{
	for (const char *p = v; *p; p++)
		if (*p == ' ' || *p == '\t' || *p == '\'' || *p == '"'
				|| *p == '(' || *p == ')')
			return 1;
	return 0;
}

/* D15B: synthesize each scoped pair into the labeled/bare leaf's decode
 * spec so the per-instance parameters reach decode() with zero plugin
 * gains. Runs after axes bind; the tree and the label table are both live.
 */
static void
corm_apply_scoped_flags(void)
{
	if (corm_scoped_n == 0)
		return;
	for (int i = 0; i < expr_used; i++) {
		struct expr_node *n = &expr_arena[i];
		if (n->kind != E_LEAF)
			continue;
		for (int s = 0; s < corm_scoped_n; s++) {
			const struct corm_scoped *sc = &corm_scoped[s];
			int match;
			/* Per-key arbitration on double scope: @label beats @axis.
			 * The collector resolves each name to a single scope (label
			 * lookup first, then axis bare-name), so a leaf targeted by
			 * both forms is only ever matched here once — the label
			 * scope wins by construction. */
			if (sc->is_axis)
				match = n->label[0] == 0
					&& !strcmp(n->name, sc->label);
			else
				match = !strcmp(n->label, sc->label);
			if (!match)
				continue;
			const char *old = n->value ? n->value : "";
			size_t oldlen = strlen(old);
			size_t vlen = strlen(sc->value);
			size_t baselen = strlen(sc->base);
			int needs_q = corm_leaf_needs_quote(sc->value);
			size_t qlen = needs_q ? corm_leaf_quote_len(sc->value) : vlen;
			size_t total = oldlen + (oldlen > 0 ? 1 : 0)
				+ baselen + 1 + qlen + 1;
			char *dst;
			if (total < EXPR_VAL_MAX && expr_val_used < EXPR_ARENA_CAP) {
				dst = expr_val_pool[expr_val_used++];
				n->heapval = 0;
			} else {
				dst = malloc(total);
				if (!dst)
					corm_expr_error("out of memory");
				n->heapval = 1;
			}
			char *o = dst;
			memcpy(o, old, oldlen);
			o += oldlen;
			if (oldlen > 0)
				*o++ = ' ';
			memcpy(o, sc->base, baselen);
			o += baselen;
			*o++ = '=';
			if (needs_q) {
				*o++ = '\'';
				for (const char *p = sc->value; *p; p++) {
					if (*p == '\\' || *p == '\'')
						*o++ = '\\';
					*o++ = *p;
				}
				*o++ = '\'';
			} else {
				memcpy(o, sc->value, vlen);
				o += vlen;
			}
			*o = '\0';
			n->value = dst;
		}
	}
}

/* Inter-pass roster handling + axis load/bind. PHASE-2-CLI.md "the @
 * roster" / 2B-1: write-if-absent, override-once (D7), alongside-
 * heuristic, load set = explicit @ (or stored roster) ∪ CORM_AXIS_LIBS
 * ∪ -X expr names (D9: by-name discovery needs no @). */
static void
corm_axes_setup(void)
{
	char sidecar[BUFSIZ + 16];

	/* Per-primary binding (7-AXIS-NAMESPACE-PLAN.md A-2): publish the
	 * primary path verbatim before any axis binds. A derived axis (stoma)
	 * rebuilds from EXACTLY this primary — the live handle, never a
	 * directory-guessed roster — so two DBs in one dir can't cross-wire. */
qsys_setenv("CORM_AXIS_PRIMARY", corm_path, 1);

	snprintf(sidecar, sizeof(sidecar), "%s.roster", corm_path);
	struct stat st;
	int have_sidecar = stat(sidecar, &st) == 0;

	if (roster_explicit) {
		if (!have_sidecar) {
			/* Write-if-absent: persist the newly declared roster. */
			uint32_t rs = corm_open(sidecar, "@roster",
					CM_STR, CM_STR, corm_mask_effective(), 0);
			char csv[BUFSIZ];
			roster_csv_join(csv, sizeof(csv));
			corm_put(rs, "v", "1");
			corm_put(rs, "axes", csv);

			/* Alongside-heuristic: axis stores exist but no roster
			 * yet — loud hint, exit 0. */
			for (int i = 0; i < roster_n; i++) {
				char spec[BUFSIZ];
				corm_axes_spec(roster_names[i], spec, sizeof(spec));
				if (stat(spec, &st) == 0) {
					fprintf(stderr,
						"corm: axis store found (%s) beside %s but no "
						"roster yet; re-specifying @ persists it\n",
						spec, corm_path);
					break;
				}
			}
		}
		/* Override-once (D7): explicit @ replaces the stored roster for
		 * this invocation only — the sidecar is never written here. */
	} else if (have_sidecar) {
		uint32_t rs = corm_open(sidecar, "@roster",
				CM_STR, CM_STR, corm_mask_effective(), 0);
		const void *axes = corm_get(rs, "axes");
		if (axes)
			roster_csv_parse(axes);
	}

	/* Load set: CORM_AXIS_LIBS first (their axes then resolve by name
	 * with no matching lib<name>.so), then each roster name. */
	corm_axes_dlopen_env(1);

	for (int i = 0; i < roster_n; i++) {
		const char *name = roster_names[i];
		int slot = corm_axes_find_slot(name);
		if (slot < 0) {
			char path[BUFSIZ];
			if (!corm_axes_find_lib(name, path, sizeof(path))) {
				fprintf(stderr,
					"corm: unknown axis name '%s' (no lib%s.so in "
					"CORM_AXIS_PATH)\n", name, name);
				exit(EXIT_FAILURE);
			}
			corm_axes_dlopen_record(path, 0);
			slot = corm_axes_find_slot(name);
			if (slot < 0) {
				fprintf(stderr,
					"corm: plugin %s registered no axis named '%s'\n",
					path, name);
				exit(EXIT_FAILURE);
			}
		}
		corm_axes_bind(slot, name);
	}

	/* D9: extend the load set with distinct -X expression names so an
	 * -X-only invocation loads + binds its axes without @.
	 * Sidecar write uses roster_names only — expr names never persist. */
	for (int i = 0; i < expr_n; i++) {
		const char *name = expr_names[i];
		int slot = corm_axes_find_slot(name);
		if (slot < 0) {
			char path[BUFSIZ];
			if (!corm_axes_find_lib(name, path, sizeof(path))) {
				fprintf(stderr,
					"corm: unknown axis name '%s' (no lib%s.so in "
					"CORM_AXIS_PATH)\n", name, name);
				exit(EXIT_FAILURE);
			}
			corm_axes_dlopen_record(path, 0);
			slot = corm_axes_find_slot(name);
			if (slot < 0) {
				fprintf(stderr,
					"corm: plugin %s registered no axis named '%s'\n",
					path, name);
				exit(EXIT_FAILURE);
			}
		}
		const rec_axis_t *axis = rec_axis_get(slot);
		if (!axis || !axis->ctx)
			corm_axes_bind(slot, name);
	}

	/* D14: after ALL binds, broadcast the collected plugin options.
	 * Plugin flags on a --list-axes no-file run are silently ignored
	 * (setup is never reached). */
	corm_cli_plugin_flush();
}

/* `--list-axes` render (header + slot/name/fill/rank/ctx rows; PHASE-2-CLI
 * 2B-3 reuses the retired recall-query mode's layout). */
static void
corm_axes_list(void)
{
	int n = rec_axis_count();
	printf("slot  name              fill  rank  ctx\n");
	for (int slot = 0; slot < n; slot++) {
		const rec_axis_t *axis = rec_axis_get(slot);
		if (!axis)
			continue;
		printf("%-4d  %-16s  %-4s  %-4s  %s\n",
				slot, axis->name,
				axis->fill ? "y" : "n",
				axis->rank ? "y" : "n",
				axis->ctx ? "y" : "n");
	}
}

/* ========================================================================
 * 2B-4 write fan-out + forget (PHASE-2-CLI.md D1 with D11..D13).
 * Write set per -p/-d/-D = {primary} ∪ {@ roster} ∪ {target}, each store
 * exactly once. The whole -p payload fans out as (ref, blob[,len,qtype]);
 * -d/-D fans out as (ref) through rec_axis_unstore. Attempts every
 * target, reports every failure by name, exits nonzero (loud partials);
 * compensation = idempotent forget. Only fires in composed mode
 * (roster_n > 0) on an :a:-type primary — classic invocations are
 * byte-identical, no extra opens, no changed semantics.
 * ======================================================================== */

/* Resolve a -p/-d/-D ref operand on an :a: primary (ref-operand rule):
 * whole-arg decimal u32, else the primary reverse-view name lookup
 * (mirrored handle hd+1 maps NAME → ref). Returns 1 with *ref set, else
 * 0 (caller falls back loud). Call BEFORE gen_lookup (it truncates). */
static int
corm_write_ref(const char *operand, uint32_t *ref)
{
	char *end;
	unsigned long v;
	const void *r;

	if (!operand || !*operand || !ref)
		return 0;
	v = strtoul(operand, &end, 10);
	if (*end == '\0') {
		*ref = (uint32_t) v;
		return 1;
	}
	/* Named: primary reverse view (s→ref). */
	r = corm_get(prim_hd + 1, operand);
	if (!r)
		return 0;
	memcpy(ref, r, sizeof(*ref));
	return 1;
}

/* Fan the stored (ref, payload) out to every @-roster axis with a bound
 * ctx (D12: typed symbol preferred when vtype != CM_STR, else the string
 * one; missing store symbols ⇒ read-only partial). Returns failures. */
static int
corm_fanout_store(uint32_t ref, const void *payload, size_t len,
		uint32_t qtype)
{
	int nfail = 0;

	for (int i = 0; i < roster_n; i++) {
		const char *name = roster_names[i];
		int slot = corm_axes_find_slot(name);
		const rec_axis_t *axis =
			(slot >= 0) ? rec_axis_get(slot) : NULL;
		struct corm_axis_fx *fx =
			(slot >= 0 && slot < REC_QUERY_MAX_AXES)
			? &corm_axis_fx[slot] : NULL;
		int rc;

		if (!axis || !axis->ctx || !fx || !fx->bound) {
			fprintf(stderr,
				"corm: axis '%s': store skipped (no ctx bound)\n",
				name);
			nfail++;
			continue;
		}
		if (!fx->has_store && !fx->has_typed) {
			fprintf(stderr,
				"corm: axis '%s': read-only (no store symbol), "
				"skipped\n", name);
			nfail++;
			continue;
		}
		if (qtype != CM_STR && !fx->has_typed) {
			/* Binary payload a text-only axis cannot index —
			 * forward nothing (loud); the typed symbol is the
			 * only honest path for non-string primaries (D12). */
			fprintf(stderr,
				"corm: axis '%s': binary payload, text-only "
				"axis (no store_typed), skipped\n", name);
			nfail++;
			continue;
		}
		if (qtype != CM_STR && fx->has_typed)
			rc = fx->typed_fn(axis->ctx, NULL, ref,
					payload, len, qtype);
		else if (fx->has_store)
			rc = fx->store_fn(axis->ctx, NULL, ref, payload);
		else
			rc = fx->typed_fn(axis->ctx, NULL, ref,
					payload, len, qtype);
		if (rc != 0) {
			fprintf(stderr,
				"corm: axis '%s': store rejected ref %u\n",
				name, ref);
			nfail++;
		}
	}
	return nfail;
}

/* Forget one ref on every @-roster axis (rec_axis_unstore — idempotent;
 * missing unstore ⇒ read-only partial). Returns failures. */
static int
corm_fanout_unstore(uint32_t ref)
{
	int nfail = 0;

	for (int i = 0; i < roster_n; i++) {
		const char *name = roster_names[i];
		int slot = corm_axes_find_slot(name);
		const rec_axis_t *axis =
			(slot >= 0) ? rec_axis_get(slot) : NULL;
		struct corm_axis_fx *fx =
			(slot >= 0 && slot < REC_QUERY_MAX_AXES)
			? &corm_axis_fx[slot] : NULL;

		if (!axis || !axis->ctx || !fx || !fx->bound) {
			fprintf(stderr,
				"corm: axis '%s': unstore skipped (no ctx bound)\n",
				name);
			nfail++;
			continue;
		}
		if (!fx->has_unstore) {
			fprintf(stderr,
				"corm: axis '%s': read-only (no unstore symbol), "
				"skipped\n", name);
			nfail++;
			continue;
		}
		if (fx->unstore_fn(axis->ctx, ref) != 0) {
			fprintf(stderr,
				"corm: axis '%s': unstore rejected ref %u\n",
				name, ref);
			nfail++;
		}
	}
	return nfail;
}

/* ========================================================================
 * 2B-3 composed -X set-expression surface (EXCEPT-based grammar). Locked
 * D10: NOT is unary-only (complement vs the primary ref universe); EXCEPT
 * is the binary setminus keyword; A NOT B is a parse error with a hint.
 * Grammar: setexpr := orexpr (EXCEPT orexpr)*; orexpr := andexpr (OR
 * andexpr)*; andexpr := notexpr (AND notexpr)*; notexpr := NOT notexpr |
 * primary; primary := '(' setexpr ')' | axis; axis := NAME ('=' VALUE)?
 * ======================================================================== */

static void
corm_expr_error(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	fprintf(stderr, "corm: -X: ");
	vfprintf(stderr, fmt, ap);
	fputc('\n', stderr);
	va_end(ap);
	exit(EXIT_FAILURE);
}

enum tok { T_EOF, T_LP, T_RP, T_KW, T_NAME };

static struct {
	enum tok t;
	const char *word;
	size_t len;
} corm_cur_tok;

static const char *lx;

static int
lx_is_keyword(const char *w, size_t len)
{
	return (len == 3 && !memcmp(w, "AND", 3))
	    || (len == 2 && !memcmp(w, "OR",  2))
	    || (len == 3 && !memcmp(w, "NOT", 3))
	    || (len == 6 && !memcmp(w, "EXCEPT", 6));
}

static void
lx_next(void)
{
	while (*lx == ' ' || *lx == '\t')
		lx++;

	if (*lx == '\0') {
		corm_cur_tok.t = T_EOF;
		corm_cur_tok.word = lx;
		corm_cur_tok.len = 0;
		return;
	}
	if (*lx == '(') {
		corm_cur_tok.t = T_LP;
		corm_cur_tok.word = lx;
		corm_cur_tok.len = 1;
		lx++;
		return;
	}
	if (*lx == ')') {
		corm_cur_tok.t = T_RP;
		corm_cur_tok.word = lx;
		corm_cur_tok.len = 1;
		lx++;
		return;
	}

	const char *start = lx;
	while (*lx && *lx != ' ' && *lx != '\t' && *lx != '='
			&& *lx != '(' && *lx != ')')
		lx++;
	size_t len = lx - start;
	corm_cur_tok.word = start;
	corm_cur_tok.len = len;
	corm_cur_tok.t = lx_is_keyword(start, len) ? T_KW : T_NAME;

	if (corm_cur_tok.t != T_NAME)
		return;

	/* Flags-first grammar (FLAGS-FIRST): a NAME inside -X never carries a
	 * value. The lexer above stops at '='; if the next non-space char is
	 * one, the user wrote the removed NAME=VALUE form — reject loudly and
	 * point at the flag surface. Runs pre-axes-setup, so the error fires
	 * before any axis wiring. */
	const char *ws = lx;
	while (*ws == ' ' || *ws == '\t')
		ws++;
	if (*ws == '=')
		corm_expr_error(
			"'%.*s=VALUE' no longer allowed in -X; pass values via "
			"--flags (e.g. --query=… / --query@A=…)",
			(int)len, start);
}

static struct expr_node *corm_expr_setexpr(void);
static struct expr_node *corm_expr_orexpr(void);
static struct expr_node *corm_expr_andexpr(void);
static struct expr_node *corm_expr_notexpr(void);
static struct expr_node *corm_expr_primary(void);
static struct expr_node *corm_expr_leaf(void);

static struct expr_node *
corm_expr_alloc(enum expr_kind kind)
{
	if (expr_used >= EXPR_ARENA_CAP)
		corm_expr_error("too many expression nodes");
	struct expr_node *n = &expr_arena[expr_used++];
	memset(n, 0, sizeof(*n));
	n->kind = kind;
	return n;
}

static struct expr_node *
corm_expr_label_find(const char *label)
{
	for (int i = 0; i < expr_labels_n; i++)
		if (!strcmp(expr_labels[i]->label, label))
			return expr_labels[i];
	return NULL;
}

static struct expr_node *
corm_expr_setexpr(void)
{
	struct expr_node *left = corm_expr_orexpr();
	while (corm_cur_tok.t == T_KW && corm_cur_tok.len == 6
			&& !memcmp(corm_cur_tok.word, "EXCEPT", 6)) {
		lx_next();
		struct expr_node *right = corm_expr_orexpr();
		struct expr_node *n = corm_expr_alloc(E_SUB);
		n->kids[0] = left;
		n->kids[1] = right;
		n->kids_n = 2;
		left = n;
	}
	return left;
}

static struct expr_node *
corm_expr_orexpr(void)
{
	struct expr_node *left = corm_expr_andexpr();
	while (corm_cur_tok.t == T_KW && corm_cur_tok.len == 2
			&& !memcmp(corm_cur_tok.word, "OR", 2)) {
		lx_next();
		struct expr_node *right = corm_expr_andexpr();
		struct expr_node *n = corm_expr_alloc(E_OR);
		n->kids[0] = left;
		n->kids[1] = right;
		n->kids_n = 2;
		left = n;
	}
	return left;
}

static struct expr_node *
corm_expr_andexpr(void)
{
	struct expr_node *left = corm_expr_notexpr();
	while (corm_cur_tok.t == T_KW && corm_cur_tok.len == 3
			&& !memcmp(corm_cur_tok.word, "AND", 3)) {
		lx_next();
		struct expr_node *right = corm_expr_notexpr();
		struct expr_node *n = corm_expr_alloc(E_AND);
		n->kids[0] = left;
		n->kids[1] = right;
		n->kids_n = 2;
		left = n;
	}
	return left;
}

static struct expr_node *
corm_expr_notexpr(void)
{
	if (corm_cur_tok.t == T_KW && corm_cur_tok.len == 3
			&& !memcmp(corm_cur_tok.word, "NOT", 3)) {
		lx_next();
		struct expr_node *child = corm_expr_notexpr();
		struct expr_node *n = corm_expr_alloc(E_NOTP);
		n->kids[0] = child;
		n->kids_n = 1;
		return n;
	}
	return corm_expr_primary();
}

static int
corm_valid_label_chars(const char *s, size_t len)
{
	if (len == 0 || len > LABEL_MAX)
		return 0;
	if (!((s[0] >= 'a' && s[0] <= 'z') || (s[0] >= 'A' && s[0] <= 'Z')
			|| s[0] == '_'))
		return 0;
	for (size_t i = 1; i < len; i++)
		if (!((s[i] >= 'a' && s[i] <= 'z') || (s[i] >= 'A' && s[i] <= 'Z')
				|| (s[i] >= '0' && s[i] <= '9') || s[i] == '_'))
			return 0;
	return 1;
}

static struct expr_node *
corm_expr_primary(void)
{
	if (corm_cur_tok.t == T_LP) {
		lx_next();
		if (corm_cur_tok.t == T_RP)
			corm_expr_error("expected expression");
		struct expr_node *n = corm_expr_setexpr();
		if (corm_cur_tok.t != T_RP)
			corm_expr_error("expected ')'");
		lx_next();
		return n;
	}
	return corm_expr_leaf();
}

static struct expr_node *
corm_expr_leaf(void)
{
	if (corm_cur_tok.t != T_NAME)
		corm_expr_error("unexpected token '%.*s'",
				(int)corm_cur_tok.len, corm_cur_tok.word);

	const char *word = corm_cur_tok.word;
	size_t len = corm_cur_tok.len;
	const char *colon = memchr(word, ':', len);

	if (!colon) {
		/* Backward-only label reference: a bare name of a label defined
		 * earlier in source reuses that instance's memoized set. */
		for (int i = 0; i < expr_labels_n; i++) {
			const char *lbl = expr_labels[i]->label;
			if (strlen(lbl) == len && !memcmp(lbl, word, len)) {
				struct expr_node *r = corm_expr_alloc(E_REF);
				r->ref = expr_labels[i];
				r->name = lbl;
				lx_next();
				return r;
			}
		}
	}

	/* A label definition splits `label:axis` at the first colon; the axis
	 * portion registers into expr_names[] exactly like a plain leaf. */
	const char *axis = word;
	size_t axis_len = len;
	size_t llen = 0;
	if (colon) {
		llen = (size_t)(colon - word);
		axis = colon + 1;
		axis_len = len - llen - 1;
	}
	if (axis_len == 0)
		corm_expr_error("empty axis name");

	int idx = -1;
	for (int i = 0; i < expr_n; i++) {
		size_t slen = strlen(expr_names[i]);
		if (slen == axis_len && !memcmp(expr_names[i], axis, axis_len)) {
			idx = i;
			break;
		}
	}
	if (idx < 0) {
		if (expr_n >= REC_QUERY_MAX_AXES)
			corm_expr_error("too many distinct axes (max %d)",
					REC_QUERY_MAX_AXES);
		if (axis_len >= CORM_AXIS_ROSTER_MAX)
			corm_expr_error("axis name too long (max %d)",
					CORM_AXIS_ROSTER_MAX - 1);
		memcpy(expr_names[expr_n], axis, axis_len);
		expr_names[expr_n][axis_len] = '\0';
		idx = expr_n++;
	}

	struct expr_node *n = corm_expr_alloc(E_LEAF);
	n->name = expr_names[idx];

	if (colon) {
		if (llen == 0)
			corm_expr_error("empty label");
		if (lx_is_keyword(word, llen))
			corm_expr_error("label cannot be a keyword");
		if (!corm_valid_label_chars(word, llen))
			corm_expr_error("invalid label '%.*s'", (int)llen, word);
		for (int i = 0; i < expr_labels_n; i++)
			if (strlen(expr_labels[i]->label) == llen
					&& !memcmp(expr_labels[i]->label, word, llen))
				corm_expr_error("duplicate label '%.*s'",
						(int)llen, word);
		if (expr_labels_n >= EXPR_LABEL_CAP)
			corm_expr_error("too many labels (max %d)",
					EXPR_LABEL_CAP);
		memcpy(n->label, word, llen);
		n->label[llen] = '\0';
		expr_labels[expr_labels_n++] = n;
	}

	lx_next();
	return n;
}

static rec_set_t *
corm_expr_eval(struct expr_node *n, rec_set_t *universe)
{
	if (n->set)
		return n->set;

	switch (n->kind) {
	case E_REF:
		if (!n->ref || !n->ref->set)
			corm_expr_error("label '%s': instance not evaluated",
					n->name);
		return n->ref->set;
	case E_LEAF: {
		int slot = corm_axes_find_slot(n->name);
		const rec_axis_t *axis = rec_axis_get(slot);
		void *params = axis->decode
			? axis->decode(n->value)
			: (void *)(n->value ? n->value : "");
		rec_set_t *s = rec_set_new();
		if (axis->fill(axis->ctx, params, s) < 0)
			corm_expr_error("axis '%s': fill failed", n->name);
		rec_set_seal(s);
		if (axis->rank && !ranker_axis) {
			ranker_axis = axis;
			ranker_params = params;
		}
		n->set = s;
		return s;
	}
	case E_NOTP: {
		rec_set_t *c = corm_expr_eval(n->kids[0], universe);
		rec_set_t *d = rec_set_new();
		rec_set_subtract(d, universe, c);
		n->set = d;
		return d;
	}
	case E_AND: case E_OR: case E_SUB: {
		rec_set_t *acc = corm_expr_eval(n->kids[0], universe);
		for (int i = 1; i < n->kids_n; i++) {
			/* Efficiency (AXIS-EFF L1): AND/EXCEPT accumulate empty once
			 * the running set is empty (∅∩X=∅, ∅∖X=∅) — skip the
			 * remaining branches so their leaves never decode (e.g. a
			 * sepal query= would otherwise pay the embed HTTP call for
			 * a conjunction an earlier branch already emptied).
			 * Result-exact (test-shortcircuit.sh); OR unchanged. The
			 * operator node takes a FRESH empty set so free_tree never
			 * sees a leaf's set aliased under two nodes. */
			if (n->kind != E_OR && rec_set_count(acc) == 0) {
				acc = rec_set_new();
				break;
			}
			rec_set_t *t = corm_expr_eval(n->kids[i], universe);
			rec_set_t *d = rec_set_new();
			if (n->kind == E_AND)
				rec_set_intersect(d, acc, t);
			else if (n->kind == E_OR)
				rec_set_union(d, acc, t);
			else
				rec_set_subtract(d, acc, t);
			if (i > 1)
				rec_set_free(acc);
			acc = d;
		}
		n->set = acc;
		return acc;
	}
	}
	return NULL;
}

static void
corm_expr_free_tree(void)
{
	for (int i = 0; i < EXPR_ARENA_CAP; i++) {
		if (expr_arena[i].set) {
			rec_set_free(expr_arena[i].set);
			expr_arena[i].set = NULL;
		}
	}
}

/* End-of-run release for D15B synthesized heap values (leaves whose spec
 * outgrew the val pool). Kept separate from free_tree so the pre-eval
 * reset never frees a spec the tree still needs. */
static void
corm_expr_free_heap_values(void)
{
	for (int i = 0; i < EXPR_ARENA_CAP; i++) {
		if (expr_arena[i].heapval) {
			free((void *)expr_arena[i].value);
			expr_arena[i].value = NULL;
			expr_arena[i].heapval = 0;
		}
	}
}

static int
corm_expr_parse(const char *expr)
{
	expr_str = expr;
	lx = expr;
	expr_used = 0;
	expr_val_used = 0;
	expr_n = 0;
	expr_labels_n = 0;
	rank_override[0] = '\0';
	corm_scoped_n = 0;
	expr_root = NULL;

	lx_next();
	if (corm_cur_tok.t == T_EOF)
		return 0;  /* unarmed: empty/whitespace-only -X */

	expr_root = corm_expr_setexpr();

	if (corm_cur_tok.t != T_EOF) {
		if (corm_cur_tok.t == T_KW && corm_cur_tok.len == 3
				&& !memcmp(corm_cur_tok.word, "NOT", 3))
			corm_expr_error("unexpected token 'NOT' "
					"(use EXCEPT for set difference)");
		corm_expr_error("unexpected token '%.*s'",
				(int)corm_cur_tok.len, corm_cur_tok.word);
	}
	return 0;
}

/* Composed get under an armed -g . (mm-plan §3): validate, eval the tree
 * over the primary ref universe, rank or pure-filter, render. */
static int
corm_composed_get(void)
{
	if (corm_get_ktype(prim_hd) != CM_HNDL) {
		fprintf(stderr, "corm: composed -g . needs an :a:-type primary\n");
		exit(EXIT_FAILURE);
	}

	/* Pre-validate every leaf axis: slot, fill fn, bound ctx. */
	for (int i = 0; i < expr_used; i++) {
		if (expr_arena[i].kind != E_LEAF)
			continue;
		const char *name = expr_arena[i].name;
		int slot = corm_axes_find_slot(name);
		if (slot < 0) {
			fprintf(stderr, "corm: axis '%s': slot not found\n", name);
			exit(EXIT_FAILURE);
		}
		const rec_axis_t *axis = rec_axis_get(slot);
		if (!axis || !axis->fill) {
			fprintf(stderr, "corm: axis '%s': no fill function\n", name);
			exit(EXIT_FAILURE);
		}
		if (!axis->ctx) {
			fprintf(stderr, "corm: axis '%s': no ctx (not bound)\n", name);
			exit(EXIT_FAILURE);
		}
	}

	/* Label × axis collision (post-parse: labels and axis leaves may
	 * interleave freely): a label may not shadow a bound axis name, or a
	 * bare reference to that axis becomes ambiguous with the label. */
	for (int i = 0; i < expr_used; i++) {
		if (expr_arena[i].kind != E_LEAF || !expr_arena[i].label[0])
			continue;
		for (int j = 0; j < expr_used; j++) {
			if (expr_arena[j].kind != E_LEAF)
				continue;
			if (!strcmp(expr_arena[i].label, expr_arena[j].name)) {
				fprintf(stderr, "corm: label '%s' collides with "
						"axis name '%s'\n",
						expr_arena[i].label,
						expr_arena[j].name);
				exit(EXIT_FAILURE);
			}
		}
	}

	ranker_axis = NULL;
	ranker_params = NULL;
	corm_expr_free_tree();

	/* Primary ref universe (once per composed run). */
	rec_set_t *universe = rec_set_new();
	rec_set_fill_corm_iter(universe, prim_hd);
	rec_set_seal(universe);

	rec_set_t *R = corm_expr_eval(expr_root, universe);
	if (!R) {
		rec_set_free(universe);
		exit(EXIT_FAILURE);
	}

	size_t set_count = rec_set_count(R);
	int dangling = 0;
	rec_ref_t *refs = NULL;
	float *scores = NULL;
	size_t nout = 0;

	if (ranker_axis || rank_override[0]) {
		/* D15E: the rank-capable instances of the ranker axis aggregate as
		 * MAX score per ref (same-axis order-independent). Distinct axes
		 * keep D2 (first rank-capable axis in preorder wins). --rank@A
		 * replaces the set with EXACTLY the labeled instance A. */
		struct { const rec_axis_t *axis; void *params; }
			ri[EXPR_ARENA_CAP];
		int rin = 0;
		if (rank_override[0]) {
			struct expr_node *def = corm_expr_label_find(rank_override);
			int slot = def ? corm_axes_find_slot(def->name) : -1;
			const rec_axis_t *ax = slot >= 0 ? rec_axis_get(slot) : NULL;
			if (!def || def->kind != E_LEAF || !ax || !ax->rank) {
				fprintf(stderr,
					"corm: --rank@%s: not a rank-capable axis\n",
					rank_override);
				exit(EXIT_FAILURE);
			}
			ri[rin].axis = ax;
			ri[rin].params = ax->decode
				? ax->decode(def->value)
				: (void *)(def->value ? def->value : "");
			rin++;
		} else {
			for (int i = 0; i < expr_used; i++) {
				struct expr_node *nn = &expr_arena[i];
				if (nn->kind != E_LEAF)
					continue;
				int slot = corm_axes_find_slot(nn->name);
				const rec_axis_t *ax = slot >= 0
					? rec_axis_get(slot) : NULL;
				if (!ax || ax != ranker_axis || !ax->rank)
					continue;
				ri[rin].axis = ax;
				ri[rin].params = ax->decode
					? ax->decode(nn->value)
					: (void *)(nn->value ? nn->value : "");
				rin++;
			}
		}

		size_t k = top_k ? top_k : set_count;
		rec_rank_t *board = NULL;
		if (k > 0) {
			board = rec_rank_new(k, min_score);
			refs = malloc(k * sizeof(*refs));
			scores = malloc(k * sizeof(*scores));
		}
		const rec_ref_t *rrefs = rec_set_at(R);
		for (size_t i = 0; i < set_count; i++) {
			float bs = 0.0f;
			int got = 0;
			for (int r = 0; r < rin; r++) {
				float s;
				if (ri[r].axis->rank((void *)ri[r].axis->ctx,
						ri[r].params, rrefs[i], &s) == 0
						&& (!got || s > bs)) {
					bs = s;
					got = 1;
				}
			}
			if (got)
				rec_rank_push(board, rrefs[i], bs);
		}
		if (board)
			nout = rec_rank_sorted(board, refs, scores);
		for (size_t i = 0; i < nout; i++) {
			const void *rec = corm_get(prim_hd, &refs[i]);
			if (!rec) {
				dangling++;
				continue;
			}
			printf("%u %f ", refs[i], scores[i]);
			corme_print(prim_hd, VALUE, rec);
			putchar('\n');
		}
		if (board)
			rec_rank_free(board);
	} else {
		const rec_ref_t *rrefs = rec_set_at(R);
		for (size_t i = 0; i < set_count; i++) {
			const void *rec = corm_get(prim_hd, &rrefs[i]);
			if (!rec) {
				dangling++;
				continue;
			}
			printf("%u ", rrefs[i]);
			corme_print(prim_hd, VALUE, rec);
			putchar('\n');
		}
	}

	if (dangling)
		fprintf(stderr, "corm: %d refs skipped: no primary record\n",
				dangling);

	free(refs);
	free(scores);
	corm_expr_free_tree();
	corm_expr_free_heap_values();
	rec_set_free(universe);
	return EXIT_SUCCESS;
}

static void u_print(const void *data) {
	printf("%u", * (uint32_t *) data);
}

static void s_print(const void *data) {
	printf("%s", (char *) data);
}

int
main(int argc, char *argv[])
{
	static char *optstr = "kxla:q:p:d:D:g:m:c:rR:L:X:t:b:?";
	char *fname = NULL;
	int ch;
	int rc = EXIT_SUCCESS;
	int query_ran = 0; /* set when an explicit -g . runs the query */
	uint32_t flags = QH_RDONLY, aux;

	if (argc < 2) {
		usage(*argv);
		return EXIT_FAILURE;
	}

	corm_cli_prog = *argv;
	corm_cli_plugin_collect(argc, argv);
	const struct option *opts = corm_cli_plugin_table();

	types[CM_HNDL].print = u_print;
	types[CM_STR].print = s_print;
	types[CM_U32].print = u_print;

	aqs[AQ_A].hd = corm_open(NULL, NULL,
			CM_HNDL, CM_HNDL,
			QDBE_QMASK, CM_AINDEX);

	aqs[AQ_Q].hd = corm_open(NULL, NULL,
			CM_HNDL, CM_HNDL,
			QDBE_QMASK, CM_AINDEX);

	while ((ch = getopt_long(argc, argv, optstr, opts, NULL)) != -1)
		switch (ch) {
		case 'a':
			aux_hd = gen_open(optarg, QH_RDONLY);
			aux = aqs[AQ_A].hd;
			corm_put(aux, NULL, &aux_hd);
			aqs[AQ_A].n++;
			break;
		case 'q':
			aux_hd = gen_open(optarg, QH_RDONLY);
			aux = aqs[AQ_Q].hd;
			corm_put(aux, NULL, &aux_hd);
			aqs[AQ_Q].n++;
			break;
		case 'x':
			bail = 1;
			break;
		case 'k':
			print_keys = 1;
			break;
		case CLIP_OPT_LIST_AXES:
			list_axes = 1;
			break;
		case 'X':
			expr_str = optarg;
			if (corm_expr_parse(optarg) != 0)
				return EXIT_FAILURE;
			break;
		case 't':
		case CLIP_OPT_TOP: {
			char *end;
			long t = strtol(optarg, &end, 10);
			if (*end != '\0' || t < 0) {
				fprintf(stderr, "corm: invalid --top value '%s'\n", optarg);
				return EXIT_FAILURE;
			}
			top_k = (size_t) t;
			break;
		}
		case 'b':
		case CLIP_OPT_BOTTOM: {
			char *end;
			float b = strtof(optarg, &end);
			if (end == optarg) {
				fprintf(stderr, "corm: invalid --bottom value '%s'\n", optarg);
				return EXIT_FAILURE;
			}
			min_score = b;
			break;
		}
		case CLIP_OPT_RANK: {
			size_t l = strlen(optarg);
			if (l == 0 || l > LABEL_MAX) {
				fprintf(stderr, "corm: invalid --rank label '%s'\n", optarg);
				return EXIT_FAILURE;
			}
			strcpy(rank_override, optarg);
			break;
		}
	case 'p':
	case 'd':
	case 'D':
		/* Write-path ref inference note: many fsck/alias helper paths want
		 * ref from the `-p <n>` / `-d <n>` numeral ("m1" in the docs). That
		 * has nothing to do with the per-axis candidate-pool knob `m=` in
		 * query specs (joint/sepal/stoma), which lives in rec_axis_decode
		 * and is clamped to the store size by the axis libraries. */
		flags &= ~QH_RDONLY;
	case 'l':
	case 'L':
	case 'R':
	case 'g':
	case 'm':
	case 'c':
	case 'r': break;
		case CLIP_OPT_PLUGIN: break;    /* collected plugin option: handled post-bind */
		default: break;
		case '?': usage(*argv); return EXIT_SUCCESS;
		}

	if (list_axes && optind >= argc) {
		/* No file: load env-axis libs and list (D9 by-name discovery). */
		corm_axes_dlopen_env(1);
		corm_axes_list();
		return EXIT_SUCCESS;
	}

	if (optind >= argc) {
		usage(*argv);
		return EXIT_FAILURE;
	}
	fname = argv[optind];

	optind = 1;
	prim_hd = gen_open(fname, flags);
	srand(time(NULL));

	/* 2B-1 inter-pass: reconcile the roster, load + bind axes by name. */
	corm_axes_setup();
	if (list_axes) {
		corm_axes_list();
		return EXIT_SUCCESS;
	}

	while ((ch = getopt_long(argc, argv, optstr, opts, NULL)) != -1) switch (ch) {
	case 'R': gen_rand(); break;
	case 'L': gen_list_missing(); break;
	case 'l': gen_list(); break;
	case 'p': rc |= gen_put(); break;
	case 'd': rc |= gen_del(); break;
	case 'D': rc |= gen_del_all(); break;
	case 'g':
		if (!strcmp(optarg, ".") && expr_root) {
			rc = corm_composed_get();
			query_ran = 1;
		} else
			gen_get(optarg);
		break;
	case 'm': gen_multi(); break;
	case 'c': gen_count(); break;
	case 'r': reverse = !reverse; break;
	case 'X': case 't': case 'b': break;
	case CLIP_OPT_TOP: case CLIP_OPT_BOTTOM: break;
	case CLIP_OPT_LIST_AXES: case CLIP_OPT_PLUGIN: break;
	}
	/* An armed -X needs no -g .: run the composed query once after all
	 * ops when no explicit -g . already ran it at argv position. */
	if (expr_root && !query_ran)
		rc |= corm_composed_get();
	return rc;
}
