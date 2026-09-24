#ifndef CORM_H
#define CORM_H

/**
 * @file corm.h
 * @brief Public header for the Corm library.
 *
 * Declares the API for Corm — associative containers
 * with optional persistence, mirror maps and
 * sorted iteration.
 */

#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>

/**
 * @brief Sentinel value for "not found" or error conditions.
 *
 * Returned by corm_reg(), corm_mreg() when type limit is reached,
 * and used internally to indicate missing entries.
 * Equal to UINT32_MAX.
 */
#define CM_MISS ((uint32_t)-1)

/** @defgroup corm_constants Corm constants
 *  @brief Constant definitions for flags and built-in types.
 *  @see corm_common
 *  @see corm_assoc
 *  @see corm_iteration
 *  @see corm_type
 *  @{
 */

/**
 * @brief Corm flags.
 */
enum corm_flags {
  /** Auto–index for NULL keys. When enabled, passing NULL
   *  as the key to corm_put generates auto-incrementing IDs. */
  CM_AINDEX = 1,

  /** Create reverse-lookup (secondary) map. The mirror map
   *  handle is always primary_hd + 1 and swaps keys/values.
   *  
   *  CM_MIRROR is useful when you need bidirectional lookup (key→value
   *  and value→key) and is commonly used with file persistence.
   *  
   *  The mirror map is automatically closed when you close the primary
   *  map via corm_close(hd). */
  CM_MIRROR = 2,

  /** For associated maps: default to obtaining primary keys
   *  instead of values. Used internally for mirror maps and
   *  essential for creating value→key secondary indexes.
   *  
   *  When a secondary map has CM_PGET, corm_get() returns
   *  the primary key instead of the primary value. This allows
   *  secondary indexes to map from values back to keys. */
  CM_PGET = 4,

  /** Enable sorted index support (B-tree search). Enables
   *  ordered iteration. Index is automatically rebuilt on
   *  modifications (put/del operations mark it dirty).
   *  
   *  Performance Note: The sorted index is rebuilt from scratch
   *  whenever it's marked dirty and iteration is requested.
   *  This makes the first iteration after modifications O(n log n)
   *  instead of O(n). */
  CM_SORTED = 8,

  /** Allow duplicate keys in sorted maps. Enables multi-value
   *  lookups where multiple entries can share the same key.
   *  
   *  REQUIRES CM_SORTED: This flag cannot be used without CM_SORTED.
   *  corm_open() will return CM_MISS if CM_MULTIVALUE is set without
   *  CM_SORTED.
   *  
   *  Behavior:
   *  - corm_put() with an existing key ADDS a new entry (does not replace)
   *  - corm_get() returns the FIRST matching value
   *  - corm_get_multi() returns cursor to iterate over ALL matching values
   *  - corm_count() returns the number of entries for a key
   *  - corm_del() deletes only the FIRST matching entry
   *  - corm_del_all() deletes ALL entries with the specified key
   *  
   *  Use Case: Secondary indexes via corm_assoc() where multiple primary
   *  entries map to the same secondary key. */
  CM_MULTIVALUE = 16,

  /** Disable auto-grow. When set and the map reaches capacity,
   *  inserts return CM_MISS instead of growing the table.
   *  Use for memory-constrained environments or fixed-size tables. */
  CM_NOGROW = 32,
};

/**
 * @brief Macro and mask for record-aware maps.
 *
 * Pass CM_RECORD(record_id) as the flags parameter to corm_open to
 * enable record-aware access.  The map must have ktype=CM_STR and
 * vtype equal to the struct type ID returned by
 * corm_record_type_id(record_id).
 *
 * When a map is record-aware, keys containing ':' are treated as
 * composite keys "struct_key:field_name" and resolve to a pointer
 * directly into the stored struct's pool allocation at the field's
 * byte offset.  This allows whole-struct storage and per-field access
 * to coexist without data duplication.
 */
#define CM_RECORD_MASK   0x0000FF00u
#define CM_RECORD_FLAG   0x00010000u
#define CM_RECORD_ID(f)  (((f) & CM_RECORD_MASK) >> 8)
#define CM_RECORD(id)    (CM_RECORD_FLAG | (((id) & 0xFF) << 8))

/**
 * @brief Built-in type identifiers.
 */
enum corm_tbi {
  /** Pointer (hashed). */
  CM_PTR  = 0,

