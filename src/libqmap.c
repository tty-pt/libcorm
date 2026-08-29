/* libqmap.c
 * Licence: BSD-2-Clause
 *
 * I'm adding some comments to make it easier to understand,
 * but whatever's user API is documented in the header file.
 */
#include "./../include/ttypt/qmap.h"
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

#define QM_SEED 13
#define QM_DEFAULT_MASK 0xFF
#define QM_MAX 1024
#define QMAP_POOL_STEP 16
#define QMAP_POOL_MAX 4096
#define QMAP_POOL_BINS (QMAP_POOL_MAX / QMAP_POOL_STEP)

#define TYPES_MASK 0xFF

#define DEBUG_LVL 1

#define DEBUG(lvl, ...) \
  if (DEBUG_LVL > lvl) WARN(__VA_ARGS__)

#define VAL_ADDR(qmap, n) \
  (void **)(((char *) qmap->table) \
      + sizeof(void *) * n)

typedef struct qmap_blk {
  struct qmap_blk *next;
  size_t size;
} qmap_blk_t;

  static inline size_t
qmap_payload_off(size_t key_len)
{
  size_t align = sizeof(void *) - 1;

  return (key_len + align) & ~align;
}

static_assert(QM_MISS == UINT32_MAX, "assume U32INT_MAX");

enum QM_MBR {
  QM_KEY,
  QM_VALUE,
};

enum qm_internal_flags {
  QM_SDIRTY = 1, // sorted list needs rebuild
  QM_IS_MIRROR = 2,  // this is a QM_MIRROR map (shares positions with primary)
};

typedef struct {
  uint32_t types[2], n, m, mask, flags,
           phd, sorted_n, iflags, dbid;
  uint32_t record_id;  /* 0 = not record-aware */
  uint32_t vstr_hd;    /* handle to QM_STR/QM_STR map for QM_VSTR fields, 0=lazy */
  const char *file;
  uint32_t *inv_hds;   /* per-field inverse map handles, calloc'd at open */
  char get_buf[64];    /* reusable formatting buffer for QM_U32/QM_REFERENCE */
} qmap_head_t;

typedef struct {
  idm_t idm;

  uint32_t *map;  	// id -> n
  const void **omap;	// n -> key
  uint32_t *key_hashes;	// n -> cached key hash
  void **table;		// n -> values
  size_t *key_sizes;	// n -> size of allocated key
  size_t *val_sizes;	// n -> size of allocated value
  qmap_blk_t *payload_bins[QMAP_POOL_BINS];

  ids_t linked;
  qmap_assoc_t *assoc;
  void *assoc_userdata;
  qmap_assoc_multi_t *m_assoc;
  void *m_assoc_userdata;

  uint32_t *sorted_idx;
} qmap_t;

  static inline void *
