#ifndef TTYPT_REC_H
#define TTYPT_REC_H

/**
 * @file rec.h
 * @brief Recall kernel: uniform candidate sets + a generic ranking loop.
 *
 * Pure C, no domain math. Candidate sets are sorted, deduplicated lists of
 * 32-bit refs (merge-join, no hashing). A ref IS a corm record reference
 * (e.g. the id returned by corm_put() under CM_AINDEX) — the same
 * primary-key space every corm-backed store shares, never an axis's own
 * internal key. An axis may index by whatever it needs internally (e.g.
 * libislet's 64-bit morton keys); those internal keys are not refs and
 * never leave the axis. The ranking loop is a bounded, streaming min-heap:
 * an axis streams refs + scores straight into rec_rank_push; a set is
 * materialized only when a second axis needs to join. Weighted composition
 * across axes lives in the consumer's score function.
 *
 * Kernel is optional and additive: raw corm / domain entry points in the
 * axis libraries are untouched. Ref mapping is the consumer's job — the
 * kernel never interprets a ref.
 *
 * Adapter contract: an axis exposes ONE filler
 *   int rec_axis_fill_*(<domain query args>, rec_set_t *out);
 * that streams the refs matching its filter into the set and seals it
 * (0 ok / -1 error). Fill-only queries stay streaming (the axis pushes
 * straight into rec_rank_push); a set is materialized only when a second
 * axis or ranker joins. Approximate fills (e.g. ANN neighbors) must
 * declare themselves, since intersecting with an approximate set bounds
 * final recall by it. Full design: docs/RECALL-KERNEL.md.
 *
 * @see corm.h
 */
#include <stdint.h>
#include <stddef.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef uint32_t rec_ref_t;

/** @defgroup rec_set Candidate sets
 *  @{
 */

typedef struct rec_set rec_set_t;

/** Create an empty, unsorted set (arena-backed, grows on demand). */
rec_set_t *rec_set_new(void);

/** Append a ref. Unseals a previously sealed set. */
void rec_set_push(rec_set_t *s, rec_ref_t r);

/** Sort + dedup in place. Joins require sealed inputs. */
void rec_set_seal(rec_set_t *s);

/** Drain a corm handle's iteration into the set (keys must be fixed-length,
 *  <= 4 bytes; refs are read from the key bytes). Excludes nothing; call
 *  rec_set_seal afterwards. Returns 0 on success, -1 if keys are variable
 *  length or wider than a ref. */
int rec_set_fill_corm_iter(rec_set_t *s, uint32_t hd);

/** dst := a ∩ b. a and b must be sealed; dst content is discarded. */
int rec_set_intersect(rec_set_t *dst, const rec_set_t *a, const rec_set_t *b);

/** dst := a − b. a and b must be sealed; dst content is discarded. */
int rec_set_subtract(rec_set_t *dst, const rec_set_t *a, const rec_set_t *b);

/** dst := a ∪ b. a and b must be sealed; dst content is discarded. */
int rec_set_union(rec_set_t *dst, const rec_set_t *a, const rec_set_t *b);

/** Number of refs currently held (works before and after sealing). */
size_t rec_set_count(const rec_set_t *s);

/** Get a pointer to the backing array (sealed → sorted, deduped). */
const rec_ref_t *rec_set_at(const rec_set_t *s);

/** Release the set's storage. */
void rec_set_free(rec_set_t *s);

/** Exactness of a filled set. Approximate fills (e.g. ANN neighbors) must
 *  declare themselves: intersecting with an approximate set bounds final
 *  recall by the approximate one. Default for every set: exact, bound 1.0. */
enum rec_set_exactness {
	REC_SET_EXACT = 0, /**< every matching ref is reported */
	REC_SET_APPROX = 1 /**< bounded candidate window m; owed recall@k < 1 */
};

/** Mark the set approximate/exact and its owed recall@k bound (0 < bound
 *  <= 1). Exact sets carry bound 1.0. Returns 0 on success, -1 when the
 *  set is NULL or the bound is outside (0,1] (the flag is left unchanged).
 *  Joins propagate exactness: intersect/union produce an approximate set
 *  when either operand is (bound = min); subtract keeps the left operand's
 *  flag and bound (the result is a subset of it). */
int rec_set_set_approx(rec_set_t *s, int approx, float recall_bound);

/** 1 if the set is approximate (REC_SET_APPROX), 0 if exact (default). */
int rec_set_approx(const rec_set_t *s);

/** Owed recall@k bound (default 1.0 for exact sets). */
float rec_set_recall_bound(const rec_set_t *s);

/** @} */

/** @defgroup rec_rank Ranking loop
 *  @{
 */

typedef struct rec_rank rec_rank_t;