  /** Opaque handle. Uses value directly as hash (no transformation). */
  CM_HNDL = 1,

  /** String contents hash and compare. */
  CM_STR  = 2,

  /** 32-bit unsigned integer (hash and mask). */
  CM_U32  = 3,
};

/**
 * @brief Record field type constants.
 *
 * These are only meaningful as corm_record_field_t.type values,
 * NOT as key/value types for corm_open().
 */
#define CM_REFERENCE          6  /**< Single uint32_t position in target's map. */
#define CM_MULTI_REFERENCE    7  /**< \n-separated positions in char[max_size]. */
#define CM_VSTR               8  /**< Variable-length string stored outside the struct. */

/**
 * @brief Iterator flags.
 */
enum corm_if {
  /** Continue iteration even if key differs from the initial.
   *  Behavior depends on whether CM_SORTED was set on the map:
   *  - With CM_SORTED: performs ordered range scan from the
   *    starting key onwards (B-tree ordered iteration)
   *  - Without CM_SORTED: performs linear scan through the
   *    hash table, comparing keys with the initial key using
   *    the type's comparison function */
  CM_RANGE = 1,

  /** Lower-bound range: with CM_RANGE on a CM_SORTED map, start
   *  iteration at the first key >= the starting key and continue to
   *  the end (all entries in ascending key order, duplicates included).
   *  Unlike plain CM_RANGE, this applies to CM_MULTIVALUE maps too
   *  (plain CM_RANGE on a MULTIVALUE map iterates only the duplicates
   *  of the one exact starting key). Requires CM_SORTED; on unsorted
   *  maps it degrades to a full linear scan. */
  CM_RANGE_GE = 2,
};

/** @} */

/** @defgroup corm_handle Corm open, close and save
 *  @brief Functions for opening, closing and saving maps.
 *
 *  @note Corm uses global state and is not thread-safe.
 *
 *  @note Capacity Limits: Initial capacity is mask + 1. When capacity is
 *        reached, the map automatically grows (doubles) unless CM_NOGROW
 *        is set. Growth is transparent to callers.
 *
 *  @note Memory Allocation: Malloc failures trigger CBUG() which terminates
 *        the process immediately. There is no graceful error handling for
 *        out-of-memory conditions.
 *
 *  @warning File-backed maps are automatically saved at process exit.
 *           Explicit corm_save() calls are only needed for mid-execution
 *           persistence. The automatic save is triggered by a library
 *           destructor function registered during initialization.
 *  @{
 */

/**
 * @brief Open a database.
 *
 * Creates an in-memory map and registers its handle
 * with the internal file cache, linking it to
 * 'filename'. If a file exists, it loads the map
 * data for the specified 'database'.
 *
 * @param[in] filename Path to file or cache key.
 *                     NULL → in-memory only.
 * @param[in] database Logical name within file.
 *                     NULL → skip file association.
 * @param[in] ktype    Built-in or registered key
 *                     type.
 * @param[in] vtype    Built-in or registered value
 *                     type.
 * @param[in] mask     Must be 2ⁿ − 1; table size is
 *                     (mask + 1).
 * @param[in] flags    Bitwise OR of CM_AINDEX,
 *                     CM_MIRROR, CM_SORTED, etc.
 * @return             Map handle (hd).
 *
 * @note File Persistence: File-backed maps automatically load
 *       data from disk when opened, regardless of flags. The
 *       CM_MIRROR flag enables bidirectional lookup (creating a
 *       reverse map at handle hd + 1) which is useful for many
 *       persistence scenarios. The mirror map is automatically
 *       closed when closing the primary map.
 *
 * @note Multiple databases can share a single file. Each database
 *       is identified by a hash of its name (XXH32). Data is saved
 *       and loaded based on this database ID.
 */
uint32_t corm_open(const char *filename,
                   const char *database,
                   uint32_t ktype,
                   uint32_t vtype,
                   uint32_t mask,
                   uint32_t flags);

/**
 * @brief Returns the value type (vtype) of a map.
 */
uint32_t corm_get_vtype(uint32_t hd);

/**
 * @brief Returns the key type (ktype) of a map.
 */
uint32_t corm_get_ktype(uint32_t hd);

/**
 * @brief Returns the fixed length of a type, or 0 if variable.
 */
size_t corm_type_len(uint32_t type_id);

