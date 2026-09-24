# The Recall Kernel — `rec.h`

A domain-free **"filter by axis → join → rank"** loop over uniform 32-bit
corm refs. The kernel lives in this library (`include/ttypt/rec.h`,
`src/rec.c`). It knows nothing about time, space, text, or meaning: it
collects refs, combines candidate sets, and keeps the best-ranked results.
Axis libraries stay independent domain stores and feed the kernel through
small **adapters**; consumers compose axes without re-writing join/rank code.

The `rec_query` engine (below) turns that composition into a registry-driven
query object with `dlopen`-loaded axis plugins — so the kernel composes axes
without the consumer hand-writing the fill/join/rank loop.

- **Optional and additive.** Raw corm / axis entry points are untouched; the
  kernel and engine are convenience layers, never mandatory wrappers.
- **Pure C, no domain math.** Weighted composition across axes lives in the
  consumer's score function.
- **Ref mapping is the consumer's job.** The kernel never interprets a ref.

## The pattern

Every search axis says one thing: *"these refs match my filter."* Searches
are:

1. **Filter** — each axis pours matching refs into a candidate set.
2. **Join** — combine axes (union, intersect, subtract) to keep refs that
   survive every applied axis.
3. **Rank** — score each surviving ref and keep the best `k`.

```c
rec_set_t  *cands = rec_set_new();
rec_rank_t *board = rec_rank_new(10, 0.4f);

rec_axis_fill_bbox(geo_db, s, l, 3, cands);        /* filter:  space    */
rec_axis_fill_interval(it_db, a, b, cands);        /* filter:  time     */
/* cands is now the intersection (in-box AND in-interval), sealed.        */

for (size_t i = 0; i < rec_set_count(cands); i++) {
    rec_ref_t ref = rec_set_at(cands)[i];
    float score;
    if (consumer_rank(ref, &score) == 0)          /* rank:    meaning  */
        rec_rank_push(board, ref, score);          /* top-10 kept        */
}
rec_rank_sorted(board, best_refs, best_scores);    /* best first */
```

## Building blocks

### Candidate sets — `rec_set_t`

An arena-backed list of `rec_ref_t` (uint32 — a corm ref, never an axis's
own internal key). Append with `rec_set_push`,
then **seal** (sort + dedup) before any join; joins are sorted **merge-joins
— no hashing** and produce sealed output.

```c
rec_set_t *s = rec_set_new();
rec_set_push(s, ref);               /* append, unsorted while filling */
rec_set_seal(s);                    /* sort + dedup in place          */
rec_set_intersect(d, a, b);         /* d := a ∩ b  (a,b sealed)       */
rec_set_union(d, a, b);             /* d := a ∪ b  (a,b sealed)       */
rec_set_subtract(d, a, b);          /* d := a − b  (a,b sealed)       */
size_t          n = rec_set_count(s);
const rec_ref_t *refs = rec_set_at(s);   /* sealed → sorted, deduped  */
rec_set_free(s);
```

A push **unseals** the set — append more and seal again. Any corm sorted
handle with fixed-length keys ≤ 4 bytes can be drained directly with
`rec_set_fill_corm_iter(s, hd)`.

### Ranking buffers — `rec_rank_t`

A bounded **top-k min-heap** with a score threshold. Stream scored
candidates in; only the best `k` survive; `rec_rank_sorted` writes them
best-first (desc score, ties by asc ref).

```c
typedef int (*rec_score_fn)(void *ud, rec_ref_t r, float *score);

rec_rank_t *board = rec_rank_new(top_k, min_score);  /* top_k==0 → NULL */
rec_rank_push(board, ref, score);                    /* O(log top_k)    */
size_t n = rec_rank_sorted(board, refs, scores);
rec_rank_free(board);
```

### Rank convention (D2, locked 2026-09-15)

When an axis composition is scored, the **first rank-capable axis in
query order wins** — the first axis in the composition that has a `rank`
fn + `ctx` supplies *the* score for every surviving ref; the other axes
contribute membership only. The rule is deliberately simple, so a
multi-axis query's score is predictable and axis order is the only knob.
Both implementations agree:

