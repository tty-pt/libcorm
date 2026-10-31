/*
 * htok.c — Native C HTOK v4 binary encoder and decoder for libcorm
 *
 * Implements corm_htok_pack and corm_htok_unpack:
 * - LEB128 varint encode / decode
 * - 1-byte triad packing (level 2-bit, lifecycle 3-bit, type 3-bit)
 * - 1-byte VarTag packing (field ID 5-bit, type tag 3-bit)
 * - Delta-compressed sorted citation arrays
 * - 16-byte raw UUID compression
 * - Self-contained, robust JSON parser and serializer with zero external dependencies
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <ctype.h>
#include <time.h>
#include "ttypt/corm.h"

/* --- LEB128 Varint Helpers --- */

static size_t htok_leb128_write(uint8_t *buf, uint64_t val) {
	size_t len = 0;
	do {
		uint8_t byte = (uint8_t)(val & 0x7F);
		val >>= 7;
		if (val != 0) {
			byte |= 0x80;
		}
		buf[len++] = byte;
	} while (val != 0);
	return len;
}

static size_t htok_leb128_read(const uint8_t *buf, size_t max_len, uint64_t *val) {
	uint64_t result = 0;
	size_t shift = 0;
	size_t i = 0;
	while (i < max_len) {
		uint8_t byte = buf[i++];
		result |= ((uint64_t)(byte & 0x7F)) << shift;
		if (!(byte & 0x80)) {
			*val = result;
			return i;
		}
		shift += 7;
		if (shift >= 64) return 0;
	}
	return 0;
}

/* --- UUID Helpers --- */

static int htok_uuid_from_str(const char *str, size_t len, uint8_t out[16]) {
	if (len != 36) return 0;
	if (str[8] != '-' || str[13] != '-' || str[18] != '-' || str[23] != '-') return 0;
	int hex_idx = 0;
	for (size_t i = 0; i < len; i++) {
		if (str[i] == '-') continue;
		char c = str[i];
		int val;
		if (c >= '0' && c <= '9') val = c - '0';
		else if (c >= 'a' && c <= 'f') val = c - 'a' + 10;
		else if (c >= 'A' && c <= 'F') val = c - 'A' + 10;
		else return 0;

		if (hex_idx % 2 == 0) {
			out[hex_idx / 2] = (uint8_t)(val << 4);
		} else {
			out[hex_idx / 2] |= (uint8_t)val;
		}
		hex_idx++;
	}
	return (hex_idx == 32) ? 1 : 0;
}

static void htok_uuid_to_str(const uint8_t bytes[16], char out[37]) {
	static const char hex[] = "0123456789abcdef";
	int p = 0;
	for (int i = 0; i < 16; i++) {
		if (i == 4 || i == 6 || i == 8 || i == 10) {
			out[p++] = '-';
		}
		out[p++] = hex[(bytes[i] >> 4) & 0x0F];
		out[p++] = hex[bytes[i] & 0x0F];
	}
	out[36] = '\0';
}

/* --- Dynamic Buffer --- */

typedef struct {
	uint8_t *data;
	size_t len;
	size_t cap;
} htok_buf_t;

static void htok_buf_init(htok_buf_t *b) {
	b->data = NULL;
	b->len = 0;
	b->cap = 0;
}

static int htok_buf_grow(htok_buf_t *b, size_t needed) {
	if (b->len + needed <= b->cap) return 1;
	size_t new_cap = b->cap ? b->cap * 2 : 256;
	while (new_cap < b->len + needed) new_cap *= 2;
	uint8_t *p = realloc(b->data, new_cap);
	if (!p) return 0;
	b->data = p;
	b->cap = new_cap;
	return 1;
}

static int htok_buf_append(htok_buf_t *b, const void *data, size_t len) {
	if (!htok_buf_grow(b, len)) return 0;
	memcpy(b->data + b->len, data, len);
	b->len += len;
	return 1;
}

static int htok_buf_append_byte(htok_buf_t *b, uint8_t byte) {
	return htok_buf_append(b, &byte, 1);
}

static int htok_buf_append_varint(htok_buf_t *b, uint64_t val) {
	uint8_t tmp[10];
	size_t n = htok_leb128_write(tmp, val);
	return htok_buf_append(b, tmp, n);
}

/* --- Known Field Mapping --- */

static const struct {
	const char *name;
	uint8_t id;
} CORM_HTOK_FIELDS[] = {
	{"text", 0},
	{"tags", 1},
	{"ctx_proj", 2},
	{"ctx_sub", 3},
	{"ctx_mod", 4},
	{"ctx_path", 5},
	{"ctx_subj", 6},
	{"ctx_grid", 7},
	{"ep_req", 8},
	{"ep_prob", 9},
	{"ep_out", 10},
	{"ep_find", 11},
	{"ep_unres", 12},
	{"ep_diff", 13},
	{"ep_step", 14},
	{"ep_obs", 15},
	{"ep_eid", 16},
	{"ep_app", 17},
	{"ep_sid", 18},
	{"ep_sfl", 19},
	{"ep_lid", 20},
	{"ep_rid", 21},
	{"ep_tid", 22},
	{"ep_env", 23},
	{"util", 24},
	{"hist", 25},
	{"ep_pr", 26},
	{"ep_ap", 27},
	{"standing", 28},
	{NULL, 0}
};

static int htok_field_id_by_name(const char *name) {
	for (int i = 0; CORM_HTOK_FIELDS[i].name; i++) {
		if (!strcmp(CORM_HTOK_FIELDS[i].name, name)) {
			return CORM_HTOK_FIELDS[i].id;
		}
	}
	return -1;
}

static const char *htok_field_name_by_id(uint8_t id) {
	for (int i = 0; CORM_HTOK_FIELDS[i].name; i++) {
		if (CORM_HTOK_FIELDS[i].id == id) {
			return CORM_HTOK_FIELDS[i].name;
		}
	}
	return NULL;
}

/* --- Minimal JSON DOM Parser --- */

typedef enum {
	J_NULL,
	J_BOOL,
	J_NUM,
	J_STR,
	J_ARR,
	J_OBJ
} jtype_t;

typedef struct jval_s jval_t;

typedef struct {
	char *k;
	jval_t *v;
} jmember_t;

struct jval_s {
	jtype_t type;
	union {
		int b;
		double n;
		char *s;
		struct { jval_t **items; size_t count; size_t cap; } arr;
		struct { jmember_t *m; size_t count; size_t cap; } obj;
	} u;
};

