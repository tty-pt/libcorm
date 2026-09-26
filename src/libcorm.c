/* libcorm.c
 * Licence: BSD-2-Clause
 *
 * I'm adding some comments to make it easier to understand,
 * but whatever's user API is documented in the header file.
 */
#include "./../include/ttypt/corm.h"
#include "./../include/ttypt/idm.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <xxhash.h>
#include <ttypt/qsys.h>
#include <limits.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/mman.h>

/* MACROS, STRUCTS, ENUMS AND GLOBALS {{{ */

#define CM_SEED 13
#define CM_DEFAULT_MASK 0xFF
#define CM_MAX 1024
#define CORM_POOL_STEP 16
#define CORM_POOL_MAX 4096
#define CORM_POOL_BINS (CORM_POOL_MAX / CORM_POOL_STEP)

#define TYPES_MASK 0xFF

#define DEBUG_LVL 1

/* Internal iterator flag: cursor walks a CM_MULTIVALUE duplicate chain
 * (via corm->mv_next[]) instead of hash/position space. */
#define CM_MVCHAIN 0x80000000u

#define DEBUG(lvl, ...) \
  if (DEBUG_LVL > lvl) WARN(__VA_ARGS__)

#define VAL_ADDR(corm, n) \
  (void **)(((char *) corm->table) \
      + sizeof(void *) * n)

typedef struct corm_blk {
  struct corm_blk *next;
  size_t size;
} corm_blk_t;

  static inline size_t
corm_payload_off(size_t key_len)
{
  size_t align = sizeof(void *) - 1;

  return (key_len + align) & ~align;
}

static_assert(CM_MISS == UINT32_MAX, "assume U32INT_MAX");

enum CM_MBR {
  CM_KEY,
  CM_VALUE,
};

enum qm_internal_flags {
  CM_SDIRTY = 1, // sorted list needs rebuild
  CM_IS_MIRROR = 2,  // this is a CM_MIRROR map (shares positions with primary)
};

typedef struct {
  uint32_t types[2], n, m, mask, flags,
           phd, sorted_n, iflags, dbid;
  uint32_t record_id;  /* 0 = not record-aware */
  uint32_t vstr_hd;    /* handle to CM_STR/CM_STR map for CM_VSTR fields, 0=lazy */
  const char *file;
  uint32_t *inv_hds;   /* per-field inverse map handles, calloc'd at open */
  char get_buf[64];    /* reusable formatting buffer for CM_U32/CM_REFERENCE */
} corm_head_t;

typedef struct {
  idm_t idm;

  uint32_t *map;  	// id -> n
  const void **omap;	// n -> key
  uint32_t *key_hashes;	// n -> cached key hash
  uint32_t *mv_next;	// n -> next duplicate position in CM_MULTIVALUE chain
  void **table;		// n -> values
  size_t *key_sizes;	// n -> size of allocated key
  size_t *val_sizes;	// n -> size of allocated value
  corm_blk_t *payload_bins[CORM_POOL_BINS];

  ids_t linked;
  corm_assoc_t *assoc;
  void *assoc_userdata;
  corm_assoc_multi_t *m_assoc;
  void *m_assoc_userdata;

  uint32_t *sorted_idx;
} corm_t;

  static inline void *
corm_payload_alloc(corm_t *corm, size_t key_len, size_t val_len)
{
  size_t raw = corm_payload_off(key_len) + val_len;
  size_t size = (raw + (CORM_POOL_STEP - 1)) & ~(CORM_POOL_STEP - 1);
  corm_blk_t *blk;
  uint32_t bin;

  if (size <= CORM_POOL_MAX) {
    bin = (uint32_t) (size / CORM_POOL_STEP - 1);
    blk = corm->payload_bins[bin];
    if (blk) {
      corm->payload_bins[bin] = blk->next;
      blk->next = NULL;
      blk->size = size;
      return (void *) (blk + 1);
    }
  } else {
    size = raw;
  }

  blk = malloc(sizeof(*blk) + size);
  CBUG(!blk, "malloc error (payload)\n");
  blk->next = NULL;
  blk->size = size;
  return (void *) (blk + 1);
}

  static inline void
corm_payload_free(corm_t *corm, void *key)
{
  corm_blk_t *blk;
  uint32_t bin;

  if (!key)
    return;

  blk = ((corm_blk_t *) key) - 1;
  if (blk->size <= CORM_POOL_MAX) {
    bin = (uint32_t) (blk->size / CORM_POOL_STEP - 1);
    blk->next = corm->payload_bins[bin];
    corm->payload_bins[bin] = blk;
  } else
    free(blk);
}

  static inline void
corm_payload_flush(corm_t *corm)
{
  for (size_t i = 0; i < CORM_POOL_BINS; i++) {
    corm_blk_t *blk = corm->payload_bins[i];
    while (blk) {
      corm_blk_t *next = blk->next;
      free(blk);
      blk = next;
    }
    corm->payload_bins[i] = NULL;
  }
}

  static inline size_t
corm_payload_cap(const void *key)
{
  const corm_blk_t *blk = ((const corm_blk_t *) key) - 1;
  return blk->size;
}

typedef struct {
  uint32_t hd, pos, sub_cur, ipos, end_pos, flags;
  size_t key_len;
  const void * key;
} corm_cur_t;

typedef uint32_t corm_hash_t(
    const void * const key,
    size_t len);

typedef struct {
  size_t len;
  corm_measure_t *measure;
  corm_hash_t *hash;
  corm_cmp_t *cmp;
} corm_type_t;

typedef struct {
  ids_t ids;
  int fd;
  char *mmaped;
  size_t size;
} corm_file_t;

static corm_head_t corm_heads[CM_MAX];
static corm_t corms[CM_MAX];
static corm_cur_t corm_cursors[CM_MAX];
static idm_t idm, cursor_idm;
static uint32_t _qsort_cmp_hd;

static corm_type_t corm_types[TYPES_MASK + 1];
static uint32_t types_n = 0;

static uint32_t corm_files_hd, corm_dbs_hd;
static int mdbs[CM_MAX];

/* ── Record-aware map support ─────────────────────────────────────────── */

#define CORM_MAX_RECORDS 64
#define CORM_MAX_RECORD_FIELDS 32

typedef struct {
  char name[64];
  size_t struct_size;
  uint32_t struct_type_id;
  struct {
    char name[64];
    uint32_t type;
    size_t offset;
    size_t max_size;
    uint32_t target_record;
    uint32_t target_hd;
    char inverse[64];
  } fields[CORM_MAX_RECORD_FIELDS];
  size_t field_count;
} corm_record_t;

static corm_record_t corm_records[CORM_MAX_RECORDS];
static uint32_t corm_records_n = 0;

/* ── Record field lookup helper ───────────────────────────────────────── */

static int corm_record_find_field(uint32_t record_id, const char *field_name)
{
  if (!record_id || record_id > corm_records_n) return -1;
  for (size_t i = 0; i < corm_records[record_id].field_count; i++) {
    if (strcmp(corm_records[record_id].fields[i].name, field_name) == 0)
      return (int)i;
  }
  return -1;
}

/* }}} */

/* BUILT-INS {{{ */

  static uint32_t
corm_nohash(const void * const key, size_t len UNUSED)
{
  uint32_t u;
  memcpy(&u, key, sizeof(u));
  return u;
}

static uint32_t
corm_chash(const void *data, size_t len) {
  return XXH32(data, len, CM_SEED);
}

  static int
corm_ccmp(const void * const a,
    const void * const b,
    size_t len)
{
  return memcmp((char *) a, (char *) b, len);
}

  static int
corm_scmp(const void * const a,
    const void * const b,
    size_t len UNUSED)
{
  return strcmp((const char *)a, (const char *)b);
}

  static int
corm_ucmp(const void * const a,
    const void * const b,
    size_t len UNUSED)
{
  uint32_t ua = * (const uint32_t *) a;
  uint32_t ub = * (const uint32_t *) b;
  if (ua < ub) return -1;
  if (ua > ub) return 1;
  return 0;
}

  static void
corm_rassoc(const void **skey,
    const void * const pkey UNUSED,
    const void * const value,
    void *userdata UNUSED)
{
  (void)userdata;
  *skey = value;
}

/* }}} */

/* HELPER FUNCTIONS {{{ */

/* Easily obtain the pointer to the key */
  static inline void *
corm_key(uint32_t hd, uint32_t n)
{
  corm_t *corm = &corms[hd];
  return (void *) corm->omap[n];
}

/* Easily obtain the pointer to the value */
static inline void *
corm_val(uint32_t hd, uint32_t n) {
  corm_head_t *head = &corm_heads[hd];
  corm_t *pcorm;

  if (head->flags & CM_PGET)
    return corm_key(head->phd, n);

  pcorm = &corms[head->phd];
  return * VAL_ADDR(pcorm, n);
}

/* In some cases we want to calculate the id based on the
 * corm's hash function and the key, and the mask. Other
 * times it's not useful to do that. This is for when it is.
 *
 * When requested, also returns the computed key length/hash so
 * callers that need to store the metadata do not recompute it.
 */
static inline uint32_t
corm_id_hash(uint32_t hd, const void * const key,
    size_t key_len, uint32_t key_hash);

  static inline uint32_t
corm_id_ex(uint32_t hd, const void * const key,
    size_t *key_len_out, uint32_t *key_hash_out)
{
  corm_head_t *head = &corm_heads[hd];
  uint32_t ktype = head->types[CM_KEY];
  corm_type_t *type = &corm_types[ktype];

  size_t key_len;
  uint32_t key_hash;

  if (ktype == CM_STR) {
    key_len = strlen((const char *)key) + 1;
    key_hash = XXH32(key, key_len, CM_SEED);
  } else if (ktype == CM_U32 || ktype == CM_HNDL) {
    key_len = sizeof(uint32_t);
    key_hash = *(const uint32_t *)key;
  } else if (ktype == CM_PTR) {
    key_len = sizeof(void *);
    key_hash = XXH32(key, key_len, CM_SEED);
  } else {
    key_len = type->measure ? type->measure(key) : type->len;
    key_hash = type->hash(key, key_len);
  }

  if (key_len_out)
    *key_len_out = key_len;
  if (key_hash_out)
    *key_hash_out = key_hash;

  return corm_id_hash(hd, key, key_len, key_hash);
}

  static inline uint32_t
corm_id_hash(uint32_t hd, const void * const key,
    size_t key_len, uint32_t key_hash)
{
  corm_head_t *head = &corm_heads[hd];
  corm_t *corm = &corms[hd];
  uint32_t ktype = head->types[CM_KEY];
  uint32_t id = key_hash & head->mask;
  uint32_t probe_count = 0;

  while (1) {
    uint32_t n = corm->map[id];

    if (n == CM_MISS)
      return id;

    if (corm->key_hashes[n] == key_hash) {
      const void *okey = corm_key(hd, n);
      if (okey) {
        if (ktype == CM_STR) {
          if (memcmp(okey, key, key_len) == 0)
            return id;
        } else if (ktype == CM_U32 || ktype == CM_HNDL) {
          if (*(const uint32_t *)okey == *(const uint32_t *)key)
            return id;
        } else if (ktype == CM_PTR) {
          if (*(const void * const *)okey == *(const void * const *)key)
            return id;
        } else {
          corm_type_t *type = &corm_types[ktype];
          size_t len;
          if (type->measure) {
            size_t okey_len = corm->key_sizes[n];
            if (okey_len != key_len)
              goto next_probe;
            len = key_len;
          } else {
            len = type->len;
          }
          if (type->cmp(okey, key, len) == 0)
            return id;
        }
      }
    }

next_probe:
    id = (id + 1) & head->mask;

    if (++probe_count >= head->m)
      return CM_MISS;
  }
}

  static inline uint32_t
