#include <node_api.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdarg.h>
#include <time.h>
#include <unistd.h>
#include <errno.h>
#include <setjmp.h>
#include <libgen.h>
#include <sys/stat.h>

#include <ttypt/corm.h>
#include <ttypt/cormr.h>
#include <ttypt/stele.h>

#define QDBE_MASK (256 - 1)
#define CM_FLAGS_MASK 0x1F
#define CORM_AXIS_ROSTER_MAX 64
#define CORM_CLI_PLUGIN_CAP 32
#define CORM_CLI_PLUGIN_OPTS_MAX 32
#define EXPR_ARENA_CAP 128
#define EXPR_VAL_MAX 512
#define EXPR_LABEL_CAP 32
#define LABEL_MAX 31
#define CORM_SCOPED_CAP 64
#define CORM_CLI_PLUGIN_NAME_MAX 63

static uint32_t corm_mask_effective(void) {
    const char *e = getenv("CORM_MASK");
    if (e && *e) {
        char *end;
        unsigned long v = strtoul(e, &end, 10);
        if (*end == '\0' && v > 0 && (v & (v + 1)) == 0)
            return (uint32_t)v;
    }
    return QDBE_MASK;
}

static uint32_t parse_corme_type(const char *which) {
    if (!which || !*which) return CM_STR;
    if (*which == '2' && which[1] != '\0')
        which++;
    if (*which == 's') return CM_STR;
    if (*which == 'u') return CM_U32;
    if (*which == 'b') return corm_blob_type();
    if (*which == 'a') return CM_HNDL;
    return CM_STR;
}

/* ============================================================================
 * 1. Low-Level CormStore Wrapper
 * ============================================================================ */

typedef struct {
    uint32_t hd;
    uint32_t ktype;
    uint32_t vtype;
    uint32_t flags;
    bool is_open;
    char path[1024];
} CormStoreWrapper;

static void CormStoreFinalizer(napi_env env, void *finalize_data, void *finalize_hint) {
    (void)env;
    (void)finalize_hint;
    CormStoreWrapper *wrap = (CormStoreWrapper *)finalize_data;
    if (wrap) {
        if (wrap->is_open && wrap->hd != 0 && wrap->hd != CM_MISS) {
            corm_close(wrap->hd);
            wrap->is_open = false;
        }
        free(wrap);
    }
}

/* CormStore.open(path, databaseName, ktypeStr, vtypeStr, mirrorBool) */
static napi_value CormOpen(napi_env env, napi_callback_info info) {
    size_t argc = 5;
    napi_value args[5];
    napi_status status = napi_get_cb_info(env, info, &argc, args, NULL, NULL);
    if (status != napi_ok || argc < 1) {
        napi_throw_type_error(env, NULL, "Expected at least 1 argument: (path, optionalDbName, optionalKtype, optionalVtype, optionalMirror)");
        return NULL;
    }

    char path_buf[1024] = {0};
    size_t path_len = 0;
    napi_valuetype t0;
    napi_typeof(env, args[0], &t0);
    if (t0 == napi_string) {
        napi_get_value_string_utf8(env, args[0], path_buf, sizeof(path_buf) - 1, &path_len);
    }

    char db_name[64] = "default";
    if (argc >= 2) {
        napi_valuetype t1;
        napi_typeof(env, args[1], &t1);
        if (t1 == napi_string) {
            size_t db_len = 0;
            napi_get_value_string_utf8(env, args[1], db_name, sizeof(db_name) - 1, &db_len);
            if (db_len == 0) strncpy(db_name, "default", sizeof(db_name) - 1);
        }
    }

    char ktype_str[16] = "s";
    if (argc >= 3) {
        napi_valuetype t2;
        napi_typeof(env, args[2], &t2);
        if (t2 == napi_string) {
            napi_get_value_string_utf8(env, args[2], ktype_str, sizeof(ktype_str) - 1, NULL);
        }
    }

    char vtype_str[16] = "s";
    if (argc >= 4) {
        napi_valuetype t3;
        napi_typeof(env, args[3], &t3);
        if (t3 == napi_string) {
            napi_get_value_string_utf8(env, args[3], vtype_str, sizeof(vtype_str) - 1, NULL);
        }
    }

    bool mirror = false;
    if (argc >= 5) {
        napi_valuetype t4;
        napi_typeof(env, args[4], &t4);
        if (t4 == napi_boolean) {
            napi_get_value_bool(env, args[4], &mirror);
        }
    }

    uint32_t flags = CM_SORTED;
    uint32_t ktype = CM_STR;
    if (!strcmp(ktype_str, "a")) {
        flags |= CM_AINDEX;
        ktype = CM_HNDL;
    } else if (ktype_str[0] == '2') {
        flags |= CM_MULTIVALUE | CM_SORTED;
        ktype = parse_corme_type(ktype_str + 1);
    } else {
        ktype = parse_corme_type(ktype_str);
        if (ktype == CM_STR) flags |= CM_AINDEX;
    }
    if (mirror) flags |= CM_MIRROR;

    uint32_t vtype = parse_corme_type(vtype_str);

    CormStoreWrapper *wrap = (CormStoreWrapper *)calloc(1, sizeof(CormStoreWrapper));
    if (!wrap) {
        napi_throw_error(env, NULL, "Out of memory");
        return NULL;
    }
    snprintf(wrap->path, sizeof(wrap->path), "%s", path_buf);

    uint32_t hd = corm_open(wrap->path[0] ? wrap->path : NULL,
                            wrap->path[0] ? db_name : NULL,
                            ktype,
                            vtype,
                            corm_mask_effective(),
                            flags);

    if (hd == CM_MISS || hd == 0) {
        free(wrap);
        napi_throw_error(env, NULL, "Failed to open or create corm database");
        return NULL;
    }

    wrap->hd = hd;
    wrap->ktype = ktype;
    wrap->vtype = vtype;
    wrap->flags = flags;
    wrap->is_open = true;

    napi_value result;
    napi_create_object(env, &result);

    napi_value hd_val;
    napi_create_uint32(env, hd, &hd_val);
    napi_set_named_property(env, result, "handle", hd_val);

    napi_wrap(env, result, wrap, CormStoreFinalizer, NULL, NULL);
    return result;
}

static napi_value CormPut(napi_env env, napi_callback_info info) {
    size_t argc = 3;
    napi_value args[3];
    napi_status status = napi_get_cb_info(env, info, &argc, args, NULL, NULL);
    if (status != napi_ok || argc < 3) {
        napi_throw_type_error(env, NULL, "Expected (storeObj, key, value)");
        return NULL;
    }

    CormStoreWrapper *wrap = NULL;
    napi_unwrap(env, args[0], (void **)&wrap);
    if (!wrap || !wrap->is_open) {
        napi_throw_error(env, NULL, "Corm store is closed or invalid");
        return NULL;
    }

    const void *key_ptr = NULL;
    uint32_t ukey = 0;
    char *skey = NULL;
    napi_valuetype kt;
    napi_typeof(env, args[1], &kt);

    if (kt != napi_null && kt != napi_undefined) {
        if (wrap->ktype == CM_HNDL || wrap->ktype == CM_U32) {
            if (kt == napi_number) {
                napi_get_value_uint32(env, args[1], &ukey);
                key_ptr = &ukey;
            } else if (kt == napi_string) {
                char kbuf[64] = {0};
                napi_get_value_string_utf8(env, args[1], kbuf, sizeof(kbuf) - 1, NULL);
                if (kbuf[0] && strcmp(kbuf, ".") != 0) {
                    ukey = (uint32_t)strtoul(kbuf, NULL, 10);
                    key_ptr = &ukey;
                }
            }
        } else {
            size_t klen = 0;
            napi_get_value_string_utf8(env, args[1], NULL, 0, &klen);
            skey = (char *)malloc(klen + 1);
            napi_get_value_string_utf8(env, args[1], skey, klen + 1, &klen);
            key_ptr = skey;
        }
    }

    uint32_t pos = CM_MISS;
    if (wrap->vtype == corm_blob_type()) {
        bool is_ta = false;
        napi_is_typedarray(env, args[2], &is_ta);
        const uint8_t *raw_bytes = NULL;
        size_t raw_len = 0;
        char *tmp_str = NULL;

        if (is_ta) {
            napi_typedarray_type tat;
            void *data = NULL;
            napi_get_typedarray_info(env, args[2], &tat, &raw_len, &data, NULL, NULL);
            raw_bytes = (const uint8_t *)data;
        } else {
            napi_get_value_string_utf8(env, args[2], NULL, 0, &raw_len);
            tmp_str = (char *)malloc(raw_len + 1);
            napi_get_value_string_utf8(env, args[2], tmp_str, raw_len + 1, &raw_len);
            raw_bytes = (const uint8_t *)tmp_str;
        }

        char *blob = (char *)malloc(sizeof(uint32_t) + raw_len);
        *(uint32_t *)blob = (uint32_t)raw_len;
        if (raw_len > 0) memcpy(blob + sizeof(uint32_t), raw_bytes, raw_len);
        pos = corm_put(wrap->hd, key_ptr, blob);
        free(blob);
        if (tmp_str) free(tmp_str);
    } else if (wrap->vtype == CM_U32) {
        uint32_t uval = 0;
        napi_get_value_uint32(env, args[2], &uval);
        pos = corm_put(wrap->hd, key_ptr, &uval);
    } else {
        size_t val_len = 0;
        napi_get_value_string_utf8(env, args[2], NULL, 0, &val_len);
        char *val_buf = (char *)malloc(val_len + 1);
        napi_get_value_string_utf8(env, args[2], val_buf, val_len + 1, &val_len);
        pos = corm_put(wrap->hd, key_ptr, val_buf);
        free(val_buf);
    }

    if (skey) free(skey);

    napi_value res;
    napi_create_uint32(env, key_ptr && (wrap->ktype == CM_HNDL || wrap->ktype == CM_U32) ? ukey : pos, &res);
    return res;
}

static napi_value CormGet(napi_env env, napi_callback_info info) {
    size_t argc = 2;
    napi_value args[2];
    napi_status status = napi_get_cb_info(env, info, &argc, args, NULL, NULL);
    if (status != napi_ok || argc < 2) {
        napi_throw_type_error(env, NULL, "Expected (storeObj, key)");
        return NULL;
    }

    CormStoreWrapper *wrap = NULL;
    napi_unwrap(env, args[0], (void **)&wrap);
    if (!wrap || !wrap->is_open) {
        napi_throw_error(env, NULL, "Corm store is closed or invalid");
        return NULL;
    }

    const void *key_ptr = NULL;
    uint32_t ukey = 0;
    char *skey = NULL;
    napi_valuetype kt;
    napi_typeof(env, args[1], &kt);

    if (wrap->ktype == CM_HNDL || wrap->ktype == CM_U32) {
        if (kt == napi_number) {
            napi_get_value_uint32(env, args[1], &ukey);
        } else {
            char kbuf[64] = {0};
            napi_get_value_string_utf8(env, args[1], kbuf, sizeof(kbuf) - 1, NULL);
            ukey = (uint32_t)strtoul(kbuf, NULL, 10);
        }
        key_ptr = &ukey;
    } else {
        size_t klen = 0;
        napi_get_value_string_utf8(env, args[1], NULL, 0, &klen);
        skey = (char *)malloc(klen + 1);
        napi_get_value_string_utf8(env, args[1], skey, klen + 1, &klen);
        key_ptr = skey;
    }

    const void *val = corm_get(wrap->hd, key_ptr);
    if (skey) free(skey);

    if (!val) {
        napi_value null_val;
        napi_get_null(env, &null_val);
        return null_val;
    }

    if (wrap->vtype == corm_blob_type()) {
        uint32_t blen = *(const uint32_t *)val;
        const uint8_t *bdata = (const uint8_t *)val + sizeof(uint32_t);
        napi_value ab, ta;
        void *dst = NULL;
        napi_create_arraybuffer(env, blen, &dst, &ab);
        if (blen > 0 && dst) memcpy(dst, bdata, blen);
        napi_create_typedarray(env, napi_uint8_array, blen, ab, 0, &ta);
        return ta;
    } else if (wrap->vtype == CM_U32) {
        napi_value res;
        napi_create_uint32(env, *(const uint32_t *)val, &res);
        return res;
    }

    napi_value res;
    napi_create_string_utf8(env, (const char *)val, NAPI_AUTO_LENGTH, &res);
    return res;
}

