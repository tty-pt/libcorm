## 1.5.0

Three new public primitives, plus a real fix to cursor teardown. All three copy-or-delete helpers exist to remove hand-rolled boilerplate at `corm_get()` / `corm_next()` call sites, where each call site had to re-derive the destination size — and where the size was routinely derived wrongly (see the measured-type trap below).

- **`corm_get_copy()` copies a value out by its real length** (`corm.h:364`): `void corm_get_copy(uint32_t hd, const void *const key, void *dst)` writes the stored value to `dst` and returns nothing. The destination must be at least as large as the value, and the size is *not* returned and no truncation is performed — a caller that wants truncation keeps using `corm_get()` with `strlcpy()`. Two failure modes are turned into hard errors rather than silent corruption: a miss `CBUG()`s, because a caller reaching for a copy of a value that is not there has a bug, not a runtime condition (callers that legitimately probe for presence use `corm_get()` and test for `NULL`, or `corm_count()`); and field access on a record-aware map — the `"id:field"` composite keys — is rejected with `CBUG()`, because `corm_get()` resolves those to a pointer *inside* the stored struct, whose length is the field's and not the map's value type's, so sizing a copy from the value type would overrun both the field and the caller's buffer.
- **`corm_next_copy()` advances a cursor and copies both sides** (`corm.h:651`): `int corm_next_copy(void *key, void *value, uint32_t cur_id)` writes the key and/or value to the supplied buffers and returns 1 while the iteration has entries, 0 when done. `NULL` skips that side. As with `corm_next()`, the cursor is released by corm at end-of-iteration and a `corm_fin()` after the loop that returned 0 is a no-op. The cursor's map handle is read from the cursor *before* `corm_next()` runs, because that call may release the cursor and recycle its id.
- **`corm_del_value()` deletes one specific value from a duplicate chain** (`corm.h:438`): `int corm_del_value(uint32_t hd, const void *const key, const void *const value)` removes the first chain entry equal to `value` and returns 1, or returns 0 if nothing matched. **Iteration stops at the first match**: the entry is unlinked and freed before returning, so the cursor is never advanced again. On a non-multivalue map the chain is a single slot which is not separately addressable, so `value` is ignored and this behaves exactly like `corm_del()`. Values are compared by content, so a measured or variable-width type matches on its bytes rather than on the identity of the caller's buffer. Note this is deliberately *not* a remove-all: `corm_del_all()` remains the remove-everything-under-this-key operation.
- **The measured-type trap these three share** (`corm_len`): `corm_type_len()` returns **0** for any type registered with `corm_mreg()`, which is how `CM_STR` is registered — so a copy sized with `corm_type_len()` copies **zero bytes** for a string, and does so silently. The new primitives size themselves with `corm_len(type, ptr)`, which measures the actual value. This is the class of bug the primitives exist to absorb: four independent hand-rolled copy helpers in the downstream `axil-nd` port had it, two of which shipped a silently truncated string.
- **Cursor release was not idempotent** (`corm_cur_release`, internal — `libcorm.c:2051`): `corm_fin()` called `idm_del()` on every live cursor, but `idm_del()` pushes the id onto its free list unconditionally, so calling it twice for the same cursor pushed a **live** id onto the free list a second time. A subsequent `corm_iter()` could then hand that same id to a second live cursor, and the two would share one cursor slot. Reachable today from any caller that does the idiomatic `while (corm_next(…)); corm_fin(cur);`, because corm already released the cursor when the loop returned 0 — a double-free of the cursor id. Release now clears the handle field before freeing, so the second call is a no-op and `corm_fin()` is safe to call after the loop.
- **Regression coverage**: `test_corm_get_copy` and `test_corm_next_copy` in `test_extended.c` cover a hit for a fixed-size type, `CM_U32`, `CM_PTR` and `CM_STR` (destination a `char[]`, asserting the full string is copied), plus a miss that must abort, run in a child process — the abort is observed as the child's signal/exit status, and since mingw has no `fork()`/`sys/wait.h` that one check is `#ifndef _WIN32`-skipped there (the Windows packaging job only compiles the suite); `test_del_value_u32`, `test_del_value_isolated`, `test_del_value_str`, `test_del_value_non_multivalue` and `test_get_copy_measured` in `test_multivalue.c` cover the delete and the measured-copy length. The `CM_STR` delete test asserts that two distinct buffers with identical bytes are *not* compared by pointer identity.
- On-disk format and all pre-existing public API signatures are unchanged; databases written by 1.4.x and earlier load correctly, and no behaviour visible through the old API changed.
- `corm.pc` still reads `Version: 0.6.0` and is deliberately left alone — it has tracked a stale version string for several releases, and correcting it belongs to a packaging change rather than a library release.

---

## 1.4.1

