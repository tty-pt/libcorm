#include "./../include/ttypt/corm.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>

#define TEST_MASK 0x7F

unsigned errors = 0;

#define PASS() printf("  OK\n")
#define FAIL(msg) do { printf("  FAIL: %s [line %d]\n", msg, __LINE__); errors++; } while(0)
#define ASSERT(cond, msg) do { if (!(cond)) FAIL(msg); else PASS(); } while(0)

typedef struct {
  char name[64];
  uint32_t age;
  char email[128];
} person_t;

/* ── Test 1: Record registration ───────────────────────────────────── */

static void test_record_register(void)
{
  printf("=== Record registration basic ===\n");

  corm_record_field_t fields[] = {
    { "name",  CM_STR, offsetof(person_t, name),  sizeof(((person_t*)0)->name)  , 0, 0, NULL },
    { "age",   CM_U32, offsetof(person_t, age),   sizeof(uint32_t)              , 0, 0, NULL },
    { "email", CM_STR, offsetof(person_t, email), sizeof(((person_t*)0)->email) , 0, 0, NULL },
  };

  uint32_t rec = corm_record_register("person", sizeof(person_t), fields, 3);
  ASSERT(rec != CM_MISS, "registration succeeds");

  uint32_t tid = corm_record_type_id(rec);
  ASSERT(tid != CM_MISS, "type id is valid");
  ASSERT(tid != CM_STR, "type id is a custom type, not CM_STR");
}

/* ── Test 2: Multiple records ───────────────────────────────────────── */

static void test_multiple_records(void)
{
  printf("=== Multiple records ===\n");

  typedef struct { char label[16]; } a_t;
  typedef struct { uint32_t val; } b_t;

  corm_record_field_t fa[] = {
    { "label", CM_STR, offsetof(a_t, label), sizeof(((a_t*)0)->label) , 0, 0, NULL },
  };
  corm_record_field_t fb[] = {
    { "val", CM_U32, offsetof(b_t, val), sizeof(uint32_t) , 0, 0, NULL },
  };

  uint32_t ra = corm_record_register("atype", sizeof(a_t), fa, 1);
  uint32_t rb = corm_record_register("btype", sizeof(b_t), fb, 1);
  ASSERT(ra != CM_MISS, "record A registered");
  ASSERT(rb != CM_MISS, "record B registered");
  ASSERT(ra != rb, "record IDs are distinct");
  ASSERT(corm_record_type_id(ra) != corm_record_type_id(rb),
         "type IDs are distinct");
}

/* ── Test 3: Open record-aware map ──────────────────────────────────── */

static void test_open_record_map(void)
{
  printf("=== Open record-aware map ===\n");

  corm_record_field_t fields[] = {
    { "name", CM_STR, offsetof(person_t, name), sizeof(((person_t*)0)->name) , 0, 0, NULL },
    { "age",  CM_U32, offsetof(person_t, age),  sizeof(uint32_t) , 0, 0, NULL },
  };
  uint32_t rec = corm_record_register("open_test", sizeof(person_t), fields, 2);
  uint32_t vtype = corm_record_type_id(rec);
  uint32_t hd = corm_open(NULL, NULL, CM_STR, vtype, TEST_MASK, CM_RECORD(rec));
  ASSERT(hd != CM_MISS, "open succeeds");
  corm_close(hd);
}

static void test_open_record_map_invalid(void)
{
  printf("=== Open record-aware map with invalid params ===\n");

  corm_record_field_t fields[] = {
    { "name", CM_STR, offsetof(person_t, name), sizeof(((person_t*)0)->name) , 0, 0, NULL },
  };
  uint32_t rec = corm_record_register("open_test_inv", sizeof(person_t), fields, 1);

  uint32_t hd = corm_open(NULL, NULL, CM_U32, corm_record_type_id(rec),
                          TEST_MASK, CM_RECORD(rec));
  ASSERT(hd == CM_MISS, "non-STR ktype rejected");

  hd = corm_open(NULL, NULL, CM_STR, CM_STR, TEST_MASK, CM_RECORD(rec));
  ASSERT(hd == CM_MISS, "wrong vtype rejected");

  hd = corm_open(NULL, NULL, CM_STR, corm_record_type_id(rec),
                 TEST_MASK, CM_RECORD(0xFF));
  ASSERT(hd == CM_MISS, "invalid record ID rejected");
}

/* ── Test 4: Non-record map unaffected (literal ':' in key) ─────────── */

static void test_non_record_unaffected(void)
{
  printf("=== Non-record map: literal ':' in key ===\n");

  uint32_t hd = corm_open(NULL, NULL, CM_STR, CM_STR, TEST_MASK, 0);
  ASSERT(hd != CM_MISS, "open non-record map");

  corm_put(hd, "key:sub", "value_with_colon");
  const char *v = corm_get(hd, "key:sub");
  ASSERT(v && strcmp(v, "value_with_colon") == 0,
         "literal ':' key works in non-record map");

  corm_close(hd);
}

/* ── Test 5: Struct put and whole-struct get ────────────────────────── */

static void test_whole_struct_put_get(void)
{
  printf("=== Struct put and whole-struct get ===\n");

  corm_record_field_t fields[] = {
    { "name",  CM_STR, offsetof(person_t, name),  sizeof(((person_t*)0)->name)  , 0, 0, NULL },
    { "age",   CM_U32, offsetof(person_t, age),   sizeof(uint32_t)              , 0, 0, NULL },
    { "email", CM_STR, offsetof(person_t, email), sizeof(((person_t*)0)->email) , 0, 0, NULL },
  };
  uint32_t rec = corm_record_register("person", sizeof(person_t), fields, 3);
  uint32_t hd = corm_open(NULL, NULL, CM_STR, corm_record_type_id(rec),
                          TEST_MASK, CM_RECORD(rec));

  person_t alice = { .name = "Alice", .age = 30, .email = "alice@x.y" };
  corm_put(hd, "alice", &alice);

  const person_t *got = corm_get(hd, "alice");
  ASSERT(got != NULL, "whole-struct get returns non-NULL");
  ASSERT(got != &alice, "returns pointer into map (not our stack)");
  ASSERT(strcmp(got->name, "Alice") == 0, "name matches");
  ASSERT(got->age == 30, "age matches");
  ASSERT(strcmp(got->email, "alice@x.y") == 0, "email matches");

  corm_close(hd);
}

/* ── Test 6: Field get via composite key ────────────────────────────── */

static void test_field_get_composite(void)
{
  printf("=== Field get via composite key ===\n");

  corm_record_field_t fields[] = {
    { "name",  CM_STR, offsetof(person_t, name),  sizeof(((person_t*)0)->name)  , 0, 0, NULL },
    { "age",   CM_U32, offsetof(person_t, age),   sizeof(uint32_t)              , 0, 0, NULL },
  };
  uint32_t rec = corm_record_register("test_field_get", sizeof(person_t), fields, 2);
  uint32_t hd = corm_open(NULL, NULL, CM_STR, corm_record_type_id(rec),
                          TEST_MASK, CM_RECORD(rec));

  person_t p = { .name = "Bob", .age = 25 };
  corm_put(hd, "bob", &p);

  const char *name = corm_get(hd, "bob:name");
  ASSERT(name != NULL, "bob:name returns non-NULL");
  ASSERT(strcmp(name, "Bob") == 0, "name field correct");

  const uint32_t *age = corm_get(hd, "bob:age");
  ASSERT(age != NULL, "bob:age returns non-NULL");
  ASSERT(*age == 25, "age field correct");

  /* Unknown field returns NULL */
  const void *bad = corm_get(hd, "bob:nonexistent");
  ASSERT(bad == NULL, "unknown field returns NULL");

  /* Non-existent key returns NULL */
  bad = corm_get(hd, "nobody:name");
  ASSERT(bad == NULL, "non-existent struct key returns NULL");

  corm_close(hd);
}