static napi_value CormDel(napi_env env, napi_callback_info info) {
    size_t argc = 2;
    napi_value args[2];
    napi_status status = napi_get_cb_info(env, info, &argc, args, NULL, NULL);
    if (status != napi_ok || argc < 2) {
        napi_throw_type_error(env, NULL, "Expected (storeObj, key)");
        return NULL;
    }

    CormStoreWrapper *wrap = NULL;
    napi_unwrap(env, args[0], (void **)&wrap);
    if (!wrap || !wrap->is_open) {
        napi_throw_error(env, NULL, "Corm store is closed or invalid");
        return NULL;
    }

    uint32_t ukey = 0;
    char *skey = NULL;
    const void *key_ptr = NULL;
    napi_valuetype kt;
    napi_typeof(env, args[1], &kt);

    if (wrap->ktype == CM_HNDL || wrap->ktype == CM_U32) {
        if (kt == napi_number) {
            napi_get_value_uint32(env, args[1], &ukey);
        } else {
            char kbuf[64] = {0};
            napi_get_value_string_utf8(env, args[1], kbuf, sizeof(kbuf) - 1, NULL);
            ukey = (uint32_t)strtoul(kbuf, NULL, 10);
        }
        key_ptr = &ukey;
    } else {
        size_t klen = 0;
        napi_get_value_string_utf8(env, args[1], NULL, 0, &klen);
        skey = (char *)malloc(klen + 1);
        napi_get_value_string_utf8(env, args[1], skey, klen + 1, &klen);
        key_ptr = skey;
    }

    corm_del(wrap->hd, key_ptr);
    if (skey) free(skey);

    napi_value res;
    napi_get_boolean(env, true, &res);
    return res;
}

static napi_value CormDelAll(napi_env env, napi_callback_info info) {
    size_t argc = 2;
    napi_value args[2];
    napi_get_cb_info(env, info, &argc, args, NULL, NULL);

    CormStoreWrapper *wrap = NULL;
    napi_unwrap(env, args[0], (void **)&wrap);
    if (!wrap || !wrap->is_open) {
        napi_throw_error(env, NULL, "Corm store is closed or invalid");
        return NULL;
    }

    if (argc < 2) {
        corm_drop(wrap->hd);
        napi_value res;
        napi_get_boolean(env, true, &res);
        return res;
    }

    napi_valuetype kt;
    napi_typeof(env, args[1], &kt);
    if (kt == napi_null || kt == napi_undefined) {
        corm_drop(wrap->hd);
        napi_value res;
        napi_get_boolean(env, true, &res);
        return res;
    }

    uint32_t ukey = 0;
    char *skey = NULL;
    const void *key_ptr = NULL;
    if (wrap->ktype == CM_HNDL || wrap->ktype == CM_U32) {
        if (kt == napi_number) {
            napi_get_value_uint32(env, args[1], &ukey);
            key_ptr = &ukey;
        } else {
            char kbuf[64] = {0};
            napi_get_value_string_utf8(env, args[1], kbuf, sizeof(kbuf) - 1, NULL);
            if (!strcmp(kbuf, "all") || !strcmp(kbuf, "*") || !strcmp(kbuf, ".")) {
                corm_drop(wrap->hd);
                napi_value res;
                napi_get_boolean(env, true, &res);
                return res;
            }
            ukey = (uint32_t)strtoul(kbuf, NULL, 10);
            key_ptr = &ukey;
        }
    } else {
        size_t klen = 0;
        napi_get_value_string_utf8(env, args[1], NULL, 0, &klen);
        skey = (char *)malloc(klen + 1);
        napi_get_value_string_utf8(env, args[1], skey, klen + 1, &klen);
        key_ptr = skey;
    }

    corm_del_all(wrap->hd, key_ptr);
    if (skey) free(skey);

    napi_value res;
    napi_get_boolean(env, true, &res);
    return res;
}

static napi_value CormListKeys(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value args[1];
    napi_get_cb_info(env, info, &argc, args, NULL, NULL);

    CormStoreWrapper *wrap = NULL;
    napi_unwrap(env, args[0], (void **)&wrap);
    if (!wrap || !wrap->is_open) {
        napi_throw_error(env, NULL, "Corm store is closed or invalid");
        return NULL;
    }

    napi_value arr;
    napi_create_array(env, &arr);
    uint32_t idx = 0;
    uint32_t cur = corm_iter(wrap->hd, NULL, 0);
    const void *k = NULL, *v = NULL;
    while (corm_next(&k, &v, cur)) {
        napi_value item;
        if (wrap->ktype == CM_HNDL || wrap->ktype == CM_U32) {
            napi_create_uint32(env, *(const uint32_t *)k, &item);
        } else {
            napi_create_string_utf8(env, (const char *)k, NAPI_AUTO_LENGTH, &item);
        }
        napi_set_element(env, arr, idx++, item);
    }
    corm_fin(cur);
    return arr;
}

static napi_value CormCount(napi_env env, napi_callback_info info) {
    size_t argc = 2;
    napi_value args[2];
    napi_get_cb_info(env, info, &argc, args, NULL, NULL);

    CormStoreWrapper *wrap = NULL;
    napi_unwrap(env, args[0], (void **)&wrap);
    if (!wrap || !wrap->is_open) {
        napi_throw_error(env, NULL, "Corm store is closed or invalid");
        return NULL;
    }

    uint32_t c = corm_count(wrap->hd, NULL);
    napi_value res;
    napi_create_uint32(env, c, &res);
    return res;
}

static napi_value CormSave(napi_env env, napi_callback_info info) {
    (void)env;
    (void)info;
    corm_save();
    napi_value res;
    napi_get_boolean(env, true, &res);
    return res;
}

static napi_value CormClose(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value args[1];
    napi_status status = napi_get_cb_info(env, info, &argc, args, NULL, NULL);
    if (status != napi_ok || argc < 1) {
        napi_throw_type_error(env, NULL, "Expected (storeObj)");
        return NULL;
    }

    corm_save();

    CormStoreWrapper *wrap = NULL;
    napi_unwrap(env, args[0], (void **)&wrap);
    if (wrap && wrap->is_open) {
        corm_close(wrap->hd);
        wrap->is_open = false;
    }

    napi_value res;
    napi_get_boolean(env, true, &res);
    return res;
}

/* ============================================================================
 * 2. HTOK v4 Binary Envelope Codec
 * ============================================================================ */

static napi_value CormHtokPack(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value args[1];
    napi_get_cb_info(env, info, &argc, args, NULL, NULL);
    if (argc < 1) {
        napi_throw_type_error(env, NULL, "Expected (jsonString)");
        return NULL;
    }

    size_t jlen = 0;
    napi_get_value_string_utf8(env, args[0], NULL, 0, &jlen);
    char *jstr = (char *)malloc(jlen + 1);
    napi_get_value_string_utf8(env, args[0], jstr, jlen + 1, &jlen);

    uint8_t *out_bytes = NULL;
    size_t out_len = 0;
    int rc = corm_htok_pack(jstr, jlen, &out_bytes, &out_len);
    free(jstr);

    if (rc != 0 || !out_bytes) {
        napi_throw_error(env, NULL, "corm_htok_pack failed");
        return NULL;
    }

    napi_value ab, ta;
    void *dst = NULL;
    napi_create_arraybuffer(env, out_len, &dst, &ab);
    if (out_len > 0 && dst) memcpy(dst, out_bytes, out_len);
    free(out_bytes);
    napi_create_typedarray(env, napi_uint8_array, out_len, ab, 0, &ta);
    return ta;
}

static napi_value CormHtokUnpack(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value args[1];
    napi_get_cb_info(env, info, &argc, args, NULL, NULL);
    if (argc < 1) {
        napi_throw_type_error(env, NULL, "Expected (uint8Array)");
        return NULL;
    }

    bool is_ta = false;
    napi_is_typedarray(env, args[0], &is_ta);
    if (!is_ta) {
        napi_throw_type_error(env, NULL, "Expected Uint8Array for htokUnpack");
        return NULL;
    }

    napi_typedarray_type tat;
    size_t len = 0;
    void *data = NULL;
    napi_get_typedarray_info(env, args[0], &tat, &len, &data, NULL, NULL);

    char *out_json = NULL;
    size_t out_json_len = 0;
    int rc = corm_htok_unpack((const uint8_t *)data, len, &out_json, &out_json_len);
    if (rc != 0 || !out_json) {
        napi_throw_error(env, NULL, "corm_htok_unpack failed");
        return NULL;
    }

    napi_value res;
    napi_create_string_utf8(env, out_json, out_json_len, &res);
    free(out_json);
    return res;
}

/* ============================================================================
 * 3. CandidateSet (cormr_set_t) & MinHeapRanker (cormr_rank_t)
 * ============================================================================ */

typedef struct {
    cormr_set_t *set;
} CormrSetWrapper;

static void CormrSetFinalizer(napi_env env, void *finalize_data, void *finalize_hint) {
    (void)env;
    (void)finalize_hint;
    CormrSetWrapper *wrap = (CormrSetWrapper *)finalize_data;
    if (wrap) {
        if (wrap->set) {
            cormr_set_free(wrap->set);
            wrap->set = NULL;
        }
        free(wrap);
    }
}

static napi_value CormrSetCreate(napi_env env, napi_callback_info info) {
    (void)info;
    cormr_set_t *s = cormr_set_new();
    if (!s) {
        napi_throw_error(env, NULL, "Failed to allocate cormr set");
        return NULL;
    }

    CormrSetWrapper *wrap = (CormrSetWrapper *)calloc(1, sizeof(CormrSetWrapper));
    wrap->set = s;

    napi_value result;
    napi_create_object(env, &result);
    napi_wrap(env, result, wrap, CormrSetFinalizer, NULL, NULL);
    return result;
}

static napi_value CormrSetPush(napi_env env, napi_callback_info info) {
    size_t argc = 2;
    napi_value args[2];
    napi_get_cb_info(env, info, &argc, args, NULL, NULL);

    CormrSetWrapper *wrap = NULL;
    napi_unwrap(env, args[0], (void **)&wrap);
    if (!wrap || !wrap->set) {
        napi_throw_error(env, NULL, "Invalid cormr set");
        return NULL;
    }

    uint32_t ref = 0;
    napi_get_value_uint32(env, args[1], &ref);
    cormr_set_push(wrap->set, ref);

    napi_value res;
    napi_get_undefined(env, &res);
    return res;
}

static napi_value CormrSetSeal(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value args[1];
    napi_get_cb_info(env, info, &argc, args, NULL, NULL);

    CormrSetWrapper *wrap = NULL;
    napi_unwrap(env, args[0], (void **)&wrap);
    if (!wrap || !wrap->set) {
        napi_throw_error(env, NULL, "Invalid cormr set");
        return NULL;
    }

    cormr_set_seal(wrap->set);

    napi_value res;
    napi_get_undefined(env, &res);
    return res;
}

static napi_value CormrSetToArray(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value args[1];
    napi_get_cb_info(env, info, &argc, args, NULL, NULL);

    CormrSetWrapper *wrap = NULL;
    napi_unwrap(env, args[0], (void **)&wrap);
    if (!wrap || !wrap->set) {
        napi_throw_error(env, NULL, "Invalid cormr set");
        return NULL;
    }

    size_t count = cormr_set_count(wrap->set);
    const cormr_ref_t *refs = cormr_set_at(wrap->set);

    napi_value arr;
    napi_create_array_with_length(env, count, &arr);
    for (size_t i = 0; i < count; i++) {
        napi_value val;
        napi_create_uint32(env, refs[i], &val);
        napi_set_element(env, arr, (uint32_t)i, val);
    }
    return arr;
}

static napi_value CormrSetIntersect(napi_env env, napi_callback_info info) {
    size_t argc = 2;
    napi_value args[2];
    napi_get_cb_info(env, info, &argc, args, NULL, NULL);

    CormrSetWrapper *wA = NULL, *wB = NULL;
    napi_unwrap(env, args[0], (void **)&wA);
    napi_unwrap(env, args[1], (void **)&wB);

    cormr_set_t *dst = cormr_set_new();
    cormr_set_intersect(dst, wA->set, wB->set);

    CormrSetWrapper *wrap = (CormrSetWrapper *)calloc(1, sizeof(CormrSetWrapper));
    wrap->set = dst;

    napi_value res;
    napi_create_object(env, &res);
    napi_wrap(env, res, wrap, CormrSetFinalizer, NULL, NULL);
    return res;
}

static napi_value CormrSetUnion(napi_env env, napi_callback_info info) {
    size_t argc = 2;
    napi_value args[2];
    napi_get_cb_info(env, info, &argc, args, NULL, NULL);

    CormrSetWrapper *wA = NULL, *wB = NULL;
    napi_unwrap(env, args[0], (void **)&wA);
    napi_unwrap(env, args[1], (void **)&wB);

    cormr_set_t *dst = cormr_set_new();
    cormr_set_union(dst, wA->set, wB->set);

    CormrSetWrapper *wrap = (CormrSetWrapper *)calloc(1, sizeof(CormrSetWrapper));
    wrap->set = dst;

    napi_value res;
    napi_create_object(env, &res);
    napi_wrap(env, res, wrap, CormrSetFinalizer, NULL, NULL);
    return res;
}

static napi_value CormrSetSubtract(napi_env env, napi_callback_info info) {
    size_t argc = 2;
    napi_value args[2];
    napi_get_cb_info(env, info, &argc, args, NULL, NULL);

    CormrSetWrapper *wA = NULL, *wB = NULL;
    napi_unwrap(env, args[0], (void **)&wA);
    napi_unwrap(env, args[1], (void **)&wB);

    cormr_set_t *dst = cormr_set_new();
    cormr_set_subtract(dst, wA->set, wB->set);

    CormrSetWrapper *wrap = (CormrSetWrapper *)calloc(1, sizeof(CormrSetWrapper));
    wrap->set = dst;

    napi_value res;
    napi_create_object(env, &res);
    napi_wrap(env, res, wrap, CormrSetFinalizer, NULL, NULL);
    return res;
}

