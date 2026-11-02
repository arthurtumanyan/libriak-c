/*
 File:   cmd_dt.c
 SPDX-License-Identifier: GPL-3.0-or-later

 dt (data types) and counter (Riak 1.4 counters).

 dt update takes an operation document whose form follows the data type:
   counter  a number added to the counter
   set      {"add": [...], "remove": [...]} or an array of elements to add
   gset     an array of elements to add; hll the same
   map      {"<field>_<type>": operation, ..., "remove": ["<field>_<type>", ...]}
            with counter fields taking a number, register fields a string,
            flag fields a boolean, set fields a set operation and map
            fields a nested map operation
 */

#include <stdlib.h>
#include <string.h>

#include "riakctl.h"

enum { OPT_CONTEXT = 256, OPT_RETURN_BODY };

char *rc_bucket_datatype(riak_client *c, const char *type, const char *bucket, riak_status *st) {
    char *json = NULL;
    *st = bucket ? riak_type_get_bucket_props(c, type, bucket, &json)
                 : riak_get_type_props(c, type, &json);
    if (*st != RIAK_OK) {
        return NULL;
    }
    json_object *o = json_tokener_parse(json);
    riak_free(json);
    json_object *props, *dt;
    char *name = NULL;
    if (o && json_object_object_get_ex(o, "props", &props) &&
        json_object_object_get_ex(props, "datatype", &dt) &&
        json_object_is_type(dt, json_type_string)) {
        name = strdup(json_object_get_string(dt));
    }
    json_object_put(o);
    return name;
}

static json_object *datatype_json(const riak_datatype *dt, bool context) {
    json_object *o = json_object_new_object();
    json_object_object_add(o, "datatype", json_object_new_string(rc_dt_name(dt->value.type)));
    json_object_object_add(o, "value", rc_dt_json(&dt->value));
    if (context && dt->context) {
        json_object_object_add(o, "context", json_object_new_string(dt->context));
    }
    return o;
}

/* ---- operation documents ----------------------------------------------- */

static bool set_op(json_object *doc, riak_dt_op *op, riak_dt_type t, bool *removes) {
    json_object *add = doc, *rem = NULL;
    if (json_object_is_type(doc, json_type_object)) {
        add = NULL;
        json_object_object_get_ex(doc, "add", &add);
        json_object_object_get_ex(doc, "remove", &rem);
    }
    if ((add && !json_object_is_type(add, json_type_array)) ||
        (rem && !json_object_is_type(rem, json_type_array))) {
        return false;
    }
    for (size_t i = 0; add && i < json_object_array_length(add); i++) {
        riak_dt_op_add(op, json_object_get_string(json_object_array_get_idx(add, i)));
    }
    if (rem && json_object_array_length(rem) > 0) {
        if (t != RIAK_DT_SET) {
            rc_warn("dt: elements can be removed from sets only");
            return false;
        }
        *removes = true;
    }
    for (size_t i = 0; rem && i < json_object_array_length(rem); i++) {
        riak_dt_op_remove(op, json_object_get_string(json_object_array_get_idx(rem, i)));
    }
    return true;
}

static bool split_field(const char *name, char *field, size_t size, riak_dt_type *t) {
    const char *us = strrchr(name, '_');
    if (us == NULL || (size_t)(us - name) >= size || !rc_dt_from_name(us + 1, t)) {
        rc_warn("dt: map field '%s' needs a type suffix (_counter, _set, _register, _flag, _map)",
                name);
        return false;
    }
    memcpy(field, name, (size_t)(us - name));
    field[us - name] = '\0';
    return true;
}

static bool map_op(json_object *doc, riak_dt_op *op, bool *removes) {
    if (!json_object_is_type(doc, json_type_object)) {
        return false;
    }
    json_object_object_foreach(doc, name, val) {
        char field[1024];
        riak_dt_type t;
        if (strcmp(name, "remove") == 0) {
            if (!json_object_is_type(val, json_type_array)) {
                return false;
            }
            for (size_t i = 0; i < json_object_array_length(val); i++) {
                if (!split_field(json_object_get_string(json_object_array_get_idx(val, i)), field,
                                 sizeof field, &t)) {
                    return false;
                }
                riak_dt_op_map_remove(op, field, t);
            }
            *removes = true;
            continue;
        }
        if (!split_field(name, field, sizeof field, &t)) {
            return false;
        }
        switch (t) {
        case RIAK_DT_COUNTER:
            riak_dt_op_map_counter(op, field, json_object_get_int64(val));
            break;
        case RIAK_DT_REGISTER:
            riak_dt_op_map_register(op, field, json_object_get_string(val));
            break;
        case RIAK_DT_FLAG:
            riak_dt_op_map_flag(op, field, json_object_get_boolean(val));
            break;
        case RIAK_DT_SET:
            if (!set_op(val, riak_dt_op_map_set(op, field), RIAK_DT_SET, removes)) {
                return false;
            }
            break;
        case RIAK_DT_MAP:
            if (!map_op(val, riak_dt_op_map_map(op, field), removes)) {
                return false;
            }
            break;
        default:
            rc_warn("dt: map field '%s' has an unsupported type", name);
            return false;
        }
    }
    return true;
}