/* ── Test 7: Field put updates struct ───────────────────────────────── */

static void test_field_put_updates_struct(void)
{
  printf("=== Field put updates struct ===\n");

  corm_record_field_t fields[] = {
    { "name",  CM_STR, offsetof(person_t, name),  sizeof(((person_t*)0)->name)  , 0, 0, NULL },
    { "age",   CM_U32, offsetof(person_t, age),   sizeof(uint32_t)              , 0, 0, NULL },
    { "email", CM_STR, offsetof(person_t, email), sizeof(((person_t*)0)->email) , 0, 0, NULL },
  };
  uint32_t rec = corm_record_register("test_field_put", sizeof(person_t), fields, 3);
  uint32_t hd = corm_open(NULL, NULL, CM_STR, corm_record_type_id(rec),
                          TEST_MASK, CM_RECORD(rec));

  person_t p = { .name = "Carol", .age = 40, .email = "carol@old" };
  corm_put(hd, "carol", &p);

  /* Update age field */
  uint32_t new_age = 41;
  corm_put(hd, "carol:age", &new_age);

  const person_t *got = corm_get(hd, "carol");
  ASSERT(got->age == 41, "age field updated in struct");
  ASSERT(strcmp(got->name, "Carol") == 0, "name unchanged");
  ASSERT(strcmp(got->email, "carol@old") == 0, "email unchanged");

  /* Check field-level get also reflects update */
  const uint32_t *age = corm_get(hd, "carol:age");
  ASSERT(age != NULL && *age == 41, "field-level get reflects update");

  /* Update name via field put */
  corm_put(hd, "carol:name", "Carol-Updated");
  got = corm_get(hd, "carol");
  ASSERT(strcmp(got->name, "Carol-Updated") == 0, "name updated via field put");

  corm_close(hd);
}

/* ── Test 8: Field put creates missing struct ──────────────────────── */

static void test_field_put_creates_struct(void)
{
  printf("=== Field put creates missing struct ===\n");

  corm_record_field_t fields[] = {
    { "name",  CM_STR, offsetof(person_t, name),  sizeof(((person_t*)0)->name)  , 0, 0, NULL },
    { "age",   CM_U32, offsetof(person_t, age),   sizeof(uint32_t)              , 0, 0, NULL },
  };
  uint32_t rec = corm_record_register("test_create", sizeof(person_t), fields, 2);
  uint32_t hd = corm_open(NULL, NULL, CM_STR, corm_record_type_id(rec),
                          TEST_MASK, CM_RECORD(rec));

  /* Struct doesn't exist yet */
  const person_t *got = corm_get(hd, "dave");
  ASSERT(got == NULL, "struct not present initially");

  /* Field put creates it */
  uint32_t age = 35;
  corm_put(hd, "dave:age", &age);

  got = corm_get(hd, "dave");
  ASSERT(got != NULL, "struct created by field put");
  ASSERT(got->age == 35, "age set correctly");
  ASSERT(got->name[0] == '\0', "name is zeroed");

  /* Field put on another field of same struct */
  corm_put(hd, "dave:name", "Dave");
  got = corm_get(hd, "dave");
  ASSERT(strcmp(got->name, "Dave") == 0, "name set on existing struct");
  ASSERT(got->age == 35, "age preserved");

  corm_close(hd);
}

/* ── Test 9: Field put overwrites existing value ───────────────────── */

static void test_field_put_overwrite(void)
{
  printf("=== Field put overwrites existing value ===\n");

  corm_record_field_t fields[] = {
    { "name", CM_STR, offsetof(person_t, name), sizeof(((person_t*)0)->name) , 0, 0, NULL },
    { "age",  CM_U32, offsetof(person_t, age),  sizeof(uint32_t) , 0, 0, NULL },
  };
  uint32_t rec = corm_record_register("test_overwrite", sizeof(person_t), fields, 2);
  uint32_t hd = corm_open(NULL, NULL, CM_STR, corm_record_type_id(rec),
                          TEST_MASK, CM_RECORD(rec));

  person_t p = { .name = "Eve", .age = 50 };
  corm_put(hd, "eve", &p);

  uint32_t new_age = 51;
  corm_put(hd, "eve:age", &new_age);

  const uint32_t *age = corm_get(hd, "eve:age");
  ASSERT(age != NULL && *age == 51, "age overwritten");

  new_age = 52;
  corm_put(hd, "eve:age", &new_age);
  age = corm_get(hd, "eve:age");
  ASSERT(age != NULL && *age == 52, "age overwritten again");

  /* Whole-struct get also reflects field updates */
  const person_t *got = corm_get(hd, "eve");
  ASSERT(got->age == 52, "whole struct reflects field updates");

  corm_close(hd);
}

/* ── Test 10: Delete whole struct ──────────────────────────────────── */

static void test_delete_whole_struct(void)
{
  printf("=== Delete whole struct ===\n");

  corm_record_field_t fields[] = {
    { "name", CM_STR, offsetof(person_t, name), sizeof(((person_t*)0)->name) , 0, 0, NULL },
    { "age",  CM_U32, offsetof(person_t, age),  sizeof(uint32_t) , 0, 0, NULL },
  };
  uint32_t rec = corm_record_register("test_del_whole", sizeof(person_t), fields, 2);
  uint32_t hd = corm_open(NULL, NULL, CM_STR, corm_record_type_id(rec),
                          TEST_MASK, CM_RECORD(rec));

  person_t p = { .name = "Frank", .age = 60 };
  corm_put(hd, "frank", &p);
  ASSERT(corm_get(hd, "frank") != NULL, "struct exists");

  corm_del(hd, "frank");
  ASSERT(corm_get(hd, "frank") == NULL, "struct deleted");

  /* Field-level get also returns NULL after delete */
  ASSERT(corm_get(hd, "frank:name") == NULL, "field get returns NULL after delete");

  corm_close(hd);
}

/* ── Test 11: Delete single field zeros it ──────────────────────────── */