corm_id(uint32_t hd, const void * const key)
{
  return corm_id_ex(hd, key, NULL, NULL);
}

/* Close the hole left by clearing map[d] by shifting the following cluster
 * left, preserving the no-holes invariant: no live element may have an
 * empty slot in front of its home-to-slot probe path. A position p at slot j
 * is moved into the hole at d iff d lies within its cyclic probe interval
 * [ideal(p), j) (i.e. the hole is reachable from home before j), with
 * ideal(p) = key_hashes[p] & mask. The scan CONTINUES past non-movable
 * elements (their home is past the hole; a later wrapped element may still
 * be movable) and stops only at an EMPTY slot or after head->m probes.
 * Only slot -> position entries move; per-key duplicate chains store
 * positions, so they are unaffected. O(cluster). */
  static inline void
corm_backshift(uint32_t hd, uint32_t d)
{
  corm_head_t *head = &corm_heads[hd];
  corm_t *corm = &corms[hd];
  uint32_t mask = head->mask;
  uint32_t j = d;
  uint32_t probe_count = 0;

  while (probe_count < head->m) {
    j = (j + 1) & mask;
    probe_count++;

    uint32_t p = corm->map[j];
    if (p == CM_MISS)
      break;

    uint32_t ideal = corm->key_hashes[p] & mask;
    if ((uint32_t)(d - ideal) < (uint32_t)(j - ideal)) {
      corm->map[d] = p;
      corm->map[j] = CM_MISS;
      d = j;
    }
  }
}

/* Append position n to the duplicate chain rooted at head (tail-append
 * preserves insertion order). n's next pointer is reset to CM_MISS. */
  static inline void
corm_mv_link(corm_t *corm, uint32_t head, uint32_t n)
{
  uint32_t tail = head;
  while (corm->mv_next[tail] != CM_MISS)
    tail = corm->mv_next[tail];
  corm->mv_next[tail] = n;
  corm->mv_next[n] = CM_MISS;
}

/* Unlink position n from the chain rooted at head; return the promoted
 * head (the next duplicate, or CM_MISS when n was the only member). n's
 * next pointer is reset to CM_MISS. */
  static uint32_t
corm_mv_unlink(corm_t *corm, uint32_t head, uint32_t n)
{
  uint32_t prev = CM_MISS, p = head;
  while (p != CM_MISS && p != n) {
    prev = p;
    p = corm->mv_next[p];
  }
  if (p == CM_MISS) {
    /* Not a member of this chain; leave everything untouched */
    return head;
  }
  if (prev == CM_MISS)
    return corm->mv_next[n];
  corm->mv_next[prev] = corm->mv_next[n];
  corm->mv_next[n] = CM_MISS;
  return head;
}

/* }}} */

/* B-TREE SUPPORT HELPERS {{{ */

  static int
corm_n_cmp(const void *a, const void *b)
{
  uint32_t n_a = *(const uint32_t *)a;
  uint32_t n_b = *(const uint32_t *)b;

  const void *key_a = corm_key(_qsort_cmp_hd, n_a);
  const void *key_b = corm_key(_qsort_cmp_hd, n_b);

  if (key_a == NULL || key_b == NULL)
    return 0;

  corm_head_t *head = &corm_heads[_qsort_cmp_hd];
  corm_t *corm = &corms[_qsort_cmp_hd];
  corm_type_t *type = &corm_types[head->types[CM_KEY]];

  if (type->measure) {
    size_t len_a = corm->key_sizes[n_a];
    size_t len_b = corm->key_sizes[n_b];
    size_t len = (len_a > len_b) ? len_a : len_b;
    return type->cmp(key_a, key_b, len);
  }

  return type->cmp(key_a, key_b, type->len);
}

  static void
corm_rebuild_sorted(uint32_t hd)
{
  corm_head_t *head = &corm_heads[hd];
  corm_t *corm = &corms[hd];
  uint32_t n_idx = 0;

  for (uint32_t n = 0; n < corm->idm.last; n++) {
    if (corm->omap[n] != NULL)
      corm->sorted_idx[n_idx++] = n;
  }
  head->sorted_n = n_idx;

  _qsort_cmp_hd = hd;
  qsort(corm->sorted_idx, head->sorted_n,
      sizeof(uint32_t), corm_n_cmp);

  head->iflags &= ~CM_SDIRTY;
}

/* Binary search modes */
enum {
  CORM_BSEARCH_ANY = 0,    /* Find any match (original behavior) */
  CORM_BSEARCH_FIRST = 1,  /* Find first occurrence */
  CORM_BSEARCH_LAST = 2    /* Find last occurrence */
};

/* Unified binary search with mode parameter.
 * For CORM_BSEARCH_ANY: returns insertion point if not found, sets *exact
 * For CORM_BSEARCH_FIRST/LAST: returns position or -1 if not found */
  static int
corm_bsearch_ex(uint32_t hd, const void *key, int *exact, int mode)
{
  corm_head_t *head = &corm_heads[hd];
  corm_t *corm = &corms[hd];
  int result = -1;

  if (head->iflags & CM_SDIRTY)
    corm_rebuild_sorted(hd);

  if (head->sorted_n == 0) {
    if (exact) *exact = 0;
    return (mode == CORM_BSEARCH_ANY) ? 0 : -1;
  }

  corm_type_t *type = &corm_types[head->types[CM_KEY]];
  size_t key_len = corm_len(head->types[CM_KEY], key);
  int low = 0, high = (int) head->sorted_n - 1;
  int mid = 0;

  if (exact) *exact = 0;

  while (low <= high) {
    mid = low + (high - low) / 2;
    const void *mid_key = corm_key(hd, corm->sorted_idx[mid]);

    size_t len;
    if (type->measure) {
      size_t mid_len = corm->key_sizes[corm->sorted_idx[mid]];
      len = (key_len > mid_len) ? key_len : mid_len;
    } else
      len = type->len;

    int cmp = type->cmp(mid_key, key, len);

    if (cmp == 0) {
      if (exact) *exact = 1;
      result = mid;
      if (mode == CORM_BSEARCH_ANY)
        return mid;  /* Return immediately for ANY mode */
      else if (mode == CORM_BSEARCH_FIRST)
        high = mid - 1;  /* Continue searching left */
      else /* CORM_BSEARCH_LAST */
        low = mid + 1;   /* Continue searching right */
    } else if (cmp < 0)
      low = mid + 1;
    else
      high = mid - 1;
  }

  return (mode == CORM_BSEARCH_ANY && (exact == NULL || !*exact)) ? low : result;
}

/* Wrapper for backward compatibility with original corm_bsearch */
  static inline int
corm_bsearch(uint32_t hd, const void *key, int *exact)
{
  return corm_bsearch_ex(hd, key, exact, CORM_BSEARCH_ANY);
}

/* OPEN / INITIALIZATION {{{ */

/* Low level way of opening databases. */
  static uint32_t
_corm_open(uint32_t ktype, uint32_t vtype,
    uint32_t mask, uint32_t flags)
{
  uint32_t hd = idm_new(&idm);
  corm_head_t *head = &corm_heads[hd];
  corm_t *corm = &corms[hd];
  uint32_t len;
  size_t ids_len;

  mask = mask ? mask : CM_DEFAULT_MASK;

  /* CM_MULTIVALUE requires CM_SORTED */
  if ((flags & CM_MULTIVALUE) && !(flags & CM_SORTED)) {
    fprintf(stderr, "corm: CM_MULTIVALUE requires CM_SORTED flag\n");
    idm_del(&idm, hd);
    return CM_MISS;
  }

  DEBUG(1, "%u %u 0x%x %u\n",
      hd, ktype,
      mask, flags);

  len = mask + 1u;

  CBUG((len & mask) != 0, "mask must be 2^k - 1\n");
  ids_len = len * sizeof(uint32_t);

  corm->map = malloc(ids_len);
  corm->omap = malloc(len * sizeof(void *));
  CBUG(!(corm->map && corm->omap), "malloc error\n");
  corm->idm = idm_init();
  corm->linked = ids_init();

  head->m = len;
  head->types[CM_KEY] = ktype;
  head->types[CM_VALUE] = vtype;
  head->mask = mask;
  head->flags = flags;
  head->phd = hd;

  // STORE {{{
  corm->table = malloc(sizeof(void *) * len);
  CBUG(!corm->table, "malloc error (table)\n");
  memset(corm->table, 0, sizeof(void *) * len);
  // }}}

  corm->key_hashes = calloc(len, sizeof(*corm->key_hashes));
  CBUG(!corm->key_hashes, "malloc error (key_hashes)\n");

  corm->mv_next = malloc(sizeof(uint32_t) * len);
  CBUG(!corm->mv_next, "malloc error (mv_next)\n");
  memset(corm->mv_next, 0xFF, sizeof(uint32_t) * len);   /* CM_MISS */

  if (flags & CM_SORTED) {
    corm->sorted_idx = malloc(sizeof(uint32_t) * len);
    CBUG(!corm->sorted_idx, "malloc error (sorted_idx)\n");
  } else
    corm->sorted_idx = NULL;

  corm->key_sizes = calloc(len, sizeof(*corm->key_sizes));
  corm->val_sizes = calloc(len, sizeof(*corm->val_sizes));
  CBUG(!(corm->key_sizes && corm->val_sizes), "malloc error (size arrays)\n");

  head->iflags |= CM_SDIRTY;
  head->sorted_n = 0;

  memset(corm->map, 0xFF, ids_len);
  memset(corm->omap, 0, sizeof(void *) * len);

  return hd;
}

static inline void
corm_load_file(char *filename, uint32_t dbid, uint32_t hd);

static inline uint32_t
_corm_put(uint32_t hd, const void * key,
    const void *value, uint32_t pn);

  static void
corm_rebuild_map(uint32_t hd)
{
  corm_head_t *head = &corm_heads[hd];
  corm_t *corm = &corms[hd];

  memset(corm->map, 0xFF,
      sizeof(uint32_t) * head->m);

  /* For CM_MULTIVALUE maps, duplicate chains live entirely in corm->mv_next[]
   * (per-key, in insertion order) and are NOT rebuilt here. The hash table
   * must simply re-point each key's home slot at its chain head. Positions
   * are stable across whole-key deletes and slot reclaims, so the existing
   * chain order is authoritative; a position is a chain head iff no LIVE
   * member links to it via mv_next. (Rebuilding chains by position order
   * instead would scramble insertion order the moment a freed low slot is
   * reclaimed by a later duplicate.) */
  int mv = (head->flags & CM_MULTIVALUE) != 0;
  uint32_t *has_pred = NULL;
  if (mv) {
    has_pred = malloc(sizeof(uint32_t) * corm->idm.last);
    CBUG(!has_pred, "malloc error (has_pred)\n");
    for (uint32_t i = 0; i < corm->idm.last; i++)
      has_pred[i] = 0;
    /* Mark every live position that a live chain member links to. */
    for (uint32_t n = 0; n < corm->idm.last; n++) {
      if (!corm->omap[n])
        continue;
      uint32_t mvn = corm->mv_next[n];
      if (mvn != CM_MISS && mvn < corm->idm.last && corm->omap[mvn])
        has_pred[mvn] = 1;
    }
  }

  for (uint32_t n = 0; n < corm->idm.last; n++) {
    const void *key = corm->omap[n];

    if (!key)
      continue;

    uint32_t id;
    if (mv) {
      id = corm_id_hash(hd, key, corm->key_sizes[n],
                        corm->key_hashes[n]);
      if (id == CM_MISS)
        continue;
      /* Only a chain head (no live predecessor) gets the home slot; the
       * chain itself is left untouched, so its order is preserved. */
      if (!has_pred[n])
        corm->map[id] = n;
    } else {
      id = corm->key_hashes[n] & head->mask;
      while (corm->map[id] != CM_MISS)
        id = (id + 1) & head->mask;
      corm->map[id] = n;
    }
  }

  free(has_pred);
}

  /* Grow one bookkeeping array to new_m slots: realloc + fill the fresh
 * tail. Every corm_grow array funnels through here. */
