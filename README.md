# libcorm

[![C99](https://img.shields.io/badge/C-C99-555?logo=c)](#)
[![BSD-2-Clause](https://img.shields.io/badge/License-BSD--2--Clause-blue)](#)
[![In-memory maps](https://img.shields.io/badge/in-memory-maps-16A34A)](#)

> A small library for in-memory maps with optional persistence and a CLI tool.

Hash maps with typed keys/values, registration of custom types, secondary
indexes (associations), record-aware field access, free-list ID management,
and the recall kernel (`rec.h`) — the "filter by axis → join → rank" engine
the axis libraries (libjoint, libislet, libstoma, libsepal) plug into.

**⚠️ Important:** libcorm uses global state and is **not thread-safe**. Use
appropriate synchronization if accessing from multiple threads.

## Contents

- [Features](#features)
- [Install](#install)
- [Build from source](#build-from-source)
- [Quickstart](#quickstart)
- [API overview](#api-overview)
- [Recall kernel (`rec.h`)](#recall-kernel-rech)
- [Full API reference](#full-api-reference)
- [Documentation](#documentation)
- [Testing](#testing)
- [License](#license)

## Features

- **In-memory maps** — put/get/del by key with built-in and custom types
- **Optional persistence** — file-backed maps, auto-saved at process exit
- **Multi-value maps** (`CM_MULTIVALUE`) — several values per key for
  secondary indexes
- **Bi-directional mirroring** (`CM_MIRROR`) — automatic reverse lookup
- **Associations** — link a secondary index map to a primary map
- **Record-aware maps** (`CM_RECORD()`) — field-level get/put and automatic
  reference resolution
- **ID management** — free-list `idm_t` / `ids_t` allocators
- **CLI tool** — the `corm` binary for scripting maps on disk
- **Recall kernel** — `rec.h` set/rank engine for axis composition

## Install

Prebuilt packages are distributed from tty.pt for Linux (APT / Alpine / Arch /
Fedora-RHEL), macOS (Homebrew), Windows (winget / MSYS2), and OpenBSD. Follow
the [installation instructions](https://github.com/tty-pt/ci/blob/main/docs/install.md)
and use **libcorm** as the package name.

## Build from source

The library builds with a plain `make` (the shared [`mk` include.mk](https://github.com/tty-pt/mk)):

```sh
make                  # builds lib/libcorm.so, bin/corm, and the test suite
make test             # run the in-tree test suite
sudo make install     # lib + headers + corm.pc → $(PREFIX), default /usr/local
```

Link it from your own C code:

```sh
cc my_app.c $(pkg-config --cflags --libs corm)
```

**Dependencies:** `libqsys`, `libxxhash`.

## Quickstart

Library gist:
```c
#include <ttypt/corm.h>

uint32_t hd = corm_open(NULL, NULL, CM_U32, CM_STR, 0xFF, 0);
uint32_t key = 1;
const char *value = "hello";
corm_put(hd, &key, value);
const char *out = corm_get(hd, &key);
```

CLI gist:
```sh
corm -p 1:hello data.db:u:s
corm -g 1 data.db:u:s
```

Persistence uses a filename and optional database name; NULL means in-memory
only. File-backed maps are **automatically saved at process exit**.

**Note on File Persistence:**
- File loading happens automatically when opening a file-backed map (no flags
  required)
- The `CM_MIRROR` flag enables automatic reverse-lookup (bidirectional maps)
- When using `CM_MIRROR`, closing the primary map automatically closes the
  mirror (handle + 1)

Persistent example:
```c
// Simple file persistence (read/write without mirroring)
uint32_t hd = corm_open("data.corm", "main", CM_U32, CM_STR, 0xFF, 0);
corm_put(hd, &key, value);
corm_save();  // Optional explicit save
corm_close(hd);

// File persistence with bidirectional mirroring
uint32_t hd = corm_open("data.corm", "main", CM_U32, CM_STR, 0xFF, CM_MIRROR);
corm_put(hd, &key, value);
corm_save();  // Optional explicit save
corm_close(hd);  // Mirror map (hd + 1) is automatically closed
```

## API overview

### Type System

libcorm supports both built-in and custom types for keys and values.

#### Built-in Type Constants

| Constant | Description |
|----------|-------------|
| `CM_U32` | 32-bit unsigned integer (4 bytes, ordered) |
| `CM_STR` | Null-terminated string (variable length, ordered lexicographically) |
| `CM_HNDL` | Map handle (uint32_t, used for maps-of-maps) |
| `CM_PTR` | Raw pointer (fixed-size, ordered by address) |

#### Custom Type Registration

Register fixed-length or variable-length types for use as keys or values:

```c
uint32_t qm_point = corm_reg(sizeof(struct point));      // fixed-length
uint32_t qm_blob = corm_mreg(measure_blob);              // variable-length
```

| Function | Signature | Description |
|----------|-----------|-------------|
| `corm_reg` | `uint32_t corm_reg(size_t len)` | Register a fixed-length type. Returns type ID or `CM_MISS`. |
| `corm_mreg` | `uint32_t corm_mreg(corm_measure_t *measure)` | Register a variable-length type. `measure` callback returns byte size. |
| `corm_type_len` | `size_t corm_type_len(uint32_t type_id)` | Get fixed byte-length of a type, or 0 if variable-length. |
| `corm_len` | `size_t corm_len(uint32_t type_id, const void *data)` | Get byte length of a specific element. |
| `corm_cmp_set` | `void corm_cmp_set(uint32_t ref, corm_cmp_t *cmp)` | Override comparison function for a type. |

**Callback typedefs:**

| Type | Signature | Description |
|------|-----------|-------------|
| `corm_measure_t` | `size_t (*)(const void *data)` | Returns byte size of a variable-length element. |
| `corm_cmp_t` | `int (*)(const void *a, const void *b, size_t len)` | Comparison: returns <0, 0, >0. |

### Flags

| Flag | Value | Applies to | Description |
|------|-------|------------|-------------|
| `CM_SORTED` | — | `corm_open` | Keep entries sorted by key (required for `CM_MULTIVALUE`). |
| `CM_MULTIVALUE` | — | `corm_open` | Allow multiple values per key (requires `CM_SORTED`). |
| `CM_MIRROR` | — | `corm_open` | Create bidirectional reverse-lookup mirror (handle + 1). |
| `CM_AINDEX` | — | `corm_open` | Auto-index: assign sequential integer IDs for each unique key. |
| `CM_NOGROW` | — | `corm_open` | Disallow auto-growth beyond initial `mask` capacity. |
| `CM_RANGE` | — | `corm_iter` | Enable ordered range scan over sorted keys. |
| `CM_RECORD()` | — | `corm_open` | Declare vtype as a record type for field-level access. |

**Sentinel:** `CM_MISS` (`UINT32_MAX`) is returned by `corm_open`, `corm_reg`,
and `corm_iter` on failure.

### Iteration

Iterate over all entries in a map, or over entries matching a key:

```c
// Iterate all entries
uint32_t cur = corm_iter(hd, NULL, 0);
const void *k, *v;
while (corm_next(&k, &v, cur)) {
    printf("key=%u val=%s\n", *(uint32_t *)k, (const char *)v);
}
corm_fin(cur);

// Range scan: iterate entries with keys in [2, 5)
uint32_t key_start = 2;
uint32_t cur2 = corm_iter(hd, &key_start, CM_RANGE);
while (corm_next(&k, &v, cur2)) {
    uint32_t k32 = *(uint32_t *)k;
    if (k32 >= 5) break;  // past range end
    printf("key=%u val=%s\n", k32, (const char *)v);
}
corm_fin(cur2);
```

| Function | Signature | Description |
|----------|-----------|-------------|
| `corm_iter` | `uint32_t corm_iter(uint32_t hd, const void *key, uint32_t flags)` | Start iteration. `key=NULL` iterates all entries; `CM_RANGE` enables range scan. Returns cursor handle or `CM_MISS`. |
| `corm_next` | `int corm_next(const void **key, const void **value, uint32_t cur_id)` | Fetch next key/value from cursor. Returns 1 if valid, 0 if done. |
| `corm_fin` | `void corm_fin(uint32_t cur_id)` | End iteration early and free cursor. |

### Multi-Value Support (CM_MULTIVALUE)

For scenarios where multiple values need to share the same key (such as
secondary indexes), use the `CM_MULTIVALUE` flag.

**Requirements**:
- Must be combined with `CM_SORTED` flag
- Opening a map with `CM_MULTIVALUE` without `CM_SORTED` will fail (returns
  `CM_MISS`)

**Behavior**:
- Multiple entries with the same key can coexist in the map
- `corm_get(hd, key)` returns the **first** value for the key
- `corm_del(hd, key)` deletes only the **first** occurrence
- Use `corm_get_multi()` to iterate over all values for a key
- Use `corm_count(hd, key)` to count entries for a specific key

**Example - Secondary Index with Duplicates**:
```c
// Primary database: intervals with {min, max, who}
uint32_t qm_interval = corm_reg(sizeof(struct interval));
uint32_t qm_time = corm_reg(sizeof(time_t));

uint32_t primary = corm_open(NULL, "primary", qm_interval, qm_interval, 0xFF, 0);
uint32_t by_time = corm_open(NULL, "by_time", qm_time, qm_interval, 0xFF,
                              CM_SORTED | CM_MULTIVALUE);

// Associate secondary index to extract 'max' time
corm_assoc(by_time, primary, extract_max_callback);

// Multiple intervals can have the same 'max' time
struct interval i1 = {.min=100, .max=9999, .who=1};
struct interval i2 = {.min=200, .max=9999, .who=2};  // Same max!

corm_put(primary, &i1, &i1);
corm_put(primary, &i2, &i2);  // Both are kept in by_time index

// Query how many intervals end at time 9999
time_t max_time = 9999;
uint32_t count = corm_count(by_time, &max_time);  // Returns 2

// Iterate over all intervals ending at time 9999
uint32_t cur = corm_get_multi(by_time, &max_time);
const void *k, *v;
while (corm_next(&k, &v, cur)) {
    const struct interval *iv = v;
    printf("Interval: min=%ld max=%ld who=%u\n", iv->min, iv->max, iv->who);
}
corm_fin(cur);
```

**Deletion Patterns**:

For `CM_MULTIVALUE` maps, you have multiple options for deleting entries:

```c
time_t max_time = 9999;

// Option 1: Delete only the first occurrence
corm_del(by_time, &max_time);

// Option 2: Delete all occurrences at once (convenience function)
corm_del_all(by_time, &max_time);

// Option 3: Drop all entries from map (clear entire map)
corm_drop(by_time);
```

**API Functions**:
- `corm_get_multi(hd, key)` - Returns a cursor to iterate over all values for
  a key, or `CM_MISS` if key doesn't exist
- `corm_count(hd, key)` - Returns the number of entries for a specific key (or
  total entries if key is NULL)
- `corm_del(hd, key)` - Deletes only the first occurrence for CM_MULTIVALUE maps
- `corm_del_all(hd, key)` - Deletes all occurrences for a given key

### Associations (Secondary Indexes)

Link a secondary (index) map to a primary map. Puts and deletes on the primary
auto-update the secondary via a callback that extracts the secondary key:

```c
static void index_by_time(const void **skey, const void *pkey,
                           const void *value, void *ud)
{
    (void)pkey; (void)ud;
    const struct interval *iv = value;
    *(const time_t **)skey = &iv->max;  // secondary key = interval.max
}

corm_assoc(by_time, primary, index_by_time, NULL);
```

| Function | Signature | Description |
|----------|-----------|-------------|
| `corm_assoc` | `void corm_assoc(uint32_t hd, uint32_t link, corm_assoc_t cb, void *userdata)` | Link secondary map to primary. Callback extracts a single secondary key. |
| `corm_assoc_multi` | `void corm_assoc_multi(uint32_t hd, uint32_t link, corm_assoc_multi_t cb, void *userdata)` | Like `corm_assoc` but callback produces multiple secondary keys per primary entry. |

**Callback typedefs:**

| Type | Signature | Description |
|------|-----------|-------------|
| `corm_assoc_t` | `void (*)(const void **skey, const void *pkey, const void *value, void *userdata)` | Sets `*skey` to the secondary key. |
| `corm_assoc_multi_t` | `size_t (*)(const void **skeys, size_t max_skeys, const void *pkey, const void *value, void *userdata)` | Fills `skeys[]` array. Returns count written. |

### Record-Aware Maps

For structured data with named fields, register a record layout and open maps
with `CM_RECORD()` for field-level get/put and automatic reference resolution.

```c
// Register an "author" record with id + name fields
corm_record_field_t author_fields[] = {
    { "id",   CM_U32,        0, 0 },
    { "name", CM_STR,        0, 0 },
};
uint32_t rec_author = corm_record_register("author",
    sizeof(struct author), author_fields, 2);

// Open a record-aware map
uint32_t authors = corm_open("authors.db", "main",
    CM_STR, corm_record_type_id(rec_author), 0xFF, CM_RECORD());

// Field-level access
corm_field_put(authors, "alice", "id", "1");
corm_field_put(authors, "alice", "name", "Alice");
const char *name = corm_field_get(authors, "alice", "name");
```

| Function | Signature | Description |
|----------|-----------|-------------|
| `corm_record_register` | `uint32_t corm_record_register(const char *name, size_t struct_size, const corm_record_field_t *fields, size_t field_count)` | Register a record layout. Returns record ID or `CM_MISS`. |
| `corm_record_type_id` | `uint32_t corm_record_type_id(uint32_t record_id)` | Get type ID for use as `vtype` in `corm_open`. |
| `corm_get_key` | `const char *corm_get_key(uint32_t hd, uint32_t pos)` | Get the string key (item ID) at position `pos`. |
| `corm_pos` | `uint32_t corm_pos(uint32_t hd, const char *key)` | Get position number for a key (O(n) linear scan). |
| `corm_field_put` | `uint32_t corm_field_put(uint32_t hd, const char *item_id, const char *field_name, const char *value)` | Set a field value. Reference fields auto-resolve string IDs to positions. |
| `corm_field_get` | `const char *corm_field_get(uint32_t hd, const char *item_id, const char *field_name)` | Get a field value. Reference fields resolve positions back to string IDs. |
| `corm_inv_get` | `size_t corm_inv_get(uint32_t hd, const char *field_name, uint32_t target_pos, uint32_t *out, size_t max)` | Inverse index query: find source positions referencing `target_pos`. |
| `corm_record_field_set_target_hd` | `void corm_record_field_set_target_hd(uint32_t record_id, const char *field_name, uint32_t target_hd)` | Configure a reference field's target map handle after both maps exist. |

### ID Management

libcorm provides a free-list ID allocator (`idm_t`) for managing reusable
integer IDs alongside `CM_AINDEX` maps:

```c
idm_t mgr = idm_init();
uint32_t id = idm_new(&mgr);          // allocate (reuses freed IDs first)
idm_del(&mgr, id);                    // free a specific ID
idm_push(&mgr, 100);                  // bulk-push IDs 0..99
idm_drop(&mgr);                       // free all managed IDs
```

| Function | Signature | Description |
|----------|-----------|-------------|
| `idm_init` | `idm_t idm_init(void)` | Initialize empty ID manager. |
| `idm_new` | `uint32_t idm_new(idm_t *idm)` | Allocate a new ID (reuses freed IDs first). |
| `idm_del` | `int idm_del(idm_t *idm, uint32_t id)` | Free a specific ID. Returns 1 if `last` updated. |
| `idm_push` | `uint32_t idm_push(idm_t *idm, uint32_t n)` | Bulk-push IDs 0..n-1 onto free list. |
| `idm_drop` | `void idm_drop(idm_t *idm)` | Free all managed IDs. |
| `ids_init` | `ids_t ids_init(void)` | Initialize raw ID list. |
| `ids_push` | `void ids_push(ids_t *list, uint32_t id)` | Push ID onto free list. |
| `ids_pop` | `uint32_t ids_pop(ids_t *list)` | Pop ID from free list. Returns `IDM_MISS` if empty. |
| `ids_peek` | `uint32_t ids_peek(ids_t *list)` | Peek at top ID without removing. |
| `ids_drop` | `void ids_drop(ids_t *list)` | Free all IDs in list. |
| `ids_iter` | `idsi_t *ids_iter(ids_t *list)` | Get pointer to first node for iteration. |
| `ids_next` | `int ids_next(uint32_t *id, idsi_t **cur)` | Advance iterator. Returns 1 if valid, 0 if done. |

## Recall kernel (`rec.h`)

A domain-free **"filter by axis → join → rank"** loop over uniform 32-bit
refs (`rec_ref_t` = `uint32_t`, a corm auto-index ref). Axis libraries
(libjoint time, libislet space, stoma text, libsepal meaning)
stay independent domain stores and feed the kernel through small adapters;
consumers compose axes without re-writing join/rank code. Optional and
additive — raw entry points are untouched. Spec and design:
`docs/RECALL-KERNEL.md`.

```c
#include <ttypt/rec.h>

rec_set_t  *cands = rec_set_new();
rec_rank_t *board = rec_rank_new(10, 0.4f);   /* top-k, min-score */

rec_axis_fill_bbox(...);                      /* space axis (libislet) */
rec_axis_fill_interval(...);                  /* time axis (libjoint)  */
/* cands == in-box AND in-interval refs, sorted, deduped.              */

for (size_t i = 0; i < rec_set_count(cands); i++) {
    rec_ref_t ref = rec_set_at(cands)[i];
    float score;
    if (my_score(ref, &score) == 0)          /* ranking fn (consumer) */
        rec_rank_push(board, ref, score);
}
rec_rank_sorted(board, refs, scores);         /* best first            */
rec_set_free(cands);
rec_rank_free(board);
```

The adapter contract every axis follows: one
`int rec_axis_fill_*(params, rec_set_t *out)` that streams matching refs
into the set and seals it (0 ok / −1 error). Fill-only queries stay
streaming (an axis can push straight into `rec_rank_push`); a set is
materialized only when a second axis joins. The kernel never interprets a
ref — mapping back to a consumer schema is the consumer's job.

| Function | Signature | Description |
|----------|-----------|-------------|
| `rec_set_new` | `rec_set_t *rec_set_new(void)` | New empty, unsorted set (arena-backed). |
| `rec_set_push` | `void rec_set_push(rec_set_t *s, rec_ref_t r)` | Append a ref (unseals). |
| `rec_set_seal` | `void rec_set_seal(rec_set_t *s)` | Sort + dedup in place; required before joins. |
| `rec_set_fill_corm_iter` | `int rec_set_fill_corm_iter(rec_set_t *s, uint32_t hd)` | Drain a fixed-key corm handle into the set. |
| `rec_set_intersect` | `int rec_set_intersect(rec_set_t *dst, const rec_set_t *a, const rec_set_t *b)` | `dst := a ∩ b` (sealed, merge-join). |
| `rec_set_subtract` | `int rec_set_subtract(rec_set_t *dst, const rec_set_t *a, const rec_set_t *b)` | `dst := a − b` (sealed). |
| `rec_set_union` | `int rec_set_union(rec_set_t *dst, const rec_set_t *a, const rec_set_t *b)` | `dst := a ∪ b` (sealed). |
| `rec_set_count` | `size_t rec_set_count(const rec_set_t *s)` | Number of refs held. |
| `rec_set_at` | `const rec_ref_t *rec_set_at(const rec_set_t *s)` | Backing array (sealed → sorted, deduped). |
| `rec_set_free` | `void rec_set_free(rec_set_t *s)` | Release storage. |
| `rec_rank_new` | `rec_rank_t *rec_rank_new(size_t top_k, float min_score)` | New bounded top-k buffer (top_k 0 → NULL). |
| `rec_rank_push` | `void rec_rank_push(rec_rank_t *rk, rec_ref_t r, float score)` | Stream a scored ref; O(log top_k). |
| `rec_rank_sorted` | `size_t rec_rank_sorted(const rec_rank_t *rk, rec_ref_t *refs, float *scores)` | Retained results best-first; returns count. |
| `rec_rank_free` | `void rec_rank_free(rec_rank_t *rk)` | Release the buffer. |

## Full API reference

| Category | Function | Signature | Description |
|----------|----------|-----------|-------------|
| **Lifecycle** | `corm_open` | `uint32_t corm_open(const char *filename, const char *database, uint32_t ktype, uint32_t vtype, uint32_t mask, uint32_t flags)` | Open/create a map. |
| | `corm_save` | `void corm_save(void)` | Write all file-backed maps to disk. |
| | `corm_close` | `void corm_close(uint32_t hd)` | Close a map and free entries. |
| | `corm_drop` | `void corm_drop(uint32_t hd)` | Remove all entries (keep map open). |
| | `corm_get_vtype` | `uint32_t corm_get_vtype(uint32_t hd)` | Get value type ID for a map. |
| **CRUD** | `corm_get` | `const void *corm_get(uint32_t hd, const void *key)` | Get value by key. |
| | `corm_put` | `uint32_t corm_put(uint32_t hd, const void *key, const void *value)` | Insert/update key-value. |
| | `corm_del` | `void corm_del(uint32_t hd, const void *key)` | Delete entry by key (first match for MULTIVALUE). |
| | `corm_del_all` | `void corm_del_all(uint32_t hd, const void *key)` | Delete all entries matching key. |
| **Iteration** | `corm_iter` | `uint32_t corm_iter(uint32_t hd, const void *key, uint32_t flags)` | Start iteration over entries. |
| | `corm_next` | `int corm_next(const void **key, const void **value, uint32_t cur_id)` | Next key/value from cursor. |
| | `corm_fin` | `void corm_fin(uint32_t cur_id)` | End iteration. |
| | `corm_get_multi` | `uint32_t corm_get_multi(uint32_t hd, const void *key)` | Iterate all values for a MULTIVALUE key. |
| | `corm_count` | `uint32_t corm_count(uint32_t hd, const void *key)` | Count entries matching key. |
| **Types** | `corm_reg` | `uint32_t corm_reg(size_t len)` | Register fixed-length type. |
| | `corm_mreg` | `uint32_t corm_mreg(corm_measure_t *measure)` | Register variable-length type. |
| | `corm_type_len` | `size_t corm_type_len(uint32_t type_id)` | Get type byte length. |
| | `corm_len` | `size_t corm_len(uint32_t type_id, const void *data)` | Get element byte length. |
| | `corm_cmp_set` | `void corm_cmp_set(uint32_t ref, corm_cmp_t *cmp)` | Override comparison for a type. |
| **Assoc** | `corm_assoc` | `void corm_assoc(uint32_t hd, uint32_t link, corm_assoc_t cb, void *ud)` | Link secondary index. |
| | `corm_assoc_multi` | `void corm_assoc_multi(uint32_t hd, uint32_t link, corm_assoc_multi_t cb, void *ud)` | Link multi-key secondary index. |
| **Records** | `corm_record_register` | `uint32_t corm_record_register(const char *name, size_t struct_size, const corm_record_field_t *fields, size_t field_count)` | Register record layout. |
| | `corm_record_type_id` | `uint32_t corm_record_type_id(uint32_t record_id)` | Get type ID for record. |
| | `corm_get_key` | `const char *corm_get_key(uint32_t hd, uint32_t pos)` | Get key at position. |
| | `corm_pos` | `uint32_t corm_pos(uint32_t hd, const char *key)` | Get position for key. |
| | `corm_field_put` | `uint32_t corm_field_put(uint32_t hd, const char *item_id, const char *field_name, const char *value)` | Set field value. |
| | `corm_field_get` | `const char *corm_field_get(uint32_t hd, const char *item_id, const char *field_name)` | Get field value. |
| | `corm_inv_get` | `size_t corm_inv_get(uint32_t hd, const char *field_name, uint32_t target_pos, uint32_t *out, size_t max)` | Inverse index query. |
| | `corm_record_field_set_target_hd` | `void corm_record_field_set_target_hd(uint32_t record_id, const char *field_name, uint32_t target_hd)` | Set reference field target. |
| **ID mgmt** | `idm_init` | `idm_t idm_init(void)` | Init ID manager. |
| | `idm_new` | `uint32_t idm_new(idm_t *idm)` | Allocate ID. |
| | `idm_del` | `int idm_del(idm_t *idm, uint32_t id)` | Free ID. |
| | `idm_push` | `uint32_t idm_push(idm_t *idm, uint32_t n)` | Bulk-push IDs. |
| | `idm_drop` | `void idm_drop(idm_t *idm)` | Free all IDs. |
| | `ids_init` | `ids_t ids_init(void)` | Init raw ID list. |
| | `ids_push` | `void ids_push(ids_t *list, uint32_t id)` | Push ID. |
| | `ids_pop` | `uint32_t ids_pop(ids_t *list)` | Pop ID. |
| | `ids_peek` | `uint32_t ids_peek(ids_t *list)` | Peek at top ID. |
| | `ids_drop` | `void ids_drop(ids_t *list)` | Free all IDs in list. |
| | `ids_iter` | `idsi_t *ids_iter(ids_t *list)` | Start iteration. |
| | `ids_next` | `int ids_next(uint32_t *id, idsi_t **cur)` | Next ID. |

## Documentation

Use the man pages for complete library and CLI documentation:
```sh
man corm_open
man corm
```

If you prefer reading code, the CLI entry point is `src/corm.c`. The recall
kernel spec lives in [`docs/RECALL-KERNEL.md`](docs/RECALL-KERNEL.md).

## Testing

```sh
make test     # test.sh + test-cli.sh + test-roster.sh + test-fanout.sh …
```

From the repository root, `make boundary-check` runs the module-layer gates,
and `make test` runs the full platform suite.

## License

BSD 2-Clause License. Copyright (c) 2025, tty-pt. See `LICENSE`.