static void test_delete_field_zeros(void)
{
  printf("=== Delete single field zeros it ===\n");

  corm_record_field_t fields[] = {
    { "name",  CM_STR, offsetof(person_t, name),  sizeof(((person_t*)0)->name)  , 0, 0, NULL },
    { "age",   CM_U32, offsetof(person_t, age),   sizeof(uint32_t)              , 0, 0, NULL },
    { "email", CM_STR, offsetof(person_t, email), sizeof(((person_t*)0)->email) , 0, 0, NULL },
  };
  uint32_t rec = corm_record_register("test_del_field", sizeof(person_t), fields, 3);
  uint32_t hd = corm_open(NULL, NULL, CM_STR, corm_record_type_id(rec),
                          TEST_MASK, CM_RECORD(rec));

  person_t p = { .name = "Grace", .age = 70, .email = "grace@x.y" };
  corm_put(hd, "grace", &p);

  /* Delete the name field */
  corm_del(hd, "grace:name");

  const person_t *got = corm_get(hd, "grace");
  ASSERT(got != NULL, "struct still exists");
  ASSERT(got->name[0] == '\0', "name field zeroed");
  ASSERT(got->age == 70, "age preserved");
  ASSERT(strcmp(got->email, "grace@x.y") == 0, "email preserved");

  /* Field-level get of deleted field returns zeroed bytes */
  const char *name = corm_get(hd, "grace:name");
  ASSERT(name != NULL, "field ptr still valid");
  ASSERT(name[0] == '\0', "field ptr points to zeroed bytes");

  /* Delete the age field */
  corm_del(hd, "grace:age");
  got = corm_get(hd, "grace");
  ASSERT(got->age == 0, "age field zeroed");

  corm_close(hd);
}

/* ── Test 12: String truncation on field put ────────────────────────── */

static void test_string_truncation(void)
{
  printf("=== String truncation on field put ===\n");

  /* Use a struct with a very small name field */
  typedef struct {
    char name[8];
  } small_t;

  corm_record_field_t fields[] = {
    { "name", CM_STR, offsetof(small_t, name), sizeof(((small_t*)0)->name) , 0, 0, NULL },
  };
  uint32_t rec = corm_record_register("small", sizeof(small_t), fields, 1);
  uint32_t hd = corm_open(NULL, NULL, CM_STR, corm_record_type_id(rec),
                          TEST_MASK, CM_RECORD(rec));

  /* Put a value that exactly fits */
  corm_put(hd, "x:name", "1234567"); /* 7 chars + null = 8 */
  const char *v = corm_get(hd, "x:name");
  ASSERT(v != NULL && strcmp(v, "1234567") == 0, "exact fit works");

  /* Put a value that is too long and gets truncated */
  corm_put(hd, "x:name", "1234567890"); /* 10 chars + null would be 11 > 8 */
  v = corm_get(hd, "x:name");
  ASSERT(v != NULL && strlen(v) == 7, "truncated to 7 chars + null");
  ASSERT(strncmp(v, "1234567", 7) == 0, "first 7 chars correct");
  ASSERT(v[7] == '\0', "null-terminated");

  corm_close(hd);
}

/* ── Test 13: Overwrite struct at same key ──────────────────────────── */

static void test_overwrite_struct(void)
{
  printf("=== Overwrite struct at same key ===\n");

  corm_record_field_t fields[] = {
    { "name", CM_STR, offsetof(person_t, name), sizeof(((person_t*)0)->name) , 0, 0, NULL },
    { "age",  CM_U32, offsetof(person_t, age),  sizeof(uint32_t) , 0, 0, NULL },
  };
  uint32_t rec = corm_record_register("test_overwrite2", sizeof(person_t), fields, 2);
  uint32_t hd = corm_open(NULL, NULL, CM_STR, corm_record_type_id(rec),
                          TEST_MASK, CM_RECORD(rec));

  person_t p1 = { .name = "Henry", .age = 80 };
  corm_put(hd, "henry", &p1);

  const person_t *ptr1 = corm_get(hd, "henry");

  /* Overwrite with completely different struct */
  person_t p2 = { .name = "Hank", .age = 81 };
  corm_put(hd, "henry", &p2);

  const person_t *ptr2 = corm_get(hd, "henry");
  ASSERT(ptr2 != NULL, "struct still exists");
  ASSERT(strcmp(ptr2->name, "Hank") == 0, "name updated");
  ASSERT(ptr2->age == 81, "age updated");

  /* Pointer should be reused (same key, same size) */
  ASSERT(ptr1 == ptr2, "pointer stable across same-key same-size overwrite");

  /* Field-level get shows new values */
  const uint32_t *age = corm_get(hd, "henry:age");
  ASSERT(age != NULL && *age == 81, "field-level get shows new age");

  corm_close(hd);
}

/* ── Test 14: Association callbacks with record map ────────────────────*/

static void assoc_cb(const void **skey, const void *pkey,
                     const void *value, void *userdata)
{
  (void)pkey;
  (void)userdata;
  /* Use the person's name field as the secondary key */
  const person_t *p = (const person_t *)value;
  *skey = strdup(p->name);
}

static void test_assoc_record(void)
{
  printf("=== Association callbacks with record map ===\n");

  corm_record_field_t fields[] = {
    { "name", CM_STR, offsetof(person_t, name), sizeof(((person_t*)0)->name) , 0, 0, NULL },
    { "age",  CM_U32, offsetof(person_t, age),  sizeof(uint32_t) , 0, 0, NULL },
  };
  uint32_t rec = corm_record_register("assoc_test", sizeof(person_t), fields, 2);
  uint32_t vtype = corm_record_type_id(rec);
  uint32_t prim = corm_open(NULL, NULL, CM_STR, vtype, TEST_MASK, CM_RECORD(rec));
  uint32_t sec  = corm_open(NULL, NULL, CM_STR, CM_STR, TEST_MASK,
                            CM_SORTED);

  corm_assoc(sec, prim, assoc_cb, NULL);

  /* Put whole struct - assoc should fire and create secondary entry */
  person_t p = { .name = "Iris", .age = 90 };
  corm_put(prim, "iris", &p);

  /* assoc stores (secondary_key=name, primary_value=struct_bytes) */
  const char *sec_val = corm_get(sec, "Iris");
  ASSERT(sec_val != NULL, "secondary entry created for 'Iris'");

  /* Field put should fire assoc and update secondary */
  corm_put(prim, "iris:name", "Iris-Updated");
  sec_val = corm_get(sec, "Iris-Updated");
  ASSERT(sec_val != NULL, "secondary updated for new name");
  sec_val = corm_get(sec, "Iris");
  ASSERT(sec_val == NULL, "old secondary key removed");

  corm_close(prim);
  corm_close(sec);
}

/* ── Test 15: Multiple items, multiple fields ───────────────────────── */