static void
qchunk_grow(void **pp, uint32_t old_m, uint32_t new_m,
    size_t esize, uint8_t fill)
{
  void *tmp = realloc(*pp, esize * new_m);
  CBUG(!tmp, "corm_grow: realloc");

  *pp = tmp;
  memset((char *)*pp + esize * old_m, fill,
      esize * (new_m - old_m));
}

  static void
corm_grow(uint32_t hd)
{
  corm_head_t *head = &corm_heads[hd];
  corm_t *corm = &corms[hd];
  uint32_t old_m = head->m;
  uint32_t new_m = old_m << 1;

  CBUG((new_m & (new_m - 1)) != 0,
      "corm_grow: capacity not power-of-two");

  qchunk_grow((void **)&corm->omap, old_m, new_m, sizeof(void *), 0);
  if (corm->table)
    qchunk_grow((void **)&corm->table, old_m, new_m, sizeof(void *), 0);
  qchunk_grow((void **)&corm->key_hashes, old_m, new_m,
      sizeof(uint32_t), 0);
  qchunk_grow((void **)&corm->mv_next, old_m, new_m,
      sizeof(uint32_t), 0xFF);
  qchunk_grow((void **)&corm->key_sizes, old_m, new_m, sizeof(size_t), 0);
  qchunk_grow((void **)&corm->val_sizes, old_m, new_m, sizeof(size_t), 0);
  if (corm->sorted_idx)
    qchunk_grow((void **)&corm->sorted_idx, old_m, new_m,
        sizeof(uint32_t), 0xFF);

  /* map is a full spread, not a size-up: free + fresh mask. */
  free(corm->map);
  corm->map = malloc(sizeof(uint32_t) * new_m);
  CBUG(!corm->map, "malloc(map)");
  memset(corm->map, 0xFF, sizeof(uint32_t) * new_m);

  head->m = new_m;
  head->mask = new_m - 1;

  CBUG(head->m != head->mask + 1,
      "corm invariant broken");

  corm_rebuild_map(hd);
}

  uint32_t /* API */
corm_open(const char *filename,
    const char *database,
    uint32_t ktype, uint32_t vtype,
    uint32_t mask, uint32_t flags)
{
  uint32_t record_id = 0;

  /* ── Handle CM_RECORD flag ────────────────────────────────────────── */
  if (flags & CM_RECORD_FLAG) {
    record_id = CM_RECORD_ID(flags);
    if (!record_id || record_id > corm_records_n) {
      fprintf(stderr, "corm_open: unknown record_id %u\n", record_id);
      return CM_MISS;
    }
    /* Validate: vtype must match the registered struct type */
    if (vtype != corm_records[record_id].struct_type_id) {
      fprintf(stderr, "corm_open: record %u requires vtype=%u, got %u\n",
              record_id, corm_records[record_id].struct_type_id, vtype);
      return CM_MISS;
    }
    /* Record-aware maps require string keys (composite key separator) */
    if (ktype != CM_STR) {
      fprintf(stderr, "corm_open: record maps require ktype=CM_STR\n");
      return CM_MISS;
    }
  }

  /* Strip record bits so _corm_open doesn't see them */
  flags &= ~(CM_RECORD_MASK | CM_RECORD_FLAG);

  /* 2B-5 (F4): opening the same (file, map) twice with the same key/value
   * shape must alias the LIVE handle, not orphan it. The old path below
   * registered the new handle in corm_dbs_hd and marked the old one dead
   * (mdbs[old]=0), so the second handle's as-of-open copy won the
   * exit-save — e.g. libstoma's sidecar-scan mirror-open of the primary
   * the CLI already held silently dropped -p seeds and -d forgets.
   * Shape match = same record type, key/value types, and table mask
   * (normalized like _corm_open does). Membership in the file's ids proves
   * the handle is still live (corm_close removes it there). */
  if (filename && database) {
    char abuf[strlen(filename)
      + strlen(database) + 2];

    snprintf(abuf, sizeof(abuf), "%s/%s",
        filename, database);

    const uint32_t *eahd = corm_get(corm_dbs_hd, abuf);
    uint32_t ahd = eahd ? *eahd : CM_MISS;
    if (ahd != CM_MISS && mdbs[ahd]) {
      const corm_file_t *afile
        = corm_get(corm_files_hd, filename);
      idsi_t *acur;
      uint32_t ah;
      int live = 0;

      if (afile) {
        acur = (idsi_t *) ids_iter((ids_t *) &afile->ids);
        while (ids_next(&ah, &acur))
          if (ah == ahd) {
            live = 1;
            break;
          }
      }
      if (live) {
        corm_head_t *ahead = &corm_heads[ahd];
        uint32_t amask = mask ? mask : CM_DEFAULT_MASK;
        if (ahead->record_id == record_id
            && ahead->types[CM_KEY] == ktype
            && ahead->types[CM_VALUE] == vtype
            && ahead->mask == amask)
          return ahd;
      }
    }
  }

  uint32_t hd = _corm_open(ktype, vtype, mask, flags);

  /* Check if open failed */
  if (hd == CM_MISS)
    return CM_MISS;

  corm_head_t *head = &corm_heads[hd];

  head->record_id = record_id;
  head->vstr_hd = 0;
  head->file = filename;

  /* Allocate per-field inverse index handles for record-aware maps */
  if (record_id > 0) {
    uint32_t fc = (uint32_t)corm_records[record_id].field_count;
    head->inv_hds = calloc(fc, sizeof(uint32_t));
  } else {
    head->inv_hds = NULL;
  }
  head->get_buf[0] = '\0';
  if (database)
    head->dbid = XXH32(database, strlen(database), CM_SEED);
  else
    head->dbid = CM_MISS;

  if (!filename)
    goto file_skip;

  if (database) {
    char buf[strlen(filename)
      + strlen(database) + 2];

    snprintf(buf, sizeof(buf), "%s/%s",
        filename, database);

    const uint32_t *ehd = corm_get(corm_dbs_hd, buf);
    uint32_t old_hd = ehd ? *ehd : CM_MISS;
    corm_put(corm_dbs_hd, buf, &hd);

    if (old_hd != CM_MISS && mdbs[old_hd])
      mdbs[old_hd] = 0;
  }

  mdbs[hd] = 1;  /* Mark as dirty for save, regardless of database name */

  const corm_file_t *file_p
    = corm_get(corm_files_hd, filename);

  if (!file_p) {
    corm_file_t file;
    memset(&file, 0, sizeof(file));
    file.ids = ids_init();
    file.fd = -1;
    ids_push(&file.ids, hd);
    corm_put(corm_files_hd, filename, &file);
  } else
    ids_push((ids_t *) &file_p->ids, hd);

file_skip:
  if (filename)
    corm_load_file((char*) filename, head->dbid, hd);

  if (!(flags & CM_MIRROR))
    return hd;

  flags &= ~CM_AINDEX;
  uint32_t mirror_hd = _corm_open(vtype, ktype, mask, flags | CM_PGET);
  corm_heads[mirror_hd].iflags |= CM_IS_MIRROR;  /* Mark as mirror for position sharing */
  corm_assoc(hd + 1, hd, NULL, NULL);

  /* If data was loaded before mirror creation, populate the mirror now */
  if (filename && head->n > 0) {
    uint32_t cur = corm_iter(hd, NULL, 0);
    const void *key, *value;
    while (corm_next(&key, &value, cur)) {
      _corm_put(mirror_hd, value, key, corms[hd].map[corm_id(hd, key)]);
    }
    corm_fin(cur);
  }

  return hd;
}

  uint32_t /* API */
corm_get_vtype(uint32_t hd)
{
  return corm_heads[hd].types[CM_VALUE];
}

  uint32_t /* API */
corm_get_ktype(uint32_t hd)
{
  return corm_heads[hd].types[CM_KEY];
}

  size_t /* API */
corm_type_len(uint32_t type_id)
{
  return corm_types[type_id].len;
}

  static size_t
s_measure(const void *key)
{
  return strlen(key) + 1;
}

static void file_close(corm_file_t *file) {
  if (file->mmaped) {
    CBUG(munmap(file->mmaped, file->size) == -1,
        "munmap failed");
    file->mmaped = 0;
  }

  if (file->fd >= 0)
    close(file->fd);
  file->fd = -1;
}

__attribute__((destructor))
  static void corm_destruct(void) {
    corm_save();

    for (uint32_t i = idm.last; i-- > 0; )
      corm_close(i);

    idm_drop(&cursor_idm);
    idm_drop(&idm);

    uint32_t cur = corm_iter(corm_files_hd, NULL, 0);
    const void *key, *value;

    while (corm_next(&key, &value, cur))
      file_close((corm_file_t *) value);

    corm_close(corm_dbs_hd);
    corm_close(corm_files_hd);
  }

__attribute__((constructor))
  static void
corm_init(void)
{
  corm_type_t *type;

  memset(corms, 0, sizeof(corms));
  memset(corm_heads, 0, sizeof(corm_heads));

  idm = idm_init();
  cursor_idm = idm_init();

  // CM_PTR
  type = &corm_types[corm_reg(sizeof(void *))];

  // CM_HNDL
  type = &corm_types[corm_reg(sizeof(uint32_t))];
  type->hash = corm_nohash;
  type->cmp = corm_ucmp;

  // CM_STR
  type = &corm_types[corm_mreg(s_measure)];
  type->cmp = corm_scmp;

  // CM_U32
  type = &corm_types[corm_reg(sizeof(uint32_t))];
  type->cmp = corm_ucmp;

  uint32_t qm_file = corm_reg(sizeof(corm_file_t));
  corm_files_hd = _corm_open(CM_STR, qm_file,
      CM_DEFAULT_MASK, 0);

  corm_dbs_hd = _corm_open(CM_STR, CM_U32, CM_DEFAULT_MASK, 0);
}

/* }}} */

/* PUT {{{ */

/* This is the low-level put. It doesn't aim to provide
 * MIRROR functionality in itself, just putting in whatever
 * kind of map.
 */
/* Helper: Update IDM last position if needed */
  static inline void
update_idm_last(corm_t *corm, uint32_t pn)
{
  if (pn >= corm->idm.last)
    corm->idm.last = pn + 1;
}

/* ── Inverse index helpers for reference fields ──────────────────────── */

