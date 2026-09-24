/* Test with exact field layout from real song registration */
#include "./../include/ttypt/corm.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <stddef.h>

typedef struct {
  char id[128];
  char title[256];
  char type[2048];
  char author[256];
  char yt[512];
  char audio[512];
  char pdf[512];
  char owner[32];
} song_cache_t;

typedef struct {
  char name[64];
  char songs[2048];
} song_type_cache_t;

static void hex_dump(const char *label, const void *ptr, size_t n)
{
  const unsigned char *p = (const unsigned char *)ptr;
  fprintf(stderr, "%s:", label);
  for (size_t i = 0; i < n; i++)
    fprintf(stderr, " %02x", p[i]);
  fprintf(stderr, "\n");
}

#define SONG_CACHE_SIZE sizeof(song_cache_t)
#define EXPECT_OK(got, expected, field) do { \
  fprintf(stderr, "%s: got='%s' expected='%s' %s\n", field, \
          (got) ? (got) : "(NULL)", expected, \
          ((got) && strcmp(got, expected) == 0) ? "OK" : "FAIL"); \
  if (!(got) || strcmp(got, expected) != 0) { errors++; hex_dump("  bytes", got, 32); } \
} while(0)

int main(void)
{
  int errors = 0;

  fprintf(stderr, "sizeof(song_cache_t) = %zu\n", SONG_CACHE_SIZE);

  /* Register song_type record first */
  corm_record_field_t type_fields[] = {
    { "name",  CM_STR, offsetof(song_type_cache_t, name),
      sizeof(((song_type_cache_t*)0)->name) },
    { "songs", CM_MULTI_REFERENCE, offsetof(song_type_cache_t, songs),
      sizeof(((song_type_cache_t*)0)->songs) },
  };
  uint32_t type_rec = corm_record_register("song_type",
      sizeof(song_type_cache_t), type_fields, 2);

  /* Register song record — match real app field list exactly:
   * id, title, type(MULTI_REF), author, yt, audio, pdf, data(VSTR), owner
   * Target record must be set BEFORE registration. */
  corm_record_field_t song_fields[] = {
    { "id",     CM_STR, offsetof(song_cache_t, id),
      sizeof(((song_cache_t*)0)->id) },
    { "title",  CM_STR, offsetof(song_cache_t, title),
      sizeof(((song_cache_t*)0)->title) },
    { "type",   CM_MULTI_REFERENCE, offsetof(song_cache_t, type),
      sizeof(((song_cache_t*)0)->type), .target_record = 0 },
    { "author", CM_STR, offsetof(song_cache_t, author),
      sizeof(((song_cache_t*)0)->author) },
    { "yt",     CM_STR, offsetof(song_cache_t, yt),
      sizeof(((song_cache_t*)0)->yt) },
    { "audio",  CM_STR, offsetof(song_cache_t, audio),
      sizeof(((song_cache_t*)0)->audio) },
    { "pdf",    CM_STR, offsetof(song_cache_t, pdf),
      sizeof(((song_cache_t*)0)->pdf) },
    { "data",   0, 0, 0, .type = 1 /*CM_VSTR placeholder for source_def_to_corm */ },
    { "owner",  CM_STR, offsetof(song_cache_t, owner),
      sizeof(((song_cache_t*)0)->owner) },
  };
  /* Set target_record for the type field */
  song_fields[2].target_record = type_rec;

  uint32_t song_rec = corm_record_register("song",
      SONG_CACHE_SIZE, song_fields, 9);

  /* Open type maps */
  uint32_t type_fields_hd = corm_open(NULL, NULL, CM_STR,
      corm_record_type_id(type_rec), 0x3FF,
      CM_RECORD(type_rec) | CM_SORTED);

  /* Open song maps — fields_hd with CM_RECORD + CM_SORTED */
  uint32_t song_fields_hd = corm_open(NULL, NULL, CM_STR,
      corm_record_type_id(song_rec), 0x3FF,
      CM_RECORD(song_rec) | CM_SORTED);

  corm_record_field_set_target_hd(song_rec, "type", type_fields_hd);

  fprintf(stderr, "song_rec=%u song_fields_hd=%u\n", song_rec, song_fields_hd);

  /* Put type entries first */
  song_type_cache_t tp = { .name = "hymn" };
  corm_put(type_fields_hd, "hymn", &tp);
  strcpy(tp.name, "worship");
  corm_put(type_fields_hd, "worship", &tp);

  /* Reproduce exact source_scan_item sequence for "amazing_grace" */
  fprintf(stderr, "\n=== Reproduce source_scan_item ===\n");

  /* Step 1: id field */
  corm_field_put(song_fields_hd, "amazing_grace", "id", "amazing_grace");
  const char *v = corm_field_get(song_fields_hd, "amazing_grace", "id");
  fprintf(stderr, "after id put: verify='%s' %s\n",
          v ? v : "(NULL)", (v && strcmp(v, "amazing_grace") == 0) ? "OK" : "FAIL");

  /* Step 2: title field */
  corm_field_put(song_fields_hd, "amazing_grace", "title", "Amazing Grace");
  v = corm_field_get(song_fields_hd, "amazing_grace", "title");
  fprintf(stderr, "after title put: verify='%s' %s\n",
          v ? v : "(NULL)", (v && strcmp(v, "Amazing Grace") == 0) ? "OK" : "FAIL");
  if (!v || strcmp(v, "Amazing Grace") != 0) {
    fprintf(stderr, "  TRUNCATION DETECTED!\n");
    hex_dump("  bytes", v, 32);
    errors++;
  }

  /* Step 3: type field (MULTI_REF) */
  corm_field_put(song_fields_hd, "amazing_grace", "type", "hymn");
  v = corm_field_get(song_fields_hd, "amazing_grace", "type");
  fprintf(stderr, "after type put: verify='%s' %s\n",
          v ? v : "(NULL)", (v && strlen(v) > 0) ? "OK" : "FAIL");

  /* Step 4: author field */
  corm_field_put(song_fields_hd, "amazing_grace", "author", "John Newton");
  v = corm_field_get(song_fields_hd, "amazing_grace", "author");
  fprintf(stderr, "after author put: verify='%s' %s\n",
          v ? v : "(NULL)", (v && strcmp(v, "John Newton") == 0) ? "OK" : "FAIL");
  if (!v || strcmp(v, "John Newton") != 0) {
    fprintf(stderr, "  TRUNCATION DETECTED!\n");
    errors++;
  }

  /* Re-check title after all puts */
  v = corm_field_get(song_fields_hd, "amazing_grace", "title");
  fprintf(stderr, "final title check: '%s' %s\n",
          v ? v : "(NULL)", (v && strcmp(v, "Amazing Grace") == 0) ? "OK" : "FAIL");
  if (!v || strcmp(v, "Amazing Grace") != 0) {
    fprintf(stderr, "  TRUNCATION DETECTED!\n");
    hex_dump("  bytes", v, 32);
    errors++;
  }

  /* Test 2: second item */
  fprintf(stderr, "\n=== Second item ===\n");
  corm_field_put(song_fields_hd, "how_great", "id", "how_great");
  corm_field_put(song_fields_hd, "how_great", "title", "How Great Thou Art");
  v = corm_field_get(song_fields_hd, "how_great", "title");
  fprintf(stderr, "title: '%s' %s\n",
          v ? v : "(NULL)", (v && strcmp(v, "How Great Thou Art") == 0) ? "OK" : "FAIL");
  if (!v || strcmp(v, "How Great Thou Art") != 0) errors++;

  /* Re-check first item again */
  v = corm_field_get(song_fields_hd, "amazing_grace", "title");
  fprintf(stderr, "re-check first: '%s' %s\n",
          v ? v : "(NULL)", (v && strcmp(v, "Amazing Grace") == 0) ? "OK" : "FAIL");
  if (!v || strcmp(v, "Amazing Grace") != 0) {
    fprintf(stderr, "  FIRST ITEM CORRUPTED AFTER SECOND INSERT!\n");
    errors++;
  }

  fprintf(stderr, "\n");
  if (errors == 0)
    printf("ALL TESTS PASSED\n");
  else
    printf("%u TEST(S) FAILED\n", errors);

  corm_close(song_fields_hd);
  corm_close(type_fields_hd);
  return errors == 0 ? 0 : 1;
}