static napi_value CormrSetFree(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value args[1];
    napi_get_cb_info(env, info, &argc, args, NULL, NULL);

    CormrSetWrapper *wrap = NULL;
    napi_unwrap(env, args[0], (void **)&wrap);
    if (wrap && wrap->set) {
        cormr_set_free(wrap->set);
        wrap->set = NULL;
    }

    napi_value res;
    napi_get_undefined(env, &res);
    return res;
}

typedef struct {
    cormr_rank_t *rk;
    uint32_t top_k;
} CormrRankWrapper;

static void CormrRankFinalizer(napi_env env, void *finalize_data, void *finalize_hint) {
    (void)env;
    (void)finalize_hint;
    CormrRankWrapper *wrap = (CormrRankWrapper *)finalize_data;
    if (wrap) {
        if (wrap->rk) {
            cormr_rank_free(wrap->rk);
            wrap->rk = NULL;
        }
        free(wrap);
    }
}

static napi_value CormrRankCreate(napi_env env, napi_callback_info info) {
    size_t argc = 2;
    napi_value args[2];
    napi_get_cb_info(env, info, &argc, args, NULL, NULL);

    uint32_t top_k = 10;
    if (argc >= 1) napi_get_value_uint32(env, args[0], &top_k);

    double min_score = 0.0;
    if (argc >= 2) napi_get_value_double(env, args[1], &min_score);

    cormr_rank_t *rk = cormr_rank_new(top_k, (float)min_score);
    if (!rk) {
        napi_throw_error(env, NULL, "Failed to allocate cormr rank buffer");
        return NULL;
    }

    CormrRankWrapper *wrap = (CormrRankWrapper *)calloc(1, sizeof(CormrRankWrapper));
    wrap->rk = rk;
    wrap->top_k = top_k;

    napi_value res;
    napi_create_object(env, &res);
    napi_wrap(env, res, wrap, CormrRankFinalizer, NULL, NULL);
    return res;
}

static napi_value CormrRankPush(napi_env env, napi_callback_info info) {
    size_t argc = 3;
    napi_value args[3];
    napi_get_cb_info(env, info, &argc, args, NULL, NULL);

    CormrRankWrapper *wrap = NULL;
    napi_unwrap(env, args[0], (void **)&wrap);
    if (!wrap || !wrap->rk) {
        napi_throw_error(env, NULL, "Invalid cormr rank handle");
        return NULL;
    }

    uint32_t ref = 0;
    napi_get_value_uint32(env, args[1], &ref);

    double score = 0.0;
    napi_get_value_double(env, args[2], &score);

    cormr_rank_push(wrap->rk, ref, (float)score);

    napi_value res;
    napi_get_undefined(env, &res);
    return res;
}

static napi_value CormrRankSorted(napi_env env, napi_callback_info info) {
    size_t argc = 2;
    napi_value args[2];
    napi_get_cb_info(env, info, &argc, args, NULL, NULL);

    CormrRankWrapper *wrap = NULL;
    napi_unwrap(env, args[0], (void **)&wrap);
    if (!wrap || !wrap->rk) {
        napi_throw_error(env, NULL, "Invalid cormr rank handle");
        return NULL;
    }

    uint32_t capacity = wrap->top_k > 0 ? wrap->top_k : 100;
    if (argc >= 2) napi_get_value_uint32(env, args[1], &capacity);

    cormr_ref_t *refs = (cormr_ref_t *)malloc(capacity * sizeof(cormr_ref_t));
    float *scores = (float *)malloc(capacity * sizeof(float));

    size_t written = cormr_rank_sorted(wrap->rk, refs, scores);

    napi_value out_arr;
    napi_create_array_with_length(env, written, &out_arr);
    for (size_t i = 0; i < written; i++) {
        napi_value item;
        napi_create_object(env, &item);

        napi_value ref_val, score_val;
        napi_create_uint32(env, refs[i], &ref_val);
        napi_create_double(env, (double)scores[i], &score_val);

        napi_set_named_property(env, item, "ref", ref_val);
        napi_set_named_property(env, item, "score", score_val);
        napi_set_element(env, out_arr, (uint32_t)i, item);
    }

    free(refs);
    free(scores);
    return out_arr;
}

static napi_value CormrRankFree(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value args[1];
    napi_get_cb_info(env, info, &argc, args, NULL, NULL);

    CormrRankWrapper *wrap = NULL;
    napi_unwrap(env, args[0], (void **)&wrap);
    if (wrap && wrap->rk) {
        cormr_rank_free(wrap->rk);
        wrap->rk = NULL;
    }

    napi_value res;
    napi_get_undefined(env, &res);
    return res;
}

/* ============================================================================
 * 4. Multi-Axis In-Process Engine (execProtocol / CormEngine)
 *    Full parity with cormd & corm CLI:
 *    - filespec `<path>@<roster>:<k>:<v>` + `.roster` sidecar
 *    - dynamic loading of libjoint.so / liblemma.so / libsepal.so / libislet.so
 *    - verbs: LIST, GET, GET_JSON, PUT, PUT_JSON, DEL, DEL_ALL, QUERY, RECONCILE
 *    - scoped `--opt@label=val` and `--opt@axis=val` flags in QUERY
 * ============================================================================ */

struct corm_axis_fx {
    int bound;
    int has_store;
    int has_typed;
    int has_unstore;
    int has_readback;
    int (*store_fn)(void *, const char *, rec_ref_t, const char *);
    int (*typed_fn)(void *, const char *, rec_ref_t, const void *, size_t, uint32_t);
    int (*unstore_fn)(void *, rec_ref_t);
    int (*readback_fn)(void *, rec_ref_t, char **, size_t *);
};

static struct corm_loaded {
    void *h;
    int lo, hi;
} corm_loaded_slots[REC_QUERY_MAX_AXES];
static int corm_loaded_n = 0;

struct corm_cli_plug {
    void *h;
    const rec_axis_cli_option_t *opts;
    int (*cfg)(const char *, const char *);
};
static struct corm_cli_plug loaded_plugs[CORM_CLI_PLUGIN_CAP];
static int loaded_plugs_n = 0;

typedef struct {
    bool is_open;
    uint32_t prim_hd;
    uint32_t ktype;
    uint32_t vtype;
    bool disk_exists;
    long disk_mtime_sec;
    long disk_mtime_nsec;
    off_t disk_size;
    ino_t disk_ino;
    char corm_path[1024];
    char sidecar_path[1060];
    char roster_names[REC_QUERY_MAX_AXES][CORM_AXIS_ROSTER_MAX];
    int roster_n;
    void *axis_ctx[REC_QUERY_MAX_AXES];
    char axis_specs[REC_QUERY_MAX_AXES][1060];
    struct corm_axis_fx axis_fx[REC_QUERY_MAX_AXES];
    uint64_t last_used_tick;
} CormEngineInstance;

static int corm_axes_find_slot(const char *name) {
    int n = rec_axis_count();
    for (int slot = 0; slot < n; slot++) {
        const rec_axis_t *axis = rec_axis_get(slot);
        if (axis && !strcmp(axis->name, name))
            return slot;
    }
    return -1;
}

static void *corm_axes_handle_for_slot(int slot) {
    for (int i = 0; i < corm_loaded_n; i++) {
        if (slot >= corm_loaded_slots[i].lo && slot <= corm_loaded_slots[i].hi)
            return corm_loaded_slots[i].h;
    }
    return NULL;
}

static int corm_axes_find_lib(const char *name, char *out, size_t cap) {
    const char *path_env = getenv("CORM_AXIS_PATH");
    const char *home = getenv("HOME");
    char dirs[4096];
    if (path_env && *path_env)
        snprintf(dirs, sizeof(dirs), "%s", path_env);
    else if (home && *home)
        snprintf(dirs, sizeof(dirs), "%s/.local/lib:/usr/local/lib:/usr/lib:./lib:../lib", home);
    else
        snprintf(dirs, sizeof(dirs), "/usr/lib:/usr/local/lib:./lib:../lib");

    char *save = NULL;
    for (char *dir = strtok_r(dirs, ":", &save); dir; dir = strtok_r(NULL, ":", &save)) {
        const char *exts[] = { ".so", ".dylib", ".dll" };
        for (int e = 0; e < 3; e++) {
            snprintf(out, cap, "%s/lib%s%s", dir, name, exts[e]);
            struct stat lst;
            if (stat(out, &lst) == 0)
                return 1;
        }
    }
    return 0;
}

static void corm_axes_sym(void *h, const char *sym_cormr, const char *sym_rec, void *fn, size_t fnsz, int *have) {
    void *sym = stl_dlsym(h, sym_cormr);
    if (!sym && sym_rec)
        sym = stl_dlsym(h, sym_rec);
    *have = 0;
    if (!sym)
        return;
    memcpy(fn, &sym, fnsz);
    *have = 1;
}

static void corm_axes_dlopen_record(const char *path, int quiet) {
    int before = rec_axis_count();
    void *h = stl_dlopen(path, 0);
    if (!h) {
        if (!quiet) {
            const char *err = stl_dlerror();
            fprintf(stderr, "corm_napi: %s: %s\n", path, err ? err : "unknown error");
        }
        return;
    }
    int after = rec_axis_count();
    if (after > before) {
        if (corm_loaded_n < REC_QUERY_MAX_AXES)
            corm_loaded_slots[corm_loaded_n++] = (struct corm_loaded){ h, before, after - 1 };
    } else {
        const char *slash = strrchr(path, '/');
        const char *base = slash ? slash + 1 : path;
        if (!strncmp(base, "lib", 3))
            base += 3;
        char ax_name[CORM_AXIS_ROSTER_MAX];
        snprintf(ax_name, sizeof(ax_name), "%s", base);
        char *dot = strchr(ax_name, '.');
        if (dot) *dot = '\0';
        int s = corm_axes_find_slot(ax_name);
        if (s >= 0 && !corm_axes_handle_for_slot(s) && corm_loaded_n < REC_QUERY_MAX_AXES) {
            corm_loaded_slots[corm_loaded_n++] = (struct corm_loaded){ h, s, s };
        }
    }

    if (loaded_plugs_n < CORM_CLI_PLUGIN_CAP) {
        void *osym = stl_dlsym(h, "cormr_axis_cli_options");
        if (!osym) osym = stl_dlsym(h, "rec_axis_cli_options");
        void *csym = stl_dlsym(h, "cormr_axis_config_arg");
        if (!csym) csym = stl_dlsym(h, "rec_axis_config_arg");
        if (osym && csym) {
            int exists = 0;
            for (int p = 0; p < loaded_plugs_n; p++) {
                if (loaded_plugs[p].h == h) { exists = 1; break; }
            }
            if (!exists) {
                const rec_axis_cli_option_t *(*opts_fn)(void);
                int (*cfg_fn)(const char *, const char *);
                memcpy(&opts_fn, &osym, sizeof(opts_fn));
                memcpy(&cfg_fn, &csym, sizeof(cfg_fn));
                loaded_plugs[loaded_plugs_n].h = h;
                loaded_plugs[loaded_plugs_n].opts = opts_fn();
                loaded_plugs[loaded_plugs_n].cfg = cfg_fn;
                loaded_plugs_n++;
            }
        }
    }
}

static void engine_axes_spec(const CormEngineInstance *eng, const char *name, char *out, size_t cap) {
    char dirbuf[1024];
    char basebuf[1024];
    snprintf(dirbuf, sizeof(dirbuf), "%s", eng->corm_path);
    snprintf(basebuf, sizeof(basebuf), "%s", eng->corm_path);
    char *sl = strrchr(basebuf, '/');
    const char *d = dirname(dirbuf);
    const char *b = sl ? sl + 1 : basebuf;
    int n = snprintf(out, cap, "%s/%s-%s", d, b, name);
    (void)n;
}

static void engine_axis_close(CormEngineInstance *eng, int slot, const char *name) {
    void *ctx = (slot >= 0 && slot < REC_QUERY_MAX_AXES) ? eng->axis_ctx[slot] : NULL;
    if (slot >= 0 && slot < REC_QUERY_MAX_AXES) {
        eng->axis_fx[slot].bound = 0;
        eng->axis_ctx[slot] = NULL;
    }
    if (!ctx) return;

    if (!strcmp(name, "islet")) {
        uint32_t idb = (uint32_t)(uintptr_t)ctx;
        corm_close(idb);
        corm_close(idb + 1);
        rec_axis_set_ctx(slot, NULL);
        return;
    }

    void *h = corm_axes_handle_for_slot(slot);
    if (h) {
        void *sym = stl_dlsym(h, "cormr_axis_close");
        if (!sym) sym = stl_dlsym(h, "rec_axis_close");
        if (sym) {
            void (*close_fn)(void *);
            memcpy(&close_fn, &sym, sizeof(close_fn));
            close_fn(ctx);
            rec_axis_set_ctx(slot, NULL);
            return;
        }
        char cname[128];
        snprintf(cname, sizeof(cname), "%s_close", name);
        sym = stl_dlsym(h, cname);
        if (sym) {
            if (!strcmp(name, "joint")) {
                void (*jclose)(unsigned);
                memcpy(&jclose, &sym, sizeof(jclose));
                jclose((unsigned)(uintptr_t)ctx);
            } else {
                void (*close_fn)(void *);
                memcpy(&close_fn, &sym, sizeof(close_fn));
                close_fn(ctx);
            }
            rec_axis_set_ctx(slot, NULL);
            return;
        }
    }
    rec_axis_set_ctx(slot, NULL);
}