- **CLI tree eval** (`src/corm.c`, `corm_expr_eval` `E_LEAF`): the first
  leaf axis with `axis->rank` seen in **preorder** records the ranker; a
  composition whose every leaf is filter-only renders **pure-filter** (no
  scores, asc-ref order).
- **Kernel `rec_query_run` fallback** (`src/rec_axis.c:199-211`): with
  `consumer_score == NULL`, the first axis in `q->axes[]` order that has
  `rank` + `ctx` scores the run.

Combining across rankers (`--score` sum/max/weighted, riding
`rec_consumer_score` + `rec_axis_score_t[]`) is **deferred**. Pinned
end-to-end in `test-real.sh` (two rank-capable axes in one `-X`
expression → first-in-preorder stoma ranks, not sepal cosine) and
`test-cli.sh` (`EXCEPT` chains left-to-right).

**D15 (labeled instances):** the same axis may appear more than once as
`label:axis` leaves. Rank-capable instances of one axis then aggregate as
the **per-ref MAX score** (order-independent — `(A:stoma OR B:stoma)`
ranks ref N by `max(scoreA, scoreB)`); distinct axes still keep the D2
first-rankable-in-preorder rule. `--rank=A` (or the scoped `--rank@A`) pins
the ranker to exactly the labeled instance A, erroring when that instance's
axis has no `rank` fn.

## Kernel vs axes

| Owns | The kernel | An axis (libjoint, libislet, stoma, …) |
|---|---|---|
| Storage | — (no domain data) | its domain store (interval DB, morton index, inverted index) |
| Candidates | `rec_set_t` (fill/intersect/subtract/union/count) | its filter result, poured into a set via its adapter |
| Ranking | `rec_rank_t` loop, min-score, top-k, stable sort | its score function (recency, distance, FTS score, cosine) |
| Query composition | a `rec_set_t` + score fn = a query | nothing — axes only answer "which refs match" |

## The adapter contract

An axis exposes **one** function that streams the match results of a domain
query into a sealed set:

```c
int rec_axis_fill_*(<domain query args>, rec_set_t *out);
```

Rules (the contract):

1. **Signature** — returns plain `int` (0 ok / −1 error), consistent with
   `rec.h`'s `int`-return convention. No bespoke result enums.
2. **Streams, does not materialize.** The adapter pushes refs as it walks;
   it never builds an intermediate result array (e.g. a full box-volume).
   Sparse queries over huge domains stay cheap.
3. **Seals.** The adapter leaves `out` sealed (sorted, deduped) so the next
   adapter can merge-join against it. A ref reachable through several
   matches enters the set once.
4. **Additive.** Raw axis entry points remain; the adapter is optional.
5. **The ref is the axis's native id, widened.** libislet stores uint32 cell
   values and widens them to `rec_ref_t` at fill time; stoma fills the
   caller's decimal row ids (`rec_axis_fill_tokens` pushes `strtoull`
   of the row id, `stoma_rank` reverses it). The kernel never interprets a
   ref; mapping back to a consumer schema is the consumer's job.

## Streaming vs sets

Filter-only queries **never materialize a set**: a single axis can stream
straight into `rec_rank_push` via its own walker. A set is built only when a
second axis or a ranker needs a join. The adapter functions above always
write into a set because that is their contract; raw streaming is available
on the axis's own iterators.

## Exact vs approximate fills

