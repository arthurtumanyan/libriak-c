/*
 File:   cmd_props.c
 SPDX-License-Identifier: GPL-3.0-or-later

 props: bucket and bucket type properties.
   props get BUCKET | props set BUCKET [JSON|-] | props reset BUCKET
   props type-get TYPE | props type-set TYPE [JSON|-]
 Properties are given as {"props": {...}} or as the bare {...} object.
 */

#include <stdlib.h>
#include <string.h>

#include "riakctl.h"

/* The properties document of a command-line argument or of stdin. */
static char *props_doc(const char *arg) {
    size_t len;
    char *text = arg && strcmp(arg, "-") != 0 ? strdup(arg) : rc_read_file("-", &len);
    if (text == NULL) {
        return NULL;
    }
    json_object *o = rc_parse_json(text, "properties");
    free(text);
    if (o == NULL) {
        return NULL;
    }
    if (!json_object_is_type(o, json_type_object)) {
        fprintf(stderr, "riakctl: properties must be a JSON object\n");
        json_object_put(o);
        return NULL;
    }
    if (!json_object_object_get_ex(o, "props", NULL)) {
        json_object *w = json_object_new_object();
        json_object_object_add(w, "props", o);
        o = w;
    }
    char *doc = strdup(json_object_to_json_string_ext(o, JSON_C_TO_STRING_PLAIN));
    json_object_put(o);
    return doc;
}

static int print_props(rc_ctx *x, riak_client *c, riak_status st, char *json, const char *what) {
    if (st != RIAK_OK) {
        return rc_fail(c, what, st);
    }
    json_object *o = rc_parse_json(json, what);
    riak_free(json);
    return o ? rc_print(x, o) : RC_ERROR;
}

int rc_cmd_props(rc_ctx *x, int argc, char **argv) {
    static const struct option opts[] = {{"type", required_argument, NULL, 't'},
                                         {NULL, 0, NULL, 0}};
    int ch;
    while ((ch = getopt_long(argc, argv, "t:", opts, NULL)) != -1) {
        if (ch != 't') {
            return rc_usage_error("props", "unknown option");
        }
        x->type = optarg;
    }
    int rest = argc - optind;
    if (rest < 2) {
        return rc_usage_error("props", "ACTION and NAME expected");
    }
    const char *action = argv[optind];
    const char *name = argv[optind + 1];
    const char *value = rest > 2 ? argv[optind + 2] : NULL;
    bool get = strcmp(action, "get") == 0, set = strcmp(action, "set") == 0;
    bool reset = strcmp(action, "reset") == 0;
    bool type_get = strcmp(action, "type-get") == 0, type_set = strcmp(action, "type-set") == 0;
    if (!get && !set && !reset && !type_get && !type_set) {
        return rc_usage_error("props", "unknown action '%s'", action);
    }
    if (rest > ((set || type_set) ? 3 : 2)) {
        return rc_usage_error("props", "too many arguments");
    }
    riak_client *c = rc_client(x);
    if (c == NULL) {
        return RC_ERROR;
    }
    char *json = NULL;
    riak_status st;
    if (get) {
        st = riak_type_get_bucket_props(c, x->type, name, &json);
        return print_props(x, c, st, json, "bucket properties");
    }
    if (type_get) {
        st = riak_get_type_props(c, name, &json);
        return print_props(x, c, st, json, "bucket type properties");
    }
    if (reset) {
        st = riak_type_reset_bucket_props(c, x->type, name);
        return st == RIAK_OK ? RC_OK : rc_fail(c, "reset bucket properties", st);
    }
    char *doc = props_doc(value);
    if (doc == NULL) {
        return RC_USAGE;
    }
    st =
        set ? riak_type_set_bucket_props(c, x->type, name, doc) : riak_set_type_props(c, name, doc);
    free(doc);
    return st == RIAK_OK ? RC_OK : rc_fail(c, "set properties", st);
}