/** Score function over a ref. Returns 0 on success, nonzero on failure
 *  (ref skipped). score is written regardless on success. */
typedef int (*rec_score_fn)(void *ud, rec_ref_t r, float *score);

/** Create a ranking buffer bounded to top_k results (top_k == 0 → NULL).
 *  min_score drops refs below the threshold at push time. */
rec_rank_t *rec_rank_new(size_t top_k, float min_score);

/** Stream a scored candidate into the buffer. O(log top_k) when bounded. */
void rec_rank_push(rec_rank_t *rk, rec_ref_t r, float score);

/** Write the retained results sorted best-first (desc score; ties by asc
 *  ref) into caller arrays sized >= top_k. Returns the count written. */
size_t rec_rank_sorted(const rec_rank_t *rk, rec_ref_t *refs, float *scores);

/** Release the ranking buffer. */
void rec_rank_free(rec_rank_t *rk);

/** @} */

/** @defgroup rec_axis Pluggable axis registry + query engine
 *  @{
 */

/** Maximum concurrently registered axes in the registry. */
#define REC_QUERY_MAX_AXES 8

/** Fill dispatch compatible with the rec_axis_fill_* adapter shape: stream
 *  the refs matching (ctx, params) into `out` and seal it. 0 ok / -1 error. */
typedef int (*rec_fill_fn)(void *ctx, void *params, rec_set_t *out);

/** Rank one ref from the filled set: write the score, return 0 (nonzero
 *  skips the ref). */
typedef int (*rec_rank_fn)(void *ctx, void *params, rec_ref_t ref,
                           float *score);

/** A registered axis. `name` is introspection only (never interpreted);
 *  `params` structs are axis-owned; `ctx` is NULL until rec_axis_set_ctx. */
typedef struct rec_axis {
	char     name[16];  /**< introspection only; never interpreted */
	rec_fill_fn fill;   /**< NULL allowed (rank-only axis) */
	rec_rank_fn rank;   /**< NULL allowed (filter-only axis) */
	void    *ctx;       /**< NULL until rec_axis_set_ctx() */
	void   *(*decode)(const char *s); /**< optional opaque param parser */
} rec_axis_t;

/** One axis's score for a ref, fed to the consumer score fn. */
typedef struct rec_axis_score {
	int   slot;
	float score;
	int   valid;        /**< 0 if no rank fn or rank skipped the ref */
} rec_axis_score_t;

/** Consumer score fn: combine the per-axis scores (all axes in query order,
 *  regardless of join — weights are the consumer's job) into out_score.
 *  Return 0 to admit the ref into the ranking buffer. */
typedef int (*rec_consumer_score_fn)(
	void *ud, rec_ref_t ref,
	const rec_axis_score_t *as, int n_as, float *out_score);

/** Boolean join between an axis fill and the running query result. */
typedef enum {
	REC_JOIN_AND = 0,   /**< intersect with the running set (default) */
	REC_JOIN_OR  = 1,   /**< union with the running set */
	REC_JOIN_NOT = 2    /**< subtract from the running set */
} rec_join_t;

/** One axis in a query: slot + opaque params (axis owns the struct) + join. */
typedef struct rec_query_axis {
	int        slot;
	void      *params;
	rec_join_t join;    /**< ignored for the first axis (it seeds) */
} rec_query_axis_t;

/** A full multi-axis query. */
typedef struct rec_query {
	int                   n_axes;   /**< 0..REC_QUERY_MAX_AXES */
	rec_join_t            combine;  /**< global shorthand for axes > seed;
	                                 REC_JOIN_AND (0) = honor per-axis join */
	rec_query_axis_t      axes[REC_QUERY_MAX_AXES];
	size_t                top_k;
	float                 min_score;
	rec_consumer_score_fn consumer_score; /**< NULL → first rank-capable axis */
	void                 *consumer_ud;
} rec_query_t;

/** Register an axis (copied). Returns its slot 0..REC_QUERY_MAX_AXES-1,
 *  or -1 when the registry is full or `axis` is NULL. */
int rec_axis_register(const rec_axis_t *axis);

/** Look up a registered axis; NULL when the slot is invalid. */
const rec_axis_t *rec_axis_get(int slot);

/** Decode an opaque param string via the axis's decode fn (NULL when the
 *  axis has none, the slot is invalid, or s is NULL). */
void *rec_axis_decode(int slot, const char *s);

/** Two-phase init: drop the store handle into a registered slot. */
int rec_axis_set_ctx(int slot, void *ctx);

/** Number of currently registered axes. */
int rec_axis_count(void);