- **Multi-db file load dropped databases — and the exit-save then made it permanent** (`corm_load_file` / `_corm_load`): loading walked the mapping by *counting the file's registered handles*, one `_corm_load` per id, each using the **iterated** handle as the load target. That only pairs correctly when the id list's order matches the on-disk block order, which it does not (blocks are emitted in `ids_iter` order, handles are registered as they are opened). Any database whose block the walk failed to reach came back as an **empty map** — and since `corm_save()` runs at process exit, a reader re-wrote the file from the emptiness, permanently truncating the block it had missed. Repro on the pre-1.4.1 library: a 3-db file opened 2 of 3 dbs empty; a 2-db file opened one empty and shrank 52 → 42 bytes on read. This is the "silently dropped" class that 1.4.0's F4 entry hit on the libstoma sidecar. `corm_load_file` now takes the requesting `hd` from `corm_open` and walks the mapping by each block's on-disk `size` prefix, so any block whose `lid` matches is loaded into the handle that asked for it, whatever order the file is in.
- **Off-by-8 block skip** (`_corm_load`): the dbid-mismatch branch returned `mm + size - mm_start` *after* `mm` had already advanced past the 8-byte `[dbid][size]` block header, so the walk drifted 8 bytes too far per skipped block and read the next block's `lid`/`size` from mid-payload bytes. The resulting garbage length can walk past the end of the mapping (one 3-db repro segfaulted here). The skip now honors the on-disk `size` exactly.
- **Bounds-checked block walk** (`corm_load_file`): every block's `size` prefix is validated (`>= 8` and `mm + block <= end`) and a non-matching `lid` is skipped without entering `_corm_load`, so a truncated, corrupt, or foreign file stops at the mapping end instead of running off it.
- **`corm_close` + save no longer zeroes the file** (`corm_save_file`): the `file->size == 0` early return ran *after* `open()` + `ftruncate()`, so once every handle for a file was closed, the next `corm_save()` truncated the file to 0 bytes and threw away its contents. The early return now precedes both, keeping the data on disk (verified 0 → 20 bytes for a closed one-key map). Side effect: a path that never held data no longer gets a 0-byte file created for it.
- **Regression coverage**: `test_persist` (wired into `make test` via `test.sh`) drives a three-phase cross-process round trip — write, read back in a fresh process *in a different order than the writer used*, then save-after-close. The read phase is what the pre-1.4.1 loader fails: it drops a database and segfaults; the close phase catches the 0-byte truncation.
- On-disk format and all public API signatures are unchanged; files written by 1.4.0 and earlier load correctly.

---

## 1.4.0

- **Renamed `libqmap` → `libcorm`**: the `qmap_*` API and headers are now `corm_*` (`include/ttypt/corm.h`, `qmap.pc` → `corm.pc`); the full test/example suite was migrated to the new name.
- **The Recall Kernel** (`include/ttypt/rec.h`, `src/rec.c`) — a domain-free "filter by axis → join → rank" loop over uniform 32-bit refs:
  - `rec_set_t`: arena-backed candidate sets (`rec_set_push`, then `rec_set_seal` = sort + dedup; `rec_set_intersect`/`_union`/`_subtract` are sorted **merge-joins**; `rec_set_count`/`rec_set_at`; drain any corm handle with `rec_set_fill_corm_iter`).
  - `rec_rank_t`: bounded **top-k min-heap** with a score threshold — `rec_rank_new(top_k, min_score)`/`rec_rank_push` (O(log k))/`rec_rank_sorted` (best-first, ties by ascending ref); scoring via `rec_score_fn`.
  - Refs are `rec_ref_t` (`uint32_t`), are never interpreted by the kernel, and never an axis's internal key.
