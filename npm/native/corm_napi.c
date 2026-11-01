#include <node_api.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#include <ttypt/corm.h>
#include <ttypt/cormr.h>

/* CormStore wrapper structure */
typedef struct {
    uint32_t hd;
    bool is_open;
    char path[256];
} CormStoreWrapper;

/* Finalizer for CormStore */
static void CormStoreFinalizer(napi_env env, void* finalize_data, void* finalize_hint) {
    (void)env;
    (void)finalize_hint;
    CormStoreWrapper* wrap = (CormStoreWrapper*)finalize_data;
    if (wrap) {
        if (wrap->is_open && wrap->hd != 0 && wrap->hd != CM_MISS) {
            corm_close(wrap->hd);
            wrap->is_open = false;
        }
        free(wrap);
    }
}

/* CormStore.open(path, databaseName) */
static napi_value CormOpen(napi_env env, napi_callback_info info) {
    size_t argc = 2;
    napi_value args[2];
    napi_status status = napi_get_cb_info(env, info, &argc, args, NULL, NULL);
    if (status != napi_ok || argc < 1) {
        napi_throw_type_error(env, NULL, "Expected at least 1 argument: (path, optionalDbName)");
        return NULL;
    }

    char path_buf[256] = {0};
    size_t path_len = 0;
    napi_get_value_string_utf8(env, args[0], path_buf, sizeof(path_buf) - 1, &path_len);

    char db_name[64] = "default";
    if (argc >= 2) {
        size_t db_len = 0;
        napi_get_value_string_utf8(env, args[1], db_name, sizeof(db_name) - 1, &db_len);
        if (db_len == 0) {
            strncpy(db_name, "default", sizeof(db_name) - 1);
        }
    }

    /* Open CM_STR -> CM_STR map with auto-index and file backing */
    uint32_t hd = corm_open(path_buf[0] ? path_buf : NULL,
                            db_name,
                            CM_STR,
                            CM_STR,
                            0xFF,
                            CM_AINDEX | CM_SORTED);

    if (hd == CM_MISS || hd == 0) {
        napi_throw_error(env, NULL, "Failed to open or create corm database");
        return NULL;
    }

    CormStoreWrapper* wrap = (CormStoreWrapper*)calloc(1, sizeof(CormStoreWrapper));
    if (!wrap) {
        corm_close(hd);
        napi_throw_error(env, NULL, "Out of memory");
        return NULL;
    }
    wrap->hd = hd;
    wrap->is_open = true;
    strncpy(wrap->path, path_buf, sizeof(wrap->path) - 1);

    napi_value result;
    napi_create_object(env, &result);

    napi_value hd_val;
    napi_create_uint32(env, hd, &hd_val);
    napi_set_named_property(env, result, "handle", hd_val);

    napi_wrap(env, result, wrap, CormStoreFinalizer, NULL, NULL);
    return result;
}

/* CormStore.put(storeObj, key, value) */
static napi_value CormPut(napi_env env, napi_callback_info info) {
    size_t argc = 3;
    napi_value args[3];
    napi_status status = napi_get_cb_info(env, info, &argc, args, NULL, NULL);
    if (status != napi_ok || argc < 3) {
        napi_throw_type_error(env, NULL, "Expected (storeObj, key, value)");
        return NULL;
    }

    CormStoreWrapper* wrap = NULL;
    napi_unwrap(env, args[0], (void**)&wrap);
    if (!wrap || !wrap->is_open) {
        napi_throw_error(env, NULL, "Corm store is closed or invalid");
        return NULL;
    }

    char key_buf[256] = {0};
    size_t key_len = 0;
    napi_get_value_string_utf8(env, args[1], key_buf, sizeof(key_buf) - 1, &key_len);

    /* Dynamically probe and allocate value buffer to prevent silent truncation */
    size_t val_len = 0;
    napi_get_value_string_utf8(env, args[2], NULL, 0, &val_len);

    char *val_buf = (char *)malloc(val_len + 1);
    if (!val_buf) {
        napi_throw_error(env, NULL, "Out of memory allocating value buffer");
        return NULL;
    }
    napi_get_value_string_utf8(env, args[2], val_buf, val_len + 1, &val_len);

    uint32_t pos = corm_put(wrap->hd, key_buf, val_buf);
    free(val_buf);

    napi_value res;
    napi_create_uint32(env, pos, &res);
    return res;
}