static void ensure_inv_hd(corm_head_t *head, int fi)
{
  if (head->inv_hds[fi] == 0)
    head->inv_hds[fi] = corm_open(NULL, NULL, CM_U32, CM_STR, 0xFF, 0);
}

static void ensure_vstr_hd(corm_head_t *head)
{
  if (head->vstr_hd == 0)
    head->vstr_hd = corm_open(NULL, NULL, CM_STR, CM_STR, 0xFF, 0);
}

static void inverse_add(corm_head_t *head, int fi,
    uint32_t target_pos, uint32_t source_pos)
{
  ensure_inv_hd(head, fi);
  uint32_t inv_hd = head->inv_hds[fi];
  const char *existing = corm_get(inv_hd, &target_pos);

  if (existing) {
    /* Check if source_pos already present */
    const char *p = existing;
    while (*p) {
      char *end;
      unsigned long v = strtoul(p, &end, 10);
      if (end > p && (uint32_t)v == source_pos)
        return;
      if (*end == '\n')
        p = end + 1;
      else
        break;
    }
    /* Append */
    char buf[4096];
    snprintf(buf, sizeof(buf), "%s%u\n", existing, source_pos);
    corm_put(inv_hd, &target_pos, buf);
  } else {
    char buf[32];
    snprintf(buf, sizeof(buf), "%u\n", source_pos);
    corm_put(inv_hd, &target_pos, buf);
  }
}

static void inverse_remove(corm_head_t *head, int fi,
    uint32_t target_pos, uint32_t source_pos)
{
  if (head->inv_hds[fi] == 0)
    return;
  uint32_t inv_hd = head->inv_hds[fi];
  const char *existing = corm_get(inv_hd, &target_pos);
  if (!existing)
    return;

  /* Rebuild string without source_pos */
  char buf[4096];
  size_t pos = 0;
  const char *p = existing;
  while (*p) {
    char *end;
    unsigned long v = strtoul(p, &end, 10);
    if (end > p && (uint32_t)v != source_pos)
      pos += snprintf(buf + pos, sizeof(buf) - pos,
                      pos > 0 ? "\n%lu" : "%lu", v);
    if (*end == '\n')
      p = end + 1;
    else
      break;
  }

  if (pos == 0)
    corm_del(inv_hd, &target_pos);
  else
    corm_put(inv_hd, &target_pos, buf);
}

static void clean_inverses_for_pos(corm_head_t *head, uint32_t pos)
{
  corm_record_t *rec = &corm_records[head->record_id];
  const void *struct_ptr = corm_val(head->phd, pos);
  if (!struct_ptr)
    return;

  for (size_t fi = 0; fi < rec->field_count; fi++) {
    if (rec->fields[fi].target_record == 0)
      continue;
    uint32_t ft = rec->fields[fi].type;
    size_t  fo = rec->fields[fi].offset;
    size_t  fm = rec->fields[fi].max_size;

    if (ft == CM_REFERENCE) {
      const char *id = (const char *)struct_ptr + fo;
      if (id && id[0]) {
        uint32_t target_hd = rec->fields[fi].target_hd;
        if (target_hd > 0) {
          uint32_t tp = corm_pos(target_hd, id);
          if (tp != UINT32_MAX)
            inverse_remove(head, (int)fi, tp, pos);
        }
      }
    } else if (ft == CM_MULTI_REFERENCE && fm > 0) {
      const char *s = (const char *)struct_ptr + fo;
      while (*s) {
        char *end;
        unsigned long p = strtoul(s, &end, 10);
        if (end > s)
          inverse_remove(head, (int)fi, (uint32_t)p, pos);
        if (*end == '\n')
          s = end + 1;
        else
          break;
      }
    }
  }
}

/* Parse \n-separated positions from a string into pos[].
 * Returns number parsed. */
static size_t parse_positions(const char *s, uint32_t *pos, size_t max)
{
  size_t count = 0;
  if (!s) return 0;
  while (*s && count < max) {
    char *end;
    unsigned long v = strtoul(s, &end, 10);
    if (end > s)
      pos[count++] = (uint32_t)v;
    if (*end == '\n')
      s = end + 1;
    else
      break;
  }
  return count;
}

/* Handle inverse index update after a field put on a reference field.
 * Called AFTER the struct has been re-put, with source_pos known. */
static void handle_inverse_put(corm_head_t *head, int fi,
    uint32_t source_pos,
    const uint8_t *old_val, const void *new_val,
    uint32_t ft, size_t fm)
{
  corm_record_t *rec = &corm_records[head->record_id];
  (void)fm;
  if (rec->fields[fi].target_record == 0)
    return;

  uint32_t old_pos[2048], new_pos[2048];
  size_t n_old = 0, n_new = 0;

  if (ft == CM_REFERENCE) {
    uint32_t target_hd = rec->fields[fi].target_hd;
    const char *id = (const char *)new_val;
    if (id && id[0] && target_hd > 0) {
      uint32_t p = corm_pos(target_hd, id);
      if (p != UINT32_MAX) new_pos[n_new++] = p;
    }
    id = (const char *)old_val;
    if (id && id[0] && target_hd > 0) {
      uint32_t p = corm_pos(target_hd, id);
      if (p != UINT32_MAX) old_pos[n_old++] = p;
    }
  } else if (ft == CM_MULTI_REFERENCE) {
    n_new = parse_positions((const char *)new_val, new_pos, 2048);
    n_old = parse_positions((const char *)old_val, old_pos, 2048);
  }

  /* removed = old \ new */
  for (size_t i = 0; i < n_old; i++) {
    int found = 0;
    for (size_t j = 0; j < n_new; j++) {
      if (old_pos[i] == new_pos[j]) { found = 1; break; }
    }
    if (!found)
      inverse_remove(head, fi, old_pos[i], source_pos);
  }

  /* added = new \ old */
  for (size_t i = 0; i < n_new; i++) {
    int found = 0;
    for (size_t j = 0; j < n_old; j++) {
      if (new_pos[i] == old_pos[j]) { found = 1; break; }
    }
    if (!found)
      inverse_add(head, fi, new_pos[i], source_pos);
  }
}

/* }}} */

  static inline uint32_t
_corm_put(uint32_t hd, const void * key,
    const void *value, uint32_t pn)
{
  corm_head_t *head = &corm_heads[hd];
  corm_t *corm = &corms[hd];

  /* Grow before clustering gets pathological */
  if (!(head->flags & CM_NOGROW)) {
    if ((head->n + 1) * 4 >= head->m * 3)
      corm_grow(hd);
  }

  uint32_t n;
  const void *aval = value;
  void *rval, *rkey;
  size_t key_len, klen;
  uint32_t key_hash;
  uint32_t lookup_id;
  uint32_t key_id;
  uint32_t old_n = CM_MISS;

  if (key) {
    lookup_id = corm_id_ex(hd, key, &key_len, &key_hash);
    if (lookup_id == CM_MISS) {
      WARN("corm %u: probe failure after grow", hd);
      return CM_MISS;
    }
    old_n = corm->map[lookup_id];

    if (old_n == CM_MISS) {
      if (pn != CM_MISS) {
        n = pn;
        /* Update IDM to know about this position */
        update_idm_last(corm, pn);
      } else {
        n = idm_new(&corm->idm);
      }
      head->n ++;
    } else if (head->flags & CM_MULTIVALUE) {
      /* CM_MULTIVALUE: Allow duplicate keys.
       * If pn is provided (from corm_assoc), use it.
       * Otherwise, allocate a new position.
       * Don't update hash table - it keeps pointing to first occurrence.
       * Duplicate is accessible via sorted_idx iteration. */
      if (pn != CM_MISS && pn != old_n) {
        n = pn;
        /* Update IDM to know about this position */
        update_idm_last(corm, pn);
      } else if (pn == CM_MISS) {
        n = idm_new(&corm->idm);
      } else {
        n = old_n;  /* pn == old_n, update in place */
      }

      if (n != old_n)
        head->n ++;
    } else
      n = old_n;
  } else {
    key_id = n = idm_new(&corm->idm);
    head->n ++;
    key = &key_id;
    if (head->types[CM_KEY] == CM_STR) {
      static char _auto_key[32];
      snprintf(_auto_key, sizeof(_auto_key), "%u", key_id);
      key = _auto_key;
    }
    lookup_id = corm_id_ex(hd, key, &key_len, &key_hash);
  }

  if (n >= head->m) {
    if (head->flags & CM_NOGROW) {
      head->n--;
      WARN("corm %u: capacity reached (%u entries, max %u)",
          hd, head->n, head->m);
      return CM_MISS;
    }
    corm_grow(hd);
    lookup_id = corm_id_hash(hd, key, key_len, key_hash);
  }
  DEBUG(2, "%u %u %u %p\n", hd, n, lookup_id, key);

  rkey = (void *) key;
  corm->key_hashes[n] = key_hash;
  corm->key_sizes[n] = key_len;

  if (head->phd == hd) {
    if (head->types[CM_VALUE] == CM_PTR)
      value = &value;

    klen = corm_len(head->types[CM_VALUE], aval);

    if (corm->map[lookup_id] == n) {
      const void *old_key = corm_key(hd, n);
      size_t off = corm_payload_off(key_len);
      size_t need = corm_payload_off(key_len) + klen;

      /* Reuse key allocation if key/value fit in the existing block. */
      if (corm->key_sizes[n] == key_len &&
          memcmp(old_key, key, key_len) == 0 &&
          corm_payload_cap(old_key) >= need) {
        rkey = (void *) old_key;
        rval = (void *) ((char *) rkey + off);
      } else {
        corm_payload_free(corm, (void *) old_key);
        rkey = corm_payload_alloc(corm, key_len, klen);
        rval = (void *) ((char *) rkey + off);
      }

      memcpy(rkey, key, key_len);
      memcpy(rval, value, klen);
      corm->key_sizes[n] = key_len;
      corm->val_sizes[n] = klen;
    } else {
      /* New entry - allocate fresh */
      size_t off = corm_payload_off(key_len);
      rkey = corm_payload_alloc(corm, key_len, klen);
      rval = (void *) ((char *) rkey + off);
      memcpy(rkey, key, key_len);
      memcpy(rval, value, klen);
      corm->key_sizes[n] = key_len;
      corm->val_sizes[n] = klen;
    }

    * VAL_ADDR(corm, n) = rval;
  }

  corm->omap[n] = rkey;

  /* CM_MULTIVALUE duplicate chains: freshly-allocated duplicate positions
   * are linked into the key's chain (head = first occurrence in map).
   * Mirrors/shared-position puts (pn != CM_MISS, n != old_n) and in-place
   * updates (n == old_n) are not chained. */
  if (head->flags & CM_MULTIVALUE) {
    if (pn == CM_MISS) {
      corm->mv_next[n] = CM_MISS;
      if (old_n != CM_MISS)
        corm_mv_link(corm, old_n, n);
    } else if (n != old_n) {
      corm->mv_next[n] = CM_MISS;
    }
  }

  /* For CM_MULTIVALUE duplicates, don't update hash table */
  if (!(head->flags & CM_MULTIVALUE) || corm->map[lookup_id] == CM_MISS || corm->map[lookup_id] == n)
    corm->map[lookup_id] = n;

  /* When sharing a position with the primary on updates, a different
   * secondary key may overwrite the same position. Clear any stale hash
   * entries that still point to this position from the former key, then
   * close the hole. Done after the head slot above is (re)filled so the
   * backshift cannot displace this put's own head entry. The stale slot
   * cannot be located by key probe (the former key is gone), so this stays
   * a scan; it only fires on the shared-position mirror path. */
  if (head->phd != hd && pn != CM_MISS) {
    for (uint32_t i = 0; i < head->m; i++) {
      if (corm->map[i] == n) {
        corm->map[i] = CM_MISS;
        if (i != lookup_id)
          corm_backshift(hd, i);
        else
          corm->map[lookup_id] = n;
        break;
      }
    }
  }

  head->iflags |= CM_SDIRTY;

  return lookup_id;
}

  uint32_t /* API */