static riak_dt_op *op_from_json(json_object *doc, riak_dt_type t, bool *removes) {
    riak_dt_op *op = riak_dt_op_new(t);
    bool ok = op != NULL;
    if (ok && t == RIAK_DT_COUNTER) {
        ok = json_object_is_type(doc, json_type_int);
        if (ok) {
            riak_dt_op_increment(op, json_object_get_int64(doc));
        }
    } else if (ok && (t == RIAK_DT_SET || t == RIAK_DT_GSET || t == RIAK_DT_HLL)) {
        ok = set_op(doc, op, t, removes);
    } else if (ok && t == RIAK_DT_MAP) {
        ok = map_op(doc, op, removes);
    }
    if (!ok) {
        rc_warn("dt: the operation does not fit a %s", rc_dt_name(t));
        riak_dt_op_free(op);
        return NULL;
    }
    return op;
}

/* ---- dt ----------------------------------------------------------------- */

static int dt_apply(rc_ctx *x, riak_client *c, const char *bucket, const char *key, riak_dt_op *op,
                    bool removes, const char *context, bool return_body) {
    riak_datatype cur = {0};
    riak_status st;
    /* removals need the causal context of a fetch */
    if (removes && context == NULL) {
        st = riak_dt_fetch(c, x->type, bucket, key, NULL, &cur);
        if (st != RIAK_OK && st != RIAK_E_NOT_FOUND) {
            return rc_fail(c, "dt fetch", st);
        }
        context = cur.context;
    }
    riak_dt_opts o = {0};
    o.return_body = return_body;
    riak_datatype out = {0};
    char *created = NULL;
    st = riak_dt_update(c, x->type, bucket, key, op, context, &o, return_body ? &out : NULL,
                        key ? NULL : &created);
    riak_datatype_free(&cur);
    if (st != RIAK_OK) {
        riak_free(created);
        return rc_fail(c, "dt update", st);
    }
    int rc = RC_OK;
    if (return_body || created) {
        json_object *j = return_body ? datatype_json(&out, false) : json_object_new_object();
        if (j && created) {
            json_object_object_add(j, "key", json_object_new_string(created));
        }
        rc = rc_print(x, j);
    }
    riak_datatype_free(&out);
    riak_free(created);
    return rc;
}