static void test_multiple_items_fields(void)
{
  printf("=== Multiple items, multiple fields ===\n");

  corm_record_field_t fields[] = {
    { "name",  CM_STR, offsetof(person_t, name),  sizeof(((person_t*)0)->name)  , 0, 0, NULL },
    { "age",   CM_U32, offsetof(person_t, age),   sizeof(uint32_t)              , 0, 0, NULL },
    { "email", CM_STR, offsetof(person_t, email), sizeof(((person_t*)0)->email) , 0, 0, NULL },
  };
  uint32_t rec = corm_record_register("multi_test", sizeof(person_t), fields, 3);
  uint32_t hd = corm_open(NULL, NULL, CM_STR, corm_record_type_id(rec),
                          TEST_MASK, CM_RECORD(rec));

  person_t people[] = {
    { .name = "Jack",   .age = 20, .email = "jack@a" },
    { .name = "Jill",   .age = 21, .email = "jill@b" },
    { .name = "Jim",    .age = 22, .email = "jim@c"  },
  };

  corm_put(hd, "jack", &people[0]);
  corm_put(hd, "jill", &people[1]);
  corm_put(hd, "jim",  &people[2]);

  /* Verify all items by whole-struct get */
  const char *keys[] = { "jack", "jill", "jim" };
  for (int i = 0; i < 3; i++) {
    const person_t *got = corm_get(hd, keys[i]);
    ASSERT(got != NULL, "struct access by lowercase key");
  }

  const person_t *jack = corm_get(hd, "jack");
  ASSERT(strcmp(jack->name, "Jack") == 0
         && jack->age == 20
         && strcmp(jack->email, "jack@a") == 0,
         "jack struct correct");

  const person_t *jill = corm_get(hd, "jill");
  ASSERT(strcmp(jill->name, "Jill") == 0
         && jill->age == 21
         && strcmp(jill->email, "jill@b") == 0,
         "jill struct correct");

  const person_t *jim = corm_get(hd, "jim");
  ASSERT(strcmp(jim->name, "Jim") == 0
         && jim->age == 22
         && strcmp(jim->email, "jim@c") == 0,
         "jim struct correct");

  /* Field-level gets for each */
  const char *name = corm_get(hd, "jack:name");
  ASSERT(name != NULL && strcmp(name, "Jack") == 0, "jack:name");

  const uint32_t *age = corm_get(hd, "jill:age");
  ASSERT(age != NULL && *age == 21, "jill:age");

  const char *email = corm_get(hd, "jim:email");
  ASSERT(email != NULL && strcmp(email, "jim@c") == 0, "jim:email");

  /* Update some fields */
  uint32_t new_age = 23;
  corm_put(hd, "jim:age", &new_age);
  age = corm_get(hd, "jim:age");
  ASSERT(age != NULL && *age == 23, "jim:age updated");

  /* Delete a field */
  corm_del(hd, "jack:email");
  email = corm_get(hd, "jack:email");
  ASSERT(email != NULL && email[0] == '\0', "jack:email zeroed");

  corm_close(hd);
}

/* ── Test 16: Registration edge cases ───────────────────────────────── */

static void test_registration_errors(void)
{
  printf("=== Registration edge cases ===\n");

  uint32_t rec;

  rec = corm_record_register("bad", 0, NULL, 0);
  ASSERT(rec == CM_MISS, "zero struct_size rejected");

  rec = corm_record_register("bad", 4, NULL, 0);
  ASSERT(rec == CM_MISS, "zero fields rejected");
}

/* ── Test 17: Pointer stability across map growth ──────────────────── */

static void test_pointer_stability_growth(void)
{
  printf("=== Pointer stability across map growth ===\n");

  /* Use tiny mask to force frequent growth */
  corm_record_field_t fields[] = {
    { "name", CM_STR, offsetof(person_t, name), sizeof(((person_t*)0)->name) , 0, 0, NULL },
  };
  uint32_t rec = corm_record_register("growth_test", sizeof(person_t), fields, 1);
  uint32_t hd = corm_open(NULL, NULL, CM_STR, corm_record_type_id(rec),
                          0x3, CM_RECORD(rec));

  person_t p = { .name = "First" };
  corm_put(hd, "first", &p);

  const char *saved = corm_get(hd, "first:name");
  ASSERT(saved != NULL && strcmp(saved, "First") == 0, "initial field value");

  /* Insert many items to trigger growth */
  for (int i = 0; i < 50; i++) {
    char key[32];
    snprintf(key, sizeof(key), "user_%d", i);
    person_t tmp;
    snprintf(tmp.name, sizeof(tmp.name), "User%d", i);
    corm_put(hd, key, &tmp);
  }

  /* First entry's field pointer must still be valid */
  const char *after = corm_get(hd, "first:name");
  ASSERT(after == saved, "field pointer stable across growth");
  ASSERT(strcmp(after, "First") == 0, "field value intact after growth");

  /* All other entries should also be accessible */
  int ok = 1;
  for (int i = 0; i < 50; i++) {
    char key[32], expected[32];
    snprintf(key, sizeof(key), "user_%d", i);
    snprintf(expected, sizeof(expected), "User%d", i);
    const char *val = corm_get(hd, key);
    if (!val || strcmp(val, expected) != 0) {
      ok = 0;
      break;
    }
  }
  ASSERT(ok == 1, "all entries accessible after growth");

  corm_close(hd);
}

/* ── Test 18: Iteration over record-aware map ──────────────────────── */

static void test_iteration(void)
{
  printf("=== Iteration over record-aware map ===\n");

  corm_record_field_t fields[] = {
    { "name", CM_STR, offsetof(person_t, name), sizeof(((person_t*)0)->name) , 0, 0, NULL },
    { "age",  CM_U32, offsetof(person_t, age),  sizeof(uint32_t) , 0, 0, NULL },
  };
  uint32_t rec = corm_record_register("iter_test", sizeof(person_t), fields, 2);
  uint32_t hd = corm_open(NULL, NULL, CM_STR, corm_record_type_id(rec),
                          TEST_MASK, CM_RECORD(rec));

  person_t p1 = { .name = "Kate", .age = 30 };
  person_t p2 = { .name = "Leo",  .age = 31 };
  corm_put(hd, "kate", &p1);
  corm_put(hd, "leo",  &p2);

  uint32_t cur = corm_iter(hd, NULL, 0);
  const void *key, *value;
  int count = 0;
  while (corm_next(&key, &value, cur)) {
    const person_t *p = (const person_t *)value;
    ASSERT(p != NULL, "iter value is non-NULL");
    count++;
  }
  ASSERT(count == 2, "iterated 2 items");

  corm_close(hd);
}



/* ── Test 20: Multiple record IDs with same struct type ────────────── */

static void test_same_type_multi_records(void)
{
  printf("=== Multiple records with same struct type ===\n");

  corm_record_field_t fields_a[] = {
    { "name", CM_STR, offsetof(person_t, name), sizeof(((person_t*)0)->name) , 0, 0, NULL },
  };
  corm_record_field_t fields_b[] = {
    { "age", CM_U32, offsetof(person_t, age), sizeof(uint32_t) , 0, 0, NULL },
  };

  uint32_t ra = corm_record_register("ra", sizeof(person_t), fields_a, 1);
  uint32_t rb = corm_record_register("rb", sizeof(person_t), fields_b, 1);
  ASSERT(ra != rb, "different record IDs");

  uint32_t ha = corm_open(NULL, NULL, CM_STR, corm_record_type_id(ra),
                          TEST_MASK, CM_RECORD(ra));
  uint32_t hb = corm_open(NULL, NULL, CM_STR, corm_record_type_id(rb),
                          TEST_MASK, CM_RECORD(rb));
  ASSERT(ha != CM_MISS && hb != CM_MISS, "both maps open");

  person_t p = { .name = "Nina", .age = 50 };
  corm_put(ha, "nina", &p);

  const char *name = corm_get(ha, "nina:name");
  ASSERT(name != NULL && strcmp(name, "Nina") == 0, "ha has name field");

  const uint32_t *age = corm_get(hb, "nina:name");
  ASSERT(age == NULL, "hb doesn't have name field (wrong record type)");

  corm_put(hb, "nina", &p);
  age = corm_get(hb, "nina:age");
  ASSERT(age != NULL && *age == 50, "hb has age field");

  corm_close(ha);
  corm_close(hb);
}