static void jval_free(jval_t *v) {
	if (!v) return;
	if (v->type == J_STR) {
		free(v->u.s);
	} else if (v->type == J_ARR) {
		for (size_t i = 0; i < v->u.arr.count; i++) jval_free(v->u.arr.items[i]);
		free(v->u.arr.items);
	} else if (v->type == J_OBJ) {
		for (size_t i = 0; i < v->u.obj.count; i++) {
			free(v->u.obj.m[i].k);
			jval_free(v->u.obj.m[i].v);
		}
		free(v->u.obj.m);
	}
	free(v);
}

static void skip_ws(const char **p) {
	while (**p && ((unsigned char)**p <= ' ' || **p == '\r' || **p == '\n' || **p == '\t')) (*p)++;
}

static char *parse_str(const char **p) {
	if (**p != '"') return NULL;
	(*p)++;
	htok_buf_t b;
	htok_buf_init(&b);

	while (**p && **p != '"') {
		if (**p == '\\') {
			(*p)++;
			if (!**p) break;
			char c = **p;
			if (c == '"' || c == '\\' || c == '/') htok_buf_append_byte(&b, (uint8_t)c);
			else if (c == 'b') htok_buf_append_byte(&b, '\b');
			else if (c == 'f') htok_buf_append_byte(&b, '\f');
			else if (c == 'n') htok_buf_append_byte(&b, '\n');
			else if (c == 'r') htok_buf_append_byte(&b, '\r');
			else if (c == 't') htok_buf_append_byte(&b, '\t');
			else if (c == 'u') {
				/* 4-digit hex escape */
				unsigned int codepoint = 0;
				for (int i = 0; i < 4 && (*p)[1]; i++) {
					(*p)++;
					char hc = **p;
					int hv = (hc >= '0' && hc <= '9') ? hc - '0' :
					         (hc >= 'a' && hc <= 'f') ? hc - 'a' + 10 :
					         (hc >= 'A' && hc <= 'F') ? hc - 'A' + 10 : 0;
					codepoint = (codepoint << 4) | hv;
				}
				if (codepoint <= 0x7F) {
					htok_buf_append_byte(&b, (uint8_t)codepoint);
				} else if (codepoint <= 0x7FF) {
					htok_buf_append_byte(&b, (uint8_t)(0xC0 | (codepoint >> 6)));
					htok_buf_append_byte(&b, (uint8_t)(0x80 | (codepoint & 0x3F)));
				} else {
					htok_buf_append_byte(&b, (uint8_t)(0xE0 | (codepoint >> 12)));
					htok_buf_append_byte(&b, (uint8_t)(0x80 | ((codepoint >> 6) & 0x3F)));
					htok_buf_append_byte(&b, (uint8_t)(0x80 | (codepoint & 0x3F)));
				}
			} else {
				htok_buf_append_byte(&b, (uint8_t)c);
			}
			(*p)++;
		} else {
			htok_buf_append_byte(&b, (uint8_t)**p);
			(*p)++;
		}
	}
	if (**p == '"') (*p)++;
	htok_buf_append_byte(&b, 0);
	return (char *)b.data;
}

static jval_t *parse_val(const char **p);

static jval_t *parse_arr(const char **p) {
	if (**p != '[') return NULL;
	(*p)++;
	jval_t *v = calloc(1, sizeof(jval_t));
	if (!v) return NULL;
	v->type = J_ARR;

	skip_ws(p);
	if (**p == ']') {
		(*p)++;
		return v;
	}

	while (**p) {
		skip_ws(p);
		jval_t *item = parse_val(p);
		if (!item) break;
		if (v->u.arr.count >= v->u.arr.cap) {
			size_t ncap = v->u.arr.cap ? v->u.arr.cap * 2 : 8;
			jval_t **nitems = realloc(v->u.arr.items, ncap * sizeof(jval_t *));
			if (!nitems) { jval_free(item); break; }
			v->u.arr.items = nitems;
			v->u.arr.cap = ncap;
		}
		v->u.arr.items[v->u.arr.count++] = item;
		skip_ws(p);
		if (**p == ',') {
			(*p)++;
		} else if (**p == ']') {
			(*p)++;
			break;
		} else {
			break;
		}
	}
	return v;
}

static jval_t *parse_obj(const char **p) {
	if (**p != '{') return NULL;
	(*p)++;
	jval_t *v = calloc(1, sizeof(jval_t));
	if (!v) return NULL;
	v->type = J_OBJ;

	skip_ws(p);
	if (**p == '}') {
		(*p)++;
		return v;
	}

	while (**p) {
		skip_ws(p);
		if (**p != '"') break;
		char *key = parse_str(p);
		if (!key) break;
		skip_ws(p);
		if (**p != ':') { free(key); break; }
		(*p)++;
		skip_ws(p);
		jval_t *val = parse_val(p);
		if (!val) { free(key); break; }

		if (v->u.obj.count >= v->u.obj.cap) {
			size_t ncap = v->u.obj.cap ? v->u.obj.cap * 2 : 8;
			jmember_t *nm = realloc(v->u.obj.m, ncap * sizeof(jmember_t));
			if (!nm) { free(key); jval_free(val); break; }
			v->u.obj.m = nm;
			v->u.obj.cap = ncap;
		}
		v->u.obj.m[v->u.obj.count].k = key;
		v->u.obj.m[v->u.obj.count].v = val;
		v->u.obj.count++;

		skip_ws(p);
		if (**p == ',') {
			(*p)++;
		} else if (**p == '}') {
			(*p)++;
			break;
		} else {
			break;
		}
	}
	return v;
}

static jval_t *parse_val(const char **p) {
	skip_ws(p);
	if (!**p) return NULL;

	if (**p == '"') {
		char *s = parse_str(p);
		if (!s) return NULL;
		jval_t *v = calloc(1, sizeof(jval_t));
		if (!v) { free(s); return NULL; }
		v->type = J_STR;
		v->u.s = s;
		return v;
	}
	if (**p == '[') return parse_arr(p);
	if (**p == '{') return parse_obj(p);
	if (!strncmp(*p, "true", 4)) {
		*p += 4;
		jval_t *v = calloc(1, sizeof(jval_t));
		if (!v) return NULL;
		v->type = J_BOOL; v->u.b = 1;
		return v;
	}
	if (!strncmp(*p, "false", 5)) {
		*p += 5;
		jval_t *v = calloc(1, sizeof(jval_t));
		if (!v) return NULL;
		v->type = J_BOOL; v->u.b = 0;
		return v;
	}
	if (!strncmp(*p, "null", 4)) {
		*p += 4;
		jval_t *v = calloc(1, sizeof(jval_t));
		if (!v) return NULL;
		v->type = J_NULL;
		return v;
	}
	if (**p == '-' || isdigit((unsigned char)**p)) {
		char *end = NULL;
		double n = strtod(*p, &end);
		if (end && end != *p) {
			*p = end;
			jval_t *v = calloc(1, sizeof(jval_t));
			if (!v) return NULL;
			v->type = J_NUM; v->u.n = n;
			return v;
		}
	}
	return NULL;
}

