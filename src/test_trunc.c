#include "./../include/ttypt/corm.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <stddef.h>

/* Reproduce the bug: struct > CORM_POOL_MAX (4096).
 * song_cache_t is 4256 bytes. We replicate that. */
typedef struct {
  char id[128];
  char title[256];
  char type[2048];
  char author[256];
  char yt[512];
  char audio[512];
  char pdf[512];
  char owner[32];
} big_cache_t;

static void hex_dump(const char *label, const void *ptr, size_t n)
{
  const unsigned char *p = (const unsigned char *)ptr;
  fprintf(stderr, "%s:", label);
  for (size_t i = 0; i < n; i++)
    fprintf(stderr, " %02x", p[i]);
  fprintf(stderr, "\n");
}

int main(void)
{
  int errors = 0;

  fprintf(stderr, "sizeof(big_cache_t) = %zu\n", sizeof(big_cache_t));
  fprintf(stderr, "CORM_POOL_MAX = 4096\n");
  fprintf(stderr, "Struct %s CORM_POOL_MAX\n",
          sizeof(big_cache_t) > 4096 ? "EXCEEDS" : "fits within");

  corm_record_field_t fields[] = {
    { "id",     CM_STR, offsetof(big_cache_t, id),     sizeof(((big_cache_t*)0)->id)     },
    { "title",  CM_STR, offsetof(big_cache_t, title),  sizeof(((big_cache_t*)0)->title)  },
    { "author", CM_STR, offsetof(big_cache_t, author), sizeof(((big_cache_t*)0)->author) },
  };
  uint32_t rec = corm_record_register("big_test", sizeof(big_cache_t), fields, 3);
  uint32_t hd = corm_open(NULL, NULL, CM_STR, corm_record_type_id(rec),
                          0xFF, CM_RECORD(rec));
  fprintf(stderr, "rec=%u hd=%u\n", rec, hd);

  /* Test 1: corm_field_put then corm_field_get — the exact failing path */
  fprintf(stderr, "\n=== Test 1: field_put + field_get (reproduces bug) ===\n");
  corm_field_put(hd, "amazing_grace", "id", "amazing_grace");
  corm_field_put(hd, "amazing_grace", "title", "Amazing Grace");
  corm_field_put(hd, "amazing_grace", "author", "John Newton");

  const char *got_id = corm_field_get(hd, "amazing_grace", "id");
  const char *got_title = corm_field_get(hd, "amazing_grace", "title");
  const char *got_author = corm_field_get(hd, "amazing_grace", "author");

  fprintf(stderr, "id:     got='%s' expected='amazing_grace' %s\n",
          got_id ? got_id : "(NULL)",
          (got_id && strcmp(got_id, "amazing_grace") == 0) ? "OK" : "FAIL");
  fprintf(stderr, "title:  got='%s' expected='Amazing Grace' %s\n",
          got_title ? got_title : "(NULL)",
          (got_title && strcmp(got_title, "Amazing Grace") == 0) ? "OK" : "FAIL");
  fprintf(stderr, "author: got='%s' expected='John Newton' %s\n",
          got_author ? got_author : "(NULL)",
          (got_author && strcmp(got_author, "John Newton") == 0) ? "OK" : "FAIL");

  if (!got_id || strcmp(got_id, "amazing_grace") != 0) {
    fprintf(stderr, "  FAIL: id mismatch\n");
    if (got_id) hex_dump("  id_bytes", got_id, 16);
    errors++;
  }
  if (!got_title || strcmp(got_title, "Amazing Grace") != 0) {
    fprintf(stderr, "  FAIL: title mismatch\n");
    if (got_title) hex_dump("  title_bytes", got_title, 16);
    errors++;
  }
  if (!got_author || strcmp(got_author, "John Newton") != 0) {
    fprintf(stderr, "  FAIL: author mismatch\n");
    if (got_author) hex_dump("  author_bytes", got_author, 16);
    errors++;
  }

  /* Test 2: whole-struct put then field_get */
  fprintf(stderr, "\n=== Test 2: whole-struct put then field_get ===\n");
  big_cache_t s2;
  memset(&s2, 0, sizeof(s2));
  strcpy(s2.id, "hymn2");
  strcpy(s2.title, "How Great Thou Art");
  strcpy(s2.author, "Carl Boberg");
  corm_put(hd, "hymn2", &s2);

  got_title = corm_get(hd, "hymn2:title");
  fprintf(stderr, "title:  got='%s' expected='How Great Thou Art' %s\n",
          got_title ? got_title : "(NULL)",
          (got_title && strcmp(got_title, "How Great Thou Art") == 0) ? "OK" : "FAIL");
  if (!got_title || strcmp(got_title, "How Great Thou Art") != 0) {
    fprintf(stderr, "  FAIL: whole-struct title mismatch\n");
    if (got_title) hex_dump("  title_bytes", got_title, 16);
    errors++;
  }

  /* Test 3: field_put then overwrite, then field_get */
  fprintf(stderr, "\n=== Test 3: overwrite field twice ===\n");
  corm_field_put(hd, "hymn2", "title", "How Great Thou Art v2");
  got_title = corm_field_get(hd, "hymn2", "title");
  fprintf(stderr, "title:  got='%s' expected='How Great Thou Art v2' %s\n",
          got_title ? got_title : "(NULL)",
          (got_title && strcmp(got_title, "How Great Thou Art v2") == 0) ? "OK" : "FAIL");
  if (!got_title || strcmp(got_title, "How Great Thou Art v2") != 0) {
    fprintf(stderr, "  FAIL: overwrite mismatch\n");
    errors++;
  }

  /* Test 4: raw corm_put/get with composite key (bypass field_put) */
  fprintf(stderr, "\n=== Test 4: raw corm_put with composite key ===\n");
  corm_put(hd, "raw_test:title", "Raw Title Value");
  got_title = corm_get(hd, "raw_test:title");
  fprintf(stderr, "title:  got='%s' expected='Raw Title Value' %s\n",
          got_title ? got_title : "(NULL)",
          (got_title && strcmp(got_title, "Raw Title Value") == 0) ? "OK" : "FAIL");
  if (!got_title || strcmp(got_title, "Raw Title Value") != 0) {
    fprintf(stderr, "  FAIL: raw composite put mismatch\n");
    if (got_title) hex_dump("  title_bytes", got_title, 32);
    errors++;
  }

  fprintf(stderr, "\n=== Test 5: field_put on empty map (create + fill) ===\n");
  corm_field_put(hd, "new_item", "id", "new_item");
  corm_field_put(hd, "new_item", "title", "Brand New Song");
  got_title = corm_field_get(hd, "new_item", "title");
  fprintf(stderr, "title:  got='%s' expected='Brand New Song' %s\n",
          got_title ? got_title : "(NULL)",
          (got_title && strcmp(got_title, "Brand New Song") == 0) ? "OK" : "FAIL");
  if (!got_title || strcmp(got_title, "Brand New Song") != 0) {
    fprintf(stderr, "  FAIL: create+fill mismatch\n");
    errors++;
  }

  fprintf(stderr, "\n");
  if (errors == 0)
    printf("ALL TRUNCATION TESTS PASSED\n");
  else
    printf("%u TRUNCATION TEST(S) FAILED\n", errors);

  corm_close(hd);
  return errors == 0 ? 0 : 1;
}
