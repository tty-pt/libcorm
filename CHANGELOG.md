## [Unreleased]

- **HTOK Token Decoder Hook & Unpack Bounds Hardening (`include/ttypt/corm.h`, `npm/include/ttypt/corm.h`, `src/htok.c`)**:
  - Added `corm_htok_tok_decoder_fn` and `corm_htok_set_tok_decoder()` so `corm_htok_unpack()` can decode `CORM_HTOK_TYPE_TOKENS` fields back into UTF-8 text (`f601900`).
  - Hardened LEB128 length bounds checking against integer overflow in `corm_htok_unpack()` (`f601900`, `5d5be7a`).
- **Dirty-File Tracking, Atomic Persistence & Close Safety (`src/libcorm.c`, `npm/dist/corm.wasm`)**:
  - Added `corm_file_t.dirty` and `corm_mark_dirty()` across `_corm_put()`, `corm_ndel_topdown()`, `corm_clear_fast()`, and `corm_del_all()`, skipping no-op disk rewrites in `corm_save_file()` when `!file->dirty` (`5d5be7a`).
  - Switched `corm_save_file()` to write to a per-PID temporary file (`%s.tmp.%d.%u`) and atomically `rename()` into place (`5d5be7a`).
  - Closed the backing file descriptor at the end of `corm_load_file()` (marking new `ENOENT` files dirty), detached `head->file` before `corm_drop()` in `corm_close()` so closing a handle does not mark its file dirty, and rebuilt `npm/dist/corm.wasm` (`5d5be7a`).
- **Standardized Package Metadata (`npm/package.json`, `npm/README.md`)**:
  - Standardized author (`Paulo Quirino <quirinpa@gmail.com>`), `BSD-2-Clause` license, and npm `README.md` (`5d5be7a`).

## [2.4.2] - 2026-11-15

- **64-Slot LRU Engine Cache, Late Axis Binding & Quiet Axis Probing (`src/libcorm.c`, `npm/native/corm_napi.c`, `npm/package.json`)**:
  - Expanded `MAX_CACHED_ENGINES` from 16 to 64 slots with tick-based LRU eviction (`last_used_tick`) instead of round-robin eviction.
  - Added late axis binding on cached `corm_engine` handles (dynamically opening newly requested axes and persisting the updated `@roster` sidecar) and per-activation `cormr_axis_env_config` / `rec_axis_env_config` invocation.
  - Honored `CORM_QUIET` in `corm_axes_dlopen_record` to suppress stderr diagnostics when probing optional axis libraries.
  - Updated `npm/package.json` `build:native` RPATH to include standalone `$ORIGIN` and sibling `stele/prebuilds/linux-x64` paths and sync `prebuilds/linux-x64/corm.node`.

## [2.4.1] - 2026-11-14

- **N-API Engine Cache Safety, Per-Call Environment Restoration & Axis Merging (`src/libcorm.c`, `npm/native/corm_napi.c`, `npm/dist/index.js`, `npm/package.json`)**:
  - Cached `corm_engine` handles across `CormExecCommand` calls with disk `mtime`/`size` verification (`engine_disk_matches`) and round-robin cache eviction.
  - Saved and restored caller `env` overrides around `CormExecCommandInner` so per-call environment variables (`CORM_AXIS_PATH`, `CORM_SEPAL_EMBED_URL`, etc.) do not mutate the host process environment.
  - Merged `CORM_DEFAULT_AXIS_PATH` with caller-supplied `CORM_AXIS_PATH` in `execCormCommand` and included `"prebuilds"` in `npm/package.json` `"files"`.

## [2.4.0] - 2026-11-14

- **In-Process Multi-Axis CormEngine & HTOK v4 Node-API Bindings (`npm/native/corm_napi.c`, `npm/dist/index.js`)**:
  - Exposed `htokPack` (`corm_htok_pack`), `htokUnpack` (`corm_htok_unpack`), `execCormCommand`, `closeCormEngine`, and `CormEngine` (`PING`, `LIST`, `GET`, `GET_JSON`, `PUT`, `PUT_JSON`, `DEL`, `DEL_ALL`, `QUERY`) in the native Node-API addon with full `-X` expression evaluation and dynamic axis loading.
  - Upgraded `CormDb` to support typed keys/values (`u`, `a`, `2u`, `2s`, `b`, `CM_MIRROR`), `delAll`, `listKeys`, and `count`, and removed all JS `Map` fallbacks.
  - Requires `@tty-pt/stele` >=1.7.0 (`^1.7.0`).

## [2.3.3] - 2026-11-13

- **Packaging**:
  - Requires `@tty-pt/stele` >=1.6.2 (`^1.6.2`).
  - The npm tarball is now built once by CI, with a self-contained native addon per platform (`prebuilds/<os>-<cpu>/`) next to `corm.wasm`, and published as verified.

## [2.3.2] - 2026-11-13

- **Shared Allocator & Disjoint Global-Base (`corm.wasm`)**:
  - Linked `corm.wasm` against `@tty-pt/stele`'s shared allocator (`malloc`/`calloc`/`realloc`/`free`) at a disjoint `--global-base=524288` to prevent static data segment collisions across co-loaded WASM modules.
  - Upgraded `@tty-pt/stele` dependency to `>=1.6.1` (`^1.6.1`).