static jval_t *json_find(const jval_t *obj, const char *key) {
	if (!obj || obj->type != J_OBJ) return NULL;
	for (size_t i = 0; i < obj->u.obj.count; i++) {
		if (!strcmp(obj->u.obj.m[i].k, key)) return obj->u.obj.m[i].v;
	}
	return NULL;
}

static const char *json_find_str(const jval_t *obj, const char *key) {
	jval_t *v = json_find(obj, key);
	return (v && v->type == J_STR) ? v->u.s : NULL;
}

static double json_find_num(const jval_t *obj, const char *key, double def) {
	jval_t *v = json_find(obj, key);
	return (v && v->type == J_NUM) ? v->u.n : def;
}

static int json_find_bool(const jval_t *obj, const char *key, int def) {
	jval_t *v = json_find(obj, key);
	return (v && v->type == J_BOOL) ? v->u.b : def;
}

/* --- JSON String Serialization Helper --- */

typedef struct {
	char *data;
	size_t len;
	size_t cap;
} strbuf_t;

static void strbuf_init(strbuf_t *sb) {
	sb->data = NULL;
	sb->len = 0;
	sb->cap = 0;
}

static void strbuf_free(strbuf_t *sb) {
	free(sb->data);
	sb->data = NULL;
	sb->len = 0;
	sb->cap = 0;
}

static int strbuf_append(strbuf_t *sb, const char *s, size_t len) {
	if (sb->len + len + 1 > sb->cap) {
		size_t ncap = sb->cap ? sb->cap * 2 : 256;
		while (ncap < sb->len + len + 1) ncap *= 2;
		char *p = realloc(sb->data, ncap);
		if (!p) return 0;
		sb->data = p;
		sb->cap = ncap;
	}
	memcpy(sb->data + sb->len, s, len);
	sb->len += len;
	sb->data[sb->len] = '\0';
	return 1;
}

static int strbuf_puts(strbuf_t *sb, const char *s) {
	return strbuf_append(sb, s, strlen(s));
}

static int strbuf_put_json_str(strbuf_t *sb, const char *s) {
	if (!strbuf_append(sb, "\"", 1)) return 0;
	for (const char *p = s; *p; p++) {
		unsigned char c = (unsigned char)*p;
		if (c == '"') { if (!strbuf_puts(sb, "\\\"")) return 0; }
		else if (c == '\\') { if (!strbuf_puts(sb, "\\\\")) return 0; }
		else if (c == '\b') { if (!strbuf_puts(sb, "\\b")) return 0; }
		else if (c == '\f') { if (!strbuf_puts(sb, "\\f")) return 0; }
		else if (c == '\n') { if (!strbuf_puts(sb, "\\n")) return 0; }
		else if (c == '\r') { if (!strbuf_puts(sb, "\\r")) return 0; }
		else if (c == '\t') { if (!strbuf_puts(sb, "\\t")) return 0; }
		else if (c < 0x20) {
			char esc[8];
			snprintf(esc, sizeof(esc), "\\u%04x", c);
			if (!strbuf_puts(sb, esc)) return 0;
		} else {
			if (!strbuf_append(sb, (const char *)p, 1)) return 0;
		}
	}
	return strbuf_append(sb, "\"", 1);
}

/* --- Field Entry for Packing --- */

typedef struct {
	char *name;
	uint8_t type_tag;
	uint8_t *data;
	size_t len;
} field_entry_t;

/* Compare function for sorting uint64_t ascending */
static int cmp_u64(const void *a, const void *b) {
	uint64_t va = *(const uint64_t *)a;
	uint64_t vb = *(const uint64_t *)b;
	return (va > vb) - (va < vb);
}

static void add_field_str(field_entry_t *fields, size_t *count, const char *fname, const char *sval) {
	if (sval && sval[0] && *count < 64) {
		fields[*count].name = strdup(fname);
		fields[*count].type_tag = CORM_HTOK_TYPE_UTF8;
		fields[*count].len = strlen(sval);
		fields[*count].data = (uint8_t *)strdup(sval);
		(*count)++;
	}
}

static field_entry_t *get_field(field_entry_t *fields, size_t count, const char *fname) {
	for (size_t i = 0; i < count; i++) {
		if (!strcmp(fields[i].name, fname)) return &fields[i];
	}
	return NULL;
}

/* --- corm_htok_pack Implementation --- */

