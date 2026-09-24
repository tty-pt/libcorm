/* Test closer to real app setup: multiple record types, associations,
 * CM_SORTED on row_hd, etc. */
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

int main(void)
{
  int errors = 0;

  fprintf(stderr, "sizeof(song_cache_t) = %zu\n", sizeof(song_cache_t));
  fprintf(stderr, "sizeof(song_type_cache_t) = %zu\n", sizeof(song_type_cache_t));

  /* Register song_type record first (as the real app does) */
  corm_record_field_t type_fields[] = {
    { "name",  CM_STR, offsetof(song_type_cache_t, name),
      sizeof(((song_type_cache_t*)0)->name) },
    { "songs", CM_MULTI_REFERENCE, offsetof(song_type_cache_t, songs),
      sizeof(((song_type_cache_t*)0)->songs) },
  };
  uint32_t type_rec = corm_record_register("song_type",
      sizeof(song_type_cache_t), type_fields, 2);
  fprintf(stderr, "type_rec=%u\n", type_rec);

  /* Register song record */
  corm_record_field_t song_fields[] = {
    { "id",     CM_STR, offsetof(song_cache_t, id),
      sizeof(((song_cache_t*)0)->id) },
    { "title",  CM_STR, offsetof(song_cache_t, title),
      sizeof(((song_cache_t*)0)->title) },
    { "type",   CM_MULTI_REFERENCE, offsetof(song_cache_t, type),
      sizeof(((song_cache_t*)0)->type) },
    { "author", CM_STR, offsetof(song_cache_t, author),
      sizeof(((song_cache_t*)0)->author) },
  };
  uint32_t song_rec = corm_record_register("song",
      sizeof(song_cache_t), song_fields, 4);
  fprintf(stderr, "song_rec=%u\n", song_rec);

  /* Set target_record for the type field BEFORE registration */
  song_fields[2].target_record = type_rec;

  /* Open song_type fields_hd (with CM_SORTED as real app does) */
  uint32_t type_fields_hd = corm_open(NULL, NULL, CM_STR,
      corm_record_type_id(type_rec), 0x3FF, CM_RECORD(type_rec) | CM_SORTED);
  fprintf(stderr, "type_fields_hd=%u\n", type_fields_hd);

  /* Open song_type data_hd (row_hd) */
  uint32_t type_data_hd = corm_open(NULL, NULL, CM_STR,
      corm_record_type_id(type_rec), 0x3FF, CM_SORTED);
  fprintf(stderr, "type_data_hd=%u\n", type_data_hd);

  /* Set target_hd for type field */
  corm_record_field_set_target_hd(song_rec, "type", type_fields_hd);

  /* Open song fields_hd (with CM_SORTED as real app does) */
  uint32_t song_fields_hd = corm_open(NULL, NULL, CM_STR,
      corm_record_type_id(song_rec), 0x3FF, CM_RECORD(song_rec) | CM_SORTED);
  fprintf(stderr, "song_fields_hd=%u\n", song_fields_hd);

  /* Open song data_hd */
  uint32_t song_data_hd = corm_open(NULL, NULL, CM_STR,
      corm_record_type_id(song_rec), 0x3FF, CM_SORTED);
  fprintf(stderr, "song_data_hd=%u\n", song_data_hd);

  /* Put some type entries first */
  song_type_cache_t tp;
  memset(&tp, 0, sizeof(tp));
  strcpy(tp.name, "hymn");
  corm_put(type_data_hd, "hymn", &tp);
  strcpy(tp.name, "worship");
  corm_put(type_data_hd, "worship", &tp);

  /* Now do the same sequence as source_scan_item */
  fprintf(stderr, "\n=== Test: field_put + field_get with full app setup ===\n");
  corm_field_put(song_fields_hd, "amazing_grace", "id", "amazing_grace");
  corm_field_put(song_fields_hd, "amazing_grace", "title", "Amazing Grace");
  corm_field_put(song_fields_hd, "amazing_grace", "type", "hymn");
  corm_field_put(song_fields_hd, "amazing_grace", "author", "John Newton");

  const char *got;
  got = corm_field_get(song_fields_hd, "amazing_grace", "title");
  fprintf(stderr, "title:  got='%s' expected='Amazing Grace' %s\n",
          got ? got : "(NULL)",
          (got && strcmp(got, "Amazing Grace") == 0) ? "OK" : "FAIL");
  if (!got || strcmp(got, "Amazing Grace") != 0) {
    fprintf(stderr, "  FAIL: title mismatch\n");
    if (got) hex_dump("  title_bytes", got, 32);
    errors++;
  }

  got = corm_field_get(song_fields_hd, "amazing_grace", "author");
  fprintf(stderr, "author: got='%s' expected='John Newton' %s\n",
          got ? got : "(NULL)",
          (got && strcmp(got, "John Newton") == 0) ? "OK" : "FAIL");
  if (!got || strcmp(got, "John Newton") != 0) {
    fprintf(stderr, "  FAIL: author mismatch\n");
    errors++;
  }

  /* Verify whole struct */
  const song_cache_t *whole = corm_get(song_data_hd, "amazing_grace");
  fprintf(stderr, "whole struct: %s\n", whole ? "found" : "NOT FOUND");
  if (whole) {
    fprintf(stderr, "  id=%s title=%s author=%s\n",
            whole->id, whole->title, whole->author);
    if (strcmp(whole->title, "Amazing Grace") != 0) {
      fprintf(stderr, "  FAIL: whole struct title mismatch\n");
      hex_dump("  title_bytes", whole->title, 32);
      errors++;
    }
  }

  /* Now try corm_field_put on the song.data vstr field */
  fprintf(stderr, "\n=== Test: VSTR field put ===\n");
  /* We need to register a VSTR field. Let's use a simpler approach. */

  fprintf(stderr, "\n");
  if (errors == 0)
    printf("ALL TESTS PASSED\n");
  else
    printf("%u TEST(S) FAILED\n", errors);

  corm_close(song_data_hd);
  corm_close(song_fields_hd);
  corm_close(type_data_hd);
  corm_close(type_fields_hd);
  return errors == 0 ? 0 : 1;
}