/** Run a query: fill each axis, fold the sets into the running result
 *  (first filled axis seeds R; AND → intersect, OR → union, NOT → subtract
 *  left-to-right; q.combine != REC_JOIN_AND overrides the per-axis join for
 *  all axes after the seed), then rank. `refs` must be sized ≥ top_k (or ≥
 *  the result count in pure-filter mode); `scores` may be NULL in
 *  pure-filter mode (no axis has a rank fn and no consumer is set). Returns
 *  the number of results, 0 when nothing matched, -1 on invalid input or
 *  allocation failure. Re-entrant; no global scratch. */
int rec_query_run(const rec_query_t *q, rec_ref_t *refs, float *scores);

/** @} */

/** @defgroup rec_axis_cli Axis CLI contract (D14/D15B)
 *  The kernel owns the axis-CLI ABI and the decode-spec grammar exactly
 *  once (implementation: libcorm src/rec_cli.c). An axis implements only
 *  its option table (rec_axis_cli_options) and its per-field mapping in
 *  rec_axis_config_arg / decode. Layouts are visible to every TU via this
 *  header — never re-declared locally.
 *  @{
 */

/** One --NAME=VALUE option a plugin contributes to the kernel CLI
 *  (getopt-compatible: --NAME requires --NAME=VALUE when has_arg != 0). */
struct rec_axis_cli_option {
	const char *name;
	int has_arg;
	const char *help;
};
typedef struct rec_axis_cli_option rec_axis_cli_option_t;

/** Read-only pass over a decode spec (the kernel-owned decode grammar,
 *  ZERO allocation — it never writes the buffer):
 *  Keys AND values are returned as length-delimited ranges into the
 *  CALLER'S string: a bare value runs from *val to the next space / tab /
 *  '=' / NUL; a single-quoted value is returned as *val past the opening
 *  quote with the RAW interior bytes spanning *val .. *val + *vlen and
 *  *quoted set (backslash escapes stay encoded — use rec_cli_str_dup to
 *  unescape into an owned copy). Tokens without '=' are skipped; every
 *  1-return yields one key=value pair (key may be empty, e.g. "=x").
 *  Pair with the rec_cli_*_b range parsers so a decode never needs to
 *  copy the spec:
 *      for (const char *cur = spec;
 *           rec_spec_scan(&cur, &k, &klen, &v, &n, &q); )
 *          if (rec_key_eq(k, klen, "dim"))
 *              if (!rec_cli_int_b(v, v + n, &i)) …
 *  Returns 1 while a pair was produced, 0 at the end. */
int rec_spec_scan(const char **cur, const char **key, size_t *klen,
                  const char **val, size_t *vlen, int *quoted);

/** Exact compare of a length-delimited range against a constant:
 *  `key` is [key, key+klen) (rec_spec_scan never NUL-terminates), so
 *  plain strcmp can overrun the range; this is the safe form. */
static inline int
rec_key_eq(const char *key, size_t klen, const char *lit)
{
	size_t n = strlen(lit);
	return n == klen && memcmp(key, lit, n) == 0;
}

/** Owned string setter for rec_axis_config_arg string fields: strdup then
 *  free-replace into *dst. 0 ok / -1 on NULL dst/value or allocation. */
int rec_cli_str_set(char **dst, const char *value);

/** Strict decimal parsers for rec_axis_config_arg numeric fields:
 *  errno-cleared strto*, reject empty/trailing-junk/overflow, and a
 *  leading '-' for the unsigned ones. */
int rec_cli_int(const char *v, int *out);
int rec_cli_uint(const char *v, unsigned *out);
int rec_cli_size(const char *v, size_t *out);
int rec_cli_float(const char *v, float *out);

/** Bounded variants for the read-only value ranges rec_spec_scan hands
 *  out: parse [v, v_end), where the byte at v_end is the range's
 *  boundary (space / tab / '=' / quote / NUL) and so can never be part
 *  of a numeric token. Identical strictness to the NUL-terminated
 *  parsers, which are these on v..v+strlen(v). */
int rec_cli_int_b(const char *v, const char *v_end, int *out);
int rec_cli_uint_b(const char *v, const char *v_end, unsigned *out);
int rec_cli_size_b(const char *v, const char *v_end, size_t *out);
int rec_cli_float_b(const char *v, const char *v_end, float *out);

/** Duplicate the value range [v, v+n) into a fresh NUL-terminated string;
 *  when quoted, unescape IN PLACE ('\\'→'\', '\''→'\'' like the scan
 *  grammar; a trailing lone '\' stays literal). 0 ok / -1 on NULL or
 *  allocation failure. The one allocation a string-field decode needs
 *  (query/field/file). */
int rec_cli_str_dup(const char *v, size_t n, int quoted, char **out);

/** @} */

#ifdef __cplusplus
}
#endif

#endif /* TTYPT_REC_H */