/* CormStore.get(storeObj, key) */
static napi_value CormGet(napi_env env, napi_callback_info info) {
    size_t argc = 2;
    napi_value args[2];
    napi_status status = napi_get_cb_info(env, info, &argc, args, NULL, NULL);
    if (status != napi_ok || argc < 2) {
        napi_throw_type_error(env, NULL, "Expected (storeObj, key)");
        return NULL;
    }

    CormStoreWrapper* wrap = NULL;
    napi_unwrap(env, args[0], (void**)&wrap);
    if (!wrap || !wrap->is_open) {
        napi_throw_error(env, NULL, "Corm store is closed or invalid");
        return NULL;
    }

    char key_buf[256] = {0};
    size_t key_len = 0;
    napi_get_value_string_utf8(env, args[1], key_buf, sizeof(key_buf) - 1, &key_len);

    const void* val = corm_get(wrap->hd, key_buf);
    if (!val) {
        napi_value null_val;
        napi_get_null(env, &null_val);
        return null_val;
    }

    napi_value res;
    napi_create_string_utf8(env, (const char*)val, NAPI_AUTO_LENGTH, &res);
    return res;
}

/* CormStore.del(storeObj, key) */
static napi_value CormDel(napi_env env, napi_callback_info info) {
    size_t argc = 2;
    napi_value args[2];
    napi_status status = napi_get_cb_info(env, info, &argc, args, NULL, NULL);
    if (status != napi_ok || argc < 2) {
        napi_throw_type_error(env, NULL, "Expected (storeObj, key)");
        return NULL;
    }

    CormStoreWrapper* wrap = NULL;
    napi_unwrap(env, args[0], (void**)&wrap);
    if (!wrap || !wrap->is_open) {
        napi_throw_error(env, NULL, "Corm store is closed or invalid");
        return NULL;
    }

    char key_buf[256] = {0};
    size_t key_len = 0;
    napi_get_value_string_utf8(env, args[1], key_buf, sizeof(key_buf) - 1, &key_len);

    corm_del(wrap->hd, key_buf);

    napi_value res;
    napi_get_boolean(env, true, &res);
    return res;
}

/* CormStore.save() */
static napi_value CormSave(napi_env env, napi_callback_info info) {
    (void)env;
    (void)info;
    corm_save();
    napi_value res;
    napi_get_boolean(env, true, &res);
    return res;
}

/* CormStore.close(storeObj) */
static napi_value CormClose(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value args[1];
    napi_status status = napi_get_cb_info(env, info, &argc, args, NULL, NULL);
    if (status != napi_ok || argc < 1) {
        napi_throw_type_error(env, NULL, "Expected (storeObj)");
        return NULL;
    }

    corm_save();

    CormStoreWrapper* wrap = NULL;
    napi_unwrap(env, args[0], (void**)&wrap);
    if (wrap && wrap->is_open) {
        corm_close(wrap->hd);
        wrap->is_open = false;
    }

    napi_value res;
    napi_get_boolean(env, true, &res);
    return res;
}

/* --- Corm Recall Kernel (cormr) Candidate Set Algebra --- */

typedef struct {
    cormr_set_t* set;
} CormrSetWrapper;

static void CormrSetFinalizer(napi_env env, void* finalize_data, void* finalize_hint) {
    (void)env;
    (void)finalize_hint;
    CormrSetWrapper* wrap = (CormrSetWrapper*)finalize_data;
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
    cormr_set_t* s = cormr_set_new();
    if (!s) {
        napi_throw_error(env, NULL, "Failed to allocate cormr set");
        return NULL;
    }

    CormrSetWrapper* wrap = (CormrSetWrapper*)calloc(1, sizeof(CormrSetWrapper));
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

    CormrSetWrapper* wrap = NULL;
    napi_unwrap(env, args[0], (void**)&wrap);
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

    CormrSetWrapper* wrap = NULL;
    napi_unwrap(env, args[0], (void**)&wrap);
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

    CormrSetWrapper* wrap = NULL;
    napi_unwrap(env, args[0], (void**)&wrap);
    if (!wrap || !wrap->set) {
        napi_throw_error(env, NULL, "Invalid cormr set");
        return NULL;
    }

    size_t count = cormr_set_count(wrap->set);
    const cormr_ref_t* refs = cormr_set_at(wrap->set);

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
    napi_unwrap(env, args[0], (void**)&wA);
    napi_unwrap(env, args[1], (void**)&wB);

    cormr_set_t* dst = cormr_set_new();
    cormr_set_intersect(dst, wA->set, wB->set);

    CormrSetWrapper* wrap = (CormrSetWrapper*)calloc(1, sizeof(CormrSetWrapper));
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
    napi_unwrap(env, args[0], (void**)&wA);
    napi_unwrap(env, args[1], (void**)&wB);

    cormr_set_t* dst = cormr_set_new();
    cormr_set_union(dst, wA->set, wB->set);

    CormrSetWrapper* wrap = (CormrSetWrapper*)calloc(1, sizeof(CormrSetWrapper));
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
    napi_unwrap(env, args[0], (void**)&wA);
    napi_unwrap(env, args[1], (void**)&wB);

    cormr_set_t* dst = cormr_set_new();
    cormr_set_subtract(dst, wA->set, wB->set);

    CormrSetWrapper* wrap = (CormrSetWrapper*)calloc(1, sizeof(CormrSetWrapper));
    wrap->set = dst;

    napi_value res;
    napi_create_object(env, &res);
    napi_wrap(env, res, wrap, CormrSetFinalizer, NULL, NULL);
    return res;
}