/* ── Test 21: U32 field value via field put ────────────────────────── */

static void test_u32_field_put_via_ptr(void)
{
  printf("=== U32 field put via value pointer ===\n");

  corm_record_field_t fields[] = {
    { "count", CM_U32, offsetof(person_t, age), sizeof(uint32_t) , 0, 0, NULL },
  };
  uint32_t rec = corm_record_register("u32test", sizeof(person_t), fields, 1);
  uint32_t hd = corm_open(NULL, NULL, CM_STR, corm_record_type_id(rec),
                          TEST_MASK, CM_RECORD(rec));

  uint32_t v = 42;
  corm_put(hd, "item:count", &v);

  const uint32_t *got = corm_get(hd, "item:count");
  ASSERT(got != NULL && *got == 42, "U32 field via field put");

  v = 99;
  corm_put(hd, "item:count", &v);
  got = corm_get(hd, "item:count");
  ASSERT(got != NULL && *got == 99, "U32 field updated");

  corm_close(hd);
}

/* ── Reference field tests ───────────────────────────────────────────── */

typedef struct {
  char id[64];
  char single[64];
  char multi[2048];
} ref_source_t;

typedef struct {
  char label[64];
} ref_target_t;

/* ── Test 22: CM_REFERENCE field put and get as string ──────────────── */

static void test_ref_field_get(void)
{
  printf("=== CM_REFERENCE field put and get ===\n");

  corm_record_field_t f[] = {
    { "id", CM_STR, offsetof(ref_source_t, id), sizeof(((ref_source_t*)0)->id) , 0, 0, NULL },
    { "ref", CM_REFERENCE, offsetof(ref_source_t, single), sizeof(((ref_source_t*)0)->single) , 0, 0, NULL },
  };
  uint32_t rec = corm_record_register("refget_src", sizeof(ref_source_t), f, 2);
  uint32_t hd = corm_open(NULL, NULL, CM_STR, corm_record_type_id(rec),
                           TEST_MASK, CM_RECORD(rec));
  ASSERT(hd != CM_MISS, "map opened");

  corm_put(hd, "x:ref", "target_id");
  const char *got = corm_get(hd, "x:ref");
  ASSERT(got != NULL && strcmp(got, "target_id") == 0,
         "ref field stores string");

  corm_put(hd, "x:ref", "other_id");
  got = corm_get(hd, "x:ref");
  ASSERT(got != NULL && strcmp(got, "other_id") == 0,
         "ref field updates string");

  corm_close(hd);
}

/* ── Test 23: CM_MULTI_REFERENCE field put and get ──────────────────── */

static void test_multi_ref_field_get(void)
{
  printf("=== CM_MULTI_REFERENCE field put and get ===\n");

  corm_record_field_t f[] = {
    { "id", CM_STR, offsetof(ref_source_t, id), sizeof(((ref_source_t*)0)->id) , 0, 0, NULL },
    { "m", CM_MULTI_REFERENCE, offsetof(ref_source_t, multi),
      sizeof(((ref_source_t*)0)->multi), 0, 0, NULL },
  };
  uint32_t rec = corm_record_register("mrefget_src", sizeof(ref_source_t), f, 2);
  uint32_t hd = corm_open(NULL, NULL, CM_STR, corm_record_type_id(rec),
                           TEST_MASK, CM_RECORD(rec));
  ASSERT(hd != CM_MISS, "map opened");

  corm_put(hd, "x:m", "7\n12");
  const char *got = corm_get(hd, "x:m");
  ASSERT(got != NULL && strcmp(got, "7\n12") == 0, "multi_ref content");

  corm_put(hd, "x:m", "");
  got = corm_get(hd, "x:m");
  ASSERT(got != NULL && got[0] == '\0', "empty multi_ref");

  corm_close(hd);
}

/* ── Test 24: Single reference inverse index ────────────────────────── */

static void test_single_ref_inverse(void)
{
  printf("=== Single reference inverse index ===\n");

  corm_record_field_t tf[] = {
    { "label", CM_STR, offsetof(ref_target_t, label), sizeof(((ref_target_t*)0)->label) , 0, 0, NULL },
  };
  uint32_t trec = corm_record_register("inv_tgt", sizeof(ref_target_t), tf, 1);

  corm_record_field_t sf[] = {
    { "id", CM_STR, offsetof(ref_source_t, id), sizeof(((ref_source_t*)0)->id) , 0, 0, NULL },
        {
      .name = "ref",
      .type = CM_REFERENCE,
      .offset = offsetof(ref_source_t, single),
      .max_size = sizeof(((ref_source_t*)0)->single),
      .target_record = trec,
      .inverse = NULL,
    },
  };
  uint32_t srec = corm_record_register("inv_src", sizeof(ref_source_t), sf, 2);

  uint32_t thd = corm_open(NULL, NULL, CM_STR, corm_record_type_id(trec),
                            TEST_MASK, CM_RECORD(trec));
  uint32_t shd = corm_open(NULL, NULL, CM_STR, corm_record_type_id(srec),
                            TEST_MASK, CM_RECORD(srec));
  ASSERT(thd != CM_MISS && shd != CM_MISS, "maps opened");
  corm_record_field_set_target_hd(srec, "ref", thd);

  /* Put two target entries (positions 0 and 1) */
  ref_target_t ta = { .label = "A" }, tb = { .label = "B" };
  corm_put(thd, "ta", &ta);
  corm_put(thd, "tb", &tb);

  /* Put source referencing target "tb" via field-level puts
   * (field-level path creates inverse entries on insert) */
  corm_put(shd, "src1:id", "src1");
  corm_put(shd, "src1:ref", "tb");

  /* Source position 0 (first entry in empty map) */
  uint32_t src_pos = 0;

  /* Query inverse: which sources reference target position for "tb"? */
  uint32_t tb_pos = corm_pos(thd, "tb");
  uint32_t inv[16];
  size_t ninv = corm_inv_get(shd, "ref", tb_pos, inv, 16);
  ASSERT(ninv >= 1, "inverse has at least 1 entry");

  int found_inv = 0;
  for (size_t i = 0; i < ninv; i++)
    if (inv[i] == src_pos) found_inv = 1;
  ASSERT(found_inv, "source position 0 in inverse for target tb");

  /* Clear ref via field-level put — inverse should be cleaned */
  corm_put(shd, "src1:ref", "");
  ninv = corm_inv_get(shd, "ref", tb_pos, inv, 16);
  ASSERT(ninv == 0, "inverse empty after ref cleared");

  corm_close(shd);
  corm_close(thd);
}

/* ── Test 25: Multi-reference inverse index ─────────────────────────── */