int rc_cmd_dt(rc_ctx *x, int argc, char **argv) {
    static const struct option opts[] = {{"type", required_argument, NULL, 't'},
                                         {"context", required_argument, NULL, OPT_CONTEXT},
                                         {"return-body", no_argument, NULL, OPT_RETURN_BODY},
                                         {NULL, 0, NULL, 0}};
    const char *context = NULL;
    bool return_body = false;
    int ch;
    while ((ch = getopt_long(argc, argv, "t:", opts, NULL)) != -1) {
        switch (ch) {
        case 't':
            x->type = optarg;
            break;
        case OPT_CONTEXT:
            context = optarg;
            break;
        case OPT_RETURN_BODY:
            return_body = true;
            break;
        default:
            return rc_usage_error("dt", "unknown option");
        }
    }
    int rest = argc - optind;
    if (rest < 1) {
        return rc_usage_error("dt", "an action expected (get, incr, decr, add, remove, update)");
    }
    const char *action = argv[optind];
    char **args = argv + optind + 1;
    rest--;
    if (x->type == NULL) {
        return rc_usage_error("dt", "data types need a bucket type (--type)");
    }
    bool is_get = strcmp(action, "get") == 0;
    bool is_update = strcmp(action, "update") == 0;
    bool is_count = strcmp(action, "incr") == 0 || strcmp(action, "decr") == 0;
    bool is_elems = strcmp(action, "add") == 0 || strcmp(action, "remove") == 0;
    if (!is_get && !is_update && !is_count && !is_elems) {
        return rc_usage_error("dt", "unknown action '%s'", action);
    }
    if ((is_get && rest != 2) || (is_update && (rest < 1 || rest > 3)) ||
        (is_count && (rest < 2 || rest > 3)) || (is_elems && rest < 3)) {
        return rc_usage_error("dt", "wrong number of arguments for '%s'", action);
    }
    riak_client *c = rc_client(x);
    if (c == NULL) {
        return RC_ERROR;
    }
    const char *bucket = args[0];
    riak_status st;
    if (is_get) {
        riak_datatype dt = {0};
        if ((st = riak_dt_fetch(c, x->type, bucket, args[1], NULL, &dt)) != RIAK_OK) {
            return rc_fail(c, "dt get", st);
        }
        int rc = rc_print(x, datatype_json(&dt, true));
        riak_datatype_free(&dt);
        return rc;
    }
    char *tname = rc_bucket_datatype(c, x->type, NULL, &st);
    if (st != RIAK_OK) {
        return rc_fail(c, "bucket type properties", st);
    }
    riak_dt_type t;
    bool known = tname && rc_dt_from_name(tname, &t);
    free(tname);
    if (!known) {
        fprintf(stderr, "riakctl: dt: bucket type %s has no datatype property\n", x->type);
        return RC_USAGE;
    }

    json_object *doc = NULL;
    const char *key = args[1];
    if (is_count) {
        unsigned long long n = 1;
        if (rest == 3 && (!rc_parse_uint(args[2], INT64_MAX, &n))) {
            return rc_usage_error("dt", "invalid amount '%s'", args[2]);
        }
        doc = json_object_new_int64(action[0] == 'd' ? -(int64_t)n : (int64_t)n);
    } else if (is_elems) {
        json_object *a = json_object_new_array();
        for (int i = 2; i < rest; i++) {
            json_object_array_add(a, json_object_new_string(args[i]));
        }
        doc = json_object_new_object();
        json_object_object_add(doc, action[0] == 'a' ? "add" : "remove", a);
    } else {
        /* dt update BUCKET [KEY] [FILE]: without KEY a key is generated */
        key = rest >= 2 && strcmp(args[1], "-") != 0 ? args[1] : NULL;
        size_t len;
        char *text = rc_read_file(rest == 3 ? args[2] : "-", &len);
        if (text == NULL) {
            return RC_ERROR;
        }
        doc = rc_parse_json(text, "operation");
        free(text);
        if (doc == NULL) {
            return RC_ERROR;
        }
    }
    if (is_elems && t == RIAK_DT_MAP) {
        json_object_put(doc);
        return rc_usage_error("dt", "add and remove apply to sets; maps use update");
    }
    if (is_count && t != RIAK_DT_COUNTER) {
        json_object_put(doc);
        return rc_usage_error("dt", "incr and decr apply to counters; maps use update");
    }
    bool removes = false;
    riak_dt_op *op = op_from_json(doc, t, &removes);
    json_object_put(doc);
    if (op == NULL) {
        return RC_USAGE;
    }
    int rc = dt_apply(x, c, bucket, key, op, removes, context, return_body);
    riak_dt_op_free(op);
    return rc;
}

/* ---- counter (Riak 1.4) ------------------------------------------------- */

int rc_cmd_counter(rc_ctx *x, int argc, char **argv) {
    if (argc < 4) {
        return rc_usage_error("counter", "ACTION BUCKET KEY expected");
    }
    const char *action = argv[1];
    bool get = strcmp(action, "get") == 0;
    bool incr = strcmp(action, "incr") == 0;
    bool decr = strcmp(action, "decr") == 0;
    if ((!get && !incr && !decr) || argc > (get ? 4 : 5)) {
        return rc_usage_error("counter", "get, incr or decr with BUCKET KEY [AMOUNT] expected");
    }
    unsigned long long n = 1;
    if (argc == 5 && !rc_parse_uint(argv[4], INT64_MAX, &n)) {
        return rc_usage_error("counter", "invalid amount '%s'", argv[4]);
    }
    riak_client *c = rc_client(x);
    if (c == NULL) {
        return RC_ERROR;
    }
    int64_t value = 0;
    riak_status st = get ? riak_counter_get(c, argv[2], argv[3], NULL, &value)
                         : riak_counter_update(c, argv[2], argv[3], decr ? -(int64_t)n : (int64_t)n,
                                               NULL, &value);
    if (st != RIAK_OK) {
        return rc_fail(c, "counter", st);
    }
    json_object *o = json_object_new_object();
    json_object_object_add(o, "value", json_object_new_int64(value));
    return rc_print(x, o);
}