/**
 * @brief Write all file-backed maps to disk.
 *
 * Walks the internal cache, computes file sizes,
 * and performs mmap/memcpy writes for maps
 * associated with a file.
 *
 * @note This is automatically called at process exit.
 *       Explicit calls are only needed for mid-execution
 *       checkpointing or when you want to ensure data
 *       is persisted before continuing.
 */
void corm_save(void);

/**
 * @brief Close a map and free its resources.
 *
 * Deletes all entries, closes associated secondary maps,
 * and frees internal structures. All open maps are
 * automatically closed at process exit by the library
 * destructor, but you can call this explicitly to free
 * resources earlier.
 *
 * @param[in] hd Handle to close.
 */
void corm_close(uint32_t hd);

/** @} */

/** @defgroup corm_common Corm get, put, del and drop
 *  @brief Core key/value operations.
 *
 *  @warning Pointer Ownership and Lifetime:
 *           All pointers returned by corm functions (corm_get, corm_next)
 *           are owned by the map. They remain valid until:
 *           - The entry is DELETED (corm_del)
 *           - The map is CLOSED (corm_close)
 *           - The entry is REPLACED with a LARGER value (corm_put)
 *           - The key is CHANGED (corm_put with different key at same hash)
 *
 *           IMPROVED BEHAVIOR: Since v0.6.0, pointers typically remain valid
 *           when updating with the same key and a same-or-smaller value.
 *           However, for maximum safety, it's still recommended to copy
 *           data before modifications.
 *
 *           Do NOT:
 *           - Free returned pointers
 *           - Use pointers after the entry is deleted
 *           - Store pointers long-term without copying the data
 *
 *           SAFE patterns after allocation reuse improvement:
 *           @code
 *           // Updating same key with same-sized value - pointer usually stays valid
 *           uint32_t *counter = corm_get(hd, key);
 *           corm_put(hd, key, &(uint32_t){*counter + 1});
 *           // counter pointer is still valid (same allocation reused)
 *           
 *           // For maximum safety, still copy before complex operations:
 *           const uint32_t *old_ptr = corm_get(hd, key);
 *           uint32_t old_val = old_ptr ? *old_ptr : 0;
 *           corm_put(hd, key, &new_val);
 *           use(old_val);  // Safe - using copied value
 *           @endcode
 *
 *           UNSAFE pattern (pointer invalidated by larger value):
 *           @code
 *           const char *str = corm_get(hd, key);  // str = "short"
 *           corm_put(hd, key, "much longer string");
 *           // str is now INVALID - new allocation was needed
 *           @endcode
 *
 *  @note For CM_PTR type values, the returned pointer points to the
 *        stored pointer bytes, not the pointer itself.
 *
 *  @see corm_handle
 *  @see corm_assoc
 *  @see corm_iteration
 *  @see corm_type
 *  @{
 */

/**
 * @brief Retrieve a value by key.
 *
 * For maps with CM_MULTIVALUE flag, this returns the FIRST
 * matching value only. To retrieve all values for a key, use
 * corm_get_multi() instead.
 *
 * @param[in] hd  Map handle.
 * @param[in] key Key to look up.
 * @return        Pointer to value or NULL if not found.
 *                For CM_MULTIVALUE maps, returns first match.
 *                See corm_common for pointer ownership rules.
 */
const void *corm_get(uint32_t hd,
                     const void * const key);

/**
 * @brief Insert or update a pair.
 *
 * Behavior depends on the CM_MULTIVALUE flag:
 * - Without CM_MULTIVALUE: Replaces existing value if key exists
 * - With CM_MULTIVALUE: Always adds a new entry (duplicates allowed)
 *
 * @param[in] hd    Map handle.
 * @param[in] key   Key (NULL if CM_AINDEX).
 * @param[in] value Value to store.
 * @return          Internal index for the entry. With CM_AINDEX,
 *                  this is the generated key ID.
 */
uint32_t corm_put(uint32_t hd,
                  const void * const key,
                  const void * const value);

/**
 * @brief Delete an entry by key.
 *
 * For maps with CM_MULTIVALUE flag, this only deletes the FIRST
 * occurrence of the key. To delete all duplicates, call this
 * function multiple times until the key no longer exists.
 *
 * @param[in] hd  Map handle.
 * @param[in] key Key to delete.
 */
void corm_del(uint32_t hd,
              const void * const key);

