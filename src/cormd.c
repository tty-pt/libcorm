/**
 * @file cormd.c
 * @brief Resident daemon for libcorm and multi-dimensional search axes.
 *
 * Keeps primary corm database and in-memory search indexes (liblemma, libjoint)
 * resident in memory between requests to eliminate repetitive process startup
 * and lemma index reconstruction.
 */

#include <ttypt/corm.h>
#include <ttypt/idm.h>
#include <ttypt/rec.h>
#include <ttypt/qsys.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <setjmp.h>
#include <getopt.h>
#include <libgen.h>
#include <limits.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/poll.h>
#include <sys/file.h>
#include <sys/resource.h>

#define CM_MAX 1024
#define QDBE_MASK (256 - 1)
#define CM_FLAGS_MASK 0x1F
#define CORM_AXIS_ROSTER_MAX 64
#define CORM_CLI_PLUGIN_CAP 32
#define CORM_CLI_PLUGIN_OPTS_MAX 32
#define EXPR_ARENA_CAP 128
#define EXPR_VAL_MAX 512
#define EXPR_LABEL_CAP 32
#define LABEL_MAX 32

#define MAX_PAYLOAD_SIZE (64 * 1024 * 1024)
#define MAX_HEADER_SIZE 4096
#define MAX_CLIENTS 64

/* ── Globals ─────────────────────────────────────────────────────────── */

static uint32_t prim_hd;
static char corm_path[BUFSIZ];
static char db_filespec[BUFSIZ];
static char socket_path[BUFSIZ];
static char lock_path[BUFSIZ];
static int lock_fd = -1;
static int server_fd = -1;
static volatile sig_atomic_t g_stop = 0;
static time_t start_time = 0;

struct corm_meta {
	uint32_t types[2];
};
static struct corm_meta metas[CM_MAX];

/* Axis and plugin state */
struct corm_axis_fx {
	int bound;
	int has_store;
	int has_typed;
	int has_unstore;
	int has_readback;
	int (*store_fn)(void *, const char *, rec_ref_t, const char *);
	int (*typed_fn)(void *, const char *, rec_ref_t, const void *, size_t, uint32_t);
	int (*unstore_fn)(void *, rec_ref_t);
	int (*readback_fn)(void *, rec_ref_t, char **, size_t *);
};
static struct corm_axis_fx corm_axis_fx[REC_QUERY_MAX_AXES];

static char roster_names[REC_QUERY_MAX_AXES][CORM_AXIS_ROSTER_MAX];
static int roster_n = 0;

static struct corm_loaded {
	void *h;
	int lo, hi;
} corm_loaded_slots[REC_QUERY_MAX_AXES];
static int corm_loaded_n = 0;

struct corm_cli_plug {
	void *h;
	const rec_axis_cli_option_t *opts;
	int (*cfg)(const char *, const char *);
};
static struct corm_cli_plug loaded_plugs[CORM_CLI_PLUGIN_CAP];
static int loaded_plugs_n = 0;

/* Expression grammar / evaluation state */
static const char *expr_str;
static const char *lx;
static char expr_names[REC_QUERY_MAX_AXES][CORM_AXIS_ROSTER_MAX];
static int expr_n;
static size_t top_k;
static float min_score;
static const rec_axis_t *ranker_axis;
static void *ranker_params;
static char rank_override[CORM_AXIS_ROSTER_MAX];

enum expr_kind { E_LEAF, E_NOTP, E_AND, E_OR, E_SUB, E_REF };

struct expr_node {
	enum expr_kind kind;
	const char *name;
	char label[LABEL_MAX + 1];
	struct expr_node *ref;
	const char *value;
	int heapval;
	struct expr_node *kids[REC_QUERY_MAX_AXES + 1];
	int kids_n;
	rec_set_t *set;
};

static struct expr_node expr_arena[EXPR_ARENA_CAP];
static int expr_used;
static struct expr_node *expr_labels[EXPR_LABEL_CAP];
static int expr_labels_n;
static struct expr_node *expr_root;

static jmp_buf g_expr_jmp;
static char g_expr_err[256];

enum tok { T_EOF, T_LP, T_RP, T_KW, T_NAME };

static struct {
	enum tok t;
	const char *word;
	size_t len;
} corm_cur_tok;

/* ── Buffer & Client structure ────────────────────────────────────────── */

struct client {
	int fd;
	char *in_buf;
	size_t in_len;
	size_t in_cap;
	char *out_buf;
	size_t out_len;
	size_t out_pos;
	size_t out_cap;
};
static struct client clients[MAX_CLIENTS];

/* ── Forward declarations ────────────────────────────────────────────── */

static uint32_t corm_mask_effective(void);
static uint32_t gen_open(char *fname, uint32_t flags);
static void corm_axes_setup(void);
static int corm_fanout_store(uint32_t ref, const void *payload, size_t len, uint32_t qtype);
static int corm_fanout_unstore(uint32_t ref);
static int corm_write_ref(const char *operand, uint32_t *ref);
static void cormd_expr_error(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* ── Effective mask & Type specifiers ────────────────────────────────── */

static uint32_t
corm_mask_effective(void)
{
	const char *e = getenv("CORM_MASK");
	if (e && *e) {
		char *end;
		unsigned long v = strtoul(e, &end, 10);
		if (*end == '\0' && v > 0 && (v & (v + 1)) == 0)
			return (uint32_t)v;
	}
	return QDBE_MASK;
}

static uint32_t
_corme_type(const char *which)
{
	if (*which == '2' && which[1] != '\0')
		which++;
	if (*which == 's')
		return CM_STR;
	if (*which == 'u')
		return CM_U32;
	return CM_STR;
}

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
		if (roster_n >= REC_QUERY_MAX_AXES)
			break;
		snprintf(roster_names[roster_n], CORM_AXIS_ROSTER_MAX, "%s", tok);
		roster_n++;
	}
	return roster_n;
}

static void
roster_csv_join(char *out, size_t cap)
{
	size_t n = 0;
	out[0] = '\0';
	for (int i = 0; i < roster_n; i++)
		n += snprintf(out + n, cap > n ? cap - n : 0,
				"%s%s", i ? "," : "", roster_names[i]);
}

static uint32_t
gen_open(char *fname, uint32_t flags)
{
	char buf[BUFSIZ];
	uint32_t ktype = CM_STR;
	uint32_t vtype = CM_STR;
	uint32_t hd;

	int written = snprintf(buf, sizeof(buf), "%s", fname);
	if (written < 0 || (size_t)written >= sizeof(buf)) {
		fprintf(stderr, "cormd: filename too long\n");
		exit(EXIT_FAILURE);
	}

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
		} else {
			ktype = _corme_type(first_col);
		}
	}

	if (at)
		roster_csv_parse(frag);
	else
		roster_n = 0;

	snprintf(corm_path, sizeof(corm_path), "%s", buf);

	hd = corm_open(buf, "hd", ktype, vtype,
			corm_mask_effective(),
			(flags & CM_FLAGS_MASK) | CM_MIRROR);
	metas[hd].types[0] = ktype;
	metas[hd].types[1] = vtype;
	metas[hd + 1].types[0] = vtype;
	metas[hd + 1].types[1] = ktype;
	return hd;
}

/* ── Axis discovery and binding ───────────────────────────────────────── */

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