int corm_htok_pack(const char *json_str, size_t json_len, uint8_t **out_bytes, size_t *out_len) {
	if (!json_str || !out_bytes || !out_len) return -1;
	if (json_len == 0) json_len = strlen(json_str);

	const char *cursor = json_str;
	jval_t *root = parse_val(&cursor);
	if (!root || root->type != J_OBJ) {
		jval_free(root);
		return -2;
	}

	/* 1. Level */
	const char *lvl_s = json_find_str(root, "level");
	if (!lvl_s) lvl_s = json_find_str(root, "l");
	uint8_t level = 2; /* L2 */
	if (lvl_s) {
		if (!strcmp(lvl_s, "L0")) level = 0;
		else if (!strcmp(lvl_s, "L1")) level = 1;
		else if (!strcmp(lvl_s, "L2")) level = 2;
		else if (!strcmp(lvl_s, "L3")) level = 3;
	}

	/* 2. Lifecycle */
	const char *lc_s = json_find_str(root, "lifecycle");
	if (!lc_s) lc_s = json_find_str(root, "c");
	uint8_t lifecycle = 3; /* supported */
	if (lc_s) {
		if (!strcmp(lc_s, "observed")) lifecycle = 0;
		else if (!strcmp(lc_s, "hypothesized")) lifecycle = 1;
		else if (!strcmp(lc_s, "tested")) lifecycle = 2;
		else if (!strcmp(lc_s, "supported")) lifecycle = 3;
		else if (!strcmp(lc_s, "contradicted")) lifecycle = 4;
		else if (!strcmp(lc_s, "superseded")) lifecycle = 5;
	}

	/* 3. Type */
	const char *t_s = json_find_str(root, "type");
	if (!t_s) t_s = json_find_str(root, "t");
	uint8_t type = 5; /* note */
	if (t_s) {
		if (!strcmp(t_s, "episode")) type = 0;
		else if (!strcmp(t_s, "investigation")) type = 1;
		else if (!strcmp(t_s, "decision")) type = 2;
		else if (!strcmp(t_s, "invariant")) type = 3;
		else if (!strcmp(t_s, "pattern")) type = 4;
		else if (!strcmp(t_s, "note")) type = 5;
	}

	/* 4. Timestamp */
	uint32_t ts = 0;
	const char *date_s = json_find_str(root, "timestamp");
	if (!date_s) date_s = json_find_str(root, "date");
	if (date_s && *date_s) {
		struct tm tm_val;
		memset(&tm_val, 0, sizeof(tm_val));
		int y = 0, m = 0, d = 0, h = 12, min = 0, sec = 0;
		if (sscanf(date_s, "%d-%d-%dT%d:%d:%d", &y, &m, &d, &h, &min, &sec) >= 3 ||
		    sscanf(date_s, "%d-%d-%d", &y, &m, &d) == 3) {
			tm_val.tm_year = y - 1900;
			tm_val.tm_mon = m - 1;
			tm_val.tm_mday = d;
			tm_val.tm_hour = h;
			tm_val.tm_min = min;
			tm_val.tm_sec = sec;
			time_t t = timegm(&tm_val);
			if (t > 0) ts = (uint32_t)t;
		}
	}
	if (ts == 0) ts = (uint32_t)time(NULL);

	/* 5. Flags, supersedes, evidence */
	uint8_t flags = 0;
	uint64_t supersedes = 0;
	uint64_t supersededBy = 0;

	double sup_n = json_find_num(root, "supersedes", 0);
	if (sup_n == 0) sup_n = json_find_num(root, "p", 0);
	if (sup_n > 0) {
		flags |= CORM_HTOK_FLAG_HAS_SUPERSEDES;
		supersedes = (uint64_t)sup_n;
	}

	double supby_n = json_find_num(root, "supersededBy", 0);
	if (supby_n == 0) supby_n = json_find_num(root, "b", 0);
	if (supby_n > 0) {
		flags |= CORM_HTOK_FLAG_HAS_SUPERSEDED_BY;
		supersededBy = (uint64_t)supby_n;
	}

	int is_anchor = json_find_bool(root, "isAnchor", 0);
	if (!is_anchor) is_anchor = json_find_bool(root, "anchor", 0);
	int standing = json_find_bool(root, "standing", 0);
	if (!standing) standing = json_find_bool(root, "st", 0);
	if (type == 3) standing = 1;
	if (is_anchor || standing) flags |= CORM_HTOK_FLAG_IS_ANCHOR;

	/* Evidence array */
	jval_t *e_val = json_find(root, "evidence");
	if (!e_val) e_val = json_find(root, "e");
	uint64_t *ev_arr = NULL;
	size_t ev_count = 0;
	if (e_val && e_val->type == J_ARR && e_val->u.arr.count > 0) {
		ev_arr = malloc(e_val->u.arr.count * sizeof(uint64_t));
		if (ev_arr) {
			for (size_t i = 0; i < e_val->u.arr.count; i++) {
				if (e_val->u.arr.items[i]->type == J_NUM && e_val->u.arr.items[i]->u.n > 0) {
					ev_arr[ev_count++] = (uint64_t)e_val->u.arr.items[i]->u.n;
				}
			}
			if (ev_count > 0) {
				qsort(ev_arr, ev_count, sizeof(uint64_t), cmp_u64);
				flags |= CORM_HTOK_FLAG_HAS_EVIDENCE;
			} else {
				free(ev_arr);
				ev_arr = NULL;
			}
		}
	}

	/* 6. Collect Fields */
	field_entry_t fields[64];
	size_t field_count = 0;

	#define ADD_FIELD_STR(fname, sval) add_field_str(fields, &field_count, fname, sval)

	#define ADD_FIELD_ARR(fname, jarr) do { \
		if (jarr && jarr->type == J_ARR && jarr->u.arr.count > 0 && field_count < 64) { \
			strbuf_t sb; strbuf_init(&sb); \
			for (size_t ai = 0; ai < jarr->u.arr.count; ai++) { \
				if (jarr->u.arr.items[ai]->type == J_STR) { \
					if (ai > 0) strbuf_append(&sb, "\n", 1); \
					strbuf_puts(&sb, jarr->u.arr.items[ai]->u.s); \
				} \
			} \
			if (sb.len > 0) { \
				fields[field_count].name = strdup(fname); \
				fields[field_count].type_tag = CORM_HTOK_TYPE_UTF8; \
				fields[field_count].len = sb.len; \
				fields[field_count].data = (uint8_t *)sb.data; \
				field_count++; \
			} else { strbuf_free(&sb); } \
		} \
	} while (0)

	/* Searchable text (summary) */
	const char *summary = json_find_str(root, "summary");
	if (!summary) summary = json_find_str(root, "s");
	if (!summary) summary = json_find_str(root, "text");

	jval_t *ep = json_find(root, "episode");
	if (!ep) ep = json_find(root, "ep");

	if (!summary && ep) {
		summary = json_find_str(ep, "problem");
		if (!summary) summary = json_find_str(ep, "p");
		if (!summary) summary = json_find_str(ep, "request");
		if (!summary) summary = json_find_str(ep, "q");
	}
	ADD_FIELD_STR("text", summary);

	if (standing) {
		ADD_FIELD_STR("standing", "1");
	}

	/* Tags */
	jval_t *tags = json_find(root, "tags");
	if (!tags) tags = json_find(root, "g");
	ADD_FIELD_ARR("tags", tags);

	/* Context */
	jval_t *ctx = json_find(root, "context");
	if (!ctx) ctx = json_find(root, "x");
	if (ctx && ctx->type == J_OBJ) {
		const char *cp = json_find_str(ctx, "project");
		if (!cp) cp = json_find_str(ctx, "p");
		ADD_FIELD_STR("ctx_proj", cp);

		const char *cs = json_find_str(ctx, "subsystem");
		if (!cs) cs = json_find_str(ctx, "s");
		ADD_FIELD_STR("ctx_sub", cs);

		const char *cm = json_find_str(ctx, "module");
		if (!cm) cm = json_find_str(ctx, "m");
		ADD_FIELD_STR("ctx_mod", cm);

		const char *cpt = json_find_str(ctx, "path");
		if (!cpt) cpt = json_find_str(ctx, "pt");
		ADD_FIELD_STR("ctx_path", cpt);

		const char *csub = json_find_str(ctx, "subject");
		if (!csub) csub = json_find_str(ctx, "sub");
		ADD_FIELD_STR("ctx_subj", csub);

		jval_t *cgd = json_find(ctx, "grid");
		if (!cgd) cgd = json_find(ctx, "gd");
		if (cgd && cgd->type == J_OBJ) {
			double gx = json_find_num(cgd, "x", 0);
			double gy = json_find_num(cgd, "y", 0);
			char gbuf[64];
			jval_t *gz = json_find(cgd, "z");
			if (gz && gz->type == J_NUM) {
				snprintf(gbuf, sizeof(gbuf), "%g,%g,%g", gx, gy, gz->u.n);
			} else {
				snprintf(gbuf, sizeof(gbuf), "%g,%g", gx, gy);
			}
			ADD_FIELD_STR("ctx_grid", gbuf);
		}
	}

	/* Episode */
	if (ep && ep->type == J_OBJ) {
		const char *ep_req = json_find_str(ep, "request");
		if (!ep_req) ep_req = json_find_str(ep, "q");
		ADD_FIELD_STR("ep_req", ep_req);

		const char *ep_prob = json_find_str(ep, "problem");
		if (!ep_prob) ep_prob = json_find_str(ep, "p");
		ADD_FIELD_STR("ep_prob", ep_prob);

		const char *ep_out = json_find_str(ep, "outcome");
		if (!ep_out) ep_out = json_find_str(ep, "o");
		ADD_FIELD_STR("ep_out", ep_out);

		jval_t *finds = json_find(ep, "findings");
		if (!finds) finds = json_find(ep, "f");
		ADD_FIELD_ARR("ep_find", finds);

		jval_t *unres = json_find(ep, "unresolved");
		if (!unres) unres = json_find(ep, "u");
		ADD_FIELD_ARR("ep_unres", unres);

		jval_t *diffs = json_find(ep, "diffs");
		if (!diffs) diffs = json_find(ep, "d");
		ADD_FIELD_ARR("ep_diff", diffs);

		jval_t *steps = json_find(ep, "steps");
		if (!steps) steps = json_find(ep, "st");
		ADD_FIELD_ARR("ep_step", steps);

		jval_t *obs = json_find(ep, "observations");
		if (!obs) obs = json_find(ep, "ob");
		ADD_FIELD_ARR("ep_obs", obs);

		jval_t *eids = json_find(ep, "eventIds");
		if (!eids) eids = json_find(ep, "ei");
		ADD_FIELD_ARR("ep_eid", eids);

		/* Approaches */
		jval_t *apps = json_find(ep, "approaches");
		if (!apps) apps = json_find(ep, "a");
		if (apps && apps->type == J_ARR && apps->u.arr.count > 0 && field_count < 64) {
			strbuf_t sb; strbuf_init(&sb);
			for (size_t ai = 0; ai < apps->u.arr.count; ai++) {
				jval_t *item = apps->u.arr.items[ai];
				if (item->type == J_OBJ) {
					const char *outc = json_find_str(item, "outcome");
					if (!outc) outc = json_find_str(item, "o");
					const char *appr = json_find_str(item, "approach");
					if (!appr) appr = json_find_str(item, "a");
					const char *reas = json_find_str(item, "reason");
					if (!reas) reas = json_find_str(item, "r");

					if (ai > 0) strbuf_append(&sb, "\n", 1);
					strbuf_puts(&sb, outc ? outc : "worked");
					strbuf_append(&sb, "\t", 1);
					strbuf_puts(&sb, appr ? appr : "");
					strbuf_append(&sb, "\t", 1);
					strbuf_puts(&sb, reas ? reas : "");
				}
			}
			if (sb.len > 0) {
				fields[field_count].name = strdup("ep_app");
				fields[field_count].type_tag = CORM_HTOK_TYPE_UTF8;
				fields[field_count].len = sb.len;
				fields[field_count].data = (uint8_t *)sb.data;
				field_count++;
			} else { strbuf_free(&sb); }
		}

		/* Session ID (UUID compression) */
		const char *sid = json_find_str(ep, "sessionId");
		if (!sid) sid = json_find_str(ep, "s");
		if (sid && *sid && field_count < 64) {
			uint8_t ubytes[16];
			if (htok_uuid_from_str(sid, strlen(sid), ubytes)) {
				fields[field_count].name = strdup("ep_sid");
				fields[field_count].type_tag = CORM_HTOK_TYPE_UUID;
				fields[field_count].len = 16;
				fields[field_count].data = malloc(16);
				if (fields[field_count].data) memcpy(fields[field_count].data, ubytes, 16);
				field_count++;
			} else {
				ADD_FIELD_STR("ep_sid", sid);
			}
		}

		const char *sfl = json_find_str(ep, "sessionFile");
		if (!sfl) sfl = json_find_str(ep, "sf");
		ADD_FIELD_STR("ep_sfl", sfl);

		const char *lid = json_find_str(ep, "leafId");
		if (!lid) lid = json_find_str(ep, "l");
		ADD_FIELD_STR("ep_lid", lid);

		const char *rid = json_find_str(ep, "runId");
		if (!rid) rid = json_find_str(ep, "r");
		ADD_FIELD_STR("ep_rid", rid);

		const char *tid = json_find_str(ep, "toolCallId");
		if (!tid) tid = json_find_str(ep, "t");
		ADD_FIELD_STR("ep_tid", tid);

		const char *principle = json_find_str(ep, "principle");
		if (!principle) principle = json_find_str(ep, "pr");
		ADD_FIELD_STR("ep_pr", principle);

		const char *applicability = json_find_str(ep, "applicability");
		if (!applicability) applicability = json_find_str(ep, "ap");
		ADD_FIELD_STR("ep_ap", applicability);
	}

	/* 7. Encode Binary Container */
	htok_buf_t out;
	htok_buf_init(&out);

	/* Magic: 4B ("HTOK" little-endian: 0x4b, 0x4f, 0x54, 0x48) */
	htok_buf_append_byte(&out, 0x4b);
	htok_buf_append_byte(&out, 0x4f);
	htok_buf_append_byte(&out, 0x54);
	htok_buf_append_byte(&out, 0x48);

	/* Version: 2B (4 in LE) */
	htok_buf_append_byte(&out, 4);
	htok_buf_append_byte(&out, 0);

	/* Timestamp: 4B LE */
	htok_buf_append_byte(&out, (uint8_t)(ts & 0xff));
	htok_buf_append_byte(&out, (uint8_t)((ts >> 8) & 0xff));
	htok_buf_append_byte(&out, (uint8_t)((ts >> 16) & 0xff));
	htok_buf_append_byte(&out, (uint8_t)((ts >> 24) & 0xff));

	/* Triad: 1B */
	uint8_t triad = (uint8_t)(((level & 0x03) << 6) | ((lifecycle & 0x07) << 3) | (type & 0x07));
	htok_buf_append_byte(&out, triad);

	/* Flags: 1B */
	htok_buf_append_byte(&out, flags);

	/* Supersedes varint */
	if (flags & CORM_HTOK_FLAG_HAS_SUPERSEDES) {
		htok_buf_append_varint(&out, supersedes);
	}

	/* SupersededBy varint */
	if (flags & CORM_HTOK_FLAG_HAS_SUPERSEDED_BY) {
		htok_buf_append_varint(&out, supersededBy);
	}

	/* Evidence delta-citations */
	if (flags & CORM_HTOK_FLAG_HAS_EVIDENCE) {
		htok_buf_append_varint(&out, ev_count);
		if (ev_count > 0) {
			htok_buf_append_varint(&out, ev_arr[0]);
			for (size_t i = 1; i < ev_count; i++) {
				uint64_t delta = ev_arr[i] - ev_arr[i - 1];
				htok_buf_append_varint(&out, delta);
			}
		}
	}

	/* Field count varint */
	htok_buf_append_varint(&out, field_count);

	/* Fields */
	for (size_t i = 0; i < field_count; i++) {
		field_entry_t *f = &fields[i];
		int fid = htok_field_id_by_name(f->name);
		uint8_t var_tag;
		if (fid >= 0 && fid < 31) {
			var_tag = (uint8_t)(((fid & 0x1f) << 3) | (f->type_tag & 0x07));
			htok_buf_append_byte(&out, var_tag);
		} else {
			var_tag = (uint8_t)((31 << 3) | (f->type_tag & 0x07));
			htok_buf_append_byte(&out, var_tag);
			size_t nlen = strlen(f->name);
			htok_buf_append_byte(&out, (uint8_t)nlen);
			htok_buf_append(&out, f->name, nlen);
		}

		htok_buf_append_varint(&out, f->len);
		htok_buf_append(&out, f->data, f->len);
	}

	/* Trailer: 0xaa */
	htok_buf_append_byte(&out, 0xaa);

	/* Cleanup */
	for (size_t i = 0; i < field_count; i++) {
		free(fields[i].name);
		free(fields[i].data);
	}
	free(ev_arr);
	jval_free(root);

	*out_bytes = out.data;
	*out_len = out.len;
	return 0;
}