static void test_multi_ref_inverse(void)
{
  printf("=== Multi-reference inverse index ===\n");

  corm_record_field_t tf[] = {
    { "label", CM_STR, offsetof(ref_target_t, label), sizeof(((ref_target_t*)0)->label) , 0, 0, NULL },
  };
  uint32_t trec = corm_record_register("m_inv_tgt", sizeof(ref_target_t), tf, 1);

  corm_record_field_t sf[] = {
    { "id", CM_STR, offsetof(ref_source_t, id), sizeof(((ref_source_t*)0)->id) , 0, 0, NULL },
        {
      .name = "m",
      .type = CM_MULTI_REFERENCE,
      .offset = offsetof(ref_source_t, multi),
      .max_size = sizeof(((ref_source_t*)0)->multi),
      .target_record = trec,
      .inverse = NULL,
    },
  };
  uint32_t srec = corm_record_register("m_inv_src", sizeof(ref_source_t), sf, 2);

  uint32_t thd = corm_open(NULL, NULL, CM_STR, corm_record_type_id(trec),
                            TEST_MASK, CM_RECORD(trec));
  uint32_t shd = corm_open(NULL, NULL, CM_STR, corm_record_type_id(srec),
                            TEST_MASK, CM_RECORD(srec));
  ASSERT(thd != CM_MISS && shd != CM_MISS, "maps opened");

  /* Put 3 target entries (positions 0, 1, 2) */
  ref_target_t tgt;
  corm_put(thd, "t0", &tgt);
  corm_put(thd, "t1", &tgt);
  corm_put(thd, "t2", &tgt);

  /* Put source with multi-ref "0\n2" via whole struct */
  ref_source_t s;
  memset(&s, 0, sizeof(s));
  strcpy(s.id, "s1");
  strcpy(s.multi, "0\n2");
  corm_put(shd, "s1", &s);

  uint32_t inv[16];

  /* t0 should have s1 in inverse */
  size_t ni = corm_inv_get(shd, "m", 0, inv, 16);
  ASSERT(ni >= 1, "t0 has inverse entries");

  /* t1 should have none */
  ni = corm_inv_get(shd, "m", 1, inv, 16);
  ASSERT(ni == 0, "t1 has no inverse entries");

  /* t2 should have s1 */
  ni = corm_inv_get(shd, "m", 2, inv, 16);
  ASSERT(ni >= 1, "t2 has inverse entries");

  /* Update to only reference t1 */
  memset(s.multi, 0, sizeof(s.multi));
  strcpy(s.multi, "1");
  corm_put(shd, "s1", &s);
  ni = corm_inv_get(shd, "m", 0, inv, 16);
  ASSERT(ni == 0, "t0 cleared after update");
  ni = corm_inv_get(shd, "m", 1, inv, 16);
  ASSERT(ni >= 1, "t1 has source after update");
  ni = corm_inv_get(shd, "m", 2, inv, 16);
  ASSERT(ni == 0, "t2 cleared after update");

  corm_close(shd);
  corm_close(thd);
}

/* ── Test 26: Field-level delete cleans inverse ──────────────────────── */

static void test_ref_field_del_cleanup(void)
{
  printf("=== Field-level delete cleans inverse ===\n");

  corm_record_field_t tf[] = {
    { "label", CM_STR, offsetof(ref_target_t, label), sizeof(((ref_target_t*)0)->label) , 0, 0, NULL },
  };
  uint32_t trec = corm_record_register("fdel_tgt", sizeof(ref_target_t), tf, 1);

  corm_record_field_t sf[] = {
    { "id", CM_STR, offsetof(ref_source_t, id), sizeof(((ref_source_t*)0)->id) , 0, 0, NULL },
        {
      .name = "ref",
      .type = CM_REFERENCE,
      .offset = offsetof(ref_source_t, single),
      .max_size = sizeof(((ref_source_t*)0)->single),
      .target_record = trec,
      .inverse = NULL,
    },
        {
      .name = "m",
      .type = CM_MULTI_REFERENCE,
      .offset = offsetof(ref_source_t, multi),
      .max_size = sizeof(((ref_source_t*)0)->multi),
      .target_record = trec,
      .inverse = NULL,
    },
  };
  uint32_t srec = corm_record_register("fdel_src", sizeof(ref_source_t), sf, 3);

  uint32_t thd = corm_open(NULL, NULL, CM_STR, corm_record_type_id(trec),
                            TEST_MASK, CM_RECORD(trec));
  uint32_t shd = corm_open(NULL, NULL, CM_STR, corm_record_type_id(srec),
                            TEST_MASK, CM_RECORD(srec));
  ASSERT(thd != CM_MISS && shd != CM_MISS, "maps opened");
  corm_record_field_set_target_hd(srec, "ref", thd);

  ref_target_t t;
  corm_put(thd, "tgt", &t);
  uint32_t tgt_pos = corm_pos(thd, "tgt");

  /* Put source with single ref = "tgt" */
  corm_put(shd, "src:id", "src");
  corm_put(shd, "src:ref", "tgt");

  /* Put with multi-ref */
  corm_put(shd, "src:m", "0");

  uint32_t inv[16];

  /* Field-level delete of ref */
  corm_del(shd, "src:ref");
  size_t ni = corm_inv_get(shd, "ref", tgt_pos, inv, 16);
  ASSERT(ni == 0, "ref inverse empty after field del");

  /* Field-level delete of multi_ref */
  corm_del(shd, "src:m");
  ni = corm_inv_get(shd, "m", 0, inv, 16);
  ASSERT(ni == 0, "multi_ref inverse empty after field del");

  corm_close(shd);
  corm_close(thd);
}

/* ── Test 27: Whole-struct delete cleans inverse ──────────────────────── */

static void test_ref_whole_struct_del_cleanup(void)
{
  printf("=== Whole-struct delete cleans inverse ===\n");

  corm_record_field_t tf[] = {
    { "label", CM_STR, offsetof(ref_target_t, label), sizeof(((ref_target_t*)0)->label) , 0, 0, NULL },
  };
  uint32_t trec = corm_record_register("wdel_tgt", sizeof(ref_target_t), tf, 1);

  corm_record_field_t sf[] = {
    { "id", CM_STR, offsetof(ref_source_t, id), sizeof(((ref_source_t*)0)->id) , 0, 0, NULL },
        {
      .name = "ref",
      .type = CM_REFERENCE,
      .offset = offsetof(ref_source_t, single),
      .max_size = sizeof(((ref_source_t*)0)->single),
      .target_record = trec,
      .inverse = NULL,
    },
        {
      .name = "m",
      .type = CM_MULTI_REFERENCE,
      .offset = offsetof(ref_source_t, multi),
      .max_size = sizeof(((ref_source_t*)0)->multi),
      .target_record = trec,
      .inverse = NULL,
    },
  };
  uint32_t srec = corm_record_register("wdel_src", sizeof(ref_source_t), sf, 3);

  uint32_t thd = corm_open(NULL, NULL, CM_STR, corm_record_type_id(trec),
                            TEST_MASK, CM_RECORD(trec));
  uint32_t shd = corm_open(NULL, NULL, CM_STR, corm_record_type_id(srec),
                            TEST_MASK, CM_RECORD(srec));
  ASSERT(thd != CM_MISS && shd != CM_MISS, "maps opened");
  corm_record_field_set_target_hd(srec, "ref", thd);

  ref_target_t t;
  corm_put(thd, "tgt", &t);
  uint32_t tgt_pos = corm_pos(thd, "tgt");

  /* Create source via field-level puts (creates inverse entries) */
  corm_put(shd, "src:id", "src");
  corm_put(shd, "src:ref", "tgt");
  corm_put(shd, "src:m", "0");

  uint32_t inv[16];

  /* Verify inverse was created */
  size_t ni = corm_inv_get(shd, "ref", tgt_pos, inv, 16);
  ASSERT(ni >= 1, "ref inverse exists before del");

  /* Delete whole struct */
  corm_del(shd, "src");

  /* Inverses should be cleaned */
  ni = corm_inv_get(shd, "ref", tgt_pos, inv, 16);
  ASSERT(ni == 0, "ref inverse empty after whole-struct del");

  ni = corm_inv_get(shd, "m", 0, inv, 16);
  ASSERT(ni == 0, "multi_ref inverse empty after whole-struct del");

  /* Source should be gone */
  ASSERT(corm_get(shd, "src") == NULL, "source deleted");

  corm_close(shd);
  corm_close(thd);
}

