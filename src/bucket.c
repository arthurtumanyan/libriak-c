/*
 File:   bucket.c
 Copyright (C) 2013-2026  Arthur Tumanyan <arthurtumanyan@gmail.com>
 SPDX-License-Identifier: GPL-3.0-or-later

 Bucket listing and bucket properties.
 */

#include "internal.h"

#include <stdlib.h>
#include <string.h>

#include <json-c/json.h>

#include "parse.h"

riak_status riak_list_buckets(riak_client *client, riak_strlist *out) {
    if (client == NULL || out == NULL) {
        return RIAK_E_INVALID_ARG;
    }
    memset(out, 0, sizeof *out);
    riak_buf url = {0};
    if (riak_buf_append_str(&url, client->base_url) != 0 ||
        riak_buf_append_str(&url, "buckets?buckets=true") != 0) {
        riak_buf_free(&url);
        return riak_set_error(client, RIAK_E_NOMEM, "out of memory");
    }
    riak_request req = {.method = "GET", .url = url.data};
    riak_response resp = {0};
    riak_status st = riak_perform(client, &req, &resp);
    riak_buf_free(&url);
    if (st != RIAK_OK) {
        return st;
    }
    if (resp.status != 200) {
        st = riak_http_error(client, &resp);
    } else {
        st = riak_parse_string_array(resp.body.data ? resp.body.data : "", resp.body.len, "buckets",
                                     out);
        if (st != RIAK_OK) {
            riak_set_error(client, st, "cannot parse bucket list");
        }
    }
    riak_response_free(&resp);
    return st;
}

static bool keystream_sink(void *ctx, const char *data, size_t len, riak_status *abort_status) {
    riak_status st = riak_keystream_feed(ctx, data, len);
    if (st != RIAK_OK) {
        *abort_status = st;
        return false;
    }
    return true;
}

riak_status riak_list_keys(riak_client *client, const char *bucket, riak_strlist *out) {
    if (client == NULL || out == NULL) {
        return RIAK_E_INVALID_ARG;
    }
    memset(out, 0, sizeof *out);
    riak_status st = riak_check_name(client, bucket, "bucket");
    if (st != RIAK_OK) {
        return st;
    }
    riak_keystream *ks = riak_keystream_new();
    riak_buf url = {0};
    if (ks == NULL || riak_url_bucket(&url, client, bucket, "keys", NULL) != 0 ||
        riak_url_append_query(&url, "keys", "stream") != 0) {
        riak_keystream_free(ks);
        riak_buf_free(&url);
        return riak_set_error(client, RIAK_E_NOMEM, "out of memory");
    }
    riak_request req = {.method = "GET", .url = url.data, .sink = keystream_sink, .sink_ctx = ks};
    riak_response resp = {0};
    st = riak_perform(client, &req, &resp);
    if (st == RIAK_OK) {
        if (resp.status != 200) {
            st = riak_http_error(client, &resp);
        } else if ((st = riak_keystream_finish(ks, out)) != RIAK_OK) {
            riak_set_error(client, st, "truncated key stream");
        }
        riak_response_free(&resp);
    }
    riak_keystream_free(ks);
    riak_buf_free(&url);
    return st;
}

static riak_status props_request(riak_client *client, const char *bucket, const char *method,
                                 const char *json, riak_response *resp) {
    riak_status st = riak_check_name(client, bucket, "bucket");
    if (st != RIAK_OK) {
        return st;
    }
    riak_buf url = {0};
    struct curl_slist *headers = NULL;
    if (riak_url_bucket(&url, client, bucket, "props", NULL) != 0) {
        riak_buf_free(&url);
        return riak_set_error(client, RIAK_E_NOMEM, "out of memory");
    }
    if (json != NULL) {
        st = riak_add_header(client, &headers, "Content-Type", NULL, "application/json");
    }
    if (st == RIAK_OK) {
        riak_request req = {.method = method,
                            .url = url.data,
                            .headers = headers,
                            .body = json,
                            .body_len = json ? strlen(json) : 0};
        st = riak_perform(client, &req, resp);
    }
    curl_slist_free_all(headers);
    riak_buf_free(&url);
    return st;
}

riak_status riak_get_bucket_props(riak_client *client, const char *bucket, char **json) {
    if (client == NULL || json == NULL) {
        return RIAK_E_INVALID_ARG;
    }
    *json = NULL;
    riak_response resp = {0};
    riak_status st = props_request(client, bucket, "GET", NULL, &resp);
    if (st != RIAK_OK) {
        return st;
    }
    if (resp.status != 200) {
        st = riak_http_error(client, &resp);
    } else {
        json_object *obj = json_tokener_parse(resp.body.data ? resp.body.data : "");
        if (obj == NULL || !json_object_is_type(obj, json_type_object)) {
            st = riak_set_error(client, RIAK_E_PARSE, "bucket props are not a JSON object");
        } else if ((*json = riak_buf_steal(&resp.body, NULL)) == NULL) {
            st = riak_set_error(client, RIAK_E_NOMEM, "out of memory");
        }
        json_object_put(obj);
    }
    riak_response_free(&resp);
    return st;
}

riak_status riak_set_bucket_props(riak_client *client, const char *bucket, const char *json) {
    if (client == NULL || json == NULL) {
        return RIAK_E_INVALID_ARG;
    }
    riak_response resp = {0};
    riak_status st = props_request(client, bucket, "PUT", json, &resp);
    if (st != RIAK_OK) {
        return st;
    }
    if (resp.status != 204 && resp.status != 200) {
        st = riak_http_error(client, &resp);
    }
    riak_response_free(&resp);
    return st;
}

riak_status riak_reset_bucket_props(riak_client *client, const char *bucket) {
    if (client == NULL) {
        return RIAK_E_INVALID_ARG;
    }
    riak_response resp = {0};
    riak_status st = props_request(client, bucket, "DELETE", NULL, &resp);
    if (st != RIAK_OK) {
        return st;
    }
    if (resp.status != 204) {
        st = riak_http_error(client, &resp);
    }
    riak_response_free(&resp);
    return st;
}