/* --- Bounded Streaming Min-Heap Ranker --- */

typedef struct {
    cormr_rank_t* rk;
} CormrRankWrapper;

static void CormrRankFinalizer(napi_env env, void* finalize_data, void* finalize_hint) {
    (void)env;
    (void)finalize_hint;
    CormrRankWrapper* wrap = (CormrRankWrapper*)finalize_data;
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

    cormr_rank_t* rk = cormr_rank_new(top_k, (float)min_score);
    if (!rk) {
        napi_throw_error(env, NULL, "Failed to allocate cormr rank buffer");
        return NULL;
    }

    CormrRankWrapper* wrap = (CormrRankWrapper*)calloc(1, sizeof(CormrRankWrapper));
    wrap->rk = rk;

    napi_value res;
    napi_create_object(env, &res);
    napi_wrap(env, res, wrap, CormrRankFinalizer, NULL, NULL);
    return res;
}

static napi_value CormrRankPush(napi_env env, napi_callback_info info) {
    size_t argc = 3;
    napi_value args[3];
    napi_get_cb_info(env, info, &argc, args, NULL, NULL);

    CormrRankWrapper* wrap = NULL;
    napi_unwrap(env, args[0], (void**)&wrap);
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

    CormrRankWrapper* wrap = NULL;
    napi_unwrap(env, args[0], (void**)&wrap);
    if (!wrap || !wrap->rk) {
        napi_throw_error(env, NULL, "Invalid cormr rank handle");
        return NULL;
    }

    uint32_t capacity = 100;
    if (argc >= 2) napi_get_value_uint32(env, args[1], &capacity);

    cormr_ref_t* refs = (cormr_ref_t*)malloc(capacity * sizeof(cormr_ref_t));
    float* scores = (float*)malloc(capacity * sizeof(float));

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

/* Init module exports */
static napi_value Init(napi_env env, napi_value exports) {
    napi_property_descriptor desc[] = {
        {"open", NULL, CormOpen, NULL, NULL, NULL, napi_default, NULL},
        {"put", NULL, CormPut, NULL, NULL, NULL, napi_default, NULL},
        {"get", NULL, CormGet, NULL, NULL, NULL, napi_default, NULL},
        {"del", NULL, CormDel, NULL, NULL, NULL, napi_default, NULL},
        {"save", NULL, CormSave, NULL, NULL, NULL, napi_default, NULL},
        {"close", NULL, CormClose, NULL, NULL, NULL, napi_default, NULL},
        {"setCreate", NULL, CormrSetCreate, NULL, NULL, NULL, napi_default, NULL},
        {"setPush", NULL, CormrSetPush, NULL, NULL, NULL, napi_default, NULL},
        {"setSeal", NULL, CormrSetSeal, NULL, NULL, NULL, napi_default, NULL},
        {"setToArray", NULL, CormrSetToArray, NULL, NULL, NULL, napi_default, NULL},
        {"setIntersect", NULL, CormrSetIntersect, NULL, NULL, NULL, napi_default, NULL},
        {"setUnion", NULL, CormrSetUnion, NULL, NULL, NULL, napi_default, NULL},
        {"setSubtract", NULL, CormrSetSubtract, NULL, NULL, NULL, napi_default, NULL},
        {"rankCreate", NULL, CormrRankCreate, NULL, NULL, NULL, napi_default, NULL},
        {"rankPush", NULL, CormrRankPush, NULL, NULL, NULL, napi_default, NULL},
        {"rankSorted", NULL, CormrRankSorted, NULL, NULL, NULL, napi_default, NULL}
    };

    napi_define_properties(env, exports, sizeof(desc) / sizeof(desc[0]), desc);
    return exports;
}

NAPI_MODULE(NODE_GYP_MODULE_NAME, Init)
