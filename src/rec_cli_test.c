/* rec_cli_test.c — unit tests for the kernel-owned axis-CLI decoders:
 * the read-only rec_spec_scan iterator, the bounded rec_cli_*_b parsers,
 * and rec_cli_str_dup. Exercises the exact grammar the five axis decodes
 * share (single-quoted values with backslash escapes, bare tokens, empty
 * values, tab/space separation). */

#include "./../include/ttypt/rec.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

unsigned errors = 0;

#define PASS() printf("  OK\n")
#define FAIL(msg) do { printf("  FAIL: %s [line %d]\n", msg, __LINE__); errors++; } while (0)
#define ASSERT(cond, msg) do { if (!(cond)) FAIL(msg); else PASS(); } while (0)

static void
test_scan(void)
{
	const char *cur, *k, *v;
	size_t klen, n;
	int q;

	printf("rec_spec_scan: pairs and values\n");
	cur = "dim=3 s=0,0,0 l=1,1,1";
	ASSERT(rec_spec_scan(&cur, &k, &klen, &v, &n, &q) == 1, "dim pair");
	ASSERT(rec_key_eq(k, klen, "dim"), "dim key exact");
	ASSERT(n == 1 && !q && v[0] == '3', "dim=3 val");
	ASSERT(rec_spec_scan(&cur, &k, &klen, &v, &n, &q) == 1
	       && rec_key_eq(k, klen, "s"), "s pair");
	ASSERT(n == 5 && !q, "s=0,0,0 val len");
	ASSERT(rec_spec_scan(&cur, &k, &klen, &v, &n, &q) == 1
	       && rec_key_eq(k, klen, "l"), "l pair");
	ASSERT(n == 5 && !q, "l=1,1,1 val len");
	ASSERT(rec_spec_scan(&cur, &k, &klen, &v, &n, &q) == 0, "end of spec");

	printf("rec_spec_scan: bare tokens skipped\n");
	cur = "junk dim=2 junk s=0,0 l=1,1";
	ASSERT(rec_spec_scan(&cur, &k, &klen, &v, &n, &q) == 1
	       && rec_key_eq(k, klen, "dim"), "first pair (dim)");
	ASSERT(rec_spec_scan(&cur, &k, &klen, &v, &n, &q) == 1
	       && rec_key_eq(k, klen, "s"), "second pair (s)");
	ASSERT(rec_spec_scan(&cur, &k, &klen, &v, &n, &q) == 1
	       && rec_key_eq(k, klen, "l") && n == 3 && strncmp(v, "1,1", 3) == 0,
	       "l=1,1 after bare");
	ASSERT(rec_spec_scan(&cur, &k, &klen, &v, &n, &q) == 0, "done");

	printf("rec_spec_scan: quoted value with escapes\n");
	cur = "query='harbor lights' m=10";
	ASSERT(rec_spec_scan(&cur, &k, &klen, &v, &n, &q) == 1
	       && rec_key_eq(k, klen, "query") && q, "query quoted");
	ASSERT(n == 13 && strncmp(v, "harbor lights", 13) == 0, "raw interior");
	ASSERT(rec_spec_scan(&cur, &k, &klen, &v, &n, &q) == 1
	       && rec_key_eq(k, klen, "m"), "m pair");
	ASSERT(n == 2 && !q && strncmp(v, "10", 2) == 0, "m=10");

	printf("rec_spec_scan: escaped quote inside quoted value\n");
	cur = "q='a\\'b' r=9";
	ASSERT(rec_spec_scan(&cur, &k, &klen, &v, &n, &q) == 1
	       && rec_key_eq(k, klen, "q"), "q pair");
	ASSERT(q && n == 4 && strncmp(v, "a\\'b", 4) == 0, "raw a\\'b");
	ASSERT(rec_spec_scan(&cur, &k, &klen, &v, &n, &q) == 1
	       && rec_key_eq(k, klen, "r") && n == 1, "r=9");

	printf("rec_spec_scan: tabs separate, empty value\n");
	cur = "a=1\tb=";
	ASSERT(rec_spec_scan(&cur, &k, &klen, &v, &n, &q) == 1
	       && rec_key_eq(k, klen, "a") && n == 1, "a=1");
	ASSERT(rec_spec_scan(&cur, &k, &klen, &v, &n, &q) == 1
	       && rec_key_eq(k, klen, "b") && n == 0, "b empty");
	ASSERT(rec_spec_scan(&cur, &k, &klen, &v, &n, &q) == 0, "done");

	printf("rec_spec_scan: unterminated quote consumes the tail\n");
	cur = "q='abc";
	ASSERT(rec_spec_scan(&cur, &k, &klen, &v, &n, &q) == 1
	       && rec_key_eq(k, klen, "q") && q && n == 3, "unterminated interior");
	ASSERT(rec_spec_scan(&cur, &k, &klen, &v, &n, &q) == 0, "done");
}