Most fills are **exact**: every matching ref is reported, so intersecting
with anything keeps exact results. Approximate axes (e.g. ANN/HNSW semantic
neighbors) report a bounded candidate window `m` and cannot promise full
recall. An approximate fill must **declare itself** (a flag on the set plus
owed recall@k — implemented: `rec.h` `rec_set_set_approx()` /
`rec_set_approx()` / `rec_set_recall_bound()`; joins propagate: intersect/
union → approximate when either operand is, bound = min; subtract keeps the
left operand's flag), because intersecting with an approximate set bounds
final recall by the approximate one — `m` is the visible knob.

| Axis | Fill | Rank | Exact? |
|---|---|---|---|
| libjoint | interval membership `[a,b)` | optional recency | exact |
| libislet | bbox membership | optional distance | exact |
| stoma | lexical token set | FTS score | exact |
| libsepal (ANN) | **approximate** Hamming top-m | exact cosine | approximate (flag) |

## Query engine & plugin registry (`rec_query`)

The `rec_query` engine is the composition layer the kernel's pattern implies.
It is a registry-driven query running wholly inside libcorm with **zero axis
dependencies**: integer slots, opaque `void*` params and ctx, and a name
string used for introspection only. See `mm-plan/PHASE-2-CLI.md` (site repo)
for how this composes into the general corm CLI surface.

### The axis object

```c
typedef int (*rec_fill_fn)(void *ctx, void *params, rec_set_t *out);
typedef int (*rec_rank_fn)(void *ctx, void *params, rec_ref_t ref, float *score);

typedef struct rec_axis {
    char        name[16];                         /* introspection only */
    rec_fill_fn fill;                             /* NULL = rank-only  */
    rec_rank_fn rank;                             /* NULL = filter-only*/
    void       *ctx;                              /* NULL until set    */
    void      *(*decode)(const char *s);          /* optional CLI parse */
} rec_axis_t;
```

### Two-phase registration

An axis `.so` registers itself from a constructor:

```c
__attribute__((constructor))
static void reg(void) {
    static const rec_axis_t a = { "time", time_fill, NULL, NULL, time_decode };
    rec_axis_register(&a);          /* → slot; ctx still NULL */
}
```

The consumer loads the `.so` with `dlopen` (the constructor runs and fills
the libcorm registry) and then binds a live store handle after opening it:

```c
int slot = rec_axis_register(...);        /* from the constructor, or looked up */
rec_axis_set_ctx(slot, store_handle);
```

#### The `rec_axis_open` convention (optional, CLI-specific — not core API)

An axis `.so` **may** additionally export a plain C symbol:

```c
void *rec_axis_open(const char *spec);
```

which opens whatever store the axis needs from an opaque spec string (a
path, a `path:opts` string, several `key=val` sub-specs joined by a
delimiter — entirely the axis's own business) and returns the ctx pointer
a consumer then passes to `rec_axis_set_ctx()`. **Persistence is axis-decided
(D13):** `spec` names the *logical* axis (`<primary-dir>/<primary>-<name>`,
e.g. `garden.db-joint` for `garden.db`); an
axis that is derived rebuilds from the primary at each open (e.g. `stoma`,
optionally `joint`/`islet`), a persisted one uses the file (`sepal` always).
Fanout in 2B-4 writes to whatever `ctx` the bound axis represents. The mask
(`corm_open(...,mask,flags)`, `corm.h:204`) is only an initial hint
(`QDBE_MASK` `4095`, `2^12-1` — D11, env `CORM_MASK` overrides; auto-grow on
overflow unless `CM_NOGROW`). This is **not** part of the `rec_query`
registry API — libcorm never declares, exports, or calls `rec_axis_open`
itself; it is purely a naming convention a `dlopen`-based consumer (the
composed corm CLI) relies on to bind a freshly-loaded axis without
per-axis-specific glue code: the CLI hands each axis an **alongside-default
spec** derived from the primary store's location (`<primary-dir>/<primary>-<name>`,
or the axis's own memory/empty-field defaults — the axis table below) and
the axis's opener interprets it in its own grammar. There is **no per-call
load/open flag** — no `--dl`, no `--open`; discovery is by name only. Keeps
the "zero axis dependency" invariant intact: this is CLI↔axis-plugin
convention, not kernel-declared API.

#### The `rec_axis_store` / `rec_axis_store_typed` / `rec_axis_unstore` / `rec_axis_readback` conventional exports (optional, CLI-specific — not core API)

Phase 2A (site `mm-plan/PHASE-2-CLI.md`) adds the write/delete/read-back half
of the same convention. An axis `.so` **may** additionally export:

```c
int rec_axis_store(void *ctx, const char *spec, rec_ref_t ref,
                   const char *value);
int rec_axis_store_typed(void *ctx, const char *spec, rec_ref_t ref,
                         const void *blob, size_t len, uint32_t qtype);
int rec_axis_unstore(void *ctx, rec_ref_t ref);
int rec_axis_readback(void *ctx, rec_ref_t ref, char **blob_out, size_t *n_out);
```

> **Name note (locked 2026-09-14, U2):** the read-back symbol is
> `rec_axis_readback`, *not* `rec_axis_get` — `rec_axis_get(int slot)` is
> the kernel registry lookup (`rec.h`), already landed and used by corm.c /
> the axis suites, so the plugin read-back must not collide with it.

**Axis autonomy (the governing principle):** the consumer passes `(ref,
value)` blindly; what each axis does on put, delete, and get is entirely the
axis's own decision, in its own domain grammar. The consumer holds no
per-axis knowledge.

**No split.** The consumer passes the **whole record string verbatim**; corm
never decomposes it — no colon split, no key/value extraction, no
interpretation. Each axis parses the **entire** string itself, with its own
grammar. (This is the whole intent of the store surface: one `-p "<string>"`
fan-outs that one string to every axis, and each axis pulls out of it exactly
what it understands.)

Uniform contract (all axes, decided 2026-09-13; single-whole-string form
confirmed 2026-09-14; D11..D13 pre-2B-4):

- `rec_axis_store` records `ref` and derives its index from `value`, the
  whole record string as given. Returns 0 on success, −1 on error (`errno` =
  `EINVAL` bad grammar/args, `ERANGE` out of domain). `value` is the string
  as written on the command line (whole, un-split); `spec` is reserved for
  per-call params, currently NULL (never credentials).
- Additive typed path (D12): `rec_axis_store_typed` is the same, but for
  binary primaries — `(blob,len,qtype)` from `corm_get`+`corm_type_len(qtype)`
  (`corm.h:241`). An axis implements both and keeps the string entry point
  (shipped `*.so`s stay valid); the CLI prefers the typed symbol when
  `vtype != CM_STR`, else the string one. Missing typed export ⇒ text-only
  axis. `readback` already returns `blob/n` (binary-capable).
- **The primary value format is not strict; axes are.** The primary map
  stores whatever string it is given — any format is valid there. An axis is
  never forced to understand an unexpected format: it parses the whole string
  in its own grammar and rejects (loud, `EINVAL` axis-side) anything outside
  it, while other axes may still accept the same string. The CLI does not
  validate a string against any axis before storing it.
- `rec_axis_unstore` removes *every* entry the store owns for `ref`
  (content and derived index). **Idempotent**: unstore of an absent ref
  returns 0 (this is the documented compensation primitive for a partial
  write-fanout). `ctx == NULL` → −1.
- `rec_axis_readback` reads back the entries the store owns for `ref`: one
  malloc'd buffer of NUL-joined entry display strings, entry count out. The
  consumer's `-g`/`-m`/`-c` render first/all/count from it.
- All three are optional like `rec_axis_open`; an axis missing store/unstore
  is **read-only** (missing get: no read-back). Not declared, exported, or
  called by libcorm — same CLI↔axis-plugin convention, same
  zero-axis-dependency invariant.
- Surface rules (uniform, consumer-enforced): axis ops require an `a`-type
  primary (refs must exist — the ref-type law, enforced); axis values come
  from `s`/`u`-origin strings (decimals rendered canonically). Notation
  validation is axis-side and loud.
- Persistence follows each axis's own store (single-writer, no-close
  invariant: consumer processes never close axis stores; libcorm's exit-time
  destructor save persists everything still open). A store write must leave
  the file consistent after every mutating op (crash-consistency).
- Writes go through exactly the same ref (`rec_ref_t`, u32, §2): the ref
  crosses the boundary as the record's id; internal keys never leave the
  axis.

#### The `rec_axis_cli_options` / `rec_axis_config_arg` convention (optional, CLI-specific — not core API)

corm (not libcorm) lets an axis `.so` declare its own CLI long-options —
whole-sentence or runtime values that do not belong in the `-X` expression
structure or the roster. An axis **may** additionally export:

```c
/* kernel-owned ABI — ttypt/rec.h, visible to every TU, never re-declared */
struct rec_axis_cli_option { const char *name; int has_arg; const char *help; };
const struct rec_axis_cli_option *rec_axis_cli_options(void);
int rec_axis_config_arg(const char *name, const char *value);
```

(`struct option`-shaped. The decode-spec grammar an axis parses —
space-separated `key=value` with single-quoted values — is likewise owned
once by the kernel: `ttypt/rec.h rec_spec_scan` (read-only, zero-alloc,
values returned as length-delimited ranges; pair with `rec_cli_*_b`). An
axis implements only its option table plus its per-field mapping in `rec_axis_config_arg` /
decode, routing string/numeric values through the shared
`rec_cli_str_set` / `rec_cli_int|uint|size|float` helpers.)

Contract:

- corm collects inline `--name=value` tokens (mirror scan before getopt;
  `-X` stays the only query *verb*), and — after every bound axis is
  connected — broadcasts each to every loaded `.so` whose
  `rec_axis_cli_options()` declares that name, via `rec_axis_config_arg`.
- `rec_axis_cli_options()` returns a NULL-`name`-terminated table;
  `has_arg` 1 = takes a value, 0 = bare flag.
- `rec_axis_config_arg(name, value)` returns 0 on accept, −1 on reject
  (unknown name, NULL value, bad range/format). corm turns a reject, an
  undeclared name, a bare `--name` for a value option, or a valued
  `--flag=x` into usage + exit 1.
- **Inline `--name=value` only** (no space form); max 16 collected options.
- **Flags-first surface (2026-09-16):** `-X` is structure only. The old
  `=VALUE` leaf grammar is gone — a hand-written `NAME=VALUE` inside `-X` is
  a hard parse error whose message points at flags. The leaf value still
  exists as **internal transport only**: scoped `--name@label` /
  `--name@axis` meet per-leaf params by synthesizing `name='value'` into the
  target leaf's decode spec (quoting/escaping internal, not user-writable
  grammar). This is what preserved zero plugin-API changes; delete
  `n->value`/decode and you are redesigning the plugin boundary, not
  finishing this flag.
