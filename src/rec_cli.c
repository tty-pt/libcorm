/**
 * @file rec_cli.c
 * @brief Kernel-owned axis CLI ABI: decode-spec scanner + strict value
 *        parsers (rec.h @ref rec_axis_cli).
 *
 * These are the SINGLE implementation of the D14/D15B shared helpers. An
 * axis library defines only its option table (rec_axis_cli_options) and
 * its per-field mapping; everything else — the struct layout, the
 * decode-spec grammar, the strict value parsing, the owned-string setter —
 * lives here in the kernel and is shared by every TU that includes
 * <ttypt/rec.h>. Never re-declare any of this locally.
 */
#include <ttypt/rec.h>

#include <errno.h>
#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

int
rec_cli_str_set(char **dst, const char *value)
{
	char *copy;

	if (!dst || !value)
		return -1;
	copy = strdup(value);
	if (!copy)
		return -1;
	free(*dst);
	*dst = copy;
	return 0;
}

int
rec_spec_scan(const char **cur, const char **key, size_t *klen,
              const char **val, size_t *vlen, int *quoted)
{
	const char *p, *s;

	if (!cur || !*cur)
		return 0;
	p = *cur;
	for (;;) {
		while (*p == ' ' || *p == '\t')
			p++;
		if (!*p) {
			*cur = p;
			return 0;
		}
		s = p;
		while (*p && *p != ' ' && *p != '\t' && *p != '=')
			p++;
		if (key)
			*key = s;
		if (klen)
			*klen = (size_t)(p - s);
		if (*p == '=')
			break;
		if (*p)                 /* bare token: skip, keep scanning */
			p++;
	}
	p++;                        /* past the '=' */
	s = p;
	if (*p == '\'') {
		p++;
		s = p;
		while (*p && *p != '\'') {
			if (*p == '\\' && p[1])
				p++;
			p++;
		}
		if (val)
			*val = s;
		if (vlen)
			*vlen = (size_t)(p - s);
		if (quoted)
			*quoted = 1;
		if (*p)                 /* past the closing quote */
			p++;
	} else {
		while (*p && *p != ' ' && *p != '\t')
			p++;
		if (val)
			*val = s;
		if (vlen)
			*vlen = (size_t)(p - s);
		if (quoted)
			*quoted = 0;
	}
	*cur = p;
	return 1;
}
int
rec_cli_int_b(const char *v, const char *v_end, int *out)
{
	char *end = NULL;
	long n;

	if (!v || !v_end || !out || v_end <= v)
		return -1;
	errno = 0;
	n = strtol(v, &end, 10);
	if (errno == ERANGE || end == v || end != v_end ||
	    n < INT_MIN || n > INT_MAX)
		return -1;
	*out = (int)n;
	return 0;
}

int
rec_cli_int(const char *v, int *out)
{
	if (!v || !out)
		return -1;
	return rec_cli_int_b(v, v + strlen(v), out);
}

int
rec_cli_uint_b(const char *v, const char *v_end, unsigned *out)
{
	char *end = NULL;
	unsigned long n;

	if (!v || !v_end || !out || v_end <= v || v[0] == '-')
		return -1;
	errno = 0;
	n = strtoul(v, &end, 10);
	if (errno == ERANGE || end == v || end != v_end || n > UINT_MAX)
		return -1;
	*out = (unsigned)n;
	return 0;
}

int
rec_cli_uint(const char *v, unsigned *out)
{
	if (!v || !out)
		return -1;
	return rec_cli_uint_b(v, v + strlen(v), out);
}

int
rec_cli_size_b(const char *v, const char *v_end, size_t *out)
{
	char *end = NULL;
	unsigned long long n;

	if (!v || !v_end || !out || v_end <= v || v[0] == '-')
		return -1;
	errno = 0;
	n = strtoull(v, &end, 10);
	if (errno == ERANGE || end == v || end != v_end || n > SIZE_MAX)
		return -1;
	*out = (size_t)n;
	return 0;
}

int
rec_cli_size(const char *v, size_t *out)
{
	if (!v || !out)
		return -1;
	return rec_cli_size_b(v, v + strlen(v), out);
}

int
rec_cli_float_b(const char *v, const char *v_end, float *out)
{
	char *end = NULL;
	float f;

	if (!v || !v_end || !out || v_end <= v)
		return -1;
	errno = 0;
	f = strtof(v, &end);
	if (end == v || end != v_end)
		return -1;
	/* magnitude overflow rejects; underflow → ±0 is accepted */
	if (errno == ERANGE && f != 0.0f)
		return -1;
	*out = f;
	return 0;
}

int
rec_cli_float(const char *v, float *out)
{
	if (!v || !out)
		return -1;
	return rec_cli_float_b(v, v + strlen(v), out);
}

int
rec_cli_str_dup(const char *v, size_t n, int quoted, char **out)
{
	char *copy, *w;
	size_t i;

	if (!v || !out)
		return -1;
	copy = malloc(n + 1);
	if (!copy)
		return -1;
	if (quoted) {
		for (i = 0, w = copy; i < n; i++) {
			if (v[i] == '\\' && i + 1 < n)
				i++;
			*w++ = v[i];
		}
		*w = '\0';
	} else {
		memcpy(copy, v, n);
		copy[n] = '\0';
	}
	*out = copy;
	return 0;
}