static void
corm_axes_dlopen_record(const char *path, int quiet)
{
	int before = rec_axis_count();
	void *h = qsys_dlopen(path, 0);
	if (!h) {
		if (!quiet) {
			const char *err = qsys_dlerror();
			fprintf(stderr, "cormd: %s: %s\n", path, err ? err : "unknown error");
		}
		return;
	}
	int after = rec_axis_count();
	if (after == before)
		return;
	if (corm_loaded_n >= REC_QUERY_MAX_AXES)
		return;
	corm_loaded_slots[corm_loaded_n++] = (struct corm_loaded){ h, before, after - 1 };

	/* Cache plugin option functions */
	if (loaded_plugs_n < CORM_CLI_PLUGIN_CAP) {
		void *osym = qsys_dlsym(h, "rec_axis_cli_options");
		void *csym = qsys_dlsym(h, "rec_axis_config_arg");
		if (osym && csym) {
			const rec_axis_cli_option_t *(*opts_fn)(void);
			int (*cfg_fn)(const char *, const char *);
			memcpy(&opts_fn, &osym, sizeof(opts_fn));
			memcpy(&cfg_fn, &csym, sizeof(cfg_fn));
			loaded_plugs[loaded_plugs_n].h = h;
			loaded_plugs[loaded_plugs_n].opts = opts_fn();
			loaded_plugs[loaded_plugs_n].cfg = cfg_fn;
			loaded_plugs_n++;
		}
	}
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
	for (int i = 0; i < corm_loaded_n; i++) {
		if (slot >= corm_loaded_slots[i].lo && slot <= corm_loaded_slots[i].hi)
			return corm_loaded_slots[i].h;
	}
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
		snprintf(dirs, sizeof(dirs), "/usr/lib:/usr/local/lib:./lib:../lib");

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
corm_axes_close(int slot, const char *name)
{
	const rec_axis_t *axis = rec_axis_get(slot);
	void *ctx = axis ? axis->ctx : NULL;

	if (slot >= 0 && slot < REC_QUERY_MAX_AXES) {
		corm_axis_fx[slot].bound = 0;
	}

	if (!ctx)
		return;

	void *h = corm_axes_handle_for_slot(slot);
	if (h) {
		/* 1. Try explicit rec_axis_close if plugin provides it */
		void *sym = qsys_dlsym(h, "rec_axis_close");
		if (sym) {
			void (*close_fn)(void *);
			memcpy(&close_fn, &sym, sizeof(close_fn));
			close_fn(ctx);
			rec_axis_set_ctx(slot, NULL);
			return;
		}
		/* 2. Try <name>_close if plugin exports it (e.g. lemma_close, joint_close, sepal_close) */
		char cname[128];
		snprintf(cname, sizeof(cname), "%s_close", name);
		sym = qsys_dlsym(h, cname);
		if (sym) {
			if (!strcmp(name, "joint")) {
				void (*jclose)(unsigned);
				memcpy(&jclose, &sym, sizeof(jclose));
				jclose((unsigned)(uintptr_t)ctx);
			} else {
				void (*close_fn)(void *);
				memcpy(&close_fn, &sym, sizeof(close_fn));
				close_fn(ctx);
			}
			rec_axis_set_ctx(slot, NULL);
			return;
		}
	}
	rec_axis_set_ctx(slot, NULL);
}

static int
corm_axes_bind(int slot, const char *name)
{
	void *h = corm_axes_handle_for_slot(slot);
	char spec[BUFSIZ];
	corm_axes_spec(name, spec, sizeof(spec));

	if (!h) {
		fprintf(stderr, "cormd: axis '%s': no dlopen handle\n", name);
		if (slot >= 0 && slot < REC_QUERY_MAX_AXES)
			corm_axis_fx[slot].bound = 0;
		return -1;
	}
	void *sym = qsys_dlsym(h, "rec_axis_open");
	if (!sym) {
		fprintf(stderr, "cormd: axis '%s': plugin has no rec_axis_open symbol\n", name);
		if (slot >= 0 && slot < REC_QUERY_MAX_AXES)
			corm_axis_fx[slot].bound = 0;
		return -1;
	}
	void *(*open_fn)(const char *);
	memcpy(&open_fn, &sym, sizeof(open_fn));
	void *ctx = open_fn(spec);
	if (!ctx) {
		fprintf(stderr, "cormd: axis '%s': rec_axis_open failed\n", name);
		if (slot >= 0 && slot < REC_QUERY_MAX_AXES)
			corm_axis_fx[slot].bound = 0;
		return -1;
	}
	rec_axis_set_ctx(slot, ctx);

	void *csym = qsys_dlsym(h, "rec_axis_env_config");
	if (csym) {
		int (*cfg_fn)(void);
		memcpy(&cfg_fn, &csym, sizeof(cfg_fn));
		cfg_fn();
	}

	if (slot >= 0 && slot < REC_QUERY_MAX_AXES) {
		struct corm_axis_fx *fx = &corm_axis_fx[slot];
		corm_axes_sym(h, "rec_axis_store", &fx->store_fn, sizeof(fx->store_fn), &fx->has_store);
		corm_axes_sym(h, "rec_axis_store_typed", &fx->typed_fn, sizeof(fx->typed_fn), &fx->has_typed);
		corm_axes_sym(h, "rec_axis_unstore", &fx->unstore_fn, sizeof(fx->unstore_fn), &fx->has_unstore);
		corm_axes_sym(h, "rec_axis_readback", &fx->readback_fn, sizeof(fx->readback_fn), &fx->has_readback);
		fx->bound = 1;
	}
	return 0;
}

static void
corm_axes_dlopen_env(int quiet)
{
	const char *env_libs = getenv("CORM_AXIS_LIBS");
	if (!env_libs || !*env_libs)
		return;
	char *copy = strdup(env_libs);
	if (!copy)
		return;
	char *save = NULL;
	for (char *tok = strtok_r(copy, ":", &save); tok;
			tok = strtok_r(NULL, ":", &save)) {
		corm_axes_dlopen_record(tok, quiet);
	}
	free(copy);
}

static void
corm_axes_setup(void)
{
	char sidecar[BUFSIZ + 16];
	qsys_setenv("CORM_AXIS_PRIMARY", corm_path, 1);

	snprintf(sidecar, sizeof(sidecar), "%s.roster", corm_path);
	struct stat st;
	int have_sidecar = stat(sidecar, &st) == 0;

	if (have_sidecar && roster_n == 0) {
		uint32_t rs = corm_open(sidecar, "@roster", CM_STR, CM_STR, corm_mask_effective(), 0);
		const void *axes = corm_get(rs, "axes");
		if (axes)
			roster_csv_parse((const char *)axes);
	} else if (!have_sidecar && roster_n > 0) {
		uint32_t rs = corm_open(sidecar, "@roster", CM_STR, CM_STR, corm_mask_effective(), 0);
		char csv[BUFSIZ];
		roster_csv_join(csv, sizeof(csv));
		corm_put(rs, "v", "1");
		corm_put(rs, "axes", csv);
	}

	corm_axes_dlopen_env(1);

	for (int i = 0; i < roster_n; i++) {
		const char *name = roster_names[i];
		int slot = corm_axes_find_slot(name);
		if (slot < 0) {
			char path[BUFSIZ];
			if (!corm_axes_find_lib(name, path, sizeof(path))) {
				fprintf(stderr, "cormd: unknown axis '%s'\n", name);
				continue;
			}
			corm_axes_dlopen_record(path, 0);
			slot = corm_axes_find_slot(name);
		}
		if (slot >= 0)
			corm_axes_bind(slot, name);
	}
}

/* ── Write fan-out ────────────────────────────────────────────────────── */

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
		*ref = (uint32_t)v;
		return 1;
	}
	r = corm_get(prim_hd + 1, operand);
	if (!r)
		return 0;
	memcpy(ref, r, sizeof(*ref));
	return 1;
}