static bool g_joint_zero_burned = false;

static int engine_axis_bind(CormEngineInstance *eng, int slot, const char *name) {
    void *h = corm_axes_handle_for_slot(slot);
    if (!h) {
        char path[1024];
        if (corm_axes_find_lib(name, path, sizeof(path))) {
            corm_axes_dlopen_record(path, 0);
            h = corm_axes_handle_for_slot(slot);
        }
    }
    if (!h) return -1;

    if (!strcmp(name, "joint") && !g_joint_zero_burned) {
        void *jsym = stl_dlsym(h, "joint_init");
        if (jsym) {
            unsigned (*jinit)(const char *);
            memcpy(&jinit, &jsym, sizeof(jinit));
            (void)jinit(NULL);
            g_joint_zero_burned = true;
        }
    }

    engine_axes_spec(eng, name, eng->axis_specs[slot], sizeof(eng->axis_specs[slot]));

    void *sym = stl_dlsym(h, "cormr_axis_open");
    if (!sym) sym = stl_dlsym(h, "rec_axis_open");
    if (!sym) return -1;

    void *(*open_fn)(const char *);
    memcpy(&open_fn, &sym, sizeof(open_fn));
    void *ctx = open_fn(eng->axis_specs[slot]);
    if (!ctx) return -1;

    eng->axis_ctx[slot] = ctx;
    rec_axis_set_ctx(slot, ctx);

    void *csym = stl_dlsym(h, "cormr_axis_env_config");
    if (!csym) csym = stl_dlsym(h, "rec_axis_env_config");
    if (csym) {
        int (*cfg_fn)(void);
        memcpy(&cfg_fn, &csym, sizeof(cfg_fn));
        cfg_fn();
    }

    struct corm_axis_fx *fx = &eng->axis_fx[slot];
    corm_axes_sym(h, "cormr_axis_store", "rec_axis_store", &fx->store_fn, sizeof(fx->store_fn), &fx->has_store);
    corm_axes_sym(h, "cormr_axis_store_typed", "rec_axis_store_typed", &fx->typed_fn, sizeof(fx->typed_fn), &fx->has_typed);
    corm_axes_sym(h, "cormr_axis_unstore", "rec_axis_unstore", &fx->unstore_fn, sizeof(fx->unstore_fn), &fx->has_unstore);
    corm_axes_sym(h, "cormr_axis_readback", "rec_axis_readback", &fx->readback_fn, sizeof(fx->readback_fn), &fx->has_readback);
    fx->bound = 1;
    return 0;
}

static void engine_activate_contexts(CormEngineInstance *eng) {
    stl_setenv("CORM_AXIS_PRIMARY", eng->corm_path, 1);
    stl_setenv("CORM_AXIS_VTYPE", (eng->vtype == corm_blob_type()) ? "b" : "s", 1);
    for (int slot = 0; slot < REC_QUERY_MAX_AXES; slot++) {
        if (eng->axis_fx[slot].bound && eng->axis_ctx[slot]) {
            rec_axis_set_ctx(slot, eng->axis_ctx[slot]);
        }
    }
}

static int engine_roster_parse(CormEngineInstance *eng, const char *csv) {
    eng->roster_n = 0;
    if (!csv || !*csv) return 0;
    char copy[1024];
    snprintf(copy, sizeof(copy), "%s", csv);
    char *save = NULL;
    for (char *tok = strtok_r(copy, ",", &save); tok; tok = strtok_r(NULL, ",", &save)) {
        if (!*tok) continue;
        if (eng->roster_n >= REC_QUERY_MAX_AXES) break;
        snprintf(eng->roster_names[eng->roster_n], CORM_AXIS_ROSTER_MAX, "%s", tok);
        eng->roster_n++;
    }
    return eng->roster_n;
}

static void engine_roster_join(const CormEngineInstance *eng, char *out, size_t cap) {
    size_t n = 0;
    out[0] = '\0';
    for (int i = 0; i < eng->roster_n; i++) {
        n += snprintf(out + n, cap > n ? cap - n : 0, "%s%s", i ? "," : "", eng->roster_names[i]);
    }
}

static void engine_record_disk_stat(CormEngineInstance *eng) {
    struct stat st;
    if (stat(eng->corm_path, &st) == 0) {
        eng->disk_exists = true;
        eng->disk_size = st.st_size;
        eng->disk_ino = st.st_ino;
#if defined(__APPLE__)
        eng->disk_mtime_sec = (long)st.st_mtimespec.tv_sec;
        eng->disk_mtime_nsec = (long)st.st_mtimespec.tv_nsec;
#elif defined(_WIN32)
        eng->disk_mtime_sec = (long)st.st_mtime;
        eng->disk_mtime_nsec = 0;
#else
        eng->disk_mtime_sec = (long)st.st_mtim.tv_sec;
        eng->disk_mtime_nsec = (long)st.st_mtim.tv_nsec;
#endif
    } else {
        eng->disk_exists = false;
        eng->disk_size = 0;
        eng->disk_ino = 0;
        eng->disk_mtime_sec = 0;
        eng->disk_mtime_nsec = 0;
    }
}

static bool engine_disk_matches(const CormEngineInstance *eng) {
    struct stat st;
    if (stat(eng->corm_path, &st) == 0) {
        if (!eng->disk_exists) return st.st_size == 0;
#if defined(__APPLE__)
        long m_sec = (long)st.st_mtimespec.tv_sec;
        long m_nsec = (long)st.st_mtimespec.tv_nsec;
#elif defined(_WIN32)
        long m_sec = (long)st.st_mtime;
        long m_nsec = 0;
#else
        long m_sec = (long)st.st_mtim.tv_sec;
        long m_nsec = (long)st.st_mtim.tv_nsec;
#endif
        return eng->disk_size == st.st_size &&
               eng->disk_ino == st.st_ino &&
               eng->disk_mtime_sec == m_sec &&
               eng->disk_mtime_nsec == m_nsec;
    }
    return !eng->disk_exists || eng->disk_size == 0;
}

static int engine_open_filespec(CormEngineInstance *eng, const char *filespec, char *err_buf, size_t err_cap) {
    memset(eng, 0, sizeof(*eng));
    stl_setenv("CORM_QUIET", "1", 0);
    char buf[1024];
    snprintf(buf, sizeof(buf), "%s", filespec);

    uint32_t ktype = CM_STR;
    uint32_t vtype = CM_STR;
    uint32_t flags = 0;

    char *at = strchr(buf, '@');
    char *frag = buf;
    if (at) {
        *at = '\0';
        frag = at + 1;
    }

    char *first_col = strchr(frag, ':');
    if (first_col) {
        *first_col = '\0';
        first_col++;
        char *second_col = strchr(first_col, ':');
        if (second_col) {
            *second_col = '\0';
            second_col++;
            vtype = parse_corme_type(second_col);
        }
        if (!strcmp(first_col, "a")) {
            flags |= CM_AINDEX;
            ktype = CM_HNDL;
        } else if (*first_col == '2') {
            flags |= CM_MULTIVALUE | CM_SORTED;
            first_col++;
            if (*first_col) ktype = parse_corme_type(first_col);
        } else {
            ktype = parse_corme_type(first_col);
        }
    }

    if (at) {
        engine_roster_parse(eng, frag);
    }

    snprintf(eng->corm_path, sizeof(eng->corm_path), "%s", buf);
    eng->ktype = ktype;
    eng->vtype = vtype;

    /* Ensure parent directory exists */
    char dir_tmp[1024];
    snprintf(dir_tmp, sizeof(dir_tmp), "%s", eng->corm_path);
    char *parent = dirname(dir_tmp);
    if (parent && *parent && strcmp(parent, ".") != 0) {
        stl_mkdir(parent);
    }

    eng->prim_hd = corm_open(eng->corm_path, "hd", ktype, vtype, corm_mask_effective(), (flags & CM_FLAGS_MASK) | CM_MIRROR);
    if (eng->prim_hd == CM_MISS || eng->prim_hd == 0) {
        snprintf(err_buf, err_cap, "failed to open primary database: %s", eng->corm_path);
        return -1;
    }

    stl_setenv("CORM_AXIS_PRIMARY", eng->corm_path, 1);
    stl_setenv("CORM_AXIS_VTYPE", (vtype == corm_blob_type()) ? "b" : "s", 1);

    snprintf(eng->sidecar_path, sizeof(eng->sidecar_path), "%s.roster", eng->corm_path);
    struct stat st;
    int have_sidecar = (stat(eng->sidecar_path, &st) == 0);

    if (have_sidecar && eng->roster_n == 0) {
        uint32_t rs = corm_open(eng->sidecar_path, "@roster", CM_STR, CM_STR, corm_mask_effective(), 0);
        if (rs != CM_MISS && rs != 0) {
            const void *axes = corm_get(rs, "axes");
            if (axes) engine_roster_parse(eng, (const char *)axes);
            corm_close(rs);
        }
    } else if (!have_sidecar && eng->roster_n > 0) {
        uint32_t rs = corm_open(eng->sidecar_path, "@roster", CM_STR, CM_STR, corm_mask_effective(), 0);
        if (rs != CM_MISS && rs != 0) {
            char csv[1024];
            engine_roster_join(eng, csv, sizeof(csv));
            corm_put(rs, "v", "1");
            corm_put(rs, "axes", csv);
            corm_save();
            corm_close(rs);
        }
    }

    for (int i = 0; i < eng->roster_n; i++) {
        const char *name = eng->roster_names[i];
        int slot = corm_axes_find_slot(name);
        if (slot < 0) {
            char lpath[1024];
            if (!corm_axes_find_lib(name, lpath, sizeof(lpath))) {
                snprintf(err_buf, err_cap, "unknown axis '%s' (not found in CORM_AXIS_PATH)", name);
                corm_close(eng->prim_hd);
                corm_close(eng->prim_hd + 1);
                return -1;
            }
            corm_axes_dlopen_record(lpath, 0);
            slot = corm_axes_find_slot(name);
        }
        if (slot < 0 || engine_axis_bind(eng, slot, name) != 0) {
            snprintf(err_buf, err_cap, "failed to bind axis '%s'", name);
            corm_close(eng->prim_hd);
            corm_close(eng->prim_hd + 1);
            return -1;
        }
    }

    engine_record_disk_stat(eng);
    eng->is_open = true;
    return 0;
}

static void engine_close(CormEngineInstance *eng) {
    if (!eng || !eng->is_open) return;
    corm_save();
    for (int i = 0; i < eng->roster_n; i++) {
        const char *name = eng->roster_names[i];
        int slot = corm_axes_find_slot(name);
        if (slot >= 0) {
            engine_axis_close(eng, slot, name);
        }
    }
    if (eng->prim_hd != 0 && eng->prim_hd != CM_MISS) {
        corm_close(eng->prim_hd);
        corm_close(eng->prim_hd + 1);
        eng->prim_hd = 0;
    }
    eng->is_open = false;
}

static int engine_write_ref(CormEngineInstance *eng, const char *operand, uint32_t *ref) {
    if (!operand || !*operand || !ref) return 0;
    char *end = NULL;
    unsigned long v = strtoul(operand, &end, 10);
    if (*end == '\0') {
        *ref = (uint32_t)v;
        return 1;
    }
    const void *r = corm_get(eng->prim_hd + 1, operand);
    if (!r) return 0;
    memcpy(ref, r, sizeof(*ref));
    return 1;
}

static int engine_fanout_store(CormEngineInstance *eng, uint32_t ref, const void *payload, size_t len, uint32_t qtype) {
    engine_activate_contexts(eng);
    int nfail = 0;
    for (int i = 0; i < eng->roster_n; i++) {
        const char *name = eng->roster_names[i];
        int slot = corm_axes_find_slot(name);
        if (slot < 0) continue;
        void *ctx = eng->axis_ctx[slot];
        struct corm_axis_fx *fx = &eng->axis_fx[slot];
        if (!ctx || !fx->bound) continue;

        int rc = 0;
        if (qtype != CM_STR && fx->has_typed)
            rc = fx->typed_fn(ctx, NULL, ref, payload, len, qtype);
        else if (fx->has_store)
            rc = fx->store_fn(ctx, NULL, ref, (const char *)payload);
        else if (fx->has_typed)
            rc = fx->typed_fn(ctx, NULL, ref, payload, len, qtype);
        else
            continue;

        if (rc != 0) nfail++;
    }
    return nfail;
}