static void
test_parsers(void)
{
	int i;
	unsigned u;
	size_t z;
	float f;

	printf("rec_cli_int_b: bounded ranges\n");
	ASSERT(rec_cli_int_b("3", "3" + 1, &i) == 0 && i == 3, "3");
	ASSERT(rec_cli_int_b("-2", "-2" + 2, &i) == 0 && i == -2, "-2");
	/* boundary byte is a separator, not the string end */
	ASSERT(rec_cli_int_b("3 \t", "3 \t" + 1, &i) == 0 && i == 3,
	       "stops at space");
	ASSERT(rec_cli_int_b("12abc", "12abc" + 5, &i) == -1, "trailing junk");
	ASSERT(rec_cli_int_b("", "" + 0, &i) == -1, "empty");
	ASSERT(rec_cli_int_b("99999999999", "99999999999" + 11, &i) == -1,
	       "overflow");
	ASSERT(rec_cli_int_b("x1", "x1" + 2, &i) == -1, "non-numeric");

	printf("rec_cli_uint_b: sign guard + range\n");
	ASSERT(rec_cli_uint_b("7", "7" + 1, &u) == 0 && u == 7, "7");
	ASSERT(rec_cli_uint_b("-7", "-7" + 2, &u) == -1, "-7 rejected");

	printf("rec_cli_size_b\n");
	ASSERT(rec_cli_size_b("42", "42" + 2, &z) == 0 && z == 42, "42");

	printf("rec_cli_float_b\n");
	ASSERT(rec_cli_float_b("0.5", "0.5" + 3, &f) == 0 && f == 0.5f, "0.5");
	ASSERT(rec_cli_float_b("1e2", "1e2" + 3, &f) == 0 && f == 100.0f, "1e2");
	ASSERT(rec_cli_float_b("0.9e99x", "0.9e99x" + 6, &f) == -1, "overflow");

	printf("rec_cli_int/uint/size/float: NUL wrappers still parse\n");
	ASSERT(rec_cli_int("42", &i) == 0 && i == 42, "rec_cli_int");
	ASSERT(rec_cli_uint("7", &u) == 0 && u == 7, "rec_cli_uint");
	ASSERT(rec_cli_size("42", &z) == 0 && z == 42, "rec_cli_size");
	ASSERT(rec_cli_float("0.25", &f) == 0 && f == 0.25f, "rec_cli_float");
}

static void
test_str_dup(void)
{
	char *s;

	printf("rec_cli_str_dup: unescape + bare\n");
	ASSERT(rec_cli_str_dup("a\\'b", 4, 1, &s) == 0, "alloc quoted");
	if (!errors && s) {
		ASSERT(strcmp(s, "a'b") == 0, "\\' unescaped");
		free(s);
	}
	ASSERT(rec_cli_str_dup("a\\\\b", 4, 1, &s) == 0, "alloc backslash");
	if (!errors && s) {
		ASSERT(strcmp(s, "a\\b") == 0, "\\\\ unescaped");
		free(s);
	}
	ASSERT(rec_cli_str_dup("text", 4, 0, &s) == 0, "alloc bare");
	if (!errors && s) {
		ASSERT(strcmp(s, "text") == 0, "bare copied");
		free(s);
	}
	ASSERT(rec_cli_str_dup("ab\\", 3, 1, &s) == 0, "trailing lone backslash");
	if (!errors && s) {
		ASSERT(strcmp(s, "ab\\") == 0, "trailing backslash literal");
		free(s);
	}
	ASSERT(rec_cli_str_dup(NULL, 0, 0, &s) == -1, "NULL rejected");
}

int main(void)
{
	test_scan();
	test_parsers();
	test_str_dup();
	if (errors) {
		printf("%u FAILURES\n", errors);
		return 1;
	}
	printf("All rec_cli tests passed!\n");
	return 0;
}