corm_put(uint32_t hd, const void * const key,
    const void * const value)
{
  uint32_t ahd, n, id;
  idsi_t *cur;
  const void *rkey, *rval;
  corm_head_t *head = &corm_heads[hd];

  /* ── Field-level put for record-aware maps ────────────────────────── */
  if (head->record_id > 0) {
    const char *k = (const char *)key;
    const char *colon = strchr(k, ':');
    if (colon) {
      size_t sk_len = (size_t)(colon - k);
      char struct_key[256];
      if (sk_len >= sizeof(struct_key))
        return CM_MISS;
      memcpy(struct_key, k, sk_len);
      struct_key[sk_len] = '\0';
      const char *field_name = colon + 1;

      int fi = corm_record_find_field(head->record_id, field_name);
      if (fi < 0)
        return CM_MISS;

      size_t struct_size = corm_records[head->record_id].struct_size;

      /* Get or create the struct entry */
      void *struct_ptr = (void *)corm_get(hd, struct_key);
      if (!struct_ptr) {
        void *tmp = calloc(1, struct_size);
        if (!tmp) return CM_MISS;
        if (corm_put(hd, struct_key, tmp) == CM_MISS) {
          free(tmp);
          return CM_MISS;
        }
        free(tmp);
        struct_ptr = (void *)corm_get(hd, struct_key);
      }

      /* Write the field value */
      uint32_t ft = corm_records[head->record_id].fields[fi].type;
      size_t  fo = corm_records[head->record_id].fields[fi].offset;
      size_t  fm = corm_records[head->record_id].fields[fi].max_size;

      if (ft == CM_VSTR) {
        /* CM_VSTR: store directly under composite key in vstr map,
         * bypassing struct modification entirely. */
        ensure_vstr_hd(head);
        return corm_put(head->vstr_hd, key, value);
      }

      /* Save old field value for inverse diff */
      size_t old_sz = (ft == CM_STR || ft == CM_REFERENCE || ft == CM_MULTI_REFERENCE) ? fm : sizeof(uint32_t);
      uint8_t old_val_stack[8192];
      uint8_t *old_val = NULL;
      if (head->inv_hds) {
        if (old_sz > sizeof(old_val_stack)) {
          old_val = malloc(old_sz);
          if (!old_val) return CM_MISS;
        } else {
          old_val = old_val_stack;
        }
        memcpy(old_val, (char *)struct_ptr + fo, old_sz);
      }

      if (ft == CM_STR && fm > 0) {
        strncpy((char *)struct_ptr + fo, (const char *)value, fm - 1);
        *((char *)struct_ptr + fo + fm - 1) = '\0';
      } else if (ft == CM_REFERENCE && fm > 0) {
        strncpy((char *)struct_ptr + fo, (const char *)value, fm - 1);
        *((char *)struct_ptr + fo + fm - 1) = '\0';
      } else if (ft == CM_MULTI_REFERENCE && fm > 0) {
        strncpy((char *)struct_ptr + fo, (const char *)value, fm - 1);
        *((char *)struct_ptr + fo + fm - 1) = '\0';
      } else {
        size_t val_len = corm_len(ft, value);
        memcpy((char *)struct_ptr + fo, value, val_len);
      }

      /* Re-put the struct */
      uint32_t put_id = corm_put(hd, struct_key, struct_ptr);
      if (put_id == CM_MISS) return CM_MISS;
      uint32_t source_pos = corms[hd].map[put_id];

      /* Auto-maintain inverse index for reference fields */
      if (head->inv_hds) {
        handle_inverse_put(head, fi, source_pos, old_val, value, ft, fm);
        if (old_val && old_val != old_val_stack) free(old_val);
      }

      return put_id;
    }
  }

  /* ── Whole-struct put: snapshot old struct for inverse diff ── */
  uint8_t *old_snap = NULL;
  if (head->record_id > 0 && head->inv_hds) {
    size_t ss = corm_records[head->record_id].struct_size;
    old_snap = malloc(ss);
    if (old_snap) {
      const void *old_val = corm_get(hd, key);
      if (old_val)
        memcpy(old_snap, old_val, ss);
      else
        memset(old_snap, 0, ss);  /* new entry, diff against zeros */
    }
  }

  id = _corm_put(hd, key, value, CM_MISS);
  if (id == CM_MISS) {
    free(old_snap);
    return CM_MISS;
  }
  n = corms[hd].map[id];

  cur = ids_iter(&corms[hd].linked);
  rkey = corm_key(hd, n);
  rval = corm_val(hd, n);

  while (ids_next(&ahd, &cur)) {
    corm_t *acorm;
    corm_head_t *ahead;

    acorm = &corms[ahd];
    ahead = &corm_heads[ahd];

    if (acorm->m_assoc) {
      /* Multi-key association: produce multiple secondary keys.
       * Secondary is a root map storing (ref_value, primary_key). */
      const void *skeys[64];
      size_t nkeys = acorm->m_assoc(skeys, 64, rkey, rval, acorm->m_assoc_userdata);
      for (size_t i = 0; i < nkeys; i++) {
        _corm_put(ahd, skeys[i], rkey, CM_MISS);
        free((void *)skeys[i]);
      }
    } else if (acorm->assoc) {
      const void *skey;

      acorm->assoc(&skey, rkey, rval, acorm->assoc_userdata);

      /* Share positions with CM_MIRROR and non-MULTIVALUE linked maps.
       * MULTIVALUE linked maps keep independent positions to avoid
       * hash table repointing complexity on duplicate removal. */
      if (ahead->iflags & CM_IS_MIRROR) {
        _corm_put(ahd, skey, rval, n);  /* Mirror: share position */
      } else if (ahead->flags & CM_MULTIVALUE) {
        _corm_put(ahd, skey, rval, CM_MISS);  /* MULTIVALUE: independent */
      } else {
        _corm_put(ahd, skey, rval, n);  /* General: share position */
      }
    }
  }

  /* ── Update inverse index for reference fields after whole-struct put ── */
  if (old_snap) {
    corm_record_t *rec = &corm_records[head->record_id];
    for (size_t fi = 0; fi < rec->field_count; fi++) {
      uint32_t ft = rec->fields[fi].type;
      if ((ft == CM_REFERENCE || ft == CM_MULTI_REFERENCE)
          && rec->fields[fi].target_record > 0) {
        handle_inverse_put(head, (int)fi, n,
            old_snap + rec->fields[fi].offset,
            (const uint8_t *)rval + rec->fields[fi].offset,
            ft, rec->fields[fi].max_size);
      }
    }
    free(old_snap);
  }

  return id;
}

/* }}} */

/* GET {{{ */

static int corm_lnext(uint32_t *sn, uint32_t cur_id);

  const void * /* API */
corm_get(uint32_t hd, const void * const key)
{
  corm_head_t *head = &corm_heads[hd];

  /* ── Composite-key resolution for record-aware maps ──────────────── */
  if (head->record_id > 0) {
    const char *k = (const char *)key;
    const char *colon = strchr(k, ':');
    if (colon) {
      size_t sk_len = (size_t)(colon - k);
      char struct_key[256];
      if (sk_len >= sizeof(struct_key))
        return NULL;
      memcpy(struct_key, k, sk_len);
      struct_key[sk_len] = '\0';

      int fi = corm_record_find_field(head->record_id, colon + 1);
      if (fi < 0)
        return NULL;

      uint32_t ft = corm_records[head->record_id].fields[fi].type;

      if (ft == CM_VSTR) {
        /* CM_VSTR: look up the composite key directly in vstr map */
        if (head->vstr_hd == 0)
          return NULL;
        return corm_get(head->vstr_hd, k);
      }

      const void *struct_ptr = corm_get(hd, struct_key);
      if (!struct_ptr)
        return NULL;

      size_t field_offset = corm_records[head->record_id].fields[fi].offset;
      const char *result = (const char *)struct_ptr + field_offset;
      return result;
    }
  }

  uint32_t id = corm_id(hd, key);
  if (id == CM_MISS)
    return NULL;

  uint32_t n = corms[hd].map[id];
  if (n == CM_MISS)
    return NULL;

  return corm_val(hd, n);
}

/* }}} */

/* DELETE {{{ */

  static inline uint32_t
corm_root(uint32_t hd)
{
  while(corm_heads[hd].phd != hd)
    hd = corm_heads[hd].phd;

  return hd;
}

static void corm_ndel(uint32_t hd, uint32_t n);

static void corm_ndel_topdown(uint32_t hd, uint32_t n) {
  corm_head_t *head = &corm_heads[hd];
  corm_t *corm = &corms[hd];
  const void *key;
  uint32_t id, ahd;
  idsi_t *cur;

  // Guard against already-closed maps (omap is NULL after close)
  if (!corm->omap)
    return;

  if (n >= head->m)
    return;

  key = corm_key(hd, n);

  cur = ids_iter(&corm->linked);

  while (ids_next(&ahd, &cur)) {
    if (corms[ahd].m_assoc && key) {
      /* Multi-assoc: secondary is a root map storing (ref_val, pkey).
       * Iterate to find and delete entries whose value matches the
       * primary key being deleted. */
      uint32_t mcur = corm_iter(ahd, NULL, 0);
      uint32_t msn;
      uint32_t to_del[256];
      size_t ndel = 0;

      while (corm_lnext(&msn, mcur)) {
        const void *mval = corm_val(ahd, msn);
        if (mval && corm_scmp(mval, key, 0) == 0) {
          to_del[ndel++] = msn;
          if (ndel >= 256) break;
        }
      }
      corm_fin(mcur);

      for (size_t i = 0; i < ndel; i++)
        corm_ndel(ahd, to_del[i]);
    } else {
      corm_ndel_topdown(ahd, n);
    }
  }

  if (!key) {
    corm->omap[n] = NULL;
    idm_del(&corm->idm, n);
    head->n --;
    return;
  }

  /* For CM_MULTIVALUE maps, unlink position n from the key's duplicate
   * chain (promoting the next duplicate as the slot head, or clearing the
   * slot when n was the only member). Chain mutation is O(k) and avoids
   * the sorted-index rebuild of the old bsearch-based path. */
  uint32_t new_map_entry = CM_MISS;
  if (head->flags & CM_MULTIVALUE) {
    size_t dklen;
    uint32_t dkhash;
    (void) corm_id_ex(hd, key, &dklen, &dkhash);
    id = corm_id_hash(hd, key, dklen, dkhash);
    if (id != CM_MISS) {
      uint32_t head_pos = corm->map[id];
      if (head_pos != CM_MISS)
        new_map_entry = corm_mv_unlink(corm, head_pos, n);
    }
  } else {
    id = corm_id(hd, key);
  }

  if (head->phd == hd) {
    corm_payload_free(corm, (void *) key);
    * VAL_ADDR(corm, n) = NULL;
  }

  corm->key_hashes[n] = 0;
  corm->key_sizes[n] = 0;
  corm->val_sizes[n] = 0;

  head->iflags |= CM_SDIRTY;

  /* Update hash table entry */
  if (id != CM_MISS) {
    if (new_map_entry != CM_MISS)
      corm->map[id] = new_map_entry;
    else {
      corm->map[id] = CM_MISS;
      corm_backshift(hd, id);
    }
  }

  corm->omap[n] = NULL;
  idm_del(&corm->idm, n);
  head->n --;

}