static int engine_fanout_unstore(CormEngineInstance *eng, uint32_t ref) {
    engine_activate_contexts(eng);
    int nfail = 0;
    for (int i = 0; i < eng->roster_n; i++) {
        const char *name = eng->roster_names[i];
        int slot = corm_axes_find_slot(name);
        if (slot < 0) continue;
        void *ctx = eng->axis_ctx[slot];
        struct corm_axis_fx *fx = &eng->axis_fx[slot];
        if (!ctx || !fx->bound || !fx->has_unstore) continue;
        if (fx->unstore_fn(ctx, ref) != 0) nfail++;
    }
    return nfail;
}

/* ── Expression Parser & Evaluator for QUERY (with scoped @label/@axis flags) ── */

enum expr_kind { E_LEAF, E_NOTP, E_AND, E_OR, E_SUB, E_REF };

struct expr_node {
    enum expr_kind kind;
    const char *name;
    char label[LABEL_MAX + 1];
    struct expr_node *ref;
    const char *value;
    int heapval;
    struct expr_node *kids[REC_QUERY_MAX_AXES + 1];
    int kids_n;
    rec_set_t *set;
};

struct corm_scoped {
    char label[LABEL_MAX + 1];
    char base[CORM_CLI_PLUGIN_NAME_MAX + 1];
    const char *value;
    int is_axis;
};

static struct expr_node expr_arena[EXPR_ARENA_CAP];
static int expr_used = 0;
static char expr_val_pool[EXPR_ARENA_CAP][EXPR_VAL_MAX];
static int expr_val_used = 0;
static struct expr_node *expr_labels[EXPR_LABEL_CAP];
static int expr_labels_n = 0;
static struct expr_node *expr_root = NULL;
static char expr_names[REC_QUERY_MAX_AXES][CORM_AXIS_ROSTER_MAX];
static int expr_n = 0;
static struct corm_scoped corm_scoped[CORM_SCOPED_CAP];
static int corm_scoped_n = 0;

static const char *lx = NULL;
static size_t top_k = 0;
static float min_score = 0.0f;
static const rec_axis_t *ranker_axis = NULL;
static void *ranker_params = NULL;
static char rank_override[CORM_AXIS_ROSTER_MAX];

static jmp_buf g_expr_jmp;
static char g_expr_err[256];

enum tok { T_EOF, T_LP, T_RP, T_KW, T_NAME };
static struct {
    enum tok t;
    const char *word;
    size_t len;
} corm_cur_tok;

static void napi_expr_error(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void napi_expr_error(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g_expr_err, sizeof(g_expr_err), fmt, ap);
    va_end(ap);
    longjmp(g_expr_jmp, 1);
}

static int lx_is_keyword(const char *w, size_t len) {
    return (len == 3 && !memcmp(w, "AND", 3))
        || (len == 2 && !memcmp(w, "OR", 2))
        || (len == 3 && !memcmp(w, "NOT", 3))
        || (len == 6 && !memcmp(w, "EXCEPT", 6));
}

static void lx_next(void) {
    while (*lx == ' ' || *lx == '\t') lx++;
    if (*lx == '\0') {
        corm_cur_tok.t = T_EOF;
        corm_cur_tok.word = lx;
        corm_cur_tok.len = 0;
        return;
    }
    if (*lx == '(') {
        corm_cur_tok.t = T_LP;
        corm_cur_tok.word = lx;
        corm_cur_tok.len = 1;
        lx++;
        return;
    }
    if (*lx == ')') {
        corm_cur_tok.t = T_RP;
        corm_cur_tok.word = lx;
        corm_cur_tok.len = 1;
        lx++;
        return;
    }
    const char *start = lx;
    while (*lx && *lx != ' ' && *lx != '\t' && *lx != '=' && *lx != '(' && *lx != ')')
        lx++;
    size_t len = (size_t)(lx - start);
    corm_cur_tok.word = start;
    corm_cur_tok.len = len;
    corm_cur_tok.t = lx_is_keyword(start, len) ? T_KW : T_NAME;

    if (corm_cur_tok.t != T_NAME) return;
    const char *ws = lx;
    while (*ws == ' ' || *ws == '\t') ws++;
    if (*ws == '=')
        napi_expr_error("'%.*s=VALUE' no longer allowed in -X; pass via flags", (int)len, start);
}

static struct expr_node *corm_expr_setexpr(void);
static struct expr_node *corm_expr_orexpr(void);
static struct expr_node *corm_expr_andexpr(void);
static struct expr_node *corm_expr_notexpr(void);
static struct expr_node *corm_expr_primary(void);
static struct expr_node *corm_expr_leaf(void);

static struct expr_node *corm_expr_alloc(enum expr_kind kind) {
    if (expr_used >= EXPR_ARENA_CAP)
        napi_expr_error("too many expression nodes");
    struct expr_node *n = &expr_arena[expr_used++];
    memset(n, 0, sizeof(*n));
    n->kind = kind;
    return n;
}

static struct expr_node *corm_expr_label_find(const char *label) {
    for (int i = 0; i < expr_labels_n; i++) {
        if (!strcmp(expr_labels[i]->label, label))
            return expr_labels[i];
    }
    return NULL;
}

static struct expr_node *corm_expr_setexpr(void) {
    struct expr_node *left = corm_expr_orexpr();
    while (corm_cur_tok.t == T_KW && corm_cur_tok.len == 6 && !memcmp(corm_cur_tok.word, "EXCEPT", 6)) {
        lx_next();
        struct expr_node *right = corm_expr_orexpr();
        struct expr_node *n = corm_expr_alloc(E_SUB);
        n->kids[0] = left;
        n->kids[1] = right;
        n->kids_n = 2;
        left = n;
    }
    return left;
}

static struct expr_node *corm_expr_orexpr(void) {
    struct expr_node *left = corm_expr_andexpr();
    while (corm_cur_tok.t == T_KW && corm_cur_tok.len == 2 && !memcmp(corm_cur_tok.word, "OR", 2)) {
        lx_next();
        struct expr_node *right = corm_expr_andexpr();
        struct expr_node *n = corm_expr_alloc(E_OR);
        n->kids[0] = left;
        n->kids[1] = right;
        n->kids_n = 2;
        left = n;
    }
    return left;
}

static struct expr_node *corm_expr_andexpr(void) {
    struct expr_node *left = corm_expr_notexpr();
    while (corm_cur_tok.t == T_KW && corm_cur_tok.len == 3 && !memcmp(corm_cur_tok.word, "AND", 3)) {
        lx_next();
        struct expr_node *right = corm_expr_notexpr();
        struct expr_node *n = corm_expr_alloc(E_AND);
        n->kids[0] = left;
        n->kids[1] = right;
        n->kids_n = 2;
        left = n;
    }
    return left;
}

static struct expr_node *corm_expr_notexpr(void) {
    if (corm_cur_tok.t == T_KW && corm_cur_tok.len == 3 && !memcmp(corm_cur_tok.word, "NOT", 3)) {
        lx_next();
        struct expr_node *child = corm_expr_notexpr();
        struct expr_node *n = corm_expr_alloc(E_NOTP);
        n->kids[0] = child;
        n->kids_n = 1;
        return n;
    }
    return corm_expr_primary();
}

static struct expr_node *corm_expr_primary(void) {
    if (corm_cur_tok.t == T_LP) {
        lx_next();
        if (corm_cur_tok.t == T_RP)
            napi_expr_error("expected expression inside parens");
        struct expr_node *n = corm_expr_setexpr();
        if (corm_cur_tok.t != T_RP)
            napi_expr_error("expected ')'");
        lx_next();
        return n;
    }
    return corm_expr_leaf();
}

static struct expr_node *corm_expr_leaf(void) {
    if (corm_cur_tok.t != T_NAME)
        napi_expr_error("unexpected token '%.*s'", (int)corm_cur_tok.len, corm_cur_tok.word);

    const char *word = corm_cur_tok.word;
    size_t len = corm_cur_tok.len;
    const char *colon = memchr(word, ':', len);

    if (!colon) {
        for (int i = 0; i < expr_labels_n; i++) {
            const char *lbl = expr_labels[i]->label;
            if (strlen(lbl) == len && !memcmp(lbl, word, len)) {
                struct expr_node *r = corm_expr_alloc(E_REF);
                r->ref = expr_labels[i];
                r->name = lbl;
                lx_next();
                return r;
            }
        }
    }

    const char *axis = word;
    size_t axis_len = len;
    size_t llen = 0;
    if (colon) {
        llen = (size_t)(colon - word);
        axis = colon + 1;
        axis_len = len - llen - 1;
    }
    if (axis_len == 0)
        napi_expr_error("empty axis name");

    int idx = -1;
    for (int i = 0; i < expr_n; i++) {
        if (strlen(expr_names[i]) == axis_len && !memcmp(expr_names[i], axis, axis_len)) {
            idx = i;
            break;
        }
    }
    if (idx < 0) {
        if (expr_n >= REC_QUERY_MAX_AXES)
            napi_expr_error("too many distinct axes (max %d)", REC_QUERY_MAX_AXES);
        if (axis_len >= CORM_AXIS_ROSTER_MAX)
            napi_expr_error("axis name too long");
        memcpy(expr_names[expr_n], axis, axis_len);
        expr_names[expr_n][axis_len] = '\0';
        idx = expr_n++;
    }

    struct expr_node *n = corm_expr_alloc(E_LEAF);
    n->name = expr_names[idx];

    if (colon && llen > 0) {
        if (llen > LABEL_MAX)
            napi_expr_error("label too long");
        memcpy(n->label, word, llen);
        n->label[llen] = '\0';
        if (expr_labels_n < EXPR_LABEL_CAP)
            expr_labels[expr_labels_n++] = n;
    }

    lx_next();
    return n;
}

static size_t corm_leaf_quote_len(const char *v) {
    size_t n = 2;
    for (const char *p = v; *p; p++)
        n += (*p == '\\' || *p == '\'') ? 2 : 1;
    return n;
}

static int corm_leaf_needs_quote(const char *v) {
    for (const char *p = v; *p; p++)
        if (*p == ' ' || *p == '\t' || *p == '\'' || *p == '"' || *p == '(' || *p == ')')
            return 1;
    return 0;
}

static void corm_apply_scoped_flags(void) {
    if (corm_scoped_n == 0) return;
    for (int i = 0; i < expr_used; i++) {
        struct expr_node *n = &expr_arena[i];
        if (n->kind != E_LEAF) continue;
        for (int s = 0; s < corm_scoped_n; s++) {
            const struct corm_scoped *sc = &corm_scoped[s];
            int match = sc->is_axis
                ? (n->label[0] == 0 && !strcmp(n->name, sc->label))
                : (!strcmp(n->label, sc->label));
            if (!match) continue;
            const char *old = n->value ? n->value : "";
            size_t oldlen = strlen(old);
            size_t vlen = strlen(sc->value);
            size_t baselen = strlen(sc->base);
            int needs_q = corm_leaf_needs_quote(sc->value);
            size_t qlen = needs_q ? corm_leaf_quote_len(sc->value) : vlen;
            size_t total = oldlen + (oldlen > 0 ? 1 : 0) + baselen + 1 + qlen + 1;
            char *dst;
            if (total < EXPR_VAL_MAX && expr_val_used < EXPR_ARENA_CAP) {
                dst = expr_val_pool[expr_val_used++];
                n->heapval = 0;
            } else {
                dst = (char *)malloc(total);
                if (!dst) napi_expr_error("out of memory");
                n->heapval = 1;
            }
            char *o = dst;
            memcpy(o, old, oldlen);
            o += oldlen;
            if (oldlen > 0) *o++ = ' ';
            memcpy(o, sc->base, baselen);
            o += baselen;
            *o++ = '=';
            if (needs_q) {
                *o++ = '\'';
                for (const char *p = sc->value; *p; p++) {
                    if (*p == '\\' || *p == '\'') *o++ = '\\';
                    *o++ = *p;
                }
                *o++ = '\'';
            } else {
                memcpy(o, sc->value, vlen);
                o += vlen;
            }
            *o = '\0';
            n->value = dst;
        }
    }
}