qmap_payload_alloc(qmap_t *qmap, size_t key_len, size_t val_len)
{
  size_t raw = qmap_payload_off(key_len) + val_len;
  size_t size = (raw + (QMAP_POOL_STEP - 1)) & ~(QMAP_POOL_STEP - 1);
  qmap_blk_t *blk;
  uint32_t bin;

  if (size <= QMAP_POOL_MAX) {
    bin = (uint32_t) (size / QMAP_POOL_STEP - 1);
    blk = qmap->payload_bins[bin];
    if (blk) {
      qmap->payload_bins[bin] = blk->next;
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
qmap_payload_free(qmap_t *qmap, void *key)
{
  qmap_blk_t *blk;
  uint32_t bin;

  if (!key)
    return;

  blk = ((qmap_blk_t *) key) - 1;
  if (blk->size <= QMAP_POOL_MAX) {
    bin = (uint32_t) (blk->size / QMAP_POOL_STEP - 1);
    blk->next = qmap->payload_bins[bin];
    qmap->payload_bins[bin] = blk;
  } else
    free(blk);
}

  static inline void
qmap_payload_flush(qmap_t *qmap)
{
  for (size_t i = 0; i < QMAP_POOL_BINS; i++) {
    qmap_blk_t *blk = qmap->payload_bins[i];
    while (blk) {
      qmap_blk_t *next = blk->next;
      free(blk);
      blk = next;
    }
    qmap->payload_bins[i] = NULL;
  }
}

  static inline size_t
qmap_payload_cap(const void *key)
{
  const qmap_blk_t *blk = ((const qmap_blk_t *) key) - 1;
  return blk->size;
}

typedef struct {
  uint32_t hd, pos, sub_cur, ipos, end_pos, flags;
  size_t key_len;
  const void * key;
} qmap_cur_t;

typedef uint32_t qmap_hash_t(
    const void * const key,
    size_t len);

typedef struct {
  size_t len;
  qmap_measure_t *measure;
  qmap_hash_t *hash;
  qmap_cmp_t *cmp;
} qmap_type_t;

typedef struct {
  ids_t ids;
  int fd;
  char *mmaped;
  size_t size;
} qmap_file_t;

static qmap_head_t qmap_heads[QM_MAX];
static qmap_t qmaps[QM_MAX];
static qmap_cur_t qmap_cursors[QM_MAX];
static idm_t idm, cursor_idm;
static uint32_t _qsort_cmp_hd;

static qmap_type_t qmap_types[TYPES_MASK + 1];
static uint32_t types_n = 0;

static uint32_t qmap_files_hd, qmap_dbs_hd;
static int mdbs[QM_MAX];

/* ── Record-aware map support ─────────────────────────────────────────── */

#define QMAP_MAX_RECORDS 64
#define QMAP_MAX_RECORD_FIELDS 32

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
  } fields[QMAP_MAX_RECORD_FIELDS];
  size_t field_count;
} qmap_record_t;

static qmap_record_t qmap_records[QMAP_MAX_RECORDS];
static uint32_t qmap_records_n = 0;

/* ── Record field lookup helper ───────────────────────────────────────── */

static int qmap_record_find_field(uint32_t record_id, const char *field_name)
{
  if (!record_id || record_id > qmap_records_n) return -1;
  for (size_t i = 0; i < qmap_records[record_id].field_count; i++) {
    if (strcmp(qmap_records[record_id].fields[i].name, field_name) == 0)
      return (int)i;
  }
  return -1;
}

/* }}} */

/* BUILT-INS {{{ */

  static uint32_t
qmap_nohash(const void * const key, size_t len UNUSED)
{
  uint32_t u;
  memcpy(&u, key, sizeof(u));
  return u;
}

static uint32_t
qmap_chash(const void *data, size_t len) {
  return XXH32(data, len, QM_SEED);
}

  static int
qmap_ccmp(const void * const a,
    const void * const b,
    size_t len)
{
  return memcmp((char *) a, (char *) b, len);
}

  static int
qmap_scmp(const void * const a,
    const void * const b,
    size_t len UNUSED)
{
  return strcmp((const char *)a, (const char *)b);
}

  static int
qmap_ucmp(const void * const a,
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
qmap_rassoc(const void **skey,
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
qmap_key(uint32_t hd, uint32_t n)
{
  qmap_t *qmap = &qmaps[hd];
  return (void *) qmap->omap[n];
}

/* Easily obtain the pointer to the value */
static inline void *
qmap_val(uint32_t hd, uint32_t n) {
  qmap_head_t *head = &qmap_heads[hd];
  qmap_t *pqmap;

  if (head->flags & QM_PGET)
    return qmap_key(head->phd, n);

  pqmap = &qmaps[head->phd];
  return * VAL_ADDR(pqmap, n);
}

/* In some cases we want to calculate the id based on the
 * qmap's hash function and the key, and the mask. Other
 * times it's not useful to do that. This is for when it is.
 *
 * When requested, also returns the computed key length/hash so
 * callers that need to store the metadata do not recompute it.
 */
static inline uint32_t
qmap_id_hash(uint32_t hd, const void * const key,
    size_t key_len, uint32_t key_hash);

  static inline uint32_t
qmap_id_ex(uint32_t hd, const void * const key,
    size_t *key_len_out, uint32_t *key_hash_out)
{
  qmap_head_t *head = &qmap_heads[hd];
  uint32_t ktype = head->types[QM_KEY];
  qmap_type_t *type = &qmap_types[ktype];

  size_t key_len;
  uint32_t key_hash;

  if (ktype == QM_STR) {
    key_len = strlen((const char *)key) + 1;
    key_hash = XXH32(key, key_len, QM_SEED);
  } else if (ktype == QM_U32 || ktype == QM_HNDL) {
    key_len = sizeof(uint32_t);
    key_hash = *(const uint32_t *)key;
  } else if (ktype == QM_PTR) {
    key_len = sizeof(void *);
    key_hash = XXH32(key, key_len, QM_SEED);
  } else {
    key_len = type->measure ? type->measure(key) : type->len;
    key_hash = type->hash(key, key_len);
  }

  if (key_len_out)
    *key_len_out = key_len;
  if (key_hash_out)
    *key_hash_out = key_hash;

  return qmap_id_hash(hd, key, key_len, key_hash);
}

  static inline uint32_t
qmap_id_hash(uint32_t hd, const void * const key,
    size_t key_len, uint32_t key_hash)
{
  qmap_head_t *head = &qmap_heads[hd];
  qmap_t *qmap = &qmaps[hd];
  uint32_t ktype = head->types[QM_KEY];
  uint32_t id = key_hash & head->mask;
  uint32_t probe_count = 0;

  while (1) {
    uint32_t n = qmap->map[id];

    if (n == QM_MISS)
      return id;

    if (qmap->key_hashes[n] == key_hash) {
      const void *okey = qmap_key(hd, n);
      if (okey) {
        if (ktype == QM_STR) {
          if (memcmp(okey, key, key_len) == 0)
            return id;
        } else if (ktype == QM_U32 || ktype == QM_HNDL) {
          if (*(const uint32_t *)okey == *(const uint32_t *)key)
            return id;
        } else if (ktype == QM_PTR) {
          if (*(const void * const *)okey == *(const void * const *)key)
            return id;
        } else {
          qmap_type_t *type = &qmap_types[ktype];
          size_t len;
          if (type->measure) {
            size_t okey_len = qmap->key_sizes[n];
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
      return QM_MISS;
  }
}

  static inline uint32_t
qmap_id(uint32_t hd, const void * const key)
{
  return qmap_id_ex(hd, key, NULL, NULL);
}

/* }}} */

/* B-TREE SUPPORT HELPERS {{{ */

  static int
qmap_n_cmp(const void *a, const void *b)
{
  uint32_t n_a = *(const uint32_t *)a;
  uint32_t n_b = *(const uint32_t *)b;

  const void *key_a = qmap_key(_qsort_cmp_hd, n_a);
  const void *key_b = qmap_key(_qsort_cmp_hd, n_b);

  if (key_a == NULL || key_b == NULL)
    return 0;

  qmap_head_t *head = &qmap_heads[_qsort_cmp_hd];
  qmap_t *qmap = &qmaps[_qsort_cmp_hd];
  qmap_type_t *type = &qmap_types[head->types[QM_KEY]];

  if (type->measure) {
    size_t len_a = qmap->key_sizes[n_a];
    size_t len_b = qmap->key_sizes[n_b];
    size_t len = (len_a > len_b) ? len_a : len_b;
    return type->cmp(key_a, key_b, len);
  }

  return type->cmp(key_a, key_b, type->len);
}

  static void
qmap_rebuild_sorted(uint32_t hd)
{
  qmap_head_t *head = &qmap_heads[hd];
  qmap_t *qmap = &qmaps[hd];
  uint32_t n_idx = 0;

  for (uint32_t n = 0; n < qmap->idm.last; n++) {
    if (qmap->omap[n] != NULL)
      qmap->sorted_idx[n_idx++] = n;
  }
  head->sorted_n = n_idx;

  _qsort_cmp_hd = hd;
  qsort(qmap->sorted_idx, head->sorted_n,
      sizeof(uint32_t), qmap_n_cmp);

  head->iflags &= ~QM_SDIRTY;
}

/* Binary search modes */
enum {
  QMAP_BSEARCH_ANY = 0,    /* Find any match (original behavior) */
  QMAP_BSEARCH_FIRST = 1,  /* Find first occurrence */
  QMAP_BSEARCH_LAST = 2    /* Find last occurrence */
};

/* Unified binary search with mode parameter.
 * For QMAP_BSEARCH_ANY: returns insertion point if not found, sets *exact
 * For QMAP_BSEARCH_FIRST/LAST: returns position or -1 if not found */
  static int
qmap_bsearch_ex(uint32_t hd, const void *key, int *exact, int mode)
{
  qmap_head_t *head = &qmap_heads[hd];
  qmap_t *qmap = &qmaps[hd];
  int result = -1;

  if (head->iflags & QM_SDIRTY)
    qmap_rebuild_sorted(hd);

  if (head->sorted_n == 0) {
    if (exact) *exact = 0;
    return (mode == QMAP_BSEARCH_ANY) ? 0 : -1;
  }

  qmap_type_t *type = &qmap_types[head->types[QM_KEY]];
  size_t key_len = qmap_len(head->types[QM_KEY], key);
  int low = 0, high = (int) head->sorted_n - 1;
  int mid = 0;

  if (exact) *exact = 0;

  while (low <= high) {
    mid = low + (high - low) / 2;
    const void *mid_key = qmap_key(hd, qmap->sorted_idx[mid]);

    size_t len;
    if (type->measure) {
      size_t mid_len = qmap->key_sizes[qmap->sorted_idx[mid]];
      len = (key_len > mid_len) ? key_len : mid_len;
    } else
      len = type->len;

    int cmp = type->cmp(mid_key, key, len);

    if (cmp == 0) {
      if (exact) *exact = 1;
      result = mid;
      if (mode == QMAP_BSEARCH_ANY)
        return mid;  /* Return immediately for ANY mode */
      else if (mode == QMAP_BSEARCH_FIRST)
        high = mid - 1;  /* Continue searching left */
      else /* QMAP_BSEARCH_LAST */
        low = mid + 1;   /* Continue searching right */
    } else if (cmp < 0)
      low = mid + 1;
    else
      high = mid - 1;
  }

  return (mode == QMAP_BSEARCH_ANY && (exact == NULL || !*exact)) ? low : result;
}

/* Wrapper for backward compatibility with original qmap_bsearch */
  static inline int
qmap_bsearch(uint32_t hd, const void *key, int *exact)
{
  return qmap_bsearch_ex(hd, key, exact, QMAP_BSEARCH_ANY);
}

/* OPEN / INITIALIZATION {{{ */

/* Low level way of opening databases. */
  static uint32_t
_qmap_open(uint32_t ktype, uint32_t vtype,
    uint32_t mask, uint32_t flags)
{
  uint32_t hd = idm_new(&idm);
  qmap_head_t *head = &qmap_heads[hd];
  qmap_t *qmap = &qmaps[hd];
  uint32_t len;
  size_t ids_len;

  mask = mask ? mask : QM_DEFAULT_MASK;

  /* QM_MULTIVALUE requires QM_SORTED */
  if ((flags & QM_MULTIVALUE) && !(flags & QM_SORTED)) {
    fprintf(stderr, "qmap: QM_MULTIVALUE requires QM_SORTED flag\n");
    idm_del(&idm, hd);
    return QM_MISS;
  }

  DEBUG(1, "%u %u 0x%x %u\n",
      hd, ktype,
      mask, flags);

  len = mask + 1u;

  CBUG((len & mask) != 0, "mask must be 2^k - 1\n");
  ids_len = len * sizeof(uint32_t);

  qmap->map = malloc(ids_len);
  qmap->omap = malloc(len * sizeof(void *));
  CBUG(!(qmap->map && qmap->omap), "malloc error\n");
  qmap->idm = idm_init();
  qmap->linked = ids_init();

  head->m = len;
  head->types[QM_KEY] = ktype;
  head->types[QM_VALUE] = vtype;
  head->mask = mask;
  head->flags = flags;
  head->phd = hd;

  // STORE {{{
  qmap->table = malloc(sizeof(void *) * len);
  CBUG(!qmap->table, "malloc error (table)\n");
  memset(qmap->table, 0, sizeof(void *) * len);
  // }}}

  qmap->key_hashes = calloc(len, sizeof(*qmap->key_hashes));
  CBUG(!qmap->key_hashes, "malloc error (key_hashes)\n");

  if (flags & QM_SORTED) {
    qmap->sorted_idx = malloc(sizeof(uint32_t) * len);
    CBUG(!qmap->sorted_idx, "malloc error (sorted_idx)\n");
  } else
    qmap->sorted_idx = NULL;

  qmap->key_sizes = calloc(len, sizeof(*qmap->key_sizes));
  qmap->val_sizes = calloc(len, sizeof(*qmap->val_sizes));
  CBUG(!(qmap->key_sizes && qmap->val_sizes), "malloc error (size arrays)\n");

  head->iflags |= QM_SDIRTY;
  head->sorted_n = 0;

  memset(qmap->map, 0xFF, ids_len);
  memset(qmap->omap, 0, sizeof(void *) * len);

  return hd;
}

static inline void
qmap_load_file(char *filename, uint32_t dbid);

static inline uint32_t
_qmap_put(uint32_t hd, const void * key,
    const void *value, uint32_t pn);

  static void
qmap_rebuild_map(uint32_t hd)
{
  qmap_head_t *head = &qmap_heads[hd];
  qmap_t *qmap = &qmaps[hd];

  memset(qmap->map, 0xFF,
      sizeof(uint32_t) * head->m);

  for (uint32_t n = 0; n < qmap->idm.last; n++) {
    const void *key = qmap->omap[n];

    if (!key)
      continue;

    uint32_t id =
      qmap->key_hashes[n] & head->mask;

    while (qmap->map[id] != QM_MISS)
      id = (id + 1) & head->mask;

    qmap->map[id] = n;
  }
}

  static void
qmap_grow(uint32_t hd)
{
  qmap_head_t *head = &qmap_heads[hd];
  qmap_t *qmap = &qmaps[hd];
  uint32_t old_m = head->m;
  uint32_t new_m = old_m << 1;

  CBUG((new_m & (new_m - 1)) != 0,
      "qmap_grow: capacity not power-of-two");

  void *tmp;

  tmp = realloc(qmap->omap, sizeof(void *) * new_m);
  CBUG(!tmp, "realloc(omap)");
  qmap->omap = tmp;
  memset(&qmap->omap[old_m], 0,
      sizeof(void *) * (new_m - old_m));

  if (qmap->table) {
    tmp = realloc(qmap->table,
        sizeof(void *) * new_m);

    CBUG(!tmp, "realloc(table)");

    qmap->table = tmp;

    memset(&qmap->table[old_m], 0,
        sizeof(void *) * (new_m - old_m));
  }

  tmp = realloc(qmap->key_hashes, sizeof(uint32_t) * new_m);
  CBUG(!tmp, "realloc(key_hashes)");
  qmap->key_hashes = tmp;
  memset(qmap->key_hashes + old_m, 0,
      sizeof(uint32_t) * (new_m - old_m));

  tmp = realloc(qmap->key_sizes, sizeof(size_t) * new_m);
  CBUG(!tmp, "realloc(key_sizes)");
  qmap->key_sizes = tmp;
  memset(qmap->key_sizes + old_m, 0,
      sizeof(size_t) * (new_m - old_m));

  tmp = realloc(qmap->val_sizes, sizeof(size_t) * new_m);
  CBUG(!tmp, "realloc(val_sizes)");
  qmap->val_sizes = tmp;
  memset(qmap->val_sizes + old_m, 0,
      sizeof(size_t) * (new_m - old_m));

  if (qmap->sorted_idx) {
    tmp = realloc(qmap->sorted_idx, sizeof(uint32_t) * new_m);
    CBUG(!tmp, "realloc(sorted_idx)");
    qmap->sorted_idx = tmp;
    memset(qmap->sorted_idx + old_m, 0xFF,
        sizeof(uint32_t) * (new_m - old_m));
  }

  free(qmap->map);
  qmap->map = malloc(sizeof(uint32_t) * new_m);
  CBUG(!qmap->map, "malloc(map)");
  memset(qmap->map, 0xFF, sizeof(uint32_t) * new_m);

  head->m = new_m;
  head->mask = new_m - 1;

  CBUG(head->m != head->mask + 1,
      "qmap invariant broken");

  qmap_rebuild_map(hd);
}

  uint32_t /* API */
qmap_open(const char *filename,
    const char *database,
    uint32_t ktype, uint32_t vtype,
    uint32_t mask, uint32_t flags)
{
  uint32_t record_id = 0;

  /* ── Handle QM_RECORD flag ────────────────────────────────────────── */
  if (flags & QM_RECORD_FLAG) {
    record_id = QM_RECORD_ID(flags);
    if (!record_id || record_id > qmap_records_n) {
      fprintf(stderr, "qmap_open: unknown record_id %u\n", record_id);
      return QM_MISS;
    }
    /* Validate: vtype must match the registered struct type */
    if (vtype != qmap_records[record_id].struct_type_id) {
      fprintf(stderr, "qmap_open: record %u requires vtype=%u, got %u\n",
              record_id, qmap_records[record_id].struct_type_id, vtype);
      return QM_MISS;
    }
    /* Record-aware maps require string keys (composite key separator) */
    if (ktype != QM_STR) {
      fprintf(stderr, "qmap_open: record maps require ktype=QM_STR\n");
      return QM_MISS;
    }
  }

  /* Strip record bits so _qmap_open doesn't see them */
  flags &= ~(QM_RECORD_MASK | QM_RECORD_FLAG);

  uint32_t hd = _qmap_open(ktype, vtype, mask, flags);

  /* Check if open failed */
  if (hd == QM_MISS)
    return QM_MISS;

  qmap_head_t *head = &qmap_heads[hd];

  head->record_id = record_id;
  head->vstr_hd = 0;
  head->file = filename;

  /* Allocate per-field inverse index handles for record-aware maps */
  if (record_id > 0) {
    uint32_t fc = (uint32_t)qmap_records[record_id].field_count;
    head->inv_hds = calloc(fc, sizeof(uint32_t));
  } else {
    head->inv_hds = NULL;
  }
  head->get_buf[0] = '\0';
  if (database)
    head->dbid = XXH32(database, strlen(database), QM_SEED);
  else
    head->dbid = QM_MISS;

  if (!filename)
    goto file_skip;

  if (database) {
    char buf[strlen(filename)
      + strlen(database) + 2];

    snprintf(buf, sizeof(buf), "%s/%s",
        filename, database);

    const uint32_t *ehd = qmap_get(qmap_dbs_hd, buf);
    uint32_t old_hd = ehd ? *ehd : QM_MISS;
    qmap_put(qmap_dbs_hd, buf, &hd);

    if (old_hd != QM_MISS && mdbs[old_hd])
      mdbs[old_hd] = 0;
  }

  mdbs[hd] = 1;  /* Mark as dirty for save, regardless of database name */

  const qmap_file_t *file_p
    = qmap_get(qmap_files_hd, filename);

  if (!file_p) {
    qmap_file_t file;
    memset(&file, 0, sizeof(file));
    file.ids = ids_init();
    file.fd = -1;
    ids_push(&file.ids, hd);
    qmap_put(qmap_files_hd, filename, &file);
  } else
    ids_push((ids_t *) &file_p->ids, hd);

file_skip:
  if (filename)
    qmap_load_file((char*) filename, head->dbid);

  if (!(flags & QM_MIRROR))
    return hd;

  flags &= ~QM_AINDEX;
  uint32_t mirror_hd = _qmap_open(vtype, ktype, mask, flags | QM_PGET);
  qmap_heads[mirror_hd].iflags |= QM_IS_MIRROR;  /* Mark as mirror for position sharing */
  qmap_assoc(hd + 1, hd, NULL, NULL);

  /* If data was loaded before mirror creation, populate the mirror now */
  if (filename && head->n > 0) {
    uint32_t cur = qmap_iter(hd, NULL, 0);
    const void *key, *value;
    while (qmap_next(&key, &value, cur)) {
      _qmap_put(mirror_hd, value, key, qmaps[hd].map[qmap_id(hd, key)]);
    }
    qmap_fin(cur);
  }

  return hd;
}

  uint32_t /* API */
qmap_get_vtype(uint32_t hd)
{
  return qmap_heads[hd].types[QM_VALUE];
}

  size_t /* API */
qmap_type_len(uint32_t type_id)
{
  return qmap_types[type_id].len;
}

  static size_t
s_measure(const void *key)
{
  return strlen(key) + 1;
}

static void file_close(qmap_file_t *file) {
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
  static void qmap_destruct(void) {
    qmap_save();

    for (uint32_t i = idm.last; i-- > 0; )
      qmap_close(i);

    idm_drop(&cursor_idm);
    idm_drop(&idm);

    uint32_t cur = qmap_iter(qmap_files_hd, NULL, 0);
    const void *key, *value;

    while (qmap_next(&key, &value, cur))
      file_close((qmap_file_t *) value);

    qmap_close(qmap_dbs_hd);
    qmap_close(qmap_files_hd);
  }

__attribute__((constructor))
  static void
qmap_init(void)
{
  qmap_type_t *type;

  memset(qmaps, 0, sizeof(qmaps));
  memset(qmap_heads, 0, sizeof(qmap_heads));

  idm = idm_init();
  cursor_idm = idm_init();

  // QM_PTR
  type = &qmap_types[qmap_reg(sizeof(void *))];

  // QM_HNDL
  type = &qmap_types[qmap_reg(sizeof(uint32_t))];
  type->hash = qmap_nohash;
  type->cmp = qmap_ucmp;

  // QM_STR
  type = &qmap_types[qmap_mreg(s_measure)];
  type->cmp = qmap_scmp;

  // QM_U32
  type = &qmap_types[qmap_reg(sizeof(uint32_t))];
  type->cmp = qmap_ucmp;

  uint32_t qm_file = qmap_reg(sizeof(qmap_file_t));
  qmap_files_hd = _qmap_open(QM_STR, qm_file,
      QM_DEFAULT_MASK, 0);

  qmap_dbs_hd = _qmap_open(QM_STR, QM_U32, QM_DEFAULT_MASK, 0);
}

/* }}} */

/* PUT {{{ */

/* This is the low-level put. It doesn't aim to provide
 * MIRROR functionality in itself, just putting in whatever
 * kind of map.
 */
/* Helper: Update IDM last position if needed */
  static inline void
update_idm_last(qmap_t *qmap, uint32_t pn)
{
  if (pn >= qmap->idm.last)
    qmap->idm.last = pn + 1;
}

/* ── Inverse index helpers for reference fields ──────────────────────── */

static void ensure_inv_hd(qmap_head_t *head, int fi)
{
  if (head->inv_hds[fi] == 0)
    head->inv_hds[fi] = qmap_open(NULL, NULL, QM_U32, QM_STR, 0xFF, 0);
}

static void ensure_vstr_hd(qmap_head_t *head)
{
  if (head->vstr_hd == 0)
    head->vstr_hd = qmap_open(NULL, NULL, QM_STR, QM_STR, 0xFF, 0);
}

static void inverse_add(qmap_head_t *head, int fi,
    uint32_t target_pos, uint32_t source_pos)
{
  ensure_inv_hd(head, fi);
  uint32_t inv_hd = head->inv_hds[fi];
  const char *existing = qmap_get(inv_hd, &target_pos);

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
    qmap_put(inv_hd, &target_pos, buf);
  } else {
    char buf[32];
    snprintf(buf, sizeof(buf), "%u\n", source_pos);
    qmap_put(inv_hd, &target_pos, buf);
  }
}

static void inverse_remove(qmap_head_t *head, int fi,
    uint32_t target_pos, uint32_t source_pos)
{
  if (head->inv_hds[fi] == 0)
    return;
  uint32_t inv_hd = head->inv_hds[fi];
  const char *existing = qmap_get(inv_hd, &target_pos);
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
    qmap_del(inv_hd, &target_pos);
  else
    qmap_put(inv_hd, &target_pos, buf);
}

static void clean_inverses_for_pos(qmap_head_t *head, uint32_t pos)
{
  qmap_record_t *rec = &qmap_records[head->record_id];
  const void *struct_ptr = qmap_val(head->phd, pos);
  if (!struct_ptr)
    return;

  for (size_t fi = 0; fi < rec->field_count; fi++) {
    if (rec->fields[fi].target_record == 0)
      continue;
    uint32_t ft = rec->fields[fi].type;
    size_t  fo = rec->fields[fi].offset;
    size_t  fm = rec->fields[fi].max_size;

    if (ft == QM_REFERENCE) {
      const char *id = (const char *)struct_ptr + fo;
      if (id && id[0]) {
        uint32_t target_hd = rec->fields[fi].target_hd;
        if (target_hd > 0) {
          uint32_t tp = qmap_pos(target_hd, id);
          if (tp != UINT32_MAX)
            inverse_remove(head, (int)fi, tp, pos);
        }
      }
    } else if (ft == QM_MULTI_REFERENCE && fm > 0) {
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
static void handle_inverse_put(qmap_head_t *head, int fi,
    uint32_t source_pos,
    const uint8_t *old_val, const void *new_val,
    uint32_t ft, size_t fm)
{
  qmap_record_t *rec = &qmap_records[head->record_id];
  (void)fm;
  if (rec->fields[fi].target_record == 0)
    return;

  uint32_t old_pos[2048], new_pos[2048];
  size_t n_old = 0, n_new = 0;

  if (ft == QM_REFERENCE) {
    uint32_t target_hd = rec->fields[fi].target_hd;
    const char *id = (const char *)new_val;
    if (id && id[0] && target_hd > 0) {
      uint32_t p = qmap_pos(target_hd, id);
      if (p != UINT32_MAX) new_pos[n_new++] = p;
    }
    id = (const char *)old_val;
    if (id && id[0] && target_hd > 0) {
      uint32_t p = qmap_pos(target_hd, id);
      if (p != UINT32_MAX) old_pos[n_old++] = p;
    }
  } else if (ft == QM_MULTI_REFERENCE) {
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
_qmap_put(uint32_t hd, const void * key,
    const void *value, uint32_t pn)
{
  qmap_head_t *head = &qmap_heads[hd];
  qmap_t *qmap = &qmaps[hd];

  /* Grow before clustering gets pathological */
  if (!(head->flags & QM_NOGROW)) {
    if ((head->n + 1) * 4 >= head->m * 3)
      qmap_grow(hd);
  }

  uint32_t n;
  const void *aval = value;
  void *rval, *rkey;
  size_t key_len, klen;
  uint32_t key_hash;
  uint32_t lookup_id;
  uint32_t key_id;

  if (key) {
    uint32_t old_n;

    lookup_id = qmap_id_ex(hd, key, &key_len, &key_hash);
    if (lookup_id == QM_MISS) {
      WARN("qmap %u: probe failure after grow", hd);
      return QM_MISS;
    }
    old_n = qmap->map[lookup_id];

    if (old_n == QM_MISS) {
      if (pn != QM_MISS) {
        n = pn;
        /* Update IDM to know about this position */
        update_idm_last(qmap, pn);
      } else {
        n = idm_new(&qmap->idm);
      }
      head->n ++;
    } else if (head->flags & QM_MULTIVALUE) {
      /* QM_MULTIVALUE: Allow duplicate keys.
       * If pn is provided (from qmap_assoc), use it.
       * Otherwise, allocate a new position.
       * Don't update hash table - it keeps pointing to first occurrence.
       * Duplicate is accessible via sorted_idx iteration. */
      if (pn != QM_MISS && pn != old_n) {
        n = pn;
        /* Update IDM to know about this position */
        update_idm_last(qmap, pn);
      } else if (pn == QM_MISS) {
        n = idm_new(&qmap->idm);
      } else {
        n = old_n;  /* pn == old_n, update in place */
      }

      if (n != old_n)
        head->n ++;
    } else
      n = old_n;
  } else {
    key_id = n = idm_new(&qmap->idm);
    head->n ++;
    key = &key_id;
    if (head->types[QM_KEY] == QM_STR) {
      static char _auto_key[32];
      snprintf(_auto_key, sizeof(_auto_key), "%u", key_id);
      key = _auto_key;
    }
    lookup_id = qmap_id_ex(hd, key, &key_len, &key_hash);
  }

  if (n >= head->m) {
    if (head->flags & QM_NOGROW) {
      head->n--;
      WARN("qmap %u: capacity reached (%u entries, max %u)",
          hd, head->n, head->m);
      return QM_MISS;
    }
    qmap_grow(hd);
    lookup_id = qmap_id_hash(hd, key, key_len, key_hash);
  }
  DEBUG(2, "%u %u %u %p\n", hd, n, lookup_id, key);

  rkey = (void *) key;
  qmap->key_hashes[n] = key_hash;
  qmap->key_sizes[n] = key_len;

  if (head->phd == hd) {
    if (head->types[QM_VALUE] == QM_PTR)
      value = &value;

    klen = qmap_len(head->types[QM_VALUE], aval);

    if (qmap->map[lookup_id] == n) {
      const void *old_key = qmap_key(hd, n);
      size_t off = qmap_payload_off(key_len);
      size_t need = qmap_payload_off(key_len) + klen;

      /* Reuse key allocation if key/value fit in the existing block. */
      if (qmap->key_sizes[n] == key_len &&
          memcmp(old_key, key, key_len) == 0 &&
          qmap_payload_cap(old_key) >= need) {
        rkey = (void *) old_key;
        rval = (void *) ((char *) rkey + off);
      } else {
        qmap_payload_free(qmap, (void *) old_key);
        rkey = qmap_payload_alloc(qmap, key_len, klen);
        rval = (void *) ((char *) rkey + off);
      }

      memcpy(rkey, key, key_len);
      memcpy(rval, value, klen);
      qmap->key_sizes[n] = key_len;
      qmap->val_sizes[n] = klen;
    } else {
      /* New entry - allocate fresh */
      size_t off = qmap_payload_off(key_len);
      rkey = qmap_payload_alloc(qmap, key_len, klen);
      rval = (void *) ((char *) rkey + off);
      memcpy(rkey, key, key_len);
      memcpy(rval, value, klen);
      qmap->key_sizes[n] = key_len;
      qmap->val_sizes[n] = klen;
    }

    * VAL_ADDR(qmap, n) = rval;
  }

  qmap->omap[n] = rkey;

  /* When sharing a position with the primary on updates, a different
   * secondary key may overwrite the same position. Clear any stale hash
   * entries that still point to this position from the former key. */
  if (head->phd != hd && pn != QM_MISS) {
    for (uint32_t i = 0; i < head->m; i++) {
      if (qmap->map[i] == n) {
        qmap->map[i] = QM_MISS;
        break;
      }
    }
  }

  /* For QM_MULTIVALUE duplicates, don't update hash table */
  if (!(head->flags & QM_MULTIVALUE) || qmap->map[lookup_id] == QM_MISS || qmap->map[lookup_id] == n)
    qmap->map[lookup_id] = n;

  head->iflags |= QM_SDIRTY;

  return lookup_id;
}

  uint32_t /* API */
qmap_put(uint32_t hd, const void * const key,
    const void * const value)
{
  uint32_t ahd, n, id;
  idsi_t *cur;
  const void *rkey, *rval;
  qmap_head_t *head = &qmap_heads[hd];

  /* ── Field-level put for record-aware maps ────────────────────────── */
  if (head->record_id > 0) {
    const char *k = (const char *)key;
    const char *colon = strchr(k, ':');
    if (colon) {
      size_t sk_len = (size_t)(colon - k);
      char struct_key[256];
      if (sk_len >= sizeof(struct_key))
        return QM_MISS;
      memcpy(struct_key, k, sk_len);
      struct_key[sk_len] = '\0';
      const char *field_name = colon + 1;

      int fi = qmap_record_find_field(head->record_id, field_name);
      if (fi < 0)
        return QM_MISS;

      size_t struct_size = qmap_records[head->record_id].struct_size;

      /* Get or create the struct entry */
      void *struct_ptr = (void *)qmap_get(hd, struct_key);
      if (!struct_ptr) {
        void *tmp = calloc(1, struct_size);
        if (!tmp) return QM_MISS;
        if (qmap_put(hd, struct_key, tmp) == QM_MISS) {
          free(tmp);
          return QM_MISS;
        }
        free(tmp);
        struct_ptr = (void *)qmap_get(hd, struct_key);
      }

      /* Write the field value */
      uint32_t ft = qmap_records[head->record_id].fields[fi].type;
      size_t  fo = qmap_records[head->record_id].fields[fi].offset;
      size_t  fm = qmap_records[head->record_id].fields[fi].max_size;

      if (ft == QM_VSTR) {
        /* QM_VSTR: store directly under composite key in vstr map,
         * bypassing struct modification entirely. */
        ensure_vstr_hd(head);
        return qmap_put(head->vstr_hd, key, value);
      }

      /* Save old field value for inverse diff */
      size_t old_sz = (ft == QM_STR || ft == QM_REFERENCE || ft == QM_MULTI_REFERENCE) ? fm : sizeof(uint32_t);
      uint8_t old_val_stack[8192];
      uint8_t *old_val = NULL;
      if (head->inv_hds) {
        if (old_sz > sizeof(old_val_stack)) {
          old_val = malloc(old_sz);
          if (!old_val) return QM_MISS;
        } else {
          old_val = old_val_stack;
        }
        memcpy(old_val, (char *)struct_ptr + fo, old_sz);
      }

      if (ft == QM_STR && fm > 0) {
        strncpy((char *)struct_ptr + fo, (const char *)value, fm - 1);
        *((char *)struct_ptr + fo + fm - 1) = '\0';
      } else if (ft == QM_REFERENCE && fm > 0) {
        strncpy((char *)struct_ptr + fo, (const char *)value, fm - 1);
        *((char *)struct_ptr + fo + fm - 1) = '\0';
      } else if (ft == QM_MULTI_REFERENCE && fm > 0) {
        strncpy((char *)struct_ptr + fo, (const char *)value, fm - 1);
        *((char *)struct_ptr + fo + fm - 1) = '\0';
      } else {
        size_t val_len = qmap_len(ft, value);
        memcpy((char *)struct_ptr + fo, value, val_len);
      }

      /* Re-put the struct */
      uint32_t put_id = qmap_put(hd, struct_key, struct_ptr);
      if (put_id == QM_MISS) return QM_MISS;
      uint32_t source_pos = qmaps[hd].map[put_id];

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
    size_t ss = qmap_records[head->record_id].struct_size;
    old_snap = malloc(ss);
    if (old_snap) {
      const void *old_val = qmap_get(hd, key);
      if (old_val)
        memcpy(old_snap, old_val, ss);
      else
        memset(old_snap, 0, ss);  /* new entry, diff against zeros */
    }
  }

  id = _qmap_put(hd, key, value, QM_MISS);
  if (id == QM_MISS) {
    free(old_snap);
    return QM_MISS;
  }
  n = qmaps[hd].map[id];

  cur = ids_iter(&qmaps[hd].linked);
  rkey = qmap_key(hd, n);
  rval = qmap_val(hd, n);

  while (ids_next(&ahd, &cur)) {
    qmap_t *aqmap;
    qmap_head_t *ahead;

    aqmap = &qmaps[ahd];
    ahead = &qmap_heads[ahd];

    if (aqmap->m_assoc) {
      /* Multi-key association: produce multiple secondary keys.
       * Secondary is a root map storing (ref_value, primary_key). */
      const void *skeys[64];
      size_t nkeys = aqmap->m_assoc(skeys, 64, rkey, rval, aqmap->m_assoc_userdata);
      for (size_t i = 0; i < nkeys; i++) {
        _qmap_put(ahd, skeys[i], rkey, QM_MISS);
        free((void *)skeys[i]);
      }
    } else if (aqmap->assoc) {
      const void *skey;

      aqmap->assoc(&skey, rkey, rval, aqmap->assoc_userdata);

      /* Share positions with QM_MIRROR and non-MULTIVALUE linked maps.
       * MULTIVALUE linked maps keep independent positions to avoid
       * hash table repointing complexity on duplicate removal. */
      if (ahead->iflags & QM_IS_MIRROR) {
        _qmap_put(ahd, skey, rval, n);  /* Mirror: share position */
      } else if (ahead->flags & QM_MULTIVALUE) {
        _qmap_put(ahd, skey, rval, QM_MISS);  /* MULTIVALUE: independent */
      } else {
        _qmap_put(ahd, skey, rval, n);  /* General: share position */
      }
    }
  }

  /* ── Update inverse index for reference fields after whole-struct put ── */
  if (old_snap) {
    qmap_record_t *rec = &qmap_records[head->record_id];
    for (size_t fi = 0; fi < rec->field_count; fi++) {
      uint32_t ft = rec->fields[fi].type;
      if ((ft == QM_REFERENCE || ft == QM_MULTI_REFERENCE)
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

static int qmap_lnext(uint32_t *sn, uint32_t cur_id);

  const void * /* API */
qmap_get(uint32_t hd, const void * const key)
{
  qmap_head_t *head = &qmap_heads[hd];

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

      int fi = qmap_record_find_field(head->record_id, colon + 1);
      if (fi < 0)
        return NULL;

      uint32_t ft = qmap_records[head->record_id].fields[fi].type;

      if (ft == QM_VSTR) {
        /* QM_VSTR: look up the composite key directly in vstr map */
        if (head->vstr_hd == 0)
          return NULL;
        return qmap_get(head->vstr_hd, k);
      }

      const void *struct_ptr = qmap_get(hd, struct_key);
      if (!struct_ptr)
        return NULL;

      size_t field_offset = qmap_records[head->record_id].fields[fi].offset;
      const char *result = (const char *)struct_ptr + field_offset;
      return result;
    }
  }

  uint32_t id = qmap_id(hd, key);
  if (id == QM_MISS)
    return NULL;

  uint32_t n = qmaps[hd].map[id];
  if (n == QM_MISS)
    return NULL;

  return qmap_val(hd, n);
}

/* }}} */

/* DELETE {{{ */

  static inline uint32_t
qmap_root(uint32_t hd)
{
  while(qmap_heads[hd].phd != hd)
    hd = qmap_heads[hd].phd;

  return hd;
}

static void qmap_ndel(uint32_t hd, uint32_t n);

static void qmap_ndel_topdown(uint32_t hd, uint32_t n) {
  qmap_head_t *head = &qmap_heads[hd];
  qmap_t *qmap = &qmaps[hd];
  const void *key;
  uint32_t id, ahd;
  idsi_t *cur;

  // Guard against already-closed maps (omap is NULL after close)
  if (!qmap->omap)
    return;

  if (n >= head->m)
    return;

  key = qmap_key(hd, n);

  cur = ids_iter(&qmap->linked);

  while (ids_next(&ahd, &cur)) {
    if (qmaps[ahd].m_assoc && key) {
      /* Multi-assoc: secondary is a root map storing (ref_val, pkey).
       * Iterate to find and delete entries whose value matches the
       * primary key being deleted. */
      uint32_t mcur = qmap_iter(ahd, NULL, 0);
      uint32_t msn;
      uint32_t to_del[256];
      size_t ndel = 0;

      while (qmap_lnext(&msn, mcur)) {
        const void *mval = qmap_val(ahd, msn);
        if (mval && qmap_scmp(mval, key, 0) == 0) {
          to_del[ndel++] = msn;
          if (ndel >= 256) break;
        }
      }
      qmap_fin(mcur);

      for (size_t i = 0; i < ndel; i++)
        qmap_ndel(ahd, to_del[i]);
    } else {
      qmap_ndel_topdown(ahd, n);
    }
  }

  if (!key) {
    qmap->omap[n] = NULL;
    idm_del(&qmap->idm, n);
    head->n --;
    return;
  }

  id = qmap_id(hd, key);

  /* For QM_MULTIVALUE maps, check if other duplicates exist before clearing hash entry.
   * Do this BEFORE freeing the key! */
  uint32_t new_map_entry = QM_MISS;
  if (head->flags & QM_MULTIVALUE) {
    int first = qmap_bsearch_ex(hd, key, NULL, QMAP_BSEARCH_FIRST);

    if (first != -1) {
      uint32_t first_pos = qmap->sorted_idx[first];
      if (first_pos == n) {
        /* Deleting the first entry, check if a second exists */
        int second = first + 1;
        if (second < (int)head->sorted_n) {
          const void *second_key = qmap_key(hd, qmap->sorted_idx[second]);
          qmap_type_t *type = &qmap_types[head->types[QM_KEY]];
          size_t key_len = qmap->key_sizes[n];
          size_t len;
          if (type->measure) {
            size_t second_len = qmap->key_sizes[qmap->sorted_idx[second]];
            len = (key_len > second_len) ? key_len : second_len;
          } else
            len = type->len;

          if (type->cmp(key, second_key, len) == 0)
            new_map_entry = qmap->sorted_idx[second];
        }
      } else {
        /* Not deleting first entry, first remains valid */
        new_map_entry = first_pos;
      }
    }
  }

  if (head->phd == hd) {
    qmap_payload_free(qmap, (void *) key);
    * VAL_ADDR(qmap, n) = NULL;
  }

  qmap->key_hashes[n] = 0;
  qmap->key_sizes[n] = 0;
  qmap->val_sizes[n] = 0;

  head->iflags |= QM_SDIRTY;

  /* Update hash table entry */
  if (id != QM_MISS) {
    if (new_map_entry != QM_MISS)
      qmap->map[id] = new_map_entry;
    else
      qmap->map[id] = QM_MISS;
  }

  qmap->omap[n] = NULL;
  idm_del(&qmap->idm, n);
  head->n --;

}

/* Delete based on position */
static inline void
qmap_ndel(uint32_t hd, uint32_t n) {
  qmap_ndel_topdown(qmap_root(hd), n);
}

  static void
qmap_clear_fast(uint32_t hd)
{
  qmap_head_t *head = &qmap_heads[hd];
  qmap_t *qmap = &qmaps[hd];

  if (head->phd == hd) {
    for (uint32_t n = 0; n < qmap->idm.last; n++) {
      const void *key = qmap->omap[n];
      if (!key)
        continue;
      qmap_payload_free(qmap, (void *) key);
    }
  }

  memset(qmap->map, 0xFF, sizeof(uint32_t) * head->m);
  memset(qmap->omap, 0, sizeof(void *) * head->m);
  memset(qmap->key_hashes, 0, sizeof(uint32_t) * head->m);
  memset(qmap->key_sizes, 0, sizeof(size_t) * head->m);
  if (head->phd == hd) {
    memset(qmap->table, 0, sizeof(void *) * head->m);
    memset(qmap->val_sizes, 0, sizeof(size_t) * head->m);
  }

  idm_drop(&qmap->idm);
  qmap->idm.last = 0;
  head->n = 0;
  head->iflags |= QM_SDIRTY;
}

  void /* API */
qmap_del(uint32_t hd, const void * const key)
{
  qmap_head_t *head = &qmap_heads[hd];

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

      int fi = qmap_record_find_field(head->record_id, colon + 1);
      if (fi < 0)
        return;

      void *struct_ptr = (void *)qmap_get(hd, struct_key);
      if (!struct_ptr)
        return;

      uint32_t ft = qmap_records[head->record_id].fields[fi].type;
      size_t   fo = qmap_records[head->record_id].fields[fi].offset;
      size_t   fm = qmap_records[head->record_id].fields[fi].max_size;

      if (ft == QM_VSTR) {
        /* QM_VSTR: delete the composite key entry from the vstr map */
        if (head->vstr_hd) {
          qmap_del(head->vstr_hd, k);
        }
        return;
      }

      size_t field_size = fm;
      if (field_size == 0)
        field_size = qmap_len(ft, NULL);

      /* Snapshot old value for inverse cleanup */
      uint8_t old_val[8192];
      size_t old_sz = (ft == QM_STR || ft == QM_REFERENCE || ft == QM_MULTI_REFERENCE) ? fm : sizeof(uint32_t);
      if (old_sz > sizeof(old_val)) return;
      memcpy(old_val, (char *)struct_ptr + fo, old_sz);

      memset((char *)struct_ptr + fo, 0, field_size);

      /* Re-put the struct */
      uint32_t put_id = qmap_put(hd, struct_key, struct_ptr);
      if (put_id == QM_MISS) return;
      uint32_t source_pos = qmaps[hd].map[put_id];

      /* Clean inverse: old references removed (new value is zeroed) */
      if (head->inv_hds && (ft == QM_REFERENCE || ft == QM_MULTI_REFERENCE)) {
        handle_inverse_put(head, fi, source_pos, old_val,
                           (const void*)"",
                           ft, fm);
      }
      return;
    }
  }

  uint32_t cur = qmap_iter(hd, key, 0), sn;

  if (head->flags & QM_MULTIVALUE) {
    if (qmap_lnext(&sn, cur)) {
      if (head->record_id > 0 && head->inv_hds)
        clean_inverses_for_pos(head, sn);
      qmap_ndel(hd, sn);
    }
    qmap_fin(cur);
  } else {
    while (qmap_lnext(&sn, cur)) {
      if (head->record_id > 0 && head->inv_hds)
        clean_inverses_for_pos(head, sn);
      qmap_ndel(hd, sn);
    }
    qmap_fin(cur);
  }
}

  void
qmap_del_all(uint32_t hd, const void * const key)
{
  qmap_head_t *head = &qmap_heads[hd];
  qmap_t *qmap = &qmaps[hd];

  if (head->flags & QM_MULTIVALUE) {
    /* Fast path: if nothing is linked to this map, bulk-delete by
     * clearing the matching slots once and rebuilding the hash table.
     * This avoids repeated probe-chain maintenance for every duplicate. */
    uint32_t cur = qmap_get_multi(hd, key);
    uint32_t sn;
    size_t n = 0, cap = head->n ? head->n : 1;
    uint32_t *positions = malloc(sizeof(*positions) * cap);
    int fast_path = ids_iter(&qmap->linked) == NULL && head->phd == hd;

    if (cur == QM_MISS)
      return;

    CBUG(!positions, "malloc error (del_all)\n");

    while (qmap_lnext(&sn, cur))
      positions[n++] = sn;

    if (fast_path) {
      for (size_t i = 0; i < n; i++) {
        uint32_t pos = positions[i];
        const void *old_key = qmap_key(hd, pos);

        qmap_payload_free(qmap, (void *) old_key);
        qmap->key_sizes[pos] = 0;
        qmap->val_sizes[pos] = 0;
        qmap->omap[pos] = NULL;
        * VAL_ADDR(qmap, pos) = NULL;
        idm_del(&qmap->idm, pos);
        head->n--;
      }

      head->iflags |= QM_SDIRTY;

      if (head->n == 0)
        memset(qmap->map, 0xFF, sizeof(uint32_t) * head->m);
      else
        qmap_rebuild_map(hd);
    } else {
      for (size_t i = 0; i < n; i++)
        qmap_ndel(hd, positions[i]);
    }

    free(positions);
  } else {
    /* For regular maps, just call qmap_del once */
    qmap_del(hd, key);
  }
}

/* }}} */

/* ITERATION {{{ */

  void /* API */
qmap_fin(uint32_t cur_id)
{
  qmap_cur_t *cursor = &qmap_cursors[cur_id];

  if (cursor->sub_cur)
    qmap_fin(cursor->sub_cur);

  idm_del(&cursor_idm, cur_id);
}

  uint32_t /* API */
qmap_iter(uint32_t hd, const void * const key, uint32_t flags)
{
  qmap_head_t *head = &qmap_heads[hd];
  qmap_t *qmap = &qmaps[hd];
  uint32_t cur_id = idm_new(&cursor_idm);
  qmap_cur_t *cursor = &qmap_cursors[cur_id];

  if (key && (head->flags & QM_MULTIVALUE)) {
    /* For QM_MULTIVALUE maps, use sorted iteration to find all duplicates.
     * Find first occurrence of this key */
    int first = qmap_bsearch_ex(hd, key, NULL, QMAP_BSEARCH_FIRST);
    cursor->pos = (first != -1) ? (uint32_t)first : head->sorted_n;
    cursor->end_pos = QM_MISS;
    /* Use sorted iteration but stop at key boundary */
    flags |= QM_RANGE;
  } else if (key && (flags & QM_RANGE) && (head->flags & QM_SORTED)) {
    int exact;
    cursor->pos = qmap_bsearch(hd, key, &exact);
    cursor->end_pos = head->sorted_n;
  } else if (key && !(flags & QM_RANGE)) {
    uint32_t id = qmap_id(hd, key);
    if (id == QM_MISS) {
      cursor->pos = QM_MISS;
      cursor->end_pos = QM_MISS;
    } else {
      cursor->pos = qmap->map[id];
      cursor->end_pos = cursor->pos;
    }
  } else
    cursor->pos = cursor->end_pos = 0;

  cursor->ipos = cursor->pos;
  cursor->sub_cur = 0;
  cursor->hd = hd;
  cursor->key = key;
  cursor->key_len = key ? qmap_len(head->types[QM_KEY], key) : 0;
  cursor->flags = flags;
  return cur_id;
}

/* low-level next */
  static int
qmap_lnext(uint32_t *sn, uint32_t cur_id)
{
  register qmap_cur_t *cursor
    = &qmap_cursors[cur_id];
  register qmap_head_t *head = &qmap_heads[cursor->hd];
  register qmap_t *qmap = &qmaps[cursor->hd];
  uint32_t n;
  const void *key;

  if ((cursor->flags & QM_RANGE)
      && (head->flags & QM_SORTED))
  {
    if (head->iflags & QM_SDIRTY)
      qmap_rebuild_sorted(cursor->hd);

    if (cursor->pos >= head->sorted_n)
      goto end;

    n = qmap->sorted_idx[cursor->pos];

    if (cursor->key && (head->flags & QM_MULTIVALUE)) {
      if (cursor->end_pos == QM_MISS)
        cursor->end_pos = (uint32_t) qmap_bsearch_ex(
            cursor->hd, cursor->key, NULL,
            QMAP_BSEARCH_LAST);
      if (cursor->pos > cursor->end_pos)
        goto end;
    }

    *sn = n;
    cursor->pos++;
    return 1;
  }

cagain:
  n = cursor->pos;

  if (n >= qmap->idm.last)
    goto end;

  key = qmap_key(cursor->hd, n);
  if (key == NULL) {
    cursor->pos++;
    goto cagain;
  }

  if (cursor->flags & QM_RANGE) {
    if (!cursor->key)
      goto next;

    qmap_type_t *type = &qmap_types[head->types[QM_KEY]];
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
  *sn = QM_MISS;
  return 0;
}

  int /* API */
qmap_next(const void ** ckey, const void ** cval,
    uint32_t cur_id)
{
  register qmap_cur_t *c;
  uint32_t sn;
  int ret = qmap_lnext(&sn, cur_id);

  if (!ret)
    return 0;

  c = &qmap_cursors[cur_id];
  if (ckey)
    *ckey = qmap_key(c->hd, sn);
  if (cval)
    *cval = qmap_val(c->hd, sn);
  return 1;
}

/* }}} */

/* DROP + CLOSE + OTHERS {{{ */

  void /* API */
qmap_drop(uint32_t hd)
{
  qmap_t *qmap = &qmaps[hd];

  if (ids_iter(&qmap->linked) == NULL) {
    qmap_clear_fast(hd);
    return;
  }

  uint32_t cur_id = qmap_iter(hd, NULL, 0), sn;

  while (qmap_lnext(&sn, cur_id))
    qmap_ndel(hd, sn);
}

  void /* API */
qmap_close(uint32_t hd)
{
  qmap_head_t *head = &qmap_heads[hd];
  qmap_t *qmap = &qmaps[hd];
  idsi_t *cur;
  uint32_t ahd;

  if (!qmap->omap)
    return;

  qmap_drop(hd);

  cur = ids_iter(&qmap->linked);
  while (ids_next(&ahd, &cur))
    qmap_close(ahd);

  ids_drop(&qmap->linked);

  /* Close inverse index maps */
  if (head->inv_hds) {
    uint32_t fc = head->record_id > 0 && head->record_id < qmap_records_n
                ? (uint32_t)qmap_records[head->record_id].field_count : 0;
    for (uint32_t i = 0; i < fc; i++) {
      if (head->inv_hds[i])
        qmap_close(head->inv_hds[i]);
    }
    free(head->inv_hds);
    head->inv_hds = NULL;
  }

  /* Close variable-length string map */
  if (head->vstr_hd) {
    qmap_close(head->vstr_hd);
    head->vstr_hd = 0;
  }

  idm_drop(&qmap->idm);
  qmap->idm.last = 0;
  qmap_payload_flush(qmap);
  free(qmap->map);
  free(qmap->omap);
  free(qmap->key_hashes);
  free(qmap->key_sizes);
  free(qmap->val_sizes);
  if (qmap->sorted_idx)
    free(qmap->sorted_idx);
  if (qmap_heads[hd].phd == hd)
    free(qmap->table);
  qmap->omap = NULL;
  idm_del(&idm, hd);

  // remove any file associations so we don't try
  // saving it to a file after it is closed.
  if (!head->file)
    return;
  const qmap_file_t *file = qmap_get(qmap_files_hd, head->file);
  if (!file)
    return;
  ids_remove((ids_t *) &file->ids, hd);

}

  void /* API */
qmap_assoc(uint32_t hd, uint32_t link, qmap_assoc_t cb, void *userdata)
{
  qmap_t *qmap = &qmaps[hd];

  if (!cb)
    cb = qmap_rassoc;

  ids_push(&qmaps[link].linked, hd);

  qmap->assoc = cb;
  qmap->assoc_userdata = userdata;
  qmap_heads[hd].phd = link;

  free(qmap->table);
  qmap->table = NULL;

  if (qmap_heads[link].n > 0) {
    uint32_t cur = qmap_iter(link, NULL, 0);
    const void *key, *value;

    while (qmap_next(&key, &value, cur)) {
      const void *skey;
      qmap->assoc(&skey, key, value, qmap->assoc_userdata);
      _qmap_put(hd, skey, value, QM_MISS);
    }
    qmap_fin(cur);
  }
}

  void /* API */
qmap_assoc_multi(uint32_t hd, uint32_t link, qmap_assoc_multi_t cb, void *userdata)
{
  qmap_t *qmap = &qmaps[hd];

  if (!cb)
    return;

  ids_push(&qmaps[link].linked, hd);

  qmap->m_assoc = cb;
  qmap->m_assoc_userdata = userdata;
  qmap_heads[hd].phd = link;

  /* Backfill existing entries in the primary */
  if (qmap_heads[link].n > 0) {
    uint32_t cur = qmap_iter(link, NULL, 0);
    const void *key, *value;

    while (qmap_next(&key, &value, cur)) {
      const void *skeys[64];
      size_t nkeys = qmap->m_assoc(skeys, 64, key, value, qmap->m_assoc_userdata);
      for (size_t i = 0; i < nkeys; i++) {
        _qmap_put(hd, skeys[i], key, QM_MISS);
        free((void *)skeys[i]);
      }
    }
    qmap_fin(cur);
  }
}

  uint32_t /* API */
qmap_reg(size_t len)
{
  if (types_n > TYPES_MASK) {
    fprintf(stderr, "qmap_reg: type limit reached\n");
    return QM_MISS;
  }
  uint32_t id = types_n ++;
  qmap_type_t *type = &qmap_types[id];

  memset(type, 0, sizeof(qmap_type_t));
  type->len = len;
  type->hash = qmap_chash;
  type->cmp = qmap_ccmp;
  return id;
}

  void
qmap_cmp_set(uint32_t ref, qmap_cmp_t *cmp)
{
  qmap_type_t *type = &qmap_types[ref];
  type->cmp = cmp;
}

  uint32_t /* API */
qmap_mreg(qmap_measure_t *measure)
{
  if (types_n > TYPES_MASK) {
    fprintf(stderr, "qmap_mreg: type limit reached\n");
    return QM_MISS;
  }
  uint32_t id = types_n ++;
  qmap_type_t *type = &qmap_types[id];

  memset(type, 0, sizeof(qmap_type_t));
  type->measure = measure;
  type->hash = qmap_chash;
  type->cmp = qmap_ccmp;
  type->len = 0;
  return id;
}

  size_t /* API */
qmap_len(uint32_t type_id, const void *key)
{
  qmap_type_t *type = &qmap_types[type_id];

  return type->measure
    ? type->measure(key)
    : type->len;
}

/* }}} */

/* RECORD-AWARE MAP SUPPORT {{{ */

  uint32_t /* API */
qmap_record_register(const char *name, size_t struct_size,
    const qmap_record_field_t *fields, size_t field_count)
{
  if (qmap_records_n >= QMAP_MAX_RECORDS) {
    fprintf(stderr, "qmap_record_register: record limit reached\n");
    return QM_MISS;
  }
  if (struct_size == 0) {
    fprintf(stderr, "qmap_record_register: struct_size must be > 0\n");
    return QM_MISS;
  }
  if (!fields || field_count == 0 || field_count > QMAP_MAX_RECORD_FIELDS) {
    fprintf(stderr, "qmap_record_register: invalid fields\n");
    return QM_MISS;
  }

  uint32_t struct_type_id = qmap_reg(struct_size);
  if (struct_type_id == QM_MISS) {
    fprintf(stderr, "qmap_record_register: qmap_reg failed\n");
    return QM_MISS;
  }

  uint32_t id = ++qmap_records_n;
  qmap_record_t *rec = &qmap_records[id];

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
qmap_record_type_id(uint32_t record_id)
{
  if (!record_id || record_id > qmap_records_n)
    return QM_MISS;
  return qmap_records[record_id].struct_type_id;
}

  void /* API */
qmap_record_field_set_target_hd(uint32_t record_id,
    const char *field_name,
    uint32_t target_hd)
{
  if (!record_id || record_id > qmap_records_n) return;
  int fi = qmap_record_find_field(record_id, field_name);
  if (fi < 0) return;
  qmap_records[record_id].fields[fi].target_hd = target_hd;
}

  uint32_t /* API */
qmap_field_put(uint32_t hd, const char *item_id,
    const char *field_name, const char *value)
{
  if (!value) return QM_MISS;
  qmap_head_t *head = &qmap_heads[hd];
  if (head->record_id == 0) return QM_MISS;
  int fi = qmap_record_find_field(head->record_id, field_name);
  if (fi < 0) return QM_MISS;
  uint32_t ft = qmap_records[head->record_id].fields[fi].type;

  char key[256];
  size_t ilen = strlen(item_id);
  size_t flen = strlen(field_name);
  if (ilen + 1 + flen >= sizeof(key)) return QM_MISS;
  memcpy(key, item_id, ilen);
  key[ilen] = ':';
  memcpy(key + ilen + 1, field_name, flen);
  key[ilen + 1 + flen] = '\0';

  if (ft == QM_REFERENCE) {
    uint32_t thd = qmap_records[head->record_id].fields[fi].target_hd;
    if (thd == 0)
      return QM_MISS;
    if (!value || ((const char *)value)[0] == '\0')
      return QM_MISS;
    uint32_t pos = qmap_pos(thd, value);
    if (pos == UINT32_MAX)
      return QM_MISS;
    return qmap_put(hd, key, value);
  }

	if (ft == QM_MULTI_REFERENCE) {
	    uint32_t thd = qmap_records[head->record_id].fields[fi].target_hd;
	    if (!thd) return qmap_put(hd, key, value);

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
	        uint32_t pos = qmap_pos(thd, id);
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
	    return qmap_put(hd, key, resolved);
	  }

  return qmap_put(hd, key, value);
}

  const char * /* API */
qmap_field_get(uint32_t hd, const char *item_id,
    const char *field_name)
{
  if (!item_id || !field_name) return NULL;
  qmap_head_t *head = &qmap_heads[hd];
  if (head->record_id == 0) return NULL;
  int fi = qmap_record_find_field(head->record_id, field_name);
  if (fi < 0) return NULL;
  uint32_t ft = qmap_records[head->record_id].fields[fi].type;

  if (ft == QM_VSTR) {
    if (head->vstr_hd == 0) return NULL;
    char key[256];
    size_t ilen = strlen(item_id);
    size_t flen = strlen(field_name);
    if (ilen + 1 + flen >= sizeof(key)) return NULL;
    memcpy(key, item_id, ilen);
    key[ilen] = ':';
    memcpy(key + ilen + 1, field_name, flen);
    key[ilen + 1 + flen] = '\0';
    return qmap_get(head->vstr_hd, key);
  }

  const void *struct_ptr = qmap_get(hd, item_id);
  if (!struct_ptr) return NULL;

  size_t field_offset = qmap_records[head->record_id].fields[fi].offset;
  const char *result = (const char *)struct_ptr + field_offset;
  if (ft == QM_REFERENCE && (!result || result[0] == '\0'))
    return NULL;
  return result;
}

  const char * /* API */
qmap_get_key(uint32_t hd, uint32_t pos)
{
  qmap_t *qmap = &qmaps[hd];
  if (pos >= qmap->idm.last)
    return NULL;
  const void *key = qmap->omap[pos];
  return key ? (const char *)key : NULL;
}

  uint32_t /* API */
qmap_pos(uint32_t hd, const char *key)
{
  if (!key)
    return UINT32_MAX;
  uint32_t id = qmap_id(hd, key);
  if (id == QM_MISS)
    return UINT32_MAX;
  uint32_t n = qmaps[hd].map[id];
  return (n == QM_MISS) ? UINT32_MAX : n;
}

  size_t /* API */
qmap_inv_get(uint32_t hd, const char *field_name,
    uint32_t target_pos,
    uint32_t *out, size_t max)
{
  qmap_head_t *head = &qmap_heads[hd];
  if (!head->inv_hds || head->record_id == 0 || !field_name)
    return 0;

  int fi = qmap_record_find_field(head->record_id, field_name);
  if (fi < 0)
    return 0;

  qmap_record_t *rec = &qmap_records[head->record_id];
  if (rec->fields[fi].target_record == 0)
    return 0;

  uint32_t inv_hd = head->inv_hds[fi];
  if (inv_hd == 0)
    return 0;

  const char *val = qmap_get(inv_hd, &target_pos);
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
_qmap_load(uint32_t hd, const char *mmaped, uint32_t dbid)
{
  const char *mm = mmaped;
  const char *mm_start = mmaped;

  uint32_t lid = * (uint32_t *) mm;
  mm += sizeof(uint32_t);

  size_t size = * (size_t *) mm;
  mm += sizeof(size_t);

  if (dbid != QM_MISS && lid != dbid)
    return mm + size + sizeof(uint32_t) - mm_start;

  uint32_t amount = * (uint32_t*) mm;
  mm += sizeof(uint32_t);

  qmap_head_t *head = &qmap_heads[hd];
  uint32_t ktype = head->types[QM_KEY];
  uint32_t vtype = head->types[QM_VALUE];

  for (uint32_t i = 0; i < amount; i++) {
    size_t klen = qmap_len(ktype, mm);
    const char *mval = mm + klen;
    size_t vlen = qmap_len(vtype, mval);

    qmap_put(hd, mm, mval);
    mm = mval + vlen;
  }

  head->iflags |= QM_SDIRTY;
  return mm - mm_start;
}

  static inline void
qmap_load_file(char *filename, uint32_t dbid)
{
  qmap_file_t *file = (qmap_file_t *)
    qmap_get(qmap_files_hd, filename);

  struct stat sb;
  char *mm;
  idsi_t *cur;
  uint32_t hd;

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
  cur = (idsi_t *) ids_iter(&file->ids);

  mm = file->mmaped;
  while (ids_next(&hd, &cur))
    mm += _qmap_load(hd, mm, dbid);
}

  static size_t
_qmap_calc_size(uint32_t hd)
{
  qmap_head_t *head = &qmap_heads[hd];
  uint32_t ktype = head->types[QM_KEY];
  uint32_t vtype = head->types[QM_VALUE];
  size_t total_size = sizeof(uint32_t) + sizeof(size_t) + sizeof(head->n);
  uint32_t cur = qmap_iter(hd, NULL, 0);
  const void *key, *value;

  while (qmap_next(&key, &value, cur)) {
    total_size += qmap_len(ktype, key);
    total_size += qmap_len(vtype, value);
  }

  qmap_fin(cur);
  return total_size;
}

  static inline size_t
qmap_calc_file_size(const ids_t *hds)
{
  size_t size = 0;
  idsi_t *cur = (idsi_t *) ids_iter((ids_t *) hds);
  uint32_t hd;

  while (ids_next(&hd, &cur))
    if (mdbs[hd])
      size += _qmap_calc_size(hd);

  return size;
}

  inline static size_t
_qmap_save(void *mmaped, uint32_t hd)
{
  qmap_head_t *head = &qmap_heads[hd];
  uint32_t ktype = head->types[QM_KEY];
  uint32_t vtype = head->types[QM_VALUE];
  char *mm_start = mmaped;
  char *mm = mmaped;
  uint32_t cur = qmap_iter(hd, NULL, 0);
  const void *key, *value;

  memcpy(mm, &head->dbid, sizeof(head->dbid));
  mm += sizeof(head->dbid);

  size_t size = _qmap_calc_size(hd);
  memcpy(mm, &size, sizeof(size));
  mm += sizeof(size);

  memcpy(mm, &head->n, sizeof(head->n));
  mm += sizeof(head->n);

  while (qmap_next(&key, &value, cur)) {
    size_t klen = qmap_len(ktype, key);
    size_t vlen = qmap_len(vtype, value);

    memcpy(mm, key, klen);
    mm += klen;
    memcpy(mm, value, vlen);
    mm += vlen;
  }

  qmap_fin(cur);
  return mm - mm_start;
}

  static inline void
qmap_save_file(char *filename)
{
  qmap_file_t *file = (qmap_file_t *) qmap_get(qmap_files_hd, filename);
  CBUG(!file, "called with unknown filename");

  if (file->mmaped)
    file_close(file);

  file->size = qmap_calc_file_size(&file->ids);

  file->fd = open(filename, O_RDWR | O_CREAT,
      S_IRUSR | S_IWUSR);

  CBUG(file->fd == -1, "open for save failed");

  CBUG(ftruncate(file->fd, (off_t) file->size) == -1,
      "ftruncate failed");

  if (file->size == 0) {
    close(file->fd);
    file->fd = -1;
    return;
  }

  file->mmaped = (char*) mmap(NULL, file->size,
      PROT_WRITE, MAP_SHARED, file->fd, 0);
  CBUG(file->mmaped == MAP_FAILED, "mmap for save failed");

  char *mm = file->mmaped;
  uint32_t hd;

  idsi_t *idsi = ids_iter(&file->ids);

  while (ids_next(&hd, &idsi)) {
    if (!mdbs[hd])
      continue;

    size_t size_written = _qmap_save(mm, hd);
    mm += size_written;
  }

  file_close(file);
}

  void /* API */
qmap_save(void)
{
  uint32_t c = qmap_iter(qmap_files_hd, NULL, 0);
  const void *key, *value;

  while (qmap_next(&key, &value, c))
    qmap_save_file((char *) key);
}

/* MULTI-VALUE API {{{ */

  uint32_t /* API */
qmap_get_multi(uint32_t hd, const void *key)
{
  qmap_head_t *head = &qmap_heads[hd];
  uint32_t cur = qmap_iter(hd, key, 0);

  if (!key)
    return cur;

  /* qmap_iter() already did the lookup. Just verify that it landed on a
   * real entry before returning the cursor to the caller. */
  if (head->flags & QM_MULTIVALUE) {
    if (qmap_cursors[cur].pos >= head->sorted_n) {
      qmap_fin(cur);
      return QM_MISS;
    }
  } else if (qmap_cursors[cur].pos == QM_MISS) {
    qmap_fin(cur);
    return QM_MISS;
  }

  return cur;
}

  uint32_t /* API */
qmap_count(uint32_t hd, const void *key)
{
  qmap_head_t *head = &qmap_heads[hd];

  if (key == NULL) {
    /* Count total entries in map */
    return head->n;
  }

  /* For non-multivalue maps, return 0 or 1 */
  if (!(head->flags & QM_MULTIVALUE)) {
    return qmap_get(hd, key) != NULL ? 1 : 0;
  }

  /* For multivalue maps, use binary search to find first and last
   * occurrences. This keeps the count path logarithmic even when the
   * duplicate run is large. */
  int first = qmap_bsearch_ex(hd, key, NULL, QMAP_BSEARCH_FIRST);
  if (first == -1)
    return 0;

  int last = qmap_bsearch_ex(hd, key, NULL, QMAP_BSEARCH_LAST);
  return (uint32_t)(last - first + 1);
}

/* }}} */