/* ── Test 28: corm_get_key API ──────────────────────────────────────── */

static void test_corm_get_key_api(void)
{
  printf("=== corm_get_key API ===\n");

  corm_record_field_t f[] = {
    { "id", CM_STR, offsetof(ref_source_t, id), sizeof(((ref_source_t*)0)->id) , 0, 0, NULL },
  };
  uint32_t rec = corm_record_register("keytest", sizeof(ref_source_t), f, 1);
  uint32_t hd = corm_open(NULL, NULL, CM_STR, corm_record_type_id(rec),
                           TEST_MASK, CM_RECORD(rec));
  ASSERT(hd != CM_MISS, "map opened");

  ref_source_t a = { .id = "alpha" }, b = { .id = "beta" };
  corm_put(hd, "alpha", &a);
  corm_put(hd, "beta", &b);

  /* alpha and beta are now in the map at some positions.
   * Verify that corm_get_key exists and returns non-NULL.
   * We just check the interface works. */
  const char *k0 = corm_get_key(hd, 0);
  const char *k1 = corm_get_key(hd, 1);
  ASSERT(k0 != NULL || k1 != NULL, "at least one key found");

  /* Out of range */
  const char *kbad = corm_get_key(hd, 9999);
  ASSERT(kbad == NULL, "out of range returns NULL");

  corm_close(hd);
}

/* ── Test 31: corm_pos API ────────────────────────────────────────────── */

static void test_corm_pos_api(void)
{
  printf("=== corm_pos API ===\n");

  corm_record_field_t f[] = {
    { "id", CM_STR, offsetof(ref_source_t, id), sizeof(((ref_source_t*)0)->id) , 0, 0, NULL },
  };
  uint32_t rec = corm_record_register("postest", sizeof(ref_source_t), f, 1);
  uint32_t hd = corm_open(NULL, NULL, CM_STR, corm_record_type_id(rec),
                           TEST_MASK, CM_RECORD(rec));
  ASSERT(hd != CM_MISS, "map opened");

  ref_source_t a = { .id = "a" }, b = { .id = "b" }, c = { .id = "c" };
  corm_put(hd, "zeta", &a);
  corm_put(hd, "alpha", &b);
  corm_put(hd, "beta", &c);

  /* All keys should be findable */
  uint32_t pz = corm_pos(hd, "zeta");
  uint32_t pa = corm_pos(hd, "alpha");
  uint32_t pb = corm_pos(hd, "beta");
  ASSERT(pz != UINT32_MAX, "zeta found");
  ASSERT(pa != UINT32_MAX, "alpha found");
  ASSERT(pb != UINT32_MAX, "beta found");
  ASSERT(pz != pa && pa != pb && pz != pb, "different positions");

  /* Bidirectional consistency: pos -> key -> pos */
  const char *kz = corm_get_key(hd, pz);
  ASSERT(kz && strcmp(kz, "zeta") == 0, "get_key back to zeta");
  ASSERT(corm_pos(hd, kz) == pz, "round-trip consistent");

  /* Nonexistent key */
  ASSERT(corm_pos(hd, "nonexistent") == UINT32_MAX,
         "nonexistent key returns UINT32_MAX");

  /* Empty map */
  uint32_t empty_hd = corm_open(NULL, NULL, CM_STR, corm_record_type_id(rec),
                                 TEST_MASK, CM_RECORD(rec));
  ASSERT(empty_hd != CM_MISS, "empty map opened");
  ASSERT(corm_pos(empty_hd, "anything") == UINT32_MAX,
         "empty map returns UINT32_MAX");
  corm_close(empty_hd);

  corm_close(hd);
}

/* ── Test 29: corm_inv_get edge cases ──────────────────────────────────── */

static void test_corm_inv_get_edge(void)
{
  printf("=== corm_inv_get edge cases ===\n");

  corm_record_field_t f[] = {
    { "id", CM_STR, offsetof(ref_source_t, id), sizeof(((ref_source_t*)0)->id) , 0, 0, NULL },
  };
  uint32_t rec = corm_record_register("invedge", sizeof(ref_source_t), f, 1);

  corm_record_field_t sf[] = {
    { "id", CM_STR, offsetof(ref_source_t, id), sizeof(((ref_source_t*)0)->id) , 0, 0, NULL },
        {
      .name = "ref",
      .type = CM_REFERENCE,
      .offset = offsetof(ref_source_t, single),
      .max_size = sizeof(((ref_source_t*)0)->single),
      .target_record = rec,
      .inverse = NULL,
    },
  };
  uint32_t srec = corm_record_register("invedge_src", sizeof(ref_source_t), sf, 2);

  uint32_t thd = corm_open(NULL, NULL, CM_STR, corm_record_type_id(rec),
                            TEST_MASK, CM_RECORD(rec));
  uint32_t shd = corm_open(NULL, NULL, CM_STR, corm_record_type_id(srec),
                            TEST_MASK, CM_RECORD(srec));
  corm_record_field_set_target_hd(srec, "ref", thd);

  /* Empty map — no inverse */
  uint32_t inv[4];
  size_t n = corm_inv_get(shd, "ref", 42, inv, 4);
  ASSERT(n == 0, "empty map returns 0");

  /* Non-record map */
  uint32_t plain_hd = corm_open(NULL, NULL, CM_STR, CM_STR, TEST_MASK, 0);
  n = corm_inv_get(plain_hd, "ref", 42, inv, 4);
  ASSERT(n == 0, "non-record map returns 0");

  /* max=0 */
  corm_put(thd, "t", &(ref_target_t){ .label="t" });
  corm_put(shd, "s:id", "s");
  corm_put(shd, "s:ref", "");
  n = corm_inv_get(shd, "ref", 0, inv, 0);
  ASSERT(n == 0, "max=0 returns 0");

  corm_close(plain_hd);
  corm_close(shd);
  corm_close(thd);
}

/* ── Test 30: Reference null behavior ────────────────────────────────── */