/**
 * @brief Delete all entries with the specified key.
 *
 * For CM_MULTIVALUE maps, removes all duplicate entries. For regular maps,
 * behaves identically to corm_del().
 *
 * @param[in] hd  Map handle.
 * @param[in] key Key of entries to delete.
 *
 * @see corm_del
 * @see corm_get_multi
 */
void corm_del_all(uint32_t hd, const void * const key);

/**
 * @brief Remove all entries from a map.
 *
 * @param[in] hd Map handle.
 */
void corm_drop(uint32_t hd);

/** @} */

/** @defgroup corm_assoc Corm associations
 *  @brief Linking of primary and secondary maps.
 *  @see corm_handle
 *  @see corm_common
 *  @see corm_iteration
 *  @see corm_type
 *  @{
 */

/**
 * @brief Association callback type.
 *
 * After association, future puts/dels on the
 * primary will update the secondary.
 * Deletes on the primary remove corresponding
 * entries from the secondary.
 *
 * @param[out] skey     Pointer to set secondary key.
 * @param[in]  pkey     Primary key.
 * @param[in]  value    Primary value.
 * @param[in]  userdata User context pointer (from corm_assoc call).
 */
typedef void corm_assoc_t(
  const void **skey,
  const void * const pkey,
  const void * const value,
  void *userdata);

/**
 * @brief Make an association between tables.
 *
 * Links a secondary (index) map to a primary map so that
 * put/delete operations on the primary automatically update
 * the secondary. The callback determines the secondary key.
 *
 * @param[in] hd       Secondary (index) map handle.
 * @param[in] link     Primary (source) map handle.
 * @param[in] cb       Callback to produce secondary
 *                     keys. NULL → use primary value.
 * @param[in] userdata User context pointer passed to callback.
 *
 * @note The secondary map stores (secondary_key, primary_value).
 *       To retrieve the primary KEY instead of the primary VALUE,
 *       the secondary map must be created with the CM_PGET flag.
 *       This is essential for creating value→key indexes.
 *
 * @code
 * // Example: Create a secondary index from username -> user_id
 * // Primary: user_id -> username
 * uint32_t users = corm_open(NULL, NULL, CM_U32, CM_STR, 0xFF, 0);
 * 
 * // Secondary: username -> user_id
 * // CM_PGET makes corm_get return the primary key instead of value
 * uint32_t by_name = corm_open(NULL, NULL, CM_STR, CM_U32, 0xFF, CM_PGET);
 * 
 * // Callback: use primary value (username) as secondary key
 * static void value_to_key(const void **skey,
 *                          const void *pkey,
 *                          const void *value) {
 *     (void) pkey;
 *     *skey = value;  // Use username as key
 * }
 * 
 * corm_assoc(by_name, users, value_to_key);
 * 
 * // Now puts to 'users' automatically update 'by_name'
 * corm_put(users, &(uint32_t){100}, "alice");
 * // by_name now contains: "alice" -> 100
 * @endcode
 */
void corm_assoc(uint32_t hd,
                uint32_t link,
                corm_assoc_t cb,
                void *userdata);

/**
 * @brief Multi-key association callback type.
 *
 * Produces multiple secondary keys from a single primary entry.
 * Used by corm_assoc_multi for fields that reference multiple values
 * (e.g., multi-reference dataset fields).
 *
 * Callback fills skeys[0..returned_count-1] with key pointers.
 * Each key is copied by corm internally; callback-owned temporary
 * copies must remain valid until the callback returns.
 *
 * @param[out] skeys     Array to fill with secondary key pointers.
 * @param[in]  max_skeys Capacity of skeys array.
 * @param[in]  pkey      Primary key.
 * @param[in]  value     Primary value.
 * @param[in]  userdata  User context pointer (from corm_assoc_multi call).
 * @return               Number of keys written to skeys.
 */
typedef size_t corm_assoc_multi_t(
	const void **skeys,
	size_t max_skeys,
	const void *pkey,
	const void *value,
	void *userdata);