static rec_set_t *corm_expr_eval(struct expr_node *n, rec_set_t *universe) {
    if (!n) return NULL;
    if (n->set) return n->set;

    switch (n->kind) {
    case E_REF:
        if (!n->ref || !n->ref->set)
            napi_expr_error("label '%s': instance not evaluated", n->name);
        return n->ref->set;
    case E_LEAF: {
        int slot = corm_axes_find_slot(n->name);
        if (slot < 0) napi_expr_error("axis '%s': not loaded", n->name);
        const rec_axis_t *axis = rec_axis_get(slot);
        if (!axis || !axis->fill) napi_expr_error("axis '%s': no fill fn", n->name);
        if (!axis->ctx) napi_expr_error("axis '%s': no ctx", n->name);

        void *params = axis->decode
            ? axis->decode(n->value)
            : (void *)(n->value ? n->value : "");
        rec_set_t *s = rec_set_new();
        if (axis->fill(axis->ctx, params, s) < 0)
            napi_expr_error("axis '%s': fill failed", n->name);
        rec_set_seal(s);
        if (axis->rank && !ranker_axis) {
            ranker_axis = axis;
            ranker_params = params;
        }
        n->set = s;
        return s;
    }
    case E_NOTP: {
        rec_set_t *c = corm_expr_eval(n->kids[0], universe);
        rec_set_t *d = rec_set_new();
        rec_set_subtract(d, universe, c);
        n->set = d;
        return d;
    }
    case E_AND:
    case E_OR:
    case E_SUB: {
        rec_set_t *acc = corm_expr_eval(n->kids[0], universe);
        for (int i = 1; i < n->kids_n; i++) {
            if (n->kind != E_OR && rec_set_count(acc) == 0) {
                acc = rec_set_new();
                break;
            }
            rec_set_t *t = corm_expr_eval(n->kids[i], universe);
            rec_set_t *d = rec_set_new();
            if (n->kind == E_AND)
                rec_set_intersect(d, acc, t);
            else if (n->kind == E_OR)
                rec_set_union(d, acc, t);
            else
                rec_set_subtract(d, acc, t);
            if (i > 1)
                rec_set_free(acc);
            acc = d;
        }
        n->set = acc;
        return acc;
    }
    }
    return NULL;
}

static void corm_expr_free_tree(void) {
    for (int i = 0; i < EXPR_ARENA_CAP; i++) {
        if (expr_arena[i].set) {
            rec_set_free(expr_arena[i].set);
            expr_arena[i].set = NULL;
        }
        if (expr_arena[i].heapval) {
            free((void *)expr_arena[i].value);
            expr_arena[i].value = NULL;
            expr_arena[i].heapval = 0;
        }
    }
}

static int corm_expr_parse(const char *expr) {
    lx = expr;
    expr_used = 0;
    expr_val_used = 0;
    expr_n = 0;
    expr_labels_n = 0;
    corm_scoped_n = 0;
    rank_override[0] = '\0';
    expr_root = NULL;

    lx_next();
    if (corm_cur_tok.t == T_EOF) return 0;

    expr_root = corm_expr_setexpr();
    if (corm_cur_tok.t != T_EOF) {
        if (corm_cur_tok.t == T_KW && corm_cur_tok.len == 3 && !memcmp(corm_cur_tok.word, "NOT", 3))
            napi_expr_error("unexpected token 'NOT' (use EXCEPT)");
        napi_expr_error("unexpected token '%.*s'", (int)corm_cur_tok.len, corm_cur_tok.word);
    }
    return 0;
}

static int buf_append(char **buf, size_t *len, size_t *cap, const char *data, size_t dlen) {
    if (*len + dlen + 1 > *cap) {
        size_t ncap = *cap ? *cap * 2 : 4096;
        while (ncap < *len + dlen + 1) ncap *= 2;
        char *nbuf = (char *)realloc(*buf, ncap);
        if (!nbuf) return -1;
        *buf = nbuf;
        *cap = ncap;
    }
    memcpy(*buf + *len, data, dlen);
    *len += dlen;
    (*buf)[*len] = '\0';
    return 0;
}

static int buf_printf(char **buf, size_t *len, size_t *cap, const char *fmt, ...) __attribute__((format(printf, 4, 5)));
static int buf_printf(char **buf, size_t *len, size_t *cap, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    char tmp[1024];
    int n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    if (n < 0) return -1;
    if ((size_t)n < sizeof(tmp)) return buf_append(buf, len, cap, tmp, (size_t)n);

    char *big = (char *)malloc((size_t)n + 1);
    if (!big) return -1;
    va_start(ap, fmt);
    vsnprintf(big, (size_t)n + 1, fmt, ap);
    va_end(ap);
    int r = buf_append(buf, len, cap, big, (size_t)n);
    free(big);
    return r;
}

/* Engine cache by canonical filespec so repeated in-process calls reuse open handles */
#define MAX_CACHED_ENGINES 64
static CormEngineInstance g_engines[MAX_CACHED_ENGINES];
static uint64_t g_engine_tick = 0;

static CormEngineInstance *get_or_open_engine(const char *filespec, char *err_buf, size_t err_cap) {
    char raw_path[1024];
    snprintf(raw_path, sizeof(raw_path), "%s", filespec);
    char *at = strchr(raw_path, '@');
    if (at) *at = '\0';
    char *col = strchr(raw_path, ':');
    if (col) *col = '\0';

    for (int i = 0; i < MAX_CACHED_ENGINES; i++) {
        if (g_engines[i].is_open && strcmp(g_engines[i].corm_path, raw_path) == 0) {
            if (!engine_disk_matches(&g_engines[i])) {
                engine_close(&g_engines[i]);
                break;
            }
            /* If filespec explicitly names @roster axes that aren't bound yet, bind them */
            const char *f_at = strchr(filespec, '@');
            if (f_at) {
                char r_csv[1024];
                snprintf(r_csv, sizeof(r_csv), "%s", f_at + 1);
                char *r_col = strchr(r_csv, ':');
                if (r_col) *r_col = '\0';
                char *save = NULL;
                engine_activate_contexts(&g_engines[i]);
                for (char *tok = strtok_r(r_csv, ",", &save); tok; tok = strtok_r(NULL, ",", &save)) {
                    if (!*tok) continue;
                    bool in_roster = false;
                    for (int r = 0; r < g_engines[i].roster_n; r++) {
                        if (strcmp(g_engines[i].roster_names[r], tok) == 0) {
                            in_roster = true;
                            break;
                        }
                    }
                    int slot = corm_axes_find_slot(tok);
                    if (slot < 0) {
                        char lpath[1024];
                        if (corm_axes_find_lib(tok, lpath, sizeof(lpath))) {
                            corm_axes_dlopen_record(lpath, 0);
                            slot = corm_axes_find_slot(tok);
                        }
                    }
                    if (slot >= 0 && !g_engines[i].axis_fx[slot].bound) {
                        if (engine_axis_bind(&g_engines[i], slot, tok) == 0 && !in_roster && g_engines[i].roster_n < REC_QUERY_MAX_AXES) {
                            snprintf(g_engines[i].roster_names[g_engines[i].roster_n], CORM_AXIS_ROSTER_MAX, "%s", tok);
                            g_engines[i].roster_n++;
                        }
                    }
                }
            }
            g_engines[i].last_used_tick = ++g_engine_tick;
            return &g_engines[i];
        }
    }

    int free_slot = -1;
    for (int i = 0; i < MAX_CACHED_ENGINES; i++) {
        if (!g_engines[i].is_open) {
            free_slot = i;
            break;
        }
    }
    if (free_slot < 0) {
        int lru_slot = 0;
        for (int i = 1; i < MAX_CACHED_ENGINES; i++) {
            if (g_engines[i].last_used_tick < g_engines[lru_slot].last_used_tick) {
                lru_slot = i;
            }
        }
        free_slot = lru_slot;
        engine_close(&g_engines[free_slot]);
    }

    if (engine_open_filespec(&g_engines[free_slot], filespec, err_buf, err_cap) != 0) {
        return NULL;
    }
    g_engines[free_slot].last_used_tick = ++g_engine_tick;
    return &g_engines[free_slot];
}

static napi_value CormCloseEngine(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value args[1];
    napi_get_cb_info(env, info, &argc, args, NULL, NULL);

    if (argc < 1) {
        for (int i = 0; i < MAX_CACHED_ENGINES; i++) {
            if (g_engines[i].is_open) engine_close(&g_engines[i]);
        }
    } else {
        char fspec[1024] = {0};
        napi_get_value_string_utf8(env, args[0], fspec, sizeof(fspec) - 1, NULL);
        char *at = strchr(fspec, '@');
        if (at) *at = '\0';
        char *col = strchr(fspec, ':');
        if (col) *col = '\0';
        for (int i = 0; i < MAX_CACHED_ENGINES; i++) {
            if (g_engines[i].is_open && (!fspec[0] || strcmp(g_engines[i].corm_path, fspec) == 0)) {
                engine_close(&g_engines[i]);
            }
        }
    }

    napi_value res;
    napi_get_boolean(env, true, &res);
    return res;
}

static napi_value make_exec_result(napi_env env, int code, const char *stdout_str, size_t stdout_len, const char *stderr_str, bool include_bytes) {
    napi_value obj, v_code, v_out, v_err;
    napi_create_object(env, &obj);
    napi_create_int32(env, code, &v_code);
    napi_create_string_utf8(env, stdout_str ? stdout_str : "", stdout_str ? stdout_len : 0, &v_out);
    napi_create_string_utf8(env, stderr_str ? stderr_str : "", NAPI_AUTO_LENGTH, &v_err);
    napi_set_named_property(env, obj, "code", v_code);
    napi_set_named_property(env, obj, "stdout", v_out);
    napi_set_named_property(env, obj, "stderr", v_err);

    if (include_bytes && stdout_str && stdout_len > 0) {
        napi_value ab, ta;
        void *dst = NULL;
        napi_create_arraybuffer(env, stdout_len, &dst, &ab);
        if (dst) memcpy(dst, stdout_str, stdout_len);
        napi_create_typedarray(env, napi_uint8_array, stdout_len, ab, 0, &ta);
        napi_set_named_property(env, obj, "stdoutBytes", ta);
    }
    return obj;
}

static napi_value CormExecCommandInner(
    napi_env env,
    const char *op,
    const char *filespec,
    const char *arg2,
    const char *body_ptr,
    size_t body_len,
    char *alloc_body
);

/* execCommand(op, filespec, arg2, payloadStrOrUint8Array, envObj) -> { code, stdout, stderr, stdoutBytes? } */
static napi_value CormExecCommand(napi_env env, napi_callback_info info) {
    size_t argc = 5;
    napi_value args[5];
    napi_get_cb_info(env, info, &argc, args, NULL, NULL);
    if (argc < 2) {
        napi_throw_type_error(env, NULL, "Expected at least (op, filespec)");
        return NULL;
    }

    char op[32] = {0};
    napi_get_value_string_utf8(env, args[0], op, sizeof(op) - 1, NULL);

    char filespec[1024] = {0};
    napi_get_value_string_utf8(env, args[1], filespec, sizeof(filespec) - 1, NULL);

    char arg2[1024] = {0};
    if (argc >= 3) {
        napi_valuetype t2;
        napi_typeof(env, args[2], &t2);
        if (t2 == napi_string) {
            napi_get_value_string_utf8(env, args[2], arg2, sizeof(arg2) - 1, NULL);
        } else if (t2 == napi_number) {
            uint32_t u = 0;
            napi_get_value_uint32(env, args[2], &u);
            snprintf(arg2, sizeof(arg2), "%u", u);
        }
    }

    const char *body_ptr = "";
    size_t body_len = 0;
    char *alloc_body = NULL;
    if (argc >= 4) {
        bool is_ta = false;
        napi_is_typedarray(env, args[3], &is_ta);
        if (is_ta) {
            napi_typedarray_type tat;
            void *data = NULL;
            napi_get_typedarray_info(env, args[3], &tat, &body_len, &data, NULL, NULL);
            body_ptr = (const char *)data;
        } else {
            napi_valuetype t3;
            napi_typeof(env, args[3], &t3);
            if (t3 == napi_string) {
                napi_get_value_string_utf8(env, args[3], NULL, 0, &body_len);
                alloc_body = (char *)malloc(body_len + 1);
                napi_get_value_string_utf8(env, args[3], alloc_body, body_len + 1, &body_len);
                body_ptr = alloc_body;
            }
        }
    }

    struct saved_env_entry {
        char key[128];
        char *old_val;
        bool had_val;
    } saved_env[64];
    uint32_t saved_env_n = 0;

    /* Apply optional environment variables before opening/running */
    if (argc >= 5) {
        napi_valuetype t4;
        napi_typeof(env, args[4], &t4);
        if (t4 == napi_object) {
            napi_value prop_names;
            if (napi_get_property_names(env, args[4], &prop_names) == napi_ok) {
                uint32_t plen = 0;
                napi_get_array_length(env, prop_names, &plen);
                for (uint32_t i = 0; i < plen; i++) {
                    napi_value k_val, v_val;
                    napi_get_element(env, prop_names, i, &k_val);
                    napi_get_property(env, args[4], k_val, &v_val);
                    char k_str[128] = {0};
                    char v_str[4096] = {0};
                    napi_get_value_string_utf8(env, k_val, k_str, sizeof(k_str) - 1, NULL);
                    napi_valuetype vt;
                    napi_typeof(env, v_val, &vt);
                    if (vt == napi_string && k_str[0]) {
                        napi_get_value_string_utf8(env, v_val, v_str, sizeof(v_str) - 1, NULL);
                        if (saved_env_n < 64) {
                            snprintf(saved_env[saved_env_n].key, sizeof(saved_env[saved_env_n].key), "%s", k_str);
                            const char *prev = getenv(k_str);
                            if (prev) {
                                saved_env[saved_env_n].had_val = true;
                                saved_env[saved_env_n].old_val = strdup(prev);
                            } else {
                                saved_env[saved_env_n].had_val = false;
                                saved_env[saved_env_n].old_val = NULL;
                            }
                            saved_env_n++;
                        }
                        stl_setenv(k_str, v_str, 1);
                    }
                }
            }
        }
    }

    napi_value result = CormExecCommandInner(env, op, filespec, arg2, body_ptr, body_len, alloc_body);

    for (uint32_t i = saved_env_n; i > 0; i--) {
        uint32_t idx = i - 1;
        if (saved_env[idx].had_val && saved_env[idx].old_val) {
            stl_setenv(saved_env[idx].key, saved_env[idx].old_val, 1);
            free(saved_env[idx].old_val);
        } else {
#ifdef _WIN32
            _putenv_s(saved_env[idx].key, "");
#else
            unsetenv(saved_env[idx].key);
#endif
        }
    }

    return result;
}