## [2.3.1] - 2026-11-13

- **macOS & CI Build System Compatibility**:
  - Restored dual GNU Make / BSD Make assignment in mk build toolchain for compatibility with macOS GNU Make 3.81.
  - Rebuilt and optimized `corm.wasm` artifact.

## [2.3.0] - 2026-11-13

- **Libstele v1.6.0 Substrate Alignment**:
  - Upgraded substrate baseline to @tty-pt/stele@^1.6.0 (>=1.6.0).
  - Aligned WebAssembly linear memory layout with generic canvas and bridge decoupled runtime.

## [2.2.4] - 2026-11-03

- **Zero-Copy Shared Memory Chaining (`CandidateSet`) & Substrate Integration**:
  - Exposed `.count()`, `.rawPointer()`, `.toUint32Array()`, and `.addFromRaw(ptr, count)` on `CandidateSet`.
  - Enables zero-copy passing of contiguous `uint32_t` reference arrays across WebAssembly modules sharing `@tty-pt/stele`'s linear memory without intermediate JavaScript array allocations.
  - Adopted strict `peerDependencies` (`@tty-pt/stele: ">=1.5.0"`) and `devDependencies` (`@tty-pt/stele: "^1.5.2"`).
- **Standalone `@tty-pt/corm` Package & Centralized Build**:
  - Published `@tty-pt/corm` providing standalone Node-API native bindings and synchronous WebAssembly fallback (`corm.wasm`).
  - Integrated with `external/mk/include.mk` (`make wasm`) using `-ffreestanding -flto -O3` and post-link `wasm-opt -O3` optimization (dropping binary size from 66 KB to 53 KB).
- **Native C HTOK v4 Binary Envelope API (`corm_htok_pack`, `corm_htok_unpack`) & `cormd` Daemon JSON Protocol**:
  - Implemented high-performance binary packing and unpacking for HTOK v4 containers directly in pure C (`src/htok.c`, `include/ttypt/corm.h`).
  - Added native `PUT_JSON` and `GET_JSON` socket protocol commands to `cormd` (`src/cormd.c`).

## [2.1.0]

- **Safe Database Retrieval Semantics (`corm_read`)**:
  - Introduced `int corm_read(uint32_t hd, const void *const key, void *dst, size_t max_sz)` with guaranteed zero-initialization on key misses, negative returns, and invalid handles, plus trailing zero-padding on hits.
  - Non-aborting, safe record retrieval eliminating crash hazards (`SIGABRT` / `CBUG`) across all downstream tenant modules and simulation engines.
- **WASM Linear Memory Arena Shims & Host Persistence (`src/corm.c`, `src/corm_compat.h`)**:
  - Added linear memory arena allocator and host persistence imports for executing Corm databases inside WebAssembly browser runtimes.
  - Standardized memory alignment and fallback hooks for headless/WASM targets.
- **`cormd` Resident Daemon Multi-Instance Resiliency**:
  - Enhanced socket lifecycle and signal handling to ensure uninterrupted resident database performance across repeated test cycles and multi-tenant instances.
- **Linker Purity & DT_RUNPATH Elimination**:
  - Stripped hardcoded `/home/` and `-Wl,-rpath` flags from Makefiles, linking cleanly against system authority (`/usr/lib`).

## [2.0.0]

- **Recall Kernel Overhaul & `rec.h` Elimination**:
  - Replaced legacy `rec.h` with `<ttypt/cormr.h>` and unified umbrella inclusion in `<ttypt/corm.h>`.
  - Canonical `cormr_*` symbol and type namespace (`cormr_set_t`, `cormr_rank_t`, `cormr_axis_t`, `cormr_query_t`, `cormr_join_t`).
  - Streamlined and shortened record schema APIs:
    * `corm_record_register` → `corm_rec_reg`
    * `corm_record_field_set_target_hd` → `corm_field_target`
    * `corm_record_type_id` → `corm_rec_type`
    * `corm_record_field_t` → `corm_field_t`
    * `rec_set_fill_corm_iter` → `cormr_set_drain`
    * `rec_set_recall_bound` → `cormr_set_bound`
    * `rec_axis_cli_option_t` → `cormr_axis_opts`
  - Multi-axis query composition engine: `cormr_query_run` with boolean expression parser (`cormr_spec_scan`) and bounded CLI flag parsers (`cormr_cli_*`).
- **Resident Memory Daemon (`cormd`)**:
  - Standalone daemon keeping primary database files and search indexes (lemma, joint) resident in memory across requests.
  - Eliminates repetitive process startup and lexical index reconstruction overhead.
- **Value Copy & Chain Primitives**:
  - `corm_get_copy()`: copies value out by measured length without caller buffer truncation hazards.
  - `corm_next_copy()`: copies key and value during iteration.
  - `corm_del_value()`: deletes specific matching value from a duplicate chain.
  - Cursor release idempotency fix in `corm_fin()`.