/* Delete based on position */
static inline void
corm_ndel(uint32_t hd, uint32_t n) {
  corm_ndel_topdown(corm_root(hd), n);
}

  static void
corm_clear_fast(uint32_t hd)
{
  corm_head_t *head = &corm_heads[hd];
  corm_t *corm = &corms[hd];

  if (head->phd == hd) {
    for (uint32_t n = 0; n < corm->idm.last; n++) {
      const void *key = corm->omap[n];
      if (!key)
        continue;
      corm_payload_free(corm, (void *) key);
    }
  }

  memset(corm->map, 0xFF, sizeof(uint32_t) * head->m);
  memset(corm->omap, 0, sizeof(void *) * head->m);
  memset(corm->key_hashes, 0, sizeof(uint32_t) * head->m);
  memset(corm->mv_next, 0xFF, sizeof(uint32_t) * head->m);
  memset(corm->key_sizes, 0, sizeof(size_t) * head->m);
  if (head->phd == hd) {
    memset(corm->table, 0, sizeof(void *) * head->m);
    memset(corm->val_sizes, 0, sizeof(size_t) * head->m);
  }

  idm_drop(&corm->idm);
  corm->idm.last = 0;
  head->n = 0;
  head->iflags |= CM_SDIRTY;
}

  void /* API */
corm_del(uint32_t hd, const void * const key)
{
  corm_head_t *head = &corm_heads[hd];

  /* ── Field-level delete for record-aware maps ─────────────────────── */
  if (head->record_id > 0) {
    const char *k = (const char *)key;
    const char *colon = strchr(k, ':');
    if (colon) {
      size_t sk_len = (size_t)(colon - k);
      char struct_key[256];
      if (sk_len >= sizeof(struct_key))
        return;
      memcpy(struct_key, k, sk_len);
      struct_key[sk_len] = '\0';

      int fi = corm_record_find_field(head->record_id, colon + 1);
      if (fi < 0)
        return;

      void *struct_ptr = (void *)corm_get(hd, struct_key);
      if (!struct_ptr)
        return;

      uint32_t ft = corm_records[head->record_id].fields[fi].type;
      size_t   fo = corm_records[head->record_id].fields[fi].offset;
      size_t   fm = corm_records[head->record_id].fields[fi].max_size;

      if (ft == CM_VSTR) {
        /* CM_VSTR: delete the composite key entry from the vstr map */
        if (head->vstr_hd) {
          corm_del(head->vstr_hd, k);
        }
        return;
      }

      size_t field_size = fm;
      if (field_size == 0)
        field_size = corm_len(ft, NULL);

      /* Snapshot old value for inverse cleanup */
      uint8_t old_val[8192];
      size_t old_sz = (ft == CM_STR || ft == CM_REFERENCE || ft == CM_MULTI_REFERENCE) ? fm : sizeof(uint32_t);
      if (old_sz > sizeof(old_val)) return;
      memcpy(old_val, (char *)struct_ptr + fo, old_sz);

      memset((char *)struct_ptr + fo, 0, field_size);

      /* Re-put the struct */
      uint32_t put_id = corm_put(hd, struct_key, struct_ptr);
      if (put_id == CM_MISS) return;
      uint32_t source_pos = corms[hd].map[put_id];

      /* Clean inverse: old references removed (new value is zeroed) */
      if (head->inv_hds && (ft == CM_REFERENCE || ft == CM_MULTI_REFERENCE)) {
        handle_inverse_put(head, fi, source_pos, old_val,
                           (const void*)"",
                           ft, fm);
      }
      return;
    }
  }

  uint32_t cur, sn;

  if (head->flags & CM_MULTIVALUE) {
    cur = corm_get_multi(hd, key);
    if (cur != CM_MISS) {
      if (corm_lnext(&sn, cur)) {
        if (head->record_id > 0 && head->inv_hds)
          clean_inverses_for_pos(head, sn);
        corm_ndel(hd, sn);
      }
      corm_fin(cur);
    }
  } else {
    cur = corm_iter(hd, key, 0);
    while (corm_lnext(&sn, cur)) {
      if (head->record_id > 0 && head->inv_hds)
        clean_inverses_for_pos(head, sn);
      corm_ndel(hd, sn);
    }
    corm_fin(cur);
  }
}

  void
corm_del_all(uint32_t hd, const void * const key)
{
  corm_head_t *head = &corm_heads[hd];
  corm_t *corm = &corms[hd];

  if (head->flags & CM_MULTIVALUE) {
    /* Fast path: if nothing is linked to this map, bulk-delete by
     * clearing the matching slots once and rebuilding the hash table.
     * This avoids repeated probe-chain maintenance for every duplicate. */
    uint32_t cur = corm_get_multi(hd, key);
    uint32_t sn;
    size_t n = 0, cap = head->n ? head->n : 1;
    uint32_t *positions = malloc(sizeof(*positions) * cap);
    int fast_path = ids_iter(&corm->linked) == NULL && head->phd == hd;

    if (cur == CM_MISS) {
      free(positions);
      return;
    }

    CBUG(!positions, "malloc error (del_all)\n");

    while (corm_lnext(&sn, cur))
      positions[n++] = sn;

    if (fast_path) {
      for (size_t i = 0; i < n; i++) {
        uint32_t pos = positions[i];
        const void *old_key = corm_key(hd, pos);

        corm_payload_free(corm, (void *) old_key);
        corm->key_sizes[pos] = 0;
        corm->val_sizes[pos] = 0;
        corm->omap[pos] = NULL;
        * VAL_ADDR(corm, pos) = NULL;
        idm_del(&corm->idm, pos);
        head->n--;
      }

      head->iflags |= CM_SDIRTY;

      if (head->n == 0)
        memset(corm->map, 0xFF, sizeof(uint32_t) * head->m);
      else
        corm_rebuild_map(hd);
    } else {
      for (size_t i = 0; i < n; i++)
        corm_ndel(hd, positions[i]);
    }

    free(positions);
  } else {
    /* For regular maps, just call corm_del once */
    corm_del(hd, key);
  }
}

/* }}} */

/* ITERATION {{{ */

  void /* API */
corm_fin(uint32_t cur_id)
{
  corm_cur_t *cursor = &corm_cursors[cur_id];

  if (cursor->sub_cur)
    corm_fin(cursor->sub_cur);

  idm_del(&cursor_idm, cur_id);
}

  uint32_t /* API */
corm_iter(uint32_t hd, const void * const key, uint32_t flags)
{
  corm_head_t *head = &corm_heads[hd];
  corm_t *corm = &corms[hd];
  uint32_t cur_id = idm_new(&cursor_idm);
  corm_cur_t *cursor = &corm_cursors[cur_id];

  if (key && (flags & CM_RANGE_GE)) {
    /* Lower-bound range: iterate every entry whose key >= the starting
     * key (all duplicates included, ascending when CM_SORTED). This is
     * the documented CM_RANGE+CM_SORTED behavior; it must also work for
     * CM_MULTIVALUE maps, which plain CM_RANGE narrows to the single
     * exact starting key. */
    if (head->flags & CM_SORTED) {
      /* Lower bound must be the FIRST occurrence of the starting key when
       * it is present (corm_bsearch_ANY could land on a middle duplicate
       * of it), otherwise the insertion point — the first key above it. */
      int exact;
      int first = corm_bsearch_ex(hd, key, &exact, CORM_BSEARCH_FIRST);
      cursor->pos = (exact)
        ? (uint32_t) first
        : (uint32_t) corm_bsearch(hd, key, NULL);
      cursor->end_pos = head->sorted_n;
    } else {
      /* Unsorted: fall back to the linear CM_RANGE scan below, which
       * applies the same "key >= starting key" filter. */
      cursor->pos = cursor->end_pos = 0;
    }
    flags |= CM_RANGE;
  } else if (key && (head->flags & CM_MULTIVALUE)) {
    /* For CM_MULTIVALUE maps, use sorted iteration to find all duplicates.
     * Find first occurrence of this key */
    int first = corm_bsearch_ex(hd, key, NULL, CORM_BSEARCH_FIRST);
    cursor->pos = (first != -1) ? (uint32_t)first : head->sorted_n;
    cursor->end_pos = CM_MISS;
    /* Use sorted iteration but stop at key boundary */
    flags |= CM_RANGE;
  } else if (key && (flags & CM_RANGE) && (head->flags & CM_SORTED)) {
    int exact;
    cursor->pos = corm_bsearch(hd, key, &exact);
    cursor->end_pos = head->sorted_n;
  } else if (key && !(flags & CM_RANGE)) {
    uint32_t id = corm_id(hd, key);
    if (id == CM_MISS) {
      cursor->pos = CM_MISS;
      cursor->end_pos = CM_MISS;
    } else {
      cursor->pos = corm->map[id];
      cursor->end_pos = cursor->pos;
    }
  } else
    cursor->pos = cursor->end_pos = 0;

  cursor->ipos = cursor->pos;
  cursor->sub_cur = 0;
  cursor->hd = hd;
  cursor->key = key;
  cursor->key_len = key ? corm_len(head->types[CM_KEY], key) : 0;
  cursor->flags = flags;
  return cur_id;
}

/* low-level next */
  static int
corm_lnext(uint32_t *sn, uint32_t cur_id)
{
  register corm_cur_t *cursor
    = &corm_cursors[cur_id];
  register corm_head_t *head = &corm_heads[cursor->hd];
  register corm_t *corm = &corms[cursor->hd];
  uint32_t n;
  const void *key;

  if (cursor->flags & CM_MVCHAIN) {
    /* Duplicate-chain walk (corm_get_multi): yield the current position
     * and advance to its next-duplicate link. */
    if (cursor->pos >= corm->idm.last)
      goto end;
    n = cursor->pos;
    cursor->pos = corm->mv_next[n];
    *sn = n;
    return 1;
  }

  if ((cursor->flags & CM_RANGE)
      && (head->flags & CM_SORTED))
  {
    if (head->iflags & CM_SDIRTY)
      corm_rebuild_sorted(cursor->hd);

    if (cursor->pos >= head->sorted_n)
      goto end;

    n = corm->sorted_idx[cursor->pos];

    if (cursor->key && (head->flags & CM_MULTIVALUE)
        && !(cursor->flags & CM_RANGE_GE)) {
      if (cursor->end_pos == CM_MISS)
        cursor->end_pos = (uint32_t) corm_bsearch_ex(
            cursor->hd, cursor->key, NULL,
            CORM_BSEARCH_LAST);
      if (cursor->pos > cursor->end_pos)
        goto end;
    }

    *sn = n;
    cursor->pos++;
    return 1;
  }

cagain:
  n = cursor->pos;

  if (n >= corm->idm.last)
    goto end;

  key = corm_key(cursor->hd, n);
  if (key == NULL) {
    cursor->pos++;
    goto cagain;
  }

  if (cursor->flags & CM_RANGE) {
    if (!cursor->key)
      goto next;

    corm_type_t *type = &corm_types[head->types[CM_KEY]];
    size_t len;

    if (type->measure) {
      size_t key_len = type->measure(key);
      size_t start_key_len = cursor->key_len;
      len = (key_len > start_key_len)
        ? key_len
        : start_key_len;
    } else
      len = type->len;

    if (type->cmp(key, cursor->key, len) < 0) {
      cursor->pos++;
      goto cagain;
    }
  } else if (cursor->key && n != cursor->ipos)
    goto end;
next:

  DEBUG(3, "NEXT! cur_id %u key %p\n",
      cur_id, key);

  cursor->pos++;
  *sn = n;
  return 1;
end:
  idm_del(&cursor_idm, cur_id);
  *sn = CM_MISS;
  return 0;
}

  int /* API */