/* --- corm_htok_unpack Implementation --- */

int corm_htok_unpack(const uint8_t *bytes, size_t len, char **out_json, size_t *out_json_len) {
	if (!bytes || !out_json || !out_json_len) return -1;
	if (len < 12) return -2;

	/* Check Magic */
	uint32_t magic = ((uint32_t)bytes[0]) |
	                (((uint32_t)bytes[1]) << 8) |
	                (((uint32_t)bytes[2]) << 16) |
	                (((uint32_t)bytes[3]) << 24);
	if (magic != CORM_HTOK_MAGIC && strncmp((const char *)bytes, "HTOK", 4) != 0) {
		return -3;
	}

	uint16_t version = ((uint16_t)bytes[4]) | (((uint16_t)bytes[5]) << 8);
	if (version < 1 || version > 4) return -4;

	uint32_t ts = ((uint32_t)bytes[6]) |
	              (((uint32_t)bytes[7]) << 8) |
	              (((uint32_t)bytes[8]) << 16) |
	              (((uint32_t)bytes[9]) << 24);

	uint8_t triad = bytes[10];
	uint8_t flags = bytes[11];

	uint8_t level_idx = (triad >> 6) & 0x03;
	uint8_t lc_idx = (triad >> 3) & 0x07;
	uint8_t type_idx = triad & 0x07;

	static const char *LEVELS[] = {"L0", "L1", "L2", "L3"};
	static const char *LIFECYCLES[] = {"observed", "hypothesized", "tested", "supported", "contradicted", "superseded"};
	static const char *TYPES[] = {"episode", "investigation", "decision", "invariant", "pattern", "note"};

	const char *level = (level_idx < 4) ? LEVELS[level_idx] : "L2";
	const char *lifecycle = (lc_idx < 6) ? LIFECYCLES[lc_idx] : "supported";
	const char *type = (type_idx < 6) ? TYPES[type_idx] : "note";

	size_t offset = 12;
	uint64_t supersedes = 0;
	uint64_t supersededBy = 0;
	uint64_t *evidence = NULL;
	size_t evid_count = 0;

	if (flags & CORM_HTOK_FLAG_HAS_SUPERSEDES) {
		size_t nr = htok_leb128_read(bytes + offset, len - offset, &supersedes);
		if (!nr) return -5;
		offset += nr;
	}

	if (flags & CORM_HTOK_FLAG_HAS_SUPERSEDED_BY) {
		size_t nr = htok_leb128_read(bytes + offset, len - offset, &supersededBy);
		if (!nr) return -6;
		offset += nr;
	}

	if (flags & CORM_HTOK_FLAG_HAS_EVIDENCE) {
		uint64_t c = 0;
		size_t nr = htok_leb128_read(bytes + offset, len - offset, &c);
		if (!nr) return -7;
		offset += nr;
		evid_count = (size_t)c;
		if (evid_count > 0) {
			evidence = malloc(evid_count * sizeof(uint64_t));
			if (!evidence) return -8;
			uint64_t base = 0;
			nr = htok_leb128_read(bytes + offset, len - offset, &base);
			if (!nr) { free(evidence); return -9; }
			offset += nr;
			evidence[0] = base;
			for (size_t i = 1; i < evid_count && offset < len; i++) {
				uint64_t delta = 0;
				nr = htok_leb128_read(bytes + offset, len - offset, &delta);
				if (!nr) break;
				offset += nr;
				base += delta;
				evidence[i] = base;
			}
		}
	}

	uint64_t raw_fc = 0;
	size_t fnr = htok_leb128_read(bytes + offset, len - offset, &raw_fc);
	if (!fnr) { free(evidence); return -10; }
	offset += fnr;
	size_t field_count = (size_t)raw_fc;

	field_entry_t fields[64];
	size_t parsed_fields = 0;

	for (size_t f = 0; f < field_count && offset < len; f++) {
		uint8_t var_tag = bytes[offset++];
		uint8_t field_id = (var_tag >> 3) & 0x1F;
		uint8_t type_tag = var_tag & 0x07;

		char name_buf[64] = {0};
		if (field_id == 31) {
			if (offset >= len) break;
			uint8_t nlen = bytes[offset++];
			if (offset + nlen > len) break;
			size_t copy_len = (nlen < sizeof(name_buf) - 1) ? nlen : sizeof(name_buf) - 1;
			memcpy(name_buf, bytes + offset, copy_len);
			offset += nlen;
		} else {
			const char *kn = htok_field_name_by_id(field_id);
			if (kn) snprintf(name_buf, sizeof(name_buf), "%s", kn);
			else snprintf(name_buf, sizeof(name_buf), "f_%u", field_id);
		}

		uint64_t dlen = 0;
		size_t dnr = htok_leb128_read(bytes + offset, len - offset, &dlen);
		if (!dnr || offset + dnr + dlen > len + 1) break;
		offset += dnr;

		if (parsed_fields < 64) {
			fields[parsed_fields].name = strdup(name_buf);
			fields[parsed_fields].type_tag = type_tag;
			fields[parsed_fields].len = (size_t)dlen;
			fields[parsed_fields].data = malloc((size_t)dlen + 1);
			if (fields[parsed_fields].data) {
				memcpy(fields[parsed_fields].data, bytes + offset, (size_t)dlen);
				fields[parsed_fields].data[dlen] = '\0';
			}
			parsed_fields++;
		}
		offset += (size_t)dlen;
	}

	/* Build Output JSON */
	strbuf_t sb;
	strbuf_init(&sb);
	strbuf_puts(&sb, "{\"level\":");
	strbuf_put_json_str(&sb, level);
	strbuf_puts(&sb, ",\"type\":");
	strbuf_put_json_str(&sb, type);
	strbuf_puts(&sb, ",\"lifecycle\":");
	strbuf_put_json_str(&sb, lifecycle);

	/* Timestamp */
	time_t tsec = (time_t)ts;
	struct tm *tm_val = gmtime(&tsec);
	if (tm_val) {
		char tbuf[32];
		strftime(tbuf, sizeof(tbuf), "%Y-%m-%dT%H:%M:%SZ", tm_val);
		strbuf_puts(&sb, ",\"timestamp\":");
		strbuf_put_json_str(&sb, tbuf);
	}

	if (flags & CORM_HTOK_FLAG_HAS_SUPERSEDES) {
		char nbuf[32];
		snprintf(nbuf, sizeof(nbuf), ",\"supersedes\":%lu", (unsigned long)supersedes);
		strbuf_puts(&sb, nbuf);
	}

	if (flags & CORM_HTOK_FLAG_HAS_SUPERSEDED_BY) {
		char nbuf[32];
		snprintf(nbuf, sizeof(nbuf), ",\"supersededBy\":%lu", (unsigned long)supersededBy);
		strbuf_puts(&sb, nbuf);
	}

	if (evid_count > 0 && evidence) {
		strbuf_puts(&sb, ",\"evidence\":[");
		for (size_t i = 0; i < evid_count; i++) {
			char nbuf[32];
			snprintf(nbuf, sizeof(nbuf), "%s%lu", (i > 0) ? "," : "", (unsigned long)evidence[i]);
			strbuf_puts(&sb, nbuf);
		}
		strbuf_puts(&sb, "]");
	}

	field_entry_t *f_text = get_field(fields, parsed_fields, "text");
	if (f_text && f_text->data) {
		strbuf_puts(&sb, ",\"summary\":");
		strbuf_put_json_str(&sb, (const char *)f_text->data);
	}

	field_entry_t *f_stand = get_field(fields, parsed_fields, "standing");
	if ((flags & CORM_HTOK_FLAG_IS_ANCHOR) || (f_stand && f_stand->data && f_stand->data[0] == '1')) {
		strbuf_puts(&sb, ",\"standing\":true");
	}

	field_entry_t *f_tags = get_field(fields, parsed_fields, "tags");
	if (f_tags && f_tags->data) {
		strbuf_puts(&sb, ",\"tags\":[");
		char *tcopy = strdup((const char *)f_tags->data);
		if (tcopy) {
			char *saveptr = NULL;
			char *tok = strtok_r(tcopy, "\n", &saveptr);
			int first = 1;
			while (tok) {
				if (*tok) {
					if (!first) strbuf_puts(&sb, ",");
					strbuf_put_json_str(&sb, tok);
					first = 0;
				}
				tok = strtok_r(NULL, "\n", &saveptr);
			}
			free(tcopy);
		}
		strbuf_puts(&sb, "]");
	}

	/* Context Object */
	field_entry_t *f_proj = get_field(fields, parsed_fields, "ctx_proj");
	field_entry_t *f_sub = get_field(fields, parsed_fields, "ctx_sub");
	field_entry_t *f_mod = get_field(fields, parsed_fields, "ctx_mod");
	field_entry_t *f_path = get_field(fields, parsed_fields, "ctx_path");
	field_entry_t *f_subj = get_field(fields, parsed_fields, "ctx_subj");
	field_entry_t *f_grid = get_field(fields, parsed_fields, "ctx_grid");

	if (f_proj || f_sub || f_mod || f_path || f_subj || f_grid) {
		strbuf_puts(&sb, ",\"context\":{");
		int cfirst = 1;
		if (f_proj && f_proj->data) {
			strbuf_puts(&sb, "\"project\":");
			strbuf_put_json_str(&sb, (const char *)f_proj->data);
			cfirst = 0;
		}
		if (f_sub && f_sub->data) {
			if (!cfirst) strbuf_puts(&sb, ",");
			strbuf_puts(&sb, "\"subsystem\":");
			strbuf_put_json_str(&sb, (const char *)f_sub->data);
			cfirst = 0;
		}
		if (f_mod && f_mod->data) {
			if (!cfirst) strbuf_puts(&sb, ",");
			strbuf_puts(&sb, "\"module\":");
			strbuf_put_json_str(&sb, (const char *)f_mod->data);
			cfirst = 0;
		}
		if (f_path && f_path->data) {
			if (!cfirst) strbuf_puts(&sb, ",");
			strbuf_puts(&sb, "\"path\":");
			strbuf_put_json_str(&sb, (const char *)f_path->data);
			cfirst = 0;
		}
		if (f_subj && f_subj->data) {
			if (!cfirst) strbuf_puts(&sb, ",");
			strbuf_puts(&sb, "\"subject\":");
			strbuf_put_json_str(&sb, (const char *)f_subj->data);
			cfirst = 0;
		}
		if (f_grid && f_grid->data) {
			if (!cfirst) strbuf_puts(&sb, ",");
			double gx = 0, gy = 0, gz = 0;
			int gn = sscanf((const char *)f_grid->data, "%lf,%lf,%lf", &gx, &gy, &gz);
			if (gn == 3) {
				char gbuf[64];
				snprintf(gbuf, sizeof(gbuf), "\"grid\":{\"x\":%g,\"y\":%g,\"z\":%g}", gx, gy, gz);
				strbuf_puts(&sb, gbuf);
			} else if (gn >= 2) {
				char gbuf[64];
				snprintf(gbuf, sizeof(gbuf), "\"grid\":{\"x\":%g,\"y\":%g}", gx, gy);
				strbuf_puts(&sb, gbuf);
			}
		}
		strbuf_puts(&sb, "}");
	}

	/* Episode Object */
	field_entry_t *f_prob = get_field(fields, parsed_fields, "ep_prob");
	field_entry_t *f_req = get_field(fields, parsed_fields, "ep_req");
	field_entry_t *f_out = get_field(fields, parsed_fields, "ep_out");
	field_entry_t *f_find = get_field(fields, parsed_fields, "ep_find");
	field_entry_t *f_diff = get_field(fields, parsed_fields, "ep_diff");
	field_entry_t *f_app = get_field(fields, parsed_fields, "ep_app");
	field_entry_t *f_sid = get_field(fields, parsed_fields, "ep_sid");
	field_entry_t *f_sfl = get_field(fields, parsed_fields, "ep_sfl");
	field_entry_t *f_lid = get_field(fields, parsed_fields, "ep_lid");
	field_entry_t *f_rid = get_field(fields, parsed_fields, "ep_rid");
	field_entry_t *f_tid = get_field(fields, parsed_fields, "ep_tid");

	if (f_prob || f_req || f_out || f_find || f_diff || f_app || f_sid || f_sfl || f_lid || f_rid || f_tid) {
		strbuf_puts(&sb, ",\"episode\":{");
		int efirst = 1;
		if (f_prob && f_prob->data) {
			strbuf_puts(&sb, "\"problem\":");
			strbuf_put_json_str(&sb, (const char *)f_prob->data);
			efirst = 0;
		}
		if (f_req && f_req->data) {
			if (!efirst) strbuf_puts(&sb, ",");
			strbuf_puts(&sb, "\"request\":");
			strbuf_put_json_str(&sb, (const char *)f_req->data);
			efirst = 0;
		}
		if (f_out && f_out->data) {
			if (!efirst) strbuf_puts(&sb, ",");
			strbuf_puts(&sb, "\"outcome\":");
			strbuf_put_json_str(&sb, (const char *)f_out->data);
			efirst = 0;
		}
		if (f_find && f_find->data) {
			if (!efirst) strbuf_puts(&sb, ",");
			strbuf_puts(&sb, "\"findings\":[");
			char *copy = strdup((const char *)f_find->data);
			if (copy) {
				char *sptr = NULL;
				char *line = strtok_r(copy, "\n", &sptr);
				int lfirst = 1;
				while (line) {
					if (*line) {
						if (!lfirst) strbuf_puts(&sb, ",");
						strbuf_put_json_str(&sb, line);
						lfirst = 0;
					}
					line = strtok_r(NULL, "\n", &sptr);
				}
				free(copy);
			}
			strbuf_puts(&sb, "]");
			efirst = 0;
		}
		if (f_diff && f_diff->data) {
			if (!efirst) strbuf_puts(&sb, ",");
			strbuf_puts(&sb, "\"diffs\":[");
			char *copy = strdup((const char *)f_diff->data);
			if (copy) {
				char *sptr = NULL;
				char *line = strtok_r(copy, "\n", &sptr);
				int lfirst = 1;
				while (line) {
					if (*line) {
						if (!lfirst) strbuf_puts(&sb, ",");
						strbuf_put_json_str(&sb, line);
						lfirst = 0;
					}
					line = strtok_r(NULL, "\n", &sptr);
				}
				free(copy);
			}
			strbuf_puts(&sb, "]");
			efirst = 0;
		}
		if (f_app && f_app->data) {
			if (!efirst) strbuf_puts(&sb, ",");
			strbuf_puts(&sb, "\"approaches\":[");
			char *copy = strdup((const char *)f_app->data);
			if (copy) {
				char *sptr = NULL;
				char *line = strtok_r(copy, "\n", &sptr);
				int afirst = 1;
				while (line) {
					char *tab1 = strchr(line, '\t');
					if (tab1) {
						*tab1 = '\0';
						char *outc = line;
						char *appr = tab1 + 1;
						char *reas = "";
						char *tab2 = strchr(appr, '\t');
						if (tab2) {
							*tab2 = '\0';
							reas = tab2 + 1;
						}
						if (!afirst) strbuf_puts(&sb, ",");
						strbuf_puts(&sb, "{\"outcome\":");
						strbuf_put_json_str(&sb, outc);
						strbuf_puts(&sb, ",\"approach\":");
						strbuf_put_json_str(&sb, appr);
						strbuf_puts(&sb, ",\"reason\":");
						strbuf_put_json_str(&sb, reas);
						strbuf_puts(&sb, "}");
						afirst = 0;
					}
					line = strtok_r(NULL, "\n", &sptr);
				}
				free(copy);
			}
			strbuf_puts(&sb, "]");
			efirst = 0;
		}
		if (f_sid && f_sid->data) {
			if (!efirst) strbuf_puts(&sb, ",");
			strbuf_puts(&sb, "\"sessionId\":");
			if (f_sid->type_tag == CORM_HTOK_TYPE_UUID && f_sid->len == 16) {
				char ustr[37];
				htok_uuid_to_str(f_sid->data, ustr);
				strbuf_put_json_str(&sb, ustr);
			} else {
				strbuf_put_json_str(&sb, (const char *)f_sid->data);
			}
			efirst = 0;
		}
		if (f_sfl && f_sfl->data) {
			if (!efirst) strbuf_puts(&sb, ",");
			strbuf_puts(&sb, "\"sessionFile\":");
			strbuf_put_json_str(&sb, (const char *)f_sfl->data);
			efirst = 0;
		}
		if (f_lid && f_lid->data) {
			if (!efirst) strbuf_puts(&sb, ",");
			strbuf_puts(&sb, "\"leafId\":");
			strbuf_put_json_str(&sb, (const char *)f_lid->data);
			efirst = 0;
		}
		if (f_rid && f_rid->data) {
			if (!efirst) strbuf_puts(&sb, ",");
			strbuf_puts(&sb, "\"runId\":");
			strbuf_put_json_str(&sb, (const char *)f_rid->data);
			efirst = 0;
		}
		if (f_tid && f_tid->data) {
			if (!efirst) strbuf_puts(&sb, ",");
			strbuf_puts(&sb, "\"toolCallId\":");
			strbuf_put_json_str(&sb, (const char *)f_tid->data);
			efirst = 0;
		}
		strbuf_puts(&sb, "}");
	}

	strbuf_puts(&sb, "}");

	/* Cleanup */
	for (size_t i = 0; i < parsed_fields; i++) {
		free(fields[i].name);
		free(fields[i].data);
	}
	free(evidence);

	*out_json = sb.data;
	*out_json_len = sb.len;
	return 0;
}