/**
 * @brief Make a multi-key association between tables.
 *
 * Like corm_assoc, but the callback produces multiple secondary keys
 * from a single primary entry. The secondary is a root map (independent
 * position space) with KEY=vtype=CM_STR, VALUE=ktype=CM_STR, and
 * CM_MULTIVALUE|CM_SORTED flags. Each entry stores (ref_value, primary_key).
 *
 * Puts to the primary automatically insert entries into the secondary.
 * Deletes from the primary automatically remove matching secondary entries.
 *
 * @param[in] hd       Secondary (index) map handle (root map, CM_STR/CM_STR).
 * @param[in] link     Primary (source) map handle.
 * @param[in] cb       Multi-key callback.
 * @param[in] userdata User context pointer passed to callback.
 */
void corm_assoc_multi(uint32_t hd,
                      uint32_t link,
                      corm_assoc_multi_t cb,
                      void *userdata);

/** @} */

/** @defgroup corm_iteration Corm iteration
 *  @brief Iteration through map contents.
 *  @see corm_handle
 *  @see corm_common
 *  @see corm_assoc
 *  @see corm_type
 *
 * @code
 * // Sorted range scan (requires CM_SORTED in corm_open).
 * uint32_t hd = corm_open(NULL, NULL, CM_U32, CM_U32,
 *                         0xFF, CM_SORTED);
 * uint32_t start = 100;
 * uint32_t cur = corm_iter(hd, &start, CM_RANGE);
 * const void *key, *value;
 * while (corm_next(&key, &value, cur)) {
 *     // handle key/value
 * }
 * corm_fin(cur);
 * @endcode
 *  @{
 */

/**
 * @brief Start iteration.
 *
 * Creates a cursor for iterating over map entries.
 * Ordered ranges require CM_SORTED flag on the map.
 *
 * @param[in] hd    Map handle.
 * @param[in] key   Starting key or NULL for all entries.
 * @param[in] flags Iterator flags (CM_RANGE valid).
 *                  - CM_RANGE with CM_SORTED: ordered scan
 *                  - CM_RANGE without CM_SORTED: linear scan
 *                  - No flags: iterate single key (or all if key is NULL)
 * @return          Cursor handle for use with corm_next.
 */
uint32_t corm_iter(uint32_t hd,
                   const void * const key,
                   uint32_t flags);

/**
 * @brief Fetch next key/value.
 *
 * @param[out] key    Pointer to key.
 * @param[out] value  Pointer to value.
 * @param[in]  cur_id Cursor handle.
 * @return            1 if valid, 0 if done.
 *                    See corm_common for pointer ownership rules.
 */
int corm_next(const void **key,
              const void **value,
              uint32_t cur_id);

/**
 * @brief End iteration early.
 *
 * @param[in] cur_id Cursor handle.
 */
void corm_fin(uint32_t cur_id);

/**
 * @brief Start iteration over all values for a key.
 *
 * For maps with CM_MULTIVALUE flag, this returns a cursor that
 * iterates over ALL values associated with the given key in
 * insertion order (the order the duplicates were put). For maps
 * without CM_MULTIVALUE, this behaves like a single-value iterator.
 *
 * @param[in] hd  Map handle.
 * @param[in] key Key to look up.
 * @return        Cursor handle for use with corm_next(), or
 *                CM_MISS if key not found.
 *
 * Example:
 * @code
 * uint32_t cur = corm_get_multi(hd, &key);
 * if (cur != CM_MISS) {
 *   const void *k, *v;
 *   while (corm_next(&k, &v, cur)) {
 *     // Process each value for this key
 *   }
 *   corm_fin(cur);
 * }
 * @endcode
 *
 * @note Internally, this walks the key's duplicate chain (O(k)) — it does
 *   NOT trigger the sorted-index rebuild, so it is cheap even on a dirty
 *   map and on maps that are never queried via sorted iteration.
 * @note For single-value lookups, corm_get() is more efficient
 * @see corm_del_all for deleting all duplicates at once
 * @see corm_count for counting entries without iteration
 */
uint32_t corm_get_multi(uint32_t hd, const void *key);

/**
 * @brief Count entries matching a key.
 *
 * @param[in] hd  Map handle.
 * @param[in] key Key to count. NULL counts total entries in map.
 * @return        Number of matching entries.
 *
 * @note For CM_MULTIVALUE maps, returns count of all duplicate values
 * @note For normal maps, returns 0 or 1
 */
uint32_t corm_count(uint32_t hd, const void *key);

/** @} */