- Unscoped `--name=value` **broadcasts** to every bound axis whose
  `rec_axis_cli_options()` declares `name` — a shared value, not a
  per-instance one. To target a single instance use `@label`/`@axis`
  (`@label` beats `@axis` on double scope); an axis must not hard-abort a
  shared broadcast it cannot interpret.
- Precedence: **leaf spec > `@label` > `@axis` > unscoped > env**.
  Credentials stay env-only — never CLI flags.
- The axis merges the CLI state in its own decode (what it omits stays
  omitted); a bare leaf may fall back to CLI state (e.g. bare `sepal` +
  `--file`), or stay unsearchable (bare `stoma` stays `NULL`). joint's
  per-end chain is **leaf `a`/`b` > `--since`/`--until` > `--query` >
  unset**; `--query` accepts a point timestamp (widened to its containing
  calendar day, TZ-safe via `localtime`/`mktime`) or a space-free `A..B`
  interval, and **ignores non-parseable values** (rc 0, store nothing) so a
  shared broadcast `--query` = plain text never aborts a mixed-axis run
  (a reversed `A..B` still errors).
- Same CLI↔axis-plugin convention as `rec_axis_open`/store: optional,
  dlsym'd, libcorm holds zero axis knowledge.

#### Worked example — the shape in one command

