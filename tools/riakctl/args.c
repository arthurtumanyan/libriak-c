/*
 File:   args.c
 SPDX-License-Identifier: GPL-3.0-or-later

 Argument helpers shared by the commands.
 */

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "riakctl.h"

void rc_getopt_reset(void) {
    /* glibc fully reinitializes getopt on 0; other C libraries expect 1 */
#ifdef __GLIBC__
    optind = 0;
#else
    optind = 1;
#endif
}

bool rc_strv_push(rc_strv *v, const char *s) {
    const char **n = (const char **)realloc((void *)v->items, (v->count + 1) * sizeof *n);
    if (n == NULL) {
        return false;
    }
    n[v->count++] = s;
    v->items = n;
    return true;
}

void rc_strv_free(rc_strv *v) {
    free((void *)v->items);
    v->items = NULL;
    v->count = 0;
}

bool rc_parse_uint(const char *s, unsigned long long max, unsigned long long *out) {
    if (s == NULL || *s < '0' || *s > '9') {
        return false;
    }
    char *end;
    errno = 0;
    unsigned long long v = strtoull(s, &end, 10);
    if (errno || *end || v > max) {
        return false;
    }
    *out = v;
    return true;
}

/* "one", "quorum", "all", "default" or a replica count */
bool rc_parse_quorum(const char *s, int *out) {
    static const struct {
        const char *name;
        int value;
    } names[] = {{"default", RIAK_Q_DEFAULT},
                 {"one", RIAK_Q_ONE},
                 {"quorum", RIAK_Q_QUORUM},
                 {"all", RIAK_Q_ALL}};
    for (size_t i = 0; i < sizeof names / sizeof *names; i++) {
        if (strcmp(s, names[i].name) == 0) {
            *out = names[i].value;
            return true;
        }
    }
    unsigned long long v;
    if (!rc_parse_uint(s, 1000, &v) || v == 0) {
        return false;
    }
    *out = (int)v;
    return true;
}

/* Splits "name=value"; both parts are allocated. */
bool rc_split_pair(const char *s, char **name, char **value) {
    const char *eq = strchr(s, '=');
    if (eq == NULL || eq == s) {
        return false;
    }
    *name = strndup(s, (size_t)(eq - s));
    *value = strdup(eq + 1);
    if (*name == NULL || *value == NULL) {
        free(*name);
        free(*value);
        return false;
    }
    return true;
}

int rc_cmd_help(const rc_cmd *cmd) {
    printf("usage: riakctl [global options] %s %s\n\n%s\n", cmd->name, cmd->usage, cmd->summary);
    if (cmd->help) {
        printf("\n%s", cmd->help);
    }
    return RC_OK;
}