corm_next(const void ** ckey, const void ** cval,
    uint32_t cur_id)
{
  register corm_cur_t *c;
  uint32_t sn;
  int ret = corm_lnext(&sn, cur_id);

  if (!ret)
    return 0;

  c = &corm_cursors[cur_id];
  if (ckey)
    *ckey = corm_key(c->hd, sn);
  if (cval)
    *cval = corm_val(c->hd, sn);
  return 1;
}

/* }}} */

/* DROP + CLOSE + OTHERS {{{ */

  void /* API */
corm_drop(uint32_t hd)
{
  corm_t *corm = &corms[hd];

  if (ids_iter(&corm->linked) == NULL) {
    corm_clear_fast(hd);
    return;
  }

  uint32_t cur_id = corm_iter(hd, NULL, 0), sn;

  while (corm_lnext(&sn, cur_id))
    corm_ndel(hd, sn);
}

  void /* API */
corm_close(uint32_t hd)
{
  corm_head_t *head = &corm_heads[hd];
  corm_t *corm = &corms[hd];
  idsi_t *cur;
  uint32_t ahd;

  if (!corm->omap)
    return;

  corm_drop(hd);

  cur = ids_iter(&corm->linked);
  while (ids_next(&ahd, &cur))
    corm_close(ahd);

  ids_drop(&corm->linked);

  /* Close inverse index maps */
  if (head->inv_hds) {
    uint32_t fc = head->record_id > 0 && head->record_id < corm_records_n
                ? (uint32_t)corm_records[head->record_id].field_count : 0;
    for (uint32_t i = 0; i < fc; i++) {
      if (head->inv_hds[i])
        corm_close(head->inv_hds[i]);
    }
    free(head->inv_hds);
    head->inv_hds = NULL;
  }

  /* Close variable-length string map */
  if (head->vstr_hd) {
    corm_close(head->vstr_hd);
    head->vstr_hd = 0;
  }

  idm_drop(&corm->idm);
  corm->idm.last = 0;
  corm_payload_flush(corm);
  free(corm->map);
  free(corm->omap);
  free(corm->key_hashes);
  free(corm->mv_next);
  free(corm->key_sizes);
  free(corm->val_sizes);
  if (corm->sorted_idx)
    free(corm->sorted_idx);
  if (corm_heads[hd].phd == hd)
    free(corm->table);
  corm->omap = NULL;
  idm_del(&idm, hd);

  // remove any file associations so we don't try
  // saving it to a file after it is closed.
  if (!head->file)
    return;
  const corm_file_t *file = corm_get(corm_files_hd, head->file);
  if (!file)
    return;
  ids_remove((ids_t *) &file->ids, hd);

}

  void /* API */
corm_assoc(uint32_t hd, uint32_t link, corm_assoc_t cb, void *userdata)
{
  corm_t *corm = &corms[hd];

  if (!cb)
    cb = corm_rassoc;

  ids_push(&corms[link].linked, hd);

  corm->assoc = cb;
  corm->assoc_userdata = userdata;
  corm_heads[hd].phd = link;

  free(corm->table);
  corm->table = NULL;

  if (corm_heads[link].n > 0) {
    uint32_t cur = corm_iter(link, NULL, 0);
    const void *key, *value;

    while (corm_next(&key, &value, cur)) {
      const void *skey;
      corm->assoc(&skey, key, value, corm->assoc_userdata);
      _corm_put(hd, skey, value, CM_MISS);
    }
    corm_fin(cur);
  }
}

  void /* API */
corm_assoc_multi(uint32_t hd, uint32_t link, corm_assoc_multi_t cb, void *userdata)
{
  corm_t *corm = &corms[hd];

  if (!cb)
    return;

  ids_push(&corms[link].linked, hd);

  corm->m_assoc = cb;
  corm->m_assoc_userdata = userdata;
  corm_heads[hd].phd = link;

  /* Backfill existing entries in the primary */
  if (corm_heads[link].n > 0) {
    uint32_t cur = corm_iter(link, NULL, 0);
    const void *key, *value;

    while (corm_next(&key, &value, cur)) {
      const void *skeys[64];
      size_t nkeys = corm->m_assoc(skeys, 64, key, value, corm->m_assoc_userdata);
      for (size_t i = 0; i < nkeys; i++) {
        _corm_put(hd, skeys[i], key, CM_MISS);
        free((void *)skeys[i]);
      }
    }
    corm_fin(cur);
  }
}

  uint32_t /* API */
corm_reg(size_t len)
{
  if (types_n > TYPES_MASK) {
    fprintf(stderr, "corm_reg: type limit reached\n");
    return CM_MISS;
  }
  uint32_t id = types_n ++;
  corm_type_t *type = &corm_types[id];

  memset(type, 0, sizeof(corm_type_t));
  type->len = len;
  type->hash = corm_chash;
  type->cmp = corm_ccmp;
  return id;
}

  void
corm_cmp_set(uint32_t ref, corm_cmp_t *cmp)
{
  corm_type_t *type = &corm_types[ref];
  type->cmp = cmp;
}

  uint32_t /* API */
corm_mreg(corm_measure_t *measure)
{
  if (types_n > TYPES_MASK) {
    fprintf(stderr, "corm_mreg: type limit reached\n");
    return CM_MISS;
  }
  uint32_t id = types_n ++;
  corm_type_t *type = &corm_types[id];

  memset(type, 0, sizeof(corm_type_t));
  type->measure = measure;
  type->hash = corm_chash;
  type->cmp = corm_ccmp;
  type->len = 0;
  return id;
}

  size_t /* API */
corm_len(uint32_t type_id, const void *key)
{
  corm_type_t *type = &corm_types[type_id];

  return type->measure
    ? type->measure(key)
    : type->len;
}

/* }}} */

/* RECORD-AWARE MAP SUPPORT {{{ */

  uint32_t /* API */
corm_record_register(const char *name, size_t struct_size,
    const corm_record_field_t *fields, size_t field_count)
{
  if (corm_records_n >= CORM_MAX_RECORDS) {
    fprintf(stderr, "corm_record_register: record limit reached\n");
    return CM_MISS;
  }
  if (struct_size == 0) {
    fprintf(stderr, "corm_record_register: struct_size must be > 0\n");
    return CM_MISS;
  }
  if (!fields || field_count == 0 || field_count > CORM_MAX_RECORD_FIELDS) {
    fprintf(stderr, "corm_record_register: invalid fields\n");
    return CM_MISS;
  }

  uint32_t struct_type_id = corm_reg(struct_size);
  if (struct_type_id == CM_MISS) {
    fprintf(stderr, "corm_record_register: corm_reg failed\n");
    return CM_MISS;
  }

  uint32_t id = ++corm_records_n;
  corm_record_t *rec = &corm_records[id];

  memset(rec, 0, sizeof(*rec));
  strncpy(rec->name, name ? name : "unnamed", sizeof(rec->name) - 1);
  rec->struct_size = struct_size;
  rec->struct_type_id = struct_type_id;
  rec->field_count = field_count;

  for (size_t i = 0; i < field_count; i++) {
    strncpy(rec->fields[i].name, fields[i].name,
            sizeof(rec->fields[i].name) - 1);
    rec->fields[i].type = fields[i].type;
    rec->fields[i].offset = fields[i].offset;
    rec->fields[i].max_size = fields[i].max_size;
    rec->fields[i].target_record = fields[i].target_record;
    rec->fields[i].target_hd = fields[i].target_hd;
    strncpy(rec->fields[i].inverse, fields[i].inverse ? fields[i].inverse : "",
            sizeof(rec->fields[i].inverse) - 1);
  }

  return id;
}

  uint32_t /* API */
corm_record_type_id(uint32_t record_id)
{
  if (!record_id || record_id > corm_records_n)
    return CM_MISS;
  return corm_records[record_id].struct_type_id;
}

  void /* API */
corm_record_field_set_target_hd(uint32_t record_id,
    const char *field_name,
    uint32_t target_hd)
{
  if (!record_id || record_id > corm_records_n) return;
  int fi = corm_record_find_field(record_id, field_name);
  if (fi < 0) return;
  corm_records[record_id].fields[fi].target_hd = target_hd;
}

  uint32_t /* API */
corm_field_put(uint32_t hd, const char *item_id,
    const char *field_name, const char *value)
{
  if (!value) return CM_MISS;
  corm_head_t *head = &corm_heads[hd];
  if (head->record_id == 0) return CM_MISS;
  int fi = corm_record_find_field(head->record_id, field_name);
  if (fi < 0) return CM_MISS;
  uint32_t ft = corm_records[head->record_id].fields[fi].type;

  char key[256];
  size_t ilen = strlen(item_id);
  size_t flen = strlen(field_name);
  if (ilen + 1 + flen >= sizeof(key)) return CM_MISS;
  memcpy(key, item_id, ilen);
  key[ilen] = ':';
  memcpy(key + ilen + 1, field_name, flen);
  key[ilen + 1 + flen] = '\0';

  if (ft == CM_REFERENCE) {
    uint32_t thd = corm_records[head->record_id].fields[fi].target_hd;
    if (thd == 0)
      return CM_MISS;
    if (!value || ((const char *)value)[0] == '\0')
      return CM_MISS;
    uint32_t pos = corm_pos(thd, value);
    if (pos == UINT32_MAX)
      return CM_MISS;
    return corm_put(hd, key, value);
  }

	if (ft == CM_MULTI_REFERENCE) {
	    uint32_t thd = corm_records[head->record_id].fields[fi].target_hd;
	    if (!thd) return corm_put(hd, key, value);

	    static char resolved[65536];
	    size_t off = 0;
	    const char *p = value;
	    while (*p) {
	      const char *nl = strchr(p, '\n');
	      size_t len = nl ? (size_t)(nl - p) : strlen(p);
	      if (len > 0) {
	        char id[256];
	        size_t cplen = len < sizeof(id) - 1 ? len : sizeof(id) - 1;
	        memcpy(id, p, cplen);
	        id[cplen] = '\0';
	        uint32_t pos = corm_pos(thd, id);
	        if (off > 0 && off < sizeof(resolved) - 1)
	          resolved[off++] = '\n';
	        if (pos != UINT32_MAX) {
	          off += (size_t)snprintf(resolved + off, sizeof(resolved) - off,
	                                 "%u", pos);
	        } else {
	          size_t slen = strlen(id);
	          if (slen > sizeof(resolved) - off - 1)
	            slen = sizeof(resolved) - off - 1;
	          memcpy(resolved + off, id, slen);
	          off += slen;
	        }
	      }
	      if (!nl) break;
	      p = nl + 1;
	    }
	    resolved[off < sizeof(resolved) ? off : sizeof(resolved) - 1] = '\0';
	    return corm_put(hd, key, resolved);
	  }

  return corm_put(hd, key, value);
}

  const char * /* API */