/** @defgroup corm_type Corm type customization
 *  @brief Functions for registering and managing key/value types.
 *
 *  @note Type Limits: There is a compile-time limit on the number of
 *        custom types that can be registered. When the limit is reached,
 *        corm_reg() and corm_mreg() return CM_MISS and print an error
 *        message to stderr. The limit is determined by TYPES_MASK in
 *        the implementation.
 *
 *  @see corm_handle
 *  @see corm_common
 *  @see corm_assoc
 *  @see corm_iteration
 *  @{
 */

/**
 * @brief Callback to measure variable-size keys.
 *
 * Keys of dynamic length need measurement when
 * hashing/comparing beyond pointer equality.
 *
 * @param[in] data Pointer to key.
 * @return         Key size in bytes.
 */
typedef size_t corm_measure_t(const void *data);

/**
 * @brief Register a fixed-length type.
 *
 * Registers a new custom type with a fixed byte length.
 * The type will use the default hash (XXH32) and
 * comparison (memcmp) functions.
 *
 * @param[in] len Length in bytes.
 * @return        Type ID for use in corm_open, or
 *                CM_MISS if type limit is reached.
 */
uint32_t corm_reg(size_t len);

/**
 * @brief Comparison callback type.
 *
 * @param[in] a   First object.
 * @param[in] b   Second object.
 * @param[in] len Length in bytes.
 * @return        <0, 0, or >0.
 */
typedef int corm_cmp_t(
  const void * const a,
  const void * const b,
  size_t len);

/**
 * @brief Assign comparison function to a type.
 *
 * @param[in] ref Type ID.
 * @param[in] cmp Comparison callback.
 */
void corm_cmp_set(uint32_t ref,
                  corm_cmp_t *cmp);

/**
 * @brief Register a variable-length type.
 *
 * Registers a new custom type with variable length.
 * A measurement callback is required to determine the
 * size of each element. The type will use the default
 * hash (XXH32) and comparison (memcmp) functions.
 *
 * @param[in] measure Size-measuring callback.
 * @return            Type ID for use in corm_open, or
 *                    CM_MISS if type limit is reached.
 */
uint32_t corm_mreg(corm_measure_t *measure);

/**
 * @brief Get the byte length of an element.
 *
 * @param[in] type_id Type ID.
 * @param[in] data    Element pointer.
 * @return            Size in bytes.
 */
size_t corm_len(uint32_t type_id,
                const void *data);

/** @defgroup corm_record Record-Aware Maps
 *  @brief Functions for record-aware (struct-aware) maps.
 *
 * Record-aware maps store C struct values by-key and automatically
 * resolve composite keys (e.g. "id:fieldname") to field offsets
 * within the stored struct.  No per-field entries are stored — the
 * struct is the single source of truth.
 *
 * Usage:
 * @code
 * typedef struct { char title[256]; uint32_t age; } item_t;
 *
 * uint32_t rec = corm_record_register("item",
 *     sizeof(item_t),
 *     (corm_record_field_t[]){
 *         { "title", CM_STR, offsetof(item_t, title),
 *           sizeof(((item_t*)0)->title) },
 *         { "age",   CM_U32, offsetof(item_t, age),
 *           sizeof(uint32_t) },
 *     }, 2);
 *
 * uint32_t hd = corm_open(NULL, NULL, CM_STR,
 *                         corm_record_type_id(rec),
 *                         0xFF, CM_RECORD(rec));
 *
 * item_t row = { .title = "Hello", .age = 42 };
 * corm_put(hd, "item1", &row);        // store whole struct
 * const char *t = corm_get(hd, "item1:title");  // → &row.title
 * const uint32_t *a = corm_get(hd, "item1:age"); // → &row.age
 * @endcode
 *
 * @{
 */

/**
 * @brief Describes a single field in a record layout.
 * @see corm_record_register
 */
typedef struct {
  const char *name;      /**< Field name (e.g. "title"). */
  uint32_t type;         /**< CM_STR, CM_U32, CM_REFERENCE, CM_MULTI_REFERENCE. */
  size_t offset;         /**< offsetof(struct_type, field). */
  size_t max_size;       /**< Buffer capacity for inline CM_STR / CM_MULTI_REFERENCE arrays. */
  uint32_t target_record; /**< Record ID of the target map (0 = none). */
  uint32_t target_hd;     /**< Head handle for target map (corm_field_put auto-resolve). Set via corm_record_field_set_target_hd(). */
  const char *inverse;   /**< Field name on target for inverse lookups, or NULL. */
} corm_record_field_t;