static napi_value CormExecCommandInner(
    napi_env env,
    const char *op,
    const char *filespec,
    const char *arg2,
    const char *body_ptr,
    size_t body_len,
    char *alloc_body
) {
    if (!strcmp(op, "PING")) {
        if (alloc_body) free(alloc_body);
        return make_exec_result(env, 0, "PONG\n", 5, "", false);
    }

    char err_buf[1024] = {0};
    CormEngineInstance *eng = get_or_open_engine(filespec, err_buf, sizeof(err_buf));
    if (!eng) {
        if (alloc_body) free(alloc_body);
        return make_exec_result(env, 1, "", 0, err_buf[0] ? err_buf : "failed to open engine", false);
    }
    engine_activate_contexts(eng);

    if (!strcmp(op, "RECONCILE")) {
        engine_close(eng);
        eng = get_or_open_engine(filespec, err_buf, sizeof(err_buf));
        if (alloc_body) free(alloc_body);
        if (!eng) {
            return make_exec_result(env, 1, "", 0, err_buf[0] ? err_buf : "reconciliation failed\n", false);
        }
        return make_exec_result(env, 0, "", 0, "", false);
    }

    if (!strcmp(op, "LIST")) {
        char *res_buf = NULL;
        size_t res_len = 0, res_cap = 0;
        uint32_t cur = corm_iter(eng->prim_hd, NULL, 0);
        const void *k = NULL, *v = NULL;
        while (corm_next(&k, &v, cur)) {
            uint32_t ref = *(const uint32_t *)k;
            buf_printf(&res_buf, &res_len, &res_cap, "%u\n", ref);
        }
        corm_fin(cur);
        napi_value ret = make_exec_result(env, 0, res_buf ? res_buf : "", res_len, "", false);
        free(res_buf);
        if (alloc_body) free(alloc_body);
        return ret;
    }

    if (!strcmp(op, "GET")) {
        uint32_t ref = 0;
        if (!engine_write_ref(eng, arg2, &ref)) {
            if (alloc_body) free(alloc_body);
            return make_exec_result(env, 0, "-1\n", 3, "", false);
        }
        const void *val = corm_get(eng->prim_hd, &ref);
        if (!val) {
            if (alloc_body) free(alloc_body);
            return make_exec_result(env, 0, "-1\n", 3, "", false);
        }
        if (eng->vtype == corm_blob_type()) {
            uint32_t blen = *(const uint32_t *)val;
            const char *bdata = (const char *)val + sizeof(uint32_t);
            napi_value ret = make_exec_result(env, 0, bdata, blen, "", true);
            if (alloc_body) free(alloc_body);
            return ret;
        }
        const char *sval = (const char *)val;
        size_t vlen = strlen(sval);
        char *out = (char *)malloc(vlen + 2);
        memcpy(out, sval, vlen);
        out[vlen] = '\n';
        out[vlen + 1] = '\0';
        napi_value ret = make_exec_result(env, 0, out, vlen + 1, "", false);
        free(out);
        if (alloc_body) free(alloc_body);
        return ret;
    }

    if (!strcmp(op, "GET_JSON")) {
        uint32_t ref = 0;
        if (!engine_write_ref(eng, arg2, &ref)) {
            if (alloc_body) free(alloc_body);
            return make_exec_result(env, 0, "-1\n", 3, "", false);
        }
        const void *val = corm_get(eng->prim_hd, &ref);
        if (!val) {
            if (alloc_body) free(alloc_body);
            return make_exec_result(env, 0, "-1\n", 3, "", false);
        }
        if (eng->vtype == corm_blob_type()) {
            uint32_t blen = *(const uint32_t *)val;
            const uint8_t *bdata = (const uint8_t *)val + sizeof(uint32_t);
            char *json_out = NULL;
            size_t json_len = 0;
            int rc = corm_htok_unpack(bdata, blen, &json_out, &json_len);
            if (rc != 0 || !json_out) {
                if (alloc_body) free(alloc_body);
                return make_exec_result(env, 1, "", 0, "htok unpack failed\n", false);
            }
            char *out = (char *)malloc(json_len + 2);
            memcpy(out, json_out, json_len);
            out[json_len] = '\n';
            out[json_len + 1] = '\0';
            napi_value ret = make_exec_result(env, 0, out, json_len + 1, "", false);
            free(json_out);
            free(out);
            if (alloc_body) free(alloc_body);
            return ret;
        } else {
            const char *sval = (const char *)val;
            size_t vlen = strlen(sval);
            char *out = (char *)malloc(vlen + 2);
            memcpy(out, sval, vlen);
            out[vlen] = '\n';
            out[vlen + 1] = '\0';
            napi_value ret = make_exec_result(env, 0, out, vlen + 1, "", false);
            free(out);
            if (alloc_body) free(alloc_body);
            return ret;
        }
    }

    if (!strcmp(op, "PUT")) {
        uint32_t ref = 0;
        int have_ref = 0;
        if (arg2[0] && strcmp(arg2, ".") != 0) {
            if (!engine_write_ref(eng, arg2, &ref)) {
                if (alloc_body) free(alloc_body);
                return make_exec_result(env, 1, "", 0, "invalid ref operand\n", false);
            }
            have_ref = 1;
        }

        int nfail = 0;
        if (eng->vtype == corm_blob_type()) {
            size_t total_sz = sizeof(uint32_t) + body_len;
            char *buf = (char *)malloc(total_sz);
            *(uint32_t *)buf = (uint32_t)body_len;
            if (body_len > 0) memcpy(buf + sizeof(uint32_t), body_ptr, body_len);
            if (have_ref) {
                corm_put(eng->prim_hd, &ref, buf);
            } else {
                ref = corm_put(eng->prim_hd, NULL, buf);
            }
            nfail = engine_fanout_store(eng, ref, body_ptr, body_len, eng->vtype);
            corm_save();
            engine_record_disk_stat(eng);
            free(buf);
        } else {
            char *payload = (char *)malloc(body_len + 1);
            if (body_len > 0) memcpy(payload, body_ptr, body_len);
            payload[body_len] = '\0';
            if (have_ref) {
                corm_put(eng->prim_hd, &ref, payload);
            } else {
                ref = corm_put(eng->prim_hd, NULL, payload);
            }
            nfail = engine_fanout_store(eng, ref, payload, body_len, CM_STR);
            corm_save();
            engine_record_disk_stat(eng);
            free(payload);
        }

        if (alloc_body) free(alloc_body);
        if (nfail > 0) {
            return make_exec_result(env, 1, "", 0, "partial failure: axis index update failed\n", false);
        }
        char rstr[32];
        int rlen = snprintf(rstr, sizeof(rstr), "%u\n", ref);
        return make_exec_result(env, 0, rstr, (size_t)rlen, "", false);
    }

    if (!strcmp(op, "PUT_JSON")) {
        uint32_t ref = 0;
        int have_ref = 0;
        if (arg2[0] && strcmp(arg2, ".") != 0) {
            if (!engine_write_ref(eng, arg2, &ref)) {
                if (alloc_body) free(alloc_body);
                return make_exec_result(env, 1, "", 0, "invalid ref operand\n", false);
            }
            have_ref = 1;
        }

        int nfail = 0;
        if (eng->vtype == corm_blob_type()) {
            uint8_t *htok_bytes = NULL;
            size_t htok_len = 0;
            int rc = corm_htok_pack(body_ptr, body_len, &htok_bytes, &htok_len);
            if (rc != 0 || !htok_bytes) {
                if (alloc_body) free(alloc_body);
                return make_exec_result(env, 1, "", 0, "htok pack failed\n", false);
            }
            size_t total_sz = sizeof(uint32_t) + htok_len;
            char *buf = (char *)malloc(total_sz);
            *(uint32_t *)buf = (uint32_t)htok_len;
            memcpy(buf + sizeof(uint32_t), htok_bytes, htok_len);

            if (have_ref) {
                corm_put(eng->prim_hd, &ref, buf);
            } else {
                ref = corm_put(eng->prim_hd, NULL, buf);
            }
            nfail = engine_fanout_store(eng, ref, htok_bytes, htok_len, eng->vtype);
            corm_save();
            engine_record_disk_stat(eng);
            free(buf);
            free(htok_bytes);
        } else {
            char *payload = NULL;
            size_t plen = 0;
            if (body_len < 11 || body_ptr[4] != '-' || body_ptr[7] != '-' || body_ptr[10] != ':') {
                time_t now = time(NULL);
                struct tm *tm = gmtime(&now);
                char pfx[32];
                int pfx_len = snprintf(pfx, sizeof(pfx), "%04d-%02d-%02d:",
                    tm ? tm->tm_year + 1900 : 2026, tm ? tm->tm_mon + 1 : 1, tm ? tm->tm_mday : 1);
                plen = (size_t)pfx_len + body_len;
                payload = (char *)malloc(plen + 1);
                memcpy(payload, pfx, (size_t)pfx_len);
                memcpy(payload + pfx_len, body_ptr, body_len);
                payload[plen] = '\0';
            } else {
                plen = body_len;
                payload = (char *)malloc(plen + 1);
                memcpy(payload, body_ptr, body_len);
                payload[plen] = '\0';
            }
            if (have_ref) {
                corm_put(eng->prim_hd, &ref, payload);
            } else {
                ref = corm_put(eng->prim_hd, NULL, payload);
            }
            nfail = engine_fanout_store(eng, ref, payload, plen, CM_STR);
            corm_save();
            engine_record_disk_stat(eng);
            free(payload);
        }

        if (alloc_body) free(alloc_body);
        if (nfail > 0) {
            return make_exec_result(env, 1, "", 0, "partial failure: axis index update failed\n", false);
        }
        char rstr[32];
        int rlen = snprintf(rstr, sizeof(rstr), "%u\n", ref);
        return make_exec_result(env, 0, rstr, (size_t)rlen, "", false);
    }

    if (!strcmp(op, "DEL")) {
        uint32_t ref = 0;
        if (!engine_write_ref(eng, arg2, &ref)) {
            if (alloc_body) free(alloc_body);
            return make_exec_result(env, 1, "", 0, "invalid ref operand\n", false);
        }
        corm_del(eng->prim_hd, &ref);
        int nfail = engine_fanout_unstore(eng, ref);
        corm_save();
        engine_record_disk_stat(eng);
        if (alloc_body) free(alloc_body);
        if (nfail > 0) {
            return make_exec_result(env, 1, "", 0, "partial failure: axis index unstore failed\n", false);
        }
        return make_exec_result(env, 0, "", 0, "", false);
    }

    if (!strcmp(op, "DEL_ALL")) {
        uint32_t ref = 0;
        int have_ref = 0;
        if (arg2[0] && strcmp(arg2, "all") != 0 && strcmp(arg2, "*") != 0 && strcmp(arg2, ".") != 0) {
            if (engine_write_ref(eng, arg2, &ref)) have_ref = 1;
        }
        if (have_ref) {
            corm_del_all(eng->prim_hd, &ref);
            int nfail = engine_fanout_unstore(eng, ref);
            corm_save();
            engine_record_disk_stat(eng);
            if (alloc_body) free(alloc_body);
            if (nfail > 0) {
                return make_exec_result(env, 1, "", 0, "partial failure: axis index unstore failed\n", false);
            }
            return make_exec_result(env, 0, "", 0, "", false);
        }

        int total_nfail = 0;
        uint32_t cur = corm_iter(eng->prim_hd, NULL, 0);
        const void *k = NULL, *v = NULL;
        while (corm_next(&k, &v, cur)) {
            uint32_t r = *(const uint32_t *)k;
            total_nfail += engine_fanout_unstore(eng, r);
        }
        corm_fin(cur);
        corm_drop(eng->prim_hd);
        corm_save();
        engine_record_disk_stat(eng);
        if (alloc_body) free(alloc_body);
        if (total_nfail > 0) {
            return make_exec_result(env, 1, "", 0, "partial failure: axis index unstore failed\n", false);
        }
        return make_exec_result(env, 0, "", 0, "", false);
    }

    if (!strcmp(op, "QUERY")) {
        top_k = (size_t)strtoul(arg2, NULL, 10);
        min_score = 0.0f;

        char * volatile copy = (char *)malloc(body_len + 1);
        rec_set_t * volatile universe = NULL;
        rec_rank_t * volatile board = NULL;
        rec_ref_t * volatile refs = NULL;
        float * volatile scores = NULL;
        char * volatile res_buf = NULL;
        size_t res_len = 0, res_cap = 0;

        memcpy(copy, body_ptr, body_len);
        copy[body_len] = '\0';

        if (setjmp(g_expr_jmp) != 0) {
            if (copy) { free(copy); copy = NULL; }
            if (board) { rec_rank_free(board); board = NULL; }
            if (refs) { free(refs); refs = NULL; }
            if (scores) { free(scores); scores = NULL; }
            if (res_buf) { free(res_buf); res_buf = NULL; }
            corm_expr_free_tree();
            if (universe) { rec_set_free(universe); universe = NULL; }
            if (alloc_body) free(alloc_body);
            return make_exec_result(env, 1, "", 0, g_expr_err, false);
        }

        char *nl = strchr(copy, '\n');
        char *expr_line = copy;
        char *opts_start = NULL;
        if (nl) {
            *nl = '\0';
            opts_start = nl + 1;
        }

        corm_expr_parse(expr_line);

        /* Ensure any axes named in -X expression are loaded and bound */
        for (int i = 0; i < expr_n; i++) {
            const char *aname = expr_names[i];
            int slot = corm_axes_find_slot(aname);
            if (slot < 0) {
                char lpath[1024];
                if (corm_axes_find_lib(aname, lpath, sizeof(lpath))) {
                    corm_axes_dlopen_record(lpath, 0);
                    slot = corm_axes_find_slot(aname);
                }
            }
            if (slot >= 0 && !eng->axis_fx[slot].bound) {
                engine_axis_bind(eng, slot, aname);
            }
        }
        engine_activate_contexts(eng);

        /* Process option lines (both global --opt=val and scoped --opt@label=val) */
        if (opts_start && *opts_start) {
            char *saveptr = NULL;
            for (char *line = strtok_r(opts_start, "\r\n", &saveptr); line; line = strtok_r(NULL, "\r\n", &saveptr)) {
                if (!strncmp(line, "--", 2)) {
                    char *eq = strchr(line + 2, '=');
                    const char *opt_name = line + 2;
                    const char *opt_val = NULL;
                    if (eq) {
                        *eq = '\0';
                        opt_val = eq + 1;
                    }
                    const char *at_sc = strchr(opt_name, '@');
                    if (at_sc) {
                        size_t blen = (size_t)(at_sc - opt_name);
                        const char *scope = at_sc + 1;
                        if (blen == 4 && !strncmp(opt_name, "rank", 4)) {
                            snprintf(rank_override, sizeof(rank_override), "%s", scope);
                        } else if (opt_val && corm_scoped_n < CORM_SCOPED_CAP && blen <= CORM_CLI_PLUGIN_NAME_MAX && strlen(scope) <= LABEL_MAX) {
                            struct corm_scoped *sc = &corm_scoped[corm_scoped_n++];
                            memcpy(sc->base, opt_name, blen);
                            sc->base[blen] = '\0';
                            snprintf(sc->label, sizeof(sc->label), "%s", scope);
                            sc->value = opt_val;
                            sc->is_axis = corm_expr_label_find(scope) ? 0 : 1;
                        }
                    } else if (opt_val) {
                        if (!strcmp(opt_name, "top") || !strcmp(opt_name, "t")) {
                            top_k = (size_t)strtoul(opt_val, NULL, 10);
                        } else if (!strcmp(opt_name, "bottom") || !strcmp(opt_name, "b")) {
                            min_score = (float)strtod(opt_val, NULL);
                        } else if (!strcmp(opt_name, "rank")) {
                            snprintf(rank_override, sizeof(rank_override), "%s", opt_val);
                        } else {
                            for (int p = 0; p < loaded_plugs_n; p++) {
                                const rec_axis_cli_option_t *o = loaded_plugs[p].opts;
                                for (int k = 0; o && o[k].name && k < CORM_CLI_PLUGIN_OPTS_MAX; k++) {
                                    if (!strcmp(o[k].name, opt_name))
                                        loaded_plugs[p].cfg(opt_name, opt_val);
                                }
                            }
                        }
                    }
                }
            }
        }

        corm_apply_scoped_flags();

        if (!expr_root) {
            uint32_t cur = corm_iter(eng->prim_hd, NULL, 0);
            const void *k = NULL, *v = NULL;
            while (corm_next(&k, &v, cur)) {
                uint32_t ref = *(const uint32_t *)k;
                buf_printf((char **)&res_buf, &res_len, &res_cap, "%u %s\n", ref, (const char *)v);
            }
            corm_fin(cur);
            napi_value ret = make_exec_result(env, 0, res_buf ? res_buf : "", res_len, "", false);
            if (res_buf) free(res_buf);
            if (copy) { free(copy); copy = NULL; }
            if (alloc_body) free(alloc_body);
            return ret;
        }

        ranker_axis = NULL;
        ranker_params = NULL;

        universe = rec_set_new();
        rec_set_fill_corm_iter(universe, eng->prim_hd);
        rec_set_seal(universe);

        rec_set_t *R = corm_expr_eval(expr_root, universe);
        if (!R) {
            rec_set_free(universe);
            corm_expr_free_tree();
            if (copy) { free(copy); copy = NULL; }
            if (alloc_body) free(alloc_body);
            return make_exec_result(env, 1, "", 0, "query evaluation produced no set\n", false);
        }

        size_t set_count = rec_set_count(R);
        size_t nout = 0;
        bool is_blob = (eng->vtype == corm_blob_type());

        if (ranker_axis || rank_override[0]) {
            struct { const rec_axis_t *axis; void *params; } ri[EXPR_ARENA_CAP];
            int rin = 0;
            if (rank_override[0]) {
                struct expr_node *def = corm_expr_label_find(rank_override);
                if (def) {
                    int slot = corm_axes_find_slot(def->name);
                    const rec_axis_t *ax = slot >= 0 ? rec_axis_get(slot) : NULL;
                    if (ax && ax->rank) {
                        ri[rin].axis = ax;
                        ri[rin].params = ax->decode ? ax->decode(def->value) : (void *)(def->value ? def->value : "");
                        rin++;
                    }
                } else {
                    int slot = corm_axes_find_slot(rank_override);
                    const rec_axis_t *ax = slot >= 0 ? rec_axis_get(slot) : NULL;
                    if (ax && ax->rank) {
                        ri[rin].axis = ax;
                        ri[rin].params = ax->decode ? ax->decode("") : (void *)"";
                        rin++;
                    }
                }
            } else {
                for (int i = 0; i < expr_used; i++) {
                    struct expr_node *nn = &expr_arena[i];
                    if (nn->kind != E_LEAF) continue;
                    int slot = corm_axes_find_slot(nn->name);
                    const rec_axis_t *ax = slot >= 0 ? rec_axis_get(slot) : NULL;
                    if (!ax || ax != ranker_axis || !ax->rank) continue;
                    ri[rin].axis = ax;
                    ri[rin].params = ax->decode ? ax->decode(nn->value) : (void *)(nn->value ? nn->value : "");
                    rin++;
                }
            }

            size_t k = top_k ? top_k : set_count;
            if (k > 0) {
                board = rec_rank_new(k, min_score);
                refs = (rec_ref_t *)malloc(k * sizeof(*refs));
                scores = (float *)malloc(k * sizeof(*scores));
            }
            const rec_ref_t *rrefs = rec_set_at(R);
            for (size_t i = 0; i < set_count; i++) {
                float bs = 0.0f;
                int got = 0;
                for (int r = 0; r < rin; r++) {
                    float s = 0.0f;
                    if (ri[r].axis->rank((void *)ri[r].axis->ctx, ri[r].params, rrefs[i], &s) == 0
                            && (!got || s > bs)) {
                        bs = s;
                        got = 1;
                    }
                }
                if (got && board) rec_rank_push(board, rrefs[i], bs);
            }
            if (board) nout = rec_rank_sorted(board, refs, scores);

            for (size_t i = 0; i < nout; i++) {
                const void *rec = corm_get(eng->prim_hd, &refs[i]);
                if (!rec) continue;
                if (is_blob) {
                    uint32_t blen = *(const uint32_t *)rec;
                    const char *bdata = (const char *)rec + sizeof(uint32_t);
                    buf_printf((char **)&res_buf, &res_len, &res_cap, "%u %f ", refs[i], scores[i]);
                    buf_append((char **)&res_buf, &res_len, &res_cap, bdata, blen);
                    buf_append((char **)&res_buf, &res_len, &res_cap, "\n", 1);
                } else {
                    buf_printf((char **)&res_buf, &res_len, &res_cap, "%u %f %s\n", refs[i], scores[i], (const char *)rec);
                }
            }
            if (board) rec_rank_free(board);
            if (refs) free(refs);
            if (scores) free(scores);
        } else {
            const rec_ref_t *rrefs = rec_set_at(R);
            size_t k = top_k ? (top_k < set_count ? top_k : set_count) : set_count;
            for (size_t i = 0; i < k; i++) {
                const void *rec = corm_get(eng->prim_hd, &rrefs[i]);
                if (!rec) continue;
                if (is_blob) {
                    uint32_t blen = *(const uint32_t *)rec;
                    const char *bdata = (const char *)rec + sizeof(uint32_t);
                    buf_printf((char **)&res_buf, &res_len, &res_cap, "%u ", rrefs[i]);
                    buf_append((char **)&res_buf, &res_len, &res_cap, bdata, blen);
                    buf_append((char **)&res_buf, &res_len, &res_cap, "\n", 1);
                } else {
                    buf_printf((char **)&res_buf, &res_len, &res_cap, "%u %s\n", rrefs[i], (const char *)rec);
                }
            }
        }

        corm_expr_free_tree();
        rec_set_free(universe);
        if (copy) { free(copy); copy = NULL; }

        napi_value ret = make_exec_result(env, 0, res_buf ? res_buf : "", res_len, "", is_blob);
        if (res_buf) free(res_buf);
        if (alloc_body) free(alloc_body);
        return ret;
    }

    if (alloc_body) free(alloc_body);
    return make_exec_result(env, 1, "", 0, "unknown command\n", false);
}