```sh
corm -p "<SOME-DATE>:<SOME STRING>" "demo.db@stoma,sepal,joint:a:s"
```

Step by step, what this does:

1. **Primary put.** `demo.db` is opened `:a:s` — key type `a` (auto-index),
   value type `s` (string). corm auto-assigns a fresh ref and stores the
   **whole** argument string `<SOME-DATE>:<SOME STRING>` as the value. Nothing
   is split, no key is extracted, nothing is validated against any axis.
2. **Fan-out.** The CLI passes `(ref, value)` to **each** axis on the roster
   (`stoma`, `sepal`, `joint`) — the same ref, the **same entire string**,
   verbatim. corm has zero per-axis knowledge: it never splits the string,
   never pulls out a date, never decides what a "key" is.
3. **Each axis parses the whole string itself, in its own grammar** (axis
   autonomy, principle 1):
   - **joint** reads a leading date out of the string → the ref exists at
     that time (an open interval starting there).
   - **sepal** takes the string's content and generates embeddings for it →
     a semantic vector store rowed by ref → similar to how later ANN queries
     find refs.
   - **stoma** FTS-indexes the string under the canonical `text` field →
     lexical/token queries find the ref.
4. **Query with AND/OR/NOT.** Each axis is queried through its own `--params`
   grammar; results are combined over ref sets, and refs resolve back to the
   primary record for rendering. The ref is the "value stored" by every axis;
   the string is the payload the axis uses to build something queryable
   (a semantic index, a timeline, a token index).

