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