static int
corm_single_axis_store(int slot, uint32_t ref, const void *payload, size_t len, uint32_t qtype)
{
	const rec_axis_t *axis = (slot >= 0) ? rec_axis_get(slot) : NULL;
	struct corm_axis_fx *fx = (slot >= 0 && slot < REC_QUERY_MAX_AXES) ? &corm_axis_fx[slot] : NULL;
	int rc = 0;

	if (!axis || !axis->ctx || !fx || !fx->bound)
		return 0;
	if (qtype != CM_STR && fx->has_typed)
		rc = fx->typed_fn(axis->ctx, NULL, ref, payload, len, qtype);
	else if (fx->has_store)
		rc = fx->store_fn(axis->ctx, NULL, ref, (const char *)payload);
	else if (fx->has_typed)
		rc = fx->typed_fn(axis->ctx, NULL, ref, payload, len, qtype);
	return rc;
}

static int
corm_fanout_store(uint32_t ref, const void *payload, size_t len, uint32_t qtype)
{
	int nfail = 0;
	for (int i = 0; i < roster_n; i++) {
		const char *name = roster_names[i];
		int slot = corm_axes_find_slot(name);
		const rec_axis_t *axis = (slot >= 0) ? rec_axis_get(slot) : NULL;
		struct corm_axis_fx *fx = (slot >= 0 && slot < REC_QUERY_MAX_AXES) ? &corm_axis_fx[slot] : NULL;
		int rc;

		if (!axis || !axis->ctx || !fx || !fx->bound)
			continue;
		if (qtype != CM_STR && fx->has_typed)
			rc = fx->typed_fn(axis->ctx, NULL, ref, payload, len, qtype);
		else if (fx->has_store)
			rc = fx->store_fn(axis->ctx, NULL, ref, (const char *)payload);
		else if (fx->has_typed)
			rc = fx->typed_fn(axis->ctx, NULL, ref, payload, len, qtype);
		else
			continue;

		if (rc != 0)
			nfail++;
	}
	return nfail;
}

static int
corm_fanout_unstore(uint32_t ref)
{
	int nfail = 0;
	for (int i = 0; i < roster_n; i++) {
		const char *name = roster_names[i];
		int slot = corm_axes_find_slot(name);
		const rec_axis_t *axis = (slot >= 0) ? rec_axis_get(slot) : NULL;
		struct corm_axis_fx *fx = (slot >= 0 && slot < REC_QUERY_MAX_AXES) ? &corm_axis_fx[slot] : NULL;

		if (!axis || !axis->ctx || !fx || !fx->bound || !fx->has_unstore)
			continue;
		if (fx->unstore_fn(axis->ctx, ref) != 0)
			nfail++;
	}
	return nfail;
}

/* ── Expression Lexer & Parser ────────────────────────────────────────── */

static void
cormd_expr_error(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(g_expr_err, sizeof(g_expr_err), fmt, ap);
	va_end(ap);
	longjmp(g_expr_jmp, 1);
}

static int
lx_is_keyword(const char *w, size_t len)
{
	return (len == 3 && !memcmp(w, "AND", 3))
		|| (len == 2 && !memcmp(w, "OR", 2))
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
	while (*lx && *lx != ' ' && *lx != '\t' && *lx != '=' && *lx != '(' && *lx != ')')
		lx++;
	size_t len = lx - start;
	corm_cur_tok.word = start;
	corm_cur_tok.len = len;
	corm_cur_tok.t = lx_is_keyword(start, len) ? T_KW : T_NAME;

	if (corm_cur_tok.t != T_NAME)
		return;

	const char *ws = lx;
	while (*ws == ' ' || *ws == '\t')
		ws++;
	if (*ws == '=')
		cormd_expr_error("'%.*s=VALUE' no longer allowed in -X; pass via flags", (int)len, start);
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
		cormd_expr_error("too many expression nodes");
	struct expr_node *n = &expr_arena[expr_used++];
	memset(n, 0, sizeof(*n));
	n->kind = kind;
	return n;
}