corm_field_get(uint32_t hd, const char *item_id,
    const char *field_name)
{
  if (!item_id || !field_name) return NULL;
  corm_head_t *head = &corm_heads[hd];
  if (head->record_id == 0) return NULL;
  int fi = corm_record_find_field(head->record_id, field_name);
  if (fi < 0) return NULL;
  uint32_t ft = corm_records[head->record_id].fields[fi].type;

  if (ft == CM_VSTR) {
    if (head->vstr_hd == 0) return NULL;
    char key[256];
    size_t ilen = strlen(item_id);
    size_t flen = strlen(field_name);
    if (ilen + 1 + flen >= sizeof(key)) return NULL;
    memcpy(key, item_id, ilen);
    key[ilen] = ':';
    memcpy(key + ilen + 1, field_name, flen);
    key[ilen + 1 + flen] = '\0';
    return corm_get(head->vstr_hd, key);
  }

  const void *struct_ptr = corm_get(hd, item_id);
  if (!struct_ptr) return NULL;

  size_t field_offset = corm_records[head->record_id].fields[fi].offset;
  const char *result = (const char *)struct_ptr + field_offset;
  if (ft == CM_REFERENCE && (!result || result[0] == '\0'))
    return NULL;
  return result;
}

  const char * /* API */
corm_get_key(uint32_t hd, uint32_t pos)
{
  corm_t *corm = &corms[hd];
  if (pos >= corm->idm.last)
    return NULL;
  const void *key = corm->omap[pos];
  return key ? (const char *)key : NULL;
}

  uint32_t /* API */
corm_pos(uint32_t hd, const char *key)
{
  if (!key)
    return UINT32_MAX;
  uint32_t id = corm_id(hd, key);
  if (id == CM_MISS)
    return UINT32_MAX;
  uint32_t n = corms[hd].map[id];
  return (n == CM_MISS) ? UINT32_MAX : n;
}

  size_t /* API */
corm_inv_get(uint32_t hd, const char *field_name,
    uint32_t target_pos,
    uint32_t *out, size_t max)
{
  corm_head_t *head = &corm_heads[hd];
  if (!head->inv_hds || head->record_id == 0 || !field_name)
    return 0;

  int fi = corm_record_find_field(head->record_id, field_name);
  if (fi < 0)
    return 0;

  corm_record_t *rec = &corm_records[head->record_id];
  if (rec->fields[fi].target_record == 0)
    return 0;

  uint32_t inv_hd = head->inv_hds[fi];
  if (inv_hd == 0)
    return 0;

  const char *val = corm_get(inv_hd, &target_pos);
  if (!val || !*val)
    return 0;

  size_t count = 0;
  const char *p = val;
  while (*p && count < max) {
    char *end;
    unsigned long v = strtoul(p, &end, 10);
    if (end > p)
      out[count++] = (uint32_t)v;
    if (*end == '\n')
      p = end + 1;
    else
      break;
  }

  return count;
}

/* }}} */

  inline static size_t
_corm_load(uint32_t hd, const char *mmaped, uint32_t dbid)
{
  const char *mm = mmaped;
  const char *mm_start = mmaped;

  uint32_t lid = * (uint32_t *) mm;
  mm += sizeof(uint32_t);

  size_t size = * (size_t *) mm;
  mm += sizeof(size_t);

  if (dbid != CM_MISS && lid != dbid)
    return mm + size - sizeof(uint32_t) - sizeof(size_t) - mm_start;

  uint32_t amount = * (uint32_t*) mm;
  mm += sizeof(uint32_t);

  corm_head_t *head = &corm_heads[hd];
  uint32_t ktype = head->types[CM_KEY];
  uint32_t vtype = head->types[CM_VALUE];

  for (uint32_t i = 0; i < amount; i++) {
    size_t klen = corm_len(ktype, mm);
    const char *mval = mm + klen;
    size_t vlen = corm_len(vtype, mval);

    corm_put(hd, mm, mval);
    mm = mval + vlen;
  }

  head->iflags |= CM_SDIRTY;
  return mm - mm_start;
}

  static void
corm_load_file(char *filename, uint32_t dbid, uint32_t hd)
{
  corm_file_t *file = (corm_file_t *)
    corm_get(corm_files_hd, filename);

  struct stat sb;
  char *mm;

  if (file->mmaped)
    goto skip_open;

  file->fd = open(filename, O_RDONLY);
  if (file->fd < 0)
    return;

  CBUG(fstat(file->fd, &sb) == -1, "fstat");

  file->size = sb.st_size;
  if (file->size == 0) {
    close(file->fd);
    file->fd = -1;
    return;
  }

  file->mmaped = (char*) mmap(NULL, file->size,
      PROT_READ, MAP_SHARED, file->fd, 0);
  if (file->mmaped == MAP_FAILED) {
    file->mmaped = 0;
    close(file->fd);
    file->fd = -1;
    return;
  }

skip_open:
  mm = file->mmaped;
  char *const end = mm + file->size;

  while (mm + sizeof(uint32_t) + sizeof(size_t) <= end) {
    uint32_t lid = * (uint32_t *) mm;
    size_t block = * (size_t *) (mm + sizeof(uint32_t));

    if (block < sizeof(uint32_t) + sizeof(size_t)
        || mm + block > end)
      break;

    if (lid == dbid)
      _corm_load(hd, mm, dbid);

    mm += block;
  }
}

  static size_t
_corm_calc_size(uint32_t hd)
{
  corm_head_t *head = &corm_heads[hd];
  uint32_t ktype = head->types[CM_KEY];
  uint32_t vtype = head->types[CM_VALUE];
  size_t total_size = sizeof(uint32_t) + sizeof(size_t) + sizeof(head->n);
  uint32_t cur = corm_iter(hd, NULL, 0);
  const void *key, *value;

  while (corm_next(&key, &value, cur)) {
    total_size += corm_len(ktype, key);
    total_size += corm_len(vtype, value);
  }

  corm_fin(cur);
  return total_size;
}

  static inline size_t
corm_calc_file_size(const ids_t *hds)
{
  size_t size = 0;
  idsi_t *cur = (idsi_t *) ids_iter((ids_t *) hds);
  uint32_t hd;

  while (ids_next(&hd, &cur))
    if (mdbs[hd])
      size += _corm_calc_size(hd);

  return size;
}

  inline static size_t
_corm_save(void *mmaped, uint32_t hd)
{
  corm_head_t *head = &corm_heads[hd];
  uint32_t ktype = head->types[CM_KEY];
  uint32_t vtype = head->types[CM_VALUE];
  char *mm_start = mmaped;
  char *mm = mmaped;
  uint32_t cur = corm_iter(hd, NULL, 0);
  const void *key, *value;

  memcpy(mm, &head->dbid, sizeof(head->dbid));
  mm += sizeof(head->dbid);

  size_t size = _corm_calc_size(hd);
  memcpy(mm, &size, sizeof(size));
  mm += sizeof(size);

  memcpy(mm, &head->n, sizeof(head->n));
  mm += sizeof(head->n);

  while (corm_next(&key, &value, cur)) {
    size_t klen = corm_len(ktype, key);
    size_t vlen = corm_len(vtype, value);

    memcpy(mm, key, klen);
    mm += klen;
    memcpy(mm, value, vlen);
    mm += vlen;
  }

  corm_fin(cur);
  return mm - mm_start;
}

  static inline void
corm_save_file(char *filename)
{
  corm_file_t *file = (corm_file_t *) corm_get(corm_files_hd, filename);
  CBUG(!file, "called with unknown filename");

  if (file->mmaped)
    file_close(file);

  file->size = corm_calc_file_size(&file->ids);

  if (file->size == 0)
    return;

  file->fd = open(filename, O_RDWR | O_CREAT,
      S_IRUSR | S_IWUSR);

  CBUG(file->fd == -1, "open for save failed");

  CBUG(ftruncate(file->fd, (off_t) file->size) == -1,
      "ftruncate failed");

  file->mmaped = (char*) mmap(NULL, file->size,
      PROT_WRITE, MAP_SHARED, file->fd, 0);
  CBUG(file->mmaped == MAP_FAILED, "mmap for save failed");

  char *mm = file->mmaped;
  uint32_t hd;

  idsi_t *idsi = ids_iter(&file->ids);

  while (ids_next(&hd, &idsi)) {
    if (!mdbs[hd])
      continue;

    size_t size_written = _corm_save(mm, hd);
    mm += size_written;
  }

  file_close(file);
}

  void /* API */
corm_save(void)
{
  uint32_t c = corm_iter(corm_files_hd, NULL, 0);
  const void *key, *value;

  while (corm_next(&key, &value, c))
    corm_save_file((char *) key);
}

/* MULTI-VALUE API {{{ */

  uint32_t /* API */
corm_get_multi(uint32_t hd, const void *key)
{
  corm_head_t *head = &corm_heads[hd];
  corm_t *corm = &corms[hd];

  if (key == NULL)
    return corm_iter(hd, NULL, 0);

  if (!(head->flags & CM_MULTIVALUE)) {
    uint32_t cur = corm_iter(hd, key, 0);
    if (corm_cursors[cur].pos == CM_MISS) {
      corm_fin(cur);
      return CM_MISS;
    }
    return cur;
  }

  /* CM_MULTIVALUE: walk the key's duplicate chain (O(k)). Cursor advances
   * through corm->mv_next[] in insertion order, avoiding the sorted-index
   * rebuild that corm_iter(key, CM_RANGE) would trigger on a dirty map. */
  size_t key_len;
  uint32_t key_hash;
  (void) corm_id_ex(hd, key, &key_len, &key_hash);
  uint32_t mv_slot = corm_id_hash(hd, key, key_len, key_hash);
  uint32_t head_pos = (mv_slot != CM_MISS) ? corm->map[mv_slot] : CM_MISS;
  if (head_pos == CM_MISS)
    return CM_MISS;

  uint32_t cur = idm_new(&cursor_idm);
  corm_cur_t *cursor = &corm_cursors[cur];
  cursor->hd = hd;
  cursor->pos = head_pos;
  cursor->ipos = head_pos;
  cursor->end_pos = CM_MISS;
  cursor->sub_cur = 0;
  cursor->key = NULL;
  cursor->key_len = 0;
  cursor->flags = CM_MVCHAIN;
  return cur;
}

  uint32_t /* API */
corm_count(uint32_t hd, const void *key)
{
  corm_head_t *head = &corm_heads[hd];

  if (key == NULL) {
    /* Count total entries in map */
    return head->n;
  }

  /* For non-multivalue maps, return 0 or 1 */
  if (!(head->flags & CM_MULTIVALUE)) {
    return corm_get(hd, key) != NULL ? 1 : 0;
  }

  /* For multivalue maps, use binary search to find first and last
   * occurrences. This keeps the count path logarithmic even when the
   * duplicate run is large. */
  int first = corm_bsearch_ex(hd, key, NULL, CORM_BSEARCH_FIRST);
  if (first == -1)
    return 0;

  int last = corm_bsearch_ex(hd, key, NULL, CORM_BSEARCH_LAST);
  return (uint32_t)(last - first + 1);
}

/* }}} */
