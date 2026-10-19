# libcorm — Complete API Reference

This document provides the exhaustive API reference, types, macros, callbacks, and function tables for `libcorm`. For a high-level overview, quickstart, and ecosystem context, see the [main README](../README.md).

## Type System

libcorm supports both built-in and custom types for keys and values.

### Built-in Type Constants

| Constant | Description |
|----------|-------------|
| `CM_U32` | 32-bit unsigned integer (4 bytes, ordered) |
| `CM_STR` | Null-terminated string (variable length, ordered lexicographically) |
| `CM_HNDL` | Map handle (uint32_t, used for maps-of-maps) |
| `CM_PTR` | Raw pointer (fixed-size, ordered by address) |

### Custom Type Registration

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

---

## Flags

| Flag | Value | Applies to | Description |
|------|-------|------------|-------------|
| `CM_SORTED` | — | `corm_open` | Keep entries sorted by key (required for `CM_MULTIVALUE`). |
| `CM_MULTIVALUE` | — | `corm_open` | Allow multiple values per key (requires `CM_SORTED`). |
| `CM_MIRROR` | — | `corm_open` | Create bidirectional reverse-lookup mirror (handle + 1). |
| `CM_AINDEX` | — | `corm_open` | Auto-index: assign sequential integer IDs for each unique key. |
| `CM_NOGROW` | — | `corm_open` | Disallow auto-growth beyond initial `mask` capacity. |
| `CM_RANGE` | — | `corm_iter` | Enable ordered range scan over sorted keys. |
| `CM_RECORD()` | — | `corm_open` | Declare vtype as a record type for field-level access. |

**Sentinel:** `CM_MISS` (`UINT32_MAX`) is returned by `corm_open`, `corm_reg`, and `corm_iter` on failure.

---

## Iteration

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

---

## Multi-Value Support (`CM_MULTIVALUE`)

For scenarios where multiple values need to share the same key (such as secondary indexes), use the `CM_MULTIVALUE` flag.

**Requirements**:
- Must be combined with `CM_SORTED` flag
- Opening a map with `CM_MULTIVALUE` without `CM_SORTED` will fail (returns `CM_MISS`)

**Behavior**:
- Multiple entries with the same key can coexist in the map
- `corm_get(hd, key)` returns the **first** value for the key
- `corm_del(hd, key)` deletes only the **first** occurrence
- Use `corm_get_multi()` to iterate over all values for a key
- Use `corm_count(hd, key)` to count entries for a specific key

**Deletion Patterns**:

```c
time_t max_time = 9999;
corm_del(by_time, &max_time);      // Delete first occurrence
corm_del_all(by_time, &max_time);  // Delete all occurrences
corm_drop(by_time);                // Drop all entries (clear entire map)
```

---

## Associations (Secondary Indexes)

Link a secondary (index) map to a primary map. Puts and deletes on the primary auto-update the secondary via a callback that extracts the secondary key:

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

---

## Record-Aware Maps

For structured data with named fields, register a record layout and open maps with `CM_RECORD()` for field-level get/put and automatic reference resolution.

| Function | Signature | Description |
|----------|-----------|-------------|
| `corm_record_register` | `uint32_t corm_record_register(const char *name, size_t struct_size, const corm_record_field_t *fields, size_t field_count)` | Register record layout. Returns record ID or `CM_MISS`. |
| `corm_record_type_id` | `uint32_t corm_record_type_id(uint32_t record_id)` | Get type ID for use as `vtype` in `corm_open`. |
| `corm_get_key` | `const char *corm_get_key(uint32_t hd, uint32_t pos)` | Get string key at position. |
| `corm_pos` | `uint32_t corm_pos(uint32_t hd, const char *key)` | Get position for key. |
| `corm_field_put` | `uint32_t corm_field_put(uint32_t hd, const char *item_id, const char *field_name, const char *value)` | Set field value. |
| `corm_field_get` | `const char *corm_field_get(uint32_t hd, const char *item_id, const char *field_name)` | Get field value. |
| `corm_inv_get` | `size_t corm_inv_get(uint32_t hd, const char *field_name, uint32_t target_pos, uint32_t *out, size_t max)` | Inverse index query. |
| `corm_record_field_set_target_hd` | `void corm_record_field_set_target_hd(uint32_t record_id, const char *field_name, uint32_t target_hd)` | Set reference field target. |

---

## ID Management

libcorm provides a free-list ID allocator (`idm_t`) for managing reusable integer IDs alongside `CM_AINDEX` maps:

| Function | Signature | Description |
|----------|-----------|-------------|
| `idm_init` | `idm_t idm_init(void)` | Initialize empty ID manager. |
| `idm_new` | `uint32_t idm_new(idm_t *idm)` | Allocate ID (reuses freed IDs first). |
| `idm_del` | `int idm_del(idm_t *idm, uint32_t id)` | Free ID. |
| `idm_push` | `uint32_t idm_push(idm_t *idm, uint32_t n)` | Bulk-push IDs. |
| `idm_drop` | `void idm_drop(idm_t *idm)` | Free all managed IDs. |
| `ids_init` | `ids_t ids_init(void)` | Init raw ID list. |
| `ids_push` | `void ids_push(ids_t *list, uint32_t id)` | Push ID. |
| `ids_pop` | `uint32_t ids_pop(ids_t *list)` | Pop ID. |
| `ids_peek` | `uint32_t ids_peek(ids_t *list)` | Peek at top ID. |
| `ids_drop` | `void ids_drop(ids_t *list)` | Free all IDs in list. |
| `ids_iter` | `idsi_t *ids_iter(ids_t *list)` | Start iteration. |
| `ids_next` | `int ids_next(uint32_t *id, idsi_t **cur)` | Next ID. |

---

## Recall Kernel (`cormr.h`)

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

---

## Complete Core API Reference Table

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