static struct expr_node *
corm_expr_setexpr(void)
{
	struct expr_node *left = corm_expr_orexpr();
	while (corm_cur_tok.t == T_KW && corm_cur_tok.len == 6 && !memcmp(corm_cur_tok.word, "EXCEPT", 6)) {
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
	while (corm_cur_tok.t == T_KW && corm_cur_tok.len == 2 && !memcmp(corm_cur_tok.word, "OR", 2)) {
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
	while (corm_cur_tok.t == T_KW && corm_cur_tok.len == 3 && !memcmp(corm_cur_tok.word, "AND", 3)) {
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
	if (corm_cur_tok.t == T_KW && corm_cur_tok.len == 3 && !memcmp(corm_cur_tok.word, "NOT", 3)) {
		lx_next();
		struct expr_node *child = corm_expr_notexpr();
		struct expr_node *n = corm_expr_alloc(E_NOTP);
		n->kids[0] = child;
		n->kids_n = 1;
		return n;
	}
	return corm_expr_primary();
}

static struct expr_node *
corm_expr_primary(void)
{
	if (corm_cur_tok.t == T_LP) {
		lx_next();
		if (corm_cur_tok.t == T_RP)
			cormd_expr_error("expected expression inside parens");
		struct expr_node *n = corm_expr_setexpr();
		if (corm_cur_tok.t != T_RP)
			cormd_expr_error("expected ')'");
		lx_next();
		return n;
	}
	return corm_expr_leaf();
}

static struct expr_node *
corm_expr_leaf(void)
{
	if (corm_cur_tok.t != T_NAME)
		cormd_expr_error("unexpected token '%.*s'", (int)corm_cur_tok.len, corm_cur_tok.word);

	const char *word = corm_cur_tok.word;
	size_t len = corm_cur_tok.len;
	const char *colon = memchr(word, ':', len);

	if (!colon) {
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

	const char *axis = word;
	size_t axis_len = len;
	size_t llen = 0;
	if (colon) {
		llen = (size_t)(colon - word);
		axis = colon + 1;
		axis_len = len - llen - 1;
	}
	if (axis_len == 0)
		cormd_expr_error("empty axis name");

	int idx = -1;
	for (int i = 0; i < expr_n; i++) {
		if (strlen(expr_names[i]) == axis_len && !memcmp(expr_names[i], axis, axis_len)) {
			idx = i;
			break;
		}
	}
	if (idx < 0) {
		if (expr_n >= REC_QUERY_MAX_AXES)
			cormd_expr_error("too many distinct axes (max %d)", REC_QUERY_MAX_AXES);
		if (axis_len >= CORM_AXIS_ROSTER_MAX)
			cormd_expr_error("axis name too long");
		memcpy(expr_names[expr_n], axis, axis_len);
		expr_names[expr_n][axis_len] = '\0';
		idx = expr_n++;
	}

	struct expr_node *n = corm_expr_alloc(E_LEAF);
	n->name = expr_names[idx];

	if (colon && llen > 0) {
		if (llen > LABEL_MAX)
			cormd_expr_error("label too long");
		memcpy(n->label, word, llen);
		n->label[llen] = '\0';
		if (expr_labels_n < EXPR_LABEL_CAP)
			expr_labels[expr_labels_n++] = n;
	}

	lx_next();
	return n;
}

static rec_set_t *
corm_expr_eval(struct expr_node *n, rec_set_t *universe)
{
	if (!n)
		return NULL;
	if (n->set)
		return n->set;

	switch (n->kind) {
	case E_REF:
		if (!n->ref || !n->ref->set)
			cormd_expr_error("label '%s': instance not evaluated", n->name);
		return n->ref->set;

	case E_LEAF: {
		int slot = corm_axes_find_slot(n->name);
		if (slot < 0)
			cormd_expr_error("axis '%s': not loaded", n->name);
		const rec_axis_t *axis = rec_axis_get(slot);
		if (!axis || !axis->fill)
			cormd_expr_error("axis '%s': no fill fn", n->name);
		if (!axis->ctx)
			cormd_expr_error("axis '%s': no ctx", n->name);

		void *params = axis->decode
			? axis->decode(n->value)
			: (void *)(n->value ? n->value : "");
		rec_set_t *s = rec_set_new();
		if (axis->fill(axis->ctx, params, s) < 0)
			cormd_expr_error("axis '%s': fill failed", n->name);
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

	case E_AND:
	case E_OR:
	case E_SUB: {
		rec_set_t *acc = corm_expr_eval(n->kids[0], universe);
		for (int i = 1; i < n->kids_n; i++) {
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
	expr_n = 0;
	expr_labels_n = 0;
	rank_override[0] = '\0';
	expr_root = NULL;

	lx_next();
	if (corm_cur_tok.t == T_EOF)
		return 0;

	expr_root = corm_expr_setexpr();
	if (corm_cur_tok.t != T_EOF) {
		if (corm_cur_tok.t == T_KW && corm_cur_tok.len == 3 && !memcmp(corm_cur_tok.word, "NOT", 3))
			cormd_expr_error("unexpected token 'NOT' (use EXCEPT)");
		cormd_expr_error("unexpected token '%.*s'", (int)corm_cur_tok.len, corm_cur_tok.word);
	}
	return 0;
}

/* ── Dynamic Buffer Helpers ───────────────────────────────────────────── */

static int
buf_append(char **buf, size_t *len, size_t *cap, const char *data, size_t dlen)
{
	if (*len + dlen + 1 > *cap) {
		size_t ncap = *cap ? *cap * 2 : 4096;
		while (ncap < *len + dlen + 1)
			ncap *= 2;
		char *nbuf = realloc(*buf, ncap);
		if (!nbuf)
			return -1;
		*buf = nbuf;
		*cap = ncap;
	}
	memcpy(*buf + *len, data, dlen);
	*len += dlen;
	(*buf)[*len] = '\0';
	return 0;
}

static int
buf_printf(char **buf, size_t *len, size_t *cap, const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	char tmp[1024];
	int n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
	va_end(ap);
	if (n < 0)
		return -1;
	if ((size_t)n < sizeof(tmp))
		return buf_append(buf, len, cap, tmp, n);

	char *big = malloc(n + 1);
	if (!big)
		return -1;
	va_start(ap, fmt);
	vsnprintf(big, n + 1, fmt, ap);
	va_end(ap);
	int r = buf_append(buf, len, cap, big, n);
	free(big);
	return r;
}

/* ── Client connection cleanup ────────────────────────────────────────── */

static void
client_close(struct client *c)
{
	if (c->fd >= 0) {
		close(c->fd);
		c->fd = -1;
	}
	free(c->in_buf);
	c->in_buf = NULL;
	c->in_len = 0;
	c->in_cap = 0;
	free(c->out_buf);
	c->out_buf = NULL;
	c->out_len = 0;
	c->out_pos = 0;
	c->out_cap = 0;
}

/* ── Request Execution ────────────────────────────────────────────────── */

static void
respond_ok(struct client *c, const char *req_id, const char *body, size_t body_len)
{
	buf_printf(&c->out_buf, &c->out_len, &c->out_cap, "OK %s %zu\n", req_id, body_len);
	if (body && body_len > 0)
		buf_append(&c->out_buf, &c->out_len, &c->out_cap, body, body_len);
}

static void
respond_err(struct client *c, const char *req_id, const char *msg)
{
	size_t mlen = strlen(msg);
	int need_nl = (mlen == 0 || msg[mlen - 1] != '\n');
	size_t total_len = mlen + (need_nl ? 1 : 0);
	buf_printf(&c->out_buf, &c->out_len, &c->out_cap, "ERR %s %zu\n%s", req_id, total_len, msg);
	if (need_nl)
		buf_append(&c->out_buf, &c->out_len, &c->out_cap, "\n", 1);
}

static int g_index_degraded = 0;

static int
check_database_match(const char *req_filespec)
{
	if (!req_filespec || !*req_filespec)
		return 1;
	char raw_req[BUFSIZ];
	snprintf(raw_req, sizeof(raw_req), "%s", req_filespec);
	char *at = strchr(raw_req, '@');
	if (at) *at = '\0';
	char *col = strchr(raw_req, ':');
	if (col) *col = '\0';

	char req_tmp1[BUFSIZ], req_tmp2[BUFSIZ];
	char srv_tmp1[BUFSIZ], srv_tmp2[BUFSIZ];
	snprintf(req_tmp1, sizeof(req_tmp1), "%s", raw_req);
	snprintf(req_tmp2, sizeof(req_tmp2), "%s", raw_req);
	snprintf(srv_tmp1, sizeof(srv_tmp1), "%s", corm_path);
	snprintf(srv_tmp2, sizeof(srv_tmp2), "%s", corm_path);

	char *rd = dirname(req_tmp1);
	char *rb = basename(req_tmp2);
	char *sd = dirname(srv_tmp1);
	char *sb = basename(srv_tmp2);

	char req_dir_real[PATH_MAX] = {0};
	char srv_dir_real[PATH_MAX] = {0};

	if (!realpath(rd, req_dir_real) || !realpath(sd, srv_dir_real))
		return 0;

	if (strcmp(req_dir_real, srv_dir_real) != 0)
		return 0;

	return strcmp(rb, sb) == 0;
}

static int
reconcile_secondary_indexes(char *err_msg, size_t err_sz)
{
	/* Flush primary changes to disk so disk state is clean */
	corm_save();

	uint32_t count = 0;
	uint32_t cur = corm_iter(prim_hd, NULL, 0);
	const void *k, *v;
	while (corm_next(&k, &v, cur)) count++;
	corm_fin(cur);

	for (int i = 0; i < roster_n; i++) {
		const char *name = roster_names[i];
		int slot = corm_axes_find_slot(name);
		if (slot < 0) {
			snprintf(err_msg, err_sz, "reconciliation failed: unknown axis '%s'", name);
			g_index_degraded = 1;
			return -1;
		}

		char spec[BUFSIZ];
		corm_axes_spec(name, spec, sizeof(spec));

		void *h = corm_axes_handle_for_slot(slot);
		const rec_axis_t *axis = rec_axis_get(slot);
		void *ctx = axis ? axis->ctx : NULL;
		int reset_done = 0;

		/* Try explicit reset if plugin exports it */
		if (h && ctx) {
			void *rsym = qsys_dlsym(h, "rec_axis_reset");
			if (!rsym) {
				char rname[128];
				snprintf(rname, sizeof(rname), "%s_reset", name);
				rsym = qsys_dlsym(h, rname);
			}
			if (rsym) {
				int (*reset_fn)(void *);
				memcpy(&reset_fn, &rsym, sizeof(reset_fn));
				if (reset_fn(ctx) == 0) {
					reset_done = 1;
				}
			}
		}

		if (!reset_done) {
			/* Close existing context cleanly to free handles and internal maps */
			corm_axes_close(slot, name);

			/* Remove existing secondary index file(s) so stale deleted records are eliminated */
			if (unlink(spec) != 0 && errno != ENOENT) {
				snprintf(err_msg, err_sz, "reconciliation failed: cannot remove index file '%s': %s",
					spec, strerror(errno));
				g_index_degraded = 1;
				return -1;
			}
			char ridx[BUFSIZ + 16];
			snprintf(ridx, sizeof(ridx), "%s.ridx", spec);
			if (unlink(ridx) != 0 && errno != ENOENT) {
				snprintf(err_msg, err_sz, "reconciliation failed: cannot remove index file '%s': %s",
					ridx, strerror(errno));
				g_index_degraded = 1;
				return -1;
			}

			/* Re-bind fresh axis context */
			if (corm_axes_bind(slot, name) != 0) {
				snprintf(err_msg, err_sz, "reconciliation failed: cannot rebind axis '%s'", name);
				g_index_degraded = 1;
				return -1;
			}
		}

		/* If prim_hd has records, re-fanout each record into the fresh/reset axis */
		if (count > 0) {
			cur = corm_iter(prim_hd, NULL, 0);
			while (corm_next(&k, &v, cur)) {
				uint32_t ref = *(const uint32_t *)k;
				if (v) {
					int rc = corm_single_axis_store(slot, ref, v, strlen((const char *)v), CM_STR);
					if (rc != 0) {
						corm_fin(cur);
						snprintf(err_msg, err_sz, "reconciliation failed: axis '%s' failed to store ref %u",
							name, ref);
						g_index_degraded = 1;
						return -1;
					}
				}
			}
			corm_fin(cur);
		}
	}

	corm_save();
	g_index_degraded = 0;
	return 0;
}

static void
process_request(struct client *c, const char *hdr, const char *body, size_t body_len)
{
	char op[32] = {0};
	char req_id[64] = {0};
	char arg1[BUFSIZ] = {0};
	char arg2[BUFSIZ] = {0};
	size_t parsed_blen = 0;

	int tokens = sscanf(hdr, "%31s %63s %1023s %1023s %zu", op, req_id, arg1, arg2, &parsed_blen);

	if (!strcmp(op, "PUT") || !strcmp(op, "DEL") || !strcmp(op, "DEL_ALL") ||
	    !strcmp(op, "GET") || !strcmp(op, "LIST") || !strcmp(op, "QUERY") ||
	    !strcmp(op, "RECONCILE")) {
		if (arg1[0] && !check_database_match(arg1)) {
			respond_err(c, req_id, "database mismatch: daemon serving different database");
			return;
		}
	}

	if (!strcmp(op, "PING")) {
		buf_printf(&c->out_buf, &c->out_len, &c->out_cap, "PONG %s 0\n", req_id);
		return;
	}

	if (!strcmp(op, "RECONCILE")) {
		char err_msg[BUFSIZ] = {0};
		if (reconcile_secondary_indexes(err_msg, sizeof(err_msg)) != 0) {
			respond_err(c, req_id, err_msg[0] ? err_msg : "reconciliation failed");
		} else {
			respond_ok(c, req_id, NULL, 0);
		}
		return;
	}

	if (!strcmp(op, "STATS")) {
		struct rusage ru;
		getrusage(RUSAGE_SELF, &ru);
		time_t uptime = time(NULL) - start_time;

		/* Count records in primary handle */
		uint32_t count = 0;
		uint32_t cur = corm_iter(prim_hd, NULL, 0);
		const void *k, *v;
		while (corm_next(&k, &v, cur))
			count++;
		corm_fin(cur);

		char sbuf[256];
		int slen = snprintf(sbuf, sizeof(sbuf),
				"{\"records\":%u,\"rss_bytes\":%ld,\"uptime_sec\":%ld}\n",
				count, (long)(ru.ru_maxrss * 1024), (long)uptime);
		respond_ok(c, req_id, sbuf, slen);
		return;
	}

	if (!strcmp(op, "LIST")) {
		char *res_buf = NULL;
		size_t res_len = 0, res_cap = 0;
		uint32_t cur = corm_iter(prim_hd, NULL, 0);
		const void *k, *v;

		while (corm_next(&k, &v, cur)) {
			uint32_t ref = *(const uint32_t *)k;
			buf_printf(&res_buf, &res_len, &res_cap, "%u\n", ref);
		}
		corm_fin(cur);
		respond_ok(c, req_id, res_buf ? res_buf : "", res_len);
		free(res_buf);
		return;
	}

	if (!strcmp(op, "GET")) {
		/* arg1 = filespec, arg2 = ref */
		uint32_t ref = 0;
		if (!corm_write_ref(arg2, &ref)) {
			respond_ok(c, req_id, "-1\n", 3);
			return;
		}
		const void *val = corm_get(prim_hd, &ref);
		if (!val) {
			respond_ok(c, req_id, "-1\n", 3);
			return;
		}
		const char *sval = (const char *)val;
		size_t vlen = strlen(sval);
		char *out = malloc(vlen + 2);
		if (!out) {
			respond_err(c, req_id, "out of memory");
			return;
		}
		memcpy(out, sval, vlen);
		out[vlen] = '\n';
		out[vlen + 1] = '\0';
		respond_ok(c, req_id, out, vlen + 1);
		free(out);
		return;
	}

	if (!strcmp(op, "PUT")) {
		/* tokens: PUT req_id filespec ref body_len */
		uint32_t ref = 0;
		int have_ref = 0;
		if (arg2[0] && strcmp(arg2, ".") != 0 && strcmp(arg2, "0") != 0) {
			if (!corm_write_ref(arg2, &ref)) {
				respond_err(c, req_id, "invalid ref operand");
				return;
			}
			have_ref = 1;
		}
		char *payload = malloc(body_len + 1);
		if (!payload) {
			respond_err(c, req_id, "out of memory");
			return;
		}
		memcpy(payload, body, body_len);
		payload[body_len] = '\0';

		if (have_ref) {
			corm_put(prim_hd, &ref, payload);
		} else {
			ref = corm_put(prim_hd, NULL, payload);
		}
		int nfail = corm_fanout_store(ref, payload, body_len, CM_STR);
		corm_save();
		free(payload);
		if (nfail > 0) {
			g_index_degraded = 1;
			respond_err(c, req_id, "partial failure: axis index update failed");
			return;
		}
		char rstr[32];
		int rlen = snprintf(rstr, sizeof(rstr), "%u\n", ref);
		respond_ok(c, req_id, rstr, rlen);
		return;
	}

	if (!strcmp(op, "DEL")) {
		/* tokens: DEL req_id filespec ref 0 */
		uint32_t ref = 0;
		if (!corm_write_ref(arg2, &ref)) {
			respond_err(c, req_id, "invalid ref operand");
			return;
		}
		corm_del(prim_hd, &ref);
		int nfail = corm_fanout_unstore(ref);
		corm_save();
		if (nfail > 0) {
			g_index_degraded = 1;
			respond_err(c, req_id, "partial failure: axis index unstore failed");
			return;
		}
		respond_ok(c, req_id, NULL, 0);
		return;
	}

	if (!strcmp(op, "DEL_ALL")) {
		/* tokens: DEL_ALL req_id filespec [target] 0 */
		uint32_t ref = 0;
		int have_ref = 0;
		if (arg2[0] && strcmp(arg2, "all") != 0 && strcmp(arg2, "*") != 0 && strcmp(arg2, ".") != 0) {
			if (corm_write_ref(arg2, &ref))
				have_ref = 1;
		}
		if (have_ref) {
			corm_del_all(prim_hd, &ref);
			int nfail = corm_fanout_unstore(ref);
			corm_save();
			if (nfail > 0) {
				g_index_degraded = 1;
				respond_err(c, req_id, "partial failure: axis index unstore failed");
				return;
			}
			respond_ok(c, req_id, NULL, 0);
			return;
		}

		/* Delete all entries */
		int total_nfail = 0;
		uint32_t cur = corm_iter(prim_hd, NULL, 0);
		const void *k, *v;
		while (corm_next(&k, &v, cur)) {
			uint32_t r = *(const uint32_t *)k;
			total_nfail += corm_fanout_unstore(r);
		}
		corm_fin(cur);

		corm_drop(prim_hd);
		corm_save();

		if (total_nfail > 0) {
			g_index_degraded = 1;
			respond_err(c, req_id, "partial failure: axis index unstore failed");
			return;
		}
		respond_ok(c, req_id, NULL, 0);
		return;
	}

	if (!strcmp(op, "QUERY")) {
		if (g_index_degraded) {
			respond_err(c, req_id, "index degraded: previous mutation partially failed; reconciliation required");
			return;
		}
		/* tokens: QUERY req_id filespec limit body_len */
		top_k = (size_t)strtoul(arg2, NULL, 10);
		min_score = 0.0f;

		char * volatile copy = NULL;
		rec_set_t * volatile universe = NULL;
		rec_rank_t * volatile board = NULL;
		rec_ref_t * volatile refs = NULL;
		float * volatile scores = NULL;
		char * volatile res_buf = NULL;
		size_t res_len = 0, res_cap = 0;

		copy = malloc(body_len + 1);
		if (!copy) {
			respond_err(c, req_id, "out of memory");
			return;
		}
		memcpy(copy, body, body_len);
		copy[body_len] = '\0';

		if (setjmp(g_expr_jmp) != 0) {
			if (copy) { free(copy); copy = NULL; }
			if (board) { rec_rank_free(board); board = NULL; }
			if (refs) { free(refs); refs = NULL; }
			if (scores) { free(scores); scores = NULL; }
			if (res_buf) { free(res_buf); res_buf = NULL; }
			corm_expr_free_tree();
			if (universe) { rec_set_free(universe); universe = NULL; }
			respond_err(c, req_id, g_expr_err);
			return;
		}

		/* Split body into expression line and option lines */
		char *nl = strchr(copy, '\n');
		char *expr_line = copy;
		char *opts_start = NULL;
		if (nl) {
			*nl = '\0';
			opts_start = nl + 1;
		}

		/* Process option lines */
		if (opts_start && *opts_start) {
			char *saveptr = NULL;
			for (char *line = strtok_r(opts_start, "\r\n", &saveptr); line;
					line = strtok_r(NULL, "\r\n", &saveptr)) {
				if (!strncmp(line, "--", 2)) {
					char *eq = strchr(line + 2, '=');
					if (eq) {
						*eq = '\0';
						const char *opt_name = line + 2;
						const char *opt_val = eq + 1;
						if (!strcmp(opt_name, "top") || !strcmp(opt_name, "t")) {
							top_k = (size_t)strtoul(opt_val, NULL, 10);
						} else if (!strcmp(opt_name, "bottom") || !strcmp(opt_name, "b")) {
							min_score = (float)strtod(opt_val, NULL);
						} else if (!strcmp(opt_name, "rank")) {
							snprintf(rank_override, sizeof(rank_override), "%s", opt_val);
						} else {
							for (int p = 0; p < loaded_plugs_n; p++) {
								const rec_axis_cli_option_t *o = loaded_plugs[p].opts;
								for (int k = 0; o && o[k].name && k < CORM_CLI_PLUGIN_OPTS_MAX; k++) {
									if (!strcmp(o[k].name, opt_name))
										loaded_plugs[p].cfg(opt_name, opt_val);
								}
							}
						}
					}
				}
			}
		}

		corm_expr_parse(expr_line);
		free(copy);
		copy = NULL;

		if (!expr_root) {
			/* Empty expression: list all */
			uint32_t cur = corm_iter(prim_hd, NULL, 0);
			const void *k, *v;
			while (corm_next(&k, &v, cur)) {
				uint32_t ref = *(const uint32_t *)k;
				buf_printf((char **)&res_buf, &res_len, &res_cap, "%u %s\n", ref, (const char *)v);
			}
			corm_fin(cur);
			respond_ok(c, req_id, res_buf ? res_buf : "", res_len);
			if (res_buf) { free(res_buf); res_buf = NULL; }
			return;
		}

		ranker_axis = NULL;
		ranker_params = NULL;
		corm_expr_free_tree();

		universe = rec_set_new();
		rec_set_fill_corm_iter(universe, prim_hd);
		rec_set_seal(universe);

		rec_set_t *R = corm_expr_eval(expr_root, universe);
		if (!R) {
			rec_set_free(universe);
			universe = NULL;
			respond_err(c, req_id, "query evaluation produced no set");
			return;
		}

		size_t set_count = rec_set_count(R);
		size_t nout = 0;

		if (ranker_axis || rank_override[0]) {
			struct { const rec_axis_t *axis; void *params; } ri[EXPR_ARENA_CAP];
			int rin = 0;
			if (rank_override[0]) {
				int slot = corm_axes_find_slot(rank_override);
				const rec_axis_t *ax = slot >= 0 ? rec_axis_get(slot) : NULL;
				if (ax && ax->rank) {
					ri[rin].axis = ax;
					ri[rin].params = ax->decode ? ax->decode("") : (void *)"";
					rin++;
				}
			} else {
				for (int i = 0; i < expr_used; i++) {
					struct expr_node *nn = &expr_arena[i];
					if (nn->kind != E_LEAF)
						continue;
					int slot = corm_axes_find_slot(nn->name);
					const rec_axis_t *ax = slot >= 0 ? rec_axis_get(slot) : NULL;
					if (!ax || ax != ranker_axis || !ax->rank)
						continue;
					ri[rin].axis = ax;
					ri[rin].params = ax->decode ? ax->decode(nn->value) : (void *)(nn->value ? nn->value : "");
					rin++;
				}
			}

			size_t k = top_k ? top_k : set_count;
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
					if (ri[r].axis->rank((void *)ri[r].axis->ctx, ri[r].params, rrefs[i], &s) == 0
							&& (!got || s > bs)) {
						bs = s;
						got = 1;
					}
				}
				if (got && board)
					rec_rank_push(board, rrefs[i], bs);
			}
			if (board)
				nout = rec_rank_sorted(board, refs, scores);

			for (size_t i = 0; i < nout; i++) {
				const void *rec = corm_get(prim_hd, &refs[i]);
				if (!rec)
					continue;
				buf_printf((char **)&res_buf, &res_len, &res_cap, "%u %f %s\n", refs[i], scores[i], (const char *)rec);
			}
			if (board) { rec_rank_free(board); board = NULL; }
			if (refs) { free(refs); refs = NULL; }
			if (scores) { free(scores); scores = NULL; }
		} else {
			const rec_ref_t *rrefs = rec_set_at(R);
			size_t k = top_k ? (top_k < set_count ? top_k : set_count) : set_count;
			for (size_t i = 0; i < k; i++) {
				const void *rec = corm_get(prim_hd, &rrefs[i]);
				if (!rec)
					continue;
				buf_printf((char **)&res_buf, &res_len, &res_cap, "%u %s\n", rrefs[i], (const char *)rec);
			}
		}

		corm_expr_free_tree();
		if (universe) { rec_set_free(universe); universe = NULL; }

		respond_ok(c, req_id, res_buf ? res_buf : "", res_len);
		if (res_buf) { free(res_buf); res_buf = NULL; }
		return;
	}

	(void)tokens;
	respond_err(c, req_id, "unknown command");
}

/* ── Signal handling ──────────────────────────────────────────────────── */

static void
sig_handler(int sig)
{
	(void)sig;
	g_stop = 1;
}

/* ── Main Server Loop ─────────────────────────────────────────────────── */

static void
usage(const char *prog)
{
	fprintf(stderr,
		"Usage: %s [options]\n"
		"Options:\n"
		"  -d, --db SPEC      Database filespec (default: mem.db@joint,lemma:a:s)\n"
		"  -s, --socket PATH  Socket path (default: .pi/mm/cormd.sock)\n"
		"  -l, --lock PATH    Lock file path (default: .cormd.lock beside db)\n"
		"  -f, --foreground   Run in foreground (default)\n"
		"  -?, --help         Show this help\n",
		prog);
}

int
main(int argc, char *argv[])
{
	char db_opt[BUFSIZ] = "mem.db@joint,lemma:a:s";
	char sock_opt[BUFSIZ] = ".pi/mm/cormd.sock";
	char lock_opt[BUFSIZ] = "";

	static struct option long_opts[] = {
		{"db", required_argument, 0, 'd'},
		{"socket", required_argument, 0, 's'},
		{"lock", required_argument, 0, 'l'},
		{"foreground", no_argument, 0, 'f'},
		{"help", no_argument, 0, '?'},
		{0, 0, 0, 0}
	};

	int opt;
	while ((opt = getopt_long(argc, argv, "d:s:l:f?", long_opts, NULL)) != -1) {
		switch (opt) {
		case 'd': snprintf(db_opt, sizeof(db_opt), "%s", optarg); break;
		case 's': snprintf(sock_opt, sizeof(sock_opt), "%s", optarg); break;
		case 'l': snprintf(lock_opt, sizeof(lock_opt), "%s", optarg); break;
		case 'f': break;
		case '?':
		default:
			usage(argv[0]);
			return (opt == '?') ? EXIT_SUCCESS : EXIT_FAILURE;
		}
	}

	start_time = time(NULL);
	snprintf(db_filespec, sizeof(db_filespec), "%s", db_opt);
	snprintf(socket_path, sizeof(socket_path), "%s", sock_opt);

	/* Extract database directory from db_filespec for consistent lock derivation */
	char raw_db[BUFSIZ];
	snprintf(raw_db, sizeof(raw_db), "%s", db_filespec);
	char *at_sep = strchr(raw_db, '@');
	if (at_sep)
		*at_sep = '\0';
	char *col_sep = strchr(raw_db, ':');
	if (col_sep)
		*col_sep = '\0';

	char db_dir_buf[BUFSIZ];
	snprintf(db_dir_buf, sizeof(db_dir_buf), "%s", raw_db);
	char *db_dir = dirname(db_dir_buf);

	/* Determine lock file path */
	if (lock_opt[0]) {
		snprintf(lock_path, sizeof(lock_path), "%s", lock_opt);
	} else {
		snprintf(lock_path, sizeof(lock_path), "%s/.cormd.lock", db_dir);
	}

	/* Coordinate with .mm.lock: wait if active mutation holds it */
	char mm_lock_path[BUFSIZ];
	snprintf(mm_lock_path, sizeof(mm_lock_path), "%s/.mm.lock", db_dir);
	int held_pid = 0;
	int contention_timeout = 0;
	for (int attempts = 0; attempts < 40; attempts++) {
		struct stat st;
		if (stat(mm_lock_path, &st) != 0) {
			contention_timeout = 0;
			break;
		}
		/* Check if file has active pid */
		FILE *f = fopen(mm_lock_path, "r");
		if (!f) {
			contention_timeout = 0;
			break;
		}
		char lbuf[256] = {0};
		held_pid = 0;
		if (fgets(lbuf, sizeof(lbuf), f)) {
			char *pp = strstr(lbuf, "\"pid\":");
			if (pp)
				held_pid = atoi(pp + 6);
			if (held_pid > 0 && kill(held_pid, 0) == 0) {
				fclose(f);
				usleep(50000); /* 50ms */
				if (attempts == 39)
					contention_timeout = 1;
				continue;
			}
		}
		fclose(f);
		contention_timeout = 0;
		break;
	}

	if (contention_timeout) {
		fprintf(stderr, "cormd: timeout waiting for .mm.lock held by active pid %d; startup aborted\n", held_pid);
		return EXIT_FAILURE;
	}

	/* Ensure parent directory exists for socket and lock */
	char sock_dir_buf[BUFSIZ];
	snprintf(sock_dir_buf, sizeof(sock_dir_buf), "%s", socket_path);
	mkdir(dirname(sock_dir_buf), 0755);
	mkdir(db_dir, 0755);

	/* 1. Acquire exclusive lifetime non-blocking flock on .cormd.lock */
	lock_fd = open(lock_path, O_RDWR | O_CREAT, 0644);
	if (lock_fd < 0) {
		fprintf(stderr, "cormd: open %s failed: %s\n", lock_path, strerror(errno));
		return EXIT_FAILURE;
	}
	if (flock(lock_fd, LOCK_EX | LOCK_NB) != 0) {
		fprintf(stderr, "cormd: database leased by another daemon instance (%s): %s\n",
				lock_path, strerror(errno));
		close(lock_fd);
		return EXIT_FAILURE;
	}
	if (ftruncate(lock_fd, 0) == 0) {
		char sock_abs[BUFSIZ * 2];
		if (socket_path[0] == '/') {
			snprintf(sock_abs, sizeof(sock_abs), "%s", socket_path);
		} else {
			char cwd_buf[BUFSIZ];
			if (getcwd(cwd_buf, sizeof(cwd_buf)))
				snprintf(sock_abs, sizeof(sock_abs), "%s/%s", cwd_buf, socket_path);
			else
				snprintf(sock_abs, sizeof(sock_abs), "%s", socket_path);
		}
		dprintf(lock_fd, "{\"pid\":%d,\"owner\":\"daemon\",\"started\":%ld,\"socket\":\"%s\"}\n",
				(int)getpid(), (long)start_time, sock_abs);
	}

	/* 2. Open primary database and setup axes */
	prim_hd = gen_open(db_filespec, 0);
	corm_axes_setup();

	/* 3. Setup Unix Domain Socket */
	struct sockaddr_un addr;
	if (strlen(socket_path) >= sizeof(addr.sun_path)) {
		fprintf(stderr, "cormd: socket path too long (max %zu): %s\n",
				sizeof(addr.sun_path) - 1, socket_path);
		return EXIT_FAILURE;
	}
	memset(&addr, 0, sizeof(addr));
	addr.sun_family = AF_UNIX;
	strncpy(addr.sun_path, socket_path, sizeof(addr.sun_path) - 1);

	/* If socket already exists, test if active; clean up if dead */
	int test_fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (test_fd >= 0) {
		if (connect(test_fd, (struct sockaddr *)&addr, sizeof(addr)) == 0) {
			fprintf(stderr, "cormd: socket %s is already in use by active server\n", socket_path);
			close(test_fd);
			flock(lock_fd, LOCK_UN);
			close(lock_fd);
			return EXIT_FAILURE;
		}
		close(test_fd);
		unlink(socket_path);
	}

	server_fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (server_fd < 0) {
		fprintf(stderr, "cormd: socket create failed: %s\n", strerror(errno));
		return EXIT_FAILURE;
	}
	int flags = fcntl(server_fd, F_GETFL, 0);
	fcntl(server_fd, F_SETFL, flags | O_NONBLOCK);

	if (bind(server_fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
		fprintf(stderr, "cormd: bind %s failed: %s\n", socket_path, strerror(errno));
		close(server_fd);
		return EXIT_FAILURE;
	}
	if (listen(server_fd, 64) != 0) {
		fprintf(stderr, "cormd: listen failed: %s\n", strerror(errno));
		close(server_fd);
		return EXIT_FAILURE;
	}

	/* 4. Setup Signal Handlers */
	struct sigaction sa;
	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = sig_handler;
	sigaction(SIGTERM, &sa, NULL);
	sigaction(SIGINT, &sa, NULL);
	signal(SIGPIPE, SIG_IGN);

	/* Initialize clients table */
	for (int i = 0; i < MAX_CLIENTS; i++)
		clients[i].fd = -1;

	/* 5. Single-threaded poll() Event Loop */
	while (!g_stop) {
		struct pollfd pfds[MAX_CLIENTS + 1];
		int pfd_map[MAX_CLIENTS + 1];
		int nfds = 0;

		pfds[nfds].fd = server_fd;
		pfds[nfds].events = POLLIN;
		pfd_map[nfds] = -1; /* server */
		nfds++;

		for (int i = 0; i < MAX_CLIENTS; i++) {
			if (clients[i].fd >= 0) {
				pfds[nfds].fd = clients[i].fd;
				pfds[nfds].events = POLLIN;
				if (clients[i].out_len > clients[i].out_pos)
					pfds[nfds].events |= POLLOUT;
				pfd_map[nfds] = i;
				nfds++;
			}
		}

		int pr = poll(pfds, nfds, 1000);
		if (pr < 0) {
			if (errno == EINTR)
				continue;
			break;
		}

		/* Check server readable (incoming connections) */
		if (pfds[0].revents & POLLIN) {
			while (1) {
				int cfd = accept(server_fd, NULL, NULL);
				if (cfd < 0)
					break;
				int cflags = fcntl(cfd, F_GETFL, 0);
				fcntl(cfd, F_SETFL, cflags | O_NONBLOCK);

				int slot = -1;
				for (int i = 0; i < MAX_CLIENTS; i++) {
					if (clients[i].fd < 0) {
						slot = i;
						break;
					}
				}
				if (slot >= 0) {
					clients[slot].fd = cfd;
					clients[slot].in_len = 0;
					clients[slot].out_len = 0;
					clients[slot].out_pos = 0;
				} else {
					/* Over capacity: close immediately */
					close(cfd);
				}
			}
		}

		/* Check clients */
		for (int p = 1; p < nfds; p++) {
			int slot = pfd_map[p];
			if (slot < 0)
				continue;
			struct client *c = &clients[slot];

			/* Handle client writes */
			if (pfds[p].revents & POLLOUT) {
				if (c->out_len > c->out_pos) {
					ssize_t nw = write(c->fd, c->out_buf + c->out_pos, c->out_len - c->out_pos);
					if (nw > 0) {
						c->out_pos += (size_t)nw;
						if (c->out_pos == c->out_len) {
							c->out_pos = 0;
							c->out_len = 0;
						}
					} else if (nw < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
						client_close(c);
						continue;
					}
				}
			}

			/* Handle client reads */
			if (pfds[p].revents & (POLLIN | POLLHUP | POLLERR)) {
				char chunk[16384];
				ssize_t nr = read(c->fd, chunk, sizeof(chunk));
				if (nr <= 0) {
					if (nr < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
						continue;
					client_close(c);
					continue;
				}

				if (buf_append(&c->in_buf, &c->in_len, &c->in_cap, chunk, (size_t)nr) != 0) {
					client_close(c);
					continue;
				}

				/* Process buffered requests */
				while (c->in_len > 0) {
					char *nl = memchr(c->in_buf, '\n', c->in_len);
					if (!nl) {
						if (c->in_len > MAX_HEADER_SIZE) {
							/* Malformed header exceeding max allowed */
							client_close(c);
						}
						break;
					}

					size_t hdr_len = (size_t)(nl - c->in_buf);
					char hdr[MAX_HEADER_SIZE];
					if (hdr_len >= sizeof(hdr)) {
						client_close(c);
						break;
					}
					memcpy(hdr, c->in_buf, hdr_len);
					hdr[hdr_len] = '\0';

					/* Extract body length: last token of header */
					char *last_sp = strrchr(hdr, ' ');
					if (!last_sp) {
						client_close(c);
						break;
					}
					size_t body_len = (size_t)strtoul(last_sp + 1, NULL, 10);
					if (body_len > MAX_PAYLOAD_SIZE) {
						client_close(c);
						break;
					}

					size_t total_req_len = hdr_len + 1 + body_len;
					if (c->in_len < total_req_len)
						break; /* Need more body bytes */

					const char *req_body = c->in_buf + hdr_len + 1;
					process_request(c, hdr, req_body, body_len);

					/* Consume request from in_buf */
					size_t rem = c->in_len - total_req_len;
					if (rem > 0)
						memmove(c->in_buf, c->in_buf + total_req_len, rem);
					c->in_len = rem;
					c->in_buf[c->in_len] = '\0';
				}

				/* Try flushing response immediately */
				if (c->fd >= 0 && c->out_len > c->out_pos) {
					ssize_t nw = write(c->fd, c->out_buf + c->out_pos, c->out_len - c->out_pos);
					if (nw > 0) {
						c->out_pos += (size_t)nw;
						if (c->out_pos == c->out_len) {
							c->out_pos = 0;
							c->out_len = 0;
						}
					}
				}
			}
		}
	}

	/* 6. Orderly Teardown */
	corm_save();

	for (int i = 0; i < MAX_CLIENTS; i++)
		client_close(&clients[i]);

	if (server_fd >= 0) {
		close(server_fd);
		unlink(socket_path);
	}
	if (lock_fd >= 0) {
		flock(lock_fd, LOCK_UN);
		close(lock_fd);
	}

	return EXIT_SUCCESS;
}