/**
 * @brief Register a record layout describing a C struct.
 *
 * Internally calls corm_reg(struct_size) to create a fixed-length
 * type for the struct.  The returned record_id is used with
 * CM_RECORD(record_id) in corm_open().
 *
 * @param[in] name         Record name (for debugging, copied internally).
 * @param[in] struct_size  sizeof(struct).
 * @param[in] fields       Array of field descriptors.
 * @param[in] field_count  Number of fields.
 * @return Record ID for use with CM_RECORD(), or CM_MISS on failure.
 */
uint32_t corm_record_register(
  const char *name,
  size_t struct_size,
  const corm_record_field_t *fields,
  size_t field_count);

/**
 * @brief Get the struct type ID registered for a record.
 *
 * @param[in] record_id  Record ID from corm_record_register().
 * @return Type ID for use as vtype in corm_open(), or CM_MISS.
 */
uint32_t corm_record_type_id(uint32_t record_id);

/**
 * @brief Get the string key at a given position number.
 *
 * For record-aware maps this is the item ID (e.g. "choir1").
 *
 * @param[in] hd   Map handle.
 * @param[in] pos  Position number.
 * @return         Key string, or NULL if pos is out of range.
 */
const char *corm_get_key(uint32_t hd, uint32_t pos);

/**
 * @brief Get the position number for a given key string.
 *
 * Performs an O(n) linear scan of the map entries.  Used when
 * converting filesystem string IDs to position numbers for
 * reference fields.
 *
 * @param[in] hd   Map handle (must be record-aware, CM_STR keys).
 * @param[in] key  Key string to look up.
 * @return         Position number, or UINT32_MAX if not found.
 */
uint32_t corm_pos(uint32_t hd, const char *key);

/**
 * @brief Query the inverse index for a reference field.
 *
 * Returns all source positions that reference @p target_pos
 * via the named reference field.
 *
 * @param[in]  hd          Map handle.
 * @param[in]  field_name  Reference field name.
 * @param[in]  target_pos  Position in the target record-aware map.
 * @param[out] out         Array to fill with source positions.
 * @param[in]  max         Capacity of out[].
 * @return Number of positions written to out[].
 */
size_t corm_inv_get(uint32_t hd, const char *field_name,
                    uint32_t target_pos,
                    uint32_t *out, size_t max);

/**
 * @brief Set the target head handle for reference resolution.
 *
 * After both source and target maps exist, configure a reference
 * field so that corm_field_put() can auto-resolve string IDs
 * to positions via corm_pos(target_hd, id).
 *
 * @param[in] record_id    Record ID from corm_record_register().
 * @param[in] field_name   Field name.
 * @param[in] target_hd    Head handle of the target map (e.g. song.items).
 */
void corm_record_field_set_target_hd(uint32_t record_id,
                                     const char *field_name,
                                     uint32_t target_hd);

/**
 * @brief Put a field value into a record-aware map, auto-resolving
 *        references when the field is CM_REFERENCE / CM_MULTI_REFERENCE
 *        and target_hd is set.
 *
 * For CM_REFERENCE: resolves @p value (a string ID) to a position via
 * corm_pos(target_hd, value) before calling corm_put.
 * For CM_MULTI_REFERENCE: splits @p value on newlines, resolves each
 * ID, and joins the positions with newlines.
 * For CM_STR: passes @p value through unchanged.
 *
 * @return corm_put result (position number, or 0 on error).
 */
uint32_t corm_field_put(uint32_t hd, const char *item_id,
                        const char *field_name, const char *value);

/**
 * @brief Get a field value from a record-aware map, auto-resolving
 *        references when the field is CM_REFERENCE.
 *
 * Inverse of corm_field_put().
 *
 * For CM_STR: returns the string value directly (points into struct memory).
 * For CM_REFERENCE: resolves the stored position back to the target ID
 * string via corm_get_key(target_hd, pos).
 * For CM_MULTI_REFERENCE and other types: returns the raw value pointer
 * (caller must know the type).
 *
 * @param[in] hd         Record-aware map handle.
 * @param[in] item_id    Record key (entry ID).
 * @param[in] field_name Field name.
 * @return               String value (corm-managed, do not free), or NULL.
 */
const char *corm_field_get(uint32_t hd, const char *item_id,
                           const char *field_name);

/** @} */

#endif /* CORM_H */