static napi_value Init(napi_env env, napi_value exports) {
    napi_property_descriptor desc[] = {
        {"open", NULL, CormOpen, NULL, NULL, NULL, napi_default, NULL},
        {"put", NULL, CormPut, NULL, NULL, NULL, napi_default, NULL},
        {"get", NULL, CormGet, NULL, NULL, NULL, napi_default, NULL},
        {"del", NULL, CormDel, NULL, NULL, NULL, napi_default, NULL},
        {"delAll", NULL, CormDelAll, NULL, NULL, NULL, napi_default, NULL},
        {"listKeys", NULL, CormListKeys, NULL, NULL, NULL, napi_default, NULL},
        {"count", NULL, CormCount, NULL, NULL, NULL, napi_default, NULL},
        {"save", NULL, CormSave, NULL, NULL, NULL, napi_default, NULL},
        {"close", NULL, CormClose, NULL, NULL, NULL, napi_default, NULL},
        {"htokPack", NULL, CormHtokPack, NULL, NULL, NULL, napi_default, NULL},
        {"htokUnpack", NULL, CormHtokUnpack, NULL, NULL, NULL, napi_default, NULL},
        {"execCommand", NULL, CormExecCommand, NULL, NULL, NULL, napi_default, NULL},
        {"closeEngine", NULL, CormCloseEngine, NULL, NULL, NULL, napi_default, NULL},
        {"setCreate", NULL, CormrSetCreate, NULL, NULL, NULL, napi_default, NULL},
        {"setPush", NULL, CormrSetPush, NULL, NULL, NULL, napi_default, NULL},
        {"setSeal", NULL, CormrSetSeal, NULL, NULL, NULL, napi_default, NULL},
        {"setToArray", NULL, CormrSetToArray, NULL, NULL, NULL, napi_default, NULL},
        {"setIntersect", NULL, CormrSetIntersect, NULL, NULL, NULL, napi_default, NULL},
        {"setUnion", NULL, CormrSetUnion, NULL, NULL, NULL, napi_default, NULL},
        {"setSubtract", NULL, CormrSetSubtract, NULL, NULL, NULL, napi_default, NULL},
        {"setFree", NULL, CormrSetFree, NULL, NULL, NULL, napi_default, NULL},
        {"rankCreate", NULL, CormrRankCreate, NULL, NULL, NULL, napi_default, NULL},
        {"rankPush", NULL, CormrRankPush, NULL, NULL, NULL, napi_default, NULL},
        {"rankSorted", NULL, CormrRankSorted, NULL, NULL, NULL, napi_default, NULL},
        {"rankFree", NULL, CormrRankFree, NULL, NULL, NULL, napi_default, NULL}
    };

    napi_define_properties(env, exports, sizeof(desc) / sizeof(desc[0]), desc);
    return exports;
}

NAPI_MODULE(NODE_GYP_MODULE_NAME, Init)