static void test_ref_null_behavior(void)
{
  printf("=== Reference null behavior ===\n");

  corm_record_field_t f[] = {
    { "id", CM_STR, offsetof(ref_source_t, id), sizeof(((ref_source_t*)0)->id) , 0, 0, NULL },
    { "ref", CM_REFERENCE, offsetof(ref_source_t, single), sizeof(((ref_source_t*)0)->single) , 0, 0, NULL },
  };
  uint32_t rec = corm_record_register("nulltest", sizeof(ref_source_t), f, 2);
  uint32_t hd = corm_open(NULL, NULL, CM_STR, corm_record_type_id(rec),
                           TEST_MASK, CM_RECORD(rec));
  ASSERT(hd != CM_MISS, "map opened");

  /* Create struct via id field put — ref starts at empty string */
  corm_put(hd, "x:id", "x");
  const char *got = corm_get(hd, "x:ref");
  ASSERT(got != NULL && got[0] == '\0', "initial ref is empty string");

  corm_close(hd);
}

/* ── Test: CM_VSTR variable-length string field ──────────────────────── */

typedef struct {
  char name[64];
  uint32_t age;
} vstr_person_t;  /* note: no inline storage for bio field */

static void test_vstr_field(void)
{
  printf("=== CM_VSTR variable-length string field ===\n");

  corm_record_field_t fields[] = {
    { "name", CM_STR, offsetof(vstr_person_t, name), sizeof(((vstr_person_t*)0)->name) , 0, 0, NULL },
    { "age",  CM_U32, offsetof(vstr_person_t, age),  sizeof(uint32_t) , 0, 0, NULL },
    { "bio",  CM_VSTR, 0, 0 , 0, 0, NULL },  /* variable string, no inline storage */
  };
  uint32_t rec = corm_record_register("vstr_test", sizeof(vstr_person_t), fields, 3);
  uint32_t hd = corm_open(NULL, NULL, CM_STR, corm_record_type_id(rec),
                          TEST_MASK, CM_RECORD(rec));
  ASSERT(hd != CM_MISS, "map opened");

  /* Put whole struct with name and age */
  vstr_person_t p = { .name = "Alice", .age = 30 };
  corm_put(hd, "alice", &p);

  /* Verify whole-struct get */
  const vstr_person_t *got = corm_get(hd, "alice");
  ASSERT(got != NULL, "whole-struct get returns non-NULL");
  ASSERT(strcmp(got->name, "Alice") == 0, "name matches");
  ASSERT(got->age == 30, "age matches");

  /* CM_VSTR field get before put returns NULL */
  const char *bio = corm_get(hd, "alice:bio");
  ASSERT(bio == NULL, "CM_VSTR get before put returns NULL");

  /* Put CM_VSTR field via composite key */
  corm_put(hd, "alice:bio", "Hello, this is a variable-length string!");

  /* Get CM_VSTR field */
  bio = corm_get(hd, "alice:bio");
  ASSERT(bio != NULL, "CM_VSTR get after put returns non-NULL");
  ASSERT(strcmp(bio, "Hello, this is a variable-length string!") == 0,
         "CM_VSTR field content matches");

  /* Whole struct is unchanged */
  got = corm_get(hd, "alice");
  ASSERT(got != NULL, "whole struct still exists");
  ASSERT(strcmp(got->name, "Alice") == 0, "name unchanged after vstr put");
  ASSERT(got->age == 30, "age unchanged after vstr put");

  /* CM_VSTR with a very long string (well beyond inline capacity) */
  char long_str[16384];
  memset(long_str, 'x', sizeof(long_str) - 1);
  long_str[sizeof(long_str) - 1] = '\0';
  corm_put(hd, "alice:bio", long_str);

  bio = corm_get(hd, "alice:bio");
  ASSERT(bio != NULL, "long vstr get returns non-NULL");
  ASSERT(strlen(bio) == sizeof(long_str) - 1, "long vstr correct length");
  ASSERT(bio[0] == 'x' && bio[sizeof(long_str) - 2] == 'x',
         "long vstr content matches");

  /* Normal fields still work alongside CM_VSTR */
  const uint32_t *age = corm_get(hd, "alice:age");
  ASSERT(age != NULL && *age == 30, "normal field still accessible");

  const char *name = corm_get(hd, "alice:name");
  ASSERT(name != NULL && strcmp(name, "Alice") == 0,
         "normal string field still accessible");

  /* Update CM_VSTR to new value */
  corm_put(hd, "alice:bio", "Updated bio text");
  bio = corm_get(hd, "alice:bio");
  ASSERT(bio != NULL && strcmp(bio, "Updated bio text") == 0,
         "CM_VSTR updated");

  /* Delete CM_VSTR field */
  corm_del(hd, "alice:bio");
  bio = corm_get(hd, "alice:bio");
  ASSERT(bio == NULL, "CM_VSTR returns NULL after delete");

  /* Struct still intact after vstr delete */
  got = corm_get(hd, "alice");
  ASSERT(got != NULL, "struct intact after vstr del");
  ASSERT(strcmp(got->name, "Alice") == 0, "name intact after vstr del");

  /* Second item with its own CM_VSTR */
  vstr_person_t p2 = { .name = "Bob", .age = 25 };
  corm_put(hd, "bob", &p2);
  corm_put(hd, "bob:bio", "Bob's biography");
  bio = corm_get(hd, "bob:bio");
  ASSERT(bio != NULL && strcmp(bio, "Bob's biography") == 0,
         "second item vstr works");
  bio = corm_get(hd, "alice:bio");
  ASSERT(bio == NULL, "alice vstr still deleted");

  /* corm_field_put also works with CM_VSTR */
  corm_field_put(hd, "alice", "bio", "Set via field_put");
  bio = corm_get(hd, "alice:bio");
  ASSERT(bio != NULL && strcmp(bio, "Set via field_put") == 0,
         "corm_field_put works with CM_VSTR");

  /* Non-existent vstr returns NULL */
  bio = corm_get(hd, "nonexistent:bio");
  ASSERT(bio == NULL, "non-existent item vstr returns NULL");

  /* Unknown field returns NULL (not CM_VSTR but unknown) */
  const void *bad = corm_get(hd, "alice:nonexistent");
  ASSERT(bad == NULL, "unknown field returns NULL");

  corm_close(hd);
}

int main(void)
{
  test_record_register();
  test_multiple_records();
  test_open_record_map();
  test_open_record_map_invalid();
  test_non_record_unaffected();
  test_whole_struct_put_get();
  test_field_get_composite();
  test_field_put_updates_struct();
  test_field_put_creates_struct();
  test_field_put_overwrite();
  test_delete_whole_struct();
  test_delete_field_zeros();
  test_string_truncation();
  test_overwrite_struct();
  test_assoc_record();
  test_multiple_items_fields();
  test_registration_errors();
  test_pointer_stability_growth();
  test_iteration();
  test_same_type_multi_records();
  test_u32_field_put_via_ptr();

  test_ref_field_get();
  test_multi_ref_field_get();
  test_single_ref_inverse();
  test_multi_ref_inverse();
  test_ref_field_del_cleanup();
  test_ref_whole_struct_del_cleanup();
  test_corm_get_key_api();
  test_corm_inv_get_edge();
  test_ref_null_behavior();
  test_corm_pos_api();

  test_vstr_field();

  printf("\n");
  if (errors == 0)
    printf("ALL RECORD TESTS PASSED\n");
  else
    printf("%u RECORD TEST(S) FAILED\n", errors);

  return errors == 0 ? 0 : 1;
}

