/*
 File:   riakctl.h
 SPDX-License-Identifier: GPL-3.0-or-later

 Declarations shared by the riakctl sources.
 */

#ifndef RIAKCTL_H
#define RIAKCTL_H

#include <getopt.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <sys/types.h>

#include <json-c/json.h>
#include <riak/riak.h>

/* Exit codes; documented in docs/riakctl.md and riakctl.1, kept stable. */
enum {
    RC_OK = 0,
    RC_ERROR = 1,
    RC_USAGE = 2,
    RC_NOT_FOUND = 3,
    RC_SIBLINGS = 4,
    RC_PRECONDITION = 5,
    RC_UNAUTHORIZED = 6,
    RC_UNREACHABLE = 7,
    RC_UNSUPPORTED = 8
};

/* Connection settings, kept so that a second client can be opened (a stream
 * callback must not use the client running the stream). */
typedef struct {
    riak_config cfg;
    riak_node *nodes;
    char **hosts; /* owned host names of nodes */
    size_t n_nodes;
    char *password;
    bool verbose;
} rc_conn;

/* State passed to every command. */
typedef struct {
    rc_conn conn;
    riak_client *client; /* opened lazily by rc_client() */
    const char *type;    /* --type: bucket type, NULL for "default" */
    bool pretty;         /* single JSON documents are indented */
} rc_ctx;

typedef int (*rc_cmd_fn)(rc_ctx *x, int argc, char **argv);

typedef struct {
    const char *name;
    rc_cmd_fn fn;
    const char *usage;   /* argument synopsis */
    const char *summary; /* one line */
    const char *help;    /* option list, may be NULL */
} rc_cmd;

/* Growable array of strings (repeatable options). */
typedef struct {
    const char **items;
    size_t count;
} rc_strv;

/* conn.c */
int rc_conn_setup(rc_conn *c, const char *urls);
void rc_conn_free(rc_conn *c);
riak_client *rc_client(rc_ctx *x);
riak_client *rc_client_new(rc_ctx *x);
char *rc_read_password_file(const char *path);

/* out.c */
int rc_exit_code(riak_status st);
int rc_fail(riak_client *c, const char *what, riak_status st);
int rc_usage_error(const char *cmd, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
void rc_warn(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
int rc_print(rc_ctx *x, json_object *o);
int rc_print_line(json_object *o);
json_object *rc_parse_json(const char *text, const char *what);
json_object *rc_value_json(const char *data, size_t len);
bool rc_value_from_json(json_object *o, char **data, size_t *len);
json_object *rc_object_json(const riak_object *o, bool with_value);
json_object *rc_dt_json(const riak_dt_value *v);
const char *rc_dt_name(riak_dt_type t);
bool rc_dt_from_name(const char *name, riak_dt_type *t);
char *rc_base64_encode(const unsigned char *data, size_t len);
unsigned char *rc_base64_decode(const char *text, size_t *len);
/* Reads one line (with its newline) into *buf, growing it as getline()
 * does. Returns its length, or -1 at the end of the input or on an error.
 * Of a line longer than max bytes only max are kept and *too_long is set;
 * the line is still consumed and its full length returned. */
ssize_t rc_read_line(FILE *f, char **buf, size_t *cap, size_t max, bool *too_long);
bool rc_read_all(FILE *f, char **data, size_t *len);
char *rc_read_file(const char *path, size_t *len);

/* args.c */
void rc_getopt_reset(void);
bool rc_strv_push(rc_strv *v, const char *s);
void rc_strv_free(rc_strv *v);
bool rc_parse_quorum(const char *s, int *out);
bool rc_parse_uint(const char *s, unsigned long long max, unsigned long long *out);
int rc_cmd_help(const rc_cmd *cmd);
bool rc_split_pair(const char *s, char **name, char **value);

/* Commands. */
int rc_cmd_ping(rc_ctx *x, int argc, char **argv);
int rc_cmd_info(rc_ctx *x, int argc, char **argv);
int rc_cmd_stats(rc_ctx *x, int argc, char **argv);
int rc_cmd_get(rc_ctx *x, int argc, char **argv);
int rc_cmd_put(rc_ctx *x, int argc, char **argv);
int rc_cmd_delete(rc_ctx *x, int argc, char **argv);
int rc_cmd_list(rc_ctx *x, int argc, char **argv);
int rc_cmd_backup(rc_ctx *x, int argc, char **argv);
int rc_cmd_restore(rc_ctx *x, int argc, char **argv);
int rc_cmd_index(rc_ctx *x, int argc, char **argv);
int rc_cmd_query(rc_ctx *x, int argc, char **argv);
int rc_cmd_mapred(rc_ctx *x, int argc, char **argv);
int rc_cmd_coverage(rc_ctx *x, int argc, char **argv);
int rc_cmd_preflist(rc_ctx *x, int argc, char **argv);
int rc_cmd_dt(rc_ctx *x, int argc, char **argv);
int rc_cmd_counter(rc_ctx *x, int argc, char **argv);
int rc_cmd_props(rc_ctx *x, int argc, char **argv);
int rc_cmd_search(rc_ctx *x, int argc, char **argv);
int rc_cmd_aae(rc_ctx *x, int argc, char **argv);
int rc_cmd_repl(rc_ctx *x, int argc, char **argv);

/* cmd_dt.c: the datatype property of a bucket (or, with bucket NULL, of a
 * bucket type); NULL when it has none or on failure (*st is set). */
char *rc_bucket_datatype(riak_client *c, const char *type, const char *bucket, riak_status *st);

const rc_cmd *rc_find_cmd(const char *name);

/* main.c: the whole program; main() in entry.c calls it. */
int rc_main(int argc, char **argv);

#endif