- **Exact vs approximate fills**: an approximate axis (ANN/semantic top-m) must declare itself — `rec_set_set_approx`/`rec_set_approx`/`rec_set_recall_bound` — and joins propagate it (intersect/union → approximate when either operand is, bound = min; subtract keeps the left operand's flag), so intersecting with an approximate set bounds final recall.
- **`rec_query` engine + axis plugin registry** — registry-driven query composition running wholly inside libcorm with **zero axis dependencies**: `rec_axis_register`/`rec_axis_set_ctx`, `rec_axis_t` (`name`/`fill`/`rank`/`ctx`/`decode`), and `rec_query_run` with `rec_join_t` (AND/OR/NOT), `top_k`/`min_score`, and an optional `consumer_score` callback fed `rec_axis_score_t[]` per surviving ref. `dlopen`-loading of axis plugins happens on the consumer/CLI side only.
- **Rank composition**: the **first rank-capable axis in query order wins** (preorder in `corm_expr_eval`, kernel fallback in `rec_query_run`); labeled instances `label:axis` rank by the per-ref MAX; `--rank=A` (or scoped `--rank@A`) pins the ranker to one labeled instance; `--score` multi-ranker combining deferred.
- **New kernel features**: proximity / approximate ranking, the `QM_RANGE_GE` range-eval flag, and `corm_get_ktype` to ask a handle's key type.
- **corm CLI composition** (`src/corm.c`): `-X` expression trees over a `@`-roster of axes (dlopen by name from `$CORM_AXIS_PATH`), `-p "<whole record string>"` fan-out to every bound axis, `--params` per-axis retrieval grammar, `-g`/`-m`/`-c` read-back rendering, and per-axis CLI long-options via `rec_axis_cli_options`/`rec_axis_config_arg` (scoped `--name@label`, precedence leaf spec > `@label` > `@axis` > unscoped > env). Typed param parsing rides the `rec_cli_*` helpers (`rec_cli_int/_uint/_float/_size/_str_dup/_str_set` + `_b` variants) backed by `rec_spec_scan`'s `key=value` spec grammar (quoted strings included).
- **Axis plugin conventions (optional, CLI-specific, never declared/exported by libcorm)**: `rec_axis_open(spec)` (opaque spec → store ctx), and the Phase 2A store half `rec_axis_store`/`rec_axis_store_typed`/`rec_axis_unstore`/`rec_axis_readback` — the consumer passes `(ref, whole-string value)` blindly; each axis parses in its own grammar; `unstore` is idempotent (delete cost ∝ entries the ref owns, never O(store)).
- **Test & bench suite**: `rec_test` (sets/joins/ranks + 1C exactness matrix), `bench_rec` (kernel merge-joins vs hand-rolled parity), `rec_axis_test`/`rec_axis_store_test`/`rec_cli_test`/`rec_axis_bench`, a mock two-axis plugin (`librec_axis_mock.c`) proving the dlopen shape end-to-end, plus the new `test-cli.sh`, `test-fanout.sh`, `test-mm.sh`, `test-real.sh`, `test-roster.sh`, and `test-shortcircuit.sh` shells (with in-repo plugin-stub starters `librec_axis_{fold,plain,probe,stub,zed}.c`).
- Rename + axis-boilerplate passes above all ride on `rec_ref_t = uint32_t`; refs and axis params are CLI parameters everywhere.
- **`-X` grammar pinned**: verified against the built 2B-3 parser (mm-plan `PHASE-2-CLI.md` D10) — quote + whole-string `VALUE` semantics, unary `NOT` with the `use EXCEPT` hint, uppercase reserved keywords. Two behaviors are now pinned by gate rows: `EXCEPT` chains left-to-right (`except-chain`, `test-cli.sh`), and the first rank-capable leaf in preorder wins even with two rankers in one expression (`stoma-over-sepal`, `test-real.sh`).
- **F4 — duplicate-open fix**: `corm_open` now aliases the live handle when the same (file, map) is opened twice with the same key/value shape (record type, key/value types, table mask), while still registered in the file's ids. Previously the second handle orphaned the first and its as-of-open copy won the exit-save — libstoma's sidecar-scan mirror-open of an already-held primary silently dropped `-p` seeds (pre-existing roster) and `-d` forgets. Real-file TDD via `test-mm.sh` (RED-A/RED-B) → green.
- **2B-5 mm dialect** (mm-plan `2B-5-IMPLEMENTATION.md`): pi-mm recipes as documented CLI invocations — explicit-ref store (`-p REF:"<DATE>:<TEXT>"`), bounded-window joint ∧ stoma composed search, idempotent roster-backed forget, and the enumerate+forget reset loop (no single-flag reset exists; the `-1` empty sentinel is skipped). `test-mm.sh` gate wired into `make test`.
- **CLI tuning**: `QDBE_MASK` shrinks `32767 → 4095` (D11 — an initial hint, auto-grow); new `CORM_MASK` env override (validated `2^n-1`) shared by the CLI opens, the test plugins, and libstoma's sidecar rebuild so co-opened files match. `gen_put`/`gen_del`/`gen_del_all` now return the op exit status.
- **2B-4 write fan-out** (mm-plan `2B-4-IMPLEMENTATION.md`): `-p`/`-d`/`-D` write to the union of `{primary} ∪ {@} ∪ {target}` — composed mode only, classic path byte-identical. Whole `-p` payloads fan out as `(ref, blob, len, qtype)` via the additive `rec_axis_store_typed` when `vtype != CM_STR` (D12), else string `rec_axis_store`; binary-payload-on-text-only axes loud-skip; `-d`/`-D` collapse to idempotent `rec_axis_unstore`; ref operands are literal u32 or primary reverse-view names; loud partials (attempt-all/report-all/nonzero). A per-slot store-capability table is built at bind time (read-only detection); `rec_axis_readback` stays dlsym'd with no CLI surface.
- **Test plugins**: write-capable `librec_axis_fold` (`.wr` stash feeds fill) + string-only `librec_axis_plain`; `test-fanout.sh` gate + dlopen-only `tests/fanout_verify.c` probe.

---

## [0.8.0] - 2026-09-10

### Performance — W3 index-read regression (MV duplicate chains)
- **Per-key MV duplicate chain** (`corm_t.mv_next`, re-linked by
  `corm_rebuild_map` in position order): `corm_get_multi` walks the duplicate
  chain in insertion order — **O(k)** instead of a full sorted-index rebuild
  (qsort) per call; MV delete is an O(k) link-unlink / head-promote instead of
  a `corm_bsearch_ex(FIRST)` rebuild. Load rebuilds chains automatically
  (`_corm_load` → `corm_put`).
- **Hole-eliminating backshift delete** (`corm_backshift`): slot clears now
  cascade-shift the following cluster left (cyclic interval test, continue past
  unmovable elements, stop at empty), restoring the no-holes invariant so every
  hash probe early-exits. Removes `corm_mv_slot` — fresh-key puts are O(cluster)
  instead of O(m) (up to 65 536 slots scanned on key-absent puts). Close path
  is O(N), no quadratic hang.
- Net: 10k libjoint start+stop pairs 21.4 s → 14.3 ms vs a 2.5 s baseline;
  MV-secondary puts ~2 400× faster (7.6 s → 3.1 ms per 10k). Zero full-table
  scans in any hot path.

### Fixed
- **Non-MV split-home hazard**: a deleted slot (hole) before an existing key
  made the early-exit probe return the hole → `corm_get` NULL / re-`put`
  inserted a duplicate into a non-multivalue map. Backshift eliminates the
  hole class entirely (covered by `test_backshift_cluster`, `test_backshift_wrap`).

### Changed
- `corm_get_multi` contract unchanged, but implementation is now a chain cursor
  (`CM_MVCHAIN`) — see corm.h. `corm_count` on a dirty MV map still triggers the
  sorted-index rebuild (out of scope; equality reads via `corm_get_multi`).

### Added
- **rec kernel** (`ttypt/rec.h`, `src/rec.c`): recall candidate sets
  (`rec_set_t`: arena-backed, push/seal + sorted merge-join intersect/
  subtract/union, drain any corm handle via `rec_set_fill_corm_iter`) and a
  generic streaming ranking loop (`rec_rank_t`: bounded top-k min-heap,
  min-score filter, stable best-first sort). Pure C, no domain math; optional
  and additive — raw corm entry points untouched.
- `corm_get_ktype()`: returns a map's key type, mirroring `corm_get_vtype`.
- `rec_test` + `bench_rec` build targets (per-area-binary convention),
  registered in `test.sh`; `bench_rec` asserts kernel-join vs hand-rolled
  join parity (identical match counts).
- Backshift regression tests: `test_backshift_cluster`, `test_backshift_wrap`
  (wrapped cluster with skip-unmovable cascade), plus 4 chain tests
  (`test_chain_basic_ops`, `test_chain_grow_relink`,
  `test_chain_assoc_close`, `test_chain_persist`).

---

## [0.7.0] - 2026-02-23

### Fixed
- **Bug #1**: CM_MIRROR + CM_MULTIVALUE persistence now works correctly
- **Bug #2**: corm_assoc + CM_MULTIVALUE no longer segfaults with multiple keys
- **Bug #3**: CM_RANGE iteration now returns all duplicates

### Improved
- corm_iter: 3x faster (eliminated double binary search)
- corm_count: 115x faster for 1000 duplicates (O(n) → O(log n))
- Code clarity with helper functions

### Added
- Comprehensive test suite for CM_MULTIVALUE (18 tests)
- CM_MULTIVALUE flag for duplicate keys in sorted maps

---

## [0.6.0] - 2026-02-23
- Remove CM_MIRROR requirement for file loading (files now load automatically regardless of mirroring)
- Add allocation reuse optimization to reduce unnecessary malloc/free and improve pointer stability
- Consolidate documentation (removed redundant IMPROVEMENTS.md, enhanced corm.h API docs)
- Fix documentation: clarify that mirror maps are automatically closed with primary map (no manual close needed)
- Fix use-after-free bug in corm_open() when reopening file-backed maps with database names

## [0.5.0] - 2025-10-24
- Add test workflow and man page generation using Doxygen
- Implement persistent storage (`corm_save()` for tracked databases, multi-database file support)
- Add CLI tool (migrated from `libqdb`, now deprecated)
- Introduce B-tree–like sorted index (`CM_SORTED`) with automatic rebuild on dirty state
- Refactor API types from `unsigned` to `uint32_t`
- Complete Doxygen annotations for automatic man page generation
- Expand test coverage for file persistence and sorted iteration
- Minor fixes and documentation improvements

## [0.4.0] - 2025-10-19
- Change release strategy
- Headers in ttypt folder