The division of labour is the whole point: **corm stores and fans out; the
axis parses** (string `rec_axis_store` or, for binary primaries, the additive
`rec_axis_store_typed(blob,len,qtype)` — D12; the CLI forwards the primary's
`qtype`+bytes and prefers the typed symbol when `vtype != CM_STR`). If a
payload isn't in an axis's grammar, that axis alone rejects it — the primary
and the other axes are unaffected.

**The inverse principle** (this is the whole delete design, not a pile of
per-axis fixes): `rec_axis_unstore` deletes exactly the entries the axis's
own store created for that ref, by walking backwards the same knowledge
the store used to put them there (and `rec_axis_readback` reads them back the
same way). Delete cost ∝ entries the ref owns, never O(store), never a
scan. Where an axis's store is already ref-indexed or keeps a ref-derived
inverse, the inverse *is* the mechanism — no new state. Where the store key
is not the ref and no inverse exists (today only libislet's cell grid),
that axis gets the one canonical addition: a `ref → entries` manifest, the
same *kind* of inverse the others already happen to keep.

| Axis | The existing inverse | New footprint for store/unstore/get |
|---|---|---|
| libsepal | store keyed **by ref** (`sepal_get`/`sepal_del`; same-ref put is replace-in-place) | adapters only (`unstore` normalizes absent → 0); string values embed-if-configured else `EINVAL` |
| libjoint | `id` secondary index (id → ti keys, kept by `corm_assoc`, backfilled from the file-backed `ti` map on open) | public `joint_erase` + `rec_axis_store`/`unstore`/`readback` (ordered-attempt whole-string grammar; id-index exact-match guard; read-back = NUL-joined store-grammar intervals); no stored state. **DONE 2A-3 (2026-09-14)** |
| stoma | doc side-table (`field\trow` → folded text) — re-tokenize → the exact posting keys (also serves phrase verification + rank lengths) | public `stoma_unindex`/`_ref` + `rec_axis_store`/`unstore`/`readback` on the canonical `text` field (read-back = folded doc, one entry); no new state; memory-only (rebuilt from primary strings per open). **DONE 2A-2 (2026-09-14)** |
| libislet | none (cell-keyed, value = ref) | **only structural addition**: `rev` manifest (own single-map `<fname>.ridx`: u32 ref → `;`-joined canonical point string, replace-in-place per ref) + `islet_del_value_N` family (`1..4` + `2_32`); `store` replace-in-place, shared cells legal, `unstore` O(cells-of-ref) |

**Persistence classes:** file-backed (`ti`, grid+`rev`, sepal blobs — reopened
across invocations) vs memory+rebuild (stoma postings+docs, re-derived from
primary strings at each open: zero disk, never stale).

Phase 2A slices, per-axis value grammars, and gates: site repo
`mm-plan/PHASE-2-CLI.md` / `mm-plan/README.md` §11.

### Running a query

```c
typedef enum { REC_JOIN_AND = 0, REC_JOIN_OR = 1, REC_JOIN_NOT = 2 } rec_join_t;

rec_query_t q = {0};
q.n_axes = 3;
q.axes[0] = (rec_query_axis_t){ it_slot,  &tp, REC_JOIN_AND };   /* seed  */
q.axes[1] = (rec_query_axis_t){ geo_slot, &gp, REC_JOIN_AND };   /* ∩ bbox */
q.axes[2] = (rec_query_axis_t){ st_slot,  &ap, REC_JOIN_AND };   /* ∩ FTS */
q.top_k = 10;
q.min_score = 0.4f;
q.consumer_score = my_score;            /* NULL → first rank-capable axis */

rec_query_run(&q, best_refs, best_scores);
```

The first axis seeds the running set; each subsequent axis's `join` selects
intersect (AND), union (OR), or subtract (NOT). A global `combine` field
applies one mode to every axis after the seed. Exactness propagates via the
kernel's joins (intersect → approx when either operand is, bound = min;
union → approx when either is; subtract keeps the left operand's flag).

The optional `consumer_score` callback receives a pre-computed
`rec_axis_score_t[]` (slot + score + valid per axis, regardless of join) for
each surviving ref, so the consumer weights axes however it likes. The
optional per-axis `decode` fn turns a CLI string into axis-owned params,
keeping the CLI generic.

### Plugin loading stays consumer-side

`dlopen` happens in the CLI or library user, never in libcorm's own link
table: `ldd bin/corm` shows only `libcorm` + `libqsys` + libc + libxxhash,
and a `grep -E 'stoma|libjoint|libislet|libsepal'` across libcorm source
and headers returns zero matches.

## Status

- Kernel (`rec.h`/`rec.c` + `rec_test`, `bench_rec`): landed in this repo,
  branch `kernel`.
- libislet adapter `rec_axis_fill_bbox`: implemented (libislet branch
  `kernel`).
- libjoint adapter `rec_axis_fill_interval`: implemented (libjoint branch
  `kernel`, via the libjoint `rec_axis_t` registration).
- libjoint Phase 2A store half (`joint_erase` + `rec_axis_store`/`unstore`/
  `readback` with the ordered-attempt value grammar): implemented (in-site
  submodule `external/libjoint`, **DONE 2A-3 2026-09-14** — `make clean &&
  make test` green: existing `./bin/test | diff expects.txt` intact,
  `test_extended` passes, new `joint_axis_store_test` 116/116; `nm -D`
  shows all four symbols; zero warnings; zero new stored state; read-back
  is the ref's intervals NUL-joined in the store grammar).
- stoma adapter `rec_axis_fill_tokens` + ranker `stoma_rank`: implemented
  (in-site submodule `external/libstoma`).
- stoma Phase 2A store half (`stoma_unindex`/`stoma_unindex_ref` +
  `rec_axis_store`/`unstore`/`readback` on the canonical `text` field):
  implemented (in-site submodule `external/libstoma`, **DONE 2A-2 2026-09-14** —
  `make test` green: `stoma_test` 227/227 incl. the index→unindex zero-
  residual differential, `stoma_prop_test` seeds pass, new
  `stoma_axis_store_test` 34/34 valgrind-clean; `nm -D` shows all five
  symbols; memory-only, zero new stored state; read-back returns the
  folded text).
- libislet Phase 2A store half (`rev` + `islet_del_value_1..4`/`_2_32` +
  `rec_axis_store`/`unstore`/`readback`, point-list value grammar):
  implemented (in-site submodule `external/libislet`, **DONE 2A-4 2026-09-14**
  — `store` replace-in-place parse-first (`x,y[,z];…`, int16 lanes, dim
  from first point 1..4, in-call dedup, over `ISLET_AXIS_MAX_POINTS`
  (1024) → `ERANGE` else `EINVAL`); `rev` in its own single-map
  `<fname>.ridx` (u32 ref → `;`-joined canonical points), file-backed via
  `rec_axis_open`, lazy in-memory for raw `islet_open` handles; shared
  cells legal; `unstore` idempotent O(cells-of-ref); read-back = NUL-
  joined canonical points, round-trippable. `make` zero warnings,
  `make test` exit 0 incl. new `test_axis_store` (15 tests / 193
  assertions, fresh local build via `LD_LIBRARY_PATH`), `nm -D` shows all
  nine symbols, new binary valgrind-clean; also fixed a latent exit-time
  UAF in `rec_axis_open`'s spec buffer).
- semantic (libsepal) adapter: implemented — `sepal_fill_approx` (streams
  top-m Hamming refs, seals, declares `REC_SET_APPROX` with owed bound
  `m/n`) + `sepal_rank` (exact cosine callback); in-site submodule
  `external/libsepal` v0.1.0. Search itself is network-free pure
  ANN; embedding stays consumer-side (no `rec_embed_t` in libsepal —
  the embedding ABI question in §4.2 stays open).
- `rec_query` engine (`rec_axis_register` / `rec_query_run` / `rec_join_t`):
  **done** — registry-driven query composition landed. Axis libs gain
  constructors that register; a mock two-axis plugin
  (`external/libcorm/src/librec_axis_mock.c`) and `test-cli.sh` (wired
  into `make test`) prove the plugin shape without any real axis's
  production data. `rec_axis_open` is implemented in all 4 real axis
  submodules (see the table below) and verified end-to-end against a real,
  file-backed libsepal store (put two vectors, reopen, `--params
  'file=... qdim=... m=... min_sim=...'` recovered both refs with exact
  expected cosine scores 1.0/0.0, and `--min` correctly filtered the
  low-score one out).
- **CLI composition is by name, never by file path or slot** (site
  `mm-plan`, phase 2B): the `@` roster in the filespec declares the load
  set once; axes dlopen by name (`lib<name>.so`) from `$CORM_AXIS_PATH`;
  stores bind via alongside-defaults (spec derived from the primary
  location); `--params` is each axis's own retrieval grammar. Refs
  crossing this surface are the `uint32_t` corm auto-index refs, never an
  axis's internal key. The earlier `-Q` recall-query mode (`--dl PATH
  --open SPEC --axis SLOT`, the `rq_*` helpers) is **retired and removed**
  with the `-g` fold (PHASE-2-CLI.md 2B-3). A `bin/qsearch` site wrapper
  script remains a deferred future workstream (no real site module writes
  through these axes yet).

| Axis | `rec_axis_open(spec)` convention | ctx type | Persisted? |
|---|---|---|---|
| libjoint | `spec` = `joint_init()` filename (empty/NULL → in-memory) | `jd` (unsigned handle, widened via `uintptr_t`) | either (file or derived rebuild; D13) |
| libislet | `spec` = `"filename:database:mask"` (`:`-separated, any field empty → `islet_open()`'s NULL/0 default) | `uint32_t` db handle (widened via `uintptr_t`) | either (file+`.ridx` or derived; D13) |
| libsepal | `spec` = `sepal_open()` fname (empty/NULL → memory-only store) | `sepal_vecstore_t *` (direct pointer) | file-backed |
| stoma | `spec` = decimal `stoma_open()` mask **or** `<dir>/<primary>-stoma` path (per-primary rebuild — `CORM_AXIS_PRIMARY` first, else the single-roster scan; 2B-2) | `stoma_db_t *` (direct pointer) | derived (memory+rebuild) |

## Verification

- `src/rec_test.c` — sets, sorts, dedup, joins (intersect/subtract/union),
  `rec_set_fill_corm_iter`, rank min-score/top_k/ties/streaming; 18 groups.
  Plus the 1C exactness group: fresh-set defaults (exact/bound 1.0),
  set/clear, bound validation (`(0,1]`, NULL → −1), join-propagation matrix
  (intersect/union → approx + bound=min; subtract keeps left's).
- `src/bench_rec.c` — kernel join vs hand-rolled join parity (identical
  match counts); equal/zero `corm_next` scans on the kernel path.
- libislet: `test_geo_fill.c`, `test_fill_parity.c` (fill == raw collect),
  `test_fill_props.c` (brute-force oracle), `bench_rec_fill.c`,
  `bench_sparse.c`.
- stoma: `stoma_test.c` groups 30-35 (fill==query equivalence both phrase
  modes, seal/additivity, arg validation + non-decimal strictness, rank
  numbers, kernel join/rank integration), `stoma_prop_test.c` (fixed-LCG
  differential: fill set == reference == stoma_query for every query, plus
  exact rank-score spot-checks).
- libsepal: 8 unit tiers (blob/cosine/sketch/matryoshka/vecstore/search/
  fill/rank — search/fill validated against an exact float-cosine
  brute-force reference), `test_persistence` (close→reopen byte-identical),
  `test_recall_prop` (seeds {1,42,1337} × dims {64,256,384,768} × N to
  3000: search==brute-force order with full pool, fill ⊇ top-k),
  `test_blob_props` (20k random buffers never misread), `test_scan_stress`
  (10k×768), standalone fuzzers; `bench_search` (two-stage vs brute force,
  see study §